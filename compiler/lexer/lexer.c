#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define _max_buffer 1024

#define _id 1
#define _key 2
#define _int 3
#define _float 4
#define _string 5
#define _operator 6
#define _separator 7
#define _space 8

typedef struct token {
    int type;
    int line;
    int left;
    int right;
} token_t;

int is_identifier_char(char string){
    if (string >= '0' && string <= '9') return 1;
    if (string >= 'a' && string <= 'z') return 1;
    if (string >= 'A' && string <= 'Z') return 1;
    return 0;
}

int lexer_space(char string){
    if (string == ' '
     || string == '\n') return 1;
    return 0;
}

int lexer_separator(char string){
    if (string == '('
      ||string == ')'
      ||string == '['
      ||string == ']'
      ||string == '{'
      ||string == '}'
      ||string == ';'
      ||string == ',') return 1;
    return 0;
}

int lexer_operator(char string){
    if (string == '+'
      ||string == '-'
      ||string == '*'
      ||string == '/'
      ||string == ':'
      ||string == '&'
      ||string == '|'
      ||string == '!'
      ||string == '.'
      ||string == '=') return 1;
    return 0;
}

int lexer_id(token_t * t, char * string, int cur){
    if (string[cur] == '\0' 
     || lexer_space(string[cur])
     || lexer_operator(string[cur])
     || lexer_separator(string[cur])){
         t->right = cur;
         t->type = _id;
         return cur;
     } 
    return lexer_id(t, string,cur + 1);
}

int lexer_key(token_t * t, char * string, int cur, int state){
    if (string[cur] == '\0'
     || lexer_space(string[cur])
     || lexer_operator(string[cur])
     || lexer_separator(string[cur])) {
         t->right = cur;
         t->type = _key;
         return cur;
    }
    if (string[cur] == 'v'){
        return lexer_key(t, string, cur + 1, 1);
    } else if (string[cur] == 'a' && state == 1) {
        return lexer_key(t, string, cur + 1, 2);
    } else if (string[cur] == 'r' && state == 2) {
        return lexer_key(t, string, cur + 1, 3);
    }
    return lexer_id(t, string, cur + 1);
}

int lexer_string(token_t * t, char * string, int cur, int state){
    //printf("Reading %c at %d\n", string[cur], cur);
    if (string[cur] == '\0') return -1;
    if ((string[cur] == '\"' && state == 1)
     || (string[cur] == '\'' && state == 2)){
         t->right = cur;
         t->type = _string;
         return cur + 1;
    }
    return lexer_string(t, string,cur + 1, state);
}

int lexer_number(token_t *t, char * string, int cur, int state){
    if (string[cur] == '\0'
      || lexer_space(string[cur])
      || (string[cur] != '.' && lexer_operator(string[cur]))
      || lexer_separator(string[cur])) {
        t->right = cur;
        t->type = (state == 2)?_float:_int;
        return cur;
    }
    if (string[cur] == '0') {
        if (state == 0){
            return lexer_number(t, string, cur + 1, 1);
        } else if(state == 1){
            return -1;
        } else {
            return lexer_number(t, string, cur + 1, state);
        }
    } else if (string[cur] == '.') {
        if (state == 2 ) {
            return -1;
        } else {
            return lexer_number(t, string, cur + 1, 2);
        }
    } else if (string[cur] >= '1' && string[cur] <= '9') {
        if (state == 1) {
           return -1;
        } else {
           return lexer_number(t, string, cur + 1, state);
        }
    }
    return -1;
}

int lexer(token_t * t, char * string, int cur){
    if (string[cur] == '\0') return -1;
    if (string[cur] >= '0' && string[cur] <= '9') {
        t->left = cur;
        return lexer_number(t, string, cur, 0);
    } else if (string[cur] == 'v') {
        t->left = cur;
        return lexer_key(t, string, cur, 0);
    } else if (string[cur] == '\''){
        t->left = cur + 1;
        return lexer_string(t, string,cur + 1, 2);
    } else if (string[cur] == '\"'){
        t->left = cur + 1;
        return lexer_string(t, string, cur + 1, 1);
    } else if (lexer_operator(string[cur])){
        t->left = cur;
        t->right = cur + 1;
        t->type = _operator;
        return cur + 1;
    } else if (lexer_separator(string[cur])){
        t->left = cur;
        t->right = cur + 1;
        t->type = _separator;
        return cur + 1;
    } else if (lexer_space(string[cur])){
        t->left = cur;
        t->right = cur + 1;
        t->type = _space;
        return cur + 1;
    } else {
        t->left = cur;
        return lexer_id(t, string, cur);
    }
}

void append_token(FILE * fp, token_t * t, char * string){
    if (t->type == _space)return;
    fprintf(fp, "%d,%d:%d,%d,", t->line, t->left, t->type, t->right - t->left);
    for(int i = t->left; i < t->right; i++){
        if (string[i] != '\n'){
            fprintf(fp, "%c", string[i]);
        } else {
            fprintf(fp, "\\n");
        }
    }
    fprintf(fp, "\n");
}

int read_line(FILE * fp, char * buffer){
    char nex;
    int index = 0;
    while (!feof(fp)){
        nex = fgetc(fp);
        buffer[index++] = nex;
        if (nex == '\n'){
            buffer[index] = '\0';
            return index;
        }
    }
    return -1;
}

void lexer_how_code(const char * source, const char * target){
    FILE * cf = fopen(source, "r");
    FILE * tf = fopen(target, "w");

    char buffer[_max_buffer];
    token_t token;
    int cur = 0;
    int nex = -1;
    int line = 1;
    while (!feof(cf)) {
        cur = 0;
        if (read_line(cf, buffer) < 0) break;
        while(buffer[cur] != '\0') {
            token.left = cur;
            token.line = line;
            if (buffer[cur] == '\n') line++;
            nex = lexer(&token, buffer, cur);
            if (nex == -1){
                printf("Error: line %d, column %d, char %c...\n", line, cur, buffer[cur]);
                fclose(tf);
                exit(1);
            } else {
                append_token(tf, &token, buffer);
            }
            if (buffer[nex] == '\0') break;
            cur = nex;
        }
    }
    fclose(cf);
    fclose(tf);
}

int main(int argc, char * argv[]){
    lexer_how_code("../../samples/sample.how","./results/temp.tokens");
    return 0;
}