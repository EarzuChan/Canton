cmake_minimum_required(VERSION 3.24)
include(cmake/CantonVersion.cmake)
project(Canton VERSION "${CANTON_NUMERIC_VERSION}" LANGUAGES C CXX)

# Version the output so reusing a build tree cannot pick up another Framework's protocol.
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/canton-runtime-protocol/${CANTON_VERSION}.txt"
  CONTENT "${CANTON_PROTOCOL_VERSION}\n")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(cmake/Dependencies.cmake)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(CANTON_IS_TOP_LEVEL ON)
else()
  set(CANTON_IS_TOP_LEVEL OFF)
endif()

# 宿主以 /MT 交付；qjs 与 Canton 各库同用静态 CRT，字节码工具和宿主才共享同一份引擎产物
if(CANTON_IS_TOP_LEVEL AND MSVC)
  set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

canton_fetch_quickjs_ng()
canton_fetch_source(miniz)
canton_fetch_source(lunasvg)

add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

add_subdirectory(native)

option(CANTON_BUILD_TESTS "Build Canton C++ tests" ${CANTON_IS_TOP_LEVEL})
if(CANTON_BUILD_TESTS)
  enable_testing()
  add_subdirectory(native-tests)
endif()
