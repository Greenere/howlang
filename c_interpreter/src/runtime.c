#include "runtime_internal.h"
#include "runtime.h"
#include "frontend.h"

/* ── Globals owned by runtime.c ─────────────────────────────────────────── */

int     g_num_builtins    = 0;
Env    *g_globals         = NULL;
HowMap *g_module_registry = NULL;

/* Import search paths */
char **import_dirs     = NULL;
int    import_dirs_len = 0;
static int import_dirs_cap = 0;

/* ── Import path management ─────────────────────────────────────────────── */

void how_add_import_dir(const char *d) {
    /* check duplicate */
    for (int i=0;i<import_dirs_len;i++)
        if (!strcmp(import_dirs[i],d)) return;
    if (import_dirs_len+1 >= import_dirs_cap) {
        import_dirs_cap = import_dirs_cap ? import_dirs_cap*2 : 8;
        import_dirs = xrealloc(import_dirs, import_dirs_cap*sizeof(char*));
    }
    import_dirs[import_dirs_len++] = xstrdup(d);
}

char *find_how_file(const char *name) {
    char path[4096];
    for (int i=0;i<import_dirs_len;i++) {
        snprintf(path, sizeof(path), "%s/%s.how", import_dirs[i], name);
        FILE *f = fopen(path,"r");
        if (f) { fclose(f); return xstrdup(path); }
    }
    snprintf(path, sizeof(path), "%s.how", name);
    return xstrdup(path);
}

/* ── Forward declarations ────────────────────────────────────────────────── */

static Value *eval(Node *node, Env *env, Signal *sig);
Value        *eval_call_val(Value *callee, Value **args, int argc, Signal *sig, int line);
static void   run_branches(NodeList *branches, Env *env, Signal *sig);
static void   run_loop(HowFunc *fn, Signal *sig);
static Value *instantiate_class(HowClass *cls, Value **args, int argc, Signal *sig);
static void   exec_stmt(Node *node, Env *env, Signal *sig);
void          exec_body(Node *body, Env *env, Signal *sig);
static void   exec_import(const char *modname, const char *alias, Env *env);

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

/* Evaluate a function call given a Value callee */
Value *eval_call_val(Value *callee, Value **args, int argc, Signal *sig, int line) {
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
            if (fp) { fclose(fp); how_add_import_dir(dir); added=1; }
        }
        if (!added) {
            /* Try as absolute or cwd-relative path */
            snprintf(dir, sizeof(dir), "%.*s", dirlen, modname);
            how_add_import_dir(dir);
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
        if (dslash) { dslash[1]=0; how_add_import_dir(dir); }
    }

    how_set_source_context(modname, src);
    Node *prog = how_parse_source(src);
    free(src);

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
