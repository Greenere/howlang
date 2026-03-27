#ifndef HOWLANG_COMMON_H
#define HOWLANG_COMMON_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <assert.h>
#include <unistd.h>
#include <setjmp.h>
#define UNUSED(x) (void)(x)
typedef struct
{
    char *buf;
    int len;
    int cap;
} Buf;
void buf_push(Buf *b, char c);
void buf_append(Buf *b, const char *s);
char *buf_done(Buf *b);
void how_set_source_context(const char *name, const char *text);
const char *how_current_source_name(void);
void print_source_context(FILE *f, int line, int col);
void die(const char *fmt, ...);
void die_at(int line, int col, const char *fmt, ...);
char *xstrdup(const char *s);
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
void how_repl_begin(void);
void how_repl_end(void);
int how_repl_is_active(void);
void how_repl_set_errorf(const char *fmt, ...);
const char *how_repl_error(void);
int how_repl_setjmp(void);
void how_repl_longjmp(void);
#endif
