#include "frontend.h"
#include "ast.h"

/* ─────────────────────────────────────────────────────────────────────────── */
/*  AST helpers                                                                 */
/* ─────────────────────────────────────────────────────────────────────────── */

void nl_push(NodeList *nl, Node *n) {
    if (nl->len + 1 >= nl->cap) {
        nl->cap = nl->cap ? nl->cap*2 : 8;
        nl->nodes = xrealloc(nl->nodes, nl->cap * sizeof(Node*));
    }
    nl->nodes[nl->len++] = n;
}
void sl_push(StrList *sl, char *s) {
    if (sl->len + 1 >= sl->cap) {
        sl->cap = sl->cap ? sl->cap*2 : 4;
        sl->s = xrealloc(sl->s, sl->cap * sizeof(char*));
    }
    sl->s[sl->len++] = s;
}
void mil_push(MapItemList *ml, Node *k, Node *v) {
    if (ml->len + 1 >= ml->cap) {
        ml->cap = ml->cap ? ml->cap*2 : 8;
        ml->items = xrealloc(ml->items, ml->cap * sizeof(MapItem));
    }
    ml->items[ml->len].key = k;
    ml->items[ml->len].val = v;
    ml->len++;
}
Node *make_node(NodeType t, int line) {
    Node *n = xmalloc(sizeof(Node));
    memset(n, 0, sizeof(*n));
    n->type = t;
    n->line = line;
    return n;
}
