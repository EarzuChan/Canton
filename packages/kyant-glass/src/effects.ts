import { ColorFilter, ColorMatrix, RectangleShape, RenderEffect, TileMode, createRuntimeShader, type Density, type DrawSize, type RuntimeEffect, type RuntimeShader, type Shape } from '@canton/framework/ui'
import { resolveShapeCornerRadiiPx } from './internal/shapeCorners.ts'
import refractionEffect from './shaders/refraction.hlsl'
import refractionDispersionEffect from './shaders/refractionDispersion.hlsl'

export function createColorControlsColorFilter(brightness = 0, contrast = 1, saturation = 1): ColorFilter {
    const invSat = 1 - saturation
    const r = 0.213 * invSat
    const g = 0.715 * invSat
    const b = 0.072 * invSat

    const c = contrast
    const t = (0.5 - c * 0.5 + brightness) * 255
    const s = saturation

    const cr = c * r
    const cg = c * g
    const cb = c * b
    const cs = c * s

    return ColorFilter.colorMatrix(
        new ColorMatrix([
            cr + cs, cg, cb, 0, t,
            cr, cg + cs, cb, 0, t,
            cr, cg, cb + cs, 0, t,
            0, 0, 0, 1, 0,
        ]),
    )
}

const vibrantColorFilter = createColorControlsColorFilter(0, 1, 1.5)

export interface LensEffectOptions {
    refractionHeight: number
    refractionAmount: number
    depthEffect?: boolean
    chromaticAberration?: boolean
}

export interface ColorControlsOptions {
    brightness?: number
    contrast?: number
    saturation?: number
}

export class BackdropEffectScope implements Density {
    private currentDensity: Density = { dpToPx: (v) => v, pxToDp: (v) => v, spToPx: (v) => v, pxToSp: (v) => v }
    size: DrawSize = { width: 0, height: 0 }
    shape: Shape = RectangleShape
    padding = 0
    renderEffect: RenderEffect | null = null

    private readonly shaderCache = new Map<string, RuntimeShader>()

    dpToPx(dp: number): number {
        return this.currentDensity.dpToPx(dp)
    }
    pxToDp(px: number): number {
        return this.currentDensity.pxToDp(px)
    }
    spToPx(sp: number): number {
        return this.currentDensity.spToPx(sp)
    }
    pxToSp(px: number): number {
        return this.currentDensity.pxToSp(px)
    }

    obtainRuntimeShader(key: string, runtimeEffect: RuntimeEffect): RuntimeShader {
        let shader = this.shaderCache.get(key)
        if (!shader) {
            shader = createRuntimeShader(runtimeEffect)
            this.shaderCache.set(key, shader)
        }
        return shader
    }

    update(size: DrawSize, density: Density, shape: Shape): void {
        this.size = size
        this.currentDensity = density
        this.shape = shape
    }

    apply(effects: (scope: BackdropEffectScope) => void): void {
        this.padding = 0
        this.renderEffect = null
        effects(this)
    }

    dispose(): void {
        for (const shader of this.shaderCache.values()) {
            shader.dispose()
        }
        this.shaderCache.clear()
        this.padding = 0
        this.renderEffect = null
    }

    effect(nextEffect: RenderEffect): void {
        this.renderEffect = this.renderEffect ? RenderEffect.chain(this.renderEffect, nextEffect) : nextEffect
    }

    runtimeShaderEffect(shader: RuntimeShader, uniformShaderName = 'content'): void {
        this.effect(RenderEffect.shader(shader, uniformShaderName))
    }

    blur(radiusPx: number, edgeTreatment: TileMode = TileMode.Clamp): void {
        if (radiusPx <= 0) return
        if (edgeTreatment !== TileMode.Clamp || this.renderEffect !== null) {
            if (radiusPx > this.padding) {
                this.padding = radiusPx
            }
        }
        this.effect(RenderEffect.blur(radiusPx, radiusPx, edgeTreatment))
    }

    colorFilter(filter: ColorFilter): void {
        this.effect(RenderEffect.colorFilter(filter))
    }

    opacity(alpha: number): void {
        const clamped = Math.max(0, Math.min(1, alpha))
        this.colorFilter(
            ColorFilter.colorMatrix(
                new ColorMatrix([
                    1, 0, 0, 0, 0,
                    0, 1, 0, 0, 0,
                    0, 0, 1, 0, 0,
                    0, 0, 0, clamped, 0,
                ]),
            ),
        )
    }

    colorControls(options: ColorControlsOptions = {}): void {
        const brightness = options.brightness ?? 0
        const contrast = options.contrast ?? 1
        const saturation = options.saturation ?? 1
        if (brightness === 0 && contrast === 1 && saturation === 1) return
        this.colorFilter(createColorControlsColorFilter(brightness, contrast, saturation))
    }

    vibrancy(): void {
        this.colorFilter(vibrantColorFilter)
    }

    lens(options: LensEffectOptions): void {
        const { refractionHeight, refractionAmount, depthEffect = false, chromaticAberration = false } = options
        if (refractionHeight <= 0 || refractionAmount <= 0) return

        if (this.padding > 0) {
            this.padding = Math.max(0, this.padding - refractionHeight)
        }

        const corners = resolveShapeCornerRadiiPx(this.shape, this.size, this)
        const shader = chromaticAberration ? this.obtainRuntimeShader('RefractionWithDispersion', refractionDispersionEffect) : this.obtainRuntimeShader('Refraction', refractionEffect)

        shader.setFloat('size', this.size.width, this.size.height).setFloat('offset', -this.padding, -this.padding).setFloat('cornerRadii', corners[0], corners[1], corners[2], corners[3]).setFloat('refractionHeight', refractionHeight).setFloat('refractionAmount', -refractionAmount).setFloat('depthEffect', depthEffect ? 1 : 0)

        if (chromaticAberration) {
            shader.setFloat('chromaticAberration', 1)
        }

        this.runtimeShaderEffect(shader, 'content')
    }
}
