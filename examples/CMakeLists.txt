set(ROUTEFABRIC_EXAMPLES
  ex_basic_publication
  ex_path_backed_route
  ex_route_replacement
  ex_withdrawal
  ex_stale_publisher
  ex_stale_epoch
  ex_backend_failure
  ex_ambiguous_completion
  ex_restart_reconciliation
  ex_snapshot_and_diff)

foreach(example ${ROUTEFABRIC_EXAMPLES})
  add_executable(${example} "${CMAKE_CURRENT_SOURCE_DIR}/${example}.cpp")
  target_link_libraries(${example} PRIVATE SummonSoftwareLabs::RouteFabric)
  routefabric_apply_compile_options(${example})
  add_test(NAME ${example} COMMAND ${example})
  set_tests_properties(${example} PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endforeach()
