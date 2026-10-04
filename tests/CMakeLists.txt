add_executable(isolate_unit_tests
  harness.cpp
  unit/test_str.cpp
  unit/test_json.cpp
  unit/test_path.cpp
  unit/test_fs.cpp
  unit/test_lock.cpp
  unit/test_spec.cpp
  unit/test_store.cpp
  unit/test_profile.cpp
  unit/test_services.cpp
  unit/test_clone.cpp
  unit/test_extents.cpp
  unit/test_diff.cpp
  unit/test_layer.cpp
  unit/test_limits.cpp
  unit/test_tag.cpp
  unit/test_tracker.cpp
)
target_include_directories(isolate_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(isolate_unit_tests PRIVATE isolate_core)

# Each suite is a separate ctest entry so a failure names the area it came from.
set(ISOLATE_UNIT_SUITES
  str json path fs lock spec store profile services clone extents diff layer snapshot limits tag tracker)
foreach(suite ${ISOLATE_UNIT_SUITES})
  add_test(NAME unit.${suite} COMMAND isolate_unit_tests ${suite})
endforeach()

# Integration tests drive the real binary: they create capsules, run programs
# inside them, and check what the kernel actually allowed.
add_executable(isolate_integration_tests
  harness.cpp
  integration/runner.cpp
  integration/test_cli.cpp
  integration/test_filesystem.cpp
  integration/test_process.cpp
  integration/test_network.cpp
  integration/test_credentials.cpp
  integration/test_resources.cpp
  integration/test_lifecycle.cpp
)
target_include_directories(isolate_integration_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(isolate_integration_tests PRIVATE isolate_core)
add_dependencies(isolate_integration_tests isolate)
target_compile_definitions(isolate_integration_tests
  PRIVATE ISOLATE_BINARY="$<TARGET_FILE:isolate>")

set(ISOLATE_INTEGRATION_SUITES cli filesystem process network resources lifecycle credentials)
foreach(suite ${ISOLATE_INTEGRATION_SUITES})
  add_test(NAME integration.${suite} COMMAND isolate_integration_tests ${suite})
  set_tests_properties(integration.${suite} PROPERTIES TIMEOUT 300)
endforeach()
