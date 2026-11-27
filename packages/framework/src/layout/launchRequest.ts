import type { InjectionKey } from '../runtime/apiInject.ts'
import type { NativeLaunchRequest } from '../native/protocol.ts'

export interface LaunchRequest {
    readonly args: readonly string[]
    readonly workingDirectory: string
}

export const LaunchRequestKey: InjectionKey<LaunchRequest> = Symbol('Canton.LaunchRequest')

export function freezeLaunchRequest(raw?: NativeLaunchRequest): LaunchRequest {
    const args = raw?.args ? Object.freeze([...raw.args]) : Object.freeze([])
    const workingDirectory = typeof raw?.workingDirectory === 'string' ? raw.workingDirectory : ''
    return Object.freeze({ args, workingDirectory })
}
