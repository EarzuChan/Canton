import assert from "node:assert/strict"
import { test } from "node:test"
import { createInitialProjectState, type CreateProjectRequest } from "../src/feature/project/CreateProject.ts"
import { effectiveDisplayName } from "../src/feature/project/ProjectMetadata.ts"
import { projectMetadataSchema, projectStateSchema } from "../src/feature/project/ProjectState.ts"

function request(): CreateProjectRequest {
    return { rootDir: "/project", projectName: "My_App2", projectVersion: "1.0.0", frameworkVersion: "0.0.0-m.3.0", packageManager: "npm", uiDirectory: "ui", nativeDirectory: "native", artifactsDirectory: "artifacts", managedItems: {} }
}

test("新工程显示名与 CMake 目标可独立于项目名称", () => {
    const state = createInitialProjectState(request())
    assert.equal(effectiveDisplayName(state), "My_App2")
    state.project.project.name = "RenamedApp"
    state.project.project.displayName = "中文 产品名称"
    assert.equal(effectiveDisplayName(state), "中文 产品名称")
    assert.equal(state.project.native.target, "My_App2")
    assert.equal(state.project.project.icon, undefined)
    assert.equal(projectStateSchema.safeParse(state).success, true)
})

test("共享配置接受中文显示名和相对 PNG；拒绝跨平台非法文件名及逃逸路径", () => {
    const metadata = createInitialProjectState(request()).project.project
    assert.equal(projectMetadataSchema.safeParse({ ...metadata, displayName: "中文 产品 & 名称", icon: "assets/中文 图标.PNG" }).success, true)
    for (const displayName of ["", "   ", "a/b", "a\\b", "a:b", "a?b", "a*b", "a\u0000b", "a\nb", "name.", "name ", "CON", "con.txt", "LPT1", "COM².txt"]) assert.equal(projectMetadataSchema.safeParse({ ...metadata, displayName }).success, false, displayName)
    for (const icon of ["/assets/icon.png", "C:/assets/icon.png", "assets\\icon.png", "../icon.png", "assets/../icon.png", "assets//icon.png", "assets/CON.png", "assets/icon.svg"]) assert.equal(projectMetadataSchema.safeParse({ ...metadata, icon }).success, false, icon)
})
