cmake_minimum_required(VERSION 3.15)
project(EncryptedSquadKVDB VERSION 1.0.0 LANGUAGES CXX)

# Enforce C++17 standard with no compiler-specific extensions
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Release build if not specified (critical for <2s 1M-key recovery)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Force all executables into ${CMAKE_BINARY_DIR} directly (even on multi-config Visual Studio)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
foreach(OUTPUTCONFIG IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${OUTPUTCONFIG}" OUTPUTCONFIG_UPPER)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} "${CMAKE_BINARY_DIR}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} "${CMAKE_BINARY_DIR}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} "${CMAKE_BINARY_DIR}")
endforeach()

# Threading support (std::thread, std::shared_mutex)
find_package(Threads REQUIRED)

# Compiler warnings and high-performance Release flags
if(MSVC)
    add_compile_options(/W4 /MP /EHsc)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /Ob2 /DNDEBUG")
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG")
endif()

# ---------------------------------------------------------------------------
# 1. Core KVDB Static Library (kvdb_core)
# ---------------------------------------------------------------------------
set(KVDB_CORE_SOURCES
    src/record.cpp
    src/engine.cpp
    src/recovery.cpp
    src/compaction.cpp
)

if(WIN32)
    list(APPEND KVDB_CORE_SOURCES src/platform_win.cpp)
else()
    list(APPEND KVDB_CORE_SOURCES src/platform_posix.cpp)
endif()

add_library(kvdb_core STATIC ${KVDB_CORE_SOURCES})
target_include_directories(kvdb_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(kvdb_core PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# 2. Deliverable 1: Engine CLI (kvdb / kvdb.exe)
# ---------------------------------------------------------------------------
add_executable(kvdb src/main_kvdb.cpp)
target_link_libraries(kvdb PRIVATE kvdb_core)

# ---------------------------------------------------------------------------
# 3. Deliverable 2: Torture Tool (torture / torture.exe)
# ---------------------------------------------------------------------------
set(TORTURE_SOURCES
    torture/main_torture.cpp
    torture/oracle.cpp
    torture/modes.cpp
)

if(WIN32)
    list(APPEND TORTURE_SOURCES torture/process_win.cpp)
else()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/torture/process_posix.cpp")
        list(APPEND TORTURE_SOURCES torture/process_posix.cpp)
    endif()
endif()

add_executable(torture ${TORTURE_SOURCES})
target_include_directories(torture PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/torture"
)
target_link_libraries(torture PRIVATE kvdb_core Threads::Threads)

# ---------------------------------------------------------------------------
# 4. Unit Tests & 1,000,000-Key Benchmark (tests/*)
# ---------------------------------------------------------------------------
enable_testing()

set(KVDB_TEST_TARGETS
    test_record
    test_recovery
    test_batch
    test_compaction
    test_concurrency
    test_disk_full
    benchmark_recovery
)

foreach(TEST_NAME IN LISTS KVDB_TEST_TARGETS)
    set(TEST_SRC "${CMAKE_CURRENT_SOURCE_DIR}/tests/${TEST_NAME}.cpp")
    if(EXISTS "${TEST_SRC}")
        add_executable(${TEST_NAME} "${TEST_SRC}")
        target_link_libraries(${TEST_NAME} PRIVATE kvdb_core Threads::Threads)
        if(NOT "${TEST_NAME}" STREQUAL "benchmark_recovery")
            add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
        endif()
    endif()
endforeach()