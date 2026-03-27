/*
 * ast.h — AST node types and structures, shared between the frontend (parser)
 *         and the runtime (evaluator).
 */
#ifndef HOWLANG_AST_H
#define HOWLANG_AST_H

typedef struct Value Value;
typedef struct Env Env;
typedef struct Node Node;
typedef struct NodeList NodeList;

typedef enum {
    /* literals */
    N_NUM, N_STR, N_BOOL, N_NONE,
    /* expressions */
    N_IDENT, N_BINOP, N_UNARY, N_ASSIGN,
    N_DOT, N_CALL, N_SLICE,
    /* callables */
    N_FUNC, N_FORLOOP, N_CLASS,
    /* composite literals */
    N_MAP_LIT,
    /* control flow */
    N_BLOCK,
    N_BRANCH,   /* cond :: body  |  cond : body  |  bare expr */
    N_CATCH,    /* expr catch handler */
    /* statements */
    N_VARDECL,
    N_EXPRSTMT,
    N_IMPORT,   /* how <path> [as <alias>] */
    N_BREAK, N_NEXT,
    N_WHERE,    /* where <path> — file-scoped import */
    N_PROG,     /* top-level program (sequence of statements) */
} NodeType;

/* Dynamic lists used inside Node payloads. */
struct NodeList { Node **nodes; int len; int cap; };
void nl_push(NodeList *nl, Node *n);

typedef struct { char **s; int len; int cap; } StrList;
void sl_push(StrList *sl, char *s);

typedef struct MapItem { Node *key; Node *val; } MapItem;  /* NULL key = list element */
typedef struct { MapItem *items; int len; int cap; } MapItemList;
void mil_push(MapItemList *ml, Node *k, Node *v);

struct Node {
    NodeType type;
    int      line;
    union {
        double  nval;
        char   *sval;
        int     bval;

        struct { char *op; Node *left; Node *right; } binop;
        struct { char *op; Node *target; Node *value; } assign;
        struct { Node *obj; char *attr; } dot;
        struct { char *path; char *alias; } import_node;
        struct { Node *callee; NodeList args; int bracket; } call;
        struct { Node *col; Node *start; Node *stop; } slice;  /* col(start:stop) */
        struct { StrList params; NodeList branches; int is_loop; } func;  /* is_loop: (:){} loop */
        struct { char *iter_var; Node *start; Node *stop; NodeList branches; } forloop;
        struct { MapItemList items; } map_lit;
        struct { NodeList stmts; } block;
        struct { Node *cond; Node *body; int is_ret; int is_throw; } branch;
        struct { Node *expr; Node *handler; } catch_node;
        struct { char *name; Node *value; } vardecl;
        struct { NodeList stmts; } prog;
    };
};

Node *make_node(NodeType t, int line);

#endif
