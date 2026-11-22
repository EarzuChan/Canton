import { PNG } from "pngjs"
import type { IconPixels } from "./PngIcon.ts"

export const icoSizes = [16, 24, 32, 48, 64, 128, 256] as const
export function encodeIconPng(image: IconPixels): Buffer {
    const png = new PNG({ width: image.size, height: image.size })
    png.data = image.data
    return PNG.sync.write(png, { colorType: 6, bitDepth: 8, inputHasAlpha: true })
}

function encodeIconBitmap(image: IconPixels): Buffer {
    const headerSize = 40
    const colorBytes = image.size * image.size * 4
    const maskStride = Math.ceil(image.size / 32) * 4
    const bytes = Buffer.alloc(headerSize + colorBytes + maskStride * image.size)
    bytes.writeUInt32LE(headerSize, 0)
    bytes.writeInt32LE(image.size, 4)
    bytes.writeInt32LE(image.size * 2, 8)
    bytes.writeUInt16LE(1, 12)
    bytes.writeUInt16LE(32, 14)
    bytes.writeUInt32LE(colorBytes + maskStride * image.size, 20)
    for (let row = 0; row < image.size; row++) for (let column = 0; column < image.size; column++) {
        const origin = (row * image.size + column) * 4
        const flippedRow = image.size - row - 1
        const destination = headerSize + (flippedRow * image.size + column) * 4
        bytes[destination] = image.data[origin + 2]
        bytes[destination + 1] = image.data[origin + 1]
        bytes[destination + 2] = image.data[origin]
        bytes[destination + 3] = image.data[origin + 3]
        if (image.data[origin + 3] === 0) bytes[headerSize + colorBytes + flippedRow * maskStride + Math.floor(column / 8)] |= 0x80 >> (column % 8)
    }
    return bytes
}

export function encodeIco(images: ReadonlyMap<number, IconPixels>): Buffer {
    const sizes = icoSizes.filter(size => images.has(size))
    const entries = sizes.map(size => size === 256 ? encodeIconPng(images.get(size)!) : encodeIconBitmap(images.get(size)!))
    const header = Buffer.alloc(6 + sizes.length * 16)
    header.writeUInt16LE(1, 2)
    header.writeUInt16LE(sizes.length, 4)
    let offset = header.length
    for (const [index, size] of sizes.entries()) {
        const entry = 6 + index * 16
        header[entry] = size === 256 ? 0 : size
        header[entry + 1] = header[entry]
        header.writeUInt16LE(1, entry + 4)
        header.writeUInt16LE(32, entry + 6)
        header.writeUInt32LE(entries[index].length, entry + 8)
        header.writeUInt32LE(offset, entry + 12)
        offset += entries[index].length
    }
    return Buffer.concat([header, ...entries])
}
