cmake_minimum_required(VERSION 3.15)
project(FastGraphRAG VERSION 0.1.0 LANGUAGES C CXX)

# use C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(TREE_SITTER REQUIRED tree-sitter)

# compile the Tree-sitter C++ grammar
# grammar consists of a parser.c and a scanner.c file
add_library(tree-sitter-cpp STATIC
    external/tree-sitter-cpp/src/parser.c
    external/tree-sitter-cpp/src/scanner.c
)
target_include_directories(tree-sitter-cpp PRIVATE external/tree-sitter-cpp/src)

#so c++ can find hnswlib headers
include_directories(external/hnswlib)

# main executable
add_executable(fastgraphrag src/main.cpp)

# link program to the grammar and the core Tree-sitter library
target_link_libraries(fastgraphrag PRIVATE 
    tree-sitter-cpp 
    ${TREE_SITTER_LIBRARIES}
)
target_include_directories(fastgraphrag PRIVATE ${TREE_SITTER_INCLUDE_DIRS})