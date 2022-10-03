#include <stdlib.h>
#include <stdio.h>

typedef struct auto_node {
    bool terminal;
    unsigned int next[128];
} anode_t;

typedef struct finite_automata {
    anode_t * start;
    anode_t * end;
} fa_t;

fa_t * h_single_char(char s) {
    anode_t * start =  (anode_t *) malloc(sizeof(anode_t));
    if (NULL == start) exit(1);
    anode_t * end =  (anode_t *) malloc(sizeof(anode_t));
    if (NULL == end) exit(1);
    start->terminal =false;
    start->next[(int)s] = (unsigned int) end;
    end->terminal = true;

    fa_t * fa = (fa_t*) malloc(sizeof(fa_t));
    if (NULL ==fa) exit(1);
    fa->start = start;
    fa->end = end;
    return fa;
}

fa_t * h_concat(fa_t *a, fa_t* b) {
    
}