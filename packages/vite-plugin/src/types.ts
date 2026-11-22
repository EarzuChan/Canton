export type CantonVitePluginOptions = {
    entry?: string
    host?: string
    port?: number
    strictPort?: boolean
}

export type MiddlewareResponse = {
    statusCode: number
    setHeader: (name: string, value: string) => void
    end: (body?: string) => void
}

export type MiddlewareHandler = (req: unknown, res: MiddlewareResponse) => void | Promise<void>

export type CantonDevServer = {
    config?: {
        root: string
        mode: string
        configFile?: string
        cantonViteApiRoot?: string
    }
    middlewares?: { use: (path: string, handler: MiddlewareHandler) => void }
}

export type HotUpdateModule = {
    id?: string
    file?: string
}

export type HotUpdateContext = {
    file: string
    modules: HotUpdateModule[]
    server?: { ws?: { send?: (event: { type: 'full-reload'; path: string }) => void } }
}

export type TransformWarning = { id: string; message: string }

export type TransformThis = { warn: (warning: TransformWarning) => void; addWatchFile?: (file: string) => void }

export type ConfigEnv = { command?: string; mode?: string }

export type CantonViteConfig = {
    define: Record<string, string>
    server: { host: string; port: number; strictPort: boolean }
    build: {
        outDir?: string
        target: string
        rollupOptions: {
            input: string
            output: {
                format: string
                entryFileNames: string
                codeSplitting: boolean
                chunkFileNames: string
                assetFileNames: string
            }
        }
    }
}

export type CantonTransformPluginOptions = {
    name: string
    hot: () => boolean
}

export type CantonTransformPlugin = {
    name: string
    enforce: "pre"
    load: (id: string) => string | null
    transform: (this: TransformThis, code: string, id: string) => string | Promise<{ code: string; map?: object | null } | null> | null
}

export type CantonVitePlugin = CantonTransformPlugin & Pick<import('vite').Plugin, 'configEnvironment' | 'configResolved' | 'writeBundle'> & {
    config: (config?: unknown, env?: ConfigEnv) => CantonViteConfig
    configureServer: (server: CantonDevServer) => void
    handleHotUpdate: (ctx: HotUpdateContext) => HotUpdateModule[]
}
