/*
 * runtime_internal.h — internal types, globals, and function declarations
 *                       shared across runtime.c, gc.c, and builtins.c.
 *                       Not part of the public API.
 */
#ifndef HOWLANG_RUNTIME_INTERNAL_H
#define HOWLANG_RUNTIME_INTERNAL_H

#include "common.h"
#include "ast.h"
#include <pthread.h>

/* ── Value types ─────────────────────────────────────────────────────────── */

typedef enum {
    VT_NONE, VT_BOOL, VT_NUM, VT_STR,
    VT_LIST, VT_MAP,
    VT_FUNC, VT_CLASS, VT_INSTANCE, VT_MODULE,
    VT_BUILTIN,
    VT_DUAL,    /* dual number {val, tan} — forward-mode AD */
    VT_TENSOR,  /* N-dimensional float64 tensor */
} VT;

typedef struct HowList    HowList;
typedef struct HowMap     HowMap;
typedef struct HowFunc    HowFunc;
typedef struct HowClass   HowClass;
typedef struct HowInstance HowInstance;
typedef struct HowModule  HowModule;
typedef struct HowTensor  HowTensor;

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
    int      is_parallel;   /* 1 = parallel (^{}) variant */
    int      is_grad;       /* 1 = grad wrapper; closure has "__primal__" */
    HowFunc *grad_fn;       /* custom backward pass function, if defined */
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
    HowMap  *fields;   /* shared with inst_env */
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

struct HowTensor {
    double      *data;      /* flat row-major float64 array; NULL for views */
    double      *data_base; /* pointer to start of allocation (owned by base or self) */
    int         *shape;
    int          ndim;
    int          nelem;
    int         *strides;
    HowTensor   *base;      /* non-NULL = view; gc_mark_tensor traces base */
    int          gc_mark;
    struct HowTensor *gc_next;
};

typedef Value* (*BuiltinFn)(int argc, Value **argv, const char **arg_names, void *ctx);

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
        struct { double val; double tan; } dual;  /* VT_DUAL */
        HowTensor   *tensor;                       /* VT_TENSOR */
    };
};

typedef struct EnvEntry { char *key; Value *val; } EnvEntry;

struct Env {
    EnvEntry *entries;
    int       len;
    int       cap;
    Env      *parent;
    int       refcount;
    int       gc_mark;
    struct Env *gc_next;
    HowInstance *inst;  /* non-NULL if this is an InstanceEnv */
    int       is_parallel; /* 1 = this is a parallel loop's local scope */
};

/* ── Control flow signals ─────────────────────────────────────────────────── */

typedef enum { SIG_NONE, SIG_RETURN, SIG_BREAK, SIG_NEXT, SIG_ERROR } SigType;
typedef struct { SigType type; Value *retval; } Signal;

/* ── Builtin helpers ─────────────────────────────────────────────────────── */

#define BUILTIN(name) Value *builtin_##name(int argc, Value **argv, const char **arg_names __attribute__((unused)), void *ctx __attribute__((unused)))
#define NEED(n) do{ if(argc<(n)) die("builtin requires %d args, got %d",(n),argc); }while(0)
#define ARG(i)  (argc>(i) ? argv[i] : V_NONE_SINGLETON)

/* ── GC root macros ──────────────────────────────────────────────────────── */

void gc_push_value_root(Value **slot);
void gc_pop_value_root(void);
void gc_push_env_root(Env **slot);
void gc_pop_env_root(void);

#define GC_ROOT_VALUE(v)  gc_push_value_root(&(v))
#define GC_UNROOT_VALUE() gc_pop_value_root()
#define GC_ROOT_ENV(e)    gc_push_env_root(&(e))
#define GC_UNROOT_ENV()   gc_pop_env_root()

/* ── Shared globals (defined in gc.c) ───────────────────────────────────── */

extern Value       *g_all_values;
extern HowMap      *g_all_maps;
extern HowList     *g_all_lists;
extern HowFunc     *g_all_funcs;
extern HowClass    *g_all_classes;
extern HowInstance *g_all_instances;
extern HowModule   *g_all_modules;
extern HowTensor   *g_all_tensors;
extern Env         *g_all_envs;
extern size_t       g_gc_allocations;

extern pthread_mutex_t g_alloc_mutex;   /* protects GC linked-list insertions */
extern volatile int    g_gc_suspended;  /* set to 1 during parallel loops */

/* Value singletons — defined in gc.c, initialised in how_runtime_bootstrap */
extern Value *V_NONE_SINGLETON;
extern Value *V_TRUE_SINGLETON;
extern Value *V_FALSE_SINGLETON;

/* ── Shared globals (defined in runtime.c) ──────────────────────────────── */

extern Env    *g_globals;
extern HowMap *g_module_registry;
extern int     g_num_builtins;
extern char  **import_dirs;
extern int     import_dirs_len;

/* ── Value constructors / destructors (defined in gc.c) ─────────────────── */

Value   *val_new(VT type);
Value   *val_none(void);
Value   *val_bool(int b);
Value   *val_num(double d);
Value   *val_str(const char *s);
Value   *val_str_own(char *s);
Value   *val_incref(Value *v);
void     val_decref(Value *v);

HowMap  *map_new(void);
Value   *val_map(HowMap *m);
void     map_decref(HowMap *m);

HowList *list_new(void);
Value   *val_list(HowList *l);
void     list_decref(HowList *l);

HowTensor *tensor_new(int ndim, int *shape);
Value     *val_tensor(HowTensor *t);
void       gc_mark_tensor(HowTensor *t);  /* for use in gc_mark_value */

void     func_decref(HowFunc *f);
void     cls_decref(HowClass *c);
void     inst_decref(HowInstance *inst);
void     mod_decref(HowModule *m);

StrList  strlist_clone(StrList src);

/* ── Environment (defined in gc.c) ─────────────────────────────────────── */

Env    *env_new(Env *parent);
Env    *inst_env_new(HowInstance *inst, Env *parent);
void    env_decref(Env *e);
void    env_set(Env *e, const char *key, Value *val);
Value  *env_get(Env *e, const char *key);
int     env_assign(Env *e, const char *key, Value *val);

/* ── Map / list data operations (defined in gc.c) ───────────────────────── */

void    map_set(HowMap *m, const char *key, Value *val);
Value  *map_get(HowMap *m, const char *key);
int     map_has(HowMap *m, const char *key);
void    map_del(HowMap *m, const char *key);
void    list_push(HowList *l, Value *v);

/* ── Value operations (defined in gc.c) ─────────────────────────────────── */

char   *val_repr(Value *v);
int     how_truthy(Value *v);
int     how_eq(Value *a, Value *b);

/* ── GC functions (defined in gc.c) ─────────────────────────────────────── */

void    gc_collect(Env *root_env);
void    gc_clear_root_stacks(void);

/* ── Evaluator internals (defined in runtime.c) ─────────────────────────── */

Value  *eval(Node *node, Env *env, Signal *sig);
void    run_branches(NodeList *branches, Env *env, Signal *sig);
void    run_loop(HowFunc *fn, Signal *sig);
Value  *run_parallel_loop(HowFunc *fn, Signal *sig);
Value  *instantiate_class(HowClass *cls, Value **args, const char **arg_names, int argc, Signal *sig, int line);
Value  *eval_call_val(Value *callee, Value **args, const char **arg_names, int argc, Signal *sig, int line);
void    exec_body(Node *body, Env *env, Signal *sig);
void    exec_stmt(Node *node, Env *env, Signal *sig);

/* ── Module import (defined in import.c) ────────────────────────────────── */

char   *find_how_file(const char *name);
void    exec_import(const char *modname, const char *alias, Env *env);

/* ── Builtin registration (defined in builtins.c) ───────────────────────── */

Value  *make_builtin(const char *name, BuiltinFn fn);
void    setup_globals(Env *env);

/* ── Automatic differentiation (defined in ad.c) ────────────────────────── */

Value  *val_dual(double v, double t);
Value  *compute_grad_closure(Value *primal_fn, Signal *sig);
Value  *call_custom_grad(HowFunc *primal_fn, Value **args, int argc, Signal *sig, int line);
Value  *dual_binop(Value *l, Value *r, const char *op, int line);
void    tape_ensure_entry(void);
Value  *tape_new_val(double primal);

typedef struct {
    int    out_id;
    char   op[8];
    int    in_ids[2];
    double in_vals[2];
} TapeEntry;

extern TapeEntry *g_tape;
extern int        g_tape_len;
extern int        g_tape_next_id;
extern int        g_tape_vsize;
extern int        g_tape_active;

#define IS_TAPE_VAL(v)  ((v)->type==VT_DUAL && (v)->dual.tan < -0.5)
#define TAPE_ID(v)      ((int)(-(v)->dual.tan) - 1)

#endif
