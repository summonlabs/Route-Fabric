set(ROUTEFABRIC_TEST_SOURCES
  support/test_framework.cpp
  t_ids.cpp
  t_destination.cpp
  t_lifecycle.cpp
  t_runtime.cpp
  t_authority.cpp
  t_path_authority.cpp
  t_backend.cpp
  t_persistence.cpp
  t_protocol.cpp
  t_races.cpp
  t_property.cpp
  t_adversarial.cpp
  t_scale.cpp
  t_process.cpp)

add_executable(rf_tests ${ROUTEFABRIC_TEST_SOURCES})
target_link_libraries(rf_tests PRIVATE SummonSoftwareLabs::RouteFabric)
target_include_directories(rf_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/support")
routefabric_apply_compile_options(rf_tests)

if(ROUTEFABRIC_BUILD_TOOLS)
  add_dependencies(rf_tests rf_coordinator rf_publisher rf_cli)
endif()

if(MSVC)
  # The process-level tests spawn real child processes; they must find the
  # executables next to the test binary.
  add_custom_command(TARGET rf_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "route fabric tests: $<TARGET_FILE:rf_tests>")
endif()

add_test(NAME rf_tests COMMAND rf_tests)
set_tests_properties(rf_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

# Downstream package consumer: installs the package into a scratch prefix and
# configures an independent project that only uses installed artifacts.
if(ROUTEFABRIC_BUILD_TOOLS)
  add_test(NAME rf_package_consumer
    COMMAND ${CMAKE_COMMAND}
      -DROUTEFABRIC_SOURCE_DIR=${CMAKE_SOURCE_DIR}
      -DROUTEFABRIC_BINARY_DIR=${CMAKE_BINARY_DIR}
      -DROUTEFABRIC_CONFIG=$<CONFIG>
      -DROUTEFABRIC_GENERATOR=${CMAKE_GENERATOR}
      -P "${CMAKE_CURRENT_SOURCE_DIR}/package/run_package_test.cmake")
  set_tests_properties(rf_package_consumer PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()
