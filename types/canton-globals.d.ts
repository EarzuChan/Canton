import '../packages/shared/src/build-globals.d.ts'

declare module '*.hlsl' {
    import type { RuntimeEffect } from '@canton/framework/ui'
    const effect: RuntimeEffect
    export default effect
}
