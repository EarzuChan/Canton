import { readFileSync, writeFileSync } from "node:fs"
import { fileURLToPath } from "node:url"
import { modifierNativeProtocol } from "../packages/shared/src/modifierNativeProtocol.ts"

const outputPath = fileURLToPath(new URL("../native/src/quickjs/ModifierNativeProtocol.inc", import.meta.url))

// 原生准入字段与回调种类表，由 Modifier 原生协议表生成；解析逻辑仍在 QuickJsModifierReader 手写
export function renderModifierNativeProtocol(): string {
    const lines = ["// 由 scripts/modifier-native-protocol.ts 依 @canton/shared 的 Modifier 原生协议表生成，勿手改", ""]
    const entries: string[] = []
    for (const [type, entry] of Object.entries(modifierNativeProtocol)) {
        const fields = entry.fields.length ? `${type}Fields` : "{}"
        if (entry.fields.length) lines.push(`inline constexpr std::string_view ${type}Fields[]{${entry.fields.map(field => `"${field}"`).join(", ")}};`)
        const events = Object.entries(entry.events ?? {})
        const callbacks = events.length ? `${type}Callbacks` : "{}"
        if (events.length) lines.push(`inline constexpr ModifierCallbackField ${type}Callbacks[]{${events.map(([field, kind]) => `{"${field}", canton::core::EventSlotKind::${kind[0].toUpperCase()}${kind.slice(1)}}`).join(", ")}};`)
        entries.push(`    {"${type}", ${fields}, ${callbacks}},`)
    }
    lines.push("", "inline constexpr ModifierProtocolEntry modifierProtocolEntries[]{", ...entries, "};", "")
    return lines.join("\n")
}

export function syncModifierNativeProtocol(): void {
    writeFileSync(outputPath, renderModifierNativeProtocol())
}

export function assertModifierNativeProtocol(): void {
    if (readFileSync(outputPath, "utf8").replace(/\r\n/g, "\n") !== renderModifierNativeProtocol()) throw new Error("ModifierNativeProtocol.inc 与 Modifier 原生协议表不一致，请运行 pnpm sync:modifier-protocol")
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
    if (process.argv.includes("--check")) {
        assertModifierNativeProtocol()
        console.log("[ModifierNativeProtocol]", "verified native modifier protocol")
    } else {
        syncModifierNativeProtocol()
        console.log("[ModifierNativeProtocol]", "synced native modifier protocol")
    }
}
