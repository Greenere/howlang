#include "runtime_internal.h"
#include <pthread.h>

/* ── GC globals ─────────────────────────────────────────────────────────── */

Value       *g_all_values   = NULL;
HowMap      *g_all_maps     = NULL;
HowList     *g_all_lists    = NULL;
HowFunc     *g_all_funcs    = NULL;
HowClass    *g_all_classes  = NULL;
HowInstance *g_all_instances = NULL;
HowModule   *g_all_modules  = NULL;
Env         *g_all_envs     = NULL;

size_t g_gc_allocations = 0;
static size_t g_gc_collections = 0;
static int    g_gc_in_progress = 0;

pthread_mutex_t g_alloc_mutex = PTHREAD_MUTEX_INITIALIZER;
volatile int    g_gc_suspended = 0;

/* Value singletons — initialised in how_runtime_bootstrap */
Value *V_NONE_SINGLETON  = NULL;
Value *V_TRUE_SINGLETON  = NULL;
Value *V_FALSE_SINGLETON = NULL;

/* ── GC root stacks ─────────────────────────────────────────────────────── */

typedef struct { Value ***slots; int len; int cap; } GcValueRootStack;
typedef struct { Env   ***slots; int len; int cap; } GcEnvRootStack;

static GcValueRootStack g_gc_value_roots = {0};
static GcEnvRootStack   g_gc_env_roots   = {0};

void gc_push_value_root(Value **slot) {
    if (g_gc_suspended) return;
    if (g_gc_value_roots.len == g_gc_value_roots.cap) {
        g_gc_value_roots.cap = g_gc_value_roots.cap ? g_gc_value_roots.cap * 2 : 64;
        g_gc_value_roots.slots = xrealloc(g_gc_value_roots.slots,
                                           sizeof(Value**) * g_gc_value_roots.cap);
    }
    g_gc_value_roots.slots[g_gc_value_roots.len++] = slot;
}
void gc_pop_value_root(void) {
    if (g_gc_suspended) return;
    if (g_gc_value_roots.len > 0) g_gc_value_roots.len--;
}
void gc_push_env_root(Env **slot) {
    if (g_gc_suspended) return;
    if (g_gc_env_roots.len == g_gc_env_roots.cap) {
        g_gc_env_roots.cap = g_gc_env_roots.cap ? g_gc_env_roots.cap * 2 : 32;
        g_gc_env_roots.slots = xrealloc(g_gc_env_roots.slots,
                                         sizeof(Env**) * g_gc_env_roots.cap);
    }
    g_gc_env_roots.slots[g_gc_env_roots.len++] = slot;
}
void gc_pop_env_root(void) {
    if (g_gc_suspended) return;
    if (g_gc_env_roots.len > 0) g_gc_env_roots.len--;
}

/* ── Value constructors ─────────────────────────────────────────────────── */

Value *val_new(VT type) {
    Value *v = xmalloc(sizeof(Value));
    memset(v, 0, sizeof(*v));
    v->type = type;
    v->refcount = 1;
    pthread_mutex_lock(&g_alloc_mutex);
    v->gc_next = g_all_values;
    g_all_values = v;
    g_gc_allocations++;
    pthread_mutex_unlock(&g_alloc_mutex);
    return v;
}

Value *val_none(void)          { V_NONE_SINGLETON->refcount++;  return V_NONE_SINGLETON; }
Value *val_bool(int b)         { Value *v = b ? V_TRUE_SINGLETON : V_FALSE_SINGLETON; v->refcount++; return v; }
Value *val_num(double d)       { Value *v = val_new(VT_NUM); v->nval = d; return v; }
Value *val_str(const char *s)  { Value *v = val_new(VT_STR); v->sval = xstrdup(s); return v; }
Value *val_str_own(char *s)    { Value *v = val_new(VT_STR); v->sval = s; return v; }

HowMap *map_new(void) {
    HowMap *m = xmalloc(sizeof(*m));
    m->pairs = NULL; m->len = m->cap = 0; m->refcount = 1; m->gc_mark = 0;
    pthread_mutex_lock(&g_alloc_mutex);
    m->gc_next = g_all_maps; g_all_maps = m; g_gc_allocations++;
    pthread_mutex_unlock(&g_alloc_mutex);
    return m;
}
Value *val_map(HowMap *m) { Value *v = val_new(VT_MAP); v->map = m; m->refcount++; return v; }

HowList *list_new(void) {
    HowList *l = xmalloc(sizeof(*l));
    l->items = NULL; l->len = l->cap = 0; l->refcount = 1; l->gc_mark = 0;
    pthread_mutex_lock(&g_alloc_mutex);
    l->gc_next = g_all_lists; g_all_lists = l; g_gc_allocations++;
    pthread_mutex_unlock(&g_alloc_mutex);
    return l;
}
Value *val_list(HowList *l) { Value *v = val_new(VT_LIST); v->list = l; l->refcount++; return v; }

/* ── Reference counting ──────────────────────────────────────────────────── */

void val_decref(Value *v)         { if (v) __atomic_fetch_sub(&v->refcount, 1, __ATOMIC_RELAXED); }
Value *val_incref(Value *v)       { if (v) __atomic_fetch_add(&v->refcount, 1, __ATOMIC_RELAXED); return v; }
void map_decref(HowMap *m)        { if (m) __atomic_fetch_sub(&m->refcount, 1, __ATOMIC_RELAXED); }
void list_decref(HowList *l)      { if (l) __atomic_fetch_sub(&l->refcount, 1, __ATOMIC_RELAXED); }
void func_decref(HowFunc *f)      { if (f) __atomic_fetch_sub(&f->refcount, 1, __ATOMIC_RELAXED); }
void cls_decref(HowClass *c)      { if (c) __atomic_fetch_sub(&c->refcount, 1, __ATOMIC_RELAXED); }
void inst_decref(HowInstance *i)  { if (i) __atomic_fetch_sub(&i->refcount, 1, __ATOMIC_RELAXED); }
void mod_decref(HowModule *m)     { if (m) __atomic_fetch_sub(&m->refcount, 1, __ATOMIC_RELAXED); }
void env_decref(Env *e)           { if (e) __atomic_fetch_sub(&e->refcount, 1, __ATOMIC_RELAXED); }

StrList strlist_clone(StrList src) {
    StrList out = {0};
    out.len = out.cap = src.len;
    if (src.len > 0) {
        out.s = xmalloc(sizeof(char*) * src.len);
        for (int i = 0; i < src.len; i++) out.s[i] = xstrdup(src.s[i]);
    }
    return out;
}

/* ── Environment ─────────────────────────────────────────────────────────── */

Env *env_new(Env *parent) {
    Env *e = xmalloc(sizeof(*e));
    e->entries = NULL; e->len = e->cap = 0;
    e->parent = parent; if (parent) __atomic_fetch_add(&parent->refcount, 1, __ATOMIC_RELAXED);
    e->refcount = 1; e->gc_mark = 0; e->inst = NULL;
    e->is_parallel = 0;
    pthread_mutex_lock(&g_alloc_mutex);
    e->gc_next = g_all_envs; g_all_envs = e; g_gc_allocations++;
    pthread_mutex_unlock(&g_alloc_mutex);
    return e;
}

Env *inst_env_new(HowInstance *inst, Env *parent) {
    Env *e = env_new(parent);
    e->inst = inst;
    return e;
}

void env_set(Env *e, const char *key, Value *val) {
    for (int i = 0; i < e->len; i++) {
        if (!strcmp(e->entries[i].key, key)) {
            val_decref(e->entries[i].val);
            e->entries[i].val = val_incref(val);
            return;
        }
    }
    if (e->inst) {
        HowMap *fields = e->inst->fields;
        for (int i = 0; i < fields->len; i++) {
            if (!strcmp(fields->pairs[i].key, key)) {
                val_decref(fields->pairs[i].val);
                fields->pairs[i].val = val_incref(val);
                return;
            }
        }
    }
    if (e->len + 1 >= e->cap) {
        e->cap = e->cap ? e->cap * 2 : 8;
        e->entries = xrealloc(e->entries, e->cap * sizeof(EnvEntry));
    }
    e->entries[e->len].key = xstrdup(key);
    e->entries[e->len].val = val_incref(val);
    e->len++;
}

Value *env_get(Env *e, const char *key) {
    for (Env *cur = e; cur; cur = cur->parent) {
        for (int i = 0; i < cur->len; i++)
            if (!strcmp(cur->entries[i].key, key))
                return cur->entries[i].val;
        if (cur->inst) {
            HowMap *fields = cur->inst->fields;
            for (int i = 0; i < fields->len; i++)
                if (!strcmp(fields->pairs[i].key, key))
                    return fields->pairs[i].val;
        }
    }
    return NULL;
}

int env_assign(Env *e, const char *key, Value *val) {
    int crossed_parallel = 0;
    for (Env *cur = e; cur; cur = cur->parent) {
        for (int i = 0; i < cur->len; i++) {
            if (!strcmp(cur->entries[i].key, key)) {
                if (crossed_parallel)
                    die("cannot write to outer variable '%s' in a parallel loop (^{})", key);
                val_decref(cur->entries[i].val);
                cur->entries[i].val = val_incref(val);
                return 1;
            }
        }
        if (cur->inst) {
            HowMap *fields = cur->inst->fields;
            for (int i = 0; i < fields->len; i++) {
                if (!strcmp(fields->pairs[i].key, key)) {
                    if (crossed_parallel)
                        die("cannot write to outer instance field '%s' in a parallel loop (^{})", key);
                    val_decref(fields->pairs[i].val);
                    fields->pairs[i].val = val_incref(val);
                    return 1;
                }
            }
        }
        /* When leaving an is_parallel scope, any outer variable becomes off-limits */
        if (cur->is_parallel) crossed_parallel = 1;
    }
    return 0;
}

/* ── Map / list data operations ─────────────────────────────────────────── */

void map_set(HowMap *m, const char *key, Value *val) {
    for (int i = 0; i < m->len; i++) {
        if (!strcmp(m->pairs[i].key, key)) {
            val_decref(m->pairs[i].val);
            m->pairs[i].val = val_incref(val);
            return;
        }
    }
    if (m->len + 1 >= m->cap) {
        m->cap = m->cap ? m->cap * 2 : 8;
        m->pairs = xrealloc(m->pairs, m->cap * sizeof(KV));
    }
    m->pairs[m->len].key = xstrdup(key);
    m->pairs[m->len].val = val_incref(val);
    m->len++;
}

Value *map_get(HowMap *m, const char *key) {
    for (int i = 0; i < m->len; i++)
        if (!strcmp(m->pairs[i].key, key)) return m->pairs[i].val;
    return NULL;
}

int map_has(HowMap *m, const char *key) {
    for (int i = 0; i < m->len; i++)
        if (!strcmp(m->pairs[i].key, key)) return 1;
    return 0;
}

void map_del(HowMap *m, const char *key) {
    for (int i = 0; i < m->len; i++) {
        if (!strcmp(m->pairs[i].key, key)) {
            free(m->pairs[i].key);
            val_decref(m->pairs[i].val);
            m->pairs[i] = m->pairs[--m->len];
            return;
        }
    }
}

void list_push(HowList *l, Value *v) {
    if (l->len + 1 >= l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->items = xrealloc(l->items, l->cap * sizeof(Value*));
    }
    l->items[l->len++] = val_incref(v);
}

/* ── Value representation and predicates ────────────────────────────────── */

char *val_repr(Value *v) {
    if (!v || v->type == VT_NONE) return xstrdup("none");
    switch (v->type) {
        case VT_BOOL: return xstrdup(v->bval ? "true" : "false");
        case VT_NUM: {
            char buf[64];
            double d = v->nval;
            if (d == (long long)d) snprintf(buf, sizeof(buf), "%lld", (long long)d);
            else snprintf(buf, sizeof(buf), "%g", d);
            return xstrdup(buf);
        }
        case VT_STR: return xstrdup(v->sval);
        case VT_LIST: {
            Buf b = {0}; buf_append(&b, "[");
            for (int i = 0; i < v->list->len; i++) {
                if (i) buf_append(&b, ", ");
                char *s = val_repr(v->list->items[i]);
                buf_append(&b, s); free(s);
            }
            buf_append(&b, "]"); return buf_done(&b);
        }
        case VT_MAP: {
            Buf b = {0}; buf_append(&b, "{");
            for (int i = 0; i < v->map->len; i++) {
                if (i) buf_append(&b, ", ");
                buf_append(&b, v->map->pairs[i].key);
                buf_append(&b, ": ");
                char *s = val_repr(v->map->pairs[i].val);
                buf_append(&b, s); free(s);
            }
            buf_append(&b, "}"); return buf_done(&b);
        }
        case VT_FUNC:     return xstrdup("<function>");
        case VT_CLASS:    return xstrdup("<class>");
        case VT_INSTANCE: {
            Buf b = {0}; buf_append(&b, "<instance {");
            HowMap *f = v->inst->fields;
            for (int i = 0; i < f->len; i++) {
                if (i) buf_append(&b, ", ");
                buf_append(&b, f->pairs[i].key); buf_append(&b, ": ");
                char *s = val_repr(f->pairs[i].val);
                buf_append(&b, s); free(s);
            }
            buf_append(&b, "}>"); return buf_done(&b);
        }
        case VT_MODULE: {
            char buf[128];
            snprintf(buf, sizeof(buf), "<module:%s>", v->mod->name);
            return xstrdup(buf);
        }
        case VT_BUILTIN: return xstrdup("<builtin>");
        default:         return xstrdup("<unknown>");
    }
}

int how_truthy(Value *v) {
    if (!v || v->type == VT_NONE) return 0;
    if (v->type == VT_BOOL) return v->bval;
    if (v->type == VT_NUM)  return v->nval != 0.0;
    if (v->type == VT_STR)  return v->sval[0] != '\0';
    return 1;
}

int how_eq(Value *a, Value *b) {
    if (!a && !b) return 1;
    if (!a || !b) return 0;
    if (a->type == VT_NONE && b->type == VT_NONE) return 1;
    if (a->type == VT_NONE || b->type == VT_NONE) return 0;
    if (a->type == VT_BOOL && b->type == VT_BOOL) return a->bval == b->bval;
    if (a->type == VT_BOOL || b->type == VT_BOOL) return 0;
    if (a->type == VT_NUM  && b->type == VT_NUM)  return a->nval == b->nval;
    if (a->type == VT_STR  && b->type == VT_STR)  return !strcmp(a->sval, b->sval);
    if (a->type == VT_LIST && b->type == VT_LIST) {
        if (a->list->len != b->list->len) return 0;
        for (int i = 0; i < a->list->len; i++)
            if (!how_eq(a->list->items[i], b->list->items[i])) return 0;
        return 1;
    }
    if (a->type == VT_MAP && b->type == VT_MAP) {
        if (a->map->len != b->map->len) return 0;
        for (int i = 0; i < a->map->len; i++) {
            Value *bv = map_get(b->map, a->map->pairs[i].key);
            if (!bv || !how_eq(a->map->pairs[i].val, bv)) return 0;
        }
        return 1;
    }
    return a == b;
}

/* ── GC: mark phase ─────────────────────────────────────────────────────── */

static void gc_mark_value(Value *v);
static void gc_mark_env(Env *e);
static void gc_mark_map(HowMap *m);
static void gc_mark_list(HowList *l);
static void gc_mark_func(HowFunc *f);
static void gc_mark_class(HowClass *c);
static void gc_mark_instance(HowInstance *inst);
static void gc_mark_module(HowModule *m);

static void gc_mark_value(Value *v) {
    if (!v || v->gc_mark) return;
    v->gc_mark = 1;
    switch (v->type) {
        case VT_LIST:     gc_mark_list(v->list);     break;
        case VT_MAP:      gc_mark_map(v->map);       break;
        case VT_FUNC:     gc_mark_func(v->func);     break;
        case VT_CLASS:    gc_mark_class(v->cls);     break;
        case VT_INSTANCE: gc_mark_instance(v->inst); break;
        case VT_MODULE:   gc_mark_module(v->mod);    break;
        default: break;
    }
}
static void gc_mark_map(HowMap *m) {
    if (!m || m->gc_mark) return;
    m->gc_mark = 1;
    for (int i = 0; i < m->len; i++) gc_mark_value(m->pairs[i].val);
}
static void gc_mark_list(HowList *l) {
    if (!l || l->gc_mark) return;
    l->gc_mark = 1;
    for (int i = 0; i < l->len; i++) gc_mark_value(l->items[i]);
}
static void gc_mark_func(HowFunc *f) {
    if (!f || f->gc_mark) return;
    f->gc_mark = 1;
    gc_mark_env(f->closure);
}
static void gc_mark_class(HowClass *c) {
    if (!c || c->gc_mark) return;
    c->gc_mark = 1;
    gc_mark_env(c->closure);
}
static void gc_mark_instance(HowInstance *inst) {
    if (!inst || inst->gc_mark) return;
    inst->gc_mark = 1;
    gc_mark_map(inst->fields);
    gc_mark_env(inst->inst_env);
}
static void gc_mark_module(HowModule *m) {
    if (!m || m->gc_mark) return;
    m->gc_mark = 1;
    gc_mark_env(m->env);
}
static void gc_mark_env(Env *e) {
    if (!e || e->gc_mark) return;
    e->gc_mark = 1;
    gc_mark_env(e->parent);
    if (e->inst) gc_mark_instance(e->inst);
    for (int i = 0; i < e->len; i++) gc_mark_value(e->entries[i].val);
}

/* ── GC: sweep phase ─────────────────────────────────────────────────────── */

static void gc_sweep_values(void) {
    Value **pp = &g_all_values;
    while (*pp) {
        Value *v = *pp;
        if (v->gc_mark) { v->gc_mark = 0; pp = &v->gc_next; continue; }
        *pp = v->gc_next;
        if (v == V_NONE_SINGLETON)  V_NONE_SINGLETON  = NULL;
        if (v == V_TRUE_SINGLETON)  V_TRUE_SINGLETON  = NULL;
        if (v == V_FALSE_SINGLETON) V_FALSE_SINGLETON = NULL;
        if (v->type == VT_STR)     free(v->sval);
        else if (v->type == VT_BUILTIN) free(v->builtin.name);
        free(v);
    }
}
static void gc_sweep_maps(void) {
    HowMap **pp = &g_all_maps;
    while (*pp) {
        HowMap *m = *pp;
        if (m->gc_mark) { m->gc_mark = 0; pp = &m->gc_next; continue; }
        *pp = m->gc_next;
        for (int i = 0; i < m->len; i++) free(m->pairs[i].key);
        free(m->pairs); free(m);
    }
}
static void gc_sweep_lists(void) {
    HowList **pp = &g_all_lists;
    while (*pp) {
        HowList *l = *pp;
        if (l->gc_mark) { l->gc_mark = 0; pp = &l->gc_next; continue; }
        *pp = l->gc_next;
        free(l->items); free(l);
    }
}
static void gc_sweep_funcs(void) {
    HowFunc **pp = &g_all_funcs;
    while (*pp) {
        HowFunc *f = *pp;
        if (f->gc_mark) { f->gc_mark = 0; pp = &f->gc_next; continue; }
        *pp = f->gc_next;
        for (int i = 0; i < f->params.len; i++) free(f->params.s[i]);
        free(f->params.s);
        if (f->iter_var) free(f->iter_var);
        free(f);
    }
}
static void gc_sweep_classes(void) {
    HowClass **pp = &g_all_classes;
    while (*pp) {
        HowClass *c = *pp;
        if (c->gc_mark) { c->gc_mark = 0; pp = &c->gc_next; continue; }
        *pp = c->gc_next;
        for (int i = 0; i < c->params.len; i++) free(c->params.s[i]);
        free(c->params.s); free(c);
    }
}
static void gc_sweep_instances(void) {
    HowInstance **pp = &g_all_instances;
    while (*pp) {
        HowInstance *inst = *pp;
        if (inst->gc_mark) { inst->gc_mark = 0; pp = &inst->gc_next; continue; }
        *pp = inst->gc_next;
        free(inst);
    }
}
static void gc_sweep_modules(void) {
    HowModule **pp = &g_all_modules;
    while (*pp) {
        HowModule *m = *pp;
        if (m->gc_mark) { m->gc_mark = 0; pp = &m->gc_next; continue; }
        *pp = m->gc_next;
        free(m->name); free(m);
    }
}
static void gc_sweep_envs(void) {
    Env **pp = &g_all_envs;
    while (*pp) {
        Env *e = *pp;
        if (e->gc_mark) { e->gc_mark = 0; pp = &e->gc_next; continue; }
        *pp = e->gc_next;
        for (int i = 0; i < e->len; i++) free(e->entries[i].key);
        free(e->entries); free(e);
    }
}

/* ── GC: collect ─────────────────────────────────────────────────────────── */

static void gc_mark_root_stacks(void) {
    for (int i = 0; i < g_gc_value_roots.len; i++) {
        Value **slot = g_gc_value_roots.slots[i];
        if (slot && *slot) gc_mark_value(*slot);
    }
    for (int i = 0; i < g_gc_env_roots.len; i++) {
        Env **slot = g_gc_env_roots.slots[i];
        if (slot && *slot) gc_mark_env(*slot);
    }
}

void gc_clear_root_stacks(void) {
    g_gc_value_roots.len = 0;
    g_gc_env_roots.len   = 0;
}

void gc_collect(Env *root_env) {
    if (g_gc_suspended) return;
    if (g_gc_in_progress) return;
    g_gc_in_progress = 1;
    if (V_NONE_SINGLETON)  gc_mark_value(V_NONE_SINGLETON);
    if (V_TRUE_SINGLETON)  gc_mark_value(V_TRUE_SINGLETON);
    if (V_FALSE_SINGLETON) gc_mark_value(V_FALSE_SINGLETON);
    gc_mark_root_stacks();
    gc_mark_env(root_env);
    if (g_globals && g_globals != root_env) gc_mark_env(g_globals);
    if (g_module_registry) gc_mark_map(g_module_registry);
    gc_sweep_values();
    gc_sweep_maps();
    gc_sweep_lists();
    gc_sweep_funcs();
    gc_sweep_classes();
    gc_sweep_instances();
    gc_sweep_modules();
    gc_sweep_envs();
    g_gc_collections++;
    g_gc_in_progress = 0;
}
