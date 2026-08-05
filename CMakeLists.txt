cmake_minimum_required(VERSION 3.14)
project(struct_view CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(struct_view INTERFACE)
target_include_directories(struct_view INTERFACE
    include
    third_party
)

enable_testing()

# All framework sources (grows as tasks add src/*.cpp). Empty at Task 1 — fine.
file(GLOB SV_SRCS CONFIGURE_DEPENDS src/*.cpp)
# Shared doctest main — compiled into every test exe, NOT a test itself.
# tests/main.cpp must exist (created in Step 3).
# Each test file is its own executable.
file(GLOB SV_TESTS CONFIGURE_DEPENDS tests/test_*.cpp)
foreach(t ${SV_TESTS})
    get_filename_component(name ${t} NAME_WE)
    add_executable(${name} ${t} ${SV_SRCS} tests/main.cpp)
    target_link_libraries(${name} PRIVATE struct_view)
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    add_test(NAME ${name} COMMAND ${name})
endforeach()
