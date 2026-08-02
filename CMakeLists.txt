cmake_minimum_required(VERSION 3.16)
project(limit_order_book LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(LOB_STRICT "Treat warnings as errors" OFF)
option(LOB_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow)

if(LOB_STRICT)
  add_compile_options(-Werror)
endif()

if(LOB_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

# Core matching-engine library.
add_library(lob STATIC src/order_book.cpp)
target_include_directories(lob PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Demo CLI.
add_executable(lob_demo src/main.cpp)
target_link_libraries(lob_demo PRIVATE lob)

# Unit tests.
add_executable(lob_tests tests/test_order_book.cpp)
target_link_libraries(lob_tests PRIVATE lob)

# Benchmark.
add_executable(lob_bench bench/benchmark.cpp)
target_link_libraries(lob_bench PRIVATE lob)

enable_testing()
add_test(NAME unit_tests COMMAND lob_tests)
