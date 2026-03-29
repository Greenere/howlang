/*
 * common.h — shared utilities used across the frontend and runtime:
 *   dynamic string buffers, memory helpers, error reporting, and REPL state.
 */
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

/* Dynamic byte buffer — append chars or strings, finalise to a heap string. */
typedef struct
{
    char *buf;
    int len;
    int cap;
} Buf;
void  buf_push(Buf *b, char c);
void  buf_append(Buf *b, const char *s);
char *buf_done(Buf *b);  /* NUL-terminates, transfers ownership; resets Buf to zero */

/* Source context — set once per file/eval so error messages can print the line. */
void        how_set_source_context(const char *name, const char *text);
const char *how_current_source_name(void);
void        print_source_context(FILE *f, int line, int col);

/* Fatal errors — print a message and exit(1). */
void die(const char *fmt, ...);
void die_at(int line, int col, const char *fmt, ...);

/* Checked memory helpers — abort on allocation failure. */
char *xstrdup(const char *s);
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);

/* REPL state — the interactive loop uses setjmp/longjmp to recover from parse
   and runtime errors without terminating the process. */
void        how_repl_begin(void);
void        how_repl_end(void);
int         how_repl_is_active(void);
void        how_repl_set_errorf(const char *fmt, ...);
void        how_repl_set_loc_errorf(const char *kind, int line, int col, const char *fmt, ...);
const char *how_repl_error(void);
jmp_buf    *how_repl_jmpbuf(void);
void        how_repl_longjmp(void);

#endif
