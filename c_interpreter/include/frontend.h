/*
 * frontend.h — public parser API.
 *   Lexes and parses howlang source text into an AST rooted at N_PROG.
 */
#ifndef HOWLANG_FRONTEND_H
#define HOWLANG_FRONTEND_H

#include "common.h"

typedef struct Node Node;
Node *how_parse_source(const char *src);

#endif
