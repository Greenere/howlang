/*
 * vm.h — tiny stack VM for the initial compiler subset.
 */
#ifndef HOWLANG_VM_H
#define HOWLANG_VM_H

#include "bytecode.h"

typedef struct VM VM;

VM    *vm_new(Env *globals);
void   vm_free(VM *vm);
Value *vm_run(VM *vm, const Proto *proto);

#endif
