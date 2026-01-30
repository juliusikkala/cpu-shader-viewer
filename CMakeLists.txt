cmake_minimum_required(VERSION 3.20)

project(Raybase LANGUAGES C CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
   add_compile_options(-fdiagnostics-color=always)
elseif("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
   add_compile_options(-fcolor-diagnostics)
endif()

set(SLANG_SLANG_LLVM_FLAVOR USE_SYSTEM_LLVM CACHE STRING "" FORCE)
add_subdirectory(external/slang)

find_package(SDL3 REQUIRED CONFIG)
find_package(OpenMP)

add_executable(cpu-shader-viewer main.cc)
target_link_libraries(cpu-shader-viewer SDL3::SDL3 slang OpenMP::OpenMP_CXX)
add_dependencies(cpu-shader-viewer slang-glsl-module)

set_property(TARGET cpu-shader-viewer PROPERTY CXX_STANDARD 20)
set_property(TARGET cpu-shader-viewer PROPERTY CXX_STANDARD_REQUIRED ON)
set_property(TARGET cpu-shader-viewer PROPERTY CXX_EXTENSIONS OFF)
