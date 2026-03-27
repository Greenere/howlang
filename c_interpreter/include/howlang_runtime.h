#ifndef HOWLANG_RUNTIME_H
#define HOWLANG_RUNTIME_H
#include "howlang_common.h"
#include "howlang_ast.h"
Env *how_runtime_bootstrap(int argc, char **argv);
Env *how_globals(void);
void how_runtime_shutdown(void);
void how_run_source(const char *name, const char *src, Env *env);
void how_add_import_dir(const char *dir);
int how_num_builtins(void);
void repl(Env *env);
#endif
