/*
 * sema.c — semantic analysis for the frontend AST.
 *
 * This first compiler-facing pass does three jobs:
 *   - resolve identifiers into local / upvalue / global buckets
 *   - collect closure metadata for functions, classes, and for-range callables
 *   - report static issues that are easier to catch here than during execution
 *
 * The implementation is intentionally conservative. It does not try to rewrite
 * the AST or impose a bytecode ABI yet; it just records enough structure that
 * later compiler phases can lower the real language faithfully.
 */
#include "sema.h"
#include "common.h"

/* ── Scope bookkeeping ──────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    int         slot;
} ScopeBinding;

typedef struct SemaFunc SemaFunc;

typedef struct SemaScope {
    ScopeBinding       *bindings;
    int                 len;
    int                 cap;
    SemaFunc           *owner;
    struct SemaScope   *parent;
} SemaScope;

struct SemaFunc {
    Node      *owner_node;
    char     **upvals;
    int        nupvals;
    int        cap_upvals;
    int        nlocals;
    SemaFunc  *parent;
};

struct SemaCtx {
    SemaScope *scope;
    SemaFunc  *func;
    char     **errors;
    int        n_errors;
    int        cap_errors;
};

/* ── Error collection ───────────────────────────────────────────────────── */

static void sema_error(SemaCtx *ctx, int line, const char *fmt, ...) {
    va_list ap;
    char msg[1024];
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    char full[1200];
    if (line > 0) snprintf(full, sizeof(full), "line %d: %s", line, msg);
    else snprintf(full, sizeof(full), "%s", msg);

    if (ctx->n_errors + 1 >= ctx->cap_errors) {
        ctx->cap_errors = ctx->cap_errors ? ctx->cap_errors * 2 : 8;
        ctx->errors = xrealloc(ctx->errors, (size_t)ctx->cap_errors * sizeof(char *));
    }
    ctx->errors[ctx->n_errors++] = xstrdup(full);
}

/* ── Scope stack helpers ────────────────────────────────────────────────── */

static SemaScope *scope_push(SemaCtx *ctx, SemaFunc *owner) {
    SemaScope *scope = xmalloc(sizeof(*scope));
    memset(scope, 0, sizeof(*scope));
    scope->owner = owner;
    scope->parent = ctx->scope;
    ctx->scope = scope;
    return scope;
}

static void scope_pop(SemaCtx *ctx) {
    SemaScope *scope = ctx->scope;
    if (!scope) return;
    ctx->scope = scope->parent;
    free(scope->bindings);
    free(scope);
}

static int scope_find_current(SemaScope *scope, const char *name) {
    for (int i = 0; i < scope->len; i++) {
        if (!strcmp(scope->bindings[i].name, name))
            return i;
    }
    return -1;
}

static int func_add_upval(SemaFunc *func, const char *name) {
    for (int i = 0; i < func->nupvals; i++) {
        if (!strcmp(func->upvals[i], name))
            return i;
    }
    if (func->nupvals + 1 >= func->cap_upvals) {
        func->cap_upvals = func->cap_upvals ? func->cap_upvals * 2 : 4;
        func->upvals = xrealloc(func->upvals, (size_t)func->cap_upvals * sizeof(char *));
    }
    func->upvals[func->nupvals] = xstrdup(name);
    return func->nupvals++;
}

static int scope_declare(SemaCtx *ctx, const char *name) {
    SemaScope *scope = ctx->scope;
    int idx = scope_find_current(scope, name);
    if (idx >= 0)
        return scope->bindings[idx].slot;

    if (scope->len + 1 >= scope->cap) {
        scope->cap = scope->cap ? scope->cap * 2 : 8;
        scope->bindings = xrealloc(scope->bindings, (size_t)scope->cap * sizeof(ScopeBinding));
    }

    int slot = -1;
    if (scope->owner)
        slot = scope->owner->nlocals++;

    scope->bindings[scope->len].name = name;
    scope->bindings[scope->len].slot = slot;
    scope->len++;
    return slot;
}

/* ── Name resolution ────────────────────────────────────────────────────── */

static void resolve_ident_node(SemaCtx *ctx, Node *node, const char *name) {
    node->resolved.valid = 1;
    node->resolved.kind = NAME_GLOBAL;
    node->resolved.slot = -1;

    for (SemaScope *scope = ctx->scope; scope; scope = scope->parent) {
        int idx = scope_find_current(scope, name);
        if (idx < 0) continue;

        if (scope->owner == ctx->func) {
            node->resolved.kind = ctx->func ? NAME_LOCAL : NAME_GLOBAL;
            node->resolved.slot = scope->bindings[idx].slot;
            return;
        }

        if (scope->owner == NULL) {
            node->resolved.kind = NAME_GLOBAL;
            node->resolved.slot = -1;
            return;
        }

        node->resolved.kind = NAME_UPVAL;
        node->resolved.slot = ctx->func ? func_add_upval(ctx->func, name) : -1;
        return;
    }
}

static void predeclare_node_list(SemaCtx *ctx, NodeList list) {
    for (int i = 0; i < list.len; i++) {
        Node *node = list.nodes[i];
        if (node->type == N_VARDECL)
            scope_declare(ctx, node->vardecl.name);
    }
}

/* ── Call validation ────────────────────────────────────────────────────── */

static int find_param(StrList params, const char *name) {
    for (int i = 0; i < params.len; i++) {
        if (!strcmp(params.s[i], name))
            return i;
    }
    return -1;
}

static void validate_named_call_against_params(SemaCtx *ctx, Node *call, StrList params, const char *kind) {
    int argc = call->call.args.len;
    int *used = xmalloc((size_t)(params.len ? params.len : 1) * sizeof(int));
    int next_positional = 0;
    for (int i = 0; i < params.len; i++) used[i] = 0;

    for (int i = 0; i < argc; i++) {
        const char *arg_name = (call->call.arg_names.len > i) ? call->call.arg_names.s[i] : NULL;
        int slot = -1;
        if (!arg_name) {
            while (next_positional < params.len && used[next_positional])
                next_positional++;
            slot = next_positional++;
            if (slot >= params.len) {
                sema_error(ctx, call->line, "%s expected %d args but got %d", kind, params.len, argc);
                free(used);
                return;
            }
        } else {
            slot = find_param(params, arg_name);
            if (slot < 0) {
                sema_error(ctx, call->line, "%s got an unexpected named argument '%s'", kind, arg_name);
                free(used);
                return;
            }
        }

        if (used[slot]) {
            sema_error(ctx, call->line, "multiple values provided for argument '%s'", params.s[slot]);
            free(used);
            return;
        }
        used[slot] = 1;
    }

    for (int i = 0; i < params.len; i++) {
        if (!used[i]) {
            sema_error(ctx, call->line, "missing required argument '%s'", params.s[i]);
            free(used);
            return;
        }
    }

    free(used);
}

static void validate_call_node(SemaCtx *ctx, Node *node) {
    for (int i = 0; i < node->call.arg_names.len; i++) {
        const char *arg_name = node->call.arg_names.s[i];
        if (!arg_name) continue;
        for (int j = i + 1; j < node->call.arg_names.len; j++) {
            const char *other = node->call.arg_names.s[j];
            if (other && !strcmp(arg_name, other)) {
                sema_error(ctx, node->line, "duplicate named argument '%s'", arg_name);
                break;
            }
        }
    }

    if (node->call.callee->type == N_FUNC) {
        validate_named_call_against_params(ctx, node, node->call.callee->func.params, "function");
    } else if (node->call.callee->type == N_CLASS) {
        validate_named_call_against_params(ctx, node, node->call.callee->func.params, "class");
    }
}

/* ── AST traversal ──────────────────────────────────────────────────────── */

static void resolve_node(SemaCtx *ctx, Node *node);

static void resolve_node_list(SemaCtx *ctx, NodeList list) {
    for (int i = 0; i < list.len; i++)
        resolve_node(ctx, list.nodes[i]);
}

static void resolve_callable_body(SemaCtx *ctx, Node *node, StrList params, NodeList branches, const char *iter_var) {
    SemaFunc func = {0};
    func.owner_node = node;
    func.parent = ctx->func;

    SemaFunc *prev_func = ctx->func;
    ctx->func = &func;
    scope_push(ctx, &func);

    for (int i = 0; i < params.len; i++) {
        if (scope_find_current(ctx->scope, params.s[i]) >= 0)
            sema_error(ctx, node->line, "duplicate parameter '%s'", params.s[i]);
        else
            scope_declare(ctx, params.s[i]);
    }
    if (iter_var) {
        if (scope_find_current(ctx->scope, iter_var) >= 0)
            sema_error(ctx, node->line, "duplicate parameter '%s'", iter_var);
        else
            scope_declare(ctx, iter_var);
    }

    predeclare_node_list(ctx, branches);
    resolve_node_list(ctx, branches);

    node->closure_info.nlocals = func.nlocals;
    node->closure_info.nupvals = func.nupvals;
    node->closure_info.upval_names = func.upvals;

    scope_pop(ctx);
    ctx->func = prev_func;
}

static void resolve_block_like(SemaCtx *ctx, NodeList stmts) {
    scope_push(ctx, ctx->func);
    predeclare_node_list(ctx, stmts);
    resolve_node_list(ctx, stmts);
    scope_pop(ctx);
}

static void resolve_map_items(SemaCtx *ctx, MapItemList items) {
    for (int i = 0; i < items.len; i++) {
        if (items.items[i].key)
            resolve_node(ctx, items.items[i].key);
        resolve_node(ctx, items.items[i].val);
    }
}

/* Walk the AST and attach semantic metadata in place. */
static void resolve_node(SemaCtx *ctx, Node *node) {
    if (!node) return;

    switch (node->type) {
        case N_NUM:
        case N_STR:
        case N_BOOL:
        case N_NONE:
        case N_BREAK:
        case N_NEXT:
        case N_IMPORT:
        case N_WHERE:
            return;

        case N_IDENT:
            resolve_ident_node(ctx, node, node->sval);
            return;

        case N_BINOP:
            resolve_node(ctx, node->binop.left);
            resolve_node(ctx, node->binop.right);
            return;

        case N_UNARY:
            resolve_node(ctx, node->binop.left);
            return;

        case N_ASSIGN:
            resolve_node(ctx, node->assign.value);
            resolve_node(ctx, node->assign.target);
            return;

        case N_DOT:
            resolve_node(ctx, node->dot.obj);
            return;

        case N_CALL:
            resolve_node(ctx, node->call.callee);
            for (int i = 0; i < node->call.args.len; i++)
                resolve_node(ctx, node->call.args.nodes[i]);
            validate_call_node(ctx, node);
            return;

        case N_SLICE:
            resolve_node(ctx, node->slice.col);
            resolve_node(ctx, node->slice.start);
            resolve_node(ctx, node->slice.stop);
            return;

        case N_FUNC:
        case N_CLASS:
            resolve_callable_body(ctx, node, node->func.params, node->func.branches, NULL);
            return;

        case N_FORLOOP:
            resolve_node(ctx, node->forloop.start);
            resolve_node(ctx, node->forloop.stop);
            resolve_callable_body(ctx, node, (StrList){0}, node->forloop.branches, node->forloop.iter_var);
            return;

        case N_MAP_LIT:
            resolve_map_items(ctx, node->map_lit.items);
            return;

        case N_BLOCK:
            resolve_block_like(ctx, node->block.stmts);
            return;

        case N_BRANCH:
            resolve_node(ctx, node->branch.cond);
            resolve_node(ctx, node->branch.body);
            return;

        case N_CATCH:
            resolve_node(ctx, node->catch_node.expr);
            resolve_node(ctx, node->catch_node.handler);
            return;

        case N_VARDECL:
            resolve_node(ctx, node->vardecl.value);
            return;

        case N_EXPRSTMT:
            return;

        case N_PROG:
            resolve_block_like(ctx, node->prog.stmts);
            return;
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

SemaCtx *sema_new(void) {
    SemaCtx *ctx = xmalloc(sizeof(*ctx));
    memset(ctx, 0, sizeof(*ctx));
    return ctx;
}

void sema_resolve(Node *prog, SemaCtx *ctx) {
    if (!prog || !ctx) return;
    resolve_node(ctx, prog);
}

int sema_ok(const SemaCtx *ctx) {
    return ctx && ctx->n_errors == 0;
}

int sema_error_count(const SemaCtx *ctx) {
    return ctx ? ctx->n_errors : 0;
}

void sema_print_errors(FILE *f, const SemaCtx *ctx) {
    if (!ctx) return;
    if (!f) f = stderr;
    for (int i = 0; i < ctx->n_errors; i++)
        fprintf(f, "%s\n", ctx->errors[i]);
}

void sema_free(SemaCtx *ctx) {
    if (!ctx) return;
    while (ctx->scope)
        scope_pop(ctx);
    for (int i = 0; i < ctx->n_errors; i++)
        free(ctx->errors[i]);
    free(ctx->errors);
    free(ctx);
}
