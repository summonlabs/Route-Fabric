add_executable(rf_bench rf_bench.cpp)
target_link_libraries(rf_bench PRIVATE SummonSoftwareLabs::RouteFabric)
routefabric_apply_compile_options(rf_bench)

add_test(NAME rf_bench COMMAND rf_bench --quick)
set_tests_properties(rf_bench PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
