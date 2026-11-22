# 002 TS 包分层、依赖方向与命名

日期 2026-11-21。

## 背景

[batch2 ADR 001](1：Native模块分层、依赖方向与命名空间.md) 定了 Native 的分层，TS 侧还没有对应约定。M1 收尾时盘点出几处问题：

- `@canton/framework` 顶层平铺约 30 个文件，`native.ts` 同时承载协议类型与上层描述类型，和 `modifierLowering`、`modifier`、`painter`、`focus` 等互相引用成环。
- `@canton/compiler` 沿用上游的 `core/`、`template/`、`cta/` 三分，`core/` 实为模板编译器本体，`template/` 只是三个文件的方言外壳；`unitSyntax` 被模板与脚本共用却挂在模板目录下。
- 包级出现循环：framework 依赖并打包 vite-plugin，vite-plugin 又从 `@canton/framework/internal` 取版本常量；CTA 离线类型检查 `checkCtaProject` 放在 vite-plugin 里，与 Vite 无关。
- `@canton/shared` 里的 `modifierSchema` 叫“schema”“白名单”，容易被误解为公开 Modifier API 清单。

## 决定

- **包依赖单向**：`shared ← reactivity`、`shared ← compiler ← vite-plugin`，`framework` 依赖并打包其余四个。下层包不得静态导入上层包；vite-plugin 只在运行期按包导出解析 `@canton/framework/hot-runtime` 与 `@canton/framework/internal` 交给用户应用，不在自身模块图里导入 framework。版本常量放 `@canton/shared`。
- **compiler 按编译阶段平铺**，层序自下而上：
  - `syntax/`：模板与脚本共用的语言层工具（单位后缀 `114.dp` 规范化、Babel AST 工具）。
  - `template/`：模板编译器（词法、解析、转换、生成、方言选项、语法契约、CTA 模板块入口）。
  - `cta/`：CTA 文件格式（拆块与描述符、缓存、类型解析选项、告警）。
  - `script/`：TS setup 编译（`defineProps`、类型解析、脚本生成）。
  - `values/`：值单位体系（单位契约、单位校验、值降级）。
  - 根目录：`compileScript` 组装、`checkCtaProject`、公开入口。
  - 重申当年自 Arrange 确立的精神：绝不因模板和反应式和“Vue”有点像就滑坡使用 `sfc` 一类的“Vue”系名词：Arrange/Canton 不是 Vue、CTA 不是单文件组件，Arrangable 也不是组件。
- **framework 目录分层**，层序自下而上，同序号的层互不依赖，根目录只放分层入口（`index`、`ui`、`foundation`、`animation`、`internal`、`vite`）：
  - 0 `values/`：Dp/Sp/Px、颜色、形状与对齐等纯值。
  - 1 `native/`：原生协议（`NativeTransactionTarget` 与跨边界描述形状）及直通能力（日志与 Toast、资产读取）。
  - 2 `runtime/`：Arrangable 运行时与重组。
  - 3 `animation/`、`draw/`：动画值；绘制、Painter、字体、数据源。
  - 4 `layout/`：测量策略、密度、滚动与 Lazy 状态。
  - 5 `modifier/`：Modifier 描述、手势、焦点、值拖拽。
  - 6 `bridge/`：把框架描述送进原生协议——结构提交（`NativeRearrangeHost`）、Modifier 降级、事件载荷适配。
  - 7 `arrangable/`：内建 Arrangable。
  - 8 `app/`：`createApp` 与 HMR 客户端运行时。
- **原生协议只用自身形状描述跨边界数据**：`native/protocol.ts` 不引用上层类型，`updateBinding` 接收 `NativeMeasurePolicy`、`NativeModifierChain`、`NativeModifierElement` 等协议形状，上层的 `MeasurePolicy`、`ModifierElement`、PX 修饰链按结构满足它们；`FocusDirection`、`PainterCompletion` 这类协议枚举与回执定义在协议里，上层再转导。
- **各包零循环导入，仅类型的导入也算**。做法是契约下沉、实现上浮：被多方引用的形状放在底层，具体类 `implements` 它们；底层服务只声明自己需要的最小 Owner 接口。
  - framework/runtime：`arrangable.ts` 是实例契约，只经两个端口接触重组引擎：`InstanceSession`（事务服务，实例创建即可用）与 `StructureScope`（结构账本句柄，setup 返回结构程序后建立，作用域相关操作由它自己执行，不把账本传回会话）。两者对应生命周期不同的两个真实对象，故不合并为一；参数仓只由引擎在调用条目中持有，不挂在实例上；`errorHandling`、`scheduler` 只认 `ErrorOwner`、`SchedulerOwner`。
  - reactivity：`types.ts` 放公共类型，`system.ts` 放追踪内核契约（订阅者、依赖、链接）；代理处理器经 `target.ts` 取用由 `reactive.ts` 求值时登记的代理工厂，不反向导入 `reactive.ts`。
  - 以上规则由 `tests/runtime/package-layers.test.ts` 按真实导入声明检查。
- **Modifier 原生协议表**：`@canton/shared` 的 `modifierNativeProtocol` 是 Modifier 描述跨 TS/C++ 边界的唯一事实源，规定每种描述可带的字段、字段单位、参数单位映射与回调对应的原生事件种类；C++ 侧生成为 `ModifierNativeProtocol.inc`。公开 Modifier API 在 framework 的 `modifier/`，经降级后的产物才必须落在这张表里。

## 为什么不选别的

- **把 framework 拆成多个 npm 包**：用户只装 `@canton/framework` 一个包，拆包只增加发布与版本协调成本；目录分层加约束测试已经能守住边界。
- **原生协议与 bridge 合成一个目录**：协议类型是最底层，被绘制、布局、焦点等普遍引用；结构提交与 Modifier 降级却依赖这些上层描述。放在一起会让一个目录同时处在最底层和最上层，目录之间必然成环。
- **把协议类型下沉到 `@canton/shared`**：协议里的排列、对齐等类型属于 framework 的值体系，下沉会把半个值层一起拖进 shared。
- **`updateBinding` 继续引用上层精确类型**：协议层被迫向上依赖，正是原来成环的根源；原生侧本就按协议表逐字段准入，边界上用结构形状不损失校验。
- **compiler 保留 `core/template/cta` 三分**：上游拆分服务于多渲染目标，Canton 只有一个目标；按编译阶段平铺更符合实际依赖。
- **容忍仅类型的环**：类型导入运行期会被擦除，但它说明契约与实现缠在一起；哪天把其中一条改成值导入（加个 `instanceof`、取个枚举），就会悄悄变成运行期环，初始化顺序随装载入口而变，出错时极难定位。

## 代价

- `updateBinding` 的类型从具体描述放宽为协议形状，传错描述种类要靠原生准入和测试发现，而不是 TS 报错。
- 端口接口与实现类各写一遍签名，靠 `implements` 由编译器保持一致。端口只有一个实现，存在理由是依赖方向而非多态。
- 值任务与 `dirtyParents` 从低层字段拿到的结构账本，引擎要经 `asRearrangeScope` 做一次运行期类型收窄；引擎内部遍历经调用条目直接持有具体账本，不再转型。
- 新增文件必须放进已登记的层，约束测试会拒绝未登记的目录。
