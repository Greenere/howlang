#include "common.h"

/* ── Source context ──────────────────────────────────────────────────────── */

/* Set once per file/eval so error messages can print the offending line. */
static const char *g_current_source_name = NULL;
static const char *g_current_source_text = NULL;

void how_set_source_context(const char *name, const char *text)
{
    g_current_source_name = name;
    g_current_source_text = text;
}

const char *how_current_source_name(void) { return g_current_source_name; }

/* Walk to the requested line and print it with a caret pointing at col. */
void print_source_context(FILE *f, int line, int col)
{
    if (!g_current_source_text || line <= 0)
        return;
    const char *src = g_current_source_text, *cur = src;
    int cur_line = 1;
    while (*cur && cur_line < line)
    {
        if (*cur == '\n')
            cur_line++;
        cur++;
    }
    if (cur_line != line)
        return;
    const char *line_start = cur;
    while (*cur && *cur != '\n')
        cur++;
    fprintf(f, "%4d | ", line);
    fwrite(line_start, 1, (size_t)(cur - line_start), f);
    fputc('\n', f);
    if (col > 0)
    {
        fprintf(f, "     | ");
        /* preserve tabs so the caret lines up correctly */
        for (int i = 1; i < col; i++)
            fputc((line_start[i - 1] == '\t') ? '\t' : ' ', f);
        fprintf(f, "^\n");
    }
}

/* ── REPL error recovery ─────────────────────────────────────────────────── */

/* The REPL uses setjmp/longjmp so parse and runtime errors return to the
   prompt instead of terminating the process. */
static jmp_buf g_repl_jmp;
static int     g_repl_active  = 0;
static char    g_repl_errmsg[512];

void        how_repl_begin(void)  { g_repl_active = 1; }
void        how_repl_end(void)    { g_repl_active = 0; }
int         how_repl_is_active(void) { return g_repl_active; }
int         how_repl_setjmp(void) { return setjmp(g_repl_jmp); }
void        how_repl_longjmp(void){ longjmp(g_repl_jmp, 1); }
const char *how_repl_error(void)  { return g_repl_errmsg; }

void how_repl_set_errorf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_repl_errmsg, sizeof(g_repl_errmsg), fmt, ap);
    va_end(ap);
}

/* ── Fatal error helpers ─────────────────────────────────────────────────── */

/* In REPL mode, errors longjmp back to the prompt instead of calling exit(). */
void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (g_repl_active)
    {
        how_repl_set_errorf("%s", msg);
        how_repl_longjmp();
    }
    fprintf(stderr, "\033[31m[RuntimeError]\033[0m %s\n", msg);
    exit(1);
}

/* Like die(), but also prints the source location and the offending line. */
void die_at(int line, int col, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (g_repl_active)
    {
        how_repl_set_errorf("%s", msg);
        how_repl_longjmp();
    }
    fprintf(stderr, "\033[31m[RuntimeError]\033[0m %s\n", msg);
    if (g_current_source_name && line > 0)
    {
        if (col > 0)
            fprintf(stderr, "  --> %s:%d:%d\n", g_current_source_name, line, col);
        else
            fprintf(stderr, "  --> %s:%d\n", g_current_source_name, line);
    }
    if (line > 0)
        print_source_context(stderr, line, col);
    exit(1);
}

/* ── Checked memory helpers ──────────────────────────────────────────────── */

char *xstrdup(const char *s)
{
    size_t len = strlen(s) + 1;
    char *p = malloc(len);
    if (!p)
        die("out of memory");
    memcpy(p, s, len);
    return p;
}

void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p)
        die("out of memory");
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q)
        die("out of memory");
    return q;
}

/* ── Dynamic string buffer ───────────────────────────────────────────────── */

void buf_push(Buf *b, char c)
{
    if (b->len + 1 >= b->cap)
    {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->buf = xrealloc(b->buf, b->cap);
    }
    b->buf[b->len++] = c;
    b->buf[b->len] = '\0';  /* keep NUL-terminated at all times */
}

void buf_append(Buf *b, const char *s)
{
    while (*s)
        buf_push(b, *s++);
}

/* Finalise the buffer: returns the heap string and resets Buf to zero.
   Caller owns the returned string. */
char *buf_done(Buf *b)
{
    char *r = b->buf ? b->buf : xstrdup("");
    b->buf = NULL;
    b->len = b->cap = 0;
    return r;
}
