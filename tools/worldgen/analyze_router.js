#!/usr/bin/env node
// Analyses vanilla's noise router (the data pack's density functions) to estimate what
// a vanilla-like generator costs: the node types, the noises and their octaves, and the
// samples needed per chunk. Reads tools/vanilla/1.21.8 (run fetch_vanilla.sh first).
//   node tools/worldgen/analyze_router.js [overworld|nether|end|large_biomes|amplified]
'use strict';
const fs = require('fs');
const path = require('path');

const W = path.join(__dirname, '..', 'vanilla', '1.21.8', 'jar', 'data', 'minecraft', 'worldgen');
const read = (sub, id) => JSON.parse(fs.readFileSync(path.join(W, sub, id.replace(/^minecraft:/, '') + '.json')));
const settings = read('noise_settings', process.argv[2] || 'overworld');

const noiseOctaves = (id) => {
  const n = read('noise', id);
  return n.amplitudes.filter((a) => a !== 0).length;
};

// walks a density function, counting nodes; shared (named) functions are counted once
// per reference, as vanilla evaluates them (caches aside)
function walk(df, stats, seen, depth = 0) {
  if (typeof df === 'number') { stats.types.constant = (stats.types.constant || 0) + 1; return; }
  if (typeof df === 'string') {   // a reference to a named function
    stats.refs[df] = (stats.refs[df] || 0) + 1;
    if (seen.has(df)) return;   // evaluated once per position thanks to caches
    seen.add(df);
    walk(read('density_function', df), stats, seen, depth + 1);
    return;
  }
  const type = df.type.replace('minecraft:', '');
  stats.types[type] = (stats.types[type] || 0) + 1;
  if (type === 'noise' || type === 'shifted_noise') {
    const oct = noiseOctaves(df.noise);
    // NormalNoise samples two PerlinNoises
    stats.noises.push({ noise: df.noise.replace('minecraft:', ''), octaves: oct, perlin: 2 * oct, type });
  }
  if (type === 'old_blended_noise') stats.noises.push({ noise: 'old_blended_noise (base_3d_noise)', octaves: 16 + 16 + 8, perlin: 40, type });
  if (type === 'weird_scaled_sampler' || type === 'shifted_noise') {
    if (type === 'weird_scaled_sampler') stats.noises.push({ noise: df.noise.replace('minecraft:', ''), octaves: noiseOctaves(df.noise), perlin: 2 * noiseOctaves(df.noise), type });
  }
  if (type === 'shift_a' || type === 'shift_b' || type === 'shift') {   // the argument is a noise
    stats.noises.push({ noise: df.argument.replace('minecraft:', ''), octaves: noiseOctaves(df.argument), perlin: 2 * noiseOctaves(df.argument), type });
    return;
  }
  if (type === 'spline') { stats.splines++; countSpline(df.spline, stats, seen, depth); }
  for (const k of ['argument', 'argument1', 'argument2', 'input', 'when_in_range', 'when_out_of_range', 'shift_x', 'shift_y', 'shift_z'])
    if (df[k] !== undefined && (typeof df[k] === 'object' || typeof df[k] === 'string')) walk(df[k], stats, seen, depth + 1);
}
function countSpline(sp, stats, seen, depth) {
  if (typeof sp === 'number') return;
  stats.splinePoints += sp.points.length;
  walk(sp.coordinate, stats, seen, depth + 1);
  for (const p of sp.points) countSpline(p.value, stats, seen, depth);
}

const router = settings.noise_router;
const fields = Object.keys(router);
const total = { perlin: 0 };
console.log(`noise router of ${process.argv[2] || 'overworld'}: height ${settings.noise.height} from ${settings.noise.min_y}, cell ${settings.noise.size_horizontal * 4}x${settings.noise.size_vertical * 4}`);
for (const f of fields) {
  const stats = { types: {}, noises: [], refs: {}, splines: 0, splinePoints: 0 };
  walk(router[f], stats, new Set());
  const perlin = stats.noises.reduce((a, n) => a + n.perlin, 0);
  total.perlin += perlin;
  if (!stats.noises.length && Object.keys(stats.types).length <= 1) continue;
  console.log(`\n${f}: ${stats.noises.length} noises, ${perlin} Perlin octave samples per evaluation` +
    (stats.splines ? `, ${stats.splines} splines with ${stats.splinePoints} points` : ''));
  console.log('  nodes: ' + Object.entries(stats.types).sort((a, b) => b[1] - a[1]).map(([t, n]) => `${t} ${n}`).join(', '));
  console.log('  noises: ' + stats.noises.map((n) => `${n.noise}(${n.octaves})`).join(' '));
}
// per chunk: final_density is evaluated at the cell corners and interpolated
const cellsH = 16 / (settings.noise.size_horizontal * 4) + 1, cellsV = settings.noise.height / (settings.noise.size_vertical * 4) + 1;
console.log(`\ncell corners per chunk: ${cellsH} x ${cellsV} x ${cellsH} = ${cellsH * cellsV * cellsH}`);
