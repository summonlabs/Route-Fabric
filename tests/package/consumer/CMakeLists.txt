cmake_minimum_required(VERSION 3.25)
project(RouteFabricConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(RouteFabric CONFIG REQUIRED)

if(NOT RouteFabric_VERSION STREQUAL ROUTEFABRIC_EXPECTED_VERSION)
  message(FATAL_ERROR "expected package version ${ROUTEFABRIC_EXPECTED_VERSION} but found ${RouteFabric_VERSION}")
endif()

if(NOT TARGET SummonSoftwareLabs::RouteFabric)
  message(FATAL_ERROR "the imported target SummonSoftwareLabs::RouteFabric is missing")
endif()

get_target_property(imported_includes SummonSoftwareLabs::RouteFabric INTERFACE_INCLUDE_DIRECTORIES)
if(NOT imported_includes)
  message(FATAL_ERROR "the imported target does not expose interface include directories")
endif()
foreach(include_dir ${imported_includes})
  if(include_dir MATCHES "Route-Fabric/(include|src)")
    message(FATAL_ERROR "the imported target leaks the source tree: ${include_dir}")
  endif()
endforeach()

add_executable(rf_consumer main.cpp)
target_link_libraries(rf_consumer PRIVATE SummonSoftwareLabs::RouteFabric)
