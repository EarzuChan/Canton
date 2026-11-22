interface DisplayNameState {
    readonly project: { readonly project: { readonly name: string, readonly displayName?: string } }
}

export function effectiveDisplayName(state: DisplayNameState): string {
    return state.project.project.displayName ?? state.project.project.name
}

export function validateDisplayName(value: string): string | undefined {
    if (!value.trim()) return "显示名称不能为空"
    if (/[\p{Cc}<>:"/\\|?*]/u.test(value)) return "显示名称不得包含控制字符或跨平台文件名禁用字符"
    if (/[. ]$/.test(value)) return "显示名称不得以点或空格结尾"
    if (/^(?:con|prn|aux|nul|com[1-9¹²³]|lpt[1-9¹²³]|conin\$|conout\$)(?:\.|$)/i.test(value)) return "显示名称不得使用 Windows 保留文件名"
    return undefined
}

export function validateIconPath(value: string): string | undefined {
    if (!value || /^[\\/]|^[A-Za-z]:/.test(value) || value.includes("\\")) return "图标须使用工程根内的相对 PNG 路径，以 '/' 分隔目录"
    const parts = value.split("/")
    if (parts.some(part => !part || part === "." || part === ".." || validateDisplayName(part))) return "图标路径不得包含空段、'.'、'..' 或非法文件名"
    if (!/\.png$/i.test(value)) return "图标源文件必须为 PNG"
    return undefined
}
