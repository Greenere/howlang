#include "howlang_runtime.h"
#include <termios.h>

/* ── REPL line editor with history (no readline dependency) ─────────────── */
#include <termios.h>

#define REPL_HIST_MAX 500
#define REPL_LINE_MAX 4096

static char  *repl_history[REPL_HIST_MAX];
static int    repl_hist_len = 0;

static void repl_hist_push(const char *line) {
    if (!line || !line[0]) return;
    /* Don't add duplicates of the immediately previous entry */
    if (repl_hist_len > 0 && !strcmp(repl_history[repl_hist_len-1], line)) return;
    if (repl_hist_len == REPL_HIST_MAX) {
        free(repl_history[0]);
        memmove(repl_history, repl_history+1, (REPL_HIST_MAX-1)*sizeof(char*));
        repl_hist_len--;
    }
    repl_history[repl_hist_len++] = xstrdup(line);
}

/* Read one key from stdin; returns char or special codes */
#define KEY_UP    0x100
#define KEY_DOWN  0x101
#define KEY_LEFT  0x102
#define KEY_RIGHT 0x103
#define KEY_HOME  0x104
#define KEY_END   0x105
#define KEY_DEL   0x106
#define KEY_CTRL_A 0x107
#define KEY_CTRL_E 0x108
#define KEY_CTRL_K 0x109
#define KEY_CTRL_U 0x10A

static int repl_read_key(void) {
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) <= 0) return -1;
    if (c == 0x1b) {
        unsigned char seq[4];
        if (read(STDIN_FILENO, &seq[0], 1) <= 0) return 0x1b;
        if (seq[0] == '[') {
            if (read(STDIN_FILENO, &seq[1], 1) <= 0) return 0x1b;
            if (seq[1] == 'A') return KEY_UP;
            if (seq[1] == 'B') return KEY_DOWN;
            if (seq[1] == 'C') return KEY_RIGHT;
            if (seq[1] == 'D') return KEY_LEFT;
            if (seq[1] == 'H') return KEY_HOME;
            if (seq[1] == 'F') return KEY_END;
            if (seq[1] == '3') {
                read(STDIN_FILENO, &seq[2], 1);
                return KEY_DEL;
            }
        }
        return 0x1b;
    }
    if (c == 1)  return KEY_CTRL_A;
    if (c == 5)  return KEY_CTRL_E;
    if (c == 11) return KEY_CTRL_K;
    if (c == 21) return KEY_CTRL_U;
    return (int)c;
}

static int repl_readline(const char *prompt, char *buf, int maxlen) {
    /* Check if stdin is a tty */
    if (!isatty(STDIN_FILENO)) {
        printf("%s", prompt); fflush(stdout);
        if (!fgets(buf, maxlen, stdin)) return -1;
        int l = strlen(buf);
        if (l > 0 && buf[l-1] == '\n') buf[l-1] = 0;
        return 0;
    }

    /* Set raw mode */
    struct termios old_t, raw_t;
    tcgetattr(STDIN_FILENO, &old_t);
    raw_t = old_t;
    raw_t.c_lflag &= ~(ICANON | ECHO);
    raw_t.c_cc[VMIN] = 1;
    raw_t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw_t);

    /* Line buffer */
    char line[REPL_LINE_MAX]; line[0] = 0;
    int  len = 0;           /* chars in line */
    int  cur = 0;           /* cursor position */
    int  hist_idx = repl_hist_len;  /* current history position */
    char saved[REPL_LINE_MAX]; saved[0] = 0;  /* saved line while browsing */

    printf("%s", prompt); fflush(stdout);

    int done = 0, result = 0;
    while (!done) {
        int k = repl_read_key();
        if (k < 0) { result = -1; done = 1; break; }

        if (k == '\n' || k == '\r') {
            done = 1;

        } else if (k == 127 || k == 8) {   /* Backspace */
            if (cur > 0) {
                memmove(line+cur-1, line+cur, len-cur+1);
                cur--; len--;
            }

        } else if (k == KEY_DEL) {          /* Delete forward */
            if (cur < len) {
                memmove(line+cur, line+cur+1, len-cur);
                len--;
            }

        } else if (k == KEY_LEFT) {
            if (cur > 0) cur--;

        } else if (k == KEY_RIGHT) {
            if (cur < len) cur++;

        } else if (k == KEY_HOME || k == KEY_CTRL_A) {
            cur = 0;

        } else if (k == KEY_END || k == KEY_CTRL_E) {
            cur = len;

        } else if (k == KEY_CTRL_K) {      /* Kill to end of line */
            line[cur] = 0; len = cur;

        } else if (k == KEY_CTRL_U) {      /* Kill whole line */
            line[0] = 0; len = 0; cur = 0;

        } else if (k == KEY_UP) {
            if (hist_idx == repl_hist_len) {
                /* Save current line before browsing */
                strncpy(saved, line, REPL_LINE_MAX-1);
            }
            if (hist_idx > 0) {
                hist_idx--;
                strncpy(line, repl_history[hist_idx], REPL_LINE_MAX-1);
                len = strlen(line); cur = len;
            }

        } else if (k == KEY_DOWN) {
            if (hist_idx < repl_hist_len) {
                hist_idx++;
                if (hist_idx == repl_hist_len) {
                    strncpy(line, saved, REPL_LINE_MAX-1);
                } else {
                    strncpy(line, repl_history[hist_idx], REPL_LINE_MAX-1);
                }
                len = strlen(line); cur = len;
            }

        } else if (k == 4) {               /* Ctrl-D */
            if (len == 0) { result = -1; done = 1; }
            /* else treat as delete-forward */

        } else if (k >= 32 && k < 127) {   /* Printable ASCII */
            if (len + 1 < REPL_LINE_MAX - 1) {
                memmove(line+cur+1, line+cur, len-cur+1);
                line[cur++] = (char)k;
                len++;
            }
        }
        /* Redraw line */
        if (!done) {
            /* Move to start of line, clear to end */
            printf("\r\033[K%s%s", prompt, line);
            /* Move cursor to correct position */
            if (cur < len) {
                printf("\r\033[%dC", (int)(strlen(prompt)) + cur);
            }
            fflush(stdout);
        }
    }

    /* Restore terminal */
    tcsetattr(STDIN_FILENO, TCSANOW, &old_t);

    if (result == -1) { printf("\n"); return -1; }
    printf("\n");
    line[len] = 0;
    strncpy(buf, line, maxlen-1);
    buf[maxlen-1] = 0;
    return 0;
}

/* Check if input looks like a bare expression (not a statement)
 * Returns 1 if we should auto-print the result */
static int repl_should_autoprint(const char *src) {
    /* Skip leading whitespace */
    while (*src == ' ' || *src == '\t') src++;
    /* Statements start with these keywords - don't auto-print */
    const char *no_print[] = {
        "var ", "how ", "where ", "print(",
        "if ", "(:)", "(:)=",
        NULL
    };
    for (int i = 0; no_print[i]; i++) {
        if (!strncmp(src, no_print[i], strlen(no_print[i]))) return 0;
    }
    /* Also skip if it contains an assignment at top level */
    /* Simple heuristic: if it has = but not == and not =>  it's an assign */
    const char *p = src;
    int depth = 0;
    while (*p) {
        if (*p == '(' || *p == '[' || *p == '{') depth++;
        else if (*p == ')' || *p == ']' || *p == '}') depth--;
        else if (depth == 0 && *p == '=' && *(p+1) != '=') {
            /* Check it's not !=, <=, >=, +=, -=, *=, /= */
            char prev = (p > src) ? *(p-1) : 0;
            if (prev != '!' && prev != '<' && prev != '>' &&
                prev != '+' && prev != '-' && prev != '*' && prev != '/') {
                return 0;  /* plain assignment - don't auto-print */
            }
        }
        p++;
    }
    return 1;
}

void repl(Env *env) {
    printf("Howlang  |  Ctrl-D or quit() to exit\n");
    char buf[REPL_LINE_MAX];

    while (1) {
        int r = repl_readline(">> ", buf, sizeof(buf));
        if (r < 0) { printf("\n"); break; }

        /* Trim */
        char *line = buf;
        while (*line == ' ' || *line == '\t') line++;
        int len = strlen(line);
        while (len > 0 && (line[len-1] == ' ' || line[len-1] == '\t')) line[--len] = 0;
        if (!len) continue;

        if (!strcmp(line, "quit()") || !strcmp(line, "exit")) break;

        repl_hist_push(line);

        /* If it looks like a bare expression, auto-print the result */
        char runbuf[REPL_LINE_MAX + 64];
        int autoprint = repl_should_autoprint(line);
        if (autoprint) {
            /* Wrap: evaluate expr, print if not none */
            snprintf(runbuf, sizeof(runbuf),
                "var _ = ((%s))\n"
                "_ != none: print(_)\n",
                line);
        } else {
            strncpy(runbuf, line, sizeof(runbuf)-1);
        }

        /* Run with graceful error recovery */
        how_repl_begin();
        if (how_repl_setjmp() == 0) {
            how_run_source("<repl>", runbuf, env);
        } else {
            /* Error was caught — print it nicely and continue */
            fprintf(stderr, "\033[31m[Error] %s\033[0m\n", how_repl_error());
        }
        how_repl_end();
    }
}




int main(int argc, char **argv) {
    Env *globals = how_runtime_bootstrap(argc, argv);

    if (argc < 2) {
        repl(globals);
        how_runtime_shutdown();
        return 0;
    }

    const char *script = argv[1];
    char dir[4096];
    strncpy(dir, script, sizeof(dir)-1);
    dir[sizeof(dir)-1] = 0;
    char *dslash = strrchr(dir, '/');
    if (dslash) { dslash[1] = 0; how_add_import_dir(dir); }

    FILE *f = fopen(script, "r");
    if (!f) {
        if (!strcmp(script, "/dev/stdin") || !strcmp(script, "-")) f = stdin;
        else { fprintf(stderr, "cannot open '%s': %s\n", script, strerror(errno)); return 1; }
    }

    size_t sz = 0, cap = 4096, n;
    char *source = xmalloc(cap);
    while ((n = fread(source + sz, 1, cap - sz - 1, f)) > 0) {
        sz += n;
        if (sz + 1 >= cap) { cap *= 2; source = xrealloc(source, cap); }
    }
    source[sz] = 0;
    if (f != stdin) fclose(f);

    how_run_source(script, source, globals);
    free(source);
    how_runtime_shutdown();
    return 0;
}
