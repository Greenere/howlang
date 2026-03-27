#include "runtime.h"
#include "frontend.h"

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Runtime values                                                              */
/* ─────────────────────────────────────────────────────────────────────────── */

typedef enum {
    VT_NONE, VT_BOOL, VT_NUM, VT_STR,
    VT_LIST, VT_MAP,
    VT_FUNC, VT_CLASS, VT_INSTANCE, VT_MODULE,
    VT_BUILTIN,
} VT;

typedef struct HowList HowList;
typedef struct HowMap  HowMap;
typedef struct HowFunc HowFunc;
typedef struct HowClass HowClass;
typedef struct HowInstance HowInstance;
typedef struct HowModule HowModule;

/* Key-value pair for maps */
typedef struct KV { char *key; Value *val; } KV;

struct HowMap {
    KV     *pairs;
    int     len;
    int     cap;
    int     refcount;
    int     gc_mark;
    struct HowMap *gc_next;
};

struct HowList {
    Value **items;
    int     len;
    int     cap;
    int     refcount;
    int     gc_mark;
    struct HowList *gc_next;
};

struct HowFunc {
    StrList  params;
    NodeList branches;
    Env     *closure;
    int      is_loop;
    /* for-range callable fields */
    int      is_forrange;
    char    *iter_var;
    Node    *fr_start;
    Node    *fr_stop;
    int      refcount;
    int      gc_mark;
    struct HowFunc *gc_next;
};

struct HowClass {
    StrList  params;
    NodeList branches;
    Env     *closure;
    int      refcount;
    int      gc_mark;
    struct HowClass *gc_next;
};

struct HowInstance {
    HowMap  *fields;  /* shared with inst_env */
    Env     *inst_env;
    int      refcount;
    int      gc_mark;
    struct HowInstance *gc_next;
};

struct HowModule {
    char *name;
    Env  *env;
    int   refcount;
    int   gc_mark;
    struct HowModule *gc_next;
};

typedef Value* (*BuiltinFn)(int argc, Value **argv, void *ctx);

struct Value {
    VT  type;
    int refcount;
    int gc_mark;
    struct Value *gc_next;
    union {
        int       bval;
        double    nval;
        char     *sval;
        HowList  *list;
        HowMap   *map;
        HowFunc  *func;
        HowClass *cls;
        HowInstance *inst;
        HowModule   *mod;
        struct { BuiltinFn fn; void *ctx; char *name; } builtin;
    };
};

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Value constructors                                                          */
/* ─────────────────────────────────────────────────────────────────────────── */

static Value *V_NONE_SINGLETON;
static Value *V_TRUE_SINGLETON;
static Value *V_FALSE_SINGLETON;

static Value       *g_all_values = NULL;
static HowMap      *g_all_maps = NULL;
static HowList     *g_all_lists = NULL;
static HowFunc     *g_all_funcs = NULL;
static HowClass    *g_all_classes = NULL;
static HowInstance *g_all_instances = NULL;
static HowModule   *g_all_modules = NULL;
static Env         *g_all_envs = NULL;
static Env         *g_globals = NULL;
static HowMap      *g_module_registry = NULL;  /* name -> module Value, for import caching */

static size_t g_gc_allocations = 0;
static size_t g_gc_collections = 0;
static int g_gc_in_progress = 0;

typedef struct { Value ***slots; int len; int cap; } GcValueRootStack;
typedef struct { Env   ***slots; int len; int cap; } GcEnvRootStack;
static GcValueRootStack g_gc_value_roots = {0};
static GcEnvRootStack g_gc_env_roots = {0};

static void gc_push_value_root(Value **slot) {
    if (g_gc_value_roots.len == g_gc_value_roots.cap) {
        g_gc_value_roots.cap = g_gc_value_roots.cap ? g_gc_value_roots.cap * 2 : 64;
        g_gc_value_roots.slots = xrealloc(g_gc_value_roots.slots, sizeof(Value**) * g_gc_value_roots.cap);
    }
    g_gc_value_roots.slots[g_gc_value_roots.len++] = slot;
}
static void gc_pop_value_root(void) {
    if (g_gc_value_roots.len > 0) g_gc_value_roots.len--;
}
static void gc_push_env_root(Env **slot) {
    if (g_gc_env_roots.len == g_gc_env_roots.cap) {
        g_gc_env_roots.cap = g_gc_env_roots.cap ? g_gc_env_roots.cap * 2 : 32;
        g_gc_env_roots.slots = xrealloc(g_gc_env_roots.slots, sizeof(Env**) * g_gc_env_roots.cap);
    }
    g_gc_env_roots.slots[g_gc_env_roots.len++] = slot;
}
static void gc_pop_env_root(void) {
    if (g_gc_env_roots.len > 0) g_gc_env_roots.len--;
}

#define GC_ROOT_VALUE(v) gc_push_value_root(&(v))
#define GC_UNROOT_VALUE() gc_pop_value_root()
#define GC_ROOT_ENV(e) gc_push_env_root(&(e))
#define GC_UNROOT_ENV() gc_pop_env_root()


static Value *val_new(VT type) {
    Value *v = xmalloc(sizeof(Value));
    memset(v, 0, sizeof(*v));
    v->type = type;
    v->refcount = 1;
    v->gc_mark = 0;
    v->gc_next = g_all_values;
    g_all_values = v;
    g_gc_allocations++;
    return v;
}

static Value *val_none(void)   { V_NONE_SINGLETON->refcount++; return V_NONE_SINGLETON; }
static Value *val_bool(int b)  { Value *v = b ? V_TRUE_SINGLETON : V_FALSE_SINGLETON; v->refcount++; return v; }
static Value *val_num(double d){ Value *v=val_new(VT_NUM); v->nval=d; return v; }
static Value *val_str(const char *s){ Value *v=val_new(VT_STR); v->sval=xstrdup(s); return v; }
static Value *val_str_own(char *s){ Value *v=val_new(VT_STR); v->sval=s; return v; }

static HowMap *map_new(void) {
    HowMap *m = xmalloc(sizeof(*m));
    m->pairs=NULL; m->len=m->cap=0; m->refcount=1;
    m->gc_mark = 0;
    m->gc_next = g_all_maps;
    g_all_maps = m;
    g_gc_allocations++;
    return m;
}

static Value *val_map(HowMap *m) {
    Value *v = val_new(VT_MAP); v->map = m; m->refcount++; return v;
}

static HowList *list_new(void) {
    HowList *l = xmalloc(sizeof(*l));
    l->items=NULL; l->len=l->cap=0; l->refcount=1;
    l->gc_mark = 0;
    l->gc_next = g_all_lists;
    g_all_lists = l;
    g_gc_allocations++;
    return l;
}

static Value *val_list(HowList *l) {
    Value *v = val_new(VT_LIST); v->list = l; l->refcount++; return v;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Reference counting (simplified — no cycle collection)                      */
/* ─────────────────────────────────────────────────────────────────────────── */

static void val_decref(Value *v);

static void map_decref(HowMap *m) {
    /* tracing GC: reclamation happens in gc_collect(), not here */
    if (!m) return;
    m->refcount--;
}


static void list_decref(HowList *l) {
    /* tracing GC: reclamation happens in gc_collect(), not here */
    if (!l) return;
    l->refcount--;
}


/* forward decl */
static void env_decref(Env *e);

static void func_decref(HowFunc *f) {
    /* tracing GC: reclamation happens in gc_collect(), not here */
    if (!f) return;
    f->refcount--;
}


static void cls_decref(HowClass *c) {
    /* tracing GC: reclamation happens in gc_collect(), not here */
    if (!c) return;
    c->refcount--;
}


static void inst_decref(HowInstance *inst) {
    /* tracing GC: reclamation happens in gc_collect(), not here */
    if (!inst) return;
    inst->refcount--;
}


static void mod_decref(HowModule *m) {
    /* tracing GC: reclamation happens in gc_collect(), not here */
    if (!m) return;
    m->refcount--;
}


static void val_decref(Value *v) {
    if (!v) return;
    v->refcount--;
}


static Value *val_incref(Value *v) { if(v) v->refcount++; return v; }

static StrList strlist_clone(StrList src) {
    StrList out = {0};
    out.len = src.len;
    out.cap = src.len;
    if (src.len > 0) {
        out.s = xmalloc(sizeof(char*) * src.len);
        for (int i = 0; i < src.len; i++) out.s[i] = xstrdup(src.s[i]);
    }
    return out;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Environment                                                                 */
/* ─────────────────────────────────────────────────────────────────────────── */

typedef struct EnvEntry { char *key; Value *val; } EnvEntry;

struct Env {
    EnvEntry *entries;
    int       len;
    int       cap;
    Env      *parent;
    int       refcount;
    int       gc_mark;
    struct Env *gc_next;
    /* for InstanceEnv */
    HowInstance *inst;  /* non-NULL if this is an InstanceEnv */
};

static Env *env_new(Env *parent) {
    Env *e = xmalloc(sizeof(*e));
    e->entries=NULL; e->len=e->cap=0;
    e->parent  = parent; if(parent) parent->refcount++;
    e->refcount=1;
    e->gc_mark = 0;
    e->gc_next = g_all_envs;
    g_all_envs = e;
    g_gc_allocations++;
    e->inst=NULL;
    return e;
}

static Env *inst_env_new(HowInstance *inst, Env *parent) {
    Env *e = env_new(parent);
    e->inst = inst;
    return e;
}

static void env_decref(Env *e) {
    /* tracing GC: reclamation happens in gc_collect(), not here */
    if (!e) return;
    e->refcount--;
}


/* set (always in local scope) */
static void env_set(Env *e, const char *key, Value *val) {
    /* check if already in local entries */
    for (int i=0;i<e->len;i++) {
        if (!strcmp(e->entries[i].key, key)) {
            val_decref(e->entries[i].val);
            e->entries[i].val = val_incref(val);
            return;
        }
    }
    /* if InstanceEnv check instance fields */
    if (e->inst) {
        HowMap *fields = e->inst->fields;
        for (int i=0;i<fields->len;i++) {
            if (!strcmp(fields->pairs[i].key, key)) {
                val_decref(fields->pairs[i].val);
                fields->pairs[i].val = val_incref(val);
                return;
            }
        }
    }
    /* add new local entry */
    if (e->len + 1 >= e->cap) {
        e->cap = e->cap ? e->cap*2 : 8;
        e->entries = xrealloc(e->entries, e->cap * sizeof(EnvEntry));
    }
    e->entries[e->len].key = xstrdup(key);
    e->entries[e->len].val = val_incref(val);
    e->len++;
}

/* get — walk up parent chain */
static Value *env_get(Env *e, const char *key) {
    for (Env *cur=e; cur; cur=cur->parent) {
        for (int i=0;i<cur->len;i++)
            if (!strcmp(cur->entries[i].key, key))
                return cur->entries[i].val;
        if (cur->inst) {
            HowMap *fields = cur->inst->fields;
            for (int i=0;i<fields->len;i++)
                if (!strcmp(fields->pairs[i].key, key))
                    return fields->pairs[i].val;
        }
    }
    return NULL;  /* undefined */
}

/* assign — walk up parent chain to mutate existing binding */
static int env_assign(Env *e, const char *key, Value *val) {
    for (Env *cur=e; cur; cur=cur->parent) {
        for (int i=0;i<cur->len;i++) {
            if (!strcmp(cur->entries[i].key, key)) {
                val_decref(cur->entries[i].val);
                cur->entries[i].val = val_incref(val);
                return 1;
            }
        }
        if (cur->inst) {
            HowMap *fields = cur->inst->fields;
            for (int i=0;i<fields->len;i++) {
                if (!strcmp(fields->pairs[i].key, key)) {
                    val_decref(fields->pairs[i].val);
                    fields->pairs[i].val = val_incref(val);
                    return 1;
                }
            }
        }
    }
    return 0; /* not found */
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Map helpers                                                                 */
/* ─────────────────────────────────────────────────────────────────────────── */

static void map_set(HowMap *m, const char *key, Value *val) {
    for (int i=0;i<m->len;i++) {
        if (!strcmp(m->pairs[i].key, key)) {
            val_decref(m->pairs[i].val);
            m->pairs[i].val = val_incref(val);
            return;
        }
    }
    if (m->len + 1 >= m->cap) {
        m->cap = m->cap ? m->cap*2 : 8;
        m->pairs = xrealloc(m->pairs, m->cap*sizeof(KV));
    }
    m->pairs[m->len].key = xstrdup(key);
    m->pairs[m->len].val = val_incref(val);
    m->len++;
}

static Value *map_get(HowMap *m, const char *key) {
    for (int i=0;i<m->len;i++)
        if (!strcmp(m->pairs[i].key, key))
            return m->pairs[i].val;
    return NULL;
}

static int map_has(HowMap *m, const char *key) {
    for (int i=0;i<m->len;i++)
        if (!strcmp(m->pairs[i].key, key)) return 1;
    return 0;
}

static void map_del(HowMap *m, const char *key) {
    for (int i=0;i<m->len;i++) {
        if (!strcmp(m->pairs[i].key, key)) {
            free(m->pairs[i].key);
            val_decref(m->pairs[i].val);
            m->pairs[i] = m->pairs[--m->len];
            return;
        }
    }
}

/* list helpers */
static void list_push(HowList *l, Value *v) {
    if (l->len + 1 >= l->cap) {
        l->cap = l->cap ? l->cap*2 : 8;
        l->items = xrealloc(l->items, l->cap*sizeof(Value*));
    }
    l->items[l->len++] = val_incref(v);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  String representation                                                       */
/* ─────────────────────────────────────────────────────────────────────────── */

static char *val_repr(Value *v) {
    if (!v || v->type==VT_NONE) return xstrdup("none");
    switch(v->type) {
        case VT_BOOL: return xstrdup(v->bval ? "true" : "false");
        case VT_NUM: {
            char buf[64];
            double d = v->nval;
            if (d == (long long)d) snprintf(buf,sizeof(buf),"%lld",(long long)d);
            else snprintf(buf,sizeof(buf),"%g",d);
            return xstrdup(buf);
        }
        case VT_STR: return xstrdup(v->sval);
        case VT_LIST: {
            Buf b = {0};
            buf_append(&b,"[");
            for (int i=0;i<v->list->len;i++) {
                if(i) buf_append(&b,", ");
                char *s = val_repr(v->list->items[i]);
                buf_append(&b,s); free(s);
            }
            buf_append(&b,"]");
            return buf_done(&b);
        }
        case VT_MAP: {
            Buf b = {0};
            buf_append(&b,"{");
            for (int i=0;i<v->map->len;i++) {
                if(i) buf_append(&b,", ");
                buf_append(&b,v->map->pairs[i].key);
                buf_append(&b,": ");
                char *s = val_repr(v->map->pairs[i].val);
                buf_append(&b,s); free(s);
            }
            buf_append(&b,"}");
            return buf_done(&b);
        }
        case VT_FUNC:     return xstrdup("<function>");
        case VT_CLASS:    return xstrdup("<class>");
        case VT_INSTANCE: {
            Buf b={0}; buf_append(&b,"<instance {");
            HowMap *f=v->inst->fields;
            for(int i=0;i<f->len;i++){
                if(i) buf_append(&b,", ");
                buf_append(&b,f->pairs[i].key);
                buf_append(&b,": ");
                char *s=val_repr(f->pairs[i].val);
                buf_append(&b,s); free(s);
            }
            buf_append(&b,"}>"); return buf_done(&b);
        }
        case VT_MODULE:   { char buf[128]; snprintf(buf,sizeof(buf),"<module:%s>",v->mod->name); return xstrdup(buf); }
        case VT_BUILTIN:  return xstrdup("<builtin>");
        default: return xstrdup("<unknown>");
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Truthiness and equality                                                     */
/* ─────────────────────────────────────────────────────────────────────────── */

static int how_truthy(Value *v) {
    if (!v || v->type==VT_NONE) return 0;
    if (v->type==VT_BOOL) return v->bval;
    if (v->type==VT_NUM)  return v->nval != 0.0;
    if (v->type==VT_STR)  return v->sval[0] != '\0';
    return 1;
}

static int how_eq(Value *a, Value *b) {
    if (!a && !b) return 1;
    if (!a || !b) return 0;
    if (a->type==VT_NONE && b->type==VT_NONE) return 1;
    if (a->type==VT_NONE || b->type==VT_NONE) return 0;
    if (a->type==VT_BOOL && b->type==VT_BOOL) return a->bval==b->bval;
    /* bool vs non-bool: different types */
    if (a->type==VT_BOOL || b->type==VT_BOOL) return 0;
    if (a->type==VT_NUM && b->type==VT_NUM) return a->nval==b->nval;
    if (a->type==VT_STR && b->type==VT_STR) return !strcmp(a->sval,b->sval);
    if (a->type==VT_LIST && b->type==VT_LIST) {
        if (a->list->len != b->list->len) return 0;
        for (int i=0;i<a->list->len;i++)
            if (!how_eq(a->list->items[i], b->list->items[i])) return 0;
        return 1;
    }
    if (a->type==VT_MAP && b->type==VT_MAP) {
        if (a->map->len != b->map->len) return 0;
        for (int i=0;i<a->map->len;i++) {
            Value *bv = map_get(b->map, a->map->pairs[i].key);
            if (!bv || !how_eq(a->map->pairs[i].val, bv)) return 0;
        }
        return 1;
    }
    return a == b;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Interpreter                                                                 */
/* ─────────────────────────────────────────────────────────────────────────── */

/* Import search paths */
int g_num_builtins = 0;  /* set after setup_globals(); only real builtins */
static char **import_dirs;
static int    import_dirs_len;
static int    import_dirs_cap;

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

static char *find_how_file(const char *name) {
    char path[4096];
    for (int i=0;i<import_dirs_len;i++) {
        snprintf(path, sizeof(path), "%s/%s.how", import_dirs[i], name);
        FILE *f = fopen(path,"r");
        if (f) { fclose(f); return xstrdup(path); }
    }
    snprintf(path, sizeof(path), "%s.how", name);
    return xstrdup(path);
}

/* Control flow signals */
typedef enum { SIG_NONE, SIG_RETURN, SIG_BREAK, SIG_NEXT, SIG_ERROR } SigType;
typedef struct { SigType type; Value *retval; } Signal;

/* Forward declarations */
static Value *eval(Node *node, Env *env, Signal *sig);
static Value *eval_call_val(Value *callee, Value **args, int argc, Signal *sig, int line);
static void run_branches(NodeList *branches, Env *env, Signal *sig);
static void run_loop(HowFunc *fn, Signal *sig);
static Value *instantiate_class(HowClass *cls, Value **args, int argc, Signal *sig);
static void exec_stmt(Node *node, Env *env, Signal *sig);
static void exec_import(const char *modname, const char *alias, Env *env);

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Builtins                                                                    */
/* ─────────────────────────────────────────────────────────────────────────── */

/* argc is the number of actual arguments passed */
#define BUILTIN(name) static Value *builtin_##name(int argc, Value **argv, void *ctx)
#define NEED(n) do{ if(argc<(n)) die("builtin requires %d args, got %d",(n),argc); }while(0)
#define ARG(i) (argc>(i) ? argv[i] : V_NONE_SINGLETON)

/* forward declaration */
static void exec_body(Node *body, Env *env, Signal *sig);

BUILTIN(print) {
    for (int i=0;i<argc;i++) {
        char *s = val_repr(argv[i]);
        if (i) printf(" ");
        printf("%s", s);
        free(s);
    }
    printf("\n");
    return val_none();
}

BUILTIN(len) {
    NEED(1);
    Value *v = ARG(0);
    if (v->type==VT_LIST) return val_num(v->list->len);
    if (v->type==VT_MAP)  return val_num(v->map->len);
    if (v->type==VT_STR)  return val_num(strlen(v->sval));
    if (v->type==VT_INSTANCE) return val_num(v->inst->fields->len);
    die("len() not supported for this type");
    return val_none();
}

BUILTIN(str_fn) {
    NEED(1);
    return val_str_own(val_repr(argv[0]));
}

BUILTIN(num_fn) {
    NEED(1);
    Value *v = ARG(0);
    if (v->type==VT_NUM) return val_num(v->nval);
    if (v->type==VT_STR) {
        char *end; double d = strtod(v->sval, &end);
        if (end == v->sval) die("cannot convert %s to number", v->sval);
        return val_num(d);
    }
    die("num() cannot convert this type");
    return val_none();
}

BUILTIN(type_fn) {
    NEED(1);
    Value *v = ARG(0);
    switch(v->type) {
        case VT_NONE:     return val_str("none");
        case VT_BOOL:     return val_str("bool");
        case VT_NUM:      return val_str("number");
        case VT_STR:      return val_str("string");
        case VT_LIST:     return val_str("list");
        case VT_MAP:      return val_str("map");
        case VT_FUNC:     return val_str("function");
        case VT_CLASS:    return val_str("class");
        case VT_INSTANCE: return val_str("instance");
        case VT_MODULE:   return val_str("module");
        case VT_BUILTIN:  return val_str("builtin");
        default:          return val_str("unknown");
    }
}

BUILTIN(floor_fn) {
    NEED(1); return val_num(floor(ARG(0)->nval));
}
BUILTIN(ceil_fn) {
    NEED(1); return val_num(ceil(ARG(0)->nval));
}
BUILTIN(abs_fn) {
    NEED(1); return val_num(fabs(ARG(0)->nval));
}
BUILTIN(sqrt_fn) {
    NEED(1); return val_num(sqrt(ARG(0)->nval));
}

BUILTIN(list_fn) {
    HowList *l = list_new();
    for (int i=0;i<argc;i++) list_push(l, argv[i]);
    Value *v = val_list(l); list_decref(l); return v;
}

BUILTIN(map_fn) {
    HowMap *m = map_new();
    Value *v = val_map(m); map_decref(m); return v;
}

BUILTIN(push_fn) {
    NEED(2);
    if (ARG(0)->type != VT_LIST) die("push() requires a list");
    list_push(ARG(0)->list, ARG(1));
    return val_incref(ARG(0));
}

BUILTIN(pop_fn) {
    NEED(1);
    if (ARG(0)->type != VT_LIST) die("pop() requires a list");
    HowList *l = ARG(0)->list;
    if (l->len == 0) die("pop() on empty list");
    Value *v = l->items[--l->len];
    /* remove from list without decref (transfer ownership) */
    return v;
}

BUILTIN(keys_fn) {
    NEED(1);
    HowMap *m = NULL;
    if (ARG(0)->type==VT_MAP)      m = ARG(0)->map;
    else if (ARG(0)->type==VT_INSTANCE) m = ARG(0)->inst->fields;
    else die("keys() requires a map or instance");
    HowList *l = list_new();
    for (int i=0;i<m->len;i++) list_push(l, val_str(m->pairs[i].key));
    Value *v = val_list(l); list_decref(l); return v;
}

BUILTIN(values_fn) {
    NEED(1);
    HowMap *m = NULL;
    if (ARG(0)->type==VT_MAP)      m = ARG(0)->map;
    else if (ARG(0)->type==VT_INSTANCE) m = ARG(0)->inst->fields;
    else die("values() requires a map or instance");
    HowList *l = list_new();
    for (int i=0;i<m->len;i++) list_push(l, m->pairs[i].val);
    Value *v = val_list(l); list_decref(l); return v;
}

BUILTIN(has_key_fn) {
    NEED(2);
    Value *obj = ARG(0), *key = ARG(1);
    if (key->type != VT_STR) {
        /* numeric key for lists */
        if (obj->type==VT_LIST && key->type==VT_NUM) {
            int i = (int)key->nval;
            return val_bool(i>=0 && i<obj->list->len);
        }
        die("has_key() key must be string");
    }
    HowMap *m = NULL;
    if (obj->type==VT_MAP)      m=obj->map;
    else if (obj->type==VT_INSTANCE) m=obj->inst->fields;
    else die("has_key() requires a map, instance, or list");
    return val_bool(map_has(m, key->sval));
}

BUILTIN(set_key_fn) {
    NEED(3);
    Value *obj=ARG(0), *key=ARG(1), *val=ARG(2);
    if (obj->type==VT_LIST) {
        int i = (int)(key->type==VT_NUM ? key->nval : atof(key->type==VT_STR?key->sval:"0"));
        if (i<0||i>=obj->list->len) die("set_key(): list index %d out of range",i);
        val_decref(obj->list->items[i]);
        obj->list->items[i] = val_incref(val);
        return val_incref(val);
    }
    HowMap *m = NULL;
    if (obj->type==VT_MAP)      m=obj->map;
    else if (obj->type==VT_INSTANCE) m=obj->inst->fields;
    else die("set_key() requires a map, instance, or list");
    if (key->type!=VT_STR) die("set_key() key must be string");
    map_set(m, key->sval, val);
    return val_incref(val);
}

BUILTIN(get_key_fn) {
    NEED(2);
    Value *obj=ARG(0), *key=ARG(1);
    if (obj->type==VT_LIST) {
        int i = (int)(key->type==VT_NUM ? key->nval : 0);
        if (i<0||i>=obj->list->len) return val_none();
        return val_incref(obj->list->items[i]);
    }
    HowMap *m = NULL;
    if (obj->type==VT_MAP)      m=obj->map;
    else if (obj->type==VT_INSTANCE) m=obj->inst->fields;
    else die("get_key() requires a map, instance, or list");
    if (key->type!=VT_STR) return val_none();
    Value *v = map_get(m, key->sval);
    return v ? val_incref(v) : val_none();
}

BUILTIN(del_key_fn) {
    NEED(2);
    Value *obj=ARG(0), *key=ARG(1);
    HowMap *m = NULL;
    if (obj->type==VT_MAP)      m=obj->map;
    else if (obj->type==VT_INSTANCE) m=obj->inst->fields;
    else die("del_key() requires a map or instance");
    if (key->type!=VT_STR) die("del_key() key must be string");
    map_del(m, key->sval);
    return val_none();
}

BUILTIN(range_fn) {
    long long start=0, stop=0, step=1;
    if (argc==1) { stop=(long long)ARG(0)->nval; }
    else if (argc==2) { start=(long long)ARG(0)->nval; stop=(long long)ARG(1)->nval; }
    else if (argc>=3) { start=(long long)ARG(0)->nval; stop=(long long)ARG(1)->nval; step=(long long)ARG(2)->nval; }
    else die("range() requires 1-3 args");
    HowList *l = list_new();
    for (long long i=start; (step>0?i<stop:i>stop); i+=step)
        list_push(l, val_num((double)i));
    Value *v = val_list(l); list_decref(l); return v;
}

BUILTIN(ask_fn) {
    if (argc>0) { char *s=val_repr(argv[0]); printf("%s",s); free(s); fflush(stdout); }
    char buf[4096]; if (!fgets(buf,sizeof(buf),stdin)) return val_none();
    int len=strlen(buf); if(len>0&&buf[len-1]=='\n') buf[len-1]=0;
    return val_str(buf);
}

BUILTIN(read_fn) {
    NEED(1);
    if (ARG(0)->type!=VT_STR) die("read() requires a string path");
    FILE *f = fopen(ARG(0)->sval,"r");
    if (!f) die("read(): cannot open '%s': %s", ARG(0)->sval, strerror(errno));
    fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f);
    char *buf = xmalloc(sz+1);
    fread(buf,1,sz,f); buf[sz]=0; fclose(f);
    return val_str_own(buf);
}

static void write_value_to_path(const char *path, Value *v) {
    FILE *f = fopen(path, "w");
    if (!f) die("write(): cannot open '%s' for writing: %s", path, strerror(errno));
    if (v->type == VT_STR) {
        if (fwrite(v->sval, 1, strlen(v->sval), f) < strlen(v->sval)) {
            fclose(f);
            die("write(): failed writing '%s'", path);
        }
    } else {
        char *s = val_repr(v);
        size_t n = strlen(s);
        if (fwrite(s, 1, n, f) < n) {
            free(s);
            fclose(f);
            die("write(): failed writing '%s'", path);
        }
        free(s);
    }
    fclose(f);
}

BUILTIN(write_fn) {
    NEED(2);
    if (ARG(0)->type!=VT_STR) die("write() requires a string path as first argument");
    write_value_to_path(ARG(0)->sval, ARG(1));
    return val_incref(ARG(1));
}

BUILTIN(args_fn) {
    /* argv is set in main */
    return val_incref(env_get(g_globals,"__args"));
}

static char *args_argv0 = NULL;  /* set in main */

BUILTIN(dirof_fn) {
    NEED(1);
    if (ARG(0)->type!=VT_STR) die("dirof() requires a string path");
    char buf[4096]; strncpy(buf,ARG(0)->sval,sizeof(buf)-1);
    /* dirname */
    char *slash = strrchr(buf,'/');
    if (!slash) return val_str("./");
    slash[1]=0;
    return val_str(buf);
}

BUILTIN(cwd_fn) {
    (void)argc; (void)argv; (void)ctx;
    char buf[4096];
    if (!getcwd(buf, sizeof(buf))) die("cwd(): cannot get working directory");
    return val_str(buf);
}

/* run(cmd) — execute a shell command, returns the exit code as a number */
BUILTIN(run_fn) {
    NEED(1);
    if (ARG(0)->type != VT_STR) die("run() requires a string command");
    int code = system(ARG(0)->sval);
    /* WEXITSTATUS extracts the real exit code from the wait() status */
#ifdef WEXITSTATUS
    if (code != -1) code = WEXITSTATUS(code);
#endif
    return val_num((double)code);
}

BUILTIN(resolve_how_fn) {
    NEED(1);
    if (ARG(0)->type!=VT_STR) die("_resolve_how() requires a string");
    char *path = find_how_file(ARG(0)->sval);
    return val_str_own(path);
}

/* _basename(path) — last path component without extension */
BUILTIN(basename_fn) {
    NEED(1);
    if (ARG(0)->type!=VT_STR) die("_basename() requires a string");
    const char *s = ARG(0)->sval;
    const char *slash = strrchr(s, '/');
    const char *base = slash ? slash+1 : s;
    /* strip .how extension if present */
    char buf[512]; strncpy(buf, base, sizeof(buf)-1); buf[sizeof(buf)-1]=0;
    int len = strlen(buf);
    if (len > 4 && !strcmp(buf+len-4, ".how")) buf[len-4] = 0;
    return val_str(buf);
}

/* _dirname(path) — directory part (empty string if no slash) */
BUILTIN(dirname_fn) {
    NEED(1);
    if (ARG(0)->type!=VT_STR) die("_dirname() requires a string");
    const char *s = ARG(0)->sval;
    const char *slash = strrchr(s, '/');
    if (!slash) return val_str("");
    char buf[4096];
    int len = (int)(slash - s);
    strncpy(buf, s, len); buf[len] = 0;
    return val_str(buf);
}

/* _add_search_dir("dir") — add a directory directly to import search path */
BUILTIN(add_search_dir_fn) {
    NEED(1);
    if (ARG(0)->type!=VT_STR) die("_add_search_dir() requires a string");
    /* Resolve relative to each existing import dir */
    const char *d = ARG(0)->sval;
    if (d[0] == '/') {
        how_add_import_dir(d);
    } else {
        int added = 0;
        for (int i=0;i<import_dirs_len && !added;i++) {
            char resolved[4096];
            snprintf(resolved,sizeof(resolved),"%s/%s",import_dirs[i],d);
            struct stat st2;
            if (stat(resolved, &st2) == 0 && S_ISDIR(st2.st_mode)) {
                how_add_import_dir(resolved); added=1;
            }
        }
        if (!added) how_add_import_dir(d);  /* add as-is */
    }
    return val_str(d);
}

BUILTIN(push_import_dir_fn) {
    NEED(1);
    if (ARG(0)->type!=VT_STR) die("_push_import_dir() requires a string");
    const char *path = ARG(0)->sval;
    /* get directory of path */
    char buf[4096]; strncpy(buf,path,sizeof(buf)-4);
    char *slash = strrchr(buf,'/');
    if (slash) { slash[1]=0; how_add_import_dir(buf); }
    else { /* no slash = cwd */ how_add_import_dir("."); }
    return val_str(buf);
}

BUILTIN(max_fn) {
    if (argc==1 && ARG(0)->type==VT_LIST) {
        HowList *l = ARG(0)->list;
        if (!l->len) return val_none();
        Value *best = l->items[0];
        for(int i=1;i<l->len;i++)
            if(l->items[i]->type==VT_NUM && l->items[i]->nval > best->nval) best=l->items[i];
        return val_incref(best);
    }
    if (!argc) return val_none();
    Value *best=argv[0];
    for(int i=1;i<argc;i++)
        if(argv[i]->type==VT_NUM && argv[i]->nval>best->nval) best=argv[i];
    return val_incref(best);
}

BUILTIN(min_fn) {
    if (argc==1 && ARG(0)->type==VT_LIST) {
        HowList *l = ARG(0)->list;
        if (!l->len) return val_none();
        Value *best = l->items[0];
        for(int i=1;i<l->len;i++)
            if(l->items[i]->type==VT_NUM && l->items[i]->nval < best->nval) best=l->items[i];
        return val_incref(best);
    }
    if (!argc) return val_none();
    Value *best=argv[0];
    for(int i=1;i<argc;i++)
        if(argv[i]->type==VT_NUM && argv[i]->nval<best->nval) best=argv[i];
    return val_incref(best);
}

BUILTIN(quit_fn) {
    (void)argc; (void)argv; (void)ctx;
    exit(0);
}

static void gc_collect(Env *root_env);

BUILTIN(gc_fn) {
    (void)argc; (void)argv; (void)ctx;
    gc_collect(g_globals);
    return val_none();
}

/* _host_call(fn, args_list) — call a native host function with a HowList of args */
BUILTIN(host_call_fn) {
    NEED(2);
    Value *fn = ARG(0);
    Value *args_val = ARG(1);
    if (args_val->type != VT_LIST) die("_host_call: args must be a list");
    HowList *args_list = args_val->list;
    Signal sig = {SIG_NONE, NULL};
    Value **argv2 = xmalloc(args_list->len * sizeof(Value*) + 1);
    for (int i=0;i<args_list->len;i++) argv2[i] = args_list->items[i];
    Value *res = eval_call_val(fn, argv2, args_list->len, &sig, 0);
    free(argv2);
    if (sig.type==SIG_RETURN) { val_decref(sig.retval); sig.retval=NULL; }
    if (sig.type==SIG_ERROR) {
        char *s = sig.retval ? val_repr(sig.retval) : xstrdup("unknown error");
        if (sig.retval) val_decref(sig.retval);
        val_decref(res);
        die("unhandled error in _host_call: %s", s);
    }
    return res;
}

static Value *make_builtin(const char *name, BuiltinFn fn) {
    Value *v = val_new(VT_BUILTIN);
    v->builtin.fn   = fn;
    v->builtin.ctx  = NULL;
    v->builtin.name = xstrdup(name);
    return v;
}


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
        case VT_LIST: gc_mark_list(v->list); break;
        case VT_MAP: gc_mark_map(v->map); break;
        case VT_FUNC: gc_mark_func(v->func); break;
        case VT_CLASS: gc_mark_class(v->cls); break;
        case VT_INSTANCE: gc_mark_instance(v->inst); break;
        case VT_MODULE: gc_mark_module(v->mod); break;
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

static void gc_sweep_values(void) {
    Value **pp = &g_all_values;
    while (*pp) {
        Value *v = *pp;
        if (v->gc_mark) {
            v->gc_mark = 0;
            pp = &v->gc_next;
            continue;
        }
        *pp = v->gc_next;
        if (v == V_NONE_SINGLETON) V_NONE_SINGLETON = NULL;
        if (v == V_TRUE_SINGLETON) V_TRUE_SINGLETON = NULL;
        if (v == V_FALSE_SINGLETON) V_FALSE_SINGLETON = NULL;
        if (v->type == VT_STR) free(v->sval);
        else if (v->type == VT_BUILTIN) free(v->builtin.name);
        free(v);
    }
}

static void gc_sweep_maps(void) {
    HowMap **pp = &g_all_maps;
    while (*pp) {
        HowMap *m = *pp;
        if (m->gc_mark) {
            m->gc_mark = 0;
            pp = &m->gc_next;
            continue;
        }
        *pp = m->gc_next;
        for (int i = 0; i < m->len; i++) free(m->pairs[i].key);
        free(m->pairs);
        free(m);
    }
}

static void gc_sweep_lists(void) {
    HowList **pp = &g_all_lists;
    while (*pp) {
        HowList *l = *pp;
        if (l->gc_mark) {
            l->gc_mark = 0;
            pp = &l->gc_next;
            continue;
        }
        *pp = l->gc_next;
        free(l->items);
        free(l);
    }
}

static void gc_sweep_funcs(void) {
    HowFunc **pp = &g_all_funcs;
    while (*pp) {
        HowFunc *f = *pp;
        if (f->gc_mark) {
            f->gc_mark = 0;
            pp = &f->gc_next;
            continue;
        }
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
        if (c->gc_mark) {
            c->gc_mark = 0;
            pp = &c->gc_next;
            continue;
        }
        *pp = c->gc_next;
        for (int i = 0; i < c->params.len; i++) free(c->params.s[i]);
        free(c->params.s);
        free(c);
    }
}

static void gc_sweep_instances(void) {
    HowInstance **pp = &g_all_instances;
    while (*pp) {
        HowInstance *inst = *pp;
        if (inst->gc_mark) {
            inst->gc_mark = 0;
            pp = &inst->gc_next;
            continue;
        }
        *pp = inst->gc_next;
        free(inst);
    }
}

static void gc_sweep_modules(void) {
    HowModule **pp = &g_all_modules;
    while (*pp) {
        HowModule *m = *pp;
        if (m->gc_mark) {
            m->gc_mark = 0;
            pp = &m->gc_next;
            continue;
        }
        *pp = m->gc_next;
        free(m->name);
        free(m);
    }
}

static void gc_sweep_envs(void) {
    Env **pp = &g_all_envs;
    while (*pp) {
        Env *e = *pp;
        if (e->gc_mark) {
            e->gc_mark = 0;
            pp = &e->gc_next;
            continue;
        }
        *pp = e->gc_next;
        for (int i = 0; i < e->len; i++) free(e->entries[i].key);
        free(e->entries);
        free(e);
    }
}

static void gc_clear_root_stacks(void) {
    g_gc_value_roots.len = 0;
    g_gc_env_roots.len = 0;
}

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

static void gc_collect(Env *root_env) {
    if (g_gc_in_progress) return;
    g_gc_in_progress = 1;
    if (V_NONE_SINGLETON) gc_mark_value(V_NONE_SINGLETON);
    if (V_TRUE_SINGLETON) gc_mark_value(V_TRUE_SINGLETON);
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

static void setup_globals(Env *env) {
#define REG(name,fn) env_set(env,name,make_builtin(name,builtin_##fn))
    REG("print",   print);
    REG("len",     len);
    REG("str",     str_fn);
    REG("num",     num_fn);
    REG("type",    type_fn);
    REG("floor",   floor_fn);
    REG("ceil",    ceil_fn);
    REG("abs",     abs_fn);
    REG("sqrt",    sqrt_fn);
    REG("list",    list_fn);
    REG("map",     map_fn);
    REG("push",    push_fn);
    REG("pop",     pop_fn);
    REG("keys",    keys_fn);
    REG("values",  values_fn);
    REG("has_key", has_key_fn);
    REG("set_key", set_key_fn);
    REG("get_key", get_key_fn);
    REG("del_key", del_key_fn);
    REG("range",   range_fn);
    REG("ask",     ask_fn);
    REG("read",    read_fn);
    REG("write",   write_fn);
    REG("args",    args_fn);
    REG("dirof",   dirof_fn);
    REG("cwd",     cwd_fn);
    REG("run",     run_fn);
    REG("_resolve_how",     resolve_how_fn);
    REG("_push_import_dir", push_import_dir_fn);
    REG("_add_search_dir",  add_search_dir_fn);
    REG("max",     max_fn);
    REG("min",     min_fn);
    REG("quit",    quit_fn);
    REG("gc",      gc_fn);
    REG("_host_call",      host_call_fn);
    REG("_basename", basename_fn);
    REG("_dirname",  dirname_fn);
    /* bool() as a function */
    Value *t = val_bool(1); env_set(env,"true",t); val_decref(t);
    Value *f = val_bool(0); env_set(env,"false",f); val_decref(f);
    Value *n = val_none(); env_set(env,"none",n); val_decref(n);
#undef REG
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
static Value *eval_call_val(Value *callee, Value **args, int argc, Signal *sig, int line) {
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
static void exec_body(Node *body, Env *env, Signal *sig) {
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


