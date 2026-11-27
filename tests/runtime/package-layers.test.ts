import test from 'node:test'
import assert from 'node:assert/strict'
import { findCycles, layerOf, moduleGraph, packageImports, packageSpecifiers, type ModuleGraph } from '../helpers/moduleGraph.ts'

// 层序：只能依赖序号更小的层或本层；同序号的不同层互不依赖，根目录入口位于最上层
const frameworkLayers: Readonly<Record<string, number>> = { values: 0, native: 1, runtime: 2, animation: 3, draw: 3, layout: 4, modifier: 5, bridge: 6, arrangable: 7, app: 8, '.': 9 }
const compilerLayers: Readonly<Record<string, number>> = { syntax: 0, template: 1, cta: 2, script: 3, values: 4, '.': 5 }

function layerViolations(graph: ModuleGraph, layers: Readonly<Record<string, number>>): string[] {
    const violations: string[] = []
    for (const [file, edges] of graph) {
        const from = layerOf(file)
        assert.ok(from in layers, `${file} 不在已登记的层中`)
        for (const { target } of edges) {
            const to = layerOf(target)
            if (to !== from && !(layers[to]! < layers[from]!)) violations.push(`${file} -> ${target}`)
        }
    }
    return violations
}

test('compiler 与 framework 目录按层单向依赖', () => {
    assert.deepEqual(layerViolations(moduleGraph('packages/compiler/src'), compilerLayers), [])
    assert.deepEqual(layerViolations(moduleGraph('packages/framework/src'), frameworkLayers), [])
})

test('各包内部零循环导入（含仅类型的导入）', () => {
    for (const name of ['shared', 'reactivity', 'compiler', 'vite-plugin', 'framework', 'kyant-glass'])
        assert.deepEqual(findCycles(moduleGraph(`packages/${name}/src`)).map(cycle => cycle.join(' -> ')), [], name)
})

test('包之间单向依赖：shared ← reactivity / compiler ← vite-plugin ← framework ← kyant-glass', () => {
    const allowed: Readonly<Record<string, readonly string[]>> = {
        shared: [],
        reactivity: ['@canton/shared'],
        compiler: ['@canton/shared'],
        'vite-plugin': ['@canton/compiler', '@canton/shared'],
        framework: ['@canton/compiler', '@canton/reactivity', '@canton/shared', '@canton/vite-plugin'],
        'kyant-glass': ['@canton/framework'],
    }
    for (const [name, packages] of Object.entries(allowed)) {
        const extra = [...packageImports(`packages/${name}/src`)].filter(item => !packages.includes(item))
        assert.deepEqual(extra, [], `${name} 不应依赖 ${extra.join('、')}`)
    }
    const kyantPublicOnly = new Set(['@canton/framework', '@canton/framework/ui', '@canton/framework/foundation', '@canton/framework/animation', '@canton/framework/shell', '@canton/framework/fs', '@canton/framework/net'])
    const forbiddenKyant = [...packageSpecifiers('packages/kyant-glass/src')].filter(item => !kyantPublicOnly.has(item))
    assert.deepEqual(forbiddenKyant, [], `kyant-glass 只能导入 @canton/framework 公开出口，不允许：${forbiddenKyant.join('、')}`)
})
