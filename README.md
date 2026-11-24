# Canton

勿动。谨以此项目献给我的家乡广州。

以下为草稿。

---

Canton 是原生面向 Windows 的声明式 UI 框架，系同一作者的 Arrange 项目的姊妹作品。

# Hello World

界面文件和独立入口脚本分别如下。

```cta
// app/src/MyApp.cta
<template>
    <App &appHandle>
        <Window title="My App" :onCloseRequest="appHandle.exitApplication">
            <MainWindowView />
        </Window>
    </App>
</template>

<script>
import { App, Window } from '@canton/framework'
import MainWindowView from './MainWindowView.cta'
</script>
```

```ts
// app/src/main.ts
import { appBuilder } from '@canton/framework'
import MyApp from './MyApp.sfa'

export default appBuilder(MyApp).build()
```

# 核心特性

- 既非套壳 Webview、也非 React Native 那种仍沿用 Web/Html/React/Css 的思维，而是在 Compose 的启发（部分受制于编程语言、平台、特性和实际场景会有所不同）之上发展而来。
- SFA 与响应式 authoring：独立打造以及结合深度魔改（基本相当重做，派生自 Vue 主要是为了 DSL 和反应式系统）Vue 的部分部件，使用 Canton 自己的参数、内容和重排契约，支持 Vite 热更新。
- Compose 风格的原生 UI：Arrangable 组合、Layout、Modifier 与测量/放置/绘制共同组织界面。
- 用户纯写 TS 就能开发绝大部分场景的 App 界面和业务逻辑，而我们框架背后有联合 C++ 的高速高性能重排、布局、绘制和输入设施。当用户需要写复杂逻辑或者调 Native 包时，也可以写 Native 模块。
- Foundation 提供无装饰样式的基础 Arrangable，设计系统和高级集合布局用户可另行扩展。