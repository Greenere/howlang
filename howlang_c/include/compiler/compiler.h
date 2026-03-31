/*
 * compiler.h — AST → bytecode compiler entry points.
 */
#ifndef HOWLANG_COMPILER_H
#define HOWLANG_COMPILER_H

#include "compiler/bytecode.h"
#include "compiler/sema.h"

Proto *how_compile(Node *prog, SemaCtx *sema, char **error_out);

#endif
