import type { Position } from './ast.ts'
import { isWhitespace } from './tokenizer.ts'

export const forAliasRE: RegExp = /([\s\S]*?)\s+(?:in|of)\s+(\S[\s\S]*)/

export function isAllWhitespace(str: string): boolean {
    for (let i = 0; i < str.length; i++) if (!isWhitespace(str.charCodeAt(i))) return false
    return true
}

export function advancePositionWithClone(pos: Position, source: string, numberOfCharacters: number = source.length): Position {
    return advancePositionWithMutation({ offset: pos.offset, line: pos.line, column: pos.column }, source, numberOfCharacters)
}

// 按原始源码前进，模板表达式与错误诊断共用位置算法
export function advancePositionWithMutation(pos: Position, source: string, numberOfCharacters: number = source.length): Position {
    let linesCount = 0
    let lastNewLinePos = -1
    for (let i = 0; i < numberOfCharacters; i++) {
        if (source.charCodeAt(i) === 10) {
            linesCount++
            lastNewLinePos = i
        }
    }
    pos.offset += numberOfCharacters
    pos.line += linesCount
    pos.column = lastNewLinePos === -1 ? pos.column + numberOfCharacters : numberOfCharacters - lastNewLinePos
    return pos
}
