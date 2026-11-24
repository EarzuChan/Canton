import { isIdentifier } from './general.ts'

export function cantonParameterName(value: unknown, source?: string): string {
    if (typeof value !== 'string' || !isIdentifier(value)) throw new TypeError(`参数名称必须是合法标识符：${String(value)}${source ? `\n来源：${source}` : ''}`)

    return value
}
