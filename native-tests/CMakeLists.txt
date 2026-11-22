function(canton_add_test name library)
  add_executable(${name} ${name}.cpp ${ARGN})
  target_link_libraries(${name} PRIVATE ${library})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

canton_add_test(canton_core_smoke Canton::runtime_live)
canton_add_test(canton_local_work Canton::core AllocationProbe.cpp)
canton_add_test(canton_repaint_region Canton::core)
canton_add_test(canton_diagnostics_overlay Canton::core)
canton_add_test(canton_input_session Canton::core)
canton_add_test(canton_quickjs_contract Canton::quickjs)
canton_add_test(canton_live_modules Canton::quickjs)
canton_add_test(canton_modifier_runtime Canton::core)
canton_add_test(canton_quickjs_modifier Canton::quickjs)
canton_add_test(canton_slot_runtime Canton::core)
canton_add_test(canton_pointer_gesture Canton::core)
canton_add_test(canton_layout Canton::core)
canton_add_test(canton_lazy_geometry Canton::core)
canton_add_test(canton_lazy_item_animation Canton::core)
canton_add_test(canton_ui_package Canton::runtime)
canton_add_test(canton_painter_resources Canton::runtime_live)
target_link_libraries(canton_painter_resources PRIVATE ws2_32)
canton_add_test(canton_fonts Canton::d3d11)
target_link_libraries(canton_fonts PRIVATE windowscodecs ole32)
canton_add_test(canton_retained_paint Canton::d3d11)
target_link_libraries(canton_retained_paint PRIVATE windowscodecs ole32)
canton_add_test(canton_frame_submission Canton::d3d11)
target_link_libraries(canton_frame_submission PRIVATE Canton::runtime windowscodecs ole32)
foreach(_canton_test canton_ui_package canton_painter_resources canton_fonts canton_retained_paint canton_frame_submission)
  target_compile_definitions(${_canton_test} PRIVATE NOMINMAX "CANTON_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
endforeach()

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
  # 从 .cta 夹具经 UI 包、QuickJS 到 core 的端到端用例，参数是构建好的夹具目录。
  # 构建脚本共用一个工作区，几个夹具放在同一步里构建，避免并行构建互相覆盖
  set(runtime_fixtures layout lazy rearrange-runtime viewport-runtime)
  set(runtime_fixture_outputs)
  foreach(_canton_fixture IN LISTS runtime_fixtures)
    list(APPEND runtime_fixture_outputs "${fixture_root}/${_canton_fixture}/app.js")
  endforeach()
  add_custom_command(
    OUTPUT ${runtime_fixture_outputs}
    COMMAND "${CANTON_NODE_EXECUTABLE}" --import tsx scripts/build-runtime-fixtures.ts "${fixture_root}" ${runtime_fixtures}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    DEPENDS ${fixture_inputs} "${PROJECT_SOURCE_DIR}/scripts/build-runtime-fixtures.ts" "${PROJECT_SOURCE_DIR}/tests/helpers/ui-fixture.ts" "${app_smoke}"
    VERBATIM
  )
  add_custom_target(canton_runtime_fixtures DEPENDS ${runtime_fixture_outputs})
  foreach(_canton_case IN ITEMS "canton_layout_boundary layout" "canton_lazy_runtime lazy" "canton_rearrange_runtime rearrange-runtime" "canton_viewport_runtime viewport-runtime")
    separate_arguments(_canton_case)
    list(GET _canton_case 0 _canton_name)
    list(GET _canton_case 1 _canton_fixture)
    add_executable(${_canton_name} ${_canton_name}.cpp)
    target_compile_definitions(${_canton_name} PRIVATE NOMINMAX)
    target_link_libraries(${_canton_name} PRIVATE Canton::d3d11 Canton::runtime windowscodecs ole32)
    add_dependencies(${_canton_name} canton_runtime_fixtures)
    add_test(NAME ${_canton_name} COMMAND ${_canton_name} "${fixture_root}/${_canton_fixture}")
  endforeach()
endif()

# 无头回归：直接链接渲染库离屏渲染，与基线图比对。基线只由 --update 写入
add_executable(canton_render_regression canton_render_regression.cpp RenderCasesDraw.cpp RenderCasesFixture.cpp)
target_compile_definitions(canton_render_regression PRIVATE NOMINMAX "CANTON_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
target_link_libraries(canton_render_regression PRIVATE Canton::d3d11 Canton::runtime windowscodecs ole32 user32)

# Live 验证的原生对端，由 scripts/verify-live-hmr.ts 拉起
add_executable(canton_live_server canton_live_server.cpp)
target_link_libraries(canton_live_server PRIVATE Canton::runtime_live)
