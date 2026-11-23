# 001 HLSL 着色器、离屏合成与布局坐标架构

日期 2026-11-23。

> 本文补齐 [batch1 ADR 006](../batch1/6：D3D11渲染管线、HLSL着色器与离屏层.md) 与 [batch2 ADR 001](../batch2/1：Native模块分层、依赖方向与命名空间.md)、[002](../batch2/2：TS包分层、依赖方向与命名.md) 在 M5 落地的核心取舍。

## 背景

引入标准 HLSL 像素着色器、`RenderEffect`、`RenderLayer`、29 种 `BlendMode`、组透明度、`onGloballyPositioned` 及液态玻璃旁包 `@canton/kyant-glass` 后，需要锁定几项长期架构约束：用户着色器与框架的寄存器边界、Dist 构建期不依赖 Windows SDK 的 `.dxbc` 预编译方案、Native 编译器装配分层、跨组件布局坐标的同帧零滞后派发时机、离屏 RT 池与 `RenderLayer` 的安全边界，以及旁包的依赖红线。

## 决定

- **标准 HLSL 寄存器避让与 `<canton.hlsl>`**：
  - 用户编写标准 HLSL SM 5.0 像素着色器，可自由声明多个 `cbuffer`、纹理与采样器，槽位范围限定为 `b0..b11`、`t0..t11`、`s0..s11`；框架保留 `b12..b13`、`t12..t15`、`s12..s15`，`D3DReflect` 阶段越界即报错。
  - 框架提供共享虚拟头 `<canton.hlsl>`（定义 `CantonInput` 与 `cantonResolution` / `cantonInvResolution`），由构建期与运行期共用同一份内容，配合原生固定 2D 顶点管线输出 `SV_Position`、`fragCoord` 与 `uv`。
- **Vite `.hlsl` 双轨与 `koffi` 直调 `d3dcompiler_47.dll`**：
  - Live 模式将 `.hlsl` 转为 `import.meta.hot.accept` 自接受模块：保存后由原生 `d3dcompiler_47.dll` 就地重编并原地替换程序，保留已有 `RuntimeShader` 的 uniform；编译失败则保留上一版程序、写错误日志并推 Dev Toast，不触发整页重载。
  - Dist 模式在 Node 构建进程内通过 `koffi` 直接调用 Windows 系统自带 `d3dcompiler_47.dll` 的 `D3DCompile` 预编译为 `assets/<name>-<hash>.dxbc` 并登记进 `canton-ui.json`，冷启动直接反序列化 `ID3D11PixelShader` 与 `D3DReflect`，仅对运行期动态拼接的 `createRuntimeEffect(src)` 保留系统 `d3dcompiler_47.dll` 兜底。
- **`core::ShaderCompiler` 抽象倒置**：
  - `native/src/core/` 定义 `ShaderCompiler` 接口与反射/封包数据结构，`d3d11::D3DShaderCompiler` 实现之，由 `host` 注入 `runtime`/`quickjs`，保持 `d3d11` 只依赖 `core`、`quickjs` 不触碰 D3D11。
- **`onGloballyPositioned` 同帧绘前派发**：
  - `SceneFramePipeline::prepare` 在 `layout_.place()` 之后、`preparation.draw()` 之前自顶向下提取节点/修饰器的窗口综合仿射矩阵与裁剪后包围盒，仅对首次或变化项同步派发回调；回调里写响应式状态驱动 `M.drawWithContent` 或 `RuntimeShader` uniform 时，紧随其后的本帧绘制立即读到最新坐标，无一帧滞后。候选帧失败时连同快照一并回滚。
- **确定性离屏 RT 池与 `RenderLayer` 环检测**：
  - `d3d11` 离屏 RT 池按 `B8G8R8A8_UNORM` 严格桶匹配（`rt.width == targetW && rt.height == targetH`），不做跨桶模糊复用，避免分桶 UV 精度随前序帧池历史漂移；高级 `BlendMode` 拷出的背景纹理用整数 `Texture2D.Load` 读取，杜绝双线性越界串样。
  - `RenderLayer` 在 `drawWithContent` 内调用 `drawContent()` 时直接捕获 C++ 原生保留子片段树（不降级为 JS 重放），`core` 与 `d3d11` 维护层访问栈，检出直接或间接自引用环立即抛出中文诊断错误。
- **私有旁包 `@canton/kyant-glass` 公开边界**：
  - `packages/kyant-glass`（`private: true`）作为高阶视觉能力的独立验证场，只允许从 `@canton/framework` 公开出口导入，禁止触碰 `@canton/framework/internal` 或 `@canton/shared`，由 `tests/runtime/package-layers.test.ts` 静态锁死。

## 为什么不选别的

- **Dist 预编译调用 `fxc.exe`**：要求打包机安装 Windows SDK，违背纯 TS 工程“零 C++/SDK 环境门槛”的目标；`d3dcompiler_47.dll` 随 Windows 10/11 系统自带，通过 `koffi` 直调免去工具链依赖且无需编译原生 Node 插件。
- **把 `onGloballyPositioned` 延后到帧发布后异步派发**：像液态玻璃拖拽折射这类依赖自身相对背景坐标的效果会肉眼可见地落后一帧，产生背景撕裂感。
- **把液态玻璃塞进 `@canton/framework` 核心包**：液态玻璃只是建立在通用渲染原语之上的风格化效果；做成独立旁包既验证框架公开 API 完备度，又不膨胀核心包体积。

## 代价

- 构建期引入轻量 FFI 依赖 `koffi`（纯 Windows 构建侧使用，不进入 QuickJS 应用包）。
- 严格桶匹配相比模糊跨桶复用在极端多尺寸并发场景下可能多占少量离屏纹理，但换来了跨帧与跨用例 100% 确定性的像素结果；空闲帧回收机制控制峰值显存。
- `onGloballyPositioned` 同步派发处于候选帧准备期内，回调中若无收敛条件地改布局尺寸会按既有重排上限保护中止。
