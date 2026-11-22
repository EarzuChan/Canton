import type { TextStyleProp } from '@canton/framework/ui'

export const fontFamily = 'Canton Test Sans'

// 文字样式在这里统一；颜色在各 .cta 里用 Color(...) 声明，值单位检查才认得
const text = 0xffe2e8f0, muted = 0xff94a3b8

export const title: TextStyleProp = { fontSize: 18, color: text, fontFamily, fontWeight: 700 }
export const body: TextStyleProp = { fontSize: 13, color: text, fontFamily }
export const caption: TextStyleProp = { fontSize: 10, color: muted, fontFamily }
