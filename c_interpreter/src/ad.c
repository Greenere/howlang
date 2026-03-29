/*
 * ad.c — Automatic differentiation: reverse-mode tape and grad utilities.
 *
 * Exports (declared in runtime_internal.h):
 *   g_tape_active, g_tape, g_tape_len, g_tape_next_id, g_tape_vsize
 *   tape_ensure_entry(), tape_new_val(), dual_binop()
 *   compute_grad_closure(), call_custom_grad()
 */
#include "runtime_internal.h"
#include <string.h>
#include <math.h>

/* ── Tape entry and saved-variable types ─────────────────────────────────── */

typedef struct {
    Env   *env;
    int    slot;
    Value *original;
} SavedVar;

/* ── Tape globals ─────────────────────────────────────────────────────────── */

TapeEntry *g_tape         = NULL;
int        g_tape_len     = 0;
int        g_tape_cap     = 0;
double    *g_tape_vals    = NULL;
double    *g_tape_grads   = NULL;
int        g_tape_next_id = 0;
int        g_tape_vsize   = 0;
int        g_tape_active  = 0;  /* 1 = recording reverse-mode tape */

/* ── Internal helpers ─────────────────────────────────────────────────────── */

void tape_ensure_entry(void) {
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

Value *tape_new_val(double primal) {
    int id = g_tape_next_id++;
    tape_ensure_val(id);
    g_tape_vals[id]  = primal;
    g_tape_grads[id] = 0.0;
    return val_dual(primal, -(double)(id + 1));
}

Value *dual_binop(Value *l, Value *r, const char *op, int line) {
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

/* ── Public AD functions ──────────────────────────────────────────────────── */

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

/* call_custom_grad — evaluates grad(f)(args...) when f has a user-defined
 * grad block.
 *
 * Algorithm (per grad_fix_proposal.md):
 *   1. Reset tape; assign tape IDs to every numeric arg.
 *   2. Run the forward pass with tape active to record all operations.
 *   3. Seed the output gradient = 1.0 and run tape_backward().
 *   4. Call the grad block with (primals..., g=1.0).
 *   5. Build result map keyed by param name:
 *        - if grad block returned a non-none value for this param → use it
 *        - else if the arg was numeric → use tape-computed gradient
 *        - else (function param, omitted) → none
 *   6. For single-numeric-arg calls: unwrap map to plain number.
 */
Value *call_custom_grad(HowFunc *primal_fn, Value **args, int argc,
                        Signal *sig, int line) {
    HowFunc *cgfn = primal_fn->grad_fn;
    int n = argc;

    /* Higher-order grad: if any arg is a dual number (forward-mode nesting),
     * the tape cannot track dual tangents.  Fall back to calling the grad block
     * directly so dual arithmetic propagates through it correctly.
     * This is the path that makes grad(grad(f)) work for old-style grad blocks. */
    for (int i = 0; i < n; i++) {
        if (args[i]->type == VT_DUAL) {
            Value **gargs = xmalloc((size_t)(n + 1) * sizeof(Value *));
            for (int j = 0; j < n; j++) gargs[j] = args[j];
            gargs[n] = val_num(1.0);
            Value *cgfn_wrap = val_new(VT_FUNC);
            cgfn_wrap->func = cgfn; cgfn->refcount++;
            GC_ROOT_VALUE(cgfn_wrap);
            Value *gres = eval_call_val(cgfn_wrap, gargs, n + 1, sig, line);
            GC_UNROOT_VALUE(); cgfn->refcount--; val_decref(cgfn_wrap);
            val_decref(gargs[n]);
            free(gargs);
            return gres;
        }
    }

    /* Save primal values before any tape wrapping */
    Value **primals = xmalloc((size_t)n * sizeof(Value *));
    for (int i = 0; i < n; i++) primals[i] = val_incref(args[i]);

    /* Reset tape; assign tape IDs to numeric args */
    g_tape_len = 0; g_tape_next_id = 0;
    if (g_tape_vsize > 0)
        memset(g_tape_grads, 0, (size_t)g_tape_vsize * sizeof(double));

    int    *arg_ids   = xmalloc((size_t)n * sizeof(int));
    Value **tape_args = xmalloc((size_t)n * sizeof(Value *));
    for (int i = 0; i < n; i++) {
        if (args[i]->type == VT_NUM) {
            arg_ids[i]   = g_tape_next_id;
            tape_args[i] = tape_new_val(args[i]->nval);
        } else {
            arg_ids[i]   = -1;
            tape_args[i] = val_incref(args[i]);
        }
    }

    /* Forward pass with tape */
    Value *pfn_wrap = val_new(VT_FUNC);
    pfn_wrap->func = primal_fn; primal_fn->refcount++;
    GC_ROOT_VALUE(pfn_wrap);
    g_tape_active = 1;
    Signal fwd_sig = {SIG_NONE, NULL};
    Value *fwd = eval_call_val(pfn_wrap, tape_args, n, &fwd_sig, line);
    g_tape_active = 0;
    GC_UNROOT_VALUE(); primal_fn->refcount--; val_decref(pfn_wrap);
    for (int i = 0; i < n; i++) val_decref(tape_args[i]);
    free(tape_args);

    /* Seed output gradient and run backward pass */
    if (fwd_sig.type == SIG_NONE && fwd && IS_TAPE_VAL(fwd)) {
        int out_id = TAPE_ID(fwd);
        if (out_id < g_tape_vsize) g_tape_grads[out_id] = 1.0;
        tape_backward();
    }
    if (fwd_sig.retval) val_decref(fwd_sig.retval);
    if (fwd) val_decref(fwd);

    /* Call the grad block with (primals..., g=1.0) */
    Value **grad_args = xmalloc((size_t)(n + 1) * sizeof(Value *));
    for (int i = 0; i < n; i++) grad_args[i] = primals[i];
    grad_args[n] = val_num(1.0);

    Value *cgfn_wrap = val_new(VT_FUNC);
    cgfn_wrap->func = cgfn; cgfn->refcount++;
    GC_ROOT_VALUE(cgfn_wrap);
    Value *override_map = eval_call_val(cgfn_wrap, grad_args, n + 1, sig, line);
    GC_UNROOT_VALUE(); cgfn->refcount--; val_decref(cgfn_wrap);
    val_decref(grad_args[n]);
    free(grad_args);

    /* Custom grad blocks must return a keyed map of parameter overrides. */
    if (override_map && override_map->type != VT_MAP) {
        Value *bad = override_map;
        char *got = val_repr(bad);
        val_decref(bad);
        free(primals);
        free(arg_ids);
        die_at(line, 0, "grad block must return a keyed map, got %s", got);
        free(got);
    }

    /* Build result map: override wins; tape fills omitted numeric params */
    HowMap *result = map_new();
    Value  *rval   = val_map(result); map_decref(result);
    GC_ROOT_VALUE(rval);

    for (int i = 0; i < n && i < primal_fn->params.len; i++) {
        const char *pname = primal_fn->params.s[i];
        Value *override = override_map ? map_get(override_map->map, pname) : NULL;

        Value *gv;
        if (override && override->type != VT_NONE) {
            gv = val_incref(override);
        } else if (arg_ids[i] >= 0 && arg_ids[i] < g_tape_vsize) {
            gv = val_num(g_tape_grads[arg_ids[i]]);
        } else {
            gv = val_none();
        }
        map_set(rval->map, pname, gv);
        val_decref(gv);
    }

    for (int i = 0; i < n; i++) val_decref(primals[i]);
    free(primals); free(arg_ids);
    if (override_map) val_decref(override_map);
    GC_UNROOT_VALUE();

    return rval;
}
