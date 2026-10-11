#include "gucode.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CALLS 16
typedef struct {
  Buffer id, name, args;
} ToolCall;
typedef struct {
  Buffer line, event, text, raw;
  ToolCall calls[CALLS];
  int count, done, failed;
  size_t received;
  char finish[64];
  char error[256];
} Stream;
static void problem(Stream *s, const char *msg) {
  s->failed = 1;
  snprintf(s->error, sizeof s->error, "%s", msg);
}
static void delta_string(Buffer *b, const cJSON *j, const char *key) {
  const char *v = json_string(j, key);
  if (v)
    buf_text(b, v);
}
static void event(Stream *s) {
  if (!s->event.len)
    return;
  if (!strcmp(s->event.data, "[DONE]")) {
    if (s->done || !s->finish[0])
      problem(s, "Invalid stream completion");
    s->done = 1;
    return;
  }
  if (s->done) {
    problem(s, "Data after stream completion");
    return;
  }
  cJSON *j = json_parse(s->event.data);
  if (!j) {
    problem(s, "Invalid JSON in SSE event");
    return;
  }
  cJSON *err = cJSON_GetObjectItemCaseSensitive(j, "error");
  if (err) {
    problem(s, json_string(err, "message")
                   ? json_string(err, "message")
                   : "Provider returned a stream error");
    cJSON_Delete(j);
    return;
  }
  cJSON *choice =
      cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j, "choices"), 0);
  const char *finish = json_string(choice, "finish_reason");
  if (s->finish[0] && choice) {
    problem(s, "Choice after finish_reason");
    cJSON_Delete(j);
    return;
  }
  if (finish)
    snprintf(s->finish, sizeof s->finish, "%s", finish);
  cJSON *d = cJSON_GetObjectItemCaseSensitive(choice, "delta");
  const char *content = json_string(d, "content");
  if (content) {
    buf_text(&s->text, content);
    terminal_text(content);
  }
  cJSON *tc;
  cJSON_ArrayForEach(tc, cJSON_GetObjectItemCaseSensitive(d, "tool_calls")) {
    cJSON *idx = cJSON_GetObjectItemCaseSensitive(tc, "index");
    if (!cJSON_IsNumber(idx) || idx->valuedouble != idx->valueint ||
        idx->valueint < 0 || idx->valueint >= CALLS) {
      problem(s, "Invalid tool call index");
      break;
    }
    int i = idx->valueint;
    if (i >= s->count)
      s->count = i + 1;
    delta_string(&s->calls[i].id, tc, "id");
    cJSON *fn = cJSON_GetObjectItemCaseSensitive(tc, "function");
    delta_string(&s->calls[i].name, fn, "name");
    delta_string(&s->calls[i].args, fn, "arguments");
  }
  cJSON_Delete(j);
}
static void line(Stream *s) {
  if (s->line.len && s->line.data[s->line.len - 1] == '\r')
    s->line.data[--s->line.len] = 0;
  if (!s->line.len) {
    event(s);
    s->event.len = 0;
    if (s->event.data)
      s->event.data[0] = 0;
  } else if (!strncmp(s->line.data, "data:", 5)) {
    const char *p = s->line.data + 5;
    if (*p == ' ')
      p++;
    if (s->event.len)
      buf_text(&s->event, "\n");
    buf_text(&s->event, p);
  }
  s->line.len = 0;
  if (s->line.data)
    s->line.data[0] = 0;
}
static size_t receive(char *data, size_t size, size_t count, void *arg) {
  Stream *s = arg;
  if (size && count > (GC_LIMIT - s->received) / size) {
    problem(s, "Response exceeds 8 MiB limit; no tools executed");
    return 0;
  }
  size_t n = size * count;
  s->received += n;
  if (memchr(data, 0, n)) {
    problem(s, "NUL byte in event stream");
    return 0;
  }
  if (interrupted || s->failed)
    return 0;
  if (s->raw.len < 4096) {
    size_t take = n < 4096 - s->raw.len ? n : 4096 - s->raw.len;
    buf_add(&s->raw, data, take);
  }
  for (size_t i = 0; i < n; i++) {
    if (data[i] == '\n')
      line(s);
    else
      buf_add(&s->line, data + i, 1);
    if (s->failed)
      return 0;
  }
  return n;
}
static int progress(void *p, curl_off_t a, curl_off_t b, curl_off_t c,
                    curl_off_t d) {
  (void)p;
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  return interrupted ? 1 : 0;
}
cJSON *api_turn(cJSON *messages, const char *endpoint, const char *model,
                const char *key, char *error, size_t cap) {
  cJSON *request = cJSON_CreateObject();
  cJSON_AddStringToObject(request, "model", model);
  cJSON_AddBoolToObject(request, "stream", 1);
  cJSON_AddNumberToObject(request, "max_tokens", 8192);
  cJSON *thinking = cJSON_CreateObject();
  cJSON_AddStringToObject(thinking, "type", "disabled");
  cJSON_AddItemToObject(request, "thinking", thinking);
  /* The synchronous request borrows history; deleting the envelope must not
   * delete it or require a second full conversation tree. */
  cJSON_AddItemReferenceToObject(request, "messages", messages);
  cJSON_AddItemToObject(request, "tools", tool_definitions());
  if (json_clean_strings(request) < 0) {
    cJSON_Delete(request);
    snprintf(error, cap, "Text exceeds 8 MiB after UTF-8 repair");
    return NULL;
  }
  char *body = json_print_limited(request, GC_LIMIT);
  cJSON_Delete(request);
  if (!body) {
    snprintf(error, cap, "Request exceeds 8 MiB limit; use /clear or a new session");
    return NULL;
  }
  Stream s = {0};
  char curl_error[CURL_ERROR_SIZE] = {0};
  CURL *c = curl_easy_init();
  struct curl_slist *headers = NULL;
  cJSON *answer = NULL;
  if (!c) {
    snprintf(error, cap, "Cannot initialize curl");
    free(body);
    return NULL;
  }
  Buffer auth = {0};
  buf_text(&auth, "Authorization: Bearer ");
  buf_text(&auth, key);
  const char *hs[] = {"Content-Type: application/json",
                      "Accept: text/event-stream", auth.data};
  for (int i = 0; i < 3; i++) {
    struct curl_slist *h = curl_slist_append(headers, hs[i]);
    if (!h) {
      snprintf(error, cap, "Cannot allocate headers");
      goto cleanup;
    }
    headers = h;
  }
  CURLcode rc;
#define OPT(k, v)                                                              \
  do {                                                                         \
    rc = curl_easy_setopt(c, k, v);                                            \
    if (rc) {                                                                  \
      snprintf(error, cap, "Unsupported transport option %s: %s", #k,          \
               curl_easy_strerror(rc));                                        \
      goto cleanup;                                                            \
    }                                                                          \
  } while (0)
  OPT(CURLOPT_URL, endpoint);
  OPT(CURLOPT_FOLLOWLOCATION, 1L);
  OPT(CURLOPT_NOSIGNAL, 1L);
  OPT(CURLOPT_HTTPHEADER, headers);
  OPT(CURLOPT_POSTFIELDS, body);
  OPT(CURLOPT_POSTFIELDSIZE, (long)strlen(body));
  OPT(CURLOPT_WRITEFUNCTION, receive);
  OPT(CURLOPT_WRITEDATA, (void *)&s);
  OPT(CURLOPT_ERRORBUFFER, curl_error);
  OPT(CURLOPT_CONNECTTIMEOUT_MS, 30000L);
  OPT(CURLOPT_TIMEOUT_MS, 300000L);
  OPT(CURLOPT_NOPROGRESS, 0L);
  OPT(CURLOPT_XFERINFOFUNCTION, progress);
  rc = curl_easy_perform(c);
  long status = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
  if (interrupted) {
    snprintf(error, cap, "Interrupted");
    goto cleanup;
  }
  if (status != 200) {
    snprintf(error, cap, "HTTP %ld: %.180s", status,
             s.raw.data ? s.raw.data : curl_error);
    goto cleanup;
  }
  if (rc || s.failed) {
    snprintf(error, cap, "%s",
             s.failed ? s.error
                      : (curl_error[0] ? curl_error : curl_easy_strerror(rc)));
    goto cleanup;
  }
  if (s.line.len || s.event.len || !s.done ||
      (!strcmp(s.finish, "tool_calls") && !s.count) ||
      (!strcmp(s.finish, "stop") ? s.count != 0
                                 : strcmp(s.finish, "tool_calls") != 0)) {
    snprintf(error, cap,
             "Incomplete response (finish=%s, done=%d); no tools executed",
             s.finish, s.done);
    goto cleanup;
  }
  answer = message("assistant", s.text.data ? s.text.data : "");
  if (!answer) {
    snprintf(error, cap, "Response text exceeds 8 MiB after UTF-8 repair");
    goto cleanup;
  }
  if (s.count) {
    cJSON *calls = cJSON_CreateArray();
    cJSON_AddItemToObject(answer, "tool_calls", calls);
    for (int i = 0; i < s.count; i++) {
      ToolCall *t = &s.calls[i];
      cJSON *args = t->args.data ? json_parse(t->args.data) : NULL;
      if (!t->id.len || !t->name.len || !cJSON_IsObject(args)) {
        cJSON_Delete(args);
        cJSON_Delete(answer);
        answer = NULL;
        snprintf(error, cap, "Incomplete tool call; no tools executed");
        goto cleanup;
      }
      for (int k = 0; k < i; k++)
        if (!strcmp(s.calls[k].id.data, t->id.data)) {
          cJSON_Delete(args);
          cJSON_Delete(answer);
          answer = NULL;
          snprintf(error, cap, "Duplicate tool call ID");
          goto cleanup;
        }
      cJSON_Delete(args);
      cJSON *call = cJSON_CreateObject(), *fn = cJSON_CreateObject();
      cJSON_AddStringToObject(call, "id", t->id.data);
      cJSON_AddStringToObject(call, "type", "function");
      cJSON_AddItemToObject(call, "function", fn);
      cJSON_AddStringToObject(fn, "name", t->name.data);
      cJSON_AddStringToObject(fn, "arguments", t->args.data);
      cJSON_AddItemToArray(calls, call);
    }
  }
  /* Normalize provider strings before history accounting, including tool IDs
   * and encoded arguments. Later request preparation must not grow history. */
  if (json_clean_strings(answer) < 0) {
    cJSON_Delete(answer);
    answer = NULL;
    snprintf(error, cap, "Response text exceeds 8 MiB after UTF-8 repair");
  }
  if (answer) {
    /* Repair can map distinct malformed IDs to the same string. Validate the
     * actual IDs that will enter history before allowing any tool execution. */
    cJSON *calls = cJSON_GetObjectItemCaseSensitive(answer, "tool_calls");
    for (int i = 0; i < cJSON_GetArraySize(calls); i++)
      for (int k = 0; k < i; k++)
        if (!strcmp(json_string(cJSON_GetArrayItem(calls, i), "id"),
                    json_string(cJSON_GetArrayItem(calls, k), "id"))) {
          cJSON_Delete(answer);
          answer = NULL;
          snprintf(error, cap, "Duplicate tool call ID after UTF-8 repair");
          goto cleanup;
        }
  }
cleanup:
  curl_easy_cleanup(c);
  curl_slist_free_all(headers);
  buf_free(&auth);
  free(body);
  buf_free(&s.line);
  buf_free(&s.event);
  buf_free(&s.text);
  buf_free(&s.raw);
  for (int i = 0; i < CALLS; i++) {
    buf_free(&s.calls[i].id);
    buf_free(&s.calls[i].name);
    buf_free(&s.calls[i].args);
  }
  return answer;
}
