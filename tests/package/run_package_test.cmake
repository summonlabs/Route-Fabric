# Installs Route Fabric into a scratch prefix, then configures, builds and runs an
# independent consumer that only sees the installed artifacts.
#
# Required -D arguments: ROUTEFABRIC_SOURCE_DIR, ROUTEFABRIC_BINARY_DIR,
# ROUTEFABRIC_CONFIG, ROUTEFABRIC_GENERATOR.

if(NOT DEFINED ROUTEFABRIC_SOURCE_DIR)
  message(FATAL_ERROR "ROUTEFABRIC_SOURCE_DIR is required")
endif()
if(NOT DEFINED ROUTEFABRIC_BINARY_DIR)
  message(FATAL_ERROR "ROUTEFABRIC_BINARY_DIR is required")
endif()

set(scratch "${ROUTEFABRIC_BINARY_DIR}/package-check")
file(REMOVE_RECURSE "${scratch}")
file(MAKE_DIRECTORY "${scratch}")
set(prefix "${scratch}/prefix")

set(config_argument)
if(ROUTEFABRIC_CONFIG)
  set(config_argument --config ${ROUTEFABRIC_CONFIG})
endif()

execute_process(
  COMMAND ${CMAKE_COMMAND} --install "${ROUTEFABRIC_BINARY_DIR}" --prefix "${prefix}" ${config_argument}
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "cmake --install failed (${install_result})\n${install_output}\n${install_error}")
endif()

set(config_file "${prefix}/lib/cmake/RouteFabric/RouteFabricConfig.cmake")
set(version_file "${prefix}/lib/cmake/RouteFabric/RouteFabricConfigVersion.cmake")
set(targets_file "${prefix}/lib/cmake/RouteFabric/RouteFabricTargets.cmake")
foreach(required ${config_file} ${version_file} ${targets_file})
  if(NOT EXISTS "${required}")
    message(FATAL_ERROR "the installed package is missing ${required}")
  endif()
endforeach()

if(NOT EXISTS "${prefix}/include/routefabric/version.hpp")
  message(FATAL_ERROR "the installed package is missing the generated version header")
endif()
if(NOT EXISTS "${prefix}/include/routefabric/runtime.hpp")
  message(FATAL_ERROR "the installed package is missing its public headers")
endif()

file(READ "${targets_file}" targets_contents)
if(NOT targets_contents MATCHES "SummonSoftwareLabs::RouteFabric")
  message(FATAL_ERROR "the exported target SummonSoftwareLabs::RouteFabric is not defined by the installed package")
endif()

set(consumer_build "${scratch}/consumer-build")
execute_process(
  COMMAND ${CMAKE_COMMAND}
    -S "${ROUTEFABRIC_SOURCE_DIR}/tests/package/consumer"
    -B "${consumer_build}"
    -DCMAKE_PREFIX_PATH=${prefix}
    -DROUTEFABRIC_EXPECTED_VERSION=1.0.0
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "the downstream consumer failed to configure\n${configure_output}\n${configure_error}")
endif()

execute_process(
  COMMAND ${CMAKE_COMMAND} --build "${consumer_build}" ${config_argument}
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "the downstream consumer failed to build\n${build_output}\n${build_error}")
endif()

set(consumer_executable "${consumer_build}/rf_consumer")
if(ROUTEFABRIC_CONFIG)
  set(consumer_executable "${consumer_build}/${ROUTEFABRIC_CONFIG}/rf_consumer")
endif()
if(WIN32)
  set(consumer_executable "${consumer_executable}.exe")
endif()
if(NOT EXISTS "${consumer_executable}")
  message(FATAL_ERROR "the downstream consumer executable was not produced at ${consumer_executable}")
endif()

execute_process(
  COMMAND "${consumer_executable}"
  RESULT_VARIABLE run_result
  OUTPUT_VARIABLE run_output
  ERROR_VARIABLE run_error)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "the downstream consumer failed at runtime (${run_result})\n${run_output}\n${run_error}")
endif()
message(STATUS "downstream consumer output:\n${run_output}")
