add_library(isolate_shim SHARED
  shim/shim.cpp
)
target_link_libraries(isolate_shim PUBLIC isolate_warnings)
set_target_properties(isolate_shim PROPERTIES
  PREFIX "lib"
  OUTPUT_NAME "isolate_shim"
)

add_library(isolate_core STATIC
  util/str.cpp
  util/json.cpp
  util/path.cpp
  util/fs.cpp
  util/log.cpp
  util/lock.cpp
  core/ids.cpp
  core/spec.cpp
  core/store.cpp
  fsview/clone.cpp
  fsview/extents.cpp
  fsview/diff.cpp
  fsview/layer.cpp
  sb/profile.cpp
  sb/services.cpp
  sb/apply.cpp
  sb/tag.cpp
  proc/limits.cpp
  proc/launch.cpp
  proc/tracker.cpp
  proc/supervisor.cpp
  proc/pty.cpp
  proc/denial_tracer.cpp
)
target_include_directories(isolate_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(isolate_core PUBLIC isolate_warnings)
target_compile_definitions(isolate_core PUBLIC
  ISOLATE_VERSION="${PROJECT_VERSION}"
  ISOLATE_SHIM_PATH="$<TARGET_FILE:isolate_shim>"
)

add_executable(isolate
  main.cpp
  cli/args.cpp
  cli/format.cpp
  cli/cmd_run.cpp
  cli/cmd_list.cpp
  cli/cmd_inspect.cpp
  cli/cmd_ps.cpp
  cli/cmd_diff.cpp
  cli/cmd_commit.cpp
  cli/cmd_snapshot.cpp
  cli/cmd_kill.cpp
  cli/cmd_destroy.cpp
  cli/cmd_doctor.cpp
  cli/cmd_gc.cpp
  cli/cmd_portability.cpp
)
target_link_libraries(isolate PRIVATE isolate_core)

install(TARGETS isolate RUNTIME DESTINATION bin)
install(TARGETS isolate_shim LIBRARY DESTINATION lib)
