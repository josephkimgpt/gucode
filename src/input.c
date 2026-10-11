#define _XOPEN_SOURCE 700
#include "gucode.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

/* The editor owns input only while a prompt is visible. No disk history: prompts
 * can contain private information. Each saved entry is bounded to 8 KiB. */
int input_failed;
static char *history[32];
static size_t history_len;
static struct termios saved;
static int active, registered, pending_byte = -1;
static int restore(void) {
  if (active) {
    fputs("\033[?2004l", stdout);
    fflush(stdout);
    int rc;
    do { rc = tcsetattr(STDIN_FILENO, TCSANOW, &saved); } while (rc < 0 && errno == EINTR);
    if (rc < 0) return -1;
    active = 0;
  }
  return 0;
}
static void cleanup(void) {
  if (restore() < 0) perror("gucode: restoring terminal");
  for (size_t i = 0; i < history_len; i++) free(history[i]);
}
/* A timeout lets signals terminate reads and makes a lone Escape harmless. */
static int byte(int timeout) {
  if (pending_byte >= 0) { int c = pending_byte; pending_byte = -1; return c; }
  do {
    fd_set fds;
    FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = {0, 100000};
    int n = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
    if (interrupted) return -1;
    if (n < 0) { if (errno == EINTR) continue; return -3; }
    if (!n) { if (timeout) return -2; continue; }
    unsigned char c;
    ssize_t got = read(STDIN_FILENO, &c, 1);
    if (got == 1) return c;
    if (got < 0 && errno == EINTR) continue;
    return got < 0 ? -3 : -1;
  } while (!interrupted);
  return -1;
}
static size_t prev(const char *s, size_t p) {
  if (p) { --p; while (p && ((unsigned char)s[p] & 0xc0) == 0x80) --p; }
  return p;
}
static size_t next(const char *s, size_t len, size_t p) {
  if (p < len) { ++p; while (p < len && ((unsigned char)s[p] & 0xc0) == 0x80) ++p; }
  return p;
}
static int width(const char *s, size_t p, size_t end) {
  unsigned char c = (unsigned char)s[p];
  unsigned cp = c < 128 ? c : c & (end-p == 2 ? 31 : end-p == 3 ? 15 : 7);
  for (size_t i = p+1; i < end; i++) cp = (cp << 6) | ((unsigned char)s[i] & 63);
  int w = wcwidth((wchar_t)cp);
  return w < 0 ? 1 : w;
}
static void redraw(const char *prompt, Buffer *b, size_t cursor) {
  struct winsize ws;
  int cols = ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col ? ws.ws_col : 80;
  size_t plen = strlen(prompt);
  if (plen > (size_t)(cols > 4 ? cols/2 : 0)) plen = cols > 4 ? (size_t)cols/2 : 0;
  int room = cols - (int)plen - 1;
  if (room < 1) room = 1;
  size_t start = cursor;
  int left = 0;
  while (start) {
    size_t p = prev(b->data, start); int w = width(b->data, p, start);
    if (left + w >= room) break;
    start = p; left += w;
  }
  size_t end = start; int used = 0;
  while (end < b->len) {
    size_t n = next(b->data, b->len, end); int w = width(b->data, end, n);
    if (used + w > room) break;
    used += w; end = n;
  }
  fputs("\r\033[2K", stdout);
  fwrite(prompt, 1, plen, stdout);
  if (end > start) fwrite(b->data + start, 1, end-start, stdout);
  fputc('\r', stdout);
  if (plen + left) fprintf(stdout, "\033[%zuC", plen + left);
  fflush(stdout);
}
static void erase(Buffer *b, size_t from, size_t to) {
  memmove(b->data+from, b->data+to, b->len-to+1); b->len -= to-from;
}
static int insert(Buffer *b, size_t *cursor, const char *s, size_t n) {
  if (b->len + n > GC_LIMIT) { fputc('\a', stdout); return 0; }
  size_t old = b->len;
  buf_add(b, s, n);
  memmove(b->data+*cursor+n, b->data+*cursor, old-*cursor);
  memcpy(b->data+*cursor, s, n); *cursor += n; return 1;
}
static int plain(const char *prompt, char **line) {
  fputs(prompt, stdout); fflush(stdout);
  Buffer b = {0}; int c, oversized = 0;
  while (!interrupted && (c = fgetc(stdin)) != EOF && c != '\n') {
    char ch = (char)c;
    if (b.len < GC_LIMIT) buf_add(&b, &ch, 1); else oversized = 1;
  }
  if (oversized || interrupted || ferror(stdin) || (!b.len && feof(stdin))) {
    buf_free(&b); return oversized ? -2 : interrupted || feof(stdin) ? 0 : -1;
  }
  *line = b.data ? b.data : gc_copy(""); return 1;
}
int input_line(const char *prompt, char **line, int keep_history) {
  *line = NULL; pending_byte = -1;
  if (active && restore() < 0) return -1;
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return plain(prompt, line);
  if (tcgetattr(STDIN_FILENO, &saved) < 0) return -1;
  struct termios raw = saved;
  raw.c_lflag &= ~(ICANON | ECHO | IEXTEN | ISIG);
  raw.c_iflag &= ~(IXON | ICRNL | INLCR | IGNCR);
  raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) return -1;
  active = 1;
  if (!registered) { atexit(cleanup); registered = 1; }
  fputs("\033[?2004h", stdout);
  Buffer b = {0}; buf_text(&b, "");
  size_t cursor = 0, hi = history_len;
  char *draft = NULL, *killed = NULL;
  int result = 0, paste = 0, oversized = 0, read_error = 0;
  redraw(prompt, &b, cursor);
  while (!interrupted) {
    int c = byte(0);
    if (c < 0) { if (c == -3) { result = -1; read_error = errno; } break; }
    if (c == 27) {
      int k = byte(1);
      if(k==-3){result=-1;read_error=errno;break;}
      if(k==3){interrupted=1;break;}
      if (k == '[' || k == 'O') {
        char seq[32]; size_t n = 0;
        while (n < sizeof(seq)-1) {
          k = byte(1);
          if(k==-3){result=-1;read_error=errno;break;}
          if(k==3){interrupted=1;break;}
          if (k < 0) break;
          seq[n++] = (char)k;
          if (k >= 0x40 && k <= 0x7e) break;
        }
        if(interrupted || result<0)break;
        seq[n] = 0;
        if (!strcmp(seq,"200~")) { paste = 1; continue; }
        if (!strcmp(seq,"201~")) { paste = 0; redraw(prompt,&b,cursor); continue; }
        if (paste) continue;
        if (!strcmp(seq,"A")) c = 16;
        else if (!strcmp(seq,"B")) c = 14;
        else if (!strcmp(seq,"C")) c = 6;
        else if (!strcmp(seq,"D")) c = 2;
        else if (!strcmp(seq,"H") || !strcmp(seq,"1~") || !strcmp(seq,"7~")) c = 1;
        else if (!strcmp(seq,"F") || !strcmp(seq,"4~") || !strcmp(seq,"8~")) c = 5;
        else if (!strcmp(seq,"3~")) c = 4;
        else continue;
      } else {
        if (paste) continue;
        if (k == 'b') { while(cursor && b.data[prev(b.data,cursor)]==' ')cursor=prev(b.data,cursor); while(cursor && b.data[prev(b.data,cursor)]!=' ')cursor=prev(b.data,cursor); }
        else if (k == 'f') { while(cursor<b.len && b.data[cursor]==' ')cursor=next(b.data,b.len,cursor); while(cursor<b.len && b.data[cursor]!=' ')cursor=next(b.data,b.len,cursor); }
        redraw(prompt,&b,cursor); continue;
      }
    }
    if (c == 3) { interrupted = 1; break; }
    if (paste && (c == '\r' || c == '\n' || c == '\t')) c = ' ';
    if (paste && (c < 32 || c == 127)) continue;
    if (!paste && (c == '\r' || c == '\n')) { result = oversized ? -2 : 1; break; }
    if (c == 4) {
      if (!b.len) break;
      if (cursor < b.len) erase(&b,cursor,next(b.data,b.len,cursor));
    } else if (c == 1) cursor = 0;
    else if (c == 5) cursor = b.len;
    else if (c == 2) cursor = prev(b.data,cursor);
    else if (c == 6) cursor = next(b.data,b.len,cursor);
    else if (c == 127 || c == 8) { size_t p = prev(b.data,cursor); erase(&b,p,cursor); cursor = p; }
    else if (c == 11 || c == 21 || c == 23) {
      size_t from = c == 11 ? cursor : 0, to = c == 11 ? b.len : cursor;
      if (c == 23) { from=cursor; while(from && b.data[prev(b.data,from)]==' ')from=prev(b.data,from); while(from && b.data[prev(b.data,from)]!=' ')from=prev(b.data,from); }
      free(killed); killed = gc_alloc(to-from+1); memcpy(killed,b.data+from,to-from); killed[to-from]=0;
      erase(&b,from,to); cursor = from;
    } else if (c == 25) { if(killed && !insert(&b,&cursor,killed,strlen(killed)))oversized=1; }
    else if (c == 12) fputs("\033[H\033[2J",stdout);
    else if ((c == 16 || c == 14) && keep_history) {
      if ((c==16 && !hi) || (c==14 && hi==history_len)) continue;
      if (hi==history_len) { free(draft); draft=gc_copy(b.data); }
      if(c==16)hi--; else hi++;
      buf_free(&b);buf_text(&b,hi==history_len?draft:history[hi]);cursor=b.len;
    } else if (c >= 32 && c != 127) {
      char bytes[4] = {(char)c}; size_t n = 1;
      if (c >= 128) {
        size_t need = c>=0xc2 && c<=0xdf ? 2 : c>=0xe0 && c<=0xef ? 3 : c>=0xf0 && c<=0xf4 ? 4 : 0;
        if (!need) continue;
        while (n<need) { int v=byte(1); if(v<128 || v>191){ if(v>=0)pending_byte=v; else if(v==-3){result=-1;read_error=errno;} break; } bytes[n++]=(char)v; }
        if(result<0)break;
        if(n!=need)continue;
        if ((c==0xe0 && (unsigned char)bytes[1]<0xa0) || (c==0xed && (unsigned char)bytes[1]>=0xa0) || (c==0xf0 && (unsigned char)bytes[1]<0x90) || (c==0xf4 && (unsigned char)bytes[1]>=0x90))continue;
      }
      if(!insert(&b,&cursor,bytes,n))oversized=1;
    }
    if (!paste) redraw(prompt,&b,cursor);
  }
  if(restore()<0){result=-1;read_error=errno;}
  fputc('\n',stdout); fflush(stdout);
  free(draft);free(killed);
  if (result == 1 && keep_history && b.len && b.len <= 8192 && (!history_len || strcmp(history[history_len-1],b.data))) {
    if(history_len==32){free(history[0]);memmove(history,history+1,31*sizeof(*history));history_len--;}
    history[history_len++]=gc_copy(b.data);
  }
  if(result==1)*line=b.data;else buf_free(&b);
  if(read_error)errno=read_error;
  return result;
}
