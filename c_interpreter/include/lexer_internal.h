/*
 * lexer_internal.h — token types and lexer interface shared between lexer.c
 *                    and parser.c.  Not part of the public API.
 */
#ifndef HOWLANG_LEXER_INTERNAL_H
#define HOWLANG_LEXER_INTERNAL_H

typedef enum
{
    TT_EOF,
    TT_NUMBER,
    TT_STRING,
    TT_BOOL,
    TT_NONE,
    TT_IDENT,
    TT_VAR,
    TT_BREAK,
    TT_CONTINUE,
    TT_HOW,
    TT_WHERE,
    TT_AS,
    TT_LPAREN,
    TT_RPAREN,
    TT_LBRACE,
    TT_RBRACE,
    TT_LBRACKET,
    TT_RBRACKET,
    TT_COMMA,
    TT_DOT,
    TT_COLON,
    TT_DCOLON,
    TT_PLUS,
    TT_MINUS,
    TT_STAR,
    TT_SLASH,
    TT_PERCENT,
    TT_EQ,
    TT_PLUSEQ,
    TT_MINUSEQ,
    TT_STAREQ,
    TT_SLASHEQ,
    TT_PERCENTEQ,
    TT_EQEQ,
    TT_NEQ,
    TT_LT,
    TT_GT,
    TT_LTE,
    TT_GTE,
    TT_AND,
    TT_OR,
    TT_NOT,
    TT_DBANG, /* !! — throw operator */
    TT_CATCH, /* catch keyword */
} TT;

typedef struct
{
    TT type;
    char *sval;  /* string / ident value */
    double nval; /* numeric value */
    int bval;    /* bool value */
    int line;
    int col; /* column number (1-based) */
    int raw; /* 1 = single-quoted string (no interpolation) */
} Token;

typedef struct
{
    Token *toks;
    int len;
    int cap;
} TokenList;

TokenList *lex(const char *src);

#endif
