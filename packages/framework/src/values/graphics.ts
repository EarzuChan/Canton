import { requireArgb } from './color.ts'

export const BlendMode = Object.freeze({
    Clear: 'Clear',
    Src: 'Src',
    Dst: 'Dst',
    SrcOver: 'SrcOver',
    DstOver: 'DstOver',
    SrcIn: 'SrcIn',
    DstIn: 'DstIn',
    SrcOut: 'SrcOut',
    DstOut: 'DstOut',
    SrcAtop: 'SrcAtop',
    DstAtop: 'DstAtop',
    Xor: 'Xor',
    Plus: 'Plus',
    Modulate: 'Modulate',
    Screen: 'Screen',
    Overlay: 'Overlay',
    Darken: 'Darken',
    Lighten: 'Lighten',
    ColorDodge: 'ColorDodge',
    ColorBurn: 'ColorBurn',
    HardLight: 'HardLight',
    SoftLight: 'SoftLight',
    Difference: 'Difference',
    Exclusion: 'Exclusion',
    Multiply: 'Multiply',
    Hue: 'Hue',
    Saturation: 'Saturation',
    Color: 'Color',
    Luminosity: 'Luminosity',
} as const)

export type BlendMode = typeof BlendMode[keyof typeof BlendMode]

export const BLEND_MODE_ORDER: readonly BlendMode[] = Object.freeze([
    BlendMode.Clear,
    BlendMode.Src,
    BlendMode.Dst,
    BlendMode.SrcOver,
    BlendMode.DstOver,
    BlendMode.SrcIn,
    BlendMode.DstIn,
    BlendMode.SrcOut,
    BlendMode.DstOut,
    BlendMode.SrcAtop,
    BlendMode.DstAtop,
    BlendMode.Xor,
    BlendMode.Plus,
    BlendMode.Modulate,
    BlendMode.Screen,
    BlendMode.Overlay,
    BlendMode.Darken,
    BlendMode.Lighten,
    BlendMode.ColorDodge,
    BlendMode.ColorBurn,
    BlendMode.HardLight,
    BlendMode.SoftLight,
    BlendMode.Difference,
    BlendMode.Exclusion,
    BlendMode.Multiply,
    BlendMode.Hue,
    BlendMode.Saturation,
    BlendMode.Color,
    BlendMode.Luminosity,
])

const blendModeIndices = new Map<string, number>(BLEND_MODE_ORDER.map((mode, index) => [mode, index]))

export function blendModeToIndex(mode: BlendMode): number {
    const index = blendModeIndices.get(mode)
    if (index === undefined) throw new TypeError(`BlendMode 无效：${String(mode)}`)
    return index
}

export const TileMode = Object.freeze({
    Clamp: 'Clamp',
    Repeated: 'Repeated',
    Mirror: 'Mirror',
    Decal: 'Decal',
} as const)

export type TileMode = typeof TileMode[keyof typeof TileMode]

const tileModeIndices: Readonly<Record<TileMode, number>> = {
    Clamp: 0,
    Repeated: 1,
    Mirror: 2,
    Decal: 3,
}

export function tileModeToIndex(mode: TileMode): number {
    if (!Object.hasOwn(tileModeIndices, mode)) throw new TypeError(`TileMode 无效：${String(mode)}`)
    return tileModeIndices[mode]
}

export const CompositingStrategy = Object.freeze({
    Auto: 'Auto',
    Offscreen: 'Offscreen',
    ModulateAlpha: 'ModulateAlpha',
} as const)

export type CompositingStrategy = typeof CompositingStrategy[keyof typeof CompositingStrategy]

const compositingStrategyIndices: Readonly<Record<CompositingStrategy, number>> = {
    Auto: 0,
    Offscreen: 1,
    ModulateAlpha: 2,
}

export function compositingStrategyToIndex(strategy: CompositingStrategy): number {
    if (!Object.hasOwn(compositingStrategyIndices, strategy)) throw new TypeError(`CompositingStrategy 无效：${String(strategy)}`)
    return compositingStrategyIndices[strategy]
}

function finiteNumber(value: number, name: string): number {
    if (typeof value !== 'number' || !Number.isFinite(value)) throw new TypeError(`${name} 需要有限数值`)
    return value
}

// 4x5 行主序色彩矩阵；第 5 列偏置（下标 4, 9, 14, 19）对齐 Compose 使用 [0..255] 量纲
export class ColorMatrix {
    readonly values: Float32Array

    constructor(values?: ArrayLike<number>) {
        this.values = new Float32Array(20)
        if (values) this.set(values)
        else this.reset()
    }

    get(row: number, column: number): number {
        if (!Number.isInteger(row) || row < 0 || row > 3 || !Number.isInteger(column) || column < 0 || column > 4) {
            throw new RangeError('ColorMatrix 下标越界：row 为 0..3，column 为 0..4')
        }
        return this.values[row * 5 + column]
    }

    set(src: ColorMatrix | ArrayLike<number>): this
    set(row: number, column: number, value: number): this
    set(rowOrSrc: number | ColorMatrix | ArrayLike<number>, column?: number, value?: number): this {
        if (typeof rowOrSrc === 'number') {
            if (!Number.isInteger(rowOrSrc) || rowOrSrc < 0 || rowOrSrc > 3 || !Number.isInteger(column) || column! < 0 || column! > 4) {
                throw new RangeError('ColorMatrix 下标越界：row 为 0..3，column 为 0..4')
            }
            this.values[rowOrSrc * 5 + column!] = finiteNumber(value!, 'ColorMatrix 元素')
            return this
        }
        const source = rowOrSrc instanceof ColorMatrix ? rowOrSrc.values : rowOrSrc
        if (!source || source.length < 20) throw new RangeError('ColorMatrix 需要 20 个数值')
        for (let i = 0; i < 20; i++) this.values[i] = finiteNumber(source[i], `ColorMatrix[${i}]`)
        return this
    }

    reset(): this {
        const v = this.values
        v.fill(0)
        v[0] = 1
        v[6] = 1
        v[12] = 1
        v[18] = 1
        return this
    }

    setToSaturation(sat: number): this {
        finiteNumber(sat, 'saturation')
        this.reset()
        const invSat = 1 - sat
        const r = 0.213 * invSat
        const g = 0.715 * invSat
        const b = 0.072 * invSat
        const v = this.values
        v[0] = r + sat
        v[1] = g
        v[2] = b
        v[5] = r
        v[6] = g + sat
        v[7] = b
        v[10] = r
        v[11] = g
        v[12] = b + sat
        return this
    }

    setToScale(redScale: number, greenScale: number, blueScale: number, alphaScale: number): this {
        this.reset()
        const v = this.values
        v[0] = finiteNumber(redScale, 'redScale')
        v[6] = finiteNumber(greenScale, 'greenScale')
        v[12] = finiteNumber(blueScale, 'blueScale')
        v[18] = finiteNumber(alphaScale, 'alphaScale')
        return this
    }

    private rotateInternal(degrees: number, apply: (cosine: number, sine: number) => void): this {
        finiteNumber(degrees, 'degrees')
        this.reset()
        const radians = (degrees * Math.PI) / 180
        apply(Math.cos(radians), Math.sin(radians))
        return this
    }

    setToRotateRed(degrees: number): this {
        return this.rotateInternal(degrees, (cosine, sine) => {
            this.values[6] = cosine
            this.values[7] = sine
            this.values[11] = -sine
            this.values[12] = cosine
        })
    }

    setToRotateGreen(degrees: number): this {
        return this.rotateInternal(degrees, (cosine, sine) => {
            this.values[0] = cosine
            this.values[2] = -sine
            this.values[10] = sine
            this.values[12] = cosine
        })
    }

    setToRotateBlue(degrees: number): this {
        return this.rotateInternal(degrees, (cosine, sine) => {
            this.values[0] = cosine
            this.values[1] = sine
            this.values[5] = -sine
            this.values[6] = cosine
        })
    }

    multiply(other: ColorMatrix): this {
        if (!(other instanceof ColorMatrix)) throw new TypeError('ColorMatrix.multiply 需要 ColorMatrix 实例')
        const a = this.values
        const b = other.values
        const out = new Float32Array(20)
        for (let row = 0; row < 4; row++) {
            const r = row * 5
            for (let col = 0; col < 4; col++) {
                out[r + col] = a[r] * b[col] + a[r + 1] * b[5 + col] + a[r + 2] * b[10 + col] + a[r + 3] * b[15 + col]
            }
            out[r + 4] = a[r] * b[4] + a[r + 1] * b[9] + a[r + 2] * b[14] + a[r + 3] * b[19] + a[r + 4]
        }
        this.values.set(out)
        return this
    }

    convertRgbToYuv(): this {
        this.reset()
        const v = this.values
        v[0] = 0.299
        v[1] = 0.587
        v[2] = 0.114
        v[5] = -0.16874
        v[6] = -0.33126
        v[7] = 0.5
        v[10] = 0.5
        v[11] = -0.41869
        v[12] = -0.08131
        return this
    }

    convertYuvToRgb(): this {
        this.reset()
        const v = this.values
        v[0] = 1
        v[1] = 0
        v[2] = 1.402
        v[5] = 1
        v[6] = -0.34414
        v[7] = -0.71414
        v[10] = 1
        v[11] = 1.772
        v[12] = 0
        return this
    }

    clone(): ColorMatrix {
        return new ColorMatrix(this.values)
    }
}

export type TintColorFilter = Readonly<{ type: 'tint'; color: number; blendMode: BlendMode }>
export type ColorMatrixColorFilter = Readonly<{ type: 'colorMatrix'; matrix: readonly number[] }>
export type LightingColorFilter = Readonly<{ type: 'lighting'; multiply: number; add: number }>
export type ColorFilter = TintColorFilter | ColorMatrixColorFilter | LightingColorFilter

export const ColorFilter = Object.freeze({
    tint(color: number, blendMode: BlendMode = BlendMode.SrcIn): TintColorFilter {
        blendModeToIndex(blendMode)
        return Object.freeze({ type: 'tint', color: requireArgb(color), blendMode })
    },
    colorMatrix(matrix: ColorMatrix | ArrayLike<number>): ColorMatrixColorFilter {
        const source = matrix instanceof ColorMatrix ? matrix.values : matrix
        if (!source || source.length < 20) throw new RangeError('ColorFilter.colorMatrix 需要 20 个数值')
        const copy = new Array<number>(20)
        for (let i = 0; i < 20; i++) copy[i] = finiteNumber(source[i], `colorMatrix[${i}]`)
        return Object.freeze({ type: 'colorMatrix', matrix: Object.freeze(copy) })
    },
    lighting(multiply: number, add: number): LightingColorFilter {
        return Object.freeze({ type: 'lighting', multiply: requireArgb(multiply), add: requireArgb(add) })
    },
})

export function isColorFilter(value: unknown): value is ColorFilter {
    if (typeof value !== 'object' || value === null) return false
    const type = (value as { type?: unknown }).type
    return type === 'tint' || type === 'colorMatrix' || type === 'lighting'
}

export function normalizeColorFilter(value: ColorFilter | Readonly<{ tint: number }> | undefined): ColorFilter | undefined {
    if (value === undefined) return undefined
    if (isColorFilter(value)) return value
    if (typeof value === 'object' && value !== null && 'tint' in value && typeof value.tint === 'number') {
        return ColorFilter.tint(value.tint, BlendMode.SrcIn)
    }
    throw new TypeError('colorFilter 需要 ColorFilter 或 { tint } 对象')
}

// 供 values/primitives.ts 与上层 draw/shader.ts 解耦的最小结构契约
export interface RuntimeShaderLike {
    readonly __cantonRuntimeShader: true
}

export type ShaderBrush = Readonly<{
    type: 'shader'
    shader: RuntimeShaderLike
}>
