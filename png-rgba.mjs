import { deflateSync, inflateSync } from 'node:zlib'

const SIGNATURE = Uint8Array.from([137, 80, 78, 71, 13, 10, 26, 10])

export function decodePng(input) {
  const bytes = input instanceof Uint8Array ? input : Uint8Array.from(input)
  if (!sameBytes(bytes.subarray(0, 8), SIGNATURE)) throw new Error('not a PNG file')

  let offset = 8
  let width = 0
  let height = 0
  let colorType = 0
  let bitDepth = 0
  let interlace = 0
  const idat = []

  while (offset + 12 <= bytes.length) {
    const length = readU32(bytes, offset)
    const type = String.fromCharCode(...bytes.subarray(offset + 4, offset + 8))
    const bodyStart = offset + 8
    const bodyEnd = bodyStart + length
    if (bodyEnd + 4 > bytes.length) throw new Error('truncated PNG chunk')
    const body = bytes.subarray(bodyStart, bodyEnd)
    if (type === 'IHDR') {
      width = readU32(body, 0)
      height = readU32(body, 4)
      bitDepth = body[8]
      colorType = body[9]
      interlace = body[12]
    } else if (type === 'IDAT') {
      idat.push(body)
    } else if (type === 'IEND') {
      break
    }
    offset = bodyEnd + 4
  }

  if (!width || !height) throw new Error('PNG is missing dimensions')
  if (bitDepth !== 8 || interlace !== 0) throw new Error('only non-interlaced 8-bit PNGs are supported')
  const channels = { 0: 1, 2: 3, 4: 2, 6: 4 }[colorType]
  if (!channels) throw new Error(`unsupported PNG color type ${colorType}`)

  const rowBytes = width * channels
  const raw = Uint8Array.from(inflateSync(Buffer.concat(idat.map((part) => Buffer.from(part)))))
  const expected = height * (rowBytes + 1)
  if (raw.length < expected) throw new Error('PNG image data is truncated')

  const rows = new Uint8Array(height * rowBytes)
  let rawOffset = 0
  for (let y = 0; y < height; y += 1) {
    const filter = raw[rawOffset++]
    const current = rows.subarray(y * rowBytes, (y + 1) * rowBytes)
    const previous = y === 0 ? null : rows.subarray((y - 1) * rowBytes, y * rowBytes)
    for (let x = 0; x < rowBytes; x += 1) {
      const left = x >= channels ? current[x - channels] : 0
      const up = previous ? previous[x] : 0
      const upLeft = previous && x >= channels ? previous[x - channels] : 0
      const value = raw[rawOffset++]
      if (filter === 0) current[x] = value
      else if (filter === 1) current[x] = (value + left) & 255
      else if (filter === 2) current[x] = (value + up) & 255
      else if (filter === 3) current[x] = (value + Math.floor((left + up) / 2)) & 255
      else if (filter === 4) current[x] = (value + paeth(left, up, upLeft)) & 255
      else throw new Error(`unsupported PNG filter ${filter}`)
    }
  }

  const rgba = new Uint8Array(width * height * 4)
  for (let i = 0, p = 0; i < rows.length; i += channels, p += 4) {
    if (colorType === 6) rgba.set(rows.subarray(i, i + 4), p)
    else if (colorType === 2) {
      rgba[p] = rows[i]
      rgba[p + 1] = rows[i + 1]
      rgba[p + 2] = rows[i + 2]
      rgba[p + 3] = 255
    } else if (colorType === 4) {
      rgba[p] = rows[i]
      rgba[p + 1] = rows[i]
      rgba[p + 2] = rows[i]
      rgba[p + 3] = rows[i + 1]
    } else {
      rgba[p] = rows[i]
      rgba[p + 1] = rows[i]
      rgba[p + 2] = rows[i]
      rgba[p + 3] = 255
    }
  }
  return { width, height, data: rgba }
}

export function encodePng({ width, height, data }) {
  if (!Number.isInteger(width) || !Number.isInteger(height) || width < 1 || height < 1)
    throw new Error('PNG dimensions must be positive integers')
  if (!(data instanceof Uint8Array) || data.length !== width * height * 4)
    throw new Error('RGBA data length does not match PNG dimensions')

  const raw = new Uint8Array(height * (width * 4 + 1))
  for (let y = 0; y < height; y += 1) {
    const rowStart = y * (width * 4 + 1)
    raw[rowStart] = 0
    raw.set(data.subarray(y * width * 4, (y + 1) * width * 4), rowStart + 1)
  }
  const ihdr = new Uint8Array(13)
  writeU32(ihdr, 0, width)
  writeU32(ihdr, 4, height)
  ihdr[8] = 8
  ihdr[9] = 6
  const chunks = [chunk('IHDR', ihdr), chunk('IDAT', Uint8Array.from(deflateSync(raw))), chunk('IEND', new Uint8Array())]
  return Buffer.concat([Buffer.from(SIGNATURE), ...chunks.map((value) => Buffer.from(value))])
}

function chunk(type, body) {
  const typeBytes = Uint8Array.from(type, (value) => value.charCodeAt(0))
  const output = new Uint8Array(12 + body.length)
  writeU32(output, 0, body.length)
  output.set(typeBytes, 4)
  output.set(body, 8)
  writeU32(output, 8 + body.length, crc32(output.subarray(4, 8 + body.length)))
  return output
}

function readU32(bytes, offset) {
  return ((bytes[offset] << 24) | (bytes[offset + 1] << 16) | (bytes[offset + 2] << 8) | bytes[offset + 3]) >>> 0
}

function writeU32(bytes, offset, value) {
  bytes[offset] = (value >>> 24) & 255
  bytes[offset + 1] = (value >>> 16) & 255
  bytes[offset + 2] = (value >>> 8) & 255
  bytes[offset + 3] = value & 255
}

function sameBytes(left, right) {
  return left.length === right.length && left.every((value, index) => value === right[index])
}

function paeth(a, b, c) {
  const p = a + b - c
  const pa = Math.abs(p - a)
  const pb = Math.abs(p - b)
  const pc = Math.abs(p - c)
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c
}

const CRC_TABLE = Array.from({ length: 256 }, (_, index) => {
  let value = index
  for (let bit = 0; bit < 8; bit += 1) value = (value & 1) ? (0xedb88320 ^ (value >>> 1)) : (value >>> 1)
  return value >>> 0
})

function crc32(bytes) {
  let value = 0xffffffff
  for (const byte of bytes) value = CRC_TABLE[(value ^ byte) & 255] ^ (value >>> 8)
  return (value ^ 0xffffffff) >>> 0
}
