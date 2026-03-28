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
static Value *run_parallel_loop(HowFunc *fn, Signal *sig);
static Value *instantiate_class(HowClass *cls, Value **args, int argc, Signal *sig);
static void   exec_stmt(Node *node, Env *env, Signal *sig);
void          exec_body(Node *body, Env *env, Signal *sig);
static void   exec_import(const char *modname, const char *alias, Env *env);

/* ── AD tape ─────────────────────────────────────────────────────────────── */

typedef struct {
    int    out_id;
    char   op[8];
    int    in_ids[2];
    double in_vals[2];
} TapeEntry;

typedef struct {
    Env   *env;
    int    slot;
    Value *original;
} SavedVar;

static TapeEntry *g_tape       = NULL;
static int        g_tape_len   = 0;
static int        g_tape_cap   = 0;
static double    *g_tape_vals  = NULL;
static double    *g_tape_grads = NULL;
static int        g_tape_next_id = 0;
static int        g_tape_vsize   = 0;
int               g_tape_active  = 0;  /* 1 = recording reverse-mode tape */

#define IS_TAPE_VAL(v)  ((v)->type==VT_DUAL && (v)->dual.tan < -0.5)
#define TAPE_ID(v)      ((int)(-(v)->dual.tan) - 1)

static void tape_ensure_entry(void) {
    if (g_tape_len >= g_tape_cap) {
        g_tape_cap = g_tape_cap ? g_tape_cap * 2 : 256;
        g_tape = xrealloc(g_tape, (size_t)g_tape_cap * sizeof(TapeEntry));
    }
}

static void tape_ensure_val(int id) {
    if (id >= g_tape_vsize) {
        int ns = g_tape_vsize ? g_tape_vsize * 2 : 256;
        while (ns <= id) ns *= 2;
        g_tape_vals  = xrealloc(g_tape_vals,  (size_t)ns * sizeof(double));
        g_tape_grads = xrealloc(g_tape_grads, (size_t)ns * sizeof(double));
        for (int i = g_tape_vsize; i < ns; i++) {
            g_tape_vals[i] = 0.0; g_tape_grads[i] = 0.0;
        }
        g_tape_vsize = ns;
    }
}

static Value *tape_new_val(double primal) {
    int id = g_tape_next_id++;
    tape_ensure_val(id);
    g_tape_vals[id]  = primal;
    g_tape_grads[id] = 0.0;
    return val_dual(primal, -(double)(id + 1));
}

static Value *dual_binop(Value *l, Value *r, const char *op, int line) {
    double lv = (l->type==VT_DUAL) ? l->dual.val : l->nval;
    double lt = (l->type==VT_DUAL) ? l->dual.tan : 0.0;
    double rv = (r->type==VT_DUAL) ? r->dual.val : r->nval;
    double rt = (r->type==VT_DUAL) ? r->dual.tan : 0.0;

    if (!strcmp(op,"==")) return val_bool(lv==rv);
    if (!strcmp(op,"!=")) return val_bool(lv!=rv);
    if (!strcmp(op,"<"))  return val_bool(lv<rv);
    if (!strcmp(op,">"))  return val_bool(lv>rv);
    if (!strcmp(op,"<=")) return val_bool(lv<=rv);
    if (!strcmp(op,">=")) return val_bool(lv>=rv);

    double out_v, out_t;
    if      (!strcmp(op,"+")) { out_v=lv+rv; out_t=lt+rt; }
    else if (!strcmp(op,"-")) { out_v=lv-rv; out_t=lt-rt; }
    else if (!strcmp(op,"*")) { out_v=lv*rv; out_t=lt*rv+lv*rt; }
    else if (!strcmp(op,"/")) {
        if (rv==0.0) die_at(line,0,"division by zero");
        out_v=lv/rv; out_t=(lt*rv - lv*rt)/(rv*rv);
    }
    else if (!strcmp(op,"%")) { out_v=fmod(lv,rv); out_t=lt; }
    else { die_at(line,0,"unknown op '%s' on dual",op); return val_none(); }

    if (g_tape_active) {
        tape_ensure_entry();
        TapeEntry *e = &g_tape[g_tape_len];
        e->out_id    = g_tape_next_id;
        strncpy(e->op, op, 7); e->op[7]='\0';
        e->in_ids[0] = IS_TAPE_VAL(l) ? TAPE_ID(l) : -1;
        e->in_ids[1] = IS_TAPE_VAL(r) ? TAPE_ID(r) : -1;
        e->in_vals[0] = lv; e->in_vals[1] = rv;
        g_tape_len++;
        return tape_new_val(out_v);
    }
    return val_dual(out_v, out_t);
}

static void tape_backward(void) {
    for (int i = g_tape_len - 1; i >= 0; i--) {
        TapeEntry *e = &g_tape[i];
        if (e->out_id >= g_tape_vsize) continue;
        double g = g_tape_grads[e->out_id];
        if (g == 0.0) continue;
        double lv=e->in_vals[0], rv=e->in_vals[1];
        double gl=0.0, gr=0.0;
        if      (!strcmp(e->op,"+"))  { gl=g;       gr=g; }
        else if (!strcmp(e->op,"-"))  { gl=g;       gr=-g; }
        else if (!strcmp(e->op,"*"))  { gl=g*rv;    gr=g*lv; }
        else if (!strcmp(e->op,"/"))  { gl=g/rv;    gr=-g*lv/(rv*rv); }
        else if (!strcmp(e->op,"%"))  { gl=g; }
        else if (!strcmp(e->op,"neg")){ gl=-g; }
        if (e->in_ids[0]>=0 && e->in_ids[0]<g_tape_vsize) g_tape_grads[e->in_ids[0]]+=gl;
        if (e->in_ids[1]>=0 && e->in_ids[1]<g_tape_vsize) g_tape_grads[e->in_ids[1]]+=gr;
    }
}

Value *compute_grad_closure(Value *primal_fn, Signal *sig) {
    if (primal_fn->type != VT_FUNC) return val_none();
    HowFunc *fn = primal_fn->func;

    g_tape_len = 0; g_tape_next_id = 0;
    if (g_tape_vsize > 0) memset(g_tape_grads, 0, (size_t)g_tape_vsize * sizeof(double));

    int saved_cap = 64, saved_len = 0;
    SavedVar *saved = xmalloc((size_t)saved_cap * sizeof(SavedVar));

    HowMap *vtoi_map = map_new();
    Value  *vtoi_val = val_map(vtoi_map);
    map_decref(vtoi_map);
    GC_ROOT_VALUE(vtoi_val);

    for (Env *e = fn->closure; e; e = e->parent) {
        for (int i = 0; i < e->len; i++) {
            if (e->entries[i].val->type == VT_NUM) {
                int tape_id = g_tape_next_id;
                tape_ensure_val(tape_id);
                g_tape_vals[tape_id]  = e->entries[i].val->nval;
                g_tape_grads[tape_id] = 0.0;
                g_tape_next_id++;
                if (saved_len >= saved_cap) {
                    saved_cap *= 2;
                    saved = xrealloc(saved, (size_t)saved_cap * sizeof(SavedVar));
                }
                saved[saved_len].env      = e;
                saved[saved_len].slot     = i;
                saved[saved_len].original = val_incref(e->entries[i].val);
                saved_len++;
                val_decref(e->entries[i].val);
                e->entries[i].val = val_dual(saved[saved_len-1].original->nval,
                                             -(double)(tape_id + 1));
                char id_str[32]; snprintf(id_str, sizeof(id_str), "%d", tape_id);
                Value *iv = val_str(id_str);
                map_set(vtoi_val->map, e->entries[i].key, iv);
                val_decref(iv);
            }
        }
    }

    g_tape_active = 1;
    Value *result = eval_call_val(primal_fn, NULL, 0, sig, 0);
    g_tape_active = 0;

    for (int i = 0; i < saved_len; i++) {
        val_decref(saved[i].env->entries[saved[i].slot].val);
        saved[i].env->entries[saved[i].slot].val = saved[i].original;
    }
    free(saved);

    if (sig->type != SIG_NONE || !result || !IS_TAPE_VAL(result)) {
        if (result) val_decref(result);
        GC_UNROOT_VALUE(); val_decref(vtoi_val);
        return val_none();
    }

    int out_id = TAPE_ID(result);
    val_decref(result);
    if (out_id < g_tape_vsize) g_tape_grads[out_id] = 1.0;
    tape_backward();

    HowMap *gmap = map_new();
    Value  *gval = val_map(gmap);
    map_decref(gmap);
    GC_ROOT_VALUE(gval);

    for (int i = 0; i < vtoi_val->map->len; i++) {
        int tid = atoi(vtoi_val->map->pairs[i].val->sval);
        double gv = (tid < g_tape_vsize) ? g_tape_grads[tid] : 0.0;
        Value *gn = val_num(gv);
        map_set(gval->map, vtoi_val->map->pairs[i].key, gn);
        val_decref(gn);
    }

    GC_UNROOT_VALUE();
    GC_UNROOT_VALUE();
    val_decref(vtoi_val);
    return gval;
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

/* ── Parallel for-range loop ─────────────────────────────────────────────── */

typedef struct {
    HowFunc  *fn;
    int       iter_start;   /* first local index this thread handles (0-based into range) */
    int       iter_end;     /* exclusive end */
    int       range_offset; /* actual loop variable = range_offset + local_index */
    Value   **results;      /* shared pre-allocated results array */
    int       had_error;
    char      error[512];
    int       any_return;   /* 1 if any iteration hit :: */
} ParArg;

static void *parallel_worker(void *varg) {
    ParArg  *arg = (ParArg *)varg;
    HowFunc *fn  = arg->fn;

    for (int idx = arg->iter_start; idx < arg->iter_end; idx++) {
        if (arg->had_error) break;

        /* Each iteration gets its own local scope */
        Env *local = env_new(fn->closure);
        local->is_parallel = 1;

        /* Bind the loop variable */
        Value *iv = val_num((double)(arg->range_offset + idx));
        env_set(local, fn->iter_var, iv);
        val_decref(iv);

        Signal sig = {SIG_NONE, NULL};
        run_branches(&fn->branches, local, &sig);

        /* continue — skip rest of this iteration's body */
        if (sig.type == SIG_NEXT) sig.type = SIG_NONE;

        /* :: value — store result at this iteration's slot */
        if (sig.type == SIG_RETURN && sig.retval) {
            val_decref(arg->results[idx]);
            arg->results[idx] = sig.retval;   /* transfer ownership */
            sig.retval = NULL;
            sig.type   = SIG_NONE;
            arg->any_return = 1;
        }

        /* break — not allowed in parallel loops */
        if (sig.type == SIG_BREAK) {
            snprintf(arg->error, sizeof(arg->error),
                     "RuntimeError: 'break' is not allowed in a parallel loop (^{})");
            arg->had_error = 1;
        } else if (sig.type == SIG_ERROR) {
            char *s = sig.retval ? val_repr(sig.retval) : NULL;
            snprintf(arg->error, sizeof(arg->error), "%s",
                     s ? s : "error in parallel loop body");
            if (s) free(s);
            if (sig.retval) val_decref(sig.retval);
            arg->had_error = 1;
        }

        env_decref(local);
    }
    return NULL;
}

static Value *run_parallel_loop(HowFunc *fn, Signal *sig) {
    /* Evaluate start/stop on the main thread before spawning workers */
    int start_v = 0;
    if (fn->fr_start) {
        Value *sv = eval(fn->fr_start, fn->closure, sig);
        if (sig->type != SIG_NONE) return sv;
        start_v = (int)sv->nval;
        val_decref(sv);
    }
    Value *stop_val = eval(fn->fr_stop, fn->closure, sig);
    if (sig->type != SIG_NONE) return stop_val;
    int stop_v = (int)stop_val->nval;
    val_decref(stop_val);

    int n = stop_v - start_v;
    if (n <= 0) return val_none();

    /* Pre-allocate results — all slots start as none */
    Value **results = xmalloc((size_t)n * sizeof(Value *));
    for (int i = 0; i < n; i++) results[i] = val_none();

    /* Determine thread count: min(n, number of logical CPUs) */
    int num_cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (num_cpus < 1) num_cpus = 1;
    int num_threads = n < num_cpus ? n : num_cpus;

    /* Build per-thread arguments with static work partitioning */
    ParArg *args = xmalloc((size_t)num_threads * sizeof(ParArg));
    int chunk = n / num_threads;
    int rem   = n % num_threads;
    int pos   = 0;
    for (int t = 0; t < num_threads; t++) {
        int this_chunk        = chunk + (t < rem ? 1 : 0);
        args[t].fn            = fn;
        args[t].iter_start    = pos;
        args[t].iter_end      = pos + this_chunk;
        args[t].range_offset  = start_v;
        args[t].results       = results;
        args[t].had_error     = 0;
        args[t].error[0]      = '\0';
        args[t].any_return    = 0;
        pos += this_chunk;
    }

    /* Suspend GC and launch workers */
    __atomic_store_n(&g_gc_suspended, 1, __ATOMIC_RELEASE);

    pthread_t *threads = xmalloc((size_t)num_threads * sizeof(pthread_t));
    for (int t = 0; t < num_threads; t++)
        pthread_create(&threads[t], NULL, parallel_worker, &args[t]);
    for (int t = 0; t < num_threads; t++)
        pthread_join(threads[t], NULL);

    __atomic_store_n(&g_gc_suspended, 0, __ATOMIC_RELEASE);

    /* Collect results and check for errors */
    char *first_error = NULL;
    int   any_return  = 0;
    for (int t = 0; t < num_threads; t++) {
        if (args[t].had_error && !first_error) first_error = args[t].error;
        if (args[t].any_return) any_return = 1;
    }

    free(threads);

    if (first_error) {
        /* Clean up result slots before dying */
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf), "%s", first_error);
        free(args);
        for (int i = 0; i < n; i++) val_decref(results[i]);
        free(results);
        die("%s", errbuf);
        return val_none(); /* unreachable */
    }

    free(args);

    /* If no :: was hit in any iteration, return none (like sequential loop) */
    if (!any_return) {
        for (int i = 0; i < n; i++) val_decref(results[i]);
        free(results);
        return val_none();
    }

    /* Build result list in original index order */
    HowList *out = list_new();
    for (int i = 0; i < n; i++) {
        list_push(out, results[i]);
        val_decref(results[i]);
    }
    free(results);
    Value *ret = val_list(out);
    list_decref(out);
    return ret;
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
        if (fn->is_grad) {
            Value *primal_v = env_get(fn->closure, "__primal__");
            if (!primal_v) die_at(line, 0, "grad wrapper lost its primal");

            /* Custom grad block */
            if (primal_v->type==VT_FUNC && primal_v->func->grad_fn) {
                HowFunc *cgfn = primal_v->func->grad_fn;
                Value **gargs = xmalloc((size_t)(argc + 1) * sizeof(Value*));
                for (int i=0; i<argc; i++) gargs[i] = args[i];
                Value *g_up = val_num(1.0);
                gargs[argc] = g_up;
                /* Wrap cgfn in a temporary Value (cgfn already in g_all_funcs) */
                Value *gfn_wrap = val_new(VT_FUNC);
                gfn_wrap->func = cgfn; cgfn->refcount++;
                GC_ROOT_VALUE(gfn_wrap);
                Value *gres = eval_call_val(gfn_wrap, gargs, argc+1, sig, line);
                GC_UNROOT_VALUE();
                cgfn->refcount--;
                val_decref(gfn_wrap);
                val_decref(g_up);
                free(gargs);
                return gres;
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
    pthread_mutex_lock(&g_alloc_mutex);
    mod->gc_next = g_all_modules; g_all_modules = mod; g_gc_allocations++;
    pthread_mutex_unlock(&g_alloc_mutex);

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
