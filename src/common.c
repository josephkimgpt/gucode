#include "gucode.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
volatile sig_atomic_t interrupted;
int use_color;
void *gc_alloc(size_t n) {
  void *p = calloc(1, n ? n : 1);
  if (!p) {
    fputs("gucode: out of memory\n", stderr);
    exit(1);
  }
  return p;
}
char *gc_copy(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = gc_alloc(n);
  memcpy(p, s, n);
  return p;
}
void buf_add(Buffer *b, const char *s, size_t n) {
  if (n > GC_LIMIT || b->len > GC_LIMIT - n) {
    fputs("gucode: data exceeds 8 MiB limit\n", stderr);
    exit(1);
  }
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + n + 1)
      cap *= 2;
    char *p = realloc(b->data, cap);
    if (!p) {
      fputs("gucode: out of memory\n", stderr);
      exit(1);
    }
    b->data = p;
    b->cap = cap;
  }
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = 0;
}
void buf_text(Buffer *b, const char *s) { buf_add(b, s, strlen(s)); }
void buf_free(Buffer *b) {
  free(b->data);
  memset(b, 0, sizeof *b);
}
char *read_file(const char *path, size_t limit) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  Buffer b = {0};
  char tmp[4096];
  size_t n;
  while ((n = fread(tmp, 1, sizeof tmp, f))) {
    if (b.len + n > limit || memchr(tmp, 0, n)) {
      fclose(f);
      buf_free(&b);
      errno = EFBIG;
      return NULL;
    }
    buf_add(&b, tmp, n);
  }
  if (ferror(f)) {
    fclose(f);
    buf_free(&b);
    return NULL;
  }
  fclose(f);
  if (!b.data)
    return gc_copy("");
  return b.data;
}
int atomic_write(const char *path, const char *text, size_t n, int priv) {
  struct stat st;
  int exists = lstat(path, &st) == 0;
  if (exists && !S_ISREG(st.st_mode)) {
    errno = EINVAL;
    return -1;
  }
  size_t z = strlen(path) + 32;
  char *tmp = gc_alloc(z);
  snprintf(tmp, z, "%s.gucode-XXXXXX", path);
  int fd = mkstemp(tmp);
  if (fd < 0) {
    free(tmp);
    return -1;
  }
  int ok = 0;
  size_t off = 0;
  while (off < n) {
    ssize_t w = write(fd, text + off, n - off);
    if (w < 0 && errno == EINTR)
      continue;
    if (w <= 0) {
      ok = -1;
      break;
    }
    off += (size_t)w;
  }
  if (fchmod(fd, priv ? 0600 : (exists ? (st.st_mode & 0777) : 0644)))
    ok = -1;
  if (fsync(fd))
    ok = -1;
  if (close(fd))
    ok = -1;
  if (!ok && rename(tmp, path))
    ok = -1;
  if (ok)
    unlink(tmp);
  free(tmp);
  return ok;
}
void terminal_write(
    FILE *out,
    const char *s) { /* Model/tool output must not inject terminal controls. */
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (*p == '\n' || *p == '\t' || (*p >= 32 && *p != 127))
      fputc(*p, out);
    else if (*p == 27)
      fputs("^[", out);
    else
      fputc('?', out);
  }
  fflush(out);
}
void terminal_text(const char *s) { terminal_write(stdout, s); }
const char *json_string(const cJSON *j, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
  return cJSON_IsString(v) ? v->valuestring : NULL;
}
cJSON *message(const char *role, const char *content) {
  cJSON *m = cJSON_CreateObject();
  cJSON_AddStringToObject(m, "role", role);
  cJSON_AddStringToObject(m, "content", content);
  return m;
}

/* Reject NUL-bearing strings and trailing garbage before using C string APIs.
 */
cJSON *json_parse(const char *text) {
  if (!text)
    return NULL;
  for (const char *p = text; *p; p++)
    if (*p == '\\') {
      p++;
      if (!*p)
        return NULL;
      if (*p == 'u' && !strncmp(p, "u0000", 5))
        return NULL;
    }
  return cJSON_ParseWithOpts(text, NULL, 1);
}
int valid_endpoint(const char *url) {
  int https = !strncmp(url, "https://", 8);
  const char *p = url + (https ? 8 : 7);
  if (!https && strncmp(url, "http://", 7))
    return 0;
  if (strpbrk(url, "@#\\"))
    return 0;
  for (const unsigned char *q = (const unsigned char *)url; *q; q++)
    if (*q <= 32 || *q == 127)
      return 0;
  const char *end = p + strcspn(p, "/?");
  if (end == p)
    return 0;
  if (!https) {
    const char *port = NULL;
    if (!strncmp(p, "localhost:", 10))
      port = p + 10;
    else if (!strncmp(p, "127.0.0.1:", 10))
      port = p + 10;
    else
      return 0;
    if (port >= end)
      return 0;
    unsigned long n = 0;
    for (const char *q = port; q < end; q++) {
      if (*q < '0' || *q > '9')
        return 0;
      n = n * 10 + (*q - '0');
      if (n > 65535)
        return 0;
    }
    if (!n)
      return 0;
  }
  return 1;
}
static char *protected_paths[2];
static struct stat protected_stats[2];
static int protected_exists[2], protected_count;
void protect_file(const char *path) {
  if (!path || protected_count >= 2)
    return;
  char resolved[4096];
  char *name = NULL;
  if (realpath(path, resolved))
    name = gc_copy(resolved);
  else {
    char *copy = gc_copy(path), *slash = strrchr(copy, '/');
    char *leaf = gc_copy(slash ? slash + 1 : copy);
    if (slash)
      *slash = 0;
    if (realpath(slash ? (*copy ? copy : "/") : ".", resolved)) {
      Buffer b = {0};
      buf_text(&b, resolved);
      buf_text(&b, "/");
      buf_text(&b, leaf);
      name = b.data;
    }
    free(copy);
    free(leaf);
  }
  if (!name) {
    fputs("gucode: cannot resolve protected file path\n", stderr);
    exit(1);
  }
  int i = protected_count++;
  protected_paths[i] = name;
  protected_exists[i] = stat(path, &protected_stats[i]) == 0;
}
int file_protected(const char *path) {
  struct stat st;
  int exists = stat(path, &st) == 0;
  for (int i = 0; i < protected_count; i++) {
    if (!strcmp(path, protected_paths[i]))
      return 1;
    struct stat current;
    if (stat(protected_paths[i], &current) == 0 && exists &&
        st.st_dev == current.st_dev && st.st_ino == current.st_ino)
      return 1;
    if (exists && protected_exists[i] &&
        st.st_dev == protected_stats[i].st_dev &&
        st.st_ino == protected_stats[i].st_ino)
      return 1;
  }
  return 0;
}
