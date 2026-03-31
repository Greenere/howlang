/*
 * vm.c — tiny stack VM for the initial compiler subset.
 */
#include "compiler/vm.h"

#define VM_STACK_MAX 1024

struct VM {
    Value *stack[VM_STACK_MAX];
    int    sp;
    Env   *globals;
};

static Value *vm_make_func_from_ast(Node *node, Env *closure) {
    HowFunc *fn = xmalloc(sizeof(*fn));
    memset(fn, 0, sizeof(*fn));
    fn->params = strlist_clone(node->func.params);
    fn->branches = node->func.branches;
    fn->closure = closure;
    if (closure) __atomic_fetch_add(&closure->refcount, 1, __ATOMIC_RELAXED);
    fn->is_loop = node->func.is_loop;
    fn->refcount = 1;
    fn->is_grad = 0;
    fn->grad_fn = NULL;

    Value *v = val_new(VT_FUNC);
    v->func = fn;
    pthread_mutex_lock(&g_alloc_mutex);
    fn->gc_next = g_all_funcs;
    g_all_funcs = fn;
    g_gc_allocations++;
    pthread_mutex_unlock(&g_alloc_mutex);
    return v;
}

static void vm_push(VM *vm, Value *v) {
    if (vm->sp >= VM_STACK_MAX)
        die("vm stack overflow");
    vm->stack[vm->sp++] = v;
}

static Value *vm_pop(VM *vm) {
    if (vm->sp <= 0)
        die("vm stack underflow");
    return vm->stack[--vm->sp];
}

VM *vm_new(Env *globals) {
    VM *vm = xmalloc(sizeof(*vm));
    memset(vm, 0, sizeof(*vm));
    vm->globals = globals;
    return vm;
}

void vm_free(VM *vm) {
    if (!vm) return;
    while (vm->sp > 0)
        val_decref(vm_pop(vm));
    free(vm);
}

static Value *binary_numeric(VM *vm, const char *op, int line) {
    Value *right = vm_pop(vm);
    Value *left = vm_pop(vm);
    GC_ROOT_VALUE(left);
    GC_ROOT_VALUE(right);
    Value *res = NULL;

    if (!strcmp(op, "+")) {
        if (left->type == VT_STR || right->type == VT_STR) {
            char *a = val_repr(left), *b = val_repr(right);
            Buf buf = {0};
            buf_append(&buf, a);
            buf_append(&buf, b);
            free(a);
            free(b);
            res = val_str_own(buf_done(&buf));
        } else {
            if (left->type != VT_NUM || right->type != VT_NUM)
                die_at(line, 0, "'+' requires numbers");
            res = val_num(left->nval + right->nval);
        }
    } else if (!strcmp(op, "-")) {
        if (left->type != VT_NUM || right->type != VT_NUM) die_at(line, 0, "'-' requires numbers");
        res = val_num(left->nval - right->nval);
    } else if (!strcmp(op, "*")) {
        if (left->type != VT_NUM || right->type != VT_NUM) die_at(line, 0, "'*' requires numbers");
        res = val_num(left->nval * right->nval);
    } else if (!strcmp(op, "/")) {
        if (left->type != VT_NUM || right->type != VT_NUM) die_at(line, 0, "'/' requires numbers");
        if (right->nval == 0) die_at(line, 0, "division by zero");
        res = val_num(left->nval / right->nval);
    } else if (!strcmp(op, "%")) {
        if (left->type != VT_NUM || right->type != VT_NUM) die_at(line, 0, "'%%' requires numbers");
        res = val_num(fmod(left->nval, right->nval));
    } else if (!strcmp(op, "==")) {
        res = val_bool(how_eq(left, right));
    } else if (!strcmp(op, "!=")) {
        res = val_bool(!how_eq(left, right));
    } else if (!strcmp(op, "<")) {
        if (left->type == VT_STR && right->type == VT_STR) res = val_bool(strcmp(left->sval, right->sval) < 0);
        else {
            if (left->type != VT_NUM || right->type != VT_NUM) die_at(line, 0, "'<' requires numbers");
            res = val_bool(left->nval < right->nval);
        }
    } else if (!strcmp(op, "<=")) {
        if (left->type == VT_STR && right->type == VT_STR) res = val_bool(strcmp(left->sval, right->sval) <= 0);
        else {
            if (left->type != VT_NUM || right->type != VT_NUM) die_at(line, 0, "'<=' requires numbers");
            res = val_bool(left->nval <= right->nval);
        }
    } else if (!strcmp(op, ">")) {
        if (left->type == VT_STR && right->type == VT_STR) res = val_bool(strcmp(left->sval, right->sval) > 0);
        else {
            if (left->type != VT_NUM || right->type != VT_NUM) die_at(line, 0, "'>' requires numbers");
            res = val_bool(left->nval > right->nval);
        }
    } else if (!strcmp(op, ">=")) {
        if (left->type == VT_STR && right->type == VT_STR) res = val_bool(strcmp(left->sval, right->sval) >= 0);
        else {
            if (left->type != VT_NUM || right->type != VT_NUM) die_at(line, 0, "'>=' requires numbers");
            res = val_bool(left->nval >= right->nval);
        }
    } else {
        die_at(line, 0, "unknown vm binary op '%s'", op);
    }

    GC_UNROOT_VALUE();
    GC_UNROOT_VALUE();
    val_decref(left);
    val_decref(right);
    return res;
}

Value *vm_run(VM *vm, const Proto *proto) {
    Signal sig = {SIG_NONE, NULL};

    for (int ip = 0; ip < proto->code_len; ip++) {
        uint32_t ins = proto->code[ip];
        OpCode op = (OpCode)BC_OP(ins);
        int a = (int)BC_A(ins);
        int b = (int)BC_B(ins);
        int line = proto->lines[ip];

        switch (op) {
            case OP_LOAD_CONST:
                vm_push(vm, val_incref(proto->consts[a]));
                break;

            case OP_LOAD_GLOBAL: {
                Value *name_v = proto->consts[a];
                Value *v = env_get(vm->globals, name_v->sval);
                if (!v) die_at(line, 0, "undefined variable '%s'", name_v->sval);
                vm_push(vm, val_incref(v));
                break;
            }

            case OP_DEFINE_GLOBAL: {
                Value *name_v = proto->consts[a];
                Value *v = vm_pop(vm);
                env_set(vm->globals, name_v->sval, v);
                val_decref(v);
                break;
            }

            case OP_STORE_GLOBAL: {
                Value *name_v = proto->consts[a];
                Value *v = vm_pop(vm);
                if (!env_assign(vm->globals, name_v->sval, v))
                    die_at(line, 0, "assignment to undeclared variable '%s'", name_v->sval);
                val_decref(v);
                break;
            }

            case OP_MAKE_FUNC_AST:
                vm_push(vm, vm_make_func_from_ast(proto->ast_nodes[a], vm->globals));
                break;

            case OP_ADD: vm_push(vm, binary_numeric(vm, "+", line)); break;
            case OP_SUB: vm_push(vm, binary_numeric(vm, "-", line)); break;
            case OP_MUL: vm_push(vm, binary_numeric(vm, "*", line)); break;
            case OP_DIV: vm_push(vm, binary_numeric(vm, "/", line)); break;
            case OP_MOD: vm_push(vm, binary_numeric(vm, "%", line)); break;
            case OP_EQ: vm_push(vm, binary_numeric(vm, "==", line)); break;
            case OP_NE: vm_push(vm, binary_numeric(vm, "!=", line)); break;
            case OP_LT: vm_push(vm, binary_numeric(vm, "<", line)); break;
            case OP_LE: vm_push(vm, binary_numeric(vm, "<=", line)); break;
            case OP_GT: vm_push(vm, binary_numeric(vm, ">", line)); break;
            case OP_GE: vm_push(vm, binary_numeric(vm, ">=", line)); break;

            case OP_NEG: {
                Value *v = vm_pop(vm);
                Value *res = NULL;
                if (v->type != VT_NUM) die_at(line, 0, "unary '-' requires a number");
                res = val_num(-v->nval);
                val_decref(v);
                vm_push(vm, res);
                break;
            }

            case OP_NOT: {
                Value *v = vm_pop(vm);
                Value *res = val_bool(!how_truthy(v));
                val_decref(v);
                vm_push(vm, res);
                break;
            }

            case OP_JUMP:
                ip += ((a << 8) | b);
                break;

            case OP_JUMP_IF_FALSE: {
                Value *v = vm_pop(vm);
                int jump = !how_truthy(v);
                val_decref(v);
                if (jump)
                    ip += ((a << 8) | b);
                break;
            }

            case OP_CALL: {
                int argc = a;
                Value **args = xmalloc((size_t)(argc ? argc : 1) * sizeof(Value *));
                for (int i = argc - 1; i >= 0; i--)
                    args[i] = vm_pop(vm);
                Value *callee = vm_pop(vm);
                Value *res = eval_call_val(callee, args, NULL, argc, &sig, line);
                val_decref(callee);
                for (int i = 0; i < argc; i++)
                    val_decref(args[i]);
                free(args);
                if (sig.type == SIG_ERROR) {
                    char *s = sig.retval ? val_repr(sig.retval) : xstrdup("unknown error");
                    if (sig.retval) val_decref(sig.retval);
                    die_at(line, 0, "Unhandled error: %s", s);
                }
                if (sig.type != SIG_NONE) die_at(line, 0, "compiler subset does not support this control-flow signal yet");
                vm_push(vm, res);
                break;
            }

            case OP_POP: {
                Value *v = vm_pop(vm);
                val_decref(v);
                break;
            }

            case OP_RETURN: {
                Value *v = vm_pop(vm);
                return v;
            }

            case OP_RETURN_NONE:
                return val_none();

            default:
                die_at(line, 0, "unknown opcode %d", (int)op);
        }
    }

    return val_none();
}
