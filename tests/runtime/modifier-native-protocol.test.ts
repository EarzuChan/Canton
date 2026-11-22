import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync, readdirSync } from 'node:fs'
import { modifierNativeProtocol } from '@canton/shared'
import { assertModifierNativeProtocol } from '../../scripts/modifier-native-protocol.ts'

test('原生准入字段表与 Modifier 原生协议表一致', () => {
    assert.doesNotThrow(assertModifierNativeProtocol)
})

test('JS 降解产出的 Modifier 种类都在原生协议表中声明', () => {
    const root = new URL('../../packages/framework/src/', import.meta.url)
    const types = new Set<string>()
    for (const file of readdirSync(root, { recursive: true, encoding: 'utf8' }).filter(name => name.endsWith('.ts'))) {
        const source = readFileSync(new URL(file.replace(/\\/g, '/'), root), 'utf8')
        for (const match of source.matchAll(/#(?:add|interaction)\((?:[^,()]*,\s*)?['"]([a-zA-Z]+)['"]/g)) types.add(match[1])
    }
    assert.ok(types.size > 30, '没有扫描到 Modifier 种类')
    for (const type of types) assert.ok(Object.hasOwn(modifierNativeProtocol, type), `Modifier ${type} 未在原生协议表中声明`)
})

test('原生协议表的回调字段都是声明过的字段', () => {
    for (const [type, entry] of Object.entries(modifierNativeProtocol))
        for (const field of Object.keys(entry.events ?? {})) assert.ok(entry.fields.includes(field), `${type}.${field} 不在字段表中`)
})
