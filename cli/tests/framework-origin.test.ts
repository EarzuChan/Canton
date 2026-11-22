import assert from "node:assert/strict"
import { test } from "node:test"
import { stateFor } from "./fixture.ts"
import { frameworkRegistryUrl } from "../src/feature/framework/FrameworkOrigin.ts"

test("本机来源优先于共享来源，未设置时回落到共享配置与官方默认", () => {
    const state = stateFor("E:/work/Demo")
    assert.equal(frameworkRegistryUrl(state), null)
    state.project.framework.nodeRegistryUrl = "https://shared.example.test"
    assert.equal(frameworkRegistryUrl(state), "https://shared.example.test")
    state.local = { framework: { nodeRegistryUrl: "http://localhost:4873" } }
    assert.equal(frameworkRegistryUrl(state), "http://localhost:4873")
})
