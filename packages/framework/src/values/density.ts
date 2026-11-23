export interface Density {
    dpToPx(value: number): number
    spToPx(value: number): number
    pxToDp(value: number): number
    pxToSp(value: number): number
}

export const defaultDensity: Density = Object.freeze({
    dpToPx: (value: number) => value,
    spToPx: (value: number) => value,
    pxToDp: (value: number) => value,
    pxToSp: (value: number) => value,
})
