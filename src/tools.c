#include "gucode.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
static double seconds(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}
static cJSON *result(int ok, const char *text) {
  cJSON *j = cJSON_CreateObject();
  cJSON_AddBoolToObject(j, "ok", ok);
  char *clean = utf8_clean(text);
  if (!clean) {
    cJSON_Delete(j);
    return result(0, "Tool output exceeds 8 MiB after UTF-8 repair");
  }
  cJSON_AddStringToObject(j, "output", clean);
  free(clean);
  return j;
}
static cJSON *error_result(const char *msg) { return result(0, msg); }
cJSON *tool_definitions(void) {
  const char *schema =
      "["
      "{\"type\":\"function\",\"function\":{\"name\":\"read_file\","
      "\"description\":\"Read a UTF-8 text file inside the working directory, "
      "at most 256 "
      "KiB.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{"
      "\"type\":\"string\"}},\"required\":[\"path\"],\"additionalProperties\":"
      "false}}},"
      "{\"type\":\"function\",\"function\":{\"name\":\"list_files\","
      "\"description\":\"List one directory inside the working "
      "directory.\",\"parameters\":{\"type\":\"object\",\"properties\":{"
      "\"path\":{\"type\":\"string\"}},\"required\":[\"path\"],"
      "\"additionalProperties\":false}}},"
      "{\"type\":\"function\",\"function\":{\"name\":\"write_file\","
      "\"description\":\"Create or replace a text file atomically. Read "
      "existing files first. Parent directory must exist. Requires "
      "permission.\",\"parameters\":{\"type\":\"object\",\"properties\":{"
      "\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},"
      "\"required\":[\"path\",\"content\"],\"additionalProperties\":false}}},"
      "{\"type\":\"function\",\"function\":{\"name\":\"edit_file\","
      "\"description\":\"Replace exactly one occurrence of old_text with "
      "new_text. Fails if zero or multiple matches. Requires "
      "permission.\",\"parameters\":{\"type\":\"object\",\"properties\":{"
      "\"path\":{\"type\":\"string\"},\"old_text\":{\"type\":\"string\"},\"new_"
      "text\":{\"type\":\"string\"}},\"required\":[\"path\",\"old_text\",\"new_"
      "text\"],\"additionalProperties\":false}}},"
      "{\"type\":\"function\",\"function\":{\"name\":\"shell\",\"description\":"
      "\"Run a shell command in the working directory; capture stdout/stderr "
      "and exit code. Commands are not sandboxed. Requires permission. Use for "
      "search, builds and "
      "tests.\",\"parameters\":{\"type\":\"object\",\"properties\":{"
      "\"command\":{\"type\":\"string\"}},\"required\":[\"command\"],"
      "\"additionalProperties\":false}}}"
      "]";
  return cJSON_Parse(schema);
}
static int approve(const char *name, const char *arguments, int yes) {
  if (use_color)
    fputs("\033[33m", stdout);
  fputs("\n[", stdout);
  terminal_text(name);
  fputs("] ", stdout);
  terminal_text(arguments);
  if (use_color)
    fputs("\033[0m", stdout);
  fputc('\n', stdout);
  if (yes)
    return 1;
  if (!isatty(STDIN_FILENO)) {
    fputs("Denied: noninteractive execution requires --yes or --yolo for edits and "
          "commands.\n",
          stderr);
    return 0;
  }
  fputs("Allow this action? [y/N] ", stdout);
  fflush(stdout);
  char line[32];
  return fgets(line, sizeof line, stdin) && !interrupted &&
         (line[0] == 'y' || line[0] == 'Y');
}
static char *path_in_workspace(const char *path, int create) {
  if (!path || !*path) {
    errno = EINVAL;
    return NULL;
  }
  char root[PATH_MAX], resolved[PATH_MAX];
  if (!getcwd(root, sizeof root))
    return NULL;
  if (!realpath(path, resolved)) {
    if (!create || errno != ENOENT)
      return NULL;
    char *p = gc_copy(path), *slash = strrchr(p, '/');
    const char *leaf = slash ? slash + 1 : p;
    if (!*leaf || !strcmp(leaf, ".") || !strcmp(leaf, "..")) {
      free(p);
      errno = EINVAL;
      return NULL;
    }
    char *name = gc_copy(leaf);
    if (slash)
      *slash = 0;
    const char *parent = slash ? (*p ? p : "/") : ".";
    if (!realpath(parent, resolved)) {
      free(name);
      free(p);
      return NULL;
    }
    size_t n = strlen(resolved);
    if (n + strlen(name) + 2 >= sizeof resolved) {
      free(name);
      free(p);
      errno = ENAMETOOLONG;
      return NULL;
    }
    strcat(resolved, "/");
    strcat(resolved, name);
    free(name);
    free(p);
  }
  size_t n = strlen(root);
  if (strcmp(root, "/") &&
      (strncmp(resolved, root, n) || (resolved[n] && resolved[n] != '/'))) {
    errno = EACCES;
    return NULL;
  }
  /* Agent session state is not a source file. */
  if (file_protected(resolved) || strstr(resolved, "/.gucode/") ||
      !strcmp(resolved + n, "/.gucode")) {
    errno = EACCES;
    return NULL;
  }
  return gc_copy(resolved);
}
static cJSON *shell_command(const char *command, long timeout) {
  int fds[2];
  if (pipe(fds))
    return error_result(strerror(errno));
  /* Closed inherited stdio must not alias later dup2/close spawn actions. */
  for (int i = 0; i < 2; i++) {
    if (fds[i] <= STDERR_FILENO) {
      int moved = fcntl(fds[i], F_DUPFD, STDERR_FILENO + 1);
      if (moved < 0) {
        close(fds[0]);
        close(fds[1]);
        return error_result("Cannot relocate command pipe");
      }
      close(fds[i]);
      fds[i] = moved;
    }
  }
  if (fds[0] >= FD_SETSIZE) {
    close(fds[0]);
    close(fds[1]);
    return error_result("Too many open descriptors");
  }
  posix_spawn_file_actions_t fa;
  posix_spawnattr_t attr;
  int rc = posix_spawn_file_actions_init(&fa);
  if (rc) {
    close(fds[0]);
    close(fds[1]);
    return error_result(strerror(rc));
  }
  rc = posix_spawnattr_init(&attr);
  if (rc) {
    posix_spawn_file_actions_destroy(&fa);
    close(fds[0]);
    close(fds[1]);
    return error_result(strerror(rc));
  }
  if ((rc = posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null",
                                             O_RDONLY, 0)) ||
      (rc = posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO)) ||
      (rc = posix_spawn_file_actions_adddup2(&fa, fds[1], STDERR_FILENO)) ||
      (rc = posix_spawn_file_actions_addclose(&fa, fds[0])) ||
      (rc = posix_spawn_file_actions_addclose(&fa, fds[1])) ||
      (rc = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP)) ||
      (rc = posix_spawnattr_setpgroup(&attr, 0))) {
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(fds[0]);
    close(fds[1]);
    return error_result(strerror(rc));
  }
  char *argv[] = {"sh", "-c", (char *)command, NULL};
  pid_t pid;
  rc = posix_spawn(&pid, "/bin/sh", &fa, &attr, argv, environ);
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  close(fds[1]);
  if (rc) {
    close(fds[0]);
    return error_result(strerror(rc));
  }
  if (fcntl(fds[0], F_SETFL, O_NONBLOCK) < 0) {
    kill(-pid, SIGKILL);
    close(fds[0]);
    waitpid(pid, NULL, 0);
    return error_result("Cannot configure command pipe");
  }
  Buffer output = {0};
  double start = seconds(), stop_time = 0, exit_time = 0;
  int status = 0, exited = 0, eof = 0, stopped = 0, truncated = 0;
  while (!exited || !eof) {
    if (!stopped && (interrupted || seconds() - start >= timeout)) {
      if (kill(-pid, SIGKILL) < 0 && errno != ESRCH) {
        kill(pid, SIGKILL);
      }
      stopped = 1;
      stop_time = seconds();
    }
    fd_set readset;
    FD_ZERO(&readset);
    FD_SET(fds[0], &readset);
    struct timeval tv = {0, 100000};
    int ready = select(fds[0] + 1, &readset, NULL, NULL, &tv);
    if (ready < 0 && errno != EINTR) {
      kill(-pid, SIGKILL);
      stopped = 1;
      break;
    }
    char data[4096];
    ssize_t n;
    n = -1;
    int drains = 0;
    while (drains++ < 16 && (n = read(fds[0], data, sizeof data)) > 0) {
      size_t take = (size_t)n;
      if (take > 65536 - output.len) {
        take = 65536 - output.len;
        truncated = 1;
      }
      for (size_t i = 0; i < take; i++)
        if (!data[i])
          data[i] = '?';
      buf_add(&output, data, take);
    }
    if (!n)
      eof = 1;
    if (!exited) {
      pid_t w = waitpid(pid, &status, WNOHANG);
      if (w == pid) {
        exited = 1;
        exit_time = seconds();
      } else if (w < 0 && errno != EINTR) {
        exited = 1;
        status = 1 << 8;
      }
    }
    if (stopped && seconds() - stop_time >= 2)
      break;
    /* A child must not leave descendants running after its command finishes. */
    if (exited && !eof && !stopped && seconds() - exit_time >= 1) {
      if (kill(-pid, SIGKILL) < 0 && errno != ESRCH) {
        kill(pid, SIGKILL);
      }
      stopped = 1;
      stop_time = seconds();
    }
  }
  kill(-pid, SIGKILL);
  close(fds[0]);
  if (!exited)
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
  if (truncated)
    buf_text(&output, "\n[output truncated at 65536 bytes]");
  if (stopped)
    buf_text(&output, interrupted
                          ? "\n[interrupted]"
                          : "\n[timeout or lingering descendants terminated]");
  int code = WIFEXITED(status)
                 ? WEXITSTATUS(status)
                 : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
  cJSON *r = result(code == 0 && !stopped, output.data ? output.data : "");
  cJSON_AddNumberToObject(r, "exit_code", code);
  cJSON_AddBoolToObject(r, "terminated", stopped);
  buf_free(&output);
  return r;
}
cJSON *run_tool(const char *name, const char *arguments, int yes,
                long timeout) {
  if (!name || !arguments)
    return error_result("Missing tool name or arguments");
  cJSON *a = json_parse(arguments);
  if (!cJSON_IsObject(a)) {
    cJSON_Delete(a);
    return error_result("Arguments must be a JSON object");
  }
  cJSON *r = NULL;
  char *path = NULL;
  if (!strcmp(name, "shell")) {
    const char *cmd = json_string(a, "command");
    if (!cmd)
      r = error_result("Missing command");
    else if (!approve(name, arguments, yes))
      r = error_result("User denied action");
    else
      r = shell_command(cmd, timeout);
    goto done;
  }
  int writing = !strcmp(name, "write_file") || !strcmp(name, "edit_file");
  if (!writing && strcmp(name, "read_file") && strcmp(name, "list_files")) {
    r = error_result("Unknown tool");
    goto done;
  }
  path = path_in_workspace(json_string(a, "path"), !strcmp(name, "write_file"));
  if (!path) {
    r = error_result(strerror(errno));
    goto done;
  }
  if (writing && !approve(name, arguments, yes)) {
    r = error_result("User denied action");
    goto done;
  }
  if (!strcmp(name, "list_files")) {
    DIR *d = opendir(path);
    if (!d) {
      r = error_result(strerror(errno));
      goto done;
    }
    Buffer b = {0};
    struct dirent *e;
    while ((e = readdir(d))) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") ||
          !strcmp(e->d_name, ".gucode"))
        continue;
      if (b.len + strlen(e->d_name) + 1 > 65536) {
        buf_text(&b, "[listing truncated]\n");
        break;
      }
      buf_text(&b, e->d_name);
      buf_text(&b, "\n");
    }
    closedir(d);
    r = result(1, b.data ? b.data : "");
    buf_free(&b);
  } else if (!strcmp(name, "write_file")) {
    const char *content = json_string(a, "content");
    if (!content)
      r = error_result("Missing content");
    else
      r = atomic_write(path, content, strlen(content), 0)
              ? error_result(strerror(errno))
              : result(1, "File written");
  } else {
    char *text = read_file(path, 256 * 1024);
    if (!text) {
      r = error_result(strerror(errno));
      goto done;
    }
    if (!strcmp(name, "read_file"))
      r = result(1, text);
    else {
      const char *old = json_string(a, "old_text"),
                 *newtext = json_string(a, "new_text");
      char *match = old && *old ? strstr(text, old) : NULL;
      if (!old || !*old || !newtext || !match || strstr(match + 1, old))
        r = error_result(
            "edit_file requires exactly one match of nonempty old_text");
      else {
        Buffer b = {0};
        buf_add(&b, text, (size_t)(match - text));
        buf_text(&b, newtext);
        buf_text(&b, match + strlen(old));
        r = atomic_write(path, b.data, b.len, 0) ? error_result(strerror(errno))
                                                 : result(1, "File edited");
        buf_free(&b);
      }
    }
    free(text);
  }
done:
  free(path);
  cJSON_Delete(a);
  return r;
}
