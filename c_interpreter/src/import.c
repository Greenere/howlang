/*
 * import.c — Module import path management and module loading.
 *
 * Exports (declared in runtime_internal.h):
 *   import_dirs, import_dirs_len
 *   how_add_import_dir(), find_how_file(), exec_import()
 */
#include "runtime_internal.h"
#include "frontend.h"
#include <string.h>

/* ── Import search path globals ──────────────────────────────────────────── */

char **import_dirs     = NULL;
int    import_dirs_len = 0;
static int import_dirs_cap = 0;

/* ── Import path management ──────────────────────────────────────────────── */

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

/* ── Module loading ──────────────────────────────────────────────────────── */

void exec_import(const char *modname, const char *alias, Env *env) {
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
