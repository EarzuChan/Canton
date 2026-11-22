import type { Node } from '@babel/types'
import type { ScriptCompileContext } from './context.ts'
import { DEFINE_SLOTS, isCallOf } from './utils.ts'

// defineSlots<T>() 只声明各槽位交付给内容的参数类型，不产生运行期代码
export function processDefineSlots(ctx: ScriptCompileContext, node: Node): boolean {
    if (!isCallOf(node, DEFINE_SLOTS)) return false
    if (ctx.slotsCall) ctx.error(`${DEFINE_SLOTS}() 只能调用一次`, node)
    ctx.slotsCall = node
    return true
}
