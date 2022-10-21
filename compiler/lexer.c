#include <stdlib.h>
#include <stdio.h>

#define _id 1
#define _key 2
#define _literal 3
#define _empty -1

typedef struct condition_jump {
    int condition;
    void * next;
} jump_t;

typedef struct auto_node {
    int type;
    int jump_num;
    jump_t** jumps;
} node_t;

typedef struct automata {
    node_t * start;
    node_t ** node_list;
    int node_num;
} automata_t;

node_t * get_node(int type){
    node_t * new_node = (node_t *)malloc(sizeof(node_t));
    if (NULL == new_node) exit(1);

    new_node->type = type;
    new_node->jump_num = 0;
    new_node->jumps = NULL;

    return new_node;
}

void add_condition_jump(node_t * source, node_t * target, int condition){
    jump_t * new_jump = (jump_t *) malloc(sizeof(jump_t));
    if (NULL == new_jump) exit(1);
    new_jump->condition = condition;
    new_jump->next = (void *) target;

    if (source->jump_num == 0){
        source->jumps = (jump_t **)malloc(sizeof(jump_t *));
        if (NULL == source->jumps) exit(1);
    } else {
        source->jumps = (jump_t **)realloc(source->jumps, (source->jump_num + 1)*sizeof(jump_t *));
        if (NULL == source->jumps) exit(1);
    }
    source->jumps[source->jump_num] = new_jump;
    source->jump_num ++;
}

node_t * jump(node_t * current, int condition){
    for(int i = 0; i < current->jump_num; i++){
        if (current->jumps[i]->condition == condition){
            return (node_t *) current->jumps[i]->next;
        }
    }
    return NULL;
}

void link_node(automata_t * at, node_t * node){
    if (at->node_num == 0) {
        at->node_list = (node_t **)malloc(sizeof(node_t *));
        if (NULL == at->node_list) exit(1);
    } else {
        at->node_list = (node_t **)realloc(at->node_list,(at->node_num + 1)*sizeof(node_t *));
        if (NULL == at->node_list) exit(1);
    }
    at->node_list[at->node_num] = node;
    at->node_num ++;
}

automata_t * get_automata(){
    automata_t * at = (automata_t *)malloc(sizeof(automata_t));
    at->start = NULL;
    at->node_list = NULL;
    at->node_num = 0;
    return at;
}

void free_automata(automata_t * at){
    node_t * cur_node = NULL;
    for (int i = 0; i < at->node_num; i++){
        cur_node = at->node_list[i];
        for (int j = 0; j < cur_node->jump_num; j++){
            free(cur_node->jumps[j]);
        }
        free(cur_node->jumps);
        free(cur_node);
    }
    free(at->node_list);
    free(at);
}

node_t * get_how_node(automata_t * at, int type){
    link_node(at, get_node(type));
    return at->node_list[at->node_num-1];
}

void build_how_automata(automata_t * at){
    node_t * start = get_how_node(at, _empty);
    node_t * id = get_how_node(at, _id);
    node_t * key = get_how_node(at, _key);
    node_t * literal = get_how_node(at, _literal);

    /* Add operators
    */
    add_condition_jump(start, key, '+');
    add_condition_jump(start, key, '-');
    add_condition_jump(start, key, '*');
    add_condition_jump(start, key, '/');
    add_condition_jump(start, key, '=');

    /* Add literal number
    */
    node_t * int_digit = get_how_node(at, _empty);
    node_t * float_digit = get_how_node(at, _empty);
    node_t * tail_digit = get_how_node(at, _empty);
    add_condition_jump(start, float_digit, '0');
    for (int i = 0; i < 8; i++) add_condition_jump(start, int_digit, '1' + i);
    add_condition_jump(float_digit, tail_digit, '.');
    add_condition_jump(int_digit, tail_digit, '.');
    for (int i = 0; i < 9; i++) add_condition_jump(tail_digit, tail_digit, '0' + i);
    for (int i = 0; i < 9; i++) add_condition_jump(int_digit, int_digit, '0' + i);
    add_condition_jump(tail_digit, literal, ' ');
    add_condition_jump(int_digit, literal, ' ');

    /* Add literal string
    */
    node_t * string_node = get_how_node(at, _empty);
    add_condition_jump(start, string_node, "'");
    for (int i = 0; i < 26; i++) add_condition_jump(string_node, string_node, "a" + i);
    add_condition_jump(string_node, literal, "'");

    at->start = start;
}

int main(int argc, char**argv){
    automata_t * at = get_automata();
    build_how_automata(at);
    node_t * next = jump(at->start, '+');
    printf("Terminal Type: %d\n", next->type);
    free_automata(at);
    return 0;
}

