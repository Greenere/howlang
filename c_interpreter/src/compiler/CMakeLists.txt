add_library(howlang_compiler STATIC
    sema.c
    compiler.c
    vm.c
)

target_link_libraries(howlang_compiler PUBLIC howlang_frontend)
