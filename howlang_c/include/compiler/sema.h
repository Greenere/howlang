/*
 * sema.h — semantic analysis / name-resolution pass for the frontend AST.
 *
 * Phase 1 goal:
 *   - attach basic name-resolution metadata to identifier sites
 *   - collect closure metadata for function/class/for-range nodes
 *   - report static issues such as duplicate named arguments
 */
#ifndef HOWLANG_SEMA_H
#define HOWLANG_SEMA_H

#include "shared/ast.h"
#include <stdio.h>

typedef struct SemaCtx SemaCtx;

/* Allocate / free a semantic-analysis context and its collected diagnostics. */
SemaCtx *sema_new(void);

/* Resolve names and attach compiler metadata to the AST in place. */
void     sema_resolve(Node *prog, SemaCtx *ctx);

/* Convenience helpers for build tooling / future compiler driver code. */
int      sema_ok(const SemaCtx *ctx);
int      sema_error_count(const SemaCtx *ctx);
void     sema_print_errors(FILE *f, const SemaCtx *ctx);
void     sema_free(SemaCtx *ctx);

#endif
