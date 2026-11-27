import { reactive } from '@canton/reactivity'
import type { InjectionKey } from '../runtime/apiInject.ts'
import type { NativeGraphicsEnvironmentSnapshot } from '../native/protocol.ts'

export type GraphicsTier = 'full' | 'reduced'

export interface GraphicsEnvironment {
    readonly backend: 'd3d11'
    readonly adapterName: string
    readonly isSoftwareRenderer: boolean
    readonly hardwareAccelerated: boolean
    readonly isRemoteSession: boolean
    readonly systemTransparency: boolean
    readonly systemAnimations: boolean
    readonly recommendedTier: GraphicsTier
}

export const GraphicsEnvironmentKey: InjectionKey<GraphicsEnvironment> = Symbol('Canton.GraphicsEnvironment')

export interface MutableGraphicsEnvironmentController {
    readonly environment: GraphicsEnvironment
    update(snapshot: NativeGraphicsEnvironmentSnapshot): void
}

const defaultSnapshot: NativeGraphicsEnvironmentSnapshot = Object.freeze({
    backend: 'd3d11',
    adapterName: 'Default Hardware Adapter',
    isSoftwareRenderer: false,
    hardwareAccelerated: true,
    isRemoteSession: false,
    systemTransparency: true,
    systemAnimations: true,
})

export function createGraphicsEnvironment(initial: Partial<NativeGraphicsEnvironmentSnapshot> = {}): MutableGraphicsEnvironmentController {
    const state = reactive({
        adapterName: initial.adapterName ?? defaultSnapshot.adapterName,
        isSoftwareRenderer: initial.isSoftwareRenderer ?? defaultSnapshot.isSoftwareRenderer,
        isRemoteSession: initial.isRemoteSession ?? defaultSnapshot.isRemoteSession,
        systemTransparency: initial.systemTransparency ?? defaultSnapshot.systemTransparency,
        systemAnimations: initial.systemAnimations ?? defaultSnapshot.systemAnimations,
    })

    const environment: GraphicsEnvironment = Object.freeze({
        get backend(): 'd3d11' { return 'd3d11' },
        get adapterName(): string { return state.adapterName },
        get isSoftwareRenderer(): boolean { return state.isSoftwareRenderer },
        get hardwareAccelerated(): boolean { return !state.isSoftwareRenderer },
        get isRemoteSession(): boolean { return state.isRemoteSession },
        get systemTransparency(): boolean { return state.systemTransparency },
        get systemAnimations(): boolean { return state.systemAnimations },
        get recommendedTier(): GraphicsTier {
            return !state.isSoftwareRenderer && !state.isRemoteSession && state.systemTransparency ? 'full' : 'reduced'
        },
    })

    return {
        environment,
        update(snapshot: NativeGraphicsEnvironmentSnapshot) {
            state.adapterName = snapshot.adapterName
            state.isSoftwareRenderer = Boolean(snapshot.isSoftwareRenderer)
            state.isRemoteSession = Boolean(snapshot.isRemoteSession)
            state.systemTransparency = Boolean(snapshot.systemTransparency)
            state.systemAnimations = Boolean(snapshot.systemAnimations)
        },
    }
}
