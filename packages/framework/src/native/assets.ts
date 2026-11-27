export const assets = Object.freeze({
    async readText(reference: string): Promise<string> {
        const native = globalThis.__CANTON_NATIVE__
        if (!native?.readAsset) throw new Error('当前 App 不提供资产读取')
        return native.readAsset(reference, true)
    },
    async readBytes(reference: string): Promise<Uint8Array> {
        const native = globalThis.__CANTON_NATIVE__
        if (!native?.readAsset) throw new Error('当前 App 不提供资产读取')
        return native.readAsset(reference, false)
    },
    async exists(reference: string): Promise<boolean> {
        const native = globalThis.__CANTON_NATIVE__
        if (!native?.assetExists) throw new Error('当前 App 不提供资产存在性查询')
        return native.assetExists(reference)
    },
})
