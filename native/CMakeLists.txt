add_library(canton_core STATIC
  src/core/DiagnosticsOverlay.cpp
  src/core/EventSlot.cpp
  src/core/FlowLayout.cpp
  src/core/Focus.cpp
  src/core/FocusOwner.cpp
  src/core/Font.cpp
  src/core/HitTest.cpp
  src/core/HostInput.cpp
  src/core/InputEditing.cpp
  src/core/InputSession.cpp
  src/core/IntrinsicLayout.cpp
  src/core/Layout.cpp
  src/core/LayoutTree.cpp
  src/core/LazyLayout.cpp
  src/core/Log.cpp
  src/core/MeasurePolicy.cpp
  src/core/Modifier.cpp
  src/core/ModifierGeometry.cpp
  src/core/Mutation.cpp
  src/core/MutationTransaction.cpp
  src/core/NativeScene.cpp
  src/core/Paint.cpp
  src/core/PointerGesture.cpp
  src/core/PointerInputState.cpp
  src/core/PropSchema.cpp
  src/core/PropValue.cpp
  src/core/Repaint.cpp
  src/core/SceneFramePipeline.cpp
  src/core/Scroll.cpp
  src/core/SlotUpdate.cpp
  src/core/TextLayoutService.cpp
  src/core/Version.cpp
  src/core/WheelSmoother.cpp
)
add_library(Canton::core ALIAS canton_core)
target_include_directories(canton_core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
target_compile_features(canton_core PUBLIC cxx_std_20)
target_compile_options(canton_core INTERFACE "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

add_library(canton_quickjs STATIC
  src/quickjs/QuickJsEventRegistry.cpp
  src/quickjs/QuickJsHotTransport.cpp
  src/quickjs/QuickJsModifierReader.cpp
  src/quickjs/QuickJsModuleLoader.cpp
  src/quickjs/QuickJsNativeApi.cpp
  src/quickjs/QuickJsPainterResources.cpp
  src/quickjs/QuickJsRuntimeContext.cpp
  src/quickjs/QuickJsScriptHeap.cpp
  src/quickjs/QuickJsScriptHost.cpp
  src/quickjs/QuickJsValueReader.cpp
)
add_library(Canton::quickjs ALIAS canton_quickjs)
target_link_libraries(canton_quickjs PUBLIC Canton::core qjs)

# Hash the linked engine artifact, including local source overrides and build flags.
set(identity_dir "${CMAKE_BINARY_DIR}/canton-quickjs-identity/$<CONFIG>")
set(identity_script "${PROJECT_SOURCE_DIR}/cmake/QuickJsBuildIdentity.cmake")
add_custom_command(
  OUTPUT "${identity_dir}/QuickJsBuildIdentity.h" "${identity_dir}/identity.txt"
  COMMAND "${CMAKE_COMMAND}" "-DINPUT=$<TARGET_FILE:qjs>" "-DOUTPUT=${identity_dir}" -P "${identity_script}"
  DEPENDS qjs "${identity_script}"
  VERBATIM
)
add_custom_target(canton_quickjs_identity DEPENDS "${identity_dir}/QuickJsBuildIdentity.h" "${identity_dir}/identity.txt")
add_dependencies(canton_quickjs canton_quickjs_identity)
target_include_directories(canton_quickjs PRIVATE "${identity_dir}")

add_executable(canton_ui_bytecode EXCLUDE_FROM_ALL src/quickjs/UiBytecode.cpp)
add_dependencies(canton_ui_bytecode canton_quickjs_identity)
target_include_directories(canton_ui_bytecode PRIVATE "${identity_dir}" src/quickjs)
target_link_libraries(canton_ui_bytecode PRIVATE qjs)

add_library(canton_runtime STATIC
  src/runtime/AppResolver.cpp
  src/runtime/CantonRuntime.cpp
  src/runtime/DevServerClient.cpp
  src/runtime/DiagnosticsModel.cpp
  src/runtime/DiagnosticsScene.cpp
  src/runtime/DiagnosticsState.cpp
  src/runtime/DiagnosticsToast.cpp
  src/runtime/ErrorScreenModel.cpp
  src/runtime/FramePlanner.cpp
  src/runtime/FramePumpDriver.cpp
  src/runtime/InputTextSession.cpp
  src/runtime/PackageRuntimeSource.cpp
  src/runtime/PainterResources.cpp
  src/runtime/RearrangeHost.cpp
  src/runtime/RuntimePackageBinder.cpp
  src/runtime/RuntimePackageLoader.cpp
  src/runtime/RuntimeSessionState.cpp
  src/runtime/ScenePipelineState.cpp
  src/runtime/ScriptEventDispatcher.cpp
  src/runtime/TextInputMutationSink.cpp
  src/runtime/UiPackage.cpp
  src/runtime/VBlankSource.cpp
)
add_library(Canton::runtime ALIAS canton_runtime)
target_link_libraries(canton_runtime PUBLIC Canton::quickjs)

add_library(canton_win32 STATIC
  src/win32/ConsoleLog.cpp
  src/win32/WindowsPrecisionWheelSource.cpp
)
add_library(Canton::win32 ALIAS canton_win32)
target_compile_definitions(canton_win32 PRIVATE NOMINMAX)
target_link_libraries(canton_win32 PUBLIC Canton::core PRIVATE comctl32 ole32 user32 uuid)
