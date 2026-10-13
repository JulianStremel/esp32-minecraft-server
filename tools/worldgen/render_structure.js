#!/usr/bin/env node
// Renders the block names the host test structures_render_village writes (MC_RENDER=<dir>):
// the village from above, then five vertical cuts through it (y 50 to 113, top at the top),
// coloured by block, as PNG images next to the text files.
//   node tools/worldgen/render_structure.js <dir>
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
const COLORS = [
  [/^(air|cave_air|void_air)$/, [235, 240, 255]], [/water/, [50, 90, 220]], [/lava/, [230, 90, 20]],
  [/grass_block/, [90, 160, 60]], [/^(short_grass|tall_grass|fern|large_fern)$/, [120, 190, 90]], [/dirt_path/, [180, 150, 90]],
  [/dirt|farmland|coarse/, [130, 90, 55]], [/sand/, [220, 205, 140]], [/gravel/, [140, 135, 130]],
  [/log|wood/, [100, 70, 40]], [/planks|stairs|slab|fence|door|trapdoor/, [190, 150, 90]], [/cobble/, [110, 110, 110]],
  [/stone|andesite|diorite|granite|deepslate|tuff/, [150, 150, 150]], [/leaves/, [40, 120, 40]], [/glass|pane/, [200, 230, 240]],
  [/bed|wool|carpet/, [200, 60, 60]], [/hay/, [220, 190, 40]], [/flower|poppy|dandelion|tulip|daisy|cornflower|allium|bluet/, [230, 120, 200]],
  [/bell/, [255, 215, 0]], [/torch|lantern/, [255, 240, 120]],
];
const colorOf = (n) => { for (const [re, c] of COLORS) if (re.test(n)) return c; return [255, 0, 255]; };
const dir = process.argv[2];
for (const f of fs.readdirSync(dir).filter((f) => f.endsWith('.txt'))) {
  const lines = fs.readFileSync(path.join(dir, f), 'utf8').split('\n');
  let at = 0;
  const images = [];
  while (at < lines.length && lines[at]) {
    const [w, h] = lines[at++].split(' ').map(Number);
    const rgb = Buffer.alloc(w * h * 3);
    for (let i = 0; i < w * h; i++) {
      const c = colorOf(lines[at + i]);
      // the cuts: y from the bottom; drawn with the top up
      const row = images.length ? h - 1 - Math.floor(i / w) : Math.floor(i / w), col = i % w;
      rgb.set(c, (row * w + col) * 3);
    }
    at += w * h;
    images.push({ w, h, rgb });
  }
  const S = 4;   // each block 4 x 4 pixels
  images.forEach((im, k) => {
    const out = path.join(dir, f.replace('.txt', k ? '_cuts.png' : '_top.png'));
    const big = Buffer.alloc(im.w * S * im.h * S * 3);
    for (let y = 0; y < im.h * S; y++)
      for (let x = 0; x < im.w * S; x++) im.rgb.copy(big, (y * im.w * S + x) * 3, (Math.floor(y / S) * im.w + Math.floor(x / S)) * 3, (Math.floor(y / S) * im.w + Math.floor(x / S)) * 3 + 3);
    fs.writeFileSync(out, png(im.w * S, im.h * S, big));
    console.log(out);
  });
}
