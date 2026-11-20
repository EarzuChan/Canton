# 002 Slot 句柄与父布局数据

日期 2026-11-19。

## 背景

Canton 继承的 Arrange 代码基线将 Slot 表示为无参内容函数，而 `provide/inject` 沿可组合往下传，更适合交付可跨层访问、子层可覆盖的环境上下文（类似 CompositionLocal）。容器需要向一次内容调用交付 `exitApplication` 或 `align` 等操作时，本次提出的 Slot 参数可提供类似 Compose 接收者 lambda 的调用契约。容器在调用时传参，调用方显式绑定所需的参数。

## Slot 传参与模板绑定

每个 Slot 可声明零个、一个或多个具名参数。Handle（句柄）是其中一种参数，通常把容器交付给内容的一组操作放在一个对象上，Handle 可类比 Compose 中的 Scope（如 ColumnScope vs ColumnHandle），Compose 采用隐式（Receive 玩法）故叫 Scope 合适，而我们因提取时的。普通值和 Handle 使用相同的传参与绑定规则。

提供方用 `defineSlots` 为各槽位声明参数，在每个 `<Slot>` 出口按名称传值。以下用示例组件 `ExampleContainer` 展示三个 Slot。`footer` 有零个参数，`default` 有一个参数，`header` 有两个参数。

```vue
<template>
    <Slot name="header" :title="title" :actions="headerActions" />
    <Slot :app-handle="appHandle" />
    <Slot name="footer" />
</template>

<script>
defineSlots<{
    default: { appHandle: ApplicationHandle }
    header: { title: string; actions: HeaderHandle }
    footer: {} // 不写这行也算作定义 0 个参数
}>()

// 略：setup 中创建 title、headerActions、appHandle
</script>
```

使用方用 `#header` 选择具名 Slot（另，不提供 `a-slot:header` / `c-slot:header` 这样的写法，如有要砍掉）。`&` 绑定该槽位的一个参数。可按需写多个 `&`，书写顺序无关；当提供方注册了某个插槽参数 `the-param`，但使用方没用 `&the-param` 提取之时，则 `the-param` 不作数，会继续按普通词法规则向外继续查找。默认 Slot 的绑定直接写在组件标签上（用户无需再写不带 `#`/带 `#default` 的Template元素），具名 Slot 的绑定写在选择该槽位的 `<Template>` 上。

```vue
<ExampleContainer &app-handle="outerApp">
    <Template #header &title &actions="headerActions">
        <Text :text="title" />
        <RefreshButton :on-press="headerActions.refresh" />
    </Template>

    <QuitButton :on-press="outerApp.exitApplication" />

    <Template #footer>
        <Text text="页脚" />
    </Template>
</ExampleContainer>
```

出口和调用处按相同的参数名规则归一化。`&app-handle` 接收 `appHandle` 并以 `appHandle` 作为局部名，`&app-handle="outerApp"` 则以 `outerApp` 作为局部名。嵌套同一容器时可以给外层起别名，内层继续用原名。调用方可省略任何不读取的参数绑定。

绑定只在对应的内容模板中可见，模板里的嵌套表达式可以捕获它。嵌套组件自己的实现需要通过 prop 或新的 Slot 参数显式接收（不透传、不像 `provide/inject` 那样深层可用）。类型检查根据槽位签名推导局部变量类型，并诊断未声明的参数、出口缺少必需参数或类型不匹配。

## 自此父布局数据应通过布局 Handle 构造

公共 `Modifier` 链应移除 `align()`、`weight()` 和 `matchParentSize()`。相应布局通过默认 Slot 交付 Handle。如 Box 提供 `align` 和 `matchParentSize`，Row、Column 及对应的 Flow 布局提供 `weight` 和与自身交叉轴匹配的 `align`。Handle 方法返回普通 Modifier 片段，由调用方用 `then()` 插入链中的指定位置。

```vue
<Column &column-handle>
    <Text text="正文" :modifier="M.then(columnHandle.align(...)).padding(8.dp)"/>
</Column>
```

注：在开工迁移前梳理一下（对照 Compose 和我们的实现），哪些内建 Arrangables 需要定义 SlotParams、哪些老玩意需要被重构为 XxxHandle。

Handle 的词法可见性约束构造入口。生成的 Modifier 仍可通过 prop 转交或参与组合，原生布局按实际布局父级继续校验。即使 `columnHandle` 在嵌套 Box 的模板内容中仍可见，把 Column 的 `weight` 装到 Box 的布局子项上也应报错。

槽位仍由接收方决定何时、在哪里执行，内容可以贡献零个或多个布局节点。`App` 通过默认 Slot 交付 `appHandle`，应用入口与窗口声明见 [ADR 001](001-Windows用户态目标、应用入口与受管工程.md)。

## 实施与验收

- 覆盖零参、单参和多参 Slot，以及默认槽和具名槽的接参。验证同名绑定、别名、嵌套时区分内外 Handle、忽略参数、类型推导与错误诊断。
- 同一个 Slot 可以在不同出口接收不同实参，各次执行保持独立的内容作用域和调用身份。实参变化按该出口的结构输入更新，存续子实例按现有规则匹配。稳定对象上通过 getter 读取的响应式字段仍由实际读取位置追踪，纯值更新沿用值绑定路径。
- 同次迁移公开 Modifier 类型和现有调用处。验收旧入口不可用、布局 Handle 按交叉轴提供操作、Modifier 顺序保持，以及跨组件转交后仍能诊断错误的布局父级。
