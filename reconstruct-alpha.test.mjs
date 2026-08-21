import assert from 'node:assert/strict'
import { mkdtempSync, readFileSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import test from 'node:test'

import { decodePng, encodePng } from './png-rgba.mjs'
import { reconstructRgba } from './reconstruct-alpha.mjs'

test('reconstructs straight RGBA from black and white captures', () => {
  const black = Uint8Array.from([
    0, 0, 0, 255,
    100, 50, 25, 255,
    51, 26, 13, 255,
  ])
  const white = Uint8Array.from([
    255, 255, 255, 255,
    100, 50, 25, 255,
    178, 153, 140, 255,
  ])

  const result = reconstructRgba(black, white)

  assert.deepEqual([...result.slice(0, 4)], [0, 0, 0, 0])
  assert.deepEqual([...result.slice(4, 8)], [100, 50, 25, 255])
  assert.ok(Math.abs(result[8] - 102) <= 1)
  assert.ok(Math.abs(result[9] - 52) <= 1)
  assert.ok(Math.abs(result[10] - 26) <= 1)
  assert.ok(Math.abs(result[11] - 128) <= 1)
})

test('rejects captures with different dimensions', () => {
  const black = { width: 2, height: 1, data: new Uint8Array(8) }
  const white = { width: 1, height: 1, data: new Uint8Array(4) }

  assert.throws(
    () => reconstructRgba(black.data, white.data, black, white),
    /same dimensions/i,
  )
})

test('writes a PNG whose alpha channel survives decoding', () => {
  const directory = mkdtempSync(join(tmpdir(), 'ns-alpha-'))
  const output = join(directory, 'transparent.png')
  const rgba = Uint8Array.from([220, 80, 40, 64, 10, 20, 30, 255])

  const encoded = encodePng({ width: 2, height: 1, data: rgba })
  assert.equal(encoded.subarray(1, 4).toString('ascii'), 'PNG')
  writeFileSync(output, encoded)

  const decoded = decodePng(readFileSync(output))
  assert.equal(decoded.width, 2)
  assert.equal(decoded.height, 1)
  assert.deepEqual([...decoded.data], [...rgba])
  assert.ok(decoded.data.some((value, index) => index % 4 === 3 && value < 255))
})
