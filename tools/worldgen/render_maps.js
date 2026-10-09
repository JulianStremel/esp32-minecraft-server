#!/usr/bin/env node
// Renders the height maps written by the host test vanilla_density_height_maps (MC_MAPS)
// as PNG images: water below sea level (63) in blue, land from green to brown to white.
//   node tools/worldgen/render_maps.js <dir> [scale]   (scale 2: every 2x2 columns averaged)
'use strict';
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

function png(w, h, rgb) {
  const crcTable = new Int32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; return c; });
  const crc = (b) => { let c = -1; for (const x of b) c = crcTable[(c ^ x) & 255] ^ (c >>> 8); return (c ^ -1) >>> 0; };
  const chunk = (type, data) => {
    const t = Buffer.concat([Buffer.from(type), data]);
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const c = Buffer.alloc(4); c.writeUInt32BE(crc(t));
    return Buffer.concat([len, t, c]);
  };
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) rgb.copy(raw, y * (w * 3 + 1) + 1, y * w * 3, (y + 1) * w * 3);
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 2;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}
function color(hgt, west, north) {
  let r, g, b;
  if (hgt < 63) { const d = Math.min(1, (63 - hgt) / 40); r = 30 - 20 * d; g = 90 - 50 * d; b = 200 - 80 * d; }
  else if (hgt < 66) { r = 220; g = 205; b = 140; }
  else if (hgt < 110) { const t = (hgt - 66) / 44; r = 70 + 60 * t; g = 150 - 30 * t; b = 60; }
  else if (hgt < 160) { const t = (hgt - 110) / 50; r = 130 - 10 * t; g = 115 - 10 * t; b = 95 + 15 * t; }
  else { const t = Math.min(1, (hgt - 160) / 60); r = 180 + 75 * t; g = 180 + 75 * t; b = 185 + 70 * t; }
  const shade = hgt >= 63 ? Math.max(-30, Math.min(30, (hgt - west + hgt - north) * 6)) : 0;   // hill shading
  return [r, g, b].map((v) => Math.max(0, Math.min(255, Math.round(v + shade))));
}
const dir = process.argv[2], scale = +(process.argv[3] || 1);
for (const name of ['current', 'vanilla']) {
  const buf = fs.readFileSync(path.join(dir, name + '.bin'));
  const n = Math.round(Math.sqrt(buf.length / 2));
  const at = (x, z) => buf.readInt16LE(2 * (Math.max(0, z) * n + Math.max(0, x)));
  const m = Math.floor(n / scale);
  const rgb = Buffer.alloc(m * m * 3);
  for (let z = 0; z < m; z++) for (let x = 0; x < m; x++) {
    const sum = [0, 0, 0];
    for (let dz = 0; dz < scale; dz++) for (let dx = 0; dx < scale; dx++) {
      const X = x * scale + dx, Z = z * scale + dz;
      const c = color(at(X, Z), at(X - scale, Z), at(X, Z - scale));
      for (let k = 0; k < 3; k++) sum[k] += c[k];
    }
    for (let k = 0; k < 3; k++) rgb[(z * m + x) * 3 + k] = Math.round(sum[k] / (scale * scale));
  }
  const file = path.join(dir, name + (scale > 1 ? '_' + m : '') + '.png');
  fs.writeFileSync(file, png(m, m, rgb));
  console.log(file, m + 'x' + m);
}
