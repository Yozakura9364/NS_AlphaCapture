import { readFileSync, writeFileSync } from 'node:fs'
import { decodePng, encodePng } from './png-rgba.mjs'

export function reconstructRgba(blackData, whiteData, blackImage, whiteImage) {
  if (blackImage && whiteImage &&
      (blackImage.width !== whiteImage.width || blackImage.height !== whiteImage.height))
    throw new Error('black and white captures must have the same dimensions')
  if (blackData.length !== whiteData.length || blackData.length % 4 !== 0)
    throw new Error('black and white captures must have the same dimensions')

  const output = new Uint8Array(blackData.length)
  for (let index = 0; index < blackData.length; index += 4) {
    const delta = (
      (whiteData[index] - blackData[index]) +
      (whiteData[index + 1] - blackData[index + 1]) +
      (whiteData[index + 2] - blackData[index + 2])
    ) / (3 * 255)
    const alpha = clamp01(1 - delta)
    const alphaByte = Math.round(alpha * 255)
    output[index + 3] = alphaByte
    if (alphaByte === 0) {
      output[index] = 0
      output[index + 1] = 0
      output[index + 2] = 0
      continue
    }
    output[index] = clampByte(blackData[index] / alpha)
    output[index + 1] = clampByte(blackData[index + 1] / alpha)
    output[index + 2] = clampByte(blackData[index + 2] / alpha)
  }
  return output
}

export function reconstructPng(blackPath, whitePath, outputPath) {
  const black = decodePng(readFileSync(blackPath))
  const white = decodePng(readFileSync(whitePath))
  const rgba = reconstructRgba(black.data, white.data, black, white)
  writeFileSync(outputPath, encodePng({ width: black.width, height: black.height, data: rgba }))
  return { width: black.width, height: black.height, outputPath }
}

function clamp01(value) {
  return Math.max(0, Math.min(1, value))
}

function clampByte(value) {
  return Math.max(0, Math.min(255, Math.round(value)))
}

if (process.argv[1] && process.argv[1].endsWith('reconstruct-alpha.mjs')) {
  const [blackPath, whitePath, outputPath = 'transparent.png'] = process.argv.slice(2)
  if (!blackPath || !whitePath) {
    console.error('Usage: node reconstruct-alpha.mjs black.png white.png [transparent.png]')
    process.exitCode = 2
  } else {
    const result = reconstructPng(blackPath, whitePath, outputPath)
    console.log(`WROTE ${result.outputPath} (${result.width}x${result.height}, RGBA)`)
  }
}
