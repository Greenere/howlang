/*
 * parallel.c — Parallel for-range loop (^{} syntax) using pthreads.
 *
 * Exports (declared in interpreter/runtime_internal.h):
 *   run_parallel_loop()
 */
#include "interpreter/runtime_internal.h"
#include <unistd.h>

/* ── Per-thread argument block ───────────────────────────────────────────── */

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

/* ── Worker thread ───────────────────────────────────────────────────────── */

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

/* ── Parallel loop entry point ───────────────────────────────────────────── */

Value *run_parallel_loop(HowFunc *fn, Signal *sig) {
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
