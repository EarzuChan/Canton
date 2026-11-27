import { FontWeight, type TextStyleProp } from '@canton/framework/ui'

export const palette = {
    bg: 0xff0b1120,
    surface: 0xff111827,
    card: 0xff1e293b,
    border: 0xff334155,
    accent: 0xff2563eb,
    accentHover: 0xff3b82f6,
    emerald: 0xff10b981,
    amber: 0xfff59e0b,
    rose: 0xffef4444,
    text: 0xfff8fafc,
    muted: 0xff94a3b8,
}

export const titleStyle: TextStyleProp = { fontSize: 18, fontWeight: FontWeight.Bold, color: palette.text }
export const headingStyle: TextStyleProp = { fontSize: 14, fontWeight: FontWeight.SemiBold, color: palette.text }
export const bodyStyle: TextStyleProp = { fontSize: 12, color: palette.text }
export const mutedStyle: TextStyleProp = { fontSize: 11, color: palette.muted }
