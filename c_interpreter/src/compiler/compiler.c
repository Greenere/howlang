/*
 * compiler.c — compile a small core subset of Howlang AST nodes into bytecode.
 *
 * Current supported subset:
 *   - literals and global identifiers
 *   - arithmetic / comparison unary+binary operators
 *   - global `var` declarations and global assignment
 *   - positional calls
 *   - top-level blocks / programs
 *
 * This is intentionally narrow. The goal is to establish the bytecode and VM
 * path without claiming full-language parity yet.
 */
#include "compiler.h"
#include "common.h"

typedef struct {
    Proto  *proto;
    SemaCtx *sema;
    char   *error;
} CompileCtx;

static void compile_error(CompileCtx *ctx, int line, const char *fmt, ...) {
    if (ctx->error) return;
    va_list ap;
    char msg[1024];
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    char full[1200];
    if (line > 0) snprintf(full, sizeof(full), "line %d: %s", line, msg);
    else snprintf(full, sizeof(full), "%s", msg);
    ctx->error = xstrdup(full);
}

Proto *proto_new(const char *name) {
    Proto *proto = xmalloc(sizeof(*proto));
    memset(proto, 0, sizeof(*proto));
    proto->name = xstrdup(name ? name : "<top>");
    return proto;
}

void proto_free(Proto *proto) {
    if (!proto) return;
    for (int i = 0; i < proto->nconsts; i++)
        val_decref(proto->consts[i]);
    free(proto->consts);
    free(proto->ast_nodes);
    free(proto->code);
    free(proto->lines);
    free(proto->name);
    free(proto);
}

int proto_add_const(Proto *proto, Value *v) {
    if (proto->nconsts + 1 >= proto->const_cap) {
        proto->const_cap = proto->const_cap ? proto->const_cap * 2 : 16;
        proto->consts = xrealloc(proto->consts, (size_t)proto->const_cap * sizeof(Value *));
    }
    proto->consts[proto->nconsts] = val_incref(v);
    return proto->nconsts++;
}

int proto_add_const_string(Proto *proto, const char *s) {
    Value *v = val_str(s);
    int idx = proto_add_const(proto, v);
    val_decref(v);
    return idx;
}

int proto_add_ast(Proto *proto, Node *node) {
    if (proto->nast_nodes + 1 >= proto->ast_cap) {
        proto->ast_cap = proto->ast_cap ? proto->ast_cap * 2 : 8;
        proto->ast_nodes = xrealloc(proto->ast_nodes, (size_t)proto->ast_cap * sizeof(Node *));
    }
    proto->ast_nodes[proto->nast_nodes] = node;
    return proto->nast_nodes++;
}

void proto_emit_abc(Proto *proto, OpCode op, int a, int b, int c, int line) {
    if (proto->code_len + 1 >= proto->code_cap) {
        proto->code_cap = proto->code_cap ? proto->code_cap * 2 : 64;
        proto->code = xrealloc(proto->code, (size_t)proto->code_cap * sizeof(uint32_t));
        proto->lines = xrealloc(proto->lines, (size_t)proto->code_cap * sizeof(int));
    }
    proto->code[proto->code_len] = BC_ENCODE(op, a, b, c);
    proto->lines[proto->code_len] = line;
    proto->code_len++;
}

int proto_emit_jump(Proto *proto, OpCode op, int line) {
    int at = proto->code_len;
    proto_emit_abc(proto, op, 0, 0, 0, line);
    return at;
}

void proto_patch_jump(Proto *proto, int at, int target) {
    int offset = target - at - 1;
    if (offset < 0 || offset > 65535)
        die("jump target out of range");
    proto->code[at] = BC_ENCODE((OpCode)BC_OP(proto->code[at]), (offset >> 8) & 0xff, offset & 0xff, 0);
}

static const char *opcode_name(OpCode op) {
    switch (op) {
        case OP_LOAD_CONST: return "LOAD_CONST";
        case OP_LOAD_GLOBAL: return "LOAD_GLOBAL";
        case OP_DEFINE_GLOBAL: return "DEFINE_GLOBAL";
        case OP_STORE_GLOBAL: return "STORE_GLOBAL";
        case OP_MAKE_FUNC_AST: return "MAKE_FUNC_AST";
        case OP_ADD: return "ADD";
        case OP_SUB: return "SUB";
        case OP_MUL: return "MUL";
        case OP_DIV: return "DIV";
        case OP_MOD: return "MOD";
        case OP_NEG: return "NEG";
        case OP_EQ: return "EQ";
        case OP_NE: return "NE";
        case OP_LT: return "LT";
        case OP_LE: return "LE";
        case OP_GT: return "GT";
        case OP_GE: return "GE";
        case OP_NOT: return "NOT";
        case OP_JUMP: return "JUMP";
        case OP_JUMP_IF_FALSE: return "JUMP_IF_FALSE";
        case OP_CALL: return "CALL";
        case OP_POP: return "POP";
        case OP_RETURN: return "RETURN";
        case OP_RETURN_NONE: return "RETURN_NONE";
        default: return "UNKNOWN";
    }
}

void proto_disassemble(FILE *out, const Proto *proto) {
    if (!out) out = stdout;
    fprintf(out, "== %s ==\n", proto->name ? proto->name : "<proto>");
    for (int i = 0; i < proto->code_len; i++) {
        uint32_t ins = proto->code[i];
        OpCode op = (OpCode)BC_OP(ins);
        int a = (int)BC_A(ins), b = (int)BC_B(ins), c = (int)BC_C(ins);
        fprintf(out, "%04d  line %-4d %-14s %3d %3d %3d", i, proto->lines[i], opcode_name(op), a, b, c);
        if ((op == OP_LOAD_CONST || op == OP_LOAD_GLOBAL || op == OP_DEFINE_GLOBAL || op == OP_STORE_GLOBAL) &&
            a < proto->nconsts) {
            char *repr = val_repr(proto->consts[a]);
            fprintf(out, "    ; %s", repr);
            free(repr);
        } else if (op == OP_MAKE_FUNC_AST && a < proto->nast_nodes && proto->ast_nodes[a]) {
            fprintf(out, "    ; <func ast>");
        }
        fprintf(out, "\n");
    }
}

static void compile_node(CompileCtx *ctx, Node *node);
static void compile_stmt(CompileCtx *ctx, Node *node);

static void compile_const_value(CompileCtx *ctx, Node *node) {
    int idx = -1;
    switch (node->type) {
        case N_NUM: {
            Value *v = val_num(node->nval);
            idx = proto_add_const(ctx->proto, v);
            val_decref(v);
            break;
        }
        case N_STR: {
            idx = proto_add_const_string(ctx->proto, node->sval);
            break;
        }
        case N_BOOL: {
            Value *v = val_bool(node->bval);
            idx = proto_add_const(ctx->proto, v);
            val_decref(v);
            break;
        }
        case N_NONE: {
            Value *v = val_none();
            idx = proto_add_const(ctx->proto, v);
            val_decref(v);
            break;
        }
        default:
            compile_error(ctx, node->line, "internal compiler error: node is not a literal");
            return;
    }
    proto_emit_abc(ctx->proto, OP_LOAD_CONST, idx, 0, 0, node->line);
}

static void compile_ident(CompileCtx *ctx, Node *node) {
    if (!node->resolved.valid || node->resolved.kind == NAME_GLOBAL) {
        int idx = proto_add_const_string(ctx->proto, node->sval);
        proto_emit_abc(ctx->proto, OP_LOAD_GLOBAL, idx, 0, 0, node->line);
        return;
    }
    compile_error(ctx, node->line, "compiler subset does not support local or upvalue loads yet");
}

static void compile_func_literal(CompileCtx *ctx, Node *node) {
    int idx = proto_add_ast(ctx->proto, node);
    proto_emit_abc(ctx->proto, OP_MAKE_FUNC_AST, idx, 0, 0, node->line);
}

static void compile_call(CompileCtx *ctx, Node *node) {
    for (int i = 0; i < node->call.arg_names.len; i++) {
        if (node->call.arg_names.s[i]) {
            compile_error(ctx, node->line, "compiler subset does not support named calls yet");
            return;
        }
    }
    compile_node(ctx, node->call.callee);
    if (ctx->error) return;
    for (int i = 0; i < node->call.args.len; i++) {
        compile_node(ctx, node->call.args.nodes[i]);
        if (ctx->error) return;
    }
    proto_emit_abc(ctx->proto, OP_CALL, node->call.args.len, 0, 0, node->line);
}

static void compile_binop(CompileCtx *ctx, Node *node) {
    const char *op = node->binop.op;
    if (!strcmp(op, "and") || !strcmp(op, "or")) {
        compile_error(ctx, node->line, "compiler subset does not support '%s' yet", op);
        return;
    }
    compile_node(ctx, node->binop.left);
    if (ctx->error) return;
    compile_node(ctx, node->binop.right);
    if (ctx->error) return;

    if (!strcmp(op, "+")) proto_emit_abc(ctx->proto, OP_ADD, 0, 0, 0, node->line);
    else if (!strcmp(op, "-")) proto_emit_abc(ctx->proto, OP_SUB, 0, 0, 0, node->line);
    else if (!strcmp(op, "*")) proto_emit_abc(ctx->proto, OP_MUL, 0, 0, 0, node->line);
    else if (!strcmp(op, "/")) proto_emit_abc(ctx->proto, OP_DIV, 0, 0, 0, node->line);
    else if (!strcmp(op, "%")) proto_emit_abc(ctx->proto, OP_MOD, 0, 0, 0, node->line);
    else if (!strcmp(op, "==")) proto_emit_abc(ctx->proto, OP_EQ, 0, 0, 0, node->line);
    else if (!strcmp(op, "!=")) proto_emit_abc(ctx->proto, OP_NE, 0, 0, 0, node->line);
    else if (!strcmp(op, "<")) proto_emit_abc(ctx->proto, OP_LT, 0, 0, 0, node->line);
    else if (!strcmp(op, "<=")) proto_emit_abc(ctx->proto, OP_LE, 0, 0, 0, node->line);
    else if (!strcmp(op, ">")) proto_emit_abc(ctx->proto, OP_GT, 0, 0, 0, node->line);
    else if (!strcmp(op, ">=")) proto_emit_abc(ctx->proto, OP_GE, 0, 0, 0, node->line);
    else compile_error(ctx, node->line, "compiler subset does not support binary op '%s' yet", op);
}

static void compile_unary(CompileCtx *ctx, Node *node) {
    compile_node(ctx, node->binop.left);
    if (ctx->error) return;
    if (!strcmp(node->binop.op, "-")) proto_emit_abc(ctx->proto, OP_NEG, 0, 0, 0, node->line);
    else if (!strcmp(node->binop.op, "not")) proto_emit_abc(ctx->proto, OP_NOT, 0, 0, 0, node->line);
    else compile_error(ctx, node->line, "compiler subset does not support unary op '%s' yet", node->binop.op);
}

static void compile_assign(CompileCtx *ctx, Node *node) {
    if (strcmp(node->assign.op, "=")) {
        compile_error(ctx, node->line, "compiler subset does not support augmented assignment yet");
        return;
    }
    if (node->assign.target->type != N_IDENT) {
        compile_error(ctx, node->line, "compiler subset only supports assignment to identifiers");
        return;
    }
    if (node->assign.target->resolved.valid && node->assign.target->resolved.kind != NAME_GLOBAL) {
        compile_error(ctx, node->line, "compiler subset does not support local or upvalue stores yet");
        return;
    }
    compile_node(ctx, node->assign.value);
    if (ctx->error) return;
    int idx = proto_add_const_string(ctx->proto, node->assign.target->sval);
    proto_emit_abc(ctx->proto, OP_STORE_GLOBAL, idx, 0, 0, node->line);
}

static void compile_stmt_list(CompileCtx *ctx, NodeList list, int is_top_level) {
    UNUSED(is_top_level);
    for (int i = 0; i < list.len; i++) {
        compile_stmt(ctx, list.nodes[i]);
        if (ctx->error) return;
    }
}

static void compile_stmt(CompileCtx *ctx, Node *node) {
    if (!node || ctx->error) return;

    switch (node->type) {
        case N_VARDECL:
        case N_ASSIGN:
            compile_node(ctx, node);
            return;

        case N_BLOCK:
            compile_stmt_list(ctx, node->block.stmts, 0);
            return;

        case N_BRANCH: {
            if (node->branch.cond) {
                compile_node(ctx, node->branch.cond);
                if (ctx->error) return;
                int skip = proto_emit_jump(ctx->proto, OP_JUMP_IF_FALSE, node->line);
                if (node->branch.is_throw) {
                    compile_error(ctx, node->line, "compiler subset does not support throw branches yet");
                    return;
                }
                if (node->branch.is_ret) {
                    compile_node(ctx, node->branch.body);
                    if (ctx->error) return;
                    proto_emit_abc(ctx->proto, OP_RETURN, 0, 0, 0, node->line);
                } else {
                    compile_stmt(ctx, node->branch.body);
                    if (ctx->error) return;
                }
                proto_patch_jump(ctx->proto, skip, ctx->proto->code_len);
                return;
            }
            if (node->branch.is_throw) {
                compile_error(ctx, node->line, "compiler subset does not support throw branches yet");
                return;
            }
            if (node->branch.is_ret) {
                compile_node(ctx, node->branch.body);
                if (ctx->error) return;
                proto_emit_abc(ctx->proto, OP_RETURN, 0, 0, 0, node->line);
                return;
            }
            compile_stmt(ctx, node->branch.body);
            return;
        }

        default:
            compile_node(ctx, node);
            if (ctx->error) return;
            proto_emit_abc(ctx->proto, OP_POP, 0, 0, 0, node->line);
            return;
    }
}

static void compile_node(CompileCtx *ctx, Node *node) {
    if (!node || ctx->error) return;

    switch (node->type) {
        case N_NUM:
        case N_STR:
        case N_BOOL:
        case N_NONE:
            compile_const_value(ctx, node);
            return;

        case N_IDENT:
            compile_ident(ctx, node);
            return;

        case N_BINOP:
            compile_binop(ctx, node);
            return;

        case N_UNARY:
            compile_unary(ctx, node);
            return;

        case N_CALL:
            compile_call(ctx, node);
            return;

        case N_FUNC:
            compile_func_literal(ctx, node);
            return;

        case N_VARDECL:
            compile_node(ctx, node->vardecl.value);
            if (ctx->error) return;
            proto_emit_abc(ctx->proto, OP_DEFINE_GLOBAL, proto_add_const_string(ctx->proto, node->vardecl.name), 0, 0, node->line);
            return;

        case N_ASSIGN:
            compile_assign(ctx, node);
            return;

        case N_BLOCK:
            compile_error(ctx, node->line, "internal compiler error: blocks must be compiled as statements");
            return;

        case N_BRANCH:
            compile_error(ctx, node->line, "internal compiler error: branches must be compiled as statements");
            return;

        case N_PROG:
            compile_stmt_list(ctx, node->prog.stmts, 1);
            return;

        default:
            compile_error(ctx, node->line, "compiler subset does not support this AST node yet");
            return;
    }
}

Proto *how_compile(Node *prog, SemaCtx *sema, char **error_out) {
    CompileCtx ctx = {0};
    ctx.proto = proto_new("<top>");
    ctx.sema = sema;
    UNUSED(sema);
    compile_node(&ctx, prog);
    if (!ctx.error)
        proto_emit_abc(ctx.proto, OP_RETURN_NONE, 0, 0, 0, prog ? prog->line : 0);

    if (ctx.error) {
        if (error_out) *error_out = ctx.error;
        else free(ctx.error);
        proto_free(ctx.proto);
        return NULL;
    }
    if (error_out) *error_out = NULL;
    return ctx.proto;
}
