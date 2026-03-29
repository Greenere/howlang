/*
 * builtins.c — all built-in functions and global environment setup.
 *              Depends on gc.c (value/env ops) and runtime.c (eval_call_val,
 *              exec_body, find_how_file, how_add_import_dir).
 */
#include "runtime_internal.h"
#include "runtime.h"
#include <pthread.h>

/* ── Static helpers ──────────────────────────────────────────────────────── */

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

static Value *call_map_callback(Value *fn, Value *item) {
    Signal sig = {SIG_NONE, NULL};
    Value *res = eval_call_val(fn, &item, NULL, 1, &sig, 0);
    if (sig.type == SIG_ERROR) {
        char *s = sig.retval ? val_repr(sig.retval) : xstrdup("unknown error");
        if (sig.retval) val_decref(sig.retval);
        if (res) val_decref(res);
        die("map(): callback error: %s", s);
    }
    if (sig.type == SIG_RETURN && sig.retval) {
        if (res) val_decref(res);
        res = sig.retval;
        sig.retval = NULL;
    }
    return res;
}

static Value *call_reduce_callback(Value *fn, Value *acc, Value *item) {
    Signal sig = {SIG_NONE, NULL};
    Value *args[2] = {acc, item};
    Value *res = eval_call_val(fn, args, NULL, 2, &sig, 0);
    if (sig.type == SIG_ERROR) {
        char *s = sig.retval ? val_repr(sig.retval) : xstrdup("unknown error");
        if (sig.retval) val_decref(sig.retval);
        if (res) val_decref(res);
        die("reduce(): callback error: %s", s);
    }
    if (sig.type == SIG_RETURN && sig.retval) {
        if (res) val_decref(res);
        res = sig.retval;
        sig.retval = NULL;
    }
    return res;
}

/* ── Built-in function implementations ──────────────────────────────────── */

BUILTIN(print) {
    int newline = 1;
    const char *end = "\n";
    int printed = 0;

    for (int i=0;i<argc;i++) {
        if (arg_names && arg_names[i]) {
            if (!strcmp(arg_names[i], "newline")) {
                if (argv[i]->type != VT_BOOL) die("print(newline=...) requires a bool");
                newline = argv[i]->bval;
                continue;
            }
            if (!strcmp(arg_names[i], "end")) {
                if (argv[i]->type != VT_STR) die("print(end=...) requires a string");
                end = argv[i]->sval;
                continue;
            }
            die("print() got an unexpected named argument '%s'", arg_names[i]);
        }
        char *s = val_repr(argv[i]);
        if (printed) printf(" ");
        printf("%s", s);
        printed = 1;
        free(s);
    }
    if (!newline && !strcmp(end, "\n")) end = "";
    printf("%s", end);
    return val_none();
}

BUILTIN(len) {
    NEED(1);
    Value *v = ARG(0);
    if (v->type==VT_LIST) return val_num(v->list->len);
    if (v->type==VT_MAP)  return val_num(v->map->len);
    if (v->type==VT_STR)  return val_num(strlen(v->sval));
    if (v->type==VT_INSTANCE) return val_num(v->inst->fields->len);
    if (v->type==VT_TENSOR) return val_num(v->tensor->shape[0]);
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

BUILTIN(chr_fn) {
    int codepoint;
    char out[5];

    if (argc != 1) die("chr() requires exactly 1 argument");
    if (arg_names && arg_names[0] && strcmp(arg_names[0], "codepoint"))
        die("chr() got an unexpected named argument '%s'", arg_names[0]);
    if (ARG(0)->type != VT_NUM) die("chr() requires a numeric code point");
    codepoint = (int)ARG(0)->nval;
    if (ARG(0)->nval != (double)codepoint) die("chr() requires an integer code point");
    if (!how_utf8_encode_one(codepoint, out)) die("chr() code point out of range");
    return val_str(out);
}

BUILTIN(ord_fn) {
    int codepoint, nbytes;

    if (argc != 1) die("ord() requires exactly 1 argument");
    if (arg_names && arg_names[0] && strcmp(arg_names[0], "char"))
        die("ord() got an unexpected named argument '%s'", arg_names[0]);
    if (ARG(0)->type != VT_STR) die("ord() requires a string");
    if (!how_utf8_decode_one(ARG(0)->sval, &codepoint, &nbytes) || ARG(0)->sval[nbytes] != '\0')
        die("ord() requires a single character string");
    return val_num(codepoint);
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
        case VT_DUAL:     return val_str("number");
        case VT_TENSOR:   return val_str("tensor");
        default:          return val_str("unknown");
    }
}

BUILTIN(floor_fn) {
    NEED(1);
    if (ARG(0)->type==VT_DUAL) return val_dual(floor(ARG(0)->dual.val), 0.0);
    return val_num(floor(ARG(0)->nval));
}
BUILTIN(ceil_fn) {
    NEED(1);
    if (ARG(0)->type==VT_DUAL) return val_dual(ceil(ARG(0)->dual.val), 0.0);
    return val_num(ceil(ARG(0)->nval));
}
BUILTIN(abs_fn) {
    NEED(1);
    if (ARG(0)->type==VT_DUAL) {
        double v=ARG(0)->dual.val, t=ARG(0)->dual.tan;
        return val_dual(fabs(v), v>=0.0 ? t : -t);
    }
    return val_num(fabs(ARG(0)->nval));
}
BUILTIN(sqrt_fn) {
    NEED(1);
    if (ARG(0)->type==VT_DUAL) {
        double v=ARG(0)->dual.val, t=ARG(0)->dual.tan;
        if (v<0) die("sqrt of negative dual number");
        double sv=sqrt(v);
        return val_dual(sv, sv>0.0 ? t/(2.0*sv) : 0.0);
    }
    return val_num(sqrt(ARG(0)->nval));
}
BUILTIN(sin_fn) {
    NEED(1);
    if (ARG(0)->type==VT_DUAL) {
        double v=ARG(0)->dual.val, t=ARG(0)->dual.tan;
        return val_dual(sin(v), cos(v)*t);
    }
    return val_num(sin(ARG(0)->nval));
}
BUILTIN(cos_fn) {
    NEED(1);
    if (ARG(0)->type==VT_DUAL) {
        double v=ARG(0)->dual.val, t=ARG(0)->dual.tan;
        return val_dual(cos(v), -sin(v)*t);
    }
    return val_num(cos(ARG(0)->nval));
}
BUILTIN(exp_fn) {
    NEED(1);
    if (ARG(0)->type==VT_DUAL) {
        double v=ARG(0)->dual.val, t=ARG(0)->dual.tan;
        double ev=exp(v);
        return val_dual(ev, ev*t);
    }
    return val_num(exp(ARG(0)->nval));
}
BUILTIN(log_fn) {
    NEED(1);
    if (ARG(0)->type==VT_DUAL) {
        double v=ARG(0)->dual.val, t=ARG(0)->dual.tan;
        if (v<=0.0) die("log() of non-positive value");
        return val_dual(log(v), t/v);
    }
    if (ARG(0)->nval<=0.0) die("log() of non-positive value");
    return val_num(log(ARG(0)->nval));
}
BUILTIN(pow_fn) {
    NEED(2);
    double base = ARG(0)->type==VT_DUAL ? ARG(0)->dual.val : ARG(0)->nval;
    double exp_ = ARG(1)->type==VT_DUAL ? ARG(1)->dual.val : ARG(1)->nval;
    if (ARG(0)->type==VT_DUAL || ARG(1)->type==VT_DUAL) {
        double v  = pow(base, exp_);
        double dt = ARG(0)->type==VT_DUAL ? ARG(0)->dual.tan : 0.0;
        double de = ARG(1)->type==VT_DUAL ? ARG(1)->dual.tan : 0.0;
        double t  = (base > 0.0 ? exp_*pow(base, exp_-1.0)*dt + v*log(base)*de
                                : exp_*pow(base, exp_-1.0)*dt);
        return val_dual(v, t);
    }
    return val_num(pow(base, exp_));
}

BUILTIN(list_fn) {
    HowList *l = list_new();
    for (int i=0;i<argc;i++) list_push(l, argv[i]);
    Value *v = val_list(l); list_decref(l); return v;
}

BUILTIN(map_fn) {
    if (argc == 0) {
        HowMap *m = map_new();
        Value *v = val_map(m); map_decref(m); return v;
    }

    if (argc != 2) die("map() requires 0 or 2 arguments");

    Value *src = ARG(0);
    Value *fn  = ARG(1);

    if (src->type == VT_LIST) {
        HowList *out = list_new();
        for (int i = 0; i < src->list->len; i++) {
            Value *mapped = call_map_callback(fn, src->list->items[i]);
            list_push(out, mapped);
            val_decref(mapped);
        }
        Value *v = val_list(out); list_decref(out); return v;
    }

    if (src->type == VT_MAP || src->type == VT_INSTANCE) {
        HowMap *in = src->type == VT_MAP ? src->map : src->inst->fields;
        HowMap *out = map_new();
        for (int i = 0; i < in->len; i++) {
            Value *mapped = call_map_callback(fn, in->pairs[i].val);
            map_set(out, in->pairs[i].key, mapped);
            val_decref(mapped);
        }
        Value *v = val_map(out); map_decref(out); return v;
    }

    if (src->type == VT_TENSOR) {
        HowTensor *t = src->tensor;
        HowTensor *out = tensor_new(t->ndim, t->shape);
        for (int i = 0; i < t->nelem; i++) {
            Value *item = val_num(t->data[i]);
            Value *mapped = call_map_callback(fn, item);
            val_decref(item);
            if (mapped->type != VT_NUM) {
                val_decref(mapped);
                die("map(tensor, fn) requires fn to return numbers");
            }
            out->data[i] = mapped->nval;
            val_decref(mapped);
        }
        return val_tensor(out);
    }

    if (src->type == VT_STR) {
        HowList *out = list_new();
        const char *p = src->sval;
        while (*p) {
            int codepoint = 0, nbytes = 0;
            char ch[5];
            if (!how_utf8_decode_one(p, &codepoint, &nbytes))
                die("map(string, fn): invalid UTF-8");
            memcpy(ch, p, (size_t)nbytes);
            ch[nbytes] = '\0';
            Value *item = val_str(ch);
            Value *mapped = call_map_callback(fn, item);
            val_decref(item);
            list_push(out, mapped);
            val_decref(mapped);
            p += nbytes;
        }
        Value *v = val_list(out); list_decref(out); return v;
    }

    die("map() requires a list, map, instance, tensor, or string as first argument");
    return val_none();
}

BUILTIN(reduce_fn) {
    Value *src, *fn, *acc;
    int have_init = 0;

    if (argc == 2) {
        src = ARG(0);
        fn = ARG(1);
    } else if (argc == 3) {
        src = ARG(0);
        acc = val_incref(ARG(1));
        fn = ARG(2);
        have_init = 1;
    } else {
        die("reduce() requires 2 or 3 arguments");
        return val_none();
    }

    if (src->type == VT_LIST) {
        HowList *list = src->list;
        int start = 0;
        if (!have_init) {
            if (list->len == 0) die("reduce(list, fn) requires a non-empty list or an explicit initial value");
            acc = val_incref(list->items[0]);
            start = 1;
        }
        for (int i = start; i < list->len; i++) {
            Value *next = call_reduce_callback(fn, acc, list->items[i]);
            val_decref(acc);
            acc = next;
        }
        return acc;
    }

    if (src->type == VT_MAP || src->type == VT_INSTANCE) {
        HowMap *map = src->type == VT_MAP ? src->map : src->inst->fields;
        int start = 0;
        if (!have_init) {
            if (map->len == 0) die("reduce(map, fn) requires a non-empty map or an explicit initial value");
            acc = val_incref(map->pairs[0].val);
            start = 1;
        }
        for (int i = start; i < map->len; i++) {
            Value *next = call_reduce_callback(fn, acc, map->pairs[i].val);
            val_decref(acc);
            acc = next;
        }
        return acc;
    }

    if (src->type == VT_TENSOR) {
        HowTensor *t = src->tensor;
        int start = 0;
        if (!have_init) {
            if (t->nelem == 0) die("reduce(tensor, fn) requires a non-empty tensor or an explicit initial value");
            acc = val_num(t->data[0]);
            start = 1;
        }
        for (int i = start; i < t->nelem; i++) {
            Value *item = val_num(t->data[i]);
            Value *next = call_reduce_callback(fn, acc, item);
            val_decref(item);
            val_decref(acc);
            acc = next;
        }
        return acc;
    }

    if (src->type == VT_STR) {
        const char *p = src->sval;
        if (!have_init) {
            int codepoint = 0, nbytes = 0;
            char ch[5];
            if (!*p) die("reduce(string, fn) requires a non-empty string or an explicit initial value");
            if (!how_utf8_decode_one(p, &codepoint, &nbytes))
                die("reduce(string, fn): invalid UTF-8");
            memcpy(ch, p, (size_t)nbytes);
            ch[nbytes] = '\0';
            acc = val_str(ch);
            p += nbytes;
        }
        while (*p) {
            int codepoint = 0, nbytes = 0;
            char ch[5];
            if (!how_utf8_decode_one(p, &codepoint, &nbytes))
                die("reduce(string, fn): invalid UTF-8");
            memcpy(ch, p, (size_t)nbytes);
            ch[nbytes] = '\0';
            Value *item = val_str(ch);
            Value *next = call_reduce_callback(fn, acc, item);
            val_decref(item);
            val_decref(acc);
            acc = next;
            p += nbytes;
        }
        return acc;
    }

    if (have_init) val_decref(acc);
    die("reduce() requires a list, map, instance, tensor, or string as first argument");
    return val_none();
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
    UNUSED(argc); UNUSED(argv);
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
    if (argc==1 && ARG(0)->type==VT_TENSOR) {
        return tensor_max_value(ARG(0));
    }
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
    if (argc==1 && ARG(0)->type==VT_TENSOR) {
        return tensor_min_value(ARG(0));
    }
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

BUILTIN(set_grad_fn) {
    NEED(2);
    if (ARG(0)->type != VT_FUNC) die("set_grad() requires a function as first argument");
    if (ARG(1)->type != VT_FUNC) die("set_grad() requires a function as second argument");

    HowFunc *target = ARG(0)->func;
    HowFunc *rule = ARG(1)->func;

    if (target->is_grad) die("set_grad() cannot override grad(...) wrapper functions directly");

    if (target->grad_fn) func_decref(target->grad_fn);
    target->grad_fn = rule;
    target->grad_fn->refcount++;
    return val_incref(ARG(0));
}

BUILTIN(grad_builtin_fn) {
    NEED(1);
    Value *f = ARG(0);
    if (f->type != VT_FUNC && f->type != VT_BUILTIN)
        die("grad() requires a function");

    Env *grad_env = env_new(NULL);
    GC_ROOT_ENV(grad_env);
    env_set(grad_env, "__primal__", f);

    HowFunc *gfn = xmalloc(sizeof(*gfn));
    memset(gfn, 0, sizeof(*gfn));
    gfn->closure  = grad_env; grad_env->refcount++;
    gfn->is_grad  = 1;
    gfn->refcount = 1;

    Value *v = val_new(VT_FUNC);
    v->func = gfn;
    pthread_mutex_lock(&g_alloc_mutex);
    gfn->gc_next = g_all_funcs; g_all_funcs = gfn; g_gc_allocations++;
    pthread_mutex_unlock(&g_alloc_mutex);

    GC_UNROOT_ENV();
    env_decref(grad_env);
    return v;
}

/* time()     — current wall-clock time as milliseconds since Unix epoch
 * time(fn)   — call fn() with no args, return elapsed wall-clock ms      */
BUILTIN(time_fn) {
    struct timespec t;
    if (argc == 0) {
        clock_gettime(CLOCK_REALTIME, &t);
        return val_num(t.tv_sec * 1e3 + t.tv_nsec * 1e-6);
    }
    NEED(1);
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t);
    Signal sig = {SIG_NONE, NULL};
    Value *res = eval_call_val(ARG(0), NULL, NULL, 0, &sig, 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    val_decref(res);
    if (sig.type == SIG_ERROR) {
        char *s = sig.retval ? val_repr(sig.retval) : xstrdup("error");
        if (sig.retval) val_decref(sig.retval);
        die("time(): %s", s);
    }
    if (sig.type == SIG_RETURN && sig.retval) val_decref(sig.retval);
    return val_num((t1.tv_sec - t.tv_sec) * 1e3
                 + (t1.tv_nsec - t.tv_nsec) * 1e-6);
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
    Value *res = eval_call_val(fn, argv2, NULL, args_list->len, &sig, 0);
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

/* par() worker — calls fn(item) for each item in a slice */
typedef struct {
    Value  *fn_val;
    Value **items;
    int     start;
    int     end;
    Value **results;
    int     had_error;
    char    error[512];
} ParBuiltinArg;

static void *par_builtin_worker(void *varg) {
    ParBuiltinArg *arg = (ParBuiltinArg *)varg;
    Signal sig = {SIG_NONE, NULL};
    for (int i = arg->start; i < arg->end; i++) {
        if (arg->had_error) break;
        Value *item = arg->items[i];
        Value *res  = eval_call_val(arg->fn_val, &item, NULL, 1, &sig, 0);
        if (sig.type == SIG_ERROR) {
            char *s = sig.retval ? val_repr(sig.retval) : NULL;
            snprintf(arg->error, sizeof(arg->error), "%s",
                     s ? s : "error in par() function call");
            if (s) free(s);
            if (sig.retval) val_decref(sig.retval);
            val_decref(res);
            arg->had_error = 1;
        } else if (sig.type == SIG_RETURN) {
            val_decref(arg->results[i]);
            arg->results[i] = sig.retval ? sig.retval : val_none();
            sig.retval = NULL;
            sig.type   = SIG_NONE;
        } else {
            val_decref(arg->results[i]);
            arg->results[i] = res;
        }
    }
    return NULL;
}

BUILTIN(par_fn) {
    NEED(2);
    Value *lst = ARG(0);
    Value *fn  = ARG(1);
    if (lst->type != VT_LIST) die("par(): first argument must be a list");

    int n = lst->list->len;
    if (n == 0) {
        HowList *empty = list_new();
        Value *v = val_list(empty); list_decref(empty); return v;
    }

    Value **results = xmalloc((size_t)n * sizeof(Value *));
    for (int i = 0; i < n; i++) results[i] = val_none();

    int num_cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (num_cpus < 1) num_cpus = 1;
    int num_threads = n < num_cpus ? n : num_cpus;

    ParBuiltinArg *args = xmalloc((size_t)num_threads * sizeof(ParBuiltinArg));
    int chunk = n / num_threads;
    int rem   = n % num_threads;
    int pos   = 0;
    for (int t = 0; t < num_threads; t++) {
        int this_chunk      = chunk + (t < rem ? 1 : 0);
        args[t].fn_val      = fn;
        args[t].items       = lst->list->items;
        args[t].start       = pos;
        args[t].end         = pos + this_chunk;
        args[t].results     = results;
        args[t].had_error   = 0;
        args[t].error[0]    = '\0';
        pos += this_chunk;
    }

    __atomic_store_n(&g_gc_suspended, 1, __ATOMIC_RELEASE);

    pthread_t *threads = xmalloc((size_t)num_threads * sizeof(pthread_t));
    for (int t = 0; t < num_threads; t++)
        pthread_create(&threads[t], NULL, par_builtin_worker, &args[t]);
    for (int t = 0; t < num_threads; t++)
        pthread_join(threads[t], NULL);

    __atomic_store_n(&g_gc_suspended, 0, __ATOMIC_RELEASE);

    char *first_error = NULL;
    for (int t = 0; t < num_threads; t++)
        if (args[t].had_error && !first_error) first_error = args[t].error;

    free(threads);
    if (first_error) {
        char errbuf[512]; snprintf(errbuf, sizeof(errbuf), "%s", first_error);
        free(args);
        for (int i = 0; i < n; i++) val_decref(results[i]);
        free(results);
        die("%s", errbuf);
        return val_none();
    }
    free(args);

    HowList *out = list_new();
    for (int i = 0; i < n; i++) {
        list_push(out, results[i]);
        val_decref(results[i]);
    }
    free(results);
    Value *ret = val_list(out); list_decref(out);
    return ret;
}

/* ── Tensor builtins ─────────────────────────────────────────────────────── */

BUILTIN(tensor_fn) {
    return tensor_build_from_args(argc, argv);
}

BUILTIN(shape_fn) {
    NEED(1);
    return tensor_shape_value(ARG(0));
}

BUILTIN(transpose_fn) {
    NEED(1);
    return tensor_transpose_value(ARG(0));
}

BUILTIN(outer_fn) {
    NEED(2);
    return tensor_outer_value(ARG(0), ARG(1));
}

BUILTIN(zeros_fn) {
    NEED(1);
    return tensor_zeros_value(ARG(0));
}

BUILTIN(ones_fn) {
    NEED(1);
    return tensor_ones_value(ARG(0));
}

BUILTIN(eye_fn) {
    NEED(1);
    return tensor_eye_value(ARG(0));
}

BUILTIN(sum_fn) {
    NEED(1);
    if (ARG(0)->type == VT_TENSOR) {
        return tensor_sum_value(ARG(0));
    }
    if (ARG(0)->type == VT_LIST) {
        HowList *lst = ARG(0)->list;
        double s = 0;
        for (int i = 0; i < lst->len; i++) {
            if (lst->items[i]->type != VT_NUM) die("sum(): list elements must be numbers");
            s += lst->items[i]->nval;
        }
        return val_num(s);
    }
    die("sum() requires a tensor or list");
    return val_none();
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
    REG("chr",     chr_fn);
    REG("ord",     ord_fn);
    REG("type",    type_fn);
    REG("floor",   floor_fn);
    REG("ceil",    ceil_fn);
    REG("abs",     abs_fn);
    REG("sqrt",    sqrt_fn);
    REG("sin",     sin_fn);
    REG("cos",     cos_fn);
    REG("exp",     exp_fn);
    REG("log",     log_fn);
    REG("pow",     pow_fn);
    REG("list",    list_fn);
    REG("map",     map_fn);
    REG("reduce",  reduce_fn);
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
    REG("set_grad", set_grad_fn);
    REG("grad",    grad_builtin_fn);
    REG("time",    time_fn);
    REG("par",     par_fn);
    REG("_host_call",      host_call_fn);
    REG("_basename", basename_fn);
    REG("_dirname",  dirname_fn);
    REG("tensor",    tensor_fn);
    REG("shape",     shape_fn);
    REG("T",         transpose_fn);
    REG("outer",     outer_fn);
    REG("zeros",     zeros_fn);
    REG("ones",      ones_fn);
    REG("eye",       eye_fn);
    REG("sum",       sum_fn);
    /* bool() as a function */
    Value *t = val_bool(1); env_set(env,"true",t); val_decref(t);
    Value *f = val_bool(0); env_set(env,"false",f); val_decref(f);
    Value *n = val_none(); env_set(env,"none",n); val_decref(n);
    /* math constants */
    Value *pi_v = val_num(3.14159265358979323846); env_set(env,"pi",pi_v); val_decref(pi_v);
#undef REG
}
