#include "common.h"
#include "ast.h"
#include "lexer_internal.h"

/* ── AST helpers ─────────────────────────────────────────────────────────── */

void nl_push(NodeList *nl, Node *n) {
    if (nl->len + 1 >= nl->cap) {
        nl->cap = nl->cap ? nl->cap*2 : 8;
        nl->nodes = xrealloc(nl->nodes, nl->cap * sizeof(Node*));
    }
    nl->nodes[nl->len++] = n;
}
void sl_push(StrList *sl, char *s) {
    if (sl->len + 1 >= sl->cap) {
        sl->cap = sl->cap ? sl->cap*2 : 4;
        sl->s = xrealloc(sl->s, sl->cap * sizeof(char*));
    }
    sl->s[sl->len++] = s;
}
void mil_push(MapItemList *ml, Node *k, Node *v) {
    if (ml->len + 1 >= ml->cap) {
        ml->cap = ml->cap ? ml->cap*2 : 8;
        ml->items = xrealloc(ml->items, ml->cap * sizeof(MapItem));
    }
    ml->items[ml->len].key = k;
    ml->items[ml->len].val = v;
    ml->len++;
}
Node *make_node(NodeType t, int line) {
    Node *n = xmalloc(sizeof(Node));
    memset(n, 0, sizeof(*n));
    n->type = t;
    n->line = line;
    return n;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Token diagnostics                                                           */
/* ─────────────────────────────────────────────────────────────────────────── */

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
        case TT_CARET:     return "'^'";
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

static void repl_parse_error(Token *cur, const char *errmsg, const char *hint) {
    how_repl_set_loc_errorf("ParseError", cur->line, cur->col, "%s", errmsg);
    if (hint) {
        char full[2048];
        snprintf(full, sizeof(full), "%s\n%s", how_repl_error(), hint);
        how_repl_set_errorf("%s", full);
    }
    how_repl_longjmp();
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
        if (how_repl_is_active()) {
            repl_parse_error(cur, errmsg, hint);
        }
        fprintf(stderr, "\033[31m[ParseError]\033[0m %s\n", errmsg);
        if (how_current_source_name() && cur->line > 0)
            fprintf(stderr, "  --> %s:%d:%d\n", how_current_source_name(), cur->line, cur->col);
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
static Node *parse_catch(Parser *p);

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Statement parsing                                                           */
/* ─────────────────────────────────────────────────────────────────────────── */

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

    /* `::` is only a conditional-return operator when it appears on the same
       line as the condition expression it follows.  A `::` that appears on a
       fresh line is an unconditional return and will be handled the next time
       parse_branch is called — just like `!!` (see the TT_DBANG check above). */
    if (p_check(p, TT_DCOLON) && p_peek(p,0)->line == cond->line) {
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

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Expression parsing (precedence climbing)                                   */
/* ─────────────────────────────────────────────────────────────────────────── */

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
static Node *parse_call_arg(Parser *p, char **arg_name);
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
        else if(p_check(p,TT_AT))      op="@";
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

static Node *parse_call_arg(Parser *p, char **arg_name) {
    *arg_name = NULL;
    if (p_check(p, TT_IDENT) && p_peek(p, 1)->type == TT_EQ) {
        *arg_name = xstrdup(p_adv(p)->sval);
        p_adv(p); /* '=' */
        return parse_catch(p);
    }
    return parse_catch(p);
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
                char *first_name = NULL;
                Node *first = parse_call_arg(p, &first_name);
                if (!first_name && p_check(p, TT_COLON)) {
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
                    sl_push(&c->call.arg_names, first_name);
                    int saw_named = (first_name != NULL);
                    while (p_match(p,TT_COMMA) && !p_check(p,TT_RPAREN)) {
                        char *arg_name = NULL;
                        Node *arg = parse_call_arg(p, &arg_name);
                        if (saw_named && !arg_name)
                            die("positional arguments cannot follow named arguments");
                        if (arg_name) saw_named = 1;
                        nl_push(&c->call.args, arg);
                        sl_push(&c->call.arg_names, arg_name);
                    }
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
                char *first_name = NULL;
                Node *first = parse_call_arg(p, &first_name);
                nl_push(&c->call.args, first);
                sl_push(&c->call.arg_names, first_name);
                int saw_named = (first_name != NULL);
                while (p_match(p,TT_COMMA) && !p_check(p,TT_RBRACKET)) {
                    char *arg_name = NULL;
                    Node *arg = parse_call_arg(p, &arg_name);
                    if (saw_named && !arg_name)
                        die("positional arguments cannot follow named arguments");
                    if (arg_name) saw_named = 1;
                    nl_push(&c->call.args, arg);
                    sl_push(&c->call.arg_names, arg_name);
                }
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

/* ─────────────────────────────────────────────────────────────────────────── */
/*  String interpolation                                                        */
/* ─────────────────────────────────────────────────────────────────────────── */

/* Chains two nodes with a "+" binop, returning the new root. */
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

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Atom / primary expression                                                   */
/* ─────────────────────────────────────────────────────────────────────────── */

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
        int lookslike_map = 0, lookslike_list = 0;
        {
            /* Map: first item is STR or IDENT immediately followed by COLON (not DCOLON)
             * List: no colons at depth 0 before a comma or }
             * Func (fallthrough): has DCOLON, OR condition is complex expression, OR body is { block }
             */
            TT t0 = p->tl->toks[p->pos].type;
            TT t1 = (p->pos+1 < p->tl->len) ? p->tl->toks[p->pos+1].type : TT_EOF;
            if (t1 == TT_DCOLON) {
                /* ident :: body → function (fallthrough) */
            } else if (t0 == TT_STRING && t1 == TT_COLON) {
                /* String key: always a map literal regardless of value type */
                lookslike_map = 1;
            } else if (t0 == TT_IDENT && t1 == TT_COLON) {
                /* Ident key followed by colon: could be map field or branch cond: body.
                 * If value after : is a block {, it's a branch function.
                 * If value is a simple expr (num/str/bool/ident/paren), it's a map. */
                TT t2 = (p->pos+2 < p->tl->len) ? p->tl->toks[p->pos+2].type : TT_EOF;
                if (t2 != TT_LBRACE) lookslike_map = 1;    /* field: value */
                /* else: cond: { block } → function (fallthrough) */
            } else if (t0 == TT_DCOLON) {
                /* :: expr → function (fallthrough) */
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
                if(!found_dcolon && !found_colon) {
                    lookslike_list=1;
                }
                /* else: contains :: or : → branch function (fallthrough) */
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
                        if (found_rparen && (st==TT_LBRACE || st==TT_CARET)) { is_forrange=1; break; }
                        if (found_rparen) break; /* something other than { or ^ after ) */
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
                n->forloop.iter_var   = ivar;
                n->forloop.start      = start;
                n->forloop.stop       = stop;
                n->forloop.is_parallel = p_match(p, TT_CARET) != NULL;
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
        if (how_repl_is_active()) {
            char errmsg[512];
            snprintf(errmsg, sizeof(errmsg), "unexpected token %s", token_type_name((int)cur->type));
            repl_parse_error(cur, errmsg, hint);
        }
        fprintf(stderr, "\033[31m[ParseError]\033[0m unexpected token %s\n",
                token_type_name((int)cur->type));
        if (how_current_source_name() && cur->line > 0)
            fprintf(stderr, "  --> %s:%d:%d\n", how_current_source_name(), cur->line, cur->col);
        if (cur->line > 0) print_source_context(stderr, cur->line, cur->col);
        if (hint) fprintf(stderr, "%s\n", hint);
        exit(1);
    }
    return NULL;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Public API                                                                  */
/* ─────────────────────────────────────────────────────────────────────────── */

Node *how_parse_source(const char *src) {
    TokenList *tl = lex(src);
    Parser p = {tl, 0};
    return parse_prog(&p);
}
