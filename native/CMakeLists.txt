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
  src/runtime/Digest.cpp
  src/runtime/DiagnosticsModel.cpp
  src/runtime/DiagnosticsScene.cpp
  src/runtime/DiagnosticsState.cpp
  src/runtime/DiagnosticsToast.cpp
  src/runtime/ErrorScreenModel.cpp
  src/runtime/FramePlanner.cpp
  src/runtime/FramePumpDriver.cpp
  src/runtime/InputTextSession.cpp
  src/runtime/Json.cpp
  src/runtime/PackageRuntimeSource.cpp
  src/runtime/PainterResources.cpp
  src/runtime/RearrangeHost.cpp
  src/runtime/RuntimePackageBinder.cpp
  src/runtime/RuntimePackageLoader.cpp
  src/runtime/RuntimeSessionState.cpp
  src/runtime/SceneHost.cpp
  src/runtime/ScenePipelineState.cpp
  src/runtime/ScriptEventDispatcher.cpp
  src/runtime/SvgDecoder.cpp
  src/runtime/TextInputMutationSink.cpp
  src/runtime/UiPackage.cpp
  src/runtime/VBlankSource.cpp
  src/runtime/ZipArchive.cpp
  "${CANTON_MINIZ_SOURCE_DIR}/miniz_tinfl.c"
)
add_library(Canton::runtime ALIAS canton_runtime)
target_link_libraries(canton_runtime PUBLIC Canton::quickjs PRIVATE bcrypt windowscodecs ole32)
target_compile_definitions(canton_runtime PRIVATE NOMINMAX)
# 第三方源码只取用到的文件：miniz 的 deflate 解压；lunasvg 与它仓库里自带的 plutovg 编成一个静态库
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/canton-third-party/miniz_export.h" CONTENT "#pragma once\n#define MINIZ_EXPORT\n")
target_include_directories(canton_runtime PRIVATE "${CANTON_MINIZ_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/canton-third-party")
set_source_files_properties("${CANTON_MINIZ_SOURCE_DIR}/miniz_tinfl.c" PROPERTIES COMPILE_OPTIONS "$<$<C_COMPILER_ID:MSVC>:/w>")
set(_canton_lunasvg "${CANTON_LUNASVG_SOURCE_DIR}")
add_library(canton_lunasvg STATIC
  "${_canton_lunasvg}/source/lunasvg.cpp"
  "${_canton_lunasvg}/source/graphics.cpp"
  "${_canton_lunasvg}/source/svgelement.cpp"
  "${_canton_lunasvg}/source/svggeometryelement.cpp"
  "${_canton_lunasvg}/source/svglayoutstate.cpp"
  "${_canton_lunasvg}/source/svgpaintelement.cpp"
  "${_canton_lunasvg}/source/svgparser.cpp"
  "${_canton_lunasvg}/source/svgproperty.cpp"
  "${_canton_lunasvg}/source/svgrenderstate.cpp"
  "${_canton_lunasvg}/source/svgtextelement.cpp"
  "${_canton_lunasvg}/plutovg/source/plutovg-blend.c"
  "${_canton_lunasvg}/plutovg/source/plutovg-canvas.c"
  "${_canton_lunasvg}/plutovg/source/plutovg-font.c"
  "${_canton_lunasvg}/plutovg/source/plutovg-matrix.c"
  "${_canton_lunasvg}/plutovg/source/plutovg-paint.c"
  "${_canton_lunasvg}/plutovg/source/plutovg-path.c"
  "${_canton_lunasvg}/plutovg/source/plutovg-rasterize.c"
  "${_canton_lunasvg}/plutovg/source/plutovg-surface.c"
  "${_canton_lunasvg}/plutovg/source/plutovg-ft-math.c"
  "${_canton_lunasvg}/plutovg/source/plutovg-ft-raster.c"
  "${_canton_lunasvg}/plutovg/source/plutovg-ft-stroker.c"
)
set_target_properties(canton_lunasvg PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_include_directories(canton_lunasvg PRIVATE "${_canton_lunasvg}/source" "${_canton_lunasvg}/plutovg/source" PUBLIC "${_canton_lunasvg}/include" "${_canton_lunasvg}/plutovg/include")
# 不读系统字体、不从文件加载字体；外部引用在解析前由 SvgDecoder 拒掉
target_compile_definitions(canton_lunasvg PRIVATE LUNASVG_BUILD PLUTOVG_BUILD LUNASVG_DISABLE_LOAD_SYSTEM_FONTS PLUTOVG_DISABLE_FONT_FACE_CACHE_LOAD PUBLIC LUNASVG_BUILD_STATIC PLUTOVG_BUILD_STATIC)
target_compile_options(canton_lunasvg PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/w>" "$<$<COMPILE_LANG_AND_ID:C,MSVC>:/w>")
target_link_libraries(canton_runtime PRIVATE canton_lunasvg)

# Live 相关的源文件单独成库，只有 Dev 宿主链接它
add_library(canton_runtime_live STATIC
  src/runtime/DevServerClient.cpp
  src/runtime/HttpClient.cpp
  src/runtime/LiveClient.cpp
  src/runtime/LiveResources.cpp
)
add_library(Canton::runtime_live ALIAS canton_runtime_live)
target_compile_definitions(canton_runtime_live PRIVATE NOMINMAX)
target_link_libraries(canton_runtime_live PUBLIC Canton::runtime PRIVATE winhttp)

add_library(canton_win32 STATIC
  src/win32/ConsoleLog.cpp
  src/win32/Window.cpp
  src/win32/WindowsPrecisionWheelSource.cpp
)
add_library(Canton::win32 ALIAS canton_win32)
target_compile_definitions(canton_win32 PRIVATE NOMINMAX)
target_link_libraries(canton_win32 PUBLIC Canton::core PRIVATE comctl32 ole32 user32 uuid)

# 内置着色器在构建期编译成 DXBC 并嵌进渲染库，运行期不依赖 d3dcompiler_47.dll
find_program(CANTON_FXC fxc HINTS "$ENV{WindowsSdkVerBinPath}x64" "$ENV{WindowsSdkVerBinPath}/x64" "$ENV{WindowsSdkDir}bin/$ENV{WindowsSDKVersion}x64" REQUIRED)
set(shader_dir "${CMAKE_CURRENT_BINARY_DIR}/canton-shaders")
set(quad_shader "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d11/shaders/Quad.hlsl")
add_custom_command(
  OUTPUT "${shader_dir}/QuadVs.h" "${shader_dir}/QuadPs.h"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${shader_dir}"
  COMMAND "${CANTON_FXC}" /nologo /O3 /T vs_5_0 /E vsMain /Vn kQuadVs /Fh "${shader_dir}/QuadVs.h" "${quad_shader}"
  COMMAND "${CANTON_FXC}" /nologo /O3 /T ps_5_0 /E psMain /Vn kQuadPs /Fh "${shader_dir}/QuadPs.h" "${quad_shader}"
  DEPENDS "${quad_shader}"
  VERBATIM
)

add_library(canton_d3d11 STATIC
  src/d3d11/DWriteFonts.cpp
  src/d3d11/DWriteText.cpp
  src/d3d11/PathRaster.cpp
  src/d3d11/RendererCaches.cpp
  src/d3d11/RendererDevice.cpp
  src/d3d11/RendererReplay.cpp
  src/d3d11/VBlankThread.cpp
  "${shader_dir}/QuadVs.h"
  "${shader_dir}/QuadPs.h"
)
add_library(Canton::d3d11 ALIAS canton_d3d11)
target_include_directories(canton_d3d11 PRIVATE "${shader_dir}")
target_link_libraries(canton_d3d11 PUBLIC Canton::core PRIVATE d3d11 dxgi dxguid dwrite dcomp dwmapi)

# 预编译宿主：同一份源码按 CANTON_ENABLE_DEV 出两份制品。开关只决定编进哪个装配文件、链不链 Live 库
option(CANTON_BUILD_HOSTS "Build the Canton host executables" ${CANTON_IS_TOP_LEVEL})
if(CANTON_BUILD_HOSTS)
  function(canton_add_host target output dev)
    add_executable(${target} WIN32 src/host/HostMain.cpp src/host/canton-host.manifest ${ARGN})
    set_target_properties(${target} PROPERTIES OUTPUT_NAME "${output}" MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_definitions(${target} PRIVATE NOMINMAX CANTON_ENABLE_DEV=${dev} CANTON_ENABLE_NATIVE_EXT=0)
    target_include_directories(${target} PRIVATE src)
    target_link_libraries(${target} PRIVATE Canton::runtime Canton::d3d11 Canton::win32 ole32)
    # 没被引用的函数和数据不进制品：Dist 宿主里因此不含 Live 客户端和开发期开关
    target_compile_options(${target} PRIVATE "$<$<CXX_COMPILER_ID:MSVC>:/Gy;/Gw>")
    target_link_options(${target} PRIVATE "$<$<CXX_COMPILER_ID:MSVC>:/OPT:REF;/OPT:ICF>")
  endfunction()
  canton_add_host(canton_host_dev canton-host-dev 1 src/host/DevAssembly.cpp)
  target_link_libraries(canton_host_dev PRIVATE Canton::runtime_live)
  canton_add_host(canton_host canton-host 0 src/host/DistAssembly.cpp)
endif()
