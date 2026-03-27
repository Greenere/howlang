/*
 * howlang.c — single-file C interpreter for Howlang
 *
 * Build:  cc -O2 -o howlang howlang.c
 * Run:    ./howlang script.how
 *         ./howlang                  (REPL)
 *         ./howlang a.how b.how ...  (concatenate files)
 */

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

/* REPL error recovery: when in REPL mode, errors jump back instead of exit() */
static jmp_buf  g_repl_jmp;
static int       g_repl_active = 0;
static char      g_repl_errmsg[512];

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Forward declarations                                                        */
/* ─────────────────────────────────────────────────────────────────────────── */

typedef struct Value Value;
typedef struct Env   Env;
typedef struct Node  Node;
typedef struct NodeList NodeList;

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Utilities                                                                   */
/* ─────────────────────────────────────────────────────────────────────────── */

/* ── Source context for error messages ─────────────────────────────────────── */
static const char *g_current_source_name = NULL;
static const char *g_current_source_text = NULL;

static void set_source_context(const char *name, const char *text) {
    g_current_source_name = name;
    g_current_source_text = text;
}

/* Print the source line + caret for a given line/col */
static void print_source_context(FILE *f, int line, int col) {
    if (!g_current_source_text || line <= 0) return;
    const char *src = g_current_source_text;
    const char *cur = src;
    int cur_line = 1;
    while (*cur && cur_line < line) {
        if (*cur == '\n') cur_line++;
        cur++;
    }
    if (cur_line != line) return;
    const char *line_start = cur;
    while (*cur && *cur != '\n') cur++;
    fprintf(f, "%4d | ", line);
    fwrite(line_start, 1, (size_t)(cur - line_start), f);
    fputc('\n', f);
    if (col > 0) {
        fprintf(f, "     | ");
        for (int i = 1; i < col; i++)
            fputc((line_start[i-1] == '\t') ? '\t' : ' ', f);
        fprintf(f, "^\n");
    }
}

static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (g_repl_active) {
        snprintf(g_repl_errmsg, sizeof(g_repl_errmsg), "%s", msg);
        longjmp(g_repl_jmp, 1);
    }
    fprintf(stderr, "\033[31m[RuntimeError]\033[0m %s\n", msg);
    exit(1);
}

/* die with line+col — shows source context */
static void die_at(int line, int col, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (g_repl_active) {
        snprintf(g_repl_errmsg, sizeof(g_repl_errmsg), "%s", msg);
        longjmp(g_repl_jmp, 1);
    }
    fprintf(stderr, "\033[31m[RuntimeError]\033[0m %s\n", msg);
    if (g_current_source_name && line > 0) {
        if (col > 0)
            fprintf(stderr, "  --> %s:%d:%d\n", g_current_source_name, line, col);
        else
            fprintf(stderr, "  --> %s:%d\n", g_current_source_name, line);
    }
    if (line > 0) print_source_context(stderr, line, col);
    exit(1);
}

static char *xstrdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *p = malloc(len);
    if (!p) die("out of memory");
    memcpy(p, s, len);
    return p;
}

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) die("out of memory");
    return p;
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) die("out of memory");
    return q;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Dynamic string buffer                                                       */
/* ─────────────────────────────────────────────────────────────────────────── */

typedef struct { char *buf; int len; int cap; } Buf;

static void buf_push(Buf *b, char c) {
    if (b->len + 1 >= b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->buf = xrealloc(b->buf, b->cap);
    }
    b->buf[b->len++] = c;
    b->buf[b->len]   = '\0';
}

static void buf_append(Buf *b, const char *s) {
    while (*s) buf_push(b, *s++);
}

static char *buf_done(Buf *b) {
    char *r = b->buf ? b->buf : xstrdup("");
    b->buf = NULL; b->len = b->cap = 0;
    return r;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Lexer                                                                       */
/* ─────────────────────────────────────────────────────────────────────────── */

typedef enum {
    TT_EOF, TT_NUMBER, TT_STRING, TT_BOOL, TT_NONE, TT_IDENT,
    TT_VAR, TT_BREAK, TT_CONTINUE, TT_HOW, TT_WHERE, TT_AS,
    TT_LPAREN, TT_RPAREN, TT_LBRACE, TT_RBRACE, TT_LBRACKET, TT_RBRACKET,
    TT_COMMA, TT_DOT, TT_COLON, TT_DCOLON,
    TT_PLUS, TT_MINUS, TT_STAR, TT_SLASH, TT_PERCENT,
    TT_EQ, TT_PLUSEQ, TT_MINUSEQ, TT_STAREQ, TT_SLASHEQ, TT_PERCENTEQ,
    TT_EQEQ, TT_NEQ, TT_LT, TT_GT, TT_LTE, TT_GTE,
    TT_AND, TT_OR, TT_NOT,
    TT_DBANG,   /* !! — throw operator */
    TT_CATCH,   /* catch keyword */
} TT;

/* Token type → readable name */
static const char *token_type_name(int t) {
    switch ((TT)t) {
        case TT_EOF:       return "EOF";
        case TT_NUMBER:    return "NUMBER";
        case TT_STRING:    return "STRING";
        case TT_BOOL:      return "BOOL";
        case TT_NONE:      return "NONE";
        case TT_IDENT:     return "identifier";
        case TT_VAR:       return "'var'";
        case TT_BREAK:     return "'break'";
        case TT_CONTINUE:  return "'continue'";
        case TT_HOW:       return "'how'";
        case TT_WHERE:     return "'where'";
        case TT_AS:        return "'as'";
        case TT_LPAREN:    return "'('";
        case TT_RPAREN:    return "')'";
        case TT_LBRACE:    return "'{'";
        case TT_RBRACE:    return "'}'";
        case TT_LBRACKET:  return "'['";
        case TT_RBRACKET:  return "']'";
        case TT_COMMA:     return "','";
        case TT_DOT:       return "'.'";
        case TT_COLON:     return "':'";
        case TT_DCOLON:    return "'::'";
        case TT_PLUS:      return "'+'";
        case TT_MINUS:     return "'-'";
        case TT_STAR:      return "'*'";
        case TT_SLASH:     return "'/'";
        case TT_PERCENT:   return "'%'";
        case TT_EQ:        return "'='";
        case TT_PLUSEQ:    return "'+='";
        case TT_MINUSEQ:   return "'-='";
        case TT_STAREQ:    return "'*='";
        case TT_SLASHEQ:   return "'/='";
        case TT_PERCENTEQ: return "'%='";
        case TT_EQEQ:      return "'=='";
        case TT_NEQ:       return "'!='";
        case TT_LT:        return "'<'";
        case TT_GT:        return "'>'";
        case TT_LTE:       return "'<='";
        case TT_GTE:       return "'>='";
        case TT_AND:       return "'and'";
        case TT_OR:        return "'or'";
        case TT_NOT:       return "'not'";
        case TT_DBANG:     return "'!!'";
        case TT_CATCH:     return "'catch'";
        default:           return "<unknown>";
    }
}

/* Hint for common parse errors */
static const char *parse_hint_for_token(int expected, int got) {
    if (expected == (int)TT_RPAREN && got == (int)TT_COMMA)
        return "Hint: missing ')' earlier, or a trailing comma before expected expression.";
    if (expected == (int)TT_RBRACE)
        return "Hint: missing closing '}' for a block or map literal.";
    if (expected == (int)TT_RBRACKET)
        return "Hint: missing closing ']' for a bracket-call or class parameter list.";
    if (got == (int)TT_COLON)
        return "Hint: ':' is a side-effect branch; use '::' to return a value.";
    if (got == (int)TT_DCOLON)
        return "Hint: '::' is only valid inside function and loop bodies.";
    if (got == (int)TT_EQ)
        return "Hint: did you mean '==' for comparison?";
    return NULL;
}

typedef struct {
    TT    type;
    char *sval;   /* string / ident value */
    double nval;  /* numeric value */
    int    bval;  /* bool value */
    int    line;
    int    col;   /* column number (1-based) */
    int    raw;   /* 1 = single-quoted string (no interpolation) */
} Token;

typedef struct { Token *toks; int len; int cap; } TokenList;

static void tl_push(TokenList *tl, Token t) {
    if (tl->len + 1 >= tl->cap) {
        tl->cap = tl->cap ? tl->cap * 2 : 256;
        tl->toks = xrealloc(tl->toks, tl->cap * sizeof(Token));
    }
    tl->toks[tl->len++] = t;
}

static TokenList *lex(const char *src) {
    TokenList *tl = xmalloc(sizeof(*tl));
    tl->toks = NULL; tl->len = tl->cap = 0;
    int pos = 0, line = 1;
    int line_start = 0;   /* byte offset of line start, for col tracking */
    int n = strlen(src);

    while (pos < n) {
        /* skip whitespace */
        while (pos < n && (src[pos]==' '||src[pos]=='\t'||src[pos]=='\r'||src[pos]=='\n')) {
            if (src[pos] == '\n') { line++; line_start = pos + 1; }
            pos++;
        }
        if (pos >= n) break;
        char c = src[pos];

        /* comment */
        if (c == '#') {
            while (pos < n && src[pos] != '\n') pos++;
            continue;
        }
        /* semicolon — skip */
        if (c == ';') { pos++; continue; }

        Token t = {0}; t.line = line; t.col = pos - line_start + 1;

        /* number */
        if (isdigit(c)) {
            int start = pos;
            while (pos < n && isdigit(src[pos])) pos++;
            if (pos < n && src[pos] == '.' && pos+1 < n && isdigit(src[pos+1])) {
                pos++;
                while (pos < n && isdigit(src[pos])) pos++;
            }
            char tmp[64]; int len = pos - start;
            if (len >= 63) die_at(line, 0, "number literal too long");
            memcpy(tmp, src+start, len); tmp[len] = 0;
            t.type = TT_NUMBER; t.nval = atof(tmp);
            tl_push(tl, t); continue;
        }

        /* string */
        if (c == '"' || c == '\'') {
            char q = src[pos++]; Buf b = {0};
            int is_raw = (q == '\'');
            while (pos < n && src[pos] != q) {
                if (src[pos] == '\\') {
                    pos++;
                    char e = src[pos++];
                    switch(e) {
                        case 'n': buf_push(&b,'\n'); break;
                        case 't': buf_push(&b,'\t'); break;
                        case 'r': buf_push(&b,'\r'); break;
                        case '\\': buf_push(&b,'\\'); break;
                        case '"':  buf_push(&b,'"');  break;
                        case '\'': buf_push(&b,'\''); break;
                        default:   buf_push(&b,e);    break;
                    }
                } else {
                    if (src[pos] == '\n') line++;
                    buf_push(&b, src[pos++]);
                }
            }
            if (pos < n) pos++; /* closing quote */
            t.type = TT_STRING; t.sval = buf_done(&b); t.raw = is_raw;
            tl_push(tl, t); continue;
        }

        /* ident / keyword */
        if (isalpha(c) || c == '_') {
            int start = pos;
            while (pos < n && (isalnum(src[pos]) || src[pos] == '_')) pos++;
            int len = pos - start;
            char tmp[256];
            if (len >= 255) die_at(line, 0, "identifier too long");
            memcpy(tmp, src+start, len); tmp[len] = 0;
            if      (!strcmp(tmp,"var"))   { t.type = TT_VAR; }
            else if (!strcmp(tmp,"true"))  { t.type = TT_BOOL; t.bval = 1; }
            else if (!strcmp(tmp,"false")) { t.type = TT_BOOL; t.bval = 0; }
            else if (!strcmp(tmp,"none"))  { t.type = TT_NONE; }
            else if (!strcmp(tmp,"and"))   { t.type = TT_AND; }
            else if (!strcmp(tmp,"or"))    { t.type = TT_OR; }
            else if (!strcmp(tmp,"not"))   { t.type = TT_NOT; }
            else if (!strcmp(tmp,"break")) { t.type = TT_BREAK; }
            else if (!strcmp(tmp,"continue")) { t.type = TT_CONTINUE; }
            else if (!strcmp(tmp,"how"))   { t.type = TT_HOW; }
            else if (!strcmp(tmp,"where")) { t.type = TT_WHERE; }
            else if (!strcmp(tmp,"as"))    { t.type = TT_AS; }
            else if (!strcmp(tmp,"catch")) { t.type = TT_CATCH; }
            else { t.type = TT_IDENT; t.sval = xstrdup(tmp); }
            tl_push(tl, t); continue;
        }

        pos++; /* consume c */

#define TWO(a,b,r) if (pos < n && src[pos]==(b)) { pos++; t.type=(r); tl_push(tl,t); continue; }
        switch(c) {
        case ':': TWO(':',':',TT_DCOLON); t.type=TT_COLON; break;
        case '=': TWO('=','=',TT_EQEQ);  t.type=TT_EQ;    break;
        case '!':
            if (pos < n && src[pos]=='!') { pos++; t.type=TT_DBANG; break; }
            TWO('!','=',TT_NEQ); t.type=TT_NOT; break;
        case '<': TWO('<','=',TT_LTE);   t.type=TT_LT;    break;
        case '>': TWO('>','=',TT_GTE);   t.type=TT_GT;    break;
        case '+': TWO('+','=',TT_PLUSEQ);    t.type=TT_PLUS;    break;
        case '-': TWO('-','=',TT_MINUSEQ);   t.type=TT_MINUS;   break;
        case '*': TWO('*','=',TT_STAREQ);    t.type=TT_STAR;    break;
        case '/': TWO('/','=',TT_SLASHEQ);   t.type=TT_SLASH;   break;
        case '%': TWO('%','=',TT_PERCENTEQ); t.type=TT_PERCENT; break;
        case '&':
            if (pos < n && src[pos] == '&') {
                pos++;
                t.type = TT_AND;
            } else {
                die_at(line, pos - line_start + 1, "unexpected '&' — use '&&' for logical and");
            }
            break;
        case '|':
            if (pos < n && src[pos] == '|') {
                pos++;
                t.type = TT_OR;
            } else {
                die_at(line, pos - line_start + 1, "unexpected '|' — use '||' for logical or");
            }
            break;
        case '(': t.type=TT_LPAREN;   break;
        case ')': t.type=TT_RPAREN;   break;
        case '{': t.type=TT_LBRACE;   break;
        case '}': t.type=TT_RBRACE;   break;
        case '[': t.type=TT_LBRACKET; break;
        case ']': t.type=TT_RBRACKET; break;
        case ',': t.type=TT_COMMA;    break;
        case '.': t.type=TT_DOT;      break;
        default: die_at(line, pos - line_start, "unexpected character '%c'", c);
        }
#undef TWO
        tl_push(tl, t);
    }
    Token eof = {TT_EOF, NULL, 0, 0, line, pos - line_start + 1, 0};
    tl_push(tl, eof);
    return tl;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  AST nodes                                                                   */
/* ─────────────────────────────────────────────────────────────────────────── */

typedef enum {
    N_NUM, N_STR, N_BOOL, N_NONE,
    N_IDENT, N_BINOP, N_UNARY, N_ASSIGN,
    N_DOT, N_CALL, N_SLICE,
    N_FUNC, N_FORLOOP, N_CLASS,
    N_MAP_LIT,      /* map or list literal */
    N_BLOCK,
    N_BRANCH,
    N_CATCH,
    N_VARDECL,
    N_EXPRSTMT,
    N_IMPORT,
    N_BREAK, N_NEXT,
    N_WHERE,   /* where "dir" — adds dir to import search path */
    N_PROG,
} NodeType;

struct NodeList { Node **nodes; int len; int cap; };

static void nl_push(NodeList *nl, Node *n) {
    if (nl->len + 1 >= nl->cap) {
        nl->cap = nl->cap ? nl->cap*2 : 8;
        nl->nodes = xrealloc(nl->nodes, nl->cap * sizeof(Node*));
    }
    nl->nodes[nl->len++] = n;
}

/* string list */
typedef struct { char **s; int len; int cap; } StrList;
static void sl_push(StrList *sl, char *s) {
    if (sl->len + 1 >= sl->cap) {
        sl->cap = sl->cap ? sl->cap*2 : 4;
        sl->s = xrealloc(sl->s, sl->cap * sizeof(char*));
    }
    sl->s[sl->len++] = s;
}

typedef struct MapItem { Node *key; Node *val; } MapItem;
typedef struct { MapItem *items; int len; int cap; } MapItemList;
static void mil_push(MapItemList *ml, Node *k, Node *v) {
    if (ml->len + 1 >= ml->cap) {
        ml->cap = ml->cap ? ml->cap*2 : 8;
        ml->items = xrealloc(ml->items, ml->cap * sizeof(MapItem));
    }
    ml->items[ml->len].key = k;
    ml->items[ml->len].val = v;
    ml->len++;
}

struct Node {
    NodeType type;
    int      line;
    union {
        double  nval;           /* N_NUM */
        char   *sval;           /* N_STR, N_IDENT, N_IMPORT, N_BINOP(op),
                                    N_UNARY(op), N_ASSIGN(op) */
        int     bval;           /* N_BOOL */

        struct { /* N_BINOP, N_UNARY */
            char  *op;
            Node  *left;
            Node  *right;       /* NULL for unary */
        } binop;

        struct { /* N_ASSIGN */
            char  *op;          /* "=", "+=", etc. */
            Node  *target;
            Node  *value;
        } assign;

        struct { /* N_DOT */
            Node *obj;
            char *attr;
        } dot;

        struct { /* N_IMPORT */
            char *path;   /* module path, e.g. "lru_cache" or "samples/graph" */
            char *alias;  /* optional alias, NULL if none */
        } import_node;

        struct { /* N_CALL */
            Node    *callee;
            NodeList args;
            int      bracket;   /* 1 if written with [] */
        } call;

        struct { /* N_SLICE */
            Node *col;
            Node *start; /* may be NULL */
            Node *stop;  /* may be NULL */
        } slice;

        struct { /* N_FUNC, N_CLASS */
            StrList  params;
            NodeList branches;  /* list of N_BRANCH nodes */
            int      is_loop;   /* N_FUNC only */
        } func;

        struct { /* N_FORLOOP */
            char    *iter_var;
            Node    *start;     /* may be NULL */
            Node    *stop;
            NodeList branches;
        } forloop;

        struct { /* N_MAP_LIT */
            MapItemList items;  /* key=NULL means list */
        } map_lit;

        struct { /* N_BLOCK */
            NodeList stmts;
        } block;

        struct { /* N_BRANCH */
            Node *cond;         /* NULL = unconditional */
            Node *body;
            int   is_ret;       /* 1 = :: */
            int   is_throw;     /* 1 = !! */
        } branch;

        struct { /* N_CATCH */
            Node *expr;     /* expression that may raise SIG_ERROR */
            Node *handler;  /* callable: called with error value on error */
        } catch_node;

        struct { /* N_VARDECL */
            char *name;
            Node *value;
        } vardecl;

        struct { /* N_PROG */
            NodeList stmts;
        } prog;
    };
};

static Node *make_node(NodeType t, int line) {
    Node *n = xmalloc(sizeof(Node));
    memset(n, 0, sizeof(*n));
    n->type = t;
    n->line = line;
    return n;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Parser                                                                      */
/* ─────────────────────────────────────────────────────────────────────────── */

typedef struct {
    TokenList *tl;
    int        pos;
} Parser;

static Token *p_peek(Parser *p, int off) {
    int i = p->pos + off;
    if (i >= p->tl->len) return &p->tl->toks[p->tl->len-1];
    return &p->tl->toks[i];
}
static Token *p_adv(Parser *p) {
    Token *t = &p->tl->toks[p->pos];
    if (t->type != TT_EOF) p->pos++;
    return t;
}
static int p_check(Parser *p, TT t) { return p_peek(p,0)->type == t; }
static Token *p_expect(Parser *p, TT t, const char *msg) {
    if (!p_check(p,t)) {
        Token *cur = p_peek(p,0);
        const char *hint = parse_hint_for_token((int)t, (int)cur->type);
        /* print parse error with source context */
        char errmsg[512];
        snprintf(errmsg, sizeof(errmsg), "%s; expected %s but got %s",
                 msg, token_type_name((int)t), token_type_name((int)cur->type));
        if (g_repl_active) {
            if (hint)
                snprintf(g_repl_errmsg, sizeof(g_repl_errmsg), "ParseError: %s\n%s", errmsg, hint);
            else
                snprintf(g_repl_errmsg, sizeof(g_repl_errmsg), "ParseError: %s", errmsg);
            longjmp(g_repl_jmp, 1);
        }
        fprintf(stderr, "\033[31m[ParseError]\033[0m %s\n", errmsg);
        if (g_current_source_name && cur->line > 0)
            fprintf(stderr, "  --> %s:%d:%d\n", g_current_source_name, cur->line, cur->col);
        if (cur->line > 0) print_source_context(stderr, cur->line, cur->col);
        if (hint) fprintf(stderr, "%s\n", hint);
        exit(1);
    }
    return p_adv(p);
}
static Token *p_match(Parser *p, TT t) {
    if (p_check(p,t)) return p_adv(p);
    return NULL;
}

/* Forward declarations */
static Node *parse_expr(Parser *p);
static Node *parse_stmt(Parser *p);
static Node *parse_branch(Parser *p);
static void  parse_func_body(Parser *p, NodeList *out);


/* Parse a module path: bare ident, ident/ident/..., /abs/path, or "quoted/path" */
static char *parse_module_path(Parser *p) {
    char buf[4096]; buf[0] = 0;
    if (p_check(p, TT_STRING)) {
        /* "quoted/path" */
        strncpy(buf, p_adv(p)->sval, sizeof(buf)-1);
    } else if (p_check(p, TT_SLASH)) {
        /* /absolute/path — starts with slash */
        strncat(buf, "/", sizeof(buf)-1);
        p_adv(p);  /* consume leading / */
        while (p_check(p, TT_IDENT)) {
            strncat(buf, p_adv(p)->sval, sizeof(buf)-strlen(buf)-1);
            if (!p_check(p, TT_SLASH)) break;
            p_adv(p);
            strncat(buf, "/", sizeof(buf)-strlen(buf)-1);
        }
    } else {
        /* bare ident, optionally followed by /ident/... */
        char *seg = p_expect(p, TT_IDENT, "expected module name")->sval;
        strncpy(buf, seg, sizeof(buf)-1);
        while (p_check(p, TT_SLASH)) {
            p_adv(p);
            if (!p_check(p, TT_IDENT)) break;
            strncat(buf, "/",  sizeof(buf)-strlen(buf)-1);
            strncat(buf, p_adv(p)->sval, sizeof(buf)-strlen(buf)-1);
        }
    }
    return xstrdup(buf);
}

static Node *parse_prog(Parser *p) {
    Node *n = make_node(N_PROG, 1);
    while (!p_check(p, TT_EOF)) {
        nl_push(&n->prog.stmts, parse_stmt(p));
        p_match(p, TT_COMMA);
    }
    return n;
}

static Node *parse_stmt(Parser *p) {
    int line = p_peek(p,0)->line;
    /* var decl */
    if (p_check(p, TT_VAR)) {
        p_adv(p);
        char *name = xstrdup(p_expect(p,TT_IDENT,"expected ident after var")->sval);
        p_expect(p, TT_EQ, "expected '=' in var decl");
        Node *val = parse_expr(p);
        Node *n = make_node(N_VARDECL, line);
        n->vardecl.name = name;
        n->vardecl.value = val;
        return n;
    }
    /* how <path> [as <alias>] */
    if (p_check(p, TT_HOW)) {
        p_adv(p);
        char *mod_path = parse_module_path(p);
        char *alias = NULL;
        if (p_check(p, TT_AS)) {
            p_adv(p);
            alias = xstrdup(p_expect(p, TT_IDENT, "expected name after 'as'")->sval);
        }
        Node *n = make_node(N_IMPORT, line);
        n->import_node.path  = mod_path;
        n->import_node.alias = alias;
        return n;
    }
    /* where <path> */
    if (p_check(p, TT_WHERE)) {
        p_adv(p);
        Node *n = make_node(N_WHERE, line);
        n->sval = parse_module_path(p);
        return n;
    }
    /* everything else — delegate to branch parser */
    return parse_branch(p);
}

/* parse_branch parses:   cond :: body  |  cond : body  |  :: body  |  bare_expr
   Also handles var_decl and how_import when inside function bodies. */
static Node *parse_branch(Parser *p) {
    int line = p_peek(p,0)->line;

    /* var decl inside function body: var x = expr */
    if (p_check(p, TT_VAR)) {
        p_adv(p);
        char *name = xstrdup(p_expect(p,TT_IDENT,"expected ident after var")->sval);
        p_expect(p, TT_EQ, "expected '=' in var decl");
        Node *val = parse_expr(p);
        Node *n = make_node(N_VARDECL, line);
        n->vardecl.name  = name;
        n->vardecl.value = val;
        return n;
    }

    /* how <path> [as <alias>] inside function body */
    if (p_check(p, TT_HOW)) {
        p_adv(p);
        char *mod_path = parse_module_path(p);
        char *alias = NULL;
        if (p_check(p, TT_AS)) {
            p_adv(p);
            alias = xstrdup(p_expect(p, TT_IDENT, "expected name after 'as'")->sval);
        }
        Node *n = make_node(N_IMPORT, line);
        n->import_node.path  = mod_path;
        n->import_node.alias = alias;
        return n;
    }
    if (p_check(p, TT_WHERE)) {
        p_adv(p);
        Node *n = make_node(N_WHERE, line);
        n->sval = parse_module_path(p);
        return n;
    }

    /* break */
    if (p_check(p, TT_BREAK)) {
        p_adv(p);
        Node *n = make_node(N_BRANCH, line);
        n->branch.cond = NULL;
        n->branch.body = make_node(N_BREAK, line);
        n->branch.is_ret = 1;
        return n;
    }
    /* next — skip rest of iteration, advance to next */
    if (p_check(p, TT_CONTINUE)) {
        p_adv(p);
        Node *n = make_node(N_BRANCH, line);
        n->branch.cond = NULL;
        n->branch.body = make_node(N_NEXT, line);
        n->branch.is_ret = 1;
        return n;
    }

    /* :: body — unconditional return */
    if (p_check(p, TT_DCOLON)) {
        p_adv(p);
        Node *body = parse_expr(p);
        Node *n = make_node(N_BRANCH, line);
        n->branch.cond   = NULL;
        n->branch.body   = body;
        n->branch.is_ret = 1;
        return n;
    }

    /* !! body — unconditional throw */
    if (p_check(p, TT_DBANG)) {
        p_adv(p);
        Node *body = parse_expr(p);
        Node *n = make_node(N_BRANCH, line);
        n->branch.cond     = NULL;
        n->branch.body     = body;
        n->branch.is_throw = 1;
        return n;
    }

    /* cond :: body  or  cond : body  or  bare expr */
    Node *cond = parse_expr(p);

    /* For-range and loop auto-calls are always statements, never conditions.
       (i=0:n){ body }  and  (:)={ body }  followed by  :: x  would otherwise
       be misread as "if loop-result-truthy :: return x". Wrap immediately.
       Detection:
         - N_FORLOOP: always a statement
         - N_CALL where callee is N_FUNC with is_loop=1 (the (:)={} pattern) */
    {
        int is_loop_stmt = 0;
        if (cond->type == N_CALL && cond->call.callee &&
                   cond->call.callee->type == N_FORLOOP) {
            /* (i=0:n){ }() — explicit for-range call is a loop statement */
            is_loop_stmt = 1;
        } else if (cond->type == N_CALL &&
                   cond->call.callee &&
                   cond->call.callee->type == N_FUNC &&
                   cond->call.callee->func.is_loop) {
            is_loop_stmt = 1;
        }
        if (is_loop_stmt) {
            Node *n = make_node(N_BRANCH, line);
            n->branch.cond   = NULL;
            n->branch.body   = cond;
            n->branch.is_ret = 0;
            return n;
        }
    }

    if (p_check(p, TT_DCOLON)) {
        p_adv(p);
        Node *body = parse_expr(p);
        Node *n = make_node(N_BRANCH, line);
        n->branch.cond = cond; n->branch.body = body; n->branch.is_ret = 1;
        return n;
    }
    if (p_check(p, TT_DBANG) && p_peek(p,0)->line == cond->line) {
        p_adv(p);
        Node *body = parse_expr(p);
        Node *n = make_node(N_BRANCH, line);
        n->branch.cond = cond; n->branch.body = body; n->branch.is_throw = 1;
        return n;
    }
    if (p_check(p, TT_COLON)) {
        p_adv(p);
        Node *body;
        if (p_check(p, TT_LBRACE)) {
            /* block body */
            body = make_node(N_BLOCK, line);
            p_adv(p); /* { */
            while (!p_check(p, TT_RBRACE) && !p_check(p, TT_EOF)) {
                nl_push(&body->block.stmts, parse_stmt(p));
                p_match(p, TT_COMMA);
            }
            p_expect(p, TT_RBRACE, "expected '}'");
        } else {
            body = parse_expr(p);
        }
        Node *n = make_node(N_BRANCH, line);
        n->branch.cond = cond; n->branch.body = body; n->branch.is_ret = 0;
        return n;
    }

    /* bare expression — wrap as unconditional side-effect branch */
    Node *n = make_node(N_BRANCH, line);
    n->branch.cond   = NULL;
    n->branch.body   = cond;
    n->branch.is_ret = 0;
    return n;
}

/* ── Expression parsing (precedence climbing) ──────────────────────────────── */

static Node *parse_assign(Parser *p);
static Node *parse_catch(Parser *p);
static Node *parse_or(Parser *p);
static Node *parse_and(Parser *p);
static Node *parse_eq(Parser *p);
static Node *parse_cmp(Parser *p);
static Node *parse_add(Parser *p);
static Node *parse_mul(Parser *p);
static Node *parse_unary(Parser *p);
static Node *parse_call(Parser *p);
static Node *parse_atom(Parser *p);

static Node *parse_expr(Parser *p) { return parse_assign(p); }

static Node *parse_assign(Parser *p) {
    int line = p_peek(p,0)->line;
    Node *left = parse_catch(p);
    TT t = p_peek(p,0)->type;
    if (t==TT_EQ||t==TT_PLUSEQ||t==TT_MINUSEQ||t==TT_STAREQ||t==TT_SLASHEQ||t==TT_PERCENTEQ) {
        p_adv(p);
        const char *os = "=";
        if      (t==TT_PLUSEQ)    os="+=";
        else if (t==TT_MINUSEQ)   os="-=";
        else if (t==TT_STAREQ)    os="*=";
        else if (t==TT_SLASHEQ)   os="/=";
        else if (t==TT_PERCENTEQ) os="%=";
        Node *right = parse_assign(p);
        Node *n = make_node(N_ASSIGN, line);
        n->assign.op     = xstrdup(os);
        n->assign.target = left;
        n->assign.value  = right;
        return n;
    }
    return left;
}


/* catch: expr catch handler — left-associative, lower than or, higher than assign */
static Node *parse_catch(Parser *p) {
    int line = p_peek(p,0)->line;
    Node *left = parse_or(p);
    while (p_check(p, TT_CATCH)) {
        p_adv(p);
        Node *handler = parse_or(p);
        Node *n = make_node(N_CATCH, line);
        n->catch_node.expr    = left;
        n->catch_node.handler = handler;
        left = n;
    }
    return left;
}

/* manual OR */
static Node *parse_or(Parser *p) {
    int line = p_peek(p,0)->line;
    Node *l = parse_and(p);
    while (p_check(p,TT_OR)) {
        p_adv(p);
        Node *r = parse_and(p);
        Node *n = make_node(N_BINOP,line);
        n->binop.op=xstrdup("or"); n->binop.left=l; n->binop.right=r;
        l=n;
    }
    return l;
}
static Node *parse_and(Parser *p) {
    int line = p_peek(p,0)->line;
    Node *l = parse_eq(p);
    while (p_check(p,TT_AND)) {
        p_adv(p);
        Node *r = parse_eq(p);
        Node *n = make_node(N_BINOP,line);
        n->binop.op=xstrdup("and"); n->binop.left=l; n->binop.right=r;
        l=n;
    }
    return l;
}
static Node *parse_eq(Parser *p) {
    int line = p_peek(p,0)->line;
    Node *l = parse_cmp(p);
    for(;;){
        const char *op=NULL;
        if(p_check(p,TT_EQEQ)) op="==";
        else if(p_check(p,TT_NEQ)) op="!=";
        else break;
        p_adv(p);
        Node *r=parse_cmp(p);
        Node *n=make_node(N_BINOP,line);
        n->binop.op=xstrdup(op); n->binop.left=l; n->binop.right=r;
        l=n;
    }
    return l;
}
static Node *parse_cmp(Parser *p) {
    int line = p_peek(p,0)->line;
    Node *l = parse_add(p);
    for(;;){
        const char *op=NULL;
        if(p_check(p,TT_LT))   op="<";
        else if(p_check(p,TT_GT))  op=">";
        else if(p_check(p,TT_LTE)) op="<=";
        else if(p_check(p,TT_GTE)) op=">=";
        else break;
        p_adv(p);
        Node *r=parse_add(p);
        Node *n=make_node(N_BINOP,line);
        n->binop.op=xstrdup(op); n->binop.left=l; n->binop.right=r;
        l=n;
    }
    return l;
}
static Node *parse_add(Parser *p) {
    int line = p_peek(p,0)->line;
    Node *l = parse_mul(p);
    for(;;){
        const char *op=NULL;
        if(p_check(p,TT_PLUS))  op="+";
        else if(p_check(p,TT_MINUS)) op="-";
        else break;
        p_adv(p);
        Node *r=parse_mul(p);
        Node *n=make_node(N_BINOP,line);
        n->binop.op=xstrdup(op); n->binop.left=l; n->binop.right=r;
        l=n;
    }
    return l;
}
static Node *parse_mul(Parser *p) {
    int line = p_peek(p,0)->line;
    Node *l = parse_unary(p);
    for(;;){
        const char *op=NULL;
        if(p_check(p,TT_STAR))    op="*";
        else if(p_check(p,TT_SLASH))   op="/";
        else if(p_check(p,TT_PERCENT)) op="%";
        else break;
        p_adv(p);
        Node *r=parse_unary(p);
        Node *n=make_node(N_BINOP,line);
        n->binop.op=xstrdup(op); n->binop.left=l; n->binop.right=r;
        l=n;
    }
    return l;
}
static Node *parse_unary(Parser *p) {
    int line = p_peek(p,0)->line;
    if (p_check(p,TT_MINUS)) {
        p_adv(p);
        Node *e = parse_unary(p);
        Node *n = make_node(N_UNARY,line);
        n->binop.op = xstrdup("-"); n->binop.left = e;
        return n;
    }
    if (p_check(p,TT_NOT)) {
        p_adv(p);
        Node *e = parse_unary(p);
        Node *n = make_node(N_UNARY,line);
        n->binop.op = xstrdup("not"); n->binop.left = e;
        return n;
    }
    return parse_call(p);
}

/* parse call/dot/slice postfix, plus bracket-call and bracket class-call */
static Node *parse_call(Parser *p) {
    int line = p_peek(p,0)->line;
    Node *n = parse_atom(p);
    /* remember the line of the callee for same-line check */
    int last_line = line;
    /* if it's a func/class literal, set last_line to the line of the
       closing brace (p->pos-1), NOT the next token (p->pos) */
    if (n->type == N_FUNC || n->type == N_CLASS || n->type == N_FORLOOP) {
        int prev = p->pos - 1;
        if (prev >= 0 && prev < p->tl->len)
            last_line = p->tl->toks[prev].line;
        else
            last_line = line;
    }

    for (;;) {
        int cur_line = p_peek(p,0)->line;
        /* For-range: (i=0:n){ }() — () always triggers a call regardless of line.
           No ambiguity: a for-range value can only be invoked with (). */
        int force_call = (n->type == N_FORLOOP && p_check(p,TT_LPAREN));
        /* ( args ) — function call or slice, same line */
        if (p_check(p,TT_LPAREN) && (force_call || cur_line == last_line)) {
            p_adv(p);
            /* check for slice: (: or (expr: */
            int is_slice = 0;
            Node *slice_start = NULL, *slice_stop = NULL;
            if (p_check(p, TT_COLON)) {
                /* (:stop) or (:) */
                is_slice = 1;
                p_adv(p);
                if (!p_check(p, TT_RPAREN))
                    slice_stop = parse_expr(p);
            } else if (!p_check(p,TT_RPAREN)) {
                /* parse first arg at catch-level (not assign-level) to prevent
                   f(x=5) from silently assigning to outer x, but allow catch */
                Node *first = parse_catch(p);
                if (p_check(p, TT_COLON)) {
                    is_slice = 1;
                    slice_start = first;
                    p_adv(p); /* consume : */
                    if (!p_check(p, TT_RPAREN))
                        slice_stop = parse_expr(p);
                } else if (is_slice == 0) {
                    /* regular call — args parsed at catch-level to allow catch
                       but not assign, so f(x=5) doesn't silently assign to x */
                    Node *c = make_node(N_CALL, line);
                    c->call.callee  = n;
                    c->call.bracket = 0;
                    nl_push(&c->call.args, first);
                    while (p_match(p,TT_COMMA) && !p_check(p,TT_RPAREN))
                        nl_push(&c->call.args, parse_catch(p));
                    p_expect(p,TT_RPAREN,"expected ')'");
                    last_line = cur_line;
                    n = c;
                    continue;
                }
            }
            if (is_slice) {
                p_expect(p,TT_RPAREN,"expected ')'");
                Node *s = make_node(N_SLICE, line);
                s->slice.col   = n;
                s->slice.start = slice_start;
                s->slice.stop  = slice_stop;
                last_line = cur_line;
                n = s;
                continue;
            }
            /* empty call () */
            p_expect(p,TT_RPAREN,"expected ')'");
            Node *c = make_node(N_CALL, line);
            c->call.callee  = n;
            c->call.bracket = 0;
            last_line = cur_line;
            n = c;
            continue;
        }
        /* [ args ] — bracket call (class instantiation) */
        if (p_check(p,TT_LBRACKET) && cur_line == last_line) {
            p_adv(p);
            Node *c = make_node(N_CALL, line);
            c->call.callee  = n;
            c->call.bracket = 1;
            if (!p_check(p,TT_RBRACKET)) {
                nl_push(&c->call.args, parse_or(p));
                while (p_match(p,TT_COMMA) && !p_check(p,TT_RBRACKET))
                    nl_push(&c->call.args, parse_or(p));
            }
            p_expect(p,TT_RBRACKET,"expected ']'");
            last_line = cur_line;
            n = c;
            continue;
        }
        /* . attr */
        if (p_check(p,TT_DOT)) {
            p_adv(p);
            char *attr = xstrdup(p_expect(p,TT_IDENT,"expected field name after '.'")->sval);
            Node *d = make_node(N_DOT, line);
            d->dot.obj = n; d->dot.attr = attr;
            last_line = p_peek(p,0)->line;
            n = d;
            continue;
        }
        break;
    }
    return n;
}

static void parse_func_body(Parser *p, NodeList *out) {
    p_expect(p,TT_LBRACE,"expected '{'");
    while (!p_check(p,TT_RBRACE) && !p_check(p,TT_EOF)) {
        nl_push(out, parse_branch(p));
        p_match(p,TT_COMMA);
    }
    p_expect(p,TT_RBRACE,"expected '}'");
}


/* ── String interpolation helper ─────────────────────────────────────────────
 * Chains two nodes with a "+" binop, returning the new root.
 */
static Node *interp_chain(Node *left, Node *right, int line) {
    Node *b = make_node(N_BINOP, line);
    b->binop.op    = xstrdup("+");
    b->binop.left  = left;
    b->binop.right = right;
    return b;
}

/* parse_interp_string: expand "hello {expr}" into AST at parse time.
 * "hello {name}, age {age+1}" → "hello " + str(name) + ", age " + str(age+1)
 * Use {{ and }} to get literal braces. Single-quoted strings skip interpolation.
 */
static Node *parse_interp_string(const char *raw, int line) {
    /* quick check: any unescaped { ? */
    int has_interp = 0;
    for (const char *q = raw; *q; q++)
        if (*q == '{' && *(q+1) != '{') { has_interp = 1; break; }
    if (!has_interp) {
        Node *n = make_node(N_STR, line);
        n->sval = xstrdup(raw);
        return n;
    }

    Node *result = NULL;
    const char *p = raw;
    Buf seg = {0};

    while (*p) {
        if (*p == '{' && *(p+1) == '{') {
            buf_push(&seg, '{'); p += 2;
        } else if (*p == '}' && *(p+1) == '}') {
            buf_push(&seg, '}'); p += 2;
        } else if (*p == '{') {
            /* flush literal segment before { */
            char *lit = buf_done(&seg);
            if (lit && lit[0]) {
                Node *s = make_node(N_STR, line);
                s->sval = lit;
                result = result ? interp_chain(result, s, line) : s;
            } else { free(lit); }
            seg = (Buf){0};
            p++; /* skip { */

            /* collect expression until matching } */
            Buf expr_src = {0};
            int depth = 0;
            while (*p) {
                if (*p == '{') depth++;
                else if (*p == '}') { if (depth == 0) break; depth--; }
                buf_push(&expr_src, *p++);
            }
            if (*p == '}') p++; /* skip closing } */

            char *esrc = buf_done(&expr_src);
            if (esrc && esrc[0]) {
                TokenList *etl = lex(esrc);
                Parser ep = {etl, 0};
                Node *expr = parse_expr(&ep);

                /* wrap in str(...) */
                Node *str_call  = make_node(N_CALL, line);
                Node *str_ident = make_node(N_IDENT, line);
                str_ident->sval = xstrdup("str");
                str_call->call.callee  = str_ident;
                str_call->call.bracket = 0;
                nl_push(&str_call->call.args, expr);

                result = result ? interp_chain(result, str_call, line) : str_call;
            }
            free(esrc);
        } else {
            buf_push(&seg, *p++);
        }
    }
    /* flush trailing literal */
    char *lit = buf_done(&seg);
    if (lit && lit[0]) {
        Node *s = make_node(N_STR, line);
        s->sval = lit;
        result = result ? interp_chain(result, s, line) : s;
    } else { free(lit); }

    if (!result) {
        Node *n = make_node(N_STR, line); n->sval = xstrdup(""); return n;
    }
    return result;
}

static Node *parse_atom(Parser *p) {
    int line = p_peek(p,0)->line;
    TT t = p_peek(p,0)->type;

    /* number */
    if (t == TT_NUMBER) {
        Node *n = make_node(N_NUM,line);
        n->nval = p_adv(p)->nval;
        return n;
    }
    /* string — double-quoted supports interpolation: "hello {expr}"
       single-quoted is always literal: 'no {interpolation}' */
    if (t == TT_STRING) {
        Token *strtok = p_adv(p);
        if (strtok->raw) {
            Node *n = make_node(N_STR, line);
            n->sval = xstrdup(strtok->sval);
            return n;
        }
        return parse_interp_string(strtok->sval, line);
    }
    /* bool */
    if (t == TT_BOOL) {
        Node *n = make_node(N_BOOL,line);
        n->bval = p_adv(p)->bval;
        return n;
    }
    /* none */
    if (t == TT_NONE) {
        p_adv(p);
        return make_node(N_NONE,line);
    }
    /* ident */
    if (t == TT_IDENT) {
        Node *n = make_node(N_IDENT,line);
        n->sval = xstrdup(p_adv(p)->sval);
        return n;
    }
    /* break */
    if (t == TT_BREAK) {
        p_adv(p);
        return make_node(N_BREAK,line);
    }
    if (t == TT_CONTINUE) {
        p_adv(p);
        return make_node(N_NEXT,line);
    }

    /* [ params ] { body } — class expression */
    if (t == TT_LBRACKET) {
        p_adv(p); /* [ */
        Node *n = make_node(N_CLASS,line);
        while (!p_check(p,TT_RBRACKET) && !p_check(p,TT_EOF)) {
            sl_push(&n->func.params,
                    xstrdup(p_expect(p,TT_IDENT,"expected param")->sval));
            p_match(p,TT_COMMA);
        }
        p_expect(p,TT_RBRACKET,"expected ']'");
        parse_func_body(p, &n->func.branches);
        return n;
    }

    /* { ... } — map literal, list literal, or anonymous branch-function */
    if (t == TT_LBRACE) {
        p_adv(p); /* { */
        /* empty {} → empty map */
        if (p_check(p,TT_RBRACE)) {
            p_adv(p);
            Node *n = make_node(N_MAP_LIT,line);
            return n;
        }
        /* starts with :: → branch function */
        if (p_check(p,TT_DCOLON)) {
            Node *fn = make_node(N_FUNC,line);
            while (!p_check(p,TT_RBRACE) && !p_check(p,TT_EOF)) {
                nl_push(&fn->func.branches, parse_branch(p));
                p_match(p,TT_COMMA);
            }
            p_expect(p,TT_RBRACE,"expected '}'");
            return fn;
        }
        /* Lookahead: is this a map/list literal?
         * Map:  { str_or_ident : expr, ... }   first token is STR/IDENT then COLON (not DCOLON)
         *        AND the key isn't followed by LBRACE (which would be a branch body)
         * List: { expr, expr, ... }             no colons at depth 0
         * Func: { cond :: body, ... }           has DCOLON
         *        OR { cond : { body }, ... }    condition with block body
         * Heuristic: scan ahead to see if first item is "key:" style
         */
        int lookslike_map = 0, lookslike_list = 0, lookslike_func = 0;
        {
            /* Map: first item is STR or IDENT immediately followed by COLON (not DCOLON)
             * List: no colons at depth 0 before a comma or }
             * Func: has DCOLON, OR condition is complex expression, OR body is { block }
             */
            TT t0 = p->tl->toks[p->pos].type;
            TT t1 = (p->pos+1 < p->tl->len) ? p->tl->toks[p->pos+1].type : TT_EOF;
            if (t1 == TT_DCOLON) {
                lookslike_func = 1;  /* ident :: body → function */
            } else if (t0 == TT_STRING && t1 == TT_COLON) {
                /* String key: always a map literal regardless of value type */
                lookslike_map = 1;
            } else if (t0 == TT_IDENT && t1 == TT_COLON) {
                /* Ident key followed by colon: could be map field or branch cond: body.
                 * If value after : is a block {, it's a branch function.
                 * If value is a simple expr (num/str/bool/ident/paren), it's a map. */
                TT t2 = (p->pos+2 < p->tl->len) ? p->tl->toks[p->pos+2].type : TT_EOF;
                if (t2 == TT_LBRACE) lookslike_func = 1;   /* cond: { block } */
                else lookslike_map = 1;                      /* field: value */
            } else if (t0 == TT_DCOLON) {
                lookslike_func = 1;  /* :: expr → function */
            } else if (t0 == TT_RBRACE) {
                lookslike_list = 0; /* empty already handled */
            } else {
                /* No immediate colon after first token → scan for colons at depth 0 */
                int scan = p->pos; int depth=0;
                int found_colon=0, found_dcolon=0;
                while (scan < p->tl->len) {
                    TT st = p->tl->toks[scan].type;
                    if(st==TT_LBRACE||st==TT_LPAREN||st==TT_LBRACKET){depth++;scan++;continue;}
                    if((st==TT_RBRACE||st==TT_RPAREN||st==TT_RBRACKET)&&depth>0){depth--;scan++;continue;}
                    if(st==TT_EOF||(st==TT_RBRACE&&depth==0)) break;
                    if(depth==0&&st==TT_DCOLON){found_dcolon=1;break;}
                    if(depth==0&&st==TT_COLON){found_colon=1;break;}
                    if(depth==0&&st==TT_COMMA) break;
                    scan++;
                }
                if(found_dcolon) {
                    lookslike_func=1;   /* contains :: → branch function */
                } else if(found_colon) {
                    lookslike_func=1;   /* contains : → branch function (cond: body) */
                } else {
                    lookslike_list=1;
                }
            }
        }
        if (lookslike_list) {
            /* List literal: { expr, expr, ... } */
            Node *n = make_node(N_MAP_LIT,line);
            if (!p_check(p,TT_RBRACE)) {
                Node *v = parse_expr(p);
                mil_push(&n->map_lit.items, NULL, v);
                while (p_match(p,TT_COMMA) && !p_check(p,TT_RBRACE))
                    mil_push(&n->map_lit.items, NULL, parse_expr(p));
            }
            p_match(p,TT_COMMA);
            p_expect(p,TT_RBRACE,"expected '}'");
            return n;
        }
        if (lookslike_map) {
            /* Map literal: { key: val, ... } */
            Node *n = make_node(N_MAP_LIT,line);
            while (!p_check(p,TT_RBRACE) && !p_check(p,TT_EOF)) {
                Node *k = parse_expr(p);
                p_expect(p,TT_COLON,"expected ':' in map literal");
                Node *v = parse_expr(p);
                mil_push(&n->map_lit.items, k, v);
                p_match(p,TT_COMMA);
            }
            p_expect(p,TT_RBRACE,"expected '}'");
            return n;
        }
        /* Branch function */
        Node *fn = make_node(N_FUNC,line);
        while (!p_check(p,TT_RBRACE) && !p_check(p,TT_EOF)) {
            nl_push(&fn->func.branches, parse_branch(p));
            p_match(p,TT_COMMA);
        }
        p_match(p,TT_COMMA);
        p_expect(p,TT_RBRACE,"expected '}'");
        return fn;
    }

    /* ( ... ) — function, loop, for-range, or grouped expr */
    if (t == TT_LPAREN) {
        p_adv(p); /* ( */

        /* (:) — unbounded loop: (:){ body } or (:)={ body } (auto-call) */
        if (p_check(p,TT_COLON) && p_peek(p,1)->type == TT_RPAREN) {
            p_adv(p); /* : */
            p_adv(p); /* ) */
            int auto_call = p_match(p,TT_EQ) != NULL;
            Node *fn = make_node(N_FUNC,line);
            fn->func.is_loop = 1;
            parse_func_body(p,&fn->func.branches);
            if (auto_call) {
                /* wrap in Call(fn, []) */
                Node *c = make_node(N_CALL,line);
                c->call.callee  = fn;
                c->call.bracket = 0;
                return c;
            }
            return fn;
        }

        /* lookahead for for-range: new syntax (var=start:stop){ }
           Pattern inside ( ): IDENT EQ expr COLON expr then RPAREN LBRACE */
        {
            int is_forrange = 0;
            /* must start with IDENT then EQ at the current position */
            if (p_check(p, TT_IDENT) && p_peek(p,1)->type == TT_EQ) {
                /* scan forward: skip past the EQ, find a COLON at depth 0
                   before RPAREN, then RPAREN followed by LBRACE */
                int scan = p->pos + 2;  /* skip IDENT and EQ */
                int depth = 0;
                int found_colon = 0, found_rparen = 0;
                while (scan < p->tl->len) {
                    TT st = p->tl->toks[scan].type;
                    if (st==TT_EOF) break;
                    /* track nesting depth; LBRACE only counts before we find RPAREN */
                    if (!found_rparen && (st==TT_LPAREN||st==TT_LBRACKET||st==TT_LBRACE)) { depth++; scan++; continue; }
                    if (!found_rparen && (st==TT_RPAREN||st==TT_RBRACKET||st==TT_RBRACE) && depth>0) { depth--; scan++; continue; }
                    if (depth==0) {
                        if (!found_colon && st==TT_COLON) { found_colon=1; scan++; continue; }
                        if (found_colon && !found_rparen && st==TT_RPAREN) { found_rparen=1; scan++; continue; }
                        if (found_rparen && st==TT_LBRACE) { is_forrange=1; break; }
                        if (found_rparen) break; /* something other than { after ) */
                        if (!found_colon && st==TT_RPAREN) break; /* no colon — not a range */
                    }
                    scan++;
                }
            }
            if (is_forrange) {
                /* consume: IDENT EQ start COLON stop RPAREN */
                char *ivar = xstrdup(p_expect(p,TT_IDENT,"expected loop var")->sval);
                p_expect(p,TT_EQ,"expected '='");
                Node *start = NULL;
                if (!p_check(p,TT_COLON)) start = parse_expr(p);
                p_expect(p,TT_COLON,"expected ':'");
                Node *stop  = parse_expr(p);
                p_expect(p,TT_RPAREN,"expected ')'");
                Node *n = make_node(N_FORLOOP,line);
                n->forloop.iter_var = ivar;
                n->forloop.start    = start;
                n->forloop.stop     = stop;
                parse_func_body(p,&n->forloop.branches);
                return n;
            }
        }

        /* () or (ident,...) { } — function with zero or more params */
        /* Check if it looks like a param list */
        {
            StrList params; memset(&params,0,sizeof(params));
            /* () { */
            if (p_check(p,TT_RPAREN)) {
                p_adv(p);
                if (p_check(p,TT_LBRACE)) {
                    Node *n = make_node(N_FUNC,line);
                    n->func.params = params;
                    parse_func_body(p,&n->func.branches);
                    return n;
                }
                /* bare () → None */
                Node *n = make_node(N_NONE,line);
                return n;
            }
            /* (ident, ...) { */
            if (p_check(p,TT_IDENT)) {
                int scan = p->pos;
                StrList pl; memset(&pl,0,sizeof(pl));
                while (scan < p->tl->len && p->tl->toks[scan].type==TT_IDENT) {
                    sl_push(&pl, xstrdup(p->tl->toks[scan++].sval));
                    if (scan < p->tl->len && p->tl->toks[scan].type==TT_COMMA) scan++;
                    else break;
                }
                if (scan < p->tl->len && p->tl->toks[scan].type==TT_RPAREN
                    && scan+1 < p->tl->len && p->tl->toks[scan+1].type==TT_LBRACE) {
                    p->pos = scan+1; /* at { */
                    Node *n = make_node(N_FUNC,line);
                    n->func.params = pl;
                    parse_func_body(p,&n->func.branches);
                    return n;
                }
                /* not a function — free param list */
                for (int i=0;i<pl.len;i++) free(pl.s[i]);
                free(pl.s);
            }
        }

        /* grouped expression */
        Node *e = parse_expr(p);
        p_expect(p,TT_RPAREN,"expected ')'");
        return e;
    }

    {
        Token *cur = p_peek(p,0);
        const char *hint = parse_hint_for_token(-1, (int)cur->type);
        if (g_repl_active) {
            snprintf(g_repl_errmsg, sizeof(g_repl_errmsg),
                     "ParseError: unexpected token %s", token_type_name((int)cur->type));
            longjmp(g_repl_jmp, 1);
        }
        fprintf(stderr, "\033[31m[ParseError]\033[0m unexpected token %s\n",
                token_type_name((int)cur->type));
        if (g_current_source_name && cur->line > 0)
            fprintf(stderr, "  --> %s:%d:%d\n", g_current_source_name, cur->line, cur->col);
        if (cur->line > 0) print_source_context(stderr, cur->line, cur->col);
        if (hint) fprintf(stderr, "%s\n", hint);
        exit(1);
    }
    return NULL;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Runtime values                                                              */
/* ─────────────────────────────────────────────────────────────────────────── */

typedef enum {
    VT_NONE, VT_BOOL, VT_NUM, VT_STR,
    VT_LIST, VT_MAP,
    VT_FUNC, VT_CLASS, VT_INSTANCE, VT_MODULE,
    VT_BUILTIN,
} VT;

typedef struct HowList HowList;
typedef struct HowMap  HowMap;
typedef struct HowFunc HowFunc;
typedef struct HowClass HowClass;
typedef struct HowInstance HowInstance;
typedef struct HowModule HowModule;

/* Key-value pair for maps */
typedef struct KV { char *key; Value *val; } KV;

struct HowMap {
    KV     *pairs;
    int     len;
    int     cap;
    int     refcount;
    int     gc_mark;
    struct HowMap *gc_next;
};

struct HowList {
    Value **items;
    int     len;
    int     cap;
    int     refcount;
    int     gc_mark;
    struct HowList *gc_next;
};

struct HowFunc {
    StrList  params;
    NodeList branches;
    Env     *closure;
    int      is_loop;
    /* for-range callable fields */
    int      is_forrange;
    char    *iter_var;
    Node    *fr_start;
    Node    *fr_stop;
    int      refcount;
    int      gc_mark;
    struct HowFunc *gc_next;
};

struct HowClass {
    StrList  params;
    NodeList branches;
    Env     *closure;
    int      refcount;
    int      gc_mark;
    struct HowClass *gc_next;
};

struct HowInstance {
    HowMap  *fields;  /* shared with inst_env */
    Env     *inst_env;
    int      refcount;
    int      gc_mark;
    struct HowInstance *gc_next;
};

struct HowModule {
    char *name;
    Env  *env;
    int   refcount;
    int   gc_mark;
    struct HowModule *gc_next;
};

typedef Value* (*BuiltinFn)(int argc, Value **argv, void *ctx);

struct Value {
    VT  type;
    int refcount;
    int gc_mark;
    struct Value *gc_next;
    union {
        int       bval;
        double    nval;
        char     *sval;
        HowList  *list;
        HowMap   *map;
        HowFunc  *func;
        HowClass *cls;
        HowInstance *inst;
        HowModule   *mod;
        struct { BuiltinFn fn; void *ctx; char *name; } builtin;
    };
};

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Value constructors                                                          */
/* ─────────────────────────────────────────────────────────────────────────── */

static Value *V_NONE_SINGLETON;
static Value *V_TRUE_SINGLETON;
static Value *V_FALSE_SINGLETON;

static Value       *g_all_values = NULL;
static HowMap      *g_all_maps = NULL;
static HowList     *g_all_lists = NULL;
static HowFunc     *g_all_funcs = NULL;
static HowClass    *g_all_classes = NULL;
static HowInstance *g_all_instances = NULL;
static HowModule   *g_all_modules = NULL;
static Env         *g_all_envs = NULL;
static Env         *g_globals = NULL;
static HowMap      *g_module_registry = NULL;  /* name -> module Value, for import caching */

static size_t g_gc_allocations = 0;
static size_t g_gc_collections = 0;
static int g_gc_in_progress = 0;

typedef struct { Value ***slots; int len; int cap; } GcValueRootStack;
typedef struct { Env   ***slots; int len; int cap; } GcEnvRootStack;
static GcValueRootStack g_gc_value_roots = {0};
static GcEnvRootStack g_gc_env_roots = {0};

static void gc_push_value_root(Value **slot) {
    if (g_gc_value_roots.len == g_gc_value_roots.cap) {
        g_gc_value_roots.cap = g_gc_value_roots.cap ? g_gc_value_roots.cap * 2 : 64;
        g_gc_value_roots.slots = xrealloc(g_gc_value_roots.slots, sizeof(Value**) * g_gc_value_roots.cap);
    }
    g_gc_value_roots.slots[g_gc_value_roots.len++] = slot;
}
static void gc_pop_value_root(void) {
    if (g_gc_value_roots.len > 0) g_gc_value_roots.len--;
}
static void gc_push_env_root(Env **slot) {
    if (g_gc_env_roots.len == g_gc_env_roots.cap) {
        g_gc_env_roots.cap = g_gc_env_roots.cap ? g_gc_env_roots.cap * 2 : 32;
        g_gc_env_roots.slots = xrealloc(g_gc_env_roots.slots, sizeof(Env**) * g_gc_env_roots.cap);
    }
    g_gc_env_roots.slots[g_gc_env_roots.len++] = slot;
}
static void gc_pop_env_root(void) {
    if (g_gc_env_roots.len > 0) g_gc_env_roots.len--;
}

#define GC_ROOT_VALUE(v) gc_push_value_root(&(v))
#define GC_UNROOT_VALUE() gc_pop_value_root()
#define GC_ROOT_ENV(e) gc_push_env_root(&(e))
#define GC_UNROOT_ENV() gc_pop_env_root()


static Value *val_new(VT type) {
    Value *v = xmalloc(sizeof(Value));
    memset(v, 0, sizeof(*v));
    v->type = type;
    v->refcount = 1;
    v->gc_mark = 0;
    v->gc_next = g_all_values;
    g_all_values = v;
    g_gc_allocations++;
    return v;
}

static Value *val_none(void)   { V_NONE_SINGLETON->refcount++; return V_NONE_SINGLETON; }
static Value *val_bool(int b)  { Value *v = b ? V_TRUE_SINGLETON : V_FALSE_SINGLETON; v->refcount++; return v; }
static Value *val_num(double d){ Value *v=val_new(VT_NUM); v->nval=d; return v; }
static Value *val_str(const char *s){ Value *v=val_new(VT_STR); v->sval=xstrdup(s); return v; }
static Value *val_str_own(char *s){ Value *v=val_new(VT_STR); v->sval=s; return v; }

static HowMap *map_new(void) {
    HowMap *m = xmalloc(sizeof(*m));
    m->pairs=NULL; m->len=m->cap=0; m->refcount=1;
    m->gc_mark = 0;
    m->gc_next = g_all_maps;
    g_all_maps = m;
    g_gc_allocations++;
    return m;
}

static Value *val_map(HowMap *m) {
    Value *v = val_new(VT_MAP); v->map = m; m->refcount++; return v;
}

static HowList *list_new(void) {
    HowList *l = xmalloc(sizeof(*l));
    l->items=NULL; l->len=l->cap=0; l->refcount=1;
    l->gc_mark = 0;
    l->gc_next = g_all_lists;
    g_all_lists = l;
    g_gc_allocations++;
    return l;
}

static Value *val_list(HowList *l) {
    Value *v = val_new(VT_LIST); v->list = l; l->refcount++; return v;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Reference counting (simplified — no cycle collection)                      */
/* ─────────────────────────────────────────────────────────────────────────── */

static void val_decref(Value *v);

static void map_decref(HowMap *m) {
    /* tracing GC: reclamation happens in gc_collect(), not here */
    if (!m) return;
    m->refcount--;
}


static void list_decref(HowList *l) {
    /* tracing GC: reclamation happens in gc_collect(), not here */
    if (!l) return;
    l->refcount--;
}


/* forward decl */
static void env_decref(Env *e);

static void func_decref(HowFunc *f) {
    /* tracing GC: reclamation happens in gc_collect(), not here */
    if (!f) return;
    f->refcount--;
}


static void cls_decref(HowClass *c) {
    /* tracing GC: reclamation happens in gc_collect(), not here */
    if (!c) return;
    c->refcount--;
}


static void inst_decref(HowInstance *inst) {
    /* tracing GC: reclamation happens in gc_collect(), not here */
    if (!inst) return;
    inst->refcount--;
}


static void mod_decref(HowModule *m) {
    /* tracing GC: reclamation happens in gc_collect(), not here */
    if (!m) return;
    m->refcount--;
}


static void val_decref(Value *v) {
    if (!v) return;
    v->refcount--;
}


static Value *val_incref(Value *v) { if(v) v->refcount++; return v; }

static StrList strlist_clone(StrList src) {
    StrList out = {0};
    out.len = src.len;
    out.cap = src.len;
    if (src.len > 0) {
        out.s = xmalloc(sizeof(char*) * src.len);
        for (int i = 0; i < src.len; i++) out.s[i] = xstrdup(src.s[i]);
    }
    return out;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Environment                                                                 */
/* ─────────────────────────────────────────────────────────────────────────── */

typedef struct EnvEntry { char *key; Value *val; } EnvEntry;

struct Env {
    EnvEntry *entries;
    int       len;
    int       cap;
    Env      *parent;
    int       refcount;
    int       gc_mark;
    struct Env *gc_next;
    /* for InstanceEnv */
    HowInstance *inst;  /* non-NULL if this is an InstanceEnv */
};

static Env *env_new(Env *parent) {
    Env *e = xmalloc(sizeof(*e));
    e->entries=NULL; e->len=e->cap=0;
    e->parent  = parent; if(parent) parent->refcount++;
    e->refcount=1;
    e->gc_mark = 0;
    e->gc_next = g_all_envs;
    g_all_envs = e;
    g_gc_allocations++;
    e->inst=NULL;
    return e;
}

static Env *inst_env_new(HowInstance *inst, Env *parent) {
    Env *e = env_new(parent);
    e->inst = inst;
    return e;
}

static void env_decref(Env *e) {
    /* tracing GC: reclamation happens in gc_collect(), not here */
    if (!e) return;
    e->refcount--;
}


/* set (always in local scope) */
static void env_set(Env *e, const char *key, Value *val) {
    /* check if already in local entries */
    for (int i=0;i<e->len;i++) {
        if (!strcmp(e->entries[i].key, key)) {
            val_decref(e->entries[i].val);
            e->entries[i].val = val_incref(val);
            return;
        }
    }
    /* if InstanceEnv check instance fields */
    if (e->inst) {
        HowMap *fields = e->inst->fields;
        for (int i=0;i<fields->len;i++) {
            if (!strcmp(fields->pairs[i].key, key)) {
                val_decref(fields->pairs[i].val);
                fields->pairs[i].val = val_incref(val);
                return;
            }
        }
    }
    /* add new local entry */
    if (e->len + 1 >= e->cap) {
        e->cap = e->cap ? e->cap*2 : 8;
        e->entries = xrealloc(e->entries, e->cap * sizeof(EnvEntry));
    }
    e->entries[e->len].key = xstrdup(key);
    e->entries[e->len].val = val_incref(val);
    e->len++;
}

/* get — walk up parent chain */
static Value *env_get(Env *e, const char *key) {
    for (Env *cur=e; cur; cur=cur->parent) {
        for (int i=0;i<cur->len;i++)
            if (!strcmp(cur->entries[i].key, key))
                return cur->entries[i].val;
        if (cur->inst) {
            HowMap *fields = cur->inst->fields;
            for (int i=0;i<fields->len;i++)
                if (!strcmp(fields->pairs[i].key, key))
                    return fields->pairs[i].val;
        }
    }
    return NULL;  /* undefined */
}

/* assign — walk up parent chain to mutate existing binding */
static int env_assign(Env *e, const char *key, Value *val) {
    for (Env *cur=e; cur; cur=cur->parent) {
        for (int i=0;i<cur->len;i++) {
            if (!strcmp(cur->entries[i].key, key)) {
                val_decref(cur->entries[i].val);
                cur->entries[i].val = val_incref(val);
                return 1;
            }
        }
        if (cur->inst) {
            HowMap *fields = cur->inst->fields;
            for (int i=0;i<fields->len;i++) {
                if (!strcmp(fields->pairs[i].key, key)) {
                    val_decref(fields->pairs[i].val);
                    fields->pairs[i].val = val_incref(val);
                    return 1;
                }
            }
        }
    }
    return 0; /* not found */
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Map helpers                                                                 */
/* ─────────────────────────────────────────────────────────────────────────── */

static void map_set(HowMap *m, const char *key, Value *val) {
    for (int i=0;i<m->len;i++) {
        if (!strcmp(m->pairs[i].key, key)) {
            val_decref(m->pairs[i].val);
            m->pairs[i].val = val_incref(val);
            return;
        }
    }
    if (m->len + 1 >= m->cap) {
        m->cap = m->cap ? m->cap*2 : 8;
        m->pairs = xrealloc(m->pairs, m->cap*sizeof(KV));
    }
    m->pairs[m->len].key = xstrdup(key);
    m->pairs[m->len].val = val_incref(val);
    m->len++;
}

static Value *map_get(HowMap *m, const char *key) {
    for (int i=0;i<m->len;i++)
        if (!strcmp(m->pairs[i].key, key))
            return m->pairs[i].val;
    return NULL;
}

static int map_has(HowMap *m, const char *key) {
    for (int i=0;i<m->len;i++)
        if (!strcmp(m->pairs[i].key, key)) return 1;
    return 0;
}

static void map_del(HowMap *m, const char *key) {
    for (int i=0;i<m->len;i++) {
        if (!strcmp(m->pairs[i].key, key)) {
            free(m->pairs[i].key);
            val_decref(m->pairs[i].val);
            m->pairs[i] = m->pairs[--m->len];
            return;
        }
    }
}

/* list helpers */
static void list_push(HowList *l, Value *v) {
    if (l->len + 1 >= l->cap) {
        l->cap = l->cap ? l->cap*2 : 8;
        l->items = xrealloc(l->items, l->cap*sizeof(Value*));
    }
    l->items[l->len++] = val_incref(v);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  String representation                                                       */
/* ─────────────────────────────────────────────────────────────────────────── */

static char *val_repr(Value *v) {
    if (!v || v->type==VT_NONE) return xstrdup("none");
    switch(v->type) {
        case VT_BOOL: return xstrdup(v->bval ? "true" : "false");
        case VT_NUM: {
            char buf[64];
            double d = v->nval;
            if (d == (long long)d) snprintf(buf,sizeof(buf),"%lld",(long long)d);
            else snprintf(buf,sizeof(buf),"%g",d);
            return xstrdup(buf);
        }
        case VT_STR: return xstrdup(v->sval);
        case VT_LIST: {
            Buf b = {0};
            buf_append(&b,"[");
            for (int i=0;i<v->list->len;i++) {
                if(i) buf_append(&b,", ");
                char *s = val_repr(v->list->items[i]);
                buf_append(&b,s); free(s);
            }
            buf_append(&b,"]");
            return buf_done(&b);
        }
        case VT_MAP: {
            Buf b = {0};
            buf_append(&b,"{");
            for (int i=0;i<v->map->len;i++) {
                if(i) buf_append(&b,", ");
                buf_append(&b,v->map->pairs[i].key);
                buf_append(&b,": ");
                char *s = val_repr(v->map->pairs[i].val);
                buf_append(&b,s); free(s);
            }
            buf_append(&b,"}");
            return buf_done(&b);
        }
        case VT_FUNC:     return xstrdup("<function>");
        case VT_CLASS:    return xstrdup("<class>");
        case VT_INSTANCE: {
            Buf b={0}; buf_append(&b,"<instance {");
            HowMap *f=v->inst->fields;
            for(int i=0;i<f->len;i++){
                if(i) buf_append(&b,", ");
                buf_append(&b,f->pairs[i].key);
                buf_append(&b,": ");
                char *s=val_repr(f->pairs[i].val);
                buf_append(&b,s); free(s);
            }
            buf_append(&b,"}>"); return buf_done(&b);
        }
        case VT_MODULE:   { char buf[128]; snprintf(buf,sizeof(buf),"<module:%s>",v->mod->name); return xstrdup(buf); }
        case VT_BUILTIN:  return xstrdup("<builtin>");
        default: return xstrdup("<unknown>");
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Truthiness and equality                                                     */
/* ─────────────────────────────────────────────────────────────────────────── */

static int how_truthy(Value *v) {
    if (!v || v->type==VT_NONE) return 0;
    if (v->type==VT_BOOL) return v->bval;
    if (v->type==VT_NUM)  return v->nval != 0.0;
    if (v->type==VT_STR)  return v->sval[0] != '\0';
    return 1;
}

static int how_eq(Value *a, Value *b) {
    if (!a && !b) return 1;
    if (!a || !b) return 0;
    if (a->type==VT_NONE && b->type==VT_NONE) return 1;
    if (a->type==VT_NONE || b->type==VT_NONE) return 0;
    if (a->type==VT_BOOL && b->type==VT_BOOL) return a->bval==b->bval;
    /* bool vs non-bool: different types */
    if (a->type==VT_BOOL || b->type==VT_BOOL) return 0;
    if (a->type==VT_NUM && b->type==VT_NUM) return a->nval==b->nval;
    if (a->type==VT_STR && b->type==VT_STR) return !strcmp(a->sval,b->sval);
    if (a->type==VT_LIST && b->type==VT_LIST) {
        if (a->list->len != b->list->len) return 0;
        for (int i=0;i<a->list->len;i++)
            if (!how_eq(a->list->items[i], b->list->items[i])) return 0;
        return 1;
    }
    if (a->type==VT_MAP && b->type==VT_MAP) {
        if (a->map->len != b->map->len) return 0;
        for (int i=0;i<a->map->len;i++) {
            Value *bv = map_get(b->map, a->map->pairs[i].key);
            if (!bv || !how_eq(a->map->pairs[i].val, bv)) return 0;
        }
        return 1;
    }
    return a == b;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Interpreter                                                                 */
/* ─────────────────────────────────────────────────────────────────────────── */

/* Import search paths */
static int    g_num_builtins = 0;  /* set after setup_globals(); only real builtins */
static char **import_dirs;
static int    import_dirs_len;
static int    import_dirs_cap;

static void add_import_dir(const char *d) {
    /* check duplicate */
    for (int i=0;i<import_dirs_len;i++)
        if (!strcmp(import_dirs[i],d)) return;
    if (import_dirs_len+1 >= import_dirs_cap) {
        import_dirs_cap = import_dirs_cap ? import_dirs_cap*2 : 8;
        import_dirs = xrealloc(import_dirs, import_dirs_cap*sizeof(char*));
    }
    import_dirs[import_dirs_len++] = xstrdup(d);
}

static char *find_how_file(const char *name) {
    char path[4096];
    for (int i=0;i<import_dirs_len;i++) {
        snprintf(path, sizeof(path), "%s/%s.how", import_dirs[i], name);
        FILE *f = fopen(path,"r");
        if (f) { fclose(f); return xstrdup(path); }
    }
    snprintf(path, sizeof(path), "%s.how", name);
    return xstrdup(path);
}

/* Control flow signals */
typedef enum { SIG_NONE, SIG_RETURN, SIG_BREAK, SIG_NEXT, SIG_ERROR } SigType;
typedef struct { SigType type; Value *retval; } Signal;

/* Forward declarations */
static Value *eval(Node *node, Env *env, Signal *sig);
static Value *eval_call_val(Value *callee, Value **args, int argc, Signal *sig, int line);
static void run_branches(NodeList *branches, Env *env, Signal *sig);
static void run_loop(HowFunc *fn, Signal *sig);
static Value *instantiate_class(HowClass *cls, Value **args, int argc, Signal *sig);
static void exec_stmt(Node *node, Env *env, Signal *sig);
static void exec_import(const char *modname, const char *alias, Env *env);

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Builtins                                                                    */
/* ─────────────────────────────────────────────────────────────────────────── */

/* argc is the number of actual arguments passed */
#define BUILTIN(name) static Value *builtin_##name(int argc, Value **argv, void *ctx)
#define NEED(n) do{ if(argc<(n)) die("builtin requires %d args, got %d",(n),argc); }while(0)
#define ARG(i) (argc>(i) ? argv[i] : V_NONE_SINGLETON)

/* forward declaration */
static void exec_body(Node *body, Env *env, Signal *sig);

BUILTIN(print) {
    for (int i=0;i<argc;i++) {
        char *s = val_repr(argv[i]);
        if (i) printf(" ");
        printf("%s", s);
        free(s);
    }
    printf("\n");
    return val_none();
}

BUILTIN(len) {
    NEED(1);
    Value *v = ARG(0);
    if (v->type==VT_LIST) return val_num(v->list->len);
    if (v->type==VT_MAP)  return val_num(v->map->len);
    if (v->type==VT_STR)  return val_num(strlen(v->sval));
    if (v->type==VT_INSTANCE) return val_num(v->inst->fields->len);
    die("len() not supported for this type");
    return val_none();
}

BUILTIN(str_fn) {
    NEED(1);
    return val_str_own(val_repr(argv[0]));
}

BUILTIN(num_fn) {
    NEED(1);
    Value *v = ARG(0);
    if (v->type==VT_NUM) return val_num(v->nval);
    if (v->type==VT_STR) {
        char *end; double d = strtod(v->sval, &end);
        if (end == v->sval) die("cannot convert %s to number", v->sval);
        return val_num(d);
    }
    die("num() cannot convert this type");
    return val_none();
}

BUILTIN(type_fn) {
    NEED(1);
    Value *v = ARG(0);
    switch(v->type) {
        case VT_NONE:     return val_str("none");
        case VT_BOOL:     return val_str("bool");
        case VT_NUM:      return val_str("number");
        case VT_STR:      return val_str("string");
        case VT_LIST:     return val_str("list");
        case VT_MAP:      return val_str("map");
        case VT_FUNC:     return val_str("function");
        case VT_CLASS:    return val_str("class");
        case VT_INSTANCE: return val_str("instance");
        case VT_MODULE:   return val_str("module");
        case VT_BUILTIN:  return val_str("builtin");
        default:          return val_str("unknown");
    }
}

BUILTIN(floor_fn) {
    NEED(1); return val_num(floor(ARG(0)->nval));
}
BUILTIN(ceil_fn) {
    NEED(1); return val_num(ceil(ARG(0)->nval));
}
BUILTIN(abs_fn) {
    NEED(1); return val_num(fabs(ARG(0)->nval));
}
BUILTIN(sqrt_fn) {
    NEED(1); return val_num(sqrt(ARG(0)->nval));
}

BUILTIN(list_fn) {
    HowList *l = list_new();
    for (int i=0;i<argc;i++) list_push(l, argv[i]);
    Value *v = val_list(l); list_decref(l); return v;
}

BUILTIN(map_fn) {
    HowMap *m = map_new();
    Value *v = val_map(m); map_decref(m); return v;
}

BUILTIN(push_fn) {
    NEED(2);
    if (ARG(0)->type != VT_LIST) die("push() requires a list");
    list_push(ARG(0)->list, ARG(1));
    return val_incref(ARG(0));
}

BUILTIN(pop_fn) {
    NEED(1);
    if (ARG(0)->type != VT_LIST) die("pop() requires a list");
    HowList *l = ARG(0)->list;
    if (l->len == 0) die("pop() on empty list");
    Value *v = l->items[--l->len];
    /* remove from list without decref (transfer ownership) */
    return v;
}

BUILTIN(keys_fn) {
    NEED(1);
    HowMap *m = NULL;
    if (ARG(0)->type==VT_MAP)      m = ARG(0)->map;
    else if (ARG(0)->type==VT_INSTANCE) m = ARG(0)->inst->fields;
    else die("keys() requires a map or instance");
    HowList *l = list_new();
    for (int i=0;i<m->len;i++) list_push(l, val_str(m->pairs[i].key));
    Value *v = val_list(l); list_decref(l); return v;
}

BUILTIN(values_fn) {
    NEED(1);
    HowMap *m = NULL;
    if (ARG(0)->type==VT_MAP)      m = ARG(0)->map;
    else if (ARG(0)->type==VT_INSTANCE) m = ARG(0)->inst->fields;
    else die("values() requires a map or instance");
    HowList *l = list_new();
    for (int i=0;i<m->len;i++) list_push(l, m->pairs[i].val);
    Value *v = val_list(l); list_decref(l); return v;
}

BUILTIN(has_key_fn) {
    NEED(2);
    Value *obj = ARG(0), *key = ARG(1);
    if (key->type != VT_STR) {
        /* numeric key for lists */
        if (obj->type==VT_LIST && key->type==VT_NUM) {
            int i = (int)key->nval;
            return val_bool(i>=0 && i<obj->list->len);
        }
        die("has_key() key must be string");
    }
    HowMap *m = NULL;
    if (obj->type==VT_MAP)      m=obj->map;
    else if (obj->type==VT_INSTANCE) m=obj->inst->fields;
    else die("has_key() requires a map, instance, or list");
    return val_bool(map_has(m, key->sval));
}

BUILTIN(set_key_fn) {
    NEED(3);
    Value *obj=ARG(0), *key=ARG(1), *val=ARG(2);
    if (obj->type==VT_LIST) {
        int i = (int)(key->type==VT_NUM ? key->nval : atof(key->type==VT_STR?key->sval:"0"));
        if (i<0||i>=obj->list->len) die("set_key(): list index %d out of range",i);
        val_decref(obj->list->items[i]);
        obj->list->items[i] = val_incref(val);
        return val_incref(val);
    }
    HowMap *m = NULL;
    if (obj->type==VT_MAP)      m=obj->map;
    else if (obj->type==VT_INSTANCE) m=obj->inst->fields;
    else die("set_key() requires a map, instance, or list");
    if (key->type!=VT_STR) die("set_key() key must be string");
    map_set(m, key->sval, val);
    return val_incref(val);
}

BUILTIN(get_key_fn) {
    NEED(2);
    Value *obj=ARG(0), *key=ARG(1);
    if (obj->type==VT_LIST) {
        int i = (int)(key->type==VT_NUM ? key->nval : 0);
        if (i<0||i>=obj->list->len) return val_none();
        return val_incref(obj->list->items[i]);
    }
    HowMap *m = NULL;
    if (obj->type==VT_MAP)      m=obj->map;
    else if (obj->type==VT_INSTANCE) m=obj->inst->fields;
    else die("get_key() requires a map, instance, or list");
    if (key->type!=VT_STR) return val_none();
    Value *v = map_get(m, key->sval);
    return v ? val_incref(v) : val_none();
}

BUILTIN(del_key_fn) {
    NEED(2);
    Value *obj=ARG(0), *key=ARG(1);
    HowMap *m = NULL;
    if (obj->type==VT_MAP)      m=obj->map;
    else if (obj->type==VT_INSTANCE) m=obj->inst->fields;
    else die("del_key() requires a map or instance");
    if (key->type!=VT_STR) die("del_key() key must be string");
    map_del(m, key->sval);
    return val_none();
}

BUILTIN(range_fn) {
    long long start=0, stop=0, step=1;
    if (argc==1) { stop=(long long)ARG(0)->nval; }
    else if (argc==2) { start=(long long)ARG(0)->nval; stop=(long long)ARG(1)->nval; }
    else if (argc>=3) { start=(long long)ARG(0)->nval; stop=(long long)ARG(1)->nval; step=(long long)ARG(2)->nval; }
    else die("range() requires 1-3 args");
    HowList *l = list_new();
    for (long long i=start; (step>0?i<stop:i>stop); i+=step)
        list_push(l, val_num((double)i));
    Value *v = val_list(l); list_decref(l); return v;
}

BUILTIN(ask_fn) {
    if (argc>0) { char *s=val_repr(argv[0]); printf("%s",s); free(s); fflush(stdout); }
    char buf[4096]; if (!fgets(buf,sizeof(buf),stdin)) return val_none();
    int len=strlen(buf); if(len>0&&buf[len-1]=='\n') buf[len-1]=0;
    return val_str(buf);
}

BUILTIN(read_fn) {
    NEED(1);
    if (ARG(0)->type!=VT_STR) die("read() requires a string path");
    FILE *f = fopen(ARG(0)->sval,"r");
    if (!f) die("read(): cannot open '%s': %s", ARG(0)->sval, strerror(errno));
    fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f);
    char *buf = xmalloc(sz+1);
    fread(buf,1,sz,f); buf[sz]=0; fclose(f);
    return val_str_own(buf);
}

static void write_value_to_path(const char *path, Value *v) {
    FILE *f = fopen(path, "w");
    if (!f) die("write(): cannot open '%s' for writing: %s", path, strerror(errno));
    if (v->type == VT_STR) {
        if (fwrite(v->sval, 1, strlen(v->sval), f) < strlen(v->sval)) {
            fclose(f);
            die("write(): failed writing '%s'", path);
        }
    } else {
        char *s = val_repr(v);
        size_t n = strlen(s);
        if (fwrite(s, 1, n, f) < n) {
            free(s);
            fclose(f);
            die("write(): failed writing '%s'", path);
        }
        free(s);
    }
    fclose(f);
}

BUILTIN(write_fn) {
    NEED(2);
    if (ARG(0)->type!=VT_STR) die("write() requires a string path as first argument");
    write_value_to_path(ARG(0)->sval, ARG(1));
    return val_incref(ARG(1));
}

BUILTIN(args_fn) {
    /* argv is set in main */
    return val_incref(env_get(g_globals,"__args"));
}

static char *args_argv0 = NULL;  /* set in main */

BUILTIN(dirof_fn) {
    NEED(1);
    if (ARG(0)->type!=VT_STR) die("dirof() requires a string path");
    char buf[4096]; strncpy(buf,ARG(0)->sval,sizeof(buf)-1);
    /* dirname */
    char *slash = strrchr(buf,'/');
    if (!slash) return val_str("./");
    slash[1]=0;
    return val_str(buf);
}

BUILTIN(cwd_fn) {
    (void)argc; (void)argv; (void)ctx;
    char buf[4096];
    if (!getcwd(buf, sizeof(buf))) die("cwd(): cannot get working directory");
    return val_str(buf);
}

/* run(cmd) — execute a shell command, returns the exit code as a number */
BUILTIN(run_fn) {
    NEED(1);
    if (ARG(0)->type != VT_STR) die("run() requires a string command");
    int code = system(ARG(0)->sval);
    /* WEXITSTATUS extracts the real exit code from the wait() status */
#ifdef WEXITSTATUS
    if (code != -1) code = WEXITSTATUS(code);
#endif
    return val_num((double)code);
}

BUILTIN(resolve_how_fn) {
    NEED(1);
    if (ARG(0)->type!=VT_STR) die("_resolve_how() requires a string");
    char *path = find_how_file(ARG(0)->sval);
    return val_str_own(path);
}

/* _basename(path) — last path component without extension */
BUILTIN(basename_fn) {
    NEED(1);
    if (ARG(0)->type!=VT_STR) die("_basename() requires a string");
    const char *s = ARG(0)->sval;
    const char *slash = strrchr(s, '/');
    const char *base = slash ? slash+1 : s;
    /* strip .how extension if present */
    char buf[512]; strncpy(buf, base, sizeof(buf)-1); buf[sizeof(buf)-1]=0;
    int len = strlen(buf);
    if (len > 4 && !strcmp(buf+len-4, ".how")) buf[len-4] = 0;
    return val_str(buf);
}

/* _dirname(path) — directory part (empty string if no slash) */
BUILTIN(dirname_fn) {
    NEED(1);
    if (ARG(0)->type!=VT_STR) die("_dirname() requires a string");
    const char *s = ARG(0)->sval;
    const char *slash = strrchr(s, '/');
    if (!slash) return val_str("");
    char buf[4096];
    int len = (int)(slash - s);
    strncpy(buf, s, len); buf[len] = 0;
    return val_str(buf);
}

/* _add_search_dir("dir") — add a directory directly to import search path */
BUILTIN(add_search_dir_fn) {
    NEED(1);
    if (ARG(0)->type!=VT_STR) die("_add_search_dir() requires a string");
    /* Resolve relative to each existing import dir */
    const char *d = ARG(0)->sval;
    if (d[0] == '/') {
        add_import_dir(d);
    } else {
        int added = 0;
        for (int i=0;i<import_dirs_len && !added;i++) {
            char resolved[4096];
            snprintf(resolved,sizeof(resolved),"%s/%s",import_dirs[i],d);
            struct stat st2;
            if (stat(resolved, &st2) == 0 && S_ISDIR(st2.st_mode)) {
                add_import_dir(resolved); added=1;
            }
        }
        if (!added) add_import_dir(d);  /* add as-is */
    }
    return val_str(d);
}

BUILTIN(push_import_dir_fn) {
    NEED(1);
    if (ARG(0)->type!=VT_STR) die("_push_import_dir() requires a string");
    const char *path = ARG(0)->sval;
    /* get directory of path */
    char buf[4096]; strncpy(buf,path,sizeof(buf)-4);
    char *slash = strrchr(buf,'/');
    if (slash) { slash[1]=0; add_import_dir(buf); }
    else { /* no slash = cwd */ add_import_dir("."); }
    return val_str(buf);
}

BUILTIN(max_fn) {
    if (argc==1 && ARG(0)->type==VT_LIST) {
        HowList *l = ARG(0)->list;
        if (!l->len) return val_none();
        Value *best = l->items[0];
        for(int i=1;i<l->len;i++)
            if(l->items[i]->type==VT_NUM && l->items[i]->nval > best->nval) best=l->items[i];
        return val_incref(best);
    }
    if (!argc) return val_none();
    Value *best=argv[0];
    for(int i=1;i<argc;i++)
        if(argv[i]->type==VT_NUM && argv[i]->nval>best->nval) best=argv[i];
    return val_incref(best);
}

BUILTIN(min_fn) {
    if (argc==1 && ARG(0)->type==VT_LIST) {
        HowList *l = ARG(0)->list;
        if (!l->len) return val_none();
        Value *best = l->items[0];
        for(int i=1;i<l->len;i++)
            if(l->items[i]->type==VT_NUM && l->items[i]->nval < best->nval) best=l->items[i];
        return val_incref(best);
    }
    if (!argc) return val_none();
    Value *best=argv[0];
    for(int i=1;i<argc;i++)
        if(argv[i]->type==VT_NUM && argv[i]->nval<best->nval) best=argv[i];
    return val_incref(best);
}

BUILTIN(quit_fn) {
    (void)argc; (void)argv; (void)ctx;
    exit(0);
}

static void gc_collect(Env *root_env);

BUILTIN(gc_fn) {
    (void)argc; (void)argv; (void)ctx;
    gc_collect(g_globals);
    return val_none();
}

/* _host_call(fn, args_list) — call a native host function with a HowList of args */
BUILTIN(host_call_fn) {
    NEED(2);
    Value *fn = ARG(0);
    Value *args_val = ARG(1);
    if (args_val->type != VT_LIST) die("_host_call: args must be a list");
    HowList *args_list = args_val->list;
    Signal sig = {SIG_NONE, NULL};
    Value **argv2 = xmalloc(args_list->len * sizeof(Value*) + 1);
    for (int i=0;i<args_list->len;i++) argv2[i] = args_list->items[i];
    Value *res = eval_call_val(fn, argv2, args_list->len, &sig, 0);
    free(argv2);
    if (sig.type==SIG_RETURN) { val_decref(sig.retval); sig.retval=NULL; }
    if (sig.type==SIG_ERROR) {
        char *s = sig.retval ? val_repr(sig.retval) : xstrdup("unknown error");
        if (sig.retval) val_decref(sig.retval);
        val_decref(res);
        die("unhandled error in _host_call: %s", s);
    }
    return res;
}

static Value *make_builtin(const char *name, BuiltinFn fn) {
    Value *v = val_new(VT_BUILTIN);
    v->builtin.fn   = fn;
    v->builtin.ctx  = NULL;
    v->builtin.name = xstrdup(name);
    return v;
}


static void gc_mark_value(Value *v);
static void gc_mark_env(Env *e);
static void gc_mark_map(HowMap *m);
static void gc_mark_list(HowList *l);
static void gc_mark_func(HowFunc *f);
static void gc_mark_class(HowClass *c);
static void gc_mark_instance(HowInstance *inst);
static void gc_mark_module(HowModule *m);

static void gc_mark_value(Value *v) {
    if (!v || v->gc_mark) return;
    v->gc_mark = 1;
    switch (v->type) {
        case VT_LIST: gc_mark_list(v->list); break;
        case VT_MAP: gc_mark_map(v->map); break;
        case VT_FUNC: gc_mark_func(v->func); break;
        case VT_CLASS: gc_mark_class(v->cls); break;
        case VT_INSTANCE: gc_mark_instance(v->inst); break;
        case VT_MODULE: gc_mark_module(v->mod); break;
        default: break;
    }
}

static void gc_mark_map(HowMap *m) {
    if (!m || m->gc_mark) return;
    m->gc_mark = 1;
    for (int i = 0; i < m->len; i++) gc_mark_value(m->pairs[i].val);
}

static void gc_mark_list(HowList *l) {
    if (!l || l->gc_mark) return;
    l->gc_mark = 1;
    for (int i = 0; i < l->len; i++) gc_mark_value(l->items[i]);
}

static void gc_mark_func(HowFunc *f) {
    if (!f || f->gc_mark) return;
    f->gc_mark = 1;
    gc_mark_env(f->closure);
}

static void gc_mark_class(HowClass *c) {
    if (!c || c->gc_mark) return;
    c->gc_mark = 1;
    gc_mark_env(c->closure);
}

static void gc_mark_instance(HowInstance *inst) {
    if (!inst || inst->gc_mark) return;
    inst->gc_mark = 1;
    gc_mark_map(inst->fields);
    gc_mark_env(inst->inst_env);
}

static void gc_mark_module(HowModule *m) {
    if (!m || m->gc_mark) return;
    m->gc_mark = 1;
    gc_mark_env(m->env);
}

static void gc_mark_env(Env *e) {
    if (!e || e->gc_mark) return;
    e->gc_mark = 1;
    gc_mark_env(e->parent);
    if (e->inst) gc_mark_instance(e->inst);
    for (int i = 0; i < e->len; i++) gc_mark_value(e->entries[i].val);
}

static void gc_sweep_values(void) {
    Value **pp = &g_all_values;
    while (*pp) {
        Value *v = *pp;
        if (v->gc_mark) {
            v->gc_mark = 0;
            pp = &v->gc_next;
            continue;
        }
        *pp = v->gc_next;
        if (v == V_NONE_SINGLETON) V_NONE_SINGLETON = NULL;
        if (v == V_TRUE_SINGLETON) V_TRUE_SINGLETON = NULL;
        if (v == V_FALSE_SINGLETON) V_FALSE_SINGLETON = NULL;
        if (v->type == VT_STR) free(v->sval);
        else if (v->type == VT_BUILTIN) free(v->builtin.name);
        free(v);
    }
}

static void gc_sweep_maps(void) {
    HowMap **pp = &g_all_maps;
    while (*pp) {
        HowMap *m = *pp;
        if (m->gc_mark) {
            m->gc_mark = 0;
            pp = &m->gc_next;
            continue;
        }
        *pp = m->gc_next;
        for (int i = 0; i < m->len; i++) free(m->pairs[i].key);
        free(m->pairs);
        free(m);
    }
}

static void gc_sweep_lists(void) {
    HowList **pp = &g_all_lists;
    while (*pp) {
        HowList *l = *pp;
        if (l->gc_mark) {
            l->gc_mark = 0;
            pp = &l->gc_next;
            continue;
        }
        *pp = l->gc_next;
        free(l->items);
        free(l);
    }
}

static void gc_sweep_funcs(void) {
    HowFunc **pp = &g_all_funcs;
    while (*pp) {
        HowFunc *f = *pp;
        if (f->gc_mark) {
            f->gc_mark = 0;
            pp = &f->gc_next;
            continue;
        }
        *pp = f->gc_next;
        for (int i = 0; i < f->params.len; i++) free(f->params.s[i]);
        free(f->params.s);
        if (f->iter_var) free(f->iter_var);
        free(f);
    }
}

static void gc_sweep_classes(void) {
    HowClass **pp = &g_all_classes;
    while (*pp) {
        HowClass *c = *pp;
        if (c->gc_mark) {
            c->gc_mark = 0;
            pp = &c->gc_next;
            continue;
        }
        *pp = c->gc_next;
        for (int i = 0; i < c->params.len; i++) free(c->params.s[i]);
        free(c->params.s);
        free(c);
    }
}

static void gc_sweep_instances(void) {
    HowInstance **pp = &g_all_instances;
    while (*pp) {
        HowInstance *inst = *pp;
        if (inst->gc_mark) {
            inst->gc_mark = 0;
            pp = &inst->gc_next;
            continue;
        }
        *pp = inst->gc_next;
        free(inst);
    }
}

static void gc_sweep_modules(void) {
    HowModule **pp = &g_all_modules;
    while (*pp) {
        HowModule *m = *pp;
        if (m->gc_mark) {
            m->gc_mark = 0;
            pp = &m->gc_next;
            continue;
        }
        *pp = m->gc_next;
        free(m->name);
        free(m);
    }
}

static void gc_sweep_envs(void) {
    Env **pp = &g_all_envs;
    while (*pp) {
        Env *e = *pp;
        if (e->gc_mark) {
            e->gc_mark = 0;
            pp = &e->gc_next;
            continue;
        }
        *pp = e->gc_next;
        for (int i = 0; i < e->len; i++) free(e->entries[i].key);
        free(e->entries);
        free(e);
    }
}

static void gc_clear_root_stacks(void) {
    g_gc_value_roots.len = 0;
    g_gc_env_roots.len = 0;
}

static void gc_mark_root_stacks(void) {
    for (int i = 0; i < g_gc_value_roots.len; i++) {
        Value **slot = g_gc_value_roots.slots[i];
        if (slot && *slot) gc_mark_value(*slot);
    }
    for (int i = 0; i < g_gc_env_roots.len; i++) {
        Env **slot = g_gc_env_roots.slots[i];
        if (slot && *slot) gc_mark_env(*slot);
    }
}

static void gc_collect(Env *root_env) {
    if (g_gc_in_progress) return;
    g_gc_in_progress = 1;
    if (V_NONE_SINGLETON) gc_mark_value(V_NONE_SINGLETON);
    if (V_TRUE_SINGLETON) gc_mark_value(V_TRUE_SINGLETON);
    if (V_FALSE_SINGLETON) gc_mark_value(V_FALSE_SINGLETON);
    gc_mark_root_stacks();
    gc_mark_env(root_env);
    if (g_globals && g_globals != root_env) gc_mark_env(g_globals);
    if (g_module_registry) gc_mark_map(g_module_registry);
    gc_sweep_values();
    gc_sweep_maps();
    gc_sweep_lists();
    gc_sweep_funcs();
    gc_sweep_classes();
    gc_sweep_instances();
    gc_sweep_modules();
    gc_sweep_envs();
    g_gc_collections++;
    g_gc_in_progress = 0;
}

static void setup_globals(Env *env) {
#define REG(name,fn) env_set(env,name,make_builtin(name,builtin_##fn))
    REG("print",   print);
    REG("len",     len);
    REG("str",     str_fn);
    REG("num",     num_fn);
    REG("type",    type_fn);
    REG("floor",   floor_fn);
    REG("ceil",    ceil_fn);
    REG("abs",     abs_fn);
    REG("sqrt",    sqrt_fn);
    REG("list",    list_fn);
    REG("map",     map_fn);
    REG("push",    push_fn);
    REG("pop",     pop_fn);
    REG("keys",    keys_fn);
    REG("values",  values_fn);
    REG("has_key", has_key_fn);
    REG("set_key", set_key_fn);
    REG("get_key", get_key_fn);
    REG("del_key", del_key_fn);
    REG("range",   range_fn);
    REG("ask",     ask_fn);
    REG("read",    read_fn);
    REG("write",   write_fn);
    REG("args",    args_fn);
    REG("dirof",   dirof_fn);
    REG("cwd",     cwd_fn);
    REG("run",     run_fn);
    REG("_resolve_how",     resolve_how_fn);
    REG("_push_import_dir", push_import_dir_fn);
    REG("_add_search_dir",  add_search_dir_fn);
    REG("max",     max_fn);
    REG("min",     min_fn);
    REG("quit",    quit_fn);
    REG("gc",      gc_fn);
    REG("_host_call",      host_call_fn);
    REG("_basename", basename_fn);
    REG("_dirname",  dirname_fn);
    /* bool() as a function */
    Value *t = val_bool(1); env_set(env,"true",t); val_decref(t);
    Value *f = val_bool(0); env_set(env,"false",f); val_decref(f);
    Value *n = val_none(); env_set(env,"none",n); val_decref(n);
#undef REG
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Interpreter core                                                            */
/* ─────────────────────────────────────────────────────────────────────────── */

/* augmented assignment helper */
static Value *apply_augop(Value *old, Value *val, const char *op, int line) {
    GC_ROOT_VALUE(old);
    GC_ROOT_VALUE(val);
    if (!strcmp(op,"+=")) {
        if (old->type==VT_STR || val->type==VT_STR) {
            char *a = val_repr(old), *b = val_repr(val);
            Buf buf={0}; buf_append(&buf,a); buf_append(&buf,b);
            free(a); free(b);
            Value *ret = val_str_own(buf_done(&buf)); GC_UNROOT_VALUE(); GC_UNROOT_VALUE(); return ret;
        }
        if (old->type==VT_LIST && val->type==VT_LIST) {
            HowList *nl = list_new();
            for (int i=0;i<old->list->len;i++) list_push(nl,old->list->items[i]);
            for (int i=0;i<val->list->len;i++) list_push(nl,val->list->items[i]);
            Value *r = val_list(nl); list_decref(nl); GC_UNROOT_VALUE(); GC_UNROOT_VALUE(); return r;
        }
        if (old->type!=VT_NUM||val->type!=VT_NUM) die_at(line, 0, "+= requires numbers or strings");
        Value *ret = val_num(old->nval + val->nval); GC_UNROOT_VALUE(); GC_UNROOT_VALUE(); return ret;
    }
    if (!strcmp(op,"-=")) {
        if (old->type!=VT_NUM||val->type!=VT_NUM) die_at(line, 0, "-= requires numbers");
        Value *ret = val_num(old->nval - val->nval); GC_UNROOT_VALUE(); GC_UNROOT_VALUE(); return ret;
    }
    if (!strcmp(op,"*=")) {
        if (old->type!=VT_NUM||val->type!=VT_NUM) die_at(line, 0, "*= requires numbers");
        Value *ret = val_num(old->nval * val->nval); GC_UNROOT_VALUE(); GC_UNROOT_VALUE(); return ret;
    }
    if (!strcmp(op,"/=")) {
        if (old->type!=VT_NUM||val->type!=VT_NUM) die_at(line, 0, "/= requires numbers");
        if (val->nval==0) die_at(line, 0, "division by zero");
        Value *ret = val_num(old->nval / val->nval); GC_UNROOT_VALUE(); GC_UNROOT_VALUE(); return ret;
    }
    if (!strcmp(op,"%=")) {
        if (old->type!=VT_NUM||val->type!=VT_NUM) die_at(line, 0, "%%= requires numbers");
        if (val->nval==0) die_at(line, 0, "modulo by zero");
        Value *ret = val_num(fmod(old->nval, val->nval)); GC_UNROOT_VALUE(); GC_UNROOT_VALUE(); return ret;
    }
    die_at(line, 0, "unknown augop %s", op);
    GC_UNROOT_VALUE();
    GC_UNROOT_VALUE();
    return val_none();
}

static Value *eval(Node *node, Env *env, Signal *sig) {
    if (!node) return val_none();
    if (sig->type != SIG_NONE) return val_none();

    switch(node->type) {
    case N_NUM:  return val_num(node->nval);
    case N_STR:  return val_str(node->sval);
    case N_BOOL: return val_bool(node->bval);
    case N_NONE: return val_none();
    case N_BREAK:
        sig->type = SIG_BREAK;
        return val_none();
    case N_NEXT:
        sig->type = SIG_NEXT;
        return val_none();

    case N_IDENT: {
        Value *v = env_get(env, node->sval);
        if (!v) die_at(node->line, 0, "undefined variable '%s'", node->sval);
        return val_incref(v);
    }

    case N_BINOP: {
        const char *op = node->binop.op;
        /* short-circuit */
        if (!strcmp(op,"and")) {
            Value *l = eval(node->binop.left, env, sig);
            if (!how_truthy(l)) return l;
            val_decref(l);
            return eval(node->binop.right, env, sig);
        }
        if (!strcmp(op,"or")) {
            Value *l = eval(node->binop.left, env, sig);
            if (how_truthy(l)) return l;
            val_decref(l);
            return eval(node->binop.right, env, sig);
        }
        Value *l = eval(node->binop.left, env, sig);
        if (sig->type!=SIG_NONE) return l;
        GC_ROOT_VALUE(l);
        Value *r = eval(node->binop.right, env, sig);
        if (sig->type!=SIG_NONE) { GC_UNROOT_VALUE(); val_decref(l); return r; }
        GC_ROOT_VALUE(r);
        Value *res;
        if (!strcmp(op,"+")) {
            if (l->type==VT_LIST && r->type==VT_LIST) {
                HowList *nl=list_new();
                for(int i=0;i<l->list->len;i++) list_push(nl,l->list->items[i]);
                for(int i=0;i<r->list->len;i++) list_push(nl,r->list->items[i]);
                res=val_list(nl); list_decref(nl);
            } else if (l->type==VT_STR||r->type==VT_STR) {
                char *a=val_repr(l), *b=val_repr(r);
                Buf buf={0}; buf_append(&buf,a); buf_append(&buf,b);
                free(a); free(b); res=val_str_own(buf_done(&buf));
            } else {
                if(l->type!=VT_NUM||r->type!=VT_NUM) die_at(node->line, 0, "'+' requires numbers");
                res=val_num(l->nval+r->nval);
            }
        } else if (!strcmp(op,"-")) {
            if(l->type!=VT_NUM||r->type!=VT_NUM) die_at(node->line, 0, "'-' requires numbers");
            res=val_num(l->nval-r->nval);
        } else if (!strcmp(op,"*")) {
            if(l->type!=VT_NUM||r->type!=VT_NUM) die_at(node->line, 0, "'*' requires numbers");
            res=val_num(l->nval*r->nval);
        } else if (!strcmp(op,"/")) {
            if(l->type!=VT_NUM||r->type!=VT_NUM) die_at(node->line, 0, "'/' requires numbers");
            if(r->nval==0) die_at(node->line, 0, "division by zero");
            res=val_num(l->nval/r->nval);
        } else if (!strcmp(op,"%")) {
            if(l->type!=VT_NUM||r->type!=VT_NUM) die_at(node->line, 0, "'%%' requires numbers");
            res=val_num(fmod(l->nval,r->nval));
        } else if (!strcmp(op,"==")) {
            res=val_bool(how_eq(l,r));
        } else if (!strcmp(op,"!=")) {
            res=val_bool(!how_eq(l,r));
        } else if (!strcmp(op,"<")) {
            if(l->type!=VT_NUM||r->type!=VT_NUM) {
                if(l->type==VT_STR&&r->type==VT_STR){res=val_bool(strcmp(l->sval,r->sval)<0);}
                else die_at(node->line, 0, "'<' requires numbers");
            } else res=val_bool(l->nval<r->nval);
        } else if (!strcmp(op,">")) {
            if(l->type!=VT_NUM||r->type!=VT_NUM) {
                if(l->type==VT_STR&&r->type==VT_STR){res=val_bool(strcmp(l->sval,r->sval)>0);}
                else die_at(node->line, 0, "'>' requires numbers");
            } else res=val_bool(l->nval>r->nval);
        } else if (!strcmp(op,"<=")) {
            if(l->type!=VT_NUM||r->type!=VT_NUM) {
                if(l->type==VT_STR&&r->type==VT_STR){res=val_bool(strcmp(l->sval,r->sval)<=0);}
                else die_at(node->line, 0, "'<=' requires numbers");
            } else res=val_bool(l->nval<=r->nval);
        } else if (!strcmp(op,">=")) {
            if(l->type!=VT_NUM||r->type!=VT_NUM) {
                if(l->type==VT_STR&&r->type==VT_STR){res=val_bool(strcmp(l->sval,r->sval)>=0);}
                else die_at(node->line, 0, "'>=' requires numbers");
            } else res=val_bool(l->nval>=r->nval);
        } else {
            die_at(node->line, 0, "unknown operator '%s'", op);
            res=val_none();
        }
        GC_UNROOT_VALUE();
        GC_UNROOT_VALUE();
        val_decref(l); val_decref(r);
        return res;
    }

    case N_UNARY: {
        Value *v = eval(node->binop.left, env, sig);
        if (sig->type!=SIG_NONE) return v;
        GC_ROOT_VALUE(v);
        const char *op = node->binop.op;
        Value *res;
        if (!strcmp(op,"-")) {
            if (v->type!=VT_NUM) die_at(node->line, 0, "unary '-' requires a number");
            res=val_num(-v->nval);
        } else {
            res=val_bool(!how_truthy(v));
        }
        GC_UNROOT_VALUE();
        val_decref(v); return res;
    }

    case N_ASSIGN: {
        Value *val = eval(node->assign.value, env, sig);
        if (sig->type!=SIG_NONE) return val;
        GC_ROOT_VALUE(val);
        Node *tgt = node->assign.target;
        const char *op = node->assign.op;

        if (tgt->type == N_IDENT) {
            if (!strcmp(op,"=")) {
                if (!env_assign(env, tgt->sval, val))
                    die_at(node->line, 0, "assignment to undeclared variable '%s'", tgt->sval);
            } else {
                Value *old = env_get(env, tgt->sval);
                if (!old) die_at(node->line, 0, "assignment to undeclared variable '%s'", tgt->sval);
                Value *newv = apply_augop(old, val, op, node->line);
                env_assign(env, tgt->sval, newv);
                val_decref(val); val_decref(newv);
                return val_none();
            }
            GC_UNROOT_VALUE();
            val_decref(val); return val_none();
        }

        if (tgt->type == N_DOT) {
            Value *obj = eval(tgt->dot.obj, env, sig);
            GC_ROOT_VALUE(obj);
            if (sig->type!=SIG_NONE) { GC_UNROOT_VALUE(); GC_UNROOT_VALUE(); val_decref(val); return obj; }
            const char *attr = tgt->dot.attr;
            HowMap *fields = NULL;
            if (obj->type==VT_INSTANCE) fields=obj->inst->fields;
            else if (obj->type==VT_MAP) fields=obj->map;
            else die_at(node->line, 0, "cannot assign to field on non-map/instance");
            if (!strcmp(op,"=")) {
                map_set(fields, attr, val);
            } else {
                Value *old = map_get(fields, attr);
                if (!old) old = val_none();
                Value *newv = apply_augop(old, val, op, node->line);
                map_set(fields, attr, newv);
                val_decref(newv);
            }
            GC_UNROOT_VALUE();
            GC_UNROOT_VALUE();
            val_decref(obj); val_decref(val);
            return val_none();
        }
        /* m(k) = v  or  m(k) += v — dynamic key assignment */
        if (tgt->type == N_CALL && tgt->call.args.len == 1 && !tgt->call.bracket) {
            Value *obj = eval(tgt->call.callee, env, sig);
            GC_ROOT_VALUE(obj);
            if (sig->type!=SIG_NONE) { GC_UNROOT_VALUE(); GC_UNROOT_VALUE(); val_decref(val); return obj; }
            Value *key = eval(tgt->call.args.nodes[0], env, sig);
            if (sig->type!=SIG_NONE) { GC_UNROOT_VALUE(); GC_UNROOT_VALUE(); val_decref(val); val_decref(obj); return key; }
            if (obj->type==VT_MAP) {
                char *ks = val_repr(key); val_decref(key);
                if (!strcmp(op,"=")) {
                    map_set(obj->map, ks, val);
                } else {
                    Value *oldv = map_get(obj->map, ks);
                    if (!oldv) oldv = val_none();
                    Value *newv = apply_augop(oldv, val, op, node->line);
                    map_set(obj->map, ks, newv);
                    val_decref(newv);
                }
                free(ks);
            } else if (obj->type==VT_LIST) {
                int idx = (int)key->nval; val_decref(key);
                HowList *l = obj->list;
                if (idx<0||idx>=(int)l->len)
                    die_at(node->line, 0, "list index %d out of bounds in assignment", idx);
                if (!strcmp(op,"=")) {
                    val_decref(l->items[idx]);
                    l->items[idx] = val_incref(val);
                } else {
                    Value *newv = apply_augop(l->items[idx], val, op, node->line);
                    val_decref(l->items[idx]);
                    l->items[idx] = newv;
                }
            } else {
                die_at(node->line, 0, "() assignment requires map or list");
            }
            GC_UNROOT_VALUE();
            val_decref(obj); val_decref(val);
            return val_none();
        }
        GC_UNROOT_VALUE();
        die_at(node->line, 0, "invalid assignment target");
        return val_none();
    }

    case N_DOT: {
        Value *obj = eval(node->dot.obj, env, sig);
        if (sig->type!=SIG_NONE) return obj;
        GC_ROOT_VALUE(obj);
        const char *attr = node->dot.attr;
        Value *res = NULL;
        if (obj->type==VT_INSTANCE) {
            res = map_get(obj->inst->fields, attr);
            if (!res) die_at(node->line, 0, "no field '%s' on instance", attr);
            res = val_incref(res);
        } else if (obj->type==VT_MAP) {
            res = map_get(obj->map, attr);
            if (!res) die_at(node->line, 0, "no key '%s' in map", attr);
            res = val_incref(res);
        } else if (obj->type==VT_MODULE) {
            res = env_get(obj->mod->env, attr);
            if (!res) die_at(node->line, 0, "module '%s' has no export '%s'", obj->mod->name, attr);
            res = val_incref(res);
        } else {
            die_at(node->line, 0, "cannot access .%s on %s", attr,
                obj->type==VT_NONE?"none":obj->type==VT_NUM?"number":"value");
        }
        GC_UNROOT_VALUE();
        val_decref(obj);
        return res;
    }

    case N_CALL: {
        Value *callee = eval(node->call.callee, env, sig);
        if (sig->type!=SIG_NONE) return callee;
        GC_ROOT_VALUE(callee);
        int argc = node->call.args.len;
        Value **args = xmalloc(argc * sizeof(Value*) + 1);
        for (int i=0;i<argc;i++) {
            args[i] = NULL;
            args[i] = eval(node->call.args.nodes[i], env, sig);
            if (sig->type==SIG_NONE) GC_ROOT_VALUE(args[i]);
            if (sig->type!=SIG_NONE) {
                Value *errv = args[i];
                for(int j=0;j<i;j++) { GC_UNROOT_VALUE(); val_decref(args[j]); }
                free(args); GC_UNROOT_VALUE(); val_decref(callee); return errv;
            }
        }
        callee->refcount++;  /* hold extra ref so callee survives the call */
        Value *res = eval_call_val(callee, args, argc, sig, node->line);
        for (int i=0;i<argc;i++) { GC_UNROOT_VALUE(); val_decref(args[i]); }
        free(args);
        GC_UNROOT_VALUE();
        val_decref(callee);  /* release the extra ref */
        val_decref(callee);  /* release the original eval ref */
        return res;
    }

    case N_SLICE: {
        Value *col = eval(node->slice.col, env, sig);
        if (sig->type!=SIG_NONE) return col;
        GC_ROOT_VALUE(col);
        Value *start_v = node->slice.start ? eval(node->slice.start, env, sig) : NULL;
        if (start_v) GC_ROOT_VALUE(start_v);
        if (sig->type!=SIG_NONE) { if (start_v) GC_UNROOT_VALUE(); GC_UNROOT_VALUE(); val_decref(col); return start_v ? start_v : val_none(); }
        Value *stop_v  = node->slice.stop  ? eval(node->slice.stop,  env, sig) : NULL;
        if (stop_v) GC_ROOT_VALUE(stop_v);
        if (sig->type!=SIG_NONE) {
            if (stop_v) GC_UNROOT_VALUE();
            if (start_v) GC_UNROOT_VALUE();
            GC_UNROOT_VALUE();
            val_decref(col); if(start_v) val_decref(start_v);
            return stop_v ? stop_v : val_none();
        }
        int start = start_v && start_v->type==VT_NUM ? (int)start_v->nval : 0;
        int stop  = stop_v  && stop_v->type==VT_NUM  ? (int)stop_v->nval  :
                    (col->type==VT_LIST ? col->list->len :
                     col->type==VT_STR  ? (int)strlen(col->sval) : 0);
        Value *res;
        if (col->type==VT_LIST) {
            HowList *nl=list_new();
            for(int i=start;i<stop&&i<col->list->len;i++) list_push(nl,col->list->items[i]);
            res=val_list(nl); list_decref(nl);
        } else if (col->type==VT_STR) {
            int slen=strlen(col->sval);
            if (start < 0) start = 0;
            if (stop > slen) stop = slen;
            if (stop < start) stop = start;
            char *s=xmalloc(stop-start+1);
            memcpy(s,col->sval+start,stop-start); s[stop-start]=0;
            res=val_str_own(s);
        } else {
            die_at(node->line, 0, "cannot slice this type");
            res=val_none();
        }
        if(stop_v) GC_UNROOT_VALUE();
        if(start_v) GC_UNROOT_VALUE();
        GC_UNROOT_VALUE();
        val_decref(col); if(start_v) val_decref(start_v); if(stop_v) val_decref(stop_v);
        return res;
    }

    case N_FUNC: {
        HowFunc *fn = xmalloc(sizeof(*fn));
        memset(fn, 0, sizeof(*fn));
        fn->params   = strlist_clone(node->func.params);
        fn->branches = node->func.branches;
        fn->closure  = env; env->refcount++;
        fn->is_loop  = node->func.is_loop;
        fn->refcount = 1;
        /* Register fn AFTER val_new to avoid GC sweeping fn before its Value exists */
        Value *v = val_new(VT_FUNC);
        v->func = fn;
        fn->gc_next = g_all_funcs; g_all_funcs = fn; g_gc_allocations++;
        return v;
    }

    case N_CLASS: {
        HowClass *cls = xmalloc(sizeof(*cls));
        memset(cls, 0, sizeof(*cls));
        cls->params   = strlist_clone(node->func.params);
        cls->branches = node->func.branches;
        cls->closure  = env; env->refcount++;
        cls->refcount = 1;
        /* Register cls AFTER val_new to avoid GC sweeping cls before its Value exists */
        Value *v = val_new(VT_CLASS);
        v->cls = cls;
        cls->gc_next = g_all_classes; g_all_classes = cls; g_gc_allocations++;
        return v;
    }

    case N_MAP_LIT: {
        MapItemList *items = &node->map_lit.items;
        if (!items->len) {
            HowMap *m=map_new(); Value *v=val_map(m); map_decref(m); return v;
        }
        /* list if first key is NULL */
        if (!items->items[0].key) {
            HowList *l=list_new();
            for(int i=0;i<items->len;i++) {
                Value *v=eval(items->items[i].val,env,sig);
                if(sig->type!=SIG_NONE){list_decref(l);return v;}
                list_push(l,v); val_decref(v);
            }
            Value *v=val_list(l); list_decref(l); return v;
        }
        HowMap *m=map_new();
        for(int i=0;i<items->len;i++) {
            Value *k=eval(items->items[i].key,env,sig);
            if(sig->type!=SIG_NONE){map_decref(m);return k;}
            Value *v=eval(items->items[i].val,env,sig);
            if(sig->type!=SIG_NONE){val_decref(k);map_decref(m);return v;}
            char *ks = val_repr(k);
            map_set(m,ks,v); free(ks);
            val_decref(k); val_decref(v);
        }
        Value *v=val_map(m); map_decref(m); return v;
    }

    case N_FORLOOP: {
        HowFunc *fn = xmalloc(sizeof(HowFunc)); memset(fn,0,sizeof(HowFunc));
        fn->is_loop     = 0;
        fn->is_forrange = 1;
        fn->iter_var    = xstrdup(node->forloop.iter_var);
        fn->fr_start    = node->forloop.start;
        fn->fr_stop     = node->forloop.stop;
        for (int _i=0; _i<node->forloop.branches.len; _i++)
            nl_push(&fn->branches, node->forloop.branches.nodes[_i]);
        fn->closure  = env;
        if (env) env->refcount++;
        fn->refcount = 1;
        Value *fv = val_new(VT_FUNC);
        fv->func = fn;
        fn->gc_next = g_all_funcs; g_all_funcs = fn; g_gc_allocations++;
        return fv;
    }

    case N_VARDECL: {
        Value *v = eval(node->vardecl.value, env, sig);
        if (sig->type!=SIG_NONE) return v;
        env_set(env, node->vardecl.name, v);
        val_decref(v);
        return val_none();
    }

    case N_BLOCK: {
        Env *child = env_new(env);
        GC_ROOT_ENV(child);
        for (int i=0;i<node->block.stmts.len;i++) {
            Node *s = node->block.stmts.nodes[i];
            exec_stmt(s, child, sig);
            if (sig->type!=SIG_NONE) break;
        }
        GC_UNROOT_ENV();
        env_decref(child);
        return val_none();
    }

    case N_BRANCH:
        /* branches should be handled by run_branches, not eval directly */
        exec_stmt(node, env, sig);
        return val_none();

    case N_CATCH: {
        /* evaluate the expression in an inner signal; catch SIG_ERROR */
        Signal inner = {SIG_NONE, NULL};
        Value *v = eval(node->catch_node.expr, env, &inner);
        if (inner.type == SIG_ERROR) {
            /* expression raised an error — call handler(err) */
            Value *err = inner.retval ? inner.retval : val_none();
            GC_ROOT_VALUE(err);
            val_decref(v);
            Value *handler = eval(node->catch_node.handler, env, sig);
            if (sig->type != SIG_NONE) {
                GC_UNROOT_VALUE(); val_decref(err);
                return handler;
            }
            GC_ROOT_VALUE(handler);
            Value *result = eval_call_val(handler, &err, 1, sig, node->line);
            GC_UNROOT_VALUE(); /* handler */
            GC_UNROOT_VALUE(); /* err */
            val_decref(handler);
            val_decref(err);
            return result;
        }
        /* no error — propagate any other signal (RETURN, BREAK, NEXT) */
        if (inner.type != SIG_NONE) *sig = inner;
        return v;
    }

    case N_IMPORT:
        exec_import(node->import_node.path, node->import_node.alias, env);
        return val_none();
    case N_WHERE:
        add_import_dir(node->sval);
        return val_none();

    case N_PROG: {
        for (int i=0;i<node->prog.stmts.len;i++) {
            exec_stmt(node->prog.stmts.nodes[i], env, sig);
            if (sig->type!=SIG_NONE) break;
        }
        return val_none();
    }

    default:
        die_at(node->line, 0, "unknown node type %d", node->type);
        return val_none();
    }
}

/* Evaluate a function call given a Value callee */
static Value *eval_call_val(Value *callee, Value **args, int argc, Signal *sig, int line) {
    if (callee->type==VT_BUILTIN) {
        return callee->builtin.fn(argc, args, callee->builtin.ctx);
    }
    if (callee->type==VT_CLASS) {
        return instantiate_class(callee->cls, args, argc, sig);
    }
    if (callee->type==VT_FUNC) {
        HowFunc *fn = callee->func;
        if (fn->is_forrange) {
            int start_v = 0;
            if (fn->fr_start) {
                Value *sv = eval(fn->fr_start, fn->closure, sig);
                if (sig->type!=SIG_NONE) return sv;
                start_v = (int)sv->nval; val_decref(sv);
            }
            Value *stop_val = eval(fn->fr_stop, fn->closure, sig);
            if (sig->type!=SIG_NONE) return stop_val;
            int stop_v = (int)stop_val->nval; val_decref(stop_val);
            Env *local = env_new(fn->closure);
            GC_ROOT_ENV(local);
            for (int _i=start_v; _i<stop_v && sig->type==SIG_NONE; _i++) {
                Value *iv = val_num((double)_i);
                if (!env_assign(local, fn->iter_var, iv))
                    env_set(local, fn->iter_var, iv);
                val_decref(iv);
                run_branches(&fn->branches, local, sig);
                if (sig->type==SIG_BREAK) { sig->type=SIG_NONE; break; }
                if (sig->type==SIG_NEXT)  { sig->type=SIG_NONE; continue; }
            }
            GC_UNROOT_ENV();
            env_decref(local);
            Value *r = sig->type==SIG_RETURN ? sig->retval : val_none();
            if (sig->type==SIG_RETURN) { sig->type=SIG_NONE; sig->retval=NULL; }
            return r;
        }
        if (fn->is_loop) {
            run_loop(fn, sig);
            Value *r = sig->type==SIG_RETURN ? sig->retval : val_none();
            if (sig->type==SIG_RETURN) { sig->type=SIG_NONE; sig->retval=NULL; }
            return r;
        }
        if (argc != fn->params.len)
            die_at(line, 0, "expected %d args but got %d", fn->params.len, argc);
        /* Hold a ref to closure so it survives env_decref(local) which decrefs parent */
        Env *closure = fn->closure;
        if (closure) closure->refcount++;
        Env *local = env_new(fn->closure);
        GC_ROOT_ENV(local);
        for (int i=0;i<fn->params.len;i++)
            env_set(local, fn->params.s[i], args[i]);
        run_branches(&fn->branches, local, sig);
        GC_UNROOT_ENV();
        env_decref(local);
        if (closure) env_decref(closure);  /* release our extra hold */
        Value *r = sig->type==SIG_RETURN ? sig->retval : val_none();
        if (sig->type==SIG_RETURN) { sig->type=SIG_NONE; sig->retval=NULL; }
        return r;
    }
    /* Map call: map(key) → map[key] */
    if (callee->type==VT_MAP) {
        if (argc!=1) die_at(line, 0, "map call requires exactly 1 argument");
        char *ks = val_repr(args[0]);
        Value *v = map_get(callee->map, ks); free(ks);
        if (!v) return val_none();  /* missing key → none */
        return val_incref(v);
    }
    /* List call: list(i) → list[i] */
    if (callee->type==VT_LIST) {
        if (argc!=1) die_at(line, 0, "list call requires exactly 1 argument");
        if (args[0]->type!=VT_NUM) die_at(line, 0, "list index must be a number");
        int i=(int)args[0]->nval;
        if (i<0||i>=callee->list->len) die_at(line, 0, "list index %d out of range", i);
        return val_incref(callee->list->items[i]);
    }
    if (callee->type==VT_INSTANCE) {
        /* instance(key) → field */
        if (argc!=1) die("instance call requires 1 argument (line %d)",line);
        if (args[0]->type!=VT_STR) die_at(line, 0, "instance call key must be string");
        Value *v = map_get(callee->inst->fields, args[0]->sval);
        if (!v) die_at(line, 0, "no field '%s' on instance", args[0]->sval);
        return val_incref(v);
    }
    { const char *tn =
        callee->type==0?"none":callee->type==1?"bool":callee->type==2?"number":
        callee->type==3?"string":callee->type==4?"list":callee->type==5?"map":"value";
      die_at(line, 0, "not callable (value is a %s)", tn); }
    return val_none();
}

/* Execute statement */
static void exec_stmt(Node *node, Env *env, Signal *sig) {
    if (!node || sig->type!=SIG_NONE) return;
    switch(node->type) {
    case N_VARDECL: {
        Value *v = eval(node->vardecl.value, env, sig);
        if (sig->type==SIG_NONE) env_set(env, node->vardecl.name, v);
        val_decref(v);
        return;
    }
    case N_IMPORT:
        exec_import(node->import_node.path, node->import_node.alias, env);
        return;
    case N_WHERE:
        /* where "dir" — add directory to module search path */
        add_import_dir(node->sval);
        return;
    case N_BLOCK: {
        Env *child = env_new(env);
        GC_ROOT_ENV(child);
        for (int i=0;i<node->block.stmts.len;i++) {
            exec_stmt(node->block.stmts.nodes[i], child, sig);
            if (sig->type!=SIG_NONE) break;
        }
        GC_UNROOT_ENV();
        env_decref(child);
        return;
    }
    case N_BRANCH: {
        /* evaluate condition */
        int cond_ok = 1;
        if (node->branch.cond) {
            Value *cv = eval(node->branch.cond, env, sig);
            if (sig->type!=SIG_NONE) { val_decref(cv); return; }
            cond_ok = how_truthy(cv);
            val_decref(cv);
        }
        if (!cond_ok) return;
        if (node->branch.is_throw) {
            Value *v = eval(node->branch.body, env, sig);
            if (sig->type==SIG_NONE) {
                sig->type = SIG_ERROR;
                sig->retval = v;
            } else {
                val_decref(v);
            }
        } else if (node->branch.is_ret) {
            Value *v = eval(node->branch.body, env, sig);
            if (sig->type==SIG_NONE) {
                sig->type = SIG_RETURN;
                sig->retval = v;
            } else {
                val_decref(v);
            }
        } else {
            exec_body(node->branch.body, env, sig);
        }
        return;
    }
    default: {
        Value *v = eval(node, env, sig);
        val_decref(v);
        return;
    }
    }
}

/* Execute a branch body (block or expr) */
static void exec_body(Node *body, Env *env, Signal *sig) {
    if (!body || sig->type!=SIG_NONE) return;
    if (body->type==N_BLOCK) {
        Env *child = env_new(env);
        GC_ROOT_ENV(child);
        for (int i=0;i<body->block.stmts.len;i++) {
            exec_stmt(body->block.stmts.nodes[i], child, sig);
            if (sig->type!=SIG_NONE) break;
        }
        GC_UNROOT_ENV();
        env_decref(child);
    } else if (body->type==N_BREAK) {
        sig->type = SIG_BREAK;
    } else if (body->type==N_NEXT) {
        sig->type = SIG_NEXT;
    } else {
        Value *v = eval(body, env, sig);
        val_decref(v);
    }
}

/* Run a list of branches in order */
static void run_branches(NodeList *branches, Env *env, Signal *sig) {
    for (int i=0; i<branches->len && sig->type==SIG_NONE; i++) {
        exec_stmt(branches->nodes[i], env, sig);
    }
}

/* Unbounded (:)= loop */
static void run_loop(HowFunc *fn, Signal *sig) {
Env *local = env_new(fn->closure);
    GC_ROOT_ENV(local);
    while (sig->type==SIG_NONE) {
        /* all : branches fire independently per iteration */
        for (int i=0;i<fn->branches.len && sig->type==SIG_NONE;i++) {
            Node *b = fn->branches.nodes[i];
            if (b->type == N_VARDECL) {
                Value *v = eval(b->vardecl.value, local, sig);
                if (sig->type==SIG_NONE) env_set(local, b->vardecl.name, v);
                val_decref(v);
                continue;
            }
            if (b->type != N_BRANCH) {
                Value *v = eval(b, local, sig); val_decref(v); continue;
            }
            /* branch node */
            if (b->branch.cond) {
                /* All : branches are evaluated independently — consistent with
                   function semantics where all matching : branches fire.
                   :: branches also always fire (they exit immediately on match). */
                Value *cv = eval(b->branch.cond, local, sig);
                if (sig->type!=SIG_NONE) { val_decref(cv); break; }
                int ok = how_truthy(cv); val_decref(cv);
                if (!ok) continue;
                if (b->branch.is_throw) {
                    Value *v = eval(b->branch.body, local, sig);
                    if (sig->type==SIG_NONE) { sig->type=SIG_ERROR; sig->retval=v; }
                    else val_decref(v);
                    break;
                }
                if (b->branch.is_ret) {
                    Value *v = eval(b->branch.body, local, sig);
                    if (sig->type==SIG_NONE) { sig->type=SIG_RETURN; sig->retval=v; }
                    else val_decref(v);
                    break;
                }
                exec_body(b->branch.body, local, sig);
                if (sig->type==SIG_BREAK) { sig->type=SIG_NONE; goto loop_done; }
                if (sig->type==SIG_NEXT)  { sig->type=SIG_NONE; break; }
                /* no conditional_fired — all : branches fire independently */
            } else {
                /* unconditional */
                if (b->branch.is_throw) {
                    Value *v = eval(b->branch.body, local, sig);
                    if (sig->type==SIG_NONE) { sig->type=SIG_ERROR; sig->retval=v; }
                    else val_decref(v);
                    break;
                }
                if (b->branch.is_ret) {
                    Value *v = eval(b->branch.body, local, sig);
                    if (sig->type==SIG_NONE) { sig->type=SIG_RETURN; sig->retval=v; }
                    else val_decref(v);
                    break;
                }
                exec_body(b->branch.body, local, sig);
                if (sig->type==SIG_BREAK) { sig->type=SIG_NONE; goto loop_done; }
            }
        }
    }
    loop_done:
    GC_UNROOT_ENV();
    env_decref(local);
}

/* Class instantiation */
static Value *instantiate_class(HowClass *cls, Value **args, int argc, Signal *sig) {
    if (argc != cls->params.len)
        die("class expects %d args but got %d", cls->params.len, argc);

    Env *init_env = env_new(cls->closure);
    GC_ROOT_ENV(init_env);
    for (int i=0;i<cls->params.len;i++)
        env_set(init_env, cls->params.s[i], args[i]);

    HowMap *fields = map_new();

    for (int i=0;i<cls->branches.len;i++) {
        Node *b = cls->branches.nodes[i];
        Signal inner = {SIG_NONE, NULL};

        /* var decl → local init var */
        if (b->type==N_VARDECL) {
            Value *v=eval(b->vardecl.value,init_env,&inner);
            if(inner.type==SIG_NONE) env_set(init_env,b->vardecl.name,v);
            val_decref(v); continue;
        }
        if (b->type!=N_BRANCH) {
            Value *v=eval(b,init_env,&inner); val_decref(v); continue;
        }

        /* named field: ident: value (is_ret=false) */
        if (b->branch.cond && !b->branch.is_ret) {
            Node *cond = b->branch.cond;
            if (cond->type==N_IDENT) {
                Value *v=eval(b->branch.body,init_env,&inner);
                if(inner.type==SIG_NONE) map_set(fields,cond->sval,v);
                val_decref(v); continue;
            }
            if (cond->type==N_STR) {
                Value *v=eval(b->branch.body,init_env,&inner);
                if(inner.type==SIG_NONE) map_set(fields,cond->sval,v);
                val_decref(v); continue;
            }
            /* conditional side-effect */
            Value *cv=eval(cond,init_env,&inner);
            if(inner.type==SIG_NONE && how_truthy(cv))
                exec_body(b->branch.body,init_env,&inner);
            val_decref(cv); continue;
        }
        /* unconditional side-effect */
        if (!b->branch.cond && !b->branch.is_ret) {
            exec_body(b->branch.body,init_env,&inner);
            continue;
        }
    }

    HowInstance *inst = xmalloc(sizeof(*inst));
    memset(inst, 0, sizeof(*inst));
    inst->fields   = fields; fields->refcount++;
    inst->refcount = 1;

    /* InstanceEnv: variables are backed by fields */
    /* Register inst AFTER inst_env_new to avoid GC sweeping inst before inst_env set */
    Env *inst_env = inst_env_new(inst, init_env);
    GC_ROOT_ENV(inst_env);
    inst->inst_env = inst_env; inst_env->refcount++;
    inst->gc_next = g_all_instances; g_all_instances = inst; g_gc_allocations++;

    /* Re-wrap method closures so they close over inst_env */
    for (int i=0;i<fields->len;i++) {
        Value *v = fields->pairs[i].val;
        if (v && v->type==VT_FUNC) {
            HowFunc *oldfn = v->func;
            StrList   saved_params   = strlist_clone(oldfn->params);
            NodeList  saved_branches = oldfn->branches;
            int       saved_is_loop  = oldfn->is_loop;
            inst_env->refcount++;
            HowFunc *newfn = xmalloc(sizeof(*newfn));
            memset(newfn, 0, sizeof(*newfn));
            newfn->params   = saved_params;
            newfn->branches = saved_branches;
            newfn->is_loop  = saved_is_loop;
            newfn->closure  = inst_env;
            newfn->refcount = 1;
            /* Register newfn AFTER val_new to avoid GC sweeping it before its Value exists */
            Value *nv = val_new(VT_FUNC);
            nv->func = newfn;
            newfn->gc_next = g_all_funcs; g_all_funcs = newfn; g_gc_allocations++;
            fields->pairs[i].val = nv;
            val_decref(v);
        }
    }

    map_decref(fields);
    GC_UNROOT_ENV();
    env_decref(init_env);

    Value *result = val_new(VT_INSTANCE);
    result->inst = inst;

    /* Auto-call _init() if defined — so classes don't need explicit q._init() */
    {
        Value *init_fn = env_get(inst->inst_env, "_init");
        if (init_fn && init_fn->type == VT_FUNC) {
            Signal init_sig = {SIG_NONE, NULL};
            Value *no_args[] = {NULL};
            GC_ROOT_VALUE(result);
            Value *init_ret = eval_call_val(init_fn, no_args, 0, &init_sig, 0);
            GC_UNROOT_VALUE();
            if (init_ret) val_decref(init_ret);
            if (init_sig.type == SIG_RETURN && init_sig.retval)
                val_decref(init_sig.retval);
        }
    }

    GC_UNROOT_ENV();
    return result;
}

/* Module import */
static void exec_import(const char *modname, const char *alias, Env *env) {
    /* modname may be "path/to/module" (string-path form) or bare "module" */
    char bind_name[256];  /* name to bind in env = last path component */
    strncpy(bind_name, modname, sizeof(bind_name)-1);
    bind_name[sizeof(bind_name)-1] = 0;
    const char *slash = strrchr(modname, '/');
    if (slash) {
        /* "samples/lru_cache" -> add "samples/" to search, bind as "lru_cache" */
        strncpy(bind_name, slash+1, sizeof(bind_name)-1);
        bind_name[sizeof(bind_name)-1] = 0;
        /* Add the directory part to import_dirs */
        char dir[4096];
        int dirlen = (int)(slash - modname);
        /* Resolve dir relative to each existing import_dir */
        int added = 0;
        for (int i=0;i<import_dirs_len && !added;i++) {
            snprintf(dir, sizeof(dir), "%s/%.*s", import_dirs[i], dirlen, modname);
            /* Check if this dir exists by trying to open a file there */
            char probe[4096];
            snprintf(probe, sizeof(probe), "%s/%s.how", dir, bind_name);
            FILE *fp = fopen(probe,"r");
            if (fp) { fclose(fp); add_import_dir(dir); added=1; }
        }
        if (!added) {
            /* Try as absolute or cwd-relative path */
            snprintf(dir, sizeof(dir), "%.*s", dirlen, modname);
            add_import_dir(dir);
        }
        /* Now import just the module name */
        exec_import(bind_name, alias, env);
        return;
    } else {
        strncpy(bind_name, modname, sizeof(bind_name)-1);
        bind_name[sizeof(bind_name)-1] = 0;
    }
    /* Check module cache to avoid re-executing already-loaded modules */
    if (g_module_registry) {
        Value *cached = map_get(g_module_registry, modname);
        if (cached) {
            /* Re-bind exports and module value from cache */
            if (cached->type == VT_MODULE) {
                Env *pub_env = cached->mod->env;
                for (int i = 0; i < pub_env->len; i++)
                    env_set(env, pub_env->entries[i].key, pub_env->entries[i].val);
            }
            const char *final_name = (alias && alias[0]) ? alias : bind_name;
            env_set(env, final_name, cached);
            return;
        }
    }

    char *path = find_how_file(modname);
    FILE *fh = fopen(path,"r");
    if (!fh) die("cannot find module '%s' (searched dirs)", modname);
    fseek(fh,0,SEEK_END); long sz=ftell(fh); rewind(fh);
    char *src = xmalloc(sz+1);
    fread(src,1,sz,fh); src[sz]=0; fclose(fh);
    free(path);

    /* push module directory to import path */
    {
        char *path2 = find_how_file(modname);
        char dir[4096]; strncpy(dir, path2, sizeof(dir)-4);
        free(path2);
        char *dslash = strrchr(dir,'/');
        if (dslash) { dslash[1]=0; add_import_dir(dir); }
    }

    set_source_context(modname, src);
    TokenList *tl = lex(src); free(src);
    Parser p = {tl, 0};
    Node *prog = parse_prog(&p);

    /* run in fresh env with copy of builtins */
    Env *mod_env = env_new(NULL);
    GC_ROOT_ENV(mod_env);
    /* copy builtins from globals */
    for (int i=0;i<g_globals->len;i++)
        env_set(mod_env, g_globals->entries[i].key, g_globals->entries[i].val);

    Signal sig = {SIG_NONE, NULL};
    for (int i=0;i<prog->prog.stmts.len;i++) {
        exec_stmt(prog->prog.stmts.nodes[i], mod_env, &sig);
        if (sig.type==SIG_ERROR) {
            char *s = sig.retval ? val_repr(sig.retval) : xstrdup("unknown error");
            if (sig.retval) val_decref(sig.retval);
            die("unhandled error in module '%s': %s", modname, s);
        }
        sig.type=SIG_NONE;
    }

    /* Build module value */
    HowModule *mod = xmalloc(sizeof(*mod));
    memset(mod, 0, sizeof(*mod));
    mod->name = xstrdup(modname);
    /* Register mod AFTER env_new to avoid GC sweeping mod before mod->env is set */
    /* expose non-builtin vars in a clean env */
    Env *pub_env = env_new(NULL);
    GC_ROOT_ENV(pub_env);
    for (int i=0;i<mod_env->len;i++) {
        /* skip true builtins (not user-imported vars that ended up in g_globals) */
        int is_builtin = 0;
        for (int j=0;j<g_num_builtins;j++) {
            if (!strcmp(g_globals->entries[j].key, mod_env->entries[i].key)) {
                is_builtin=1; break;
            }
        }
        if (!is_builtin)
            env_set(pub_env, mod_env->entries[i].key, mod_env->entries[i].val);
    }
    mod->env = pub_env;
    mod->refcount = 1;
    /* Now mod->env is set; register with GC so gc_mark_module can trace pub_env */
    mod->gc_next = g_all_modules; g_all_modules = mod; g_gc_allocations++;

    Value *modval = val_new(VT_MODULE); modval->mod = mod;
    /* Register in module cache to prevent re-loading and circular import loops */
    if (g_module_registry) map_set(g_module_registry, modname, modval);
    /* Bind each exported var directly so helpers are accessible by name */
    for (int i=0;i<pub_env->len;i++) {
        env_set(env, pub_env->entries[i].key, pub_env->entries[i].val);
    }
    /* Bind the module under alias (if given) or bind_name.
     * Module binding wins last so dot-access always works:
     *   how lru_cache           → lru_cache = module
     *   how lru_cache as cache  → cache = module, cache.lru_cache = class */
    const char *final_name = (alias && alias[0]) ? alias : bind_name;
    env_set(env, final_name, modval);
    val_decref(modval);
    GC_UNROOT_ENV();
    env_decref(mod_env);
    GC_UNROOT_ENV();
    /* Collect without wiping the caller's GC roots */
    gc_collect(env);
}


/* ─────────────────────────────────────────────────────────────────────────── */
/*  REPL                                                                        */
/* ─────────────────────────────────────────────────────────────────────────── */

static void run_source(const char *name, const char *src, Env *env) {
    set_source_context(name, src);
    TokenList *tl = lex(src);
    Parser p = {tl, 0};
    Node *prog = parse_prog(&p);
    Signal sig = {SIG_NONE, NULL};
    for (int i=0;i<prog->prog.stmts.len;i++) {
        exec_stmt(prog->prog.stmts.nodes[i], env, &sig);
        if (sig.type==SIG_ERROR) {
            char *s = sig.retval ? val_repr(sig.retval) : xstrdup("unknown error");
            if (sig.retval) val_decref(sig.retval);
            sig.retval = NULL; sig.type = SIG_NONE;
            if (g_repl_active) {
                snprintf(g_repl_errmsg, sizeof(g_repl_errmsg), "Unhandled error: %s", s);
                free(s);
                longjmp(g_repl_jmp, 1);
            }
            fprintf(stderr, "\033[31m[Error]\033[0m Unhandled error: %s\n", s);
            free(s);
            exit(1);
        }
        if (sig.type==SIG_RETURN && sig.retval) {
            val_decref(sig.retval);
            sig.retval=NULL;
        }
        sig.type=SIG_NONE;
        gc_clear_root_stacks();
        GC_ROOT_ENV(env);
        gc_collect(env);
        GC_UNROOT_ENV();
    }
    gc_clear_root_stacks();
    GC_ROOT_ENV(env);
    gc_collect(env);
    GC_UNROOT_ENV();
}


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

static void repl(Env *env) {
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
        g_repl_active = 1;
        if (setjmp(g_repl_jmp) == 0) {
            run_source("<repl>", runbuf, env);
        } else {
            /* Error was caught — print it nicely and continue */
            fprintf(stderr, "\033[31m[Error] %s\033[0m\n", g_repl_errmsg);
        }
        g_repl_active = 0;
    }
}


/* ─────────────────────────────────────────────────────────────────────────── */
/*  main                                                                        */
/* ─────────────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    /* Init singletons */
    V_NONE_SINGLETON  = val_new(VT_NONE);  V_NONE_SINGLETON->refcount  = 999999;
    V_TRUE_SINGLETON  = val_new(VT_BOOL);  V_TRUE_SINGLETON->bval  = 1; V_TRUE_SINGLETON->refcount  = 999999;
    V_FALSE_SINGLETON = val_new(VT_BOOL);  V_FALSE_SINGLETON->bval = 0; V_FALSE_SINGLETON->refcount = 999999;

    /* Setup globals */
    g_globals = env_new(NULL);
    setup_globals(g_globals);
    g_num_builtins = g_globals->len;  /* snapshot: only real builtins */
    g_module_registry = map_new();

    /* Build args list: argv[1:] */
    HowList *args_list = list_new();
    for (int i=1;i<argc;i++) list_push(args_list, val_str(argv[i]));
    Value *args_val = val_list(args_list); list_decref(args_list);
    env_set(g_globals, "__args", args_val);
    env_set(g_globals, "__argv", args_val);
    Value *imp_false = val_bool(0);
    env_set(g_globals, "__is_import", imp_false);
    val_decref(imp_false);
    Value *file_val = (argc >= 2) ? val_str(argv[1]) : val_none();
    env_set(g_globals, "__file", file_val);
    val_decref(file_val);
    val_decref(args_val);

    /* Add cwd to import dirs */
    add_import_dir(".");

    if (argc < 2) {
        repl(g_globals);
        gc_clear_root_stacks();
        GC_ROOT_ENV(g_globals);
        gc_collect(g_globals);
        GC_UNROOT_ENV();
        g_globals = NULL;
        gc_clear_root_stacks();
        gc_collect(NULL);
        return 0;
    }

    /* Run only the first file; remaining argv are accessible via args() builtin.
     * This matches Python howlang.py behaviour: run argv[1], expose argv[1:] as args. */
    {
        const char *script = argv[1];

        /* Add script directory to import path */
        char dir[4096]; strncpy(dir, script, sizeof(dir)-1);
        char *dslash = strrchr(dir, '/');
        if (dslash) { dslash[1] = 0; add_import_dir(dir); }

        /* Read the script file */
        FILE *f = fopen(script, "r");
        if (!f) {
            /* Try as stdin path */
            if (!strcmp(script, "/dev/stdin") || !strcmp(script, "-")) {
                f = stdin;
            } else {
                fprintf(stderr, "cannot open '%s': %s\n", script, strerror(errno));
                return 1;
            }
        }
        char *source = NULL; size_t sz = 0, cap = 4096;
        source = xmalloc(cap);
        size_t n;
        while ((n = fread(source+sz, 1, cap-sz-1, f)) > 0) {
            sz += n;
            if (sz + 1 >= cap) { cap *= 2; source = xrealloc(source, cap); }
        }
        source[sz] = 0;
        if (f != stdin) fclose(f);

        run_source(script, source, g_globals);
        free(source);
    }
    return 0;
}
