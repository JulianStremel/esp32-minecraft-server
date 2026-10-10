#!/usr/bin/env node
// Compiles vanilla's overworld biome parameter list (the data generator's report
// reports/biome_parameters/minecraft/overworld.json: OverworldBiomeBuilder's 7,593 points,
// which exist only as code in the game) into lib/mcore/src/mc/world/vanilla/biome_table.cpp.
//
// Vanilla picks the entry nearest to the climate (temperature, humidity, continentalness,
// erosion, depth, weirdness; squared distances outside each interval) with an R-tree.
// The surface entries (each twice: depth 0 and depth 1) tile a grid in the five other
// parameters exactly (checked here), so the surface biome is a table lookup after five
// interval searches; the three cave entries (lush caves, dripstone caves, deep dark) are
// compared by distance as vanilla does.
//   java -DbundlerMainClass=net.minecraft.data.Main -jar tools/vanilla/1.21.8/server.jar \
//        --reports --output tools/vanilla/1.21.8      (Java 21)
//   node tools/worldgen/compile_biomes.js
'use strict';
const fs = require('fs');
const path = require('path');

const REPORT = process.argv[2] ||
  path.join(__dirname, '..', 'vanilla', '1.21.8', 'reports', 'biome_parameters', 'minecraft', 'overworld.json');
const OUT = path.join(__dirname, '..', '..', 'lib', 'mcore', 'src', 'mc', 'world', 'vanilla', 'biome_table.cpp');
const entries = JSON.parse(fs.readFileSync(REPORT)).biomes;

// Climate.quantizeCoord: (long)(f * 10000.0F) in floats
const q = (v) => Math.trunc(Math.fround(Math.fround(v) * 10000));
const iv = (p) => (Array.isArray(p) ? p : [p, p]).map(q);
const camel = (id) => id.replace(/^minecraft:/, '').split('_').map((w) => w[0].toUpperCase() + w.slice(1)).join('');

const PARAMS = ['temperature', 'humidity', 'continentalness', 'erosion', 'weirdness'];
const isSurface = (e) => { const d = iv(e.parameters.depth); return (d[0] === 0 && d[1] === 0) || (d[0] === 10000 && d[1] === 10000); };
const surface = entries.filter((e) => iv(e.parameters.depth)[0] === 0 && iv(e.parameters.depth)[1] === 0);
const caves = entries.filter((e) => !isSurface(e));
for (const e of entries)
  if (e.parameters.offset !== 0) throw new Error('an entry with an offset: ' + e.biome);
if (entries.filter((e) => iv(e.parameters.depth)[0] === 10000).length !== surface.length)
  throw new Error('surface entries are not all at depths 0 and 1');

// the grid: per parameter the sorted bounds, a segment between two neighbours
const bounds = PARAMS.map((k) => [...new Set(surface.flatMap((e) => iv(e.parameters[k])))].sort((a, b) => a - b));
const dims = bounds.map((b) => b.length - 1);
const cells = dims.reduce((a, b) => a * b, 1);
const grid = new Array(cells).fill(null);
for (const e of surface) {
  const r = PARAMS.map((k, i) => {
    const [lo, hi] = iv(e.parameters[k]);
    return [bounds[i].indexOf(lo), bounds[i].indexOf(hi)];
  });
  const walk = (d, idx) => {
    if (d === PARAMS.length) {
      if (grid[idx] && grid[idx] !== e.biome) throw new Error('overlapping entries: ' + grid[idx] + ' ' + e.biome);
      grid[idx] = e.biome;
      return;
    }
    for (let s = r[d][0]; s < r[d][1]; s++) walk(d + 1, idx * dims[d] + s);
  };
  walk(0, 0);
}
if (grid.some((b) => !b)) throw new Error('the surface entries leave gaps');

// check: the lookup below (as biomes.cpp does it) against vanilla's nearest entry over all
// of them, at random climates (some outside every interval)
{
  const out = (v, lo, hi) => { const d = v < lo ? lo - v : v > hi ? v - hi : 0; return d * d; };
  const ORDER6 = ['temperature', 'humidity', 'continentalness', 'erosion', 'depth', 'weirdness'];
  const all = entries.map((e) => ({ biome: e.biome, r: ORDER6.map((k) => iv(e.parameters[k])) }));
  let seed = 12345;
  const rnd = () => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed / 2147483648; };
  let wrong = 0, ties = 0;
  const N = 20000;
  for (let n = 0; n < N; n++) {
    const p = [0, 1, 2, 3, 4, 5].map(() => Math.trunc((rnd() * 2.6 - 1.3) * 10000));
    let best = Infinity, bestB = null, tie = false;
    for (const e of all) {
      let d = 0;
      for (let i = 0; i < 6; i++) d += out(p[i], e.r[i][0], e.r[i][1]);
      if (d < best) { best = d; bestB = e.biome; tie = false; } else if (d === best && e.biome !== bestB) tie = true;
    }
    const five = [p[0], p[1], p[2], p[3], p[5]];
    let idx = 0, dist = 0;
    five.forEach((v, i) => {
      const b = bounds[i];
      dist += out(v, b[0], b[b.length - 1]);
      let seg = 0;
      while (seg < b.length - 2 && v > b[seg + 1]) seg++;
      idx = idx * dims[i] + seg;
    });
    let mine = grid[idx], mineD = dist + Math.min(out(p[4], 0, 0), out(p[4], 10000, 10000));
    for (const e of caves) {
      const r = ORDER6.map((k) => iv(e.parameters[k]));
      let d = 0;
      for (let i = 0; i < 6; i++) d += out(p[i], r[i][0], r[i][1]);
      if (d < mineD) { mineD = d; mine = e.biome; }
    }
    if (tie) ties++;
    else if (mine !== bestB) wrong++;
  }
  if (wrong) throw new Error(`the lookup differs from the nearest entry at ${wrong} of ${N} climates`);
  console.log(`checked against the nearest entry at ${N} random climates: the same (${ties} ties left out)`);
}

let s = '// GENERATED by tools/worldgen/compile_biomes.js from vanilla 1.21.8\'s overworld biome\n' +
  '// parameter list (the data generator\'s report). DO NOT EDIT.\n' +
  '#include "mc/world/vanilla/biomes.h"\n#include "mc/registry.h"\nnamespace mc {\nnamespace vanilla {\n';
PARAMS.forEach((k, i) => {
  s += `static const int32_t B_${k.toUpperCase()}[${bounds[i].length}] = {${bounds[i].join(', ')}};\n`;
});
s += `const BiomeAxis BIOME_AXES[5] = {${PARAMS.map((k, i) => `{B_${k.toUpperCase()}, ${bounds[i].length}}`).join(', ')}};\n`;
s += `const uint8_t BIOME_GRID[${cells}] = {\n`;
for (let i = 0; i < cells; i += 16)
  s += '  ' + grid.slice(i, i + 16).map((b) => `biome::${camel(b)}`).join(', ') + ',\n';
s += '};\n';
const ORDER = ['temperature', 'humidity', 'continentalness', 'erosion', 'depth', 'weirdness'];
s += `const CaveBiome CAVE_BIOMES[${caves.length}] = {\n` + caves.map((e) => {
  const r = ORDER.map((k) => iv(e.parameters[k]));
  return `  {{${r.map((x) => x[0]).join(', ')}}, {${r.map((x) => x[1]).join(', ')}}, biome::${camel(e.biome)}},`;
}).join('\n') + '\n};\n';
s += `const int NUM_CAVE_BIOMES = ${caves.length};\n`;
s += '}  // namespace vanilla\n}  // namespace mc\n';
fs.writeFileSync(OUT, s);
console.log(`${surface.length} surface entries in a ${dims.join(' x ')} grid (${cells} cells), ${caves.length} cave biomes`);
