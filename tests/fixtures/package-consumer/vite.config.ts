import { defineConfig } from 'vite'
import canton from '@canton/framework/vite'

export default defineConfig({
    plugins: [canton(), {
        name: 'canton-package-runtime-boundary',
        generateBundle() {
            for (const id of this.getModuleIds()) {
                const path = id.replaceAll('\\', '/')
                if (/\/node_modules\/(?:@canton\/(?:compiler|vite-plugin)|@babel\/[^/]+|vite|typescript)\//.test(path)) this.error(`应用运行时引入了构建工具：${id}`)
            }
        },
    }],
})
