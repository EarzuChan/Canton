import assert from 'node:assert/strict'
import test from 'node:test'
import { join } from 'node:path'
import { projectLocationHint } from '../src/business/ProjectLocation.ts'

test('切换目录提示按本机 shell 引用空格和单引号，选项前缀与通配符不会误解释', () => {
    assert.equal(projectLocationHint(process.cwd()), '')
    const apostrophe = projectLocationHint(join(process.cwd(), "Earzu's Plugin"))
    assert.ok(apostrophe.includes("cd 'Earzu''s Plugin'"))
    const dash = projectLocationHint(join(process.cwd(), '-Plugin'))
    assert.ok(dash.includes("cd '.\\-Plugin'"))
    assert.match(projectLocationHint(join(process.cwd(), '[Plugin]')), /cd -LiteralPath '\[Plugin\]'/)
})
