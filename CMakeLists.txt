cmake_minimum_required(VERSION 3.20)

project(gorillats
    VERSION 0.1.0
    DESCRIPTION "Gorilla time series compression (Pelkonen et al., VLDB 2015)"
    LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(GORILLATS_BUILD_TESTS "Build C++ unit tests (GoogleTest)" ON)
option(GORILLATS_BUILD_PYTHON "Build the pybind11 Python extension" OFF)
option(GORILLATS_ENABLE_SANITIZERS "Enable ASan + UBSan in Debug builds" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Core library: gorillats_core (C)
# ---------------------------------------------------------------------------
add_library(gorillats_core src/gorillats.c)
add_library(gorillats::core ALIAS gorillats_core)

target_include_directories(gorillats_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

set_target_properties(gorillats_core PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_options(gorillats_core PRIVATE /W4)
else()
    target_compile_options(gorillats_core PRIVATE -Wall -Wextra)
endif()

# Header-only C++ interface target (the idiomatic wrapper).
add_library(gorillats_cpp INTERFACE)
add_library(gorillats::cpp ALIAS gorillats_cpp)
target_link_libraries(gorillats_cpp INTERFACE gorillats_core)
target_compile_features(gorillats_cpp INTERFACE cxx_std_17)

# Link-time optimization in Release.
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_ok OUTPUT ipo_msg)
if(ipo_ok)
    set_property(TARGET gorillats_core
        PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

# Sanitizers for Debug.
if(GORILLATS_ENABLE_SANITIZERS AND NOT MSVC)
    target_compile_options(gorillats_core PRIVATE
        $<$<CONFIG:Debug>:-fsanitize=address,undefined -fno-omit-frame-pointer>)
    target_link_options(gorillats_core PRIVATE
        $<$<CONFIG:Debug>:-fsanitize=address,undefined>)
endif()

# ---------------------------------------------------------------------------
# Python extension (pybind11)
# ---------------------------------------------------------------------------
if(GORILLATS_BUILD_PYTHON)
    find_package(pybind11 CONFIG REQUIRED)
    pybind11_add_module(_gorillats python/bindings.cpp)
    target_link_libraries(_gorillats PRIVATE gorillats::cpp)
    install(TARGETS _gorillats LIBRARY DESTINATION gorillats)
endif()

# ---------------------------------------------------------------------------
# Tests (GoogleTest)
# ---------------------------------------------------------------------------
if(GORILLATS_BUILD_TESTS)
    enable_testing()
    find_package(GTest CONFIG QUIET)
    if(NOT GTest_FOUND)
        include(FetchContent)
        FetchContent_Declare(googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.14.0)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    add_executable(gorillats_tests tests/test_compression.cpp)
    target_link_libraries(gorillats_tests
        PRIVATE gorillats::cpp GTest::gtest GTest::gtest_main)
    if(NOT MSVC)
        target_compile_options(gorillats_tests PRIVATE -Wall -Wextra)
    endif()

    include(GoogleTest)
    gtest_discover_tests(gorillats_tests)
endif()

# ---------------------------------------------------------------------------
# Install + export (so find_package(gorillats) works for consumers)
# ---------------------------------------------------------------------------
install(TARGETS gorillats_core gorillats_cpp
    EXPORT gorillatsTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT gorillatsTargets
    FILE gorillatsTargets.cmake
    NAMESPACE gorillats::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/gorillats)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gorillatsConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/gorillatsConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/gorillats)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/gorillatsConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/gorillatsConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/gorillatsConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/gorillats)
