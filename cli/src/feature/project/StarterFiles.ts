import { resolve } from "node:path"
import { defaultUiOutputDirectory } from "../../CliMetadata.ts"
import type { ProjectState } from "./ProjectState.ts"
import { effectiveDisplayName } from "./ProjectMetadata.ts"

export interface InitialFile {
    readonly path: string
    readonly content: string
}

export function uiStarterFiles(state: ProjectState): InitialFile[] {
    const root = resolve(state.rootDir, state.project.ui.directory)
    const outputDirectory = state.project.ui.outputDirectory ?? defaultUiOutputDirectory
    return [
        {
            path: resolve(root, "src/main.ts"), content: `import { appBuilder } from '@canton/framework'
import App from './App.cta'

export default appBuilder(App).build()
` },
        {
            path: resolve(root, "src/App.cta"), content: `<template>
    <App &app-handle>
        <Window :title="projectTitle" :on-close-request="appHandle.exitApplication">
            <Column :modifier="M.fillMaxSize().background(Color(0xff17212e)).padding(24.dp)" :vertical-arrangement="Arrangement.spacedBy(16.dp)">
                <Text :text="projectTitle" :style="{ fontSize: 28.sp, color: Color(0xfff4f6fa) }"/>

                <Text :text="'点击次数：' + count" :modifier="M.clickable(increment).padding(12.dp).background(Color(0xff336699))" :style="{ fontSize: 18.sp, color: Color(0xffffffff) }"/>
            </Column>
        </Window>
    </App>
</template>

<script>
import { ref } from '@canton/framework'
import { Column, Text } from '@canton/framework/foundation'
import { Arrangement, Color, M } from '@canton/framework/ui'

const projectTitle = ${JSON.stringify(effectiveDisplayName(state))}
const count = ref(0)

function increment() {
    count.value += 1
}
</script>
` },
        {
            path: resolve(root, "vite.config.ts"), content: `import { defineConfig } from 'vite'
import canton from '@canton/framework/vite'

export default defineConfig({
    plugins: [canton()],
    build: { outDir: ${JSON.stringify(outputDirectory)} },
})
` },
        { path: resolve(root, "tsconfig.json"), content: `${JSON.stringify({ compilerOptions: { target: "ES2022", module: "ESNext", moduleResolution: "Bundler", allowImportingTsExtensions: true, allowArbitraryExtensions: true, noEmit: true, skipLibCheck: true, strict: true, lib: ["ES2022", "DOM"], types: ["vite/client", "node"] }, include: ["src/**/*.ts", "src/**/*.cta"] }, null, 4)}\n` },
        {
            path: resolve(root, "scripts/check-types.ts"), content: `import { fileURLToPath } from 'node:url'
import { checkCtaProject } from '@canton/framework/vite'

const configPath = fileURLToPath(new URL('../tsconfig.json', import.meta.url))
const sourceRoot = fileURLToPath(new URL('../src', import.meta.url))
const diagnostics = checkCtaProject(configPath, [sourceRoot])
for (const diagnostic of diagnostics) console.error('[CantonTypecheck]', \`\${diagnostic.file}:\${diagnostic.line}:\${diagnostic.column} \${diagnostic.code}：\${diagnostic.message}\`)
if (diagnostics.length) process.exitCode = 1
else console.log('[CantonTypecheck]', 'CTA 脚本与模板类型检查通过')
` },
    ]
}
