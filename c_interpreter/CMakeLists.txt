cmake_minimum_required(VERSION 3.16)
project(howlang C)
set(CMAKE_C_STANDARD 11)
include_directories(include)

add_compile_options(-Wall -Wextra -Wpedantic)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fsanitize=address,undefined -g)
    add_link_options(-fsanitize=address,undefined)
endif()
find_package(Threads REQUIRED)
add_library(howlang_frontend STATIC
    src/frontend/common.c
    src/frontend/lexer.c
    src/frontend/parser.c
)
add_subdirectory(src/compiler)
add_library(howlang_runtime STATIC
    src/core/runtime.c
    src/core/gc.c
    src/runtime/tensor.c
    src/runtime/ad.c
    src/runtime/import.c
    src/runtime/parallel.c
    src/runtime/call.c
    src/runtime/builtins.c
)
target_link_libraries(howlang_runtime PUBLIC howlang_frontend m Threads::Threads)
target_link_libraries(howlang_compiler PUBLIC howlang_runtime)
add_executable(howlang src/core/driver.c)
target_link_libraries(howlang PRIVATE howlang_compiler howlang_runtime howlang_frontend m Threads::Threads)
