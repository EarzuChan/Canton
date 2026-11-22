import test from 'node:test'
import assert from 'node:assert/strict'
import * as core from '@canton/framework'
import * as foundation from '@canton/framework/foundation'
import * as ui from '@canton/framework/ui'
import * as animation from '@canton/framework/animation'
import * as internal from '@canton/framework/internal'
import { frameScope } from './frameScope.ts'
import { requireCtaModule } from './ctaModules.ts'

test('分层入口、编译协议与动画通道共享响应式身份', () => {
    assert.equal(typeof core.Log, 'object')

    assert.equal(requireCtaModule('@canton/framework/foundation'), foundation)
    assert.equal(requireCtaModule('@canton/framework/ui'), ui)
    assert.equal(requireCtaModule('@canton/framework/animation'), animation)
    assert.equal(requireCtaModule('@canton/framework/internal'), internal)
    assert.equal(core.ref, internal.ref)
    assert.ok(ui.M instanceof ui.Modifier)

    const target = core.ref(1)
    const clock = frameScope()
    const value = clock.run(() => animation.animatedNumberAsRef(target, { animationSpec: animation.tween({ durationMillis: 10 }) }))
    assert.ok(core.isRef(value))
    target.value = 2
    clock.advanceBy(10)
    assert.equal(value.value, 2)
    clock.stop()
})
