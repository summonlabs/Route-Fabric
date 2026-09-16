cmake_minimum_required(VERSION 3.25)

project(RouteFabric
  VERSION 1.0.0
  DESCRIPTION "Authoritative route-lifecycle and route-state runtime"
  HOMEPAGE_URL "https://github.com/summonlabs/Route-Fabric"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(ROUTEFABRIC_BUILD_TESTS "Build Route Fabric tests" ON)
option(ROUTEFABRIC_BUILD_EXAMPLES "Build Route Fabric examples" ON)
option(ROUTEFABRIC_BUILD_BENCHMARKS "Build Route Fabric benchmarks" ON)
option(ROUTEFABRIC_BUILD_TOOLS "Build Route Fabric executables (coordinator, publisher, CLI)" ON)
option(ROUTEFABRIC_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)
option(ROUTEFABRIC_ENABLE_ANALYZE "Enable the MSVC static analyzer for first-party code" OFF)
option(ROUTEFABRIC_ENABLE_ASAN "Enable AddressSanitizer for first-party code" OFF)
option(ROUTEFABRIC_ENABLE_COVERAGE "Enable code coverage instrumentation (clang/gcc only)" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(RouteFabricCompileOptions)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Version header (single source of truth: project version above)
# ---------------------------------------------------------------------------
string(REPLACE "." ";" ROUTEFABRIC_VERSION_PARTS "${PROJECT_VERSION}")
list(GET ROUTEFABRIC_VERSION_PARTS 0 ROUTEFABRIC_VERSION_MAJOR)
list(GET ROUTEFABRIC_VERSION_PARTS 1 ROUTEFABRIC_VERSION_MINOR)
list(GET ROUTEFABRIC_VERSION_PARTS 2 ROUTEFABRIC_VERSION_PATCH)
set(ROUTEFABRIC_VERSION_STRING "${PROJECT_VERSION}")

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/include/routefabric/version.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/routefabric/version.hpp"
  @ONLY)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(RouteFabric
  src/version.cpp
  src/error.cpp
  src/hash.cpp
  src/limits.cpp
  src/encoding.cpp
  src/ids.cpp
  src/destination.cpp
  src/route_key.cpp
  src/lifecycle.cpp
  src/record.cpp
  src/path_authority.cpp
  src/backend.cpp
  src/real_backend.cpp
  src/snapshot.cpp
  src/persistence.cpp
  src/runtime.cpp
  src/wire.cpp
  src/server.cpp
  src/client.cpp)

add_library(SummonSoftwareLabs::RouteFabric ALIAS RouteFabric)

set_target_properties(RouteFabric PROPERTIES
  EXPORT_NAME RouteFabric
  OUTPUT_NAME "RouteFabric"
  VERSION "${PROJECT_VERSION}"
  SOVERSION "${ROUTEFABRIC_VERSION_MAJOR}"
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON)

target_include_directories(RouteFabric
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

routefabric_apply_compile_options(RouteFabric)

if(WIN32)
  target_link_libraries(RouteFabric PRIVATE iphlpapi ws2_32)
  target_compile_definitions(RouteFabric PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
endif()

if(ROUTEFABRIC_ENABLE_ASAN)
  target_compile_options(RouteFabric PUBLIC $<$<CONFIG:Debug,RelWithDebInfo,Release>:/fsanitize=address>)
  target_link_options(RouteFabric PUBLIC $<$<CONFIG:Debug,RelWithDebInfo,Release>:/INCREMENTAL:NO>)
endif()

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
if(ROUTEFABRIC_BUILD_TOOLS)
  add_executable(rf_coordinator tools/rf_coordinator.cpp)
  add_executable(rf_publisher tools/rf_publisher.cpp)
  add_executable(rf_cli tools/rf_cli.cpp)
  foreach(tool rf_coordinator rf_publisher rf_cli)
    target_link_libraries(${tool} PRIVATE SummonSoftwareLabs::RouteFabric)
    routefabric_apply_compile_options(${tool})
  endforeach()
  install(TARGETS rf_coordinator rf_publisher rf_cli
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT tools)
endif()

# ---------------------------------------------------------------------------
# Tests / examples / benchmarks
# ---------------------------------------------------------------------------
if(ROUTEFABRIC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(ROUTEFABRIC_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(ROUTEFABRIC_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS RouteFabric
  EXPORT RouteFabricTargets
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT runtime
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT runtime
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT development
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/routefabric"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  COMPONENT development
  FILES_MATCHING PATTERN "*.hpp")

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/routefabric/version.hpp"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/routefabric"
  COMPONENT development)

install(EXPORT RouteFabricTargets
  FILE RouteFabricTargets.cmake
  NAMESPACE SummonSoftwareLabs::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/RouteFabric"
  COMPONENT development)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RouteFabricConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/RouteFabricConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/RouteFabric"
  PATH_VARS CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR CMAKE_INSTALL_BINDIR)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/RouteFabricConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/RouteFabricConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/RouteFabricConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/RouteFabric"
  COMPONENT development)

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/RouteFabric"
  COMPONENT development)
