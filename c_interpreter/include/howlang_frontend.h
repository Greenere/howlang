#ifndef HOWLANG_FRONTEND_H
#define HOWLANG_FRONTEND_H
#include "howlang_common.h"
typedef struct Node Node;
Node *how_parse_source(const char *src);
#endif
