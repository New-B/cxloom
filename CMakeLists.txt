cmake_minimum_required(VERSION 3.20)

project(CXLoom VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(cxloom_common STATIC
    src/common/config.cpp
    src/common/tracing.cpp
)
target_include_directories(cxloom_common PUBLIC include)

add_library(cxloom_loommem STATIC
    src/loommem/allocator.cpp
    src/loommem/coherence.cpp
    src/loommem/queue.cpp
    src/loommem/runtime.cpp
)
target_include_directories(cxloom_loommem PUBLIC include)
target_link_libraries(cxloom_loommem PUBLIC cxloom_common)

add_library(cxloom_loompar STATIC
    src/loompar/barrier.cpp
    src/loompar/runtime.cpp
    src/loompar/scheduler.cpp
    src/loompar/threading.cpp
)
target_include_directories(cxloom_loompar PUBLIC include)
target_link_libraries(cxloom_loompar PUBLIC cxloom_common cxloom_loommem)

add_library(cxloom STATIC src/api/cxloom_mem.cpp)
target_include_directories(cxloom PUBLIC include)
target_link_libraries(cxloom PUBLIC cxloom_loommem)

add_executable(cxloom_smoke examples/smoke_main.cpp)
target_link_libraries(cxloom_smoke PRIVATE cxloom_common cxloom_loommem cxloom_loompar)

enable_testing()

add_executable(cxloom_allocator_test tests/allocator_test.cpp)
target_link_libraries(cxloom_allocator_test PRIVATE cxloom_loommem)
add_test(NAME cxloom_allocator_test COMMAND cxloom_allocator_test)

add_executable(cxloom_cl_mem_api_test tests/cl_mem_api_test.c)
target_link_libraries(cxloom_cl_mem_api_test PRIVATE cxloom)
set_target_properties(cxloom_cl_mem_api_test PROPERTIES LINKER_LANGUAGE CXX)
add_test(NAME cxloom_cl_mem_api_test COMMAND cxloom_cl_mem_api_test)
