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
    target_include_directories(${name} PRIVATE examples/security)
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    add_test(NAME ${name} COMMAND ${name})
endforeach()

# --- End-to-end security example ---
find_package(Python3 REQUIRED)
add_custom_command(
    OUTPUT ${CMAKE_SOURCE_DIR}/examples/security/registry_gen.cpp
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_registry.py
            ${CMAKE_SOURCE_DIR}/examples/security/schema.json
            ${CMAKE_SOURCE_DIR}/examples/security/registry_gen.cpp
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen_registry.py
            ${CMAKE_SOURCE_DIR}/examples/security/schema.json
    COMMENT "codegen: generating registry_gen.cpp from schema.json")
# registry_gen.cpp is #included textually into main.cpp (matching the drift-test
# pattern), NOT compiled as a separate TU — the generated file references Event/
# PersonInfo/Box/MyDeviceCtx which are only in scope via event.h/device_ctx.h
# included ahead of it in main.cpp. The custom target drives regeneration when
# schema.json changes; the committed registry_gen.cpp is the source of truth for
# a clean checkout.
add_custom_target(security_codegen
    DEPENDS ${CMAKE_SOURCE_DIR}/examples/security/registry_gen.cpp)
add_executable(security_example
    examples/security/main.cpp
    ${SV_SRCS})
target_include_directories(security_example PRIVATE examples/security include third_party)
target_compile_options(security_example PRIVATE -Wall -Wextra)
add_dependencies(security_example security_codegen)

# --- Route A vs Route B benchmark (not a test; run manually) ---
add_executable(bench_routes
    bench/bench_routes.cpp
    ${SV_SRCS})
target_include_directories(bench_routes PRIVATE examples/security include third_party)
target_compile_options(bench_routes PRIVATE -O2)

# --- Recipe Builder UI server ---
add_executable(recipe_server
    examples/security/server_main.cpp
    ${SV_SRCS})
target_include_directories(recipe_server PRIVATE
    examples/security include third_party)
target_compile_options(recipe_server PRIVATE -Wall -Wextra)
add_dependencies(recipe_server security_codegen)
