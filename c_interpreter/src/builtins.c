/*
 * builtins.c — all built-in functions and global environment setup.
 *              Depends on gc.c (value/env ops) and runtime.c (eval_call_val,
 *              exec_body, find_how_file, how_add_import_dir).
 */
#include "runtime_internal.h"
#include "runtime.h"

/* ── Static helpers ──────────────────────────────────────────────────────── */

static char *args_argv0 = NULL;  /* set in main */

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

/* ── Built-in function implementations ──────────────────────────────────── */

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

/* ── Builtin value factory ───────────────────────────────────────────────── */

Value *make_builtin(const char *name, BuiltinFn fn) {
    Value *v = val_new(VT_BUILTIN);
    v->builtin.fn   = fn;
    v->builtin.ctx  = NULL;
    v->builtin.name = xstrdup(name);
    return v;
}

/* ── Global environment setup ────────────────────────────────────────────── */

void setup_globals(Env *env) {
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
