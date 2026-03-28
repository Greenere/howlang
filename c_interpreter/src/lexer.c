#include "frontend.h"
#include "lexer_internal.h"

static void tl_push(TokenList *tl, Token t) {
    if (tl->len + 1 >= tl->cap) {
        tl->cap = tl->cap ? tl->cap * 2 : 256;
        tl->toks = xrealloc(tl->toks, tl->cap * sizeof(Token));
    }
    tl->toks[tl->len++] = t;
}

TokenList *lex(const char *src) {
    TokenList *tl = xmalloc(sizeof(*tl));
    tl->toks = NULL; tl->len = tl->cap = 0;
    int pos = 0, line = 1;
    int line_start = 0;   /* byte offset of line start, for col tracking */
    int n = strlen(src);

    while (pos < n) {
        /* skip whitespace */
        while (pos < n && (src[pos]==' '||src[pos]=='\t'||src[pos]=='\r'||src[pos]=='\n')) {
            if (src[pos] == '\n') { line++; line_start = pos + 1; }
            pos++;
        }
        if (pos >= n) break;
        char c = src[pos];

        /* comment */
        if (c == '#') {
            while (pos < n && src[pos] != '\n') pos++;
            continue;
        }
        /* semicolon — skip */
        if (c == ';') { pos++; continue; }

        Token t = {0}; t.line = line; t.col = pos - line_start + 1;

        /* number */
        if (isdigit(c)) {
            int start = pos;
            while (pos < n && isdigit(src[pos])) pos++;
            if (pos < n && src[pos] == '.' && pos+1 < n && isdigit(src[pos+1])) {
                pos++;
                while (pos < n && isdigit(src[pos])) pos++;
            }
            char tmp[64]; int len = pos - start;
            if (len >= 63) die_at(line, 0, "number literal too long");
            memcpy(tmp, src+start, len); tmp[len] = 0;
            t.type = TT_NUMBER; t.nval = atof(tmp);
            tl_push(tl, t); continue;
        }

        /* string */
        if (c == '"' || c == '\'') {
            char q = src[pos++]; Buf b = {0};
            int is_raw = (q == '\'');
            while (pos < n && src[pos] != q) {
                if (src[pos] == '\\') {
                    pos++;
                    char e = src[pos++];
                    switch(e) {
                        case 'n': buf_push(&b,'\n'); break;
                        case 't': buf_push(&b,'\t'); break;
                        case 'r': buf_push(&b,'\r'); break;
                        case '\\': buf_push(&b,'\\'); break;
                        case '"':  buf_push(&b,'"');  break;
                        case '\'': buf_push(&b,'\''); break;
                        default:   buf_push(&b,e);    break;
                    }
                } else {
                    if (src[pos] == '\n') line++;
                    buf_push(&b, src[pos++]);
                }
            }
            if (pos < n) pos++; /* closing quote */
            t.type = TT_STRING; t.sval = buf_done(&b); t.raw = is_raw;
            tl_push(tl, t); continue;
        }

        /* ident / keyword */
        if (isalpha(c) || c == '_') {
            int start = pos;
            while (pos < n && (isalnum(src[pos]) || src[pos] == '_')) pos++;
            int len = pos - start;
            char tmp[256];
            if (len >= 255) die_at(line, 0, "identifier too long");
            memcpy(tmp, src+start, len); tmp[len] = 0;
            if      (!strcmp(tmp,"var"))   { t.type = TT_VAR; }
            else if (!strcmp(tmp,"true"))  { t.type = TT_BOOL; t.bval = 1; }
            else if (!strcmp(tmp,"false")) { t.type = TT_BOOL; t.bval = 0; }
            else if (!strcmp(tmp,"none"))  { t.type = TT_NONE; }
            else if (!strcmp(tmp,"and"))   { t.type = TT_AND; }
            else if (!strcmp(tmp,"or"))    { t.type = TT_OR; }
            else if (!strcmp(tmp,"not"))   { t.type = TT_NOT; }
            else if (!strcmp(tmp,"break")) { t.type = TT_BREAK; }
            else if (!strcmp(tmp,"continue")) { t.type = TT_CONTINUE; }
            else if (!strcmp(tmp,"how"))   { t.type = TT_HOW; }
            else if (!strcmp(tmp,"where")) { t.type = TT_WHERE; }
            else if (!strcmp(tmp,"as"))    { t.type = TT_AS; }
            else if (!strcmp(tmp,"catch")) { t.type = TT_CATCH; }
            else { t.type = TT_IDENT; t.sval = xstrdup(tmp); }
            tl_push(tl, t); continue;
        }

        pos++; /* consume c */

#define TWO(a,b,r) if (pos < n && src[pos]==(b)) { pos++; t.type=(r); tl_push(tl,t); continue; }
        switch(c) {
        case ':': TWO(':',':',TT_DCOLON); t.type=TT_COLON; break;
        case '=': TWO('=','=',TT_EQEQ);  t.type=TT_EQ;    break;
        case '!':
            if (pos < n && src[pos]=='!') { pos++; t.type=TT_DBANG; break; }
            TWO('!','=',TT_NEQ); t.type=TT_NOT; break;
        case '<': TWO('<','=',TT_LTE);   t.type=TT_LT;    break;
        case '>': TWO('>','=',TT_GTE);   t.type=TT_GT;    break;
        case '+': TWO('+','=',TT_PLUSEQ);    t.type=TT_PLUS;    break;
        case '-': TWO('-','=',TT_MINUSEQ);   t.type=TT_MINUS;   break;
        case '*': TWO('*','=',TT_STAREQ);    t.type=TT_STAR;    break;
        case '/': TWO('/','=',TT_SLASHEQ);   t.type=TT_SLASH;   break;
        case '%': TWO('%','=',TT_PERCENTEQ); t.type=TT_PERCENT; break;
        case '&':
            if (pos < n && src[pos] == '&') {
                pos++;
                t.type = TT_AND;
            } else {
                die_at(line, pos - line_start + 1, "unexpected '&' — use '&&' for logical and");
            }
            break;
        case '|':
            if (pos < n && src[pos] == '|') {
                pos++;
                t.type = TT_OR;
            } else {
                die_at(line, pos - line_start + 1, "unexpected '|' — use '||' for logical or");
            }
            break;
        case '(': t.type=TT_LPAREN;   break;
        case ')': t.type=TT_RPAREN;   break;
        case '{': t.type=TT_LBRACE;   break;
        case '}': t.type=TT_RBRACE;   break;
        case '[': t.type=TT_LBRACKET; break;
        case ']': t.type=TT_RBRACKET; break;
        case ',': t.type=TT_COMMA;    break;
        case '.': t.type=TT_DOT;      break;
        case '^': t.type=TT_CARET; break;
        default: die_at(line, pos - line_start, "unexpected character '%c'", c);
        }
#undef TWO
        tl_push(tl, t);
    }
    Token eof = {TT_EOF, NULL, 0, 0, line, pos - line_start + 1, 0};
    tl_push(tl, eof);
    return tl;
}
