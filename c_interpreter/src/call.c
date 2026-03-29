/*
 * call.c — Function/class invocation: eval_call_val, run_loop, instantiate_class.
 *
 * Exports (declared in runtime_internal.h):
 *   run_loop(), instantiate_class(), eval_call_val()
 */
#include "runtime_internal.h"

/* ── Unbounded (:)= loop ─────────────────────────────────────────────────── */

void run_loop(HowFunc *fn, Signal *sig) {
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

/* ── Class instantiation ─────────────────────────────────────────────────── */

Value *instantiate_class(HowClass *cls, Value **args, int argc, Signal *sig) {
    UNUSED(sig);
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
    pthread_mutex_lock(&g_alloc_mutex);
    inst->gc_next = g_all_instances; g_all_instances = inst; g_gc_allocations++;
    pthread_mutex_unlock(&g_alloc_mutex);

    /* Re-wrap method closures so they close over inst_env */
    for (int i=0;i<fields->len;i++) {
        Value *v = fields->pairs[i].val;
        if (v && v->type==VT_FUNC) {
            HowFunc *oldfn = v->func;
            /* Grad wrappers keep their own closure (__primal__ env) —
               re-wrapping would clobber is_grad and break the primal lookup. */
            if (oldfn->is_grad) continue;
            StrList   saved_params   = strlist_clone(oldfn->params);
            NodeList  saved_branches = oldfn->branches;
            int       saved_is_loop  = oldfn->is_loop;
            HowFunc  *saved_grad_fn  = oldfn->grad_fn;
            inst_env->refcount++;
            HowFunc *newfn = xmalloc(sizeof(*newfn));
            memset(newfn, 0, sizeof(*newfn));
            newfn->params   = saved_params;
            newfn->branches = saved_branches;
            newfn->is_loop  = saved_is_loop;
            newfn->grad_fn  = saved_grad_fn;
            newfn->closure  = inst_env;
            newfn->refcount = 1;
            /* Register newfn AFTER val_new to avoid GC sweeping it before its Value exists */
            Value *nv = val_new(VT_FUNC);
            nv->func = newfn;
            pthread_mutex_lock(&g_alloc_mutex);
            newfn->gc_next = g_all_funcs; g_all_funcs = newfn; g_gc_allocations++;
            pthread_mutex_unlock(&g_alloc_mutex);
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

/* ── Function call dispatch ──────────────────────────────────────────────── */

Value *eval_call_val(Value *callee, Value **args, int argc, Signal *sig, int line) {
    if (callee->type==VT_BUILTIN) {
        return callee->builtin.fn(argc, args, callee->builtin.ctx);
    }
    if (callee->type==VT_CLASS) {
        return instantiate_class(callee->cls, args, argc, sig);
    }
    if (callee->type==VT_FUNC) {
        HowFunc *fn = callee->func;
        if (fn->is_grad) {
            Value *primal_v = env_get(fn->closure, "__primal__");
            if (!primal_v) die_at(line, 0, "grad wrapper lost its primal");

            /* Custom grad block: run tape + merge with grad block overrides */
            if (primal_v->type==VT_FUNC && primal_v->func->grad_fn) {
                return call_custom_grad(primal_v->func, args, argc, sig, line);
            }

            /* Zero-arg closure: reverse-mode */
            if (argc == 0) {
                return compute_grad_closure(primal_v, sig);
            }

            /* Single numeric arg: forward-mode */
            if (argc == 1 && (args[0]->type == VT_NUM || args[0]->type == VT_DUAL)) {
                double xv = (args[0]->type == VT_DUAL) ? args[0]->dual.val : args[0]->nval;
                double xt = (args[0]->type == VT_DUAL) ? args[0]->dual.tan : 0.0;
                Value *da = val_dual(xv, 1.0);
                GC_ROOT_VALUE(da);
                Value *res = eval_call_val(primal_v, &da, 1, sig, line);
                GC_UNROOT_VALUE(); val_decref(da);
                if (sig->type != SIG_NONE) return res;
                double gr;
                if (res->type == VT_DUAL) {
                    gr = res->dual.tan;
                } else if (res->type == VT_NUM) {
                    gr = 0.0;
                } else {
                    val_decref(res); return val_none();
                }
                val_decref(res);
                /* If input was dual (nested grad), compute d²f/dx² via second pass */
                if (args[0]->type == VT_DUAL) {
                    const double h = 1e-5;
                    Value *da2 = val_dual(xv + h, 1.0);
                    GC_ROOT_VALUE(da2);
                    Signal sig2 = {SIG_NONE, NULL};
                    Value *res2 = eval_call_val(primal_v, &da2, 1, &sig2, line);
                    GC_UNROOT_VALUE(); val_decref(da2);
                    double gr2 = (res2 && res2->type == VT_DUAL) ? res2->dual.tan :
                                 (res2 && res2->type == VT_NUM ? 0.0 : gr);
                    if (res2) val_decref(res2);
                    double dgr_dx = (gr2 - gr) / h;
                    return val_dual(gr, dgr_dx * xt);
                }
                return val_num(gr);
            }

            /* Multi-arg: list of partial derivatives */
            if (argc > 1) {
                int all_num = 1;
                for (int i=0; i<argc; i++) if (args[i]->type!=VT_NUM) { all_num=0; break; }
                if (all_num) {
                    HowList *gl = list_new();
                    for (int i=0; i<argc; i++) {
                        Value **da = xmalloc((size_t)argc * sizeof(Value*));
                        for (int j=0; j<argc; j++)
                            da[j] = (j==i) ? val_dual(args[j]->nval,1.0)
                                           : val_dual(args[j]->nval,0.0);
                        Signal inner2 = {SIG_NONE, NULL};
                        Value *res = eval_call_val(primal_v, da, argc, &inner2, line);
                        for (int j=0; j<argc; j++) val_decref(da[j]);
                        free(da);
                        if (inner2.type != SIG_NONE) {
                            if (inner2.retval) val_decref(inner2.retval);
                            list_decref(gl);
                            return res;
                        }
                        double gv = (res->type==VT_DUAL) ? res->dual.tan : 0.0;
                        val_decref(res);
                        Value *gn = val_num(gv);
                        list_push(gl, gn); val_decref(gn);
                    }
                    Value *rv = val_list(gl); list_decref(gl); return rv;
                }
            }

            return val_none();
        }
        if (fn->is_forrange && fn->is_parallel) {
            return run_parallel_loop(fn, sig);
        }
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
        if (closure) env_decref(closure);
        Value *r = sig->type==SIG_RETURN ? sig->retval : val_none();
        if (sig->type==SIG_RETURN) { sig->type=SIG_NONE; sig->retval=NULL; }
        return r;
    }
    /* Tensor: t(i) → scalar (1D) or row view (nD) */
    if (callee->type == VT_TENSOR) {
        HowTensor *t = callee->tensor;
        if (argc != 1) die_at(line, 0, "tensor call requires exactly 1 argument");
        if (args[0]->type != VT_NUM) die_at(line, 0, "tensor index must be a number");
        int i = (int)args[0]->nval;
        if (i < 0 || i >= t->shape[0])
            die_at(line, 0, "tensor index %d out of range (size %d)", i, t->shape[0]);
        if (t->ndim == 1) {
            return val_num(t->data[i * t->strides[0]]);
        }
        /* nD tensor: return (n-1)D view */
        HowTensor *view = xmalloc(sizeof(*view));
        view->ndim    = t->ndim - 1;
        view->shape   = xmalloc(view->ndim * sizeof(int));
        view->strides = xmalloc(view->ndim * sizeof(int));
        for (int d = 0; d < view->ndim; d++) {
            view->shape[d]   = t->shape[d+1];
            view->strides[d] = t->strides[d+1];
        }
        view->nelem     = 1;
        for (int d = 0; d < view->ndim; d++) view->nelem *= view->shape[d];
        view->data      = t->data + i * t->strides[0];
        view->data_base = NULL;   /* view: data is not owned */
        view->base      = t;      /* gc_mark_tensor traces base to keep parent alive */
        view->gc_mark   = 0;
        pthread_mutex_lock(&g_alloc_mutex);
        view->gc_next    = g_all_tensors;
        g_all_tensors    = view;
        g_gc_allocations++;
        pthread_mutex_unlock(&g_alloc_mutex);
        return val_tensor(view);
    }
    /* Map call: map(key) → map[key] */
    if (callee->type==VT_MAP) {
        if (argc!=1) die_at(line, 0, "map call requires exactly 1 argument");
        char *ks = val_repr(args[0]);
        Value *v = map_get(callee->map, ks); free(ks);
        if (!v) return val_none();
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
