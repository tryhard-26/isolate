cmake_minimum_required(VERSION 3.20)
project(isolate
        VERSION 0.1.0
        DESCRIPTION "Process capsules for macOS"
        LANGUAGES CXX)

if(NOT APPLE)
  message(FATAL_ERROR "isolate targets macOS only; it is built on XNU sandbox and APFS facilities")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

add_library(isolate_warnings INTERFACE)
target_compile_options(isolate_warnings INTERFACE
  -Wall -Wextra -Wpedantic
  -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual
  -Wno-gnu-zero-variadic-macro-arguments)

option(ISOLATE_ASAN "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(ISOLATE_ASAN)
  target_compile_options(isolate_warnings INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(isolate_warnings INTERFACE -fsanitize=address,undefined)
endif()

add_subdirectory(src)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
