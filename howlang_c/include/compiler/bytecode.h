/*
 * bytecode.h — minimal bytecode representation for the first compiler slice.
 *
 * This starts with a deliberately small instruction set so we can get an
 * end-to-end compile/disassemble/run path working before tackling closures,
 * classes, catch, or full feature parity.
 */
#ifndef HOWLANG_BYTECODE_H
#define HOWLANG_BYTECODE_H

#include "interpreter/runtime_internal.h"
#include <stdint.h>
#include <stdio.h>

typedef enum {
    OP_LOAD_CONST = 1,
    OP_LOAD_GLOBAL,
    OP_DEFINE_GLOBAL,
    OP_STORE_GLOBAL,
    OP_MAKE_FUNC_AST,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_NEG,
    OP_EQ,
    OP_NE,
    OP_LT,
    OP_LE,
    OP_GT,
    OP_GE,
    OP_NOT,
    OP_JUMP,
    OP_JUMP_IF_FALSE,
    OP_CALL,
    OP_POP,
    OP_RETURN,
    OP_RETURN_NONE,
} OpCode;

typedef struct Proto {
    uint32_t  *code;
    int        code_len;
    int        code_cap;
    Value    **consts;
    int        nconsts;
    int        const_cap;
    Node     **ast_nodes;
    int        nast_nodes;
    int        ast_cap;
    char      *name;
    int       *lines;
} Proto;

Proto *proto_new(const char *name);
void   proto_free(Proto *proto);
int    proto_add_const(Proto *proto, Value *v);
int    proto_add_const_string(Proto *proto, const char *s);
int    proto_add_ast(Proto *proto, Node *node);
void   proto_emit_abc(Proto *proto, OpCode op, int a, int b, int c, int line);
int    proto_emit_jump(Proto *proto, OpCode op, int line);
void   proto_patch_jump(Proto *proto, int at, int target);
void   proto_disassemble(FILE *out, const Proto *proto);

#define BC_ENCODE(op, a, b, c) \
    ((((uint32_t)(op) & 0xffu) << 24) | (((uint32_t)(a) & 0xffu) << 16) | \
     (((uint32_t)(b) & 0xffu) << 8) | ((uint32_t)(c) & 0xffu))
#define BC_OP(i) (((i) >> 24) & 0xffu)
#define BC_A(i)  (((i) >> 16) & 0xffu)
#define BC_B(i)  (((i) >> 8) & 0xffu)
#define BC_C(i)  ((i) & 0xffu)

#endif
