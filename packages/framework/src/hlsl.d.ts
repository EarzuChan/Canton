declare module '*.hlsl' {
    import type { RuntimeEffect } from './draw/shader.ts'
    const effect: RuntimeEffect
    export default effect
}
