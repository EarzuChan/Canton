import type { NativeTransactionTarget } from '../../packages/framework/src/native/protocol.ts'
import { CANTON_RUNTIME_VERSION } from '../../packages/framework/src/native/protocol.ts'

// 补齐真实宿主必然提供的全部成员，测试按需覆盖
export function createFakeNative(overrides: Partial<NativeTransactionTarget> = {}): NativeTransactionTarget {
    let handle = 1n, pixelVersion = 1
    return {
        focusCommand: () => false,
        installFrameDriver: () => { },
        installLayoutDriver: () => { },
        continueRearrange: () => { },
        currentTime: () => 0,
        requestFrame: () => { },
        beginRearrange: () => { },
        submitRearrange: complete => complete(),
        abortRearrange: () => { },
        acquirePainter: () => ({ identity: handle++, generation: 1n }),
        releasePainter: () => { },
        defineFontFamily: () => { },
        removeFontFamily: () => { },
        beginFontPreload: () => { },
        endFontPreload: () => { },
        createPixelBuffer: () => ({ identity: handle++, generation: 1n }),
        commitPixelBuffer: () => ++pixelVersion,
        registerBinding: () => ({ identity: handle++, generation: 1n }),
        updateBinding: () => { },
        releaseBinding: () => { },
        modifierInstances: () => [],
        registerModifierBinding: () => ({ identity: handle++, generation: 1n }),
        runtimeVersion: CANTON_RUNTIME_VERSION,
        createNode: () => { },
        deleteNode: () => { },
        insertChild: () => { },
        removeChild: () => { },
        unmount: () => { },
        log: () => { },
        diagnosticsToast: () => { },
        diagnosticsRequestReload: () => { },
        diagnosticsTriggerFakeError: () => { },
        diagnosticsSetToastsEnabled: () => { },
        ...overrides,
    }
}
