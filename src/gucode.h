#ifndef GUCODE_H
#define GUCODE_H
#define _POSIX_C_SOURCE 200809L
#include "cJSON.h"
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#define GUCODE_VERSION "0.1.0"
#define GC_LIMIT (8u * 1024u * 1024u)
typedef struct {
  char *data;
  size_t len, cap;
} Buffer;
extern volatile sig_atomic_t interrupted;
extern int use_color;
void *gc_alloc(size_t n);
char *gc_copy(const char *s);
void buf_add(Buffer *b, const char *s, size_t n);
void buf_text(Buffer *b, const char *s);
void buf_free(Buffer *b);
char *read_file(const char *path, size_t limit);
int atomic_write(const char *path, const char *text, size_t n,
                 int private_file);
void terminal_text(const char *s);
void terminal_write(FILE *out, const char *s);
const char *json_string(const cJSON *j, const char *key);
cJSON *json_parse(const char *text);
int valid_endpoint(const char *url);
void protect_file(const char *path);
int file_protected(const char *path);
cJSON *message(const char *role, const char *content);
cJSON *tool_definitions(void);
cJSON *run_tool(const char *name, const char *arguments, int yes, long timeout);
cJSON *api_turn(cJSON *messages, const char *endpoint, const char *model,
                const char *key, char *error, size_t cap);
#endif
