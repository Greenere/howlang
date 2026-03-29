#include "runtime_internal.h"
#include "runtime.h"

/* ── Globals owned by runtime.c ─────────────────────────────────────────── */

int     g_num_builtins    = 0;
Env    *g_globals         = NULL;
HowMap *g_module_registry = NULL;

/* ── Forward declarations ────────────────────────────────────────────────── */

static Value *tensor_binop(Value *l, Value *r, const char *op, int line);

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Interpreter core                                                            */
/* ─────────────────────────────────────────────────────────────────────────── */

/* ── Tensor arithmetic helpers ───────────────────────────────────────────── */

static void tensor_shape_str(HowTensor *t, char *buf, int bufsz) {
    int pos = 0;
    pos += snprintf(buf+pos, bufsz-pos, "{");
    for (int i = 0; i < t->ndim; i++)
        pos += snprintf(buf+pos, bufsz-pos, "%d%s", t->shape[i], i<t->ndim-1?",":"");
    snprintf(buf+pos, bufsz-pos, "}");
}

static void tensor_check_shapes(HowTensor *a, HowTensor *b, int line, const char *op) {
    int ok = (a->ndim == b->ndim);
    for (int d = 0; ok && d < a->ndim; d++)
        ok = (a->shape[d] == b->shape[d]);
    if (!ok) {
        char sa[64], sb[64];
        tensor_shape_str(a, sa, sizeof(sa));
        tensor_shape_str(b, sb, sizeof(sb));
        die_at(line, 0, "tensor shapes %s and %s incompatible for '%s'", sa, sb, op);
    }
}

static Value *tensor_elemwise(HowTensor *a, HowTensor *b, const char *op, int line) {
    tensor_check_shapes(a, b, line, op);
    HowTensor *out = tensor_new(a->ndim, a->shape);
    for (int i = 0; i < a->nelem; i++) {
        double av = a->data[i], bv = b->data[i];
        if      (!strcmp(op,"+")) out->data[i] = av + bv;
        else if (!strcmp(op,"-")) out->data[i] = av - bv;
        else if (!strcmp(op,"*")) out->data[i] = av * bv;
        else if (!strcmp(op,"/")) {
            if (bv == 0) die_at(line, 0, "tensor division by zero at index %d", i);
            out->data[i] = av / bv;
        }
        else die_at(line, 0, "unsupported element-wise tensor op '%s'", op);
    }
    return val_tensor(out);
}

static Value *tensor_scale(HowTensor *t, double s, int line) {
    (void)line;
    HowTensor *out = tensor_new(t->ndim, t->shape);
    for (int i = 0; i < t->nelem; i++) out->data[i] = t->data[i] * s;
    return val_tensor(out);
}

static Value *tensor_matmul(HowTensor *a, HowTensor *b, int line) {
    if (a->ndim == 1 && b->ndim == 1) {
        if (a->shape[0] != b->shape[0])
            die_at(line, 0, "dot product: shapes {%d} and {%d} incompatible", a->shape[0], b->shape[0]);
        double s = 0;
        for (int i = 0; i < a->shape[0]; i++)
            s += a->data[i * a->strides[0]] * b->data[i * b->strides[0]];
        return val_num(s);
    }
    if (a->ndim == 2 && b->ndim == 1) {
        int m = a->shape[0], k = a->shape[1];
        if (k != b->shape[0])
            die_at(line, 0, "matmul: shapes {%d,%d} and {%d} incompatible", m, k, b->shape[0]);
        int out_shape[] = {m};
        HowTensor *out = tensor_new(1, out_shape);
        for (int i = 0; i < m; i++) {
            double s = 0;
            for (int j = 0; j < k; j++)
                s += a->data[i*a->strides[0] + j*a->strides[1]] * b->data[j*b->strides[0]];
            out->data[i] = s;
        }
        return val_tensor(out);
    }
    if (a->ndim == 2 && b->ndim == 2) {
        int m = a->shape[0], k = a->shape[1], n = b->shape[1];
        if (k != b->shape[0])
            die_at(line, 0, "matmul: shapes {%d,%d} and {%d,%d} incompatible", m, k, b->shape[0], n);
        int out_shape[] = {m, n};
        HowTensor *out = tensor_new(2, out_shape);
        for (int i = 0; i < m; i++)
            for (int j = 0; j < n; j++) {
                double s = 0;
                for (int p = 0; p < k; p++)
                    s += a->data[i*a->strides[0] + p*a->strides[1]]
                       * b->data[p*b->strides[0] + j*b->strides[1]];
                out->data[i*n+j] = s;
            }
        return val_tensor(out);
    }
    die_at(line, 0, "matmul: unsupported %dD @ %dD", a->ndim, b->ndim);
    return val_none();
}

static Value *tensor_binop(Value *l, Value *r, const char *op, int line) {
    /* @ — matmul */
    if (!strcmp(op, "@")) {
        if (l->type != VT_TENSOR || r->type != VT_TENSOR)
            die_at(line, 0, "@ requires two tensors");
        return tensor_matmul(l->tensor, r->tensor, line);
    }
    /* tensor OP tensor — element-wise */
    if (l->type == VT_TENSOR && r->type == VT_TENSOR) {
        if (!strcmp(op,"+")||!strcmp(op,"-")||!strcmp(op,"*")||!strcmp(op,"/"))
            return tensor_elemwise(l->tensor, r->tensor, op, line);
        die_at(line, 0, "unsupported tensor-tensor op '%s'", op);
    }
    /* tensor * scalar  or  scalar * tensor */
    if (!strcmp(op,"*")) {
        if (l->type==VT_TENSOR && r->type==VT_NUM) return tensor_scale(l->tensor, r->nval, line);
        if (l->type==VT_NUM && r->type==VT_TENSOR) return tensor_scale(r->tensor, l->nval, line);
    }
    /* tensor / scalar */
    if (!strcmp(op,"/") && l->type==VT_TENSOR && r->type==VT_NUM) {
        if (r->nval == 0) die_at(line, 0, "tensor division by zero");
        return tensor_scale(l->tensor, 1.0/r->nval, line);
    }
    /* tensor +/- scalar  or  scalar +/- tensor */
    if (!strcmp(op,"+") || !strcmp(op,"-")) {
        if (l->type==VT_TENSOR && r->type==VT_NUM) {
            double s = !strcmp(op,"+") ? r->nval : -r->nval;
            HowTensor *out = tensor_new(l->tensor->ndim, l->tensor->shape);
            for (int i=0; i<l->tensor->nelem; i++) out->data[i] = l->tensor->data[i] + s;
            return val_tensor(out);
        }
        if (l->type==VT_NUM && r->type==VT_TENSOR) {
            HowTensor *out = tensor_new(r->tensor->ndim, r->tensor->shape);
            for (int i=0; i<r->tensor->nelem; i++)
                out->data[i] = !strcmp(op,"+") ? l->nval + r->tensor->data[i]
                                               : l->nval - r->tensor->data[i];
            return val_tensor(out);
        }
    }
    die_at(line, 0, "unsupported tensor operation '%s'", op);
    return val_none();
}

/* augmented assignment helper */
static Value *apply_augop(Value *old, Value *val, const char *op, int line) {
    GC_ROOT_VALUE(old);
    GC_ROOT_VALUE(val);
    /* Tensor augmented assignment: W -= lr * grad, etc. */
    if (old->type == VT_TENSOR) {
        if (g_tape_active)
            die_at(line, 0, "tensor operations are not supported inside grad()");
        if (!strcmp(op,"+=") || !strcmp(op,"-=") || !strcmp(op,"*=") || !strcmp(op,"/=")) {
            const char *base_op = !strcmp(op,"+=") ? "+" :
                                  !strcmp(op,"-=") ? "-" :
                                  !strcmp(op,"*=") ? "*" : "/";
            Value *res = NULL;
            if (val->type == VT_TENSOR)
                res = tensor_elemwise(old->tensor, val->tensor, base_op, line);
            else if (val->type == VT_NUM) {
                if (!strcmp(base_op,"*") || !strcmp(base_op,"/"))
                    res = tensor_scale(old->tensor,
                          !strcmp(base_op,"*") ? val->nval : 1.0/val->nval, line);
                else {
                    double s = !strcmp(base_op,"+") ? val->nval : -val->nval;
                    HowTensor *out = tensor_new(old->tensor->ndim, old->tensor->shape);
                    for (int i=0; i<out->nelem; i++) out->data[i] = old->tensor->data[i] + s;
                    res = val_tensor(out);
                }
            } else die_at(line, 0, "incompatible types for tensor %s", op);
            GC_UNROOT_VALUE(); GC_UNROOT_VALUE();
            return res;
        }
    }
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

Value *eval(Node *node, Env *env, Signal *sig) {
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
        /* Dual number arithmetic — forward-mode or tape AD */
        if (l->type==VT_DUAL || r->type==VT_DUAL) {
            if ((l->type==VT_NUM||l->type==VT_DUAL) &&
                (r->type==VT_NUM||r->type==VT_DUAL)) {
                Value *res = dual_binop(l, r, op, node->line);
                GC_UNROOT_VALUE(); GC_UNROOT_VALUE();
                val_decref(l); val_decref(r);
                return res;
            }
        }
        /* ── Tensor arithmetic ────────────────────────────────────────────── */
        if (l->type == VT_TENSOR || r->type == VT_TENSOR) {
            if (g_tape_active)
                die_at(node->line, 0, "tensor operations are not supported inside grad()");
            Value *res2 = tensor_binop(l, r, op, node->line);
            GC_UNROOT_VALUE(); GC_UNROOT_VALUE();
            val_decref(l); val_decref(r);
            return res2;
        }
        /* ── End tensor arithmetic ────────────────────────────────────────── */
        Value *res = NULL;
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
            if (v->type==VT_DUAL) {
                Value *dr;
                if (g_tape_active && IS_TAPE_VAL(v)) {
                    tape_ensure_entry();
                    TapeEntry *te = &g_tape[g_tape_len];
                    te->out_id    = g_tape_next_id;
                    strncpy(te->op, "neg", 7); te->op[7]='\0';
                    te->in_ids[0] = TAPE_ID(v); te->in_ids[1] = -1;
                    te->in_vals[0] = v->dual.val; te->in_vals[1] = 0.0;
                    g_tape_len++;
                    dr = tape_new_val(-v->dual.val);
                } else {
                    dr = val_dual(-v->dual.val, -v->dual.tan);
                }
                GC_UNROOT_VALUE(); val_decref(v); return dr;
            }
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
            } else if (obj->type == VT_TENSOR) {
                HowTensor *t = obj->tensor;
                if (t->ndim != 1)
                    die_at(node->line, 0, "use t(i)(j) = v to write into a 2D tensor row");
                if (key->type != VT_NUM)
                    die_at(node->line, 0, "tensor index must be a number");
                int idx = (int)key->nval; val_decref(key);
                if (idx < 0 || idx >= t->shape[0])
                    die_at(node->line, 0, "tensor index %d out of range (size %d)", idx, t->shape[0]);
                if (val->type != VT_NUM)
                    die_at(node->line, 0, "tensor element must be a number");
                if (!strcmp(op,"=")) {
                    t->data[idx * t->strides[0]] = val->nval;
                } else {
                    double old = t->data[idx * t->strides[0]];
                    double newv = 0.0;
                    if      (!strcmp(op,"+=")) newv = old + val->nval;
                    else if (!strcmp(op,"-=")) newv = old - val->nval;
                    else if (!strcmp(op,"*=")) newv = old * val->nval;
                    else if (!strcmp(op,"/=")) newv = old / val->nval;
                    else die_at(node->line, 0, "unknown augop on tensor element");
                    t->data[idx * t->strides[0]] = newv;
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
        const char **arg_names = node->call.arg_names.len ? (const char **)node->call.arg_names.s : NULL;
        Value *res = eval_call_val(callee, args, arg_names, argc, sig, node->line);
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
        fn->closure  = env; __atomic_fetch_add(&env->refcount, 1, __ATOMIC_RELAXED);
        fn->is_loop  = node->func.is_loop;
        fn->refcount = 1;
        /* Register fn AFTER val_new to avoid GC sweeping fn before its Value exists */
        Value *v = val_new(VT_FUNC);
        v->func = fn;
        pthread_mutex_lock(&g_alloc_mutex);
        fn->gc_next = g_all_funcs; g_all_funcs = fn; g_gc_allocations++;
        pthread_mutex_unlock(&g_alloc_mutex);
        fn->is_grad = 0;
        fn->grad_fn = NULL;
        if (node->func.grad_body) {
            Signal ginner = {SIG_NONE, NULL};
            Value *gbv = eval(node->func.grad_body, env, &ginner);
            if (ginner.type == SIG_NONE && gbv && gbv->type == VT_FUNC) {
                fn->grad_fn = gbv->func;
                fn->grad_fn->refcount++;
            }
            val_decref(gbv);
        }
        return v;
    }

    case N_CLASS: {
        HowClass *cls = xmalloc(sizeof(*cls));
        memset(cls, 0, sizeof(*cls));
        cls->params   = strlist_clone(node->func.params);
        cls->branches = node->func.branches;
        cls->closure  = env; __atomic_fetch_add(&env->refcount, 1, __ATOMIC_RELAXED);
        cls->refcount = 1;
        /* Register cls AFTER val_new to avoid GC sweeping cls before its Value exists */
        Value *v = val_new(VT_CLASS);
        v->cls = cls;
        pthread_mutex_lock(&g_alloc_mutex);
        cls->gc_next = g_all_classes; g_all_classes = cls; g_gc_allocations++;
        pthread_mutex_unlock(&g_alloc_mutex);
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
        fn->is_parallel = node->forloop.is_parallel;
        fn->iter_var    = xstrdup(node->forloop.iter_var);
        fn->fr_start    = node->forloop.start;
        fn->fr_stop     = node->forloop.stop;
        for (int _i=0; _i<node->forloop.branches.len; _i++)
            nl_push(&fn->branches, node->forloop.branches.nodes[_i]);
        fn->closure  = env;
        if (env) __atomic_fetch_add(&env->refcount, 1, __ATOMIC_RELAXED);
        fn->refcount = 1;
        Value *fv = val_new(VT_FUNC);
        fv->func = fn;
        pthread_mutex_lock(&g_alloc_mutex);
        fn->gc_next = g_all_funcs; g_all_funcs = fn; g_gc_allocations++;
        pthread_mutex_unlock(&g_alloc_mutex);
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
            Value *result = eval_call_val(handler, &err, NULL, 1, sig, node->line);
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
        how_add_import_dir(node->sval);
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

/* Execute statement */
void exec_stmt(Node *node, Env *env, Signal *sig) {
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
        how_add_import_dir(node->sval);
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
void exec_body(Node *body, Env *env, Signal *sig) {
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
void run_branches(NodeList *branches, Env *env, Signal *sig) {
    for (int i=0; i<branches->len && sig->type==SIG_NONE; i++) {
        exec_stmt(branches->nodes[i], env, sig);
    }
}



/* ─────────────────────────────────────────────────────────────────────────── */
/*  Public API                                                                  */
/* ─────────────────────────────────────────────────────────────────────────── */

Env *how_runtime_bootstrap(int argc, char **argv) {
    V_NONE_SINGLETON  = val_new(VT_NONE);  V_NONE_SINGLETON->refcount  = 999999;
    V_TRUE_SINGLETON  = val_new(VT_BOOL);  V_TRUE_SINGLETON->bval  = 1; V_TRUE_SINGLETON->refcount  = 999999;
    V_FALSE_SINGLETON = val_new(VT_BOOL);  V_FALSE_SINGLETON->bval = 0; V_FALSE_SINGLETON->refcount = 999999;
    g_globals = env_new(NULL);
    setup_globals(g_globals);
    g_num_builtins = g_globals->len;
    g_module_registry = map_new();
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
    how_add_import_dir(".");
    return g_globals;
}

Env *how_globals(void) { return g_globals; }
int how_num_builtins(void) { return g_num_builtins; }

void how_runtime_shutdown(void) {
    gc_clear_root_stacks();
    if (g_globals) {
        GC_ROOT_ENV(g_globals);
        gc_collect(g_globals);
        GC_UNROOT_ENV();
    }
    g_globals = NULL;
    gc_clear_root_stacks();
    gc_collect(NULL);
}

void how_run_source(const char *name, const char *src, Env *env) {
    how_set_source_context(name, src);
    Node *prog = how_parse_source(src);
    Signal sig = {SIG_NONE, NULL};
    for (int i=0;i<prog->prog.stmts.len;i++) {
        exec_stmt(prog->prog.stmts.nodes[i], env, &sig);
        if (sig.type==SIG_ERROR) {
            char *s = sig.retval ? val_repr(sig.retval) : xstrdup("unknown error");
            if (sig.retval) val_decref(sig.retval);
            sig.retval = NULL; sig.type = SIG_NONE;
            if (how_repl_is_active()) {
                how_repl_set_errorf("Unhandled error: %s", s);
                free(s);
                how_repl_longjmp();
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
