function(canton_add_test name library)
  add_executable(${name} ${name}.cpp ${ARGN})
  target_link_libraries(${name} PRIVATE ${library})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

canton_add_test(canton_core_smoke Canton::runtime)
canton_add_test(canton_local_work Canton::core AllocationProbe.cpp)
canton_add_test(canton_repaint_region Canton::core)
canton_add_test(canton_diagnostics_overlay Canton::core)
canton_add_test(canton_input_session Canton::core)
canton_add_test(canton_m24_contract Canton::quickjs)
canton_add_test(canton_live_modules Canton::quickjs)
canton_add_test(canton_modifier_runtime Canton::core)
canton_add_test(canton_quickjs_modifier Canton::quickjs)
canton_add_test(canton_slot_runtime Canton::core)
canton_add_test(canton_pointer_gesture Canton::core)
canton_add_test(canton_m3_layout Canton::core)
canton_add_test(canton_m3_lazy_geometry Canton::core)

add_executable(canton_bytecode_bench EXCLUDE_FROM_ALL canton_bytecode_bench.cpp)
target_link_libraries(canton_bytecode_bench PRIVATE qjs)

option(CANTON_BUILD_FIXTURE_TESTS "Build C++ tests that use the TS/CTA fixture toolchain" OFF)
if(CANTON_BUILD_FIXTURE_TESTS)
  find_program(CANTON_NODE_EXECUTABLE node REQUIRED)
  set(fixture_root "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
  set(app_smoke "${fixture_root}/app-smoke/app.js")
  file(GLOB_RECURSE fixture_inputs CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/packages/*/src/*.ts"
    "${PROJECT_SOURCE_DIR}/tests/fixtures/*")
  add_custom_command(
    OUTPUT "${app_smoke}"
    COMMAND "${CANTON_NODE_EXECUTABLE}" --import tsx scripts/build-runtime-fixtures.ts "${fixture_root}" app-smoke
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    DEPENDS ${fixture_inputs} "${PROJECT_SOURCE_DIR}/scripts/build-runtime-fixtures.ts" "${PROJECT_SOURCE_DIR}/tests/helpers/ui-fixture.ts"
    VERBATIM
  )
  add_custom_target(canton_runtime_fixture DEPENDS "${app_smoke}")
  add_executable(canton_quickjs_app_smoke canton_quickjs_app_smoke.cpp)
  target_link_libraries(canton_quickjs_app_smoke PRIVATE Canton::quickjs)
  add_dependencies(canton_quickjs_app_smoke canton_runtime_fixture)
  add_test(NAME canton_quickjs_app_smoke COMMAND canton_quickjs_app_smoke "${app_smoke}")
  add_test(NAME canton_script_diagnostics COMMAND canton_quickjs_app_smoke "${app_smoke}" --expect-script-diagnostics)
  add_test(NAME canton_script_diagnostics_rejection COMMAND canton_quickjs_app_smoke "${app_smoke}" --expect-script-diagnostics-rejection)
endif()
