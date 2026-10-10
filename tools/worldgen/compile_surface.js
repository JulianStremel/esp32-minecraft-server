#!/usr/bin/env node
// Compiles vanilla's overworld surface rule (noise_settings/overworld.json, 30 KB of JSON:
// sequences of conditions on biome, depth into the stone, water, y, noises, steepness)
// into the tables of lib/mcore/src/mc/world/vanilla/surface_rules.cpp, evaluated by
// surface.cpp. Only the part under above_preliminary_surface is compiled: the bedrock
// floor and the deepslate gradient around it are the generator's own, and everything
// else applies only from a column's top down to its preliminary surface.
//   node tools/worldgen/compile_surface.js
'use strict';
const fs = require('fs');
const path = require('path');

const ROOT = path.join(__dirname, '..', '..');
const W = path.join(__dirname, '..', 'vanilla', '1.21.8', 'jar', 'data', 'minecraft', 'worldgen');
const OUT = path.join(ROOT, 'lib', 'mcore', 'src', 'mc', 'world', 'vanilla', 'surface_rules.cpp');
const settings = JSON.parse(fs.readFileSync(path.join(W, 'noise_settings', 'overworld.json')));
const sh = (id) => id.replace(/^minecraft:/, '');
const camel = (id) => sh(id).split('_').map((w) => w[0].toUpperCase() + w.slice(1)).join('');

// ids: biomes from biome_ids_gen.h, noises from router_data.cpp (compile_router.js puts the
// surface rule's noises there)
const biomeIds = {};
const idsH = fs.readFileSync(path.join(ROOT, 'lib/mcore/src/mc/data/biome_ids_gen.h'), 'utf8');
const biomeBlock = idsH.slice(idsH.indexOf('namespace biome {'), idsH.indexOf('}', idsH.indexOf('namespace biome {')));
for (const m of biomeBlock.matchAll(/constexpr uint8_t (\w+) = (\d+);/g)) biomeIds[m[1]] = +m[2];
const routerData = fs.readFileSync(path.join(ROOT, 'lib/mcore/src/mc/world/vanilla/router_data.cpp'), 'utf8');
const noiseSection = routerData.slice(routerData.indexOf('DF_NOISES['), routerData.indexOf('NUM_DF_NOISES'));
const noiseIds = {};
[...noiseSection.matchAll(/\{-?\d+, \d+, \d+\}, \/\/ (\S+)/g)].forEach((m, i) => { noiseIds[m[1]] = i; });
const noiseOf = (id) => {
  if (!(sh(id) in noiseIds)) throw new Error('noise not in the router (run compile_router.js): ' + id);
  return noiseIds[sh(id)];
};

// the rule's shape: [bedrock floor, above_preliminary_surface -> X, deepslate]
const top = settings.surface_rule;
const seq = top.sequence || [];
if (sh(top.type) !== 'sequence' || seq.length !== 3 || sh(seq[0].if_true.type) !== 'vertical_gradient' ||
    sh(seq[1].if_true.type) !== 'above_preliminary_surface' || sh(seq[2].if_true.type) !== 'vertical_gradient')
  throw new Error('the surface rule does not have the expected shape');

const states = [];   // block state constants (bs::)
const DEFAULTS = { grass_block: { snowy: 'false' }, podzol: { snowy: 'false' }, mycelium: { snowy: 'false' },
  water: { level: '0' }, deepslate: { axis: 'y' } };
const stateOf = (name, props) => {
  const n = sh(name);
  for (const k in props || {})
    if (!DEFAULTS[n] || DEFAULTS[n][k] !== props[k]) throw new Error(`not a default state: ${n} ${JSON.stringify(props)}`);
  const expr = n === 'air' ? '0' : `bs::${camel(n)}`;
  let i = states.indexOf(expr);
  if (i < 0) { i = states.length; states.push(expr); }
  return i;
};
const BANDS = ['terracotta', 'orange_terracotta', 'yellow_terracotta', 'brown_terracotta', 'red_terracotta',
  'white_terracotta', 'light_gray_terracotta'];
const bandStates = BANDS.map((b) => stateOf(b));

const conds = [], nodes = [], children = [];
let maxCeiling = 0;
function cond(c) {
  const t = sh(c.type);
  const e = { type: 'SC_' + t.toUpperCase(), flags: 0, a: 0, b: 0, lo: 0, hi: 0, mask: [0n, 0n], noise: -1, inner: -1 };
  switch (t) {
    case 'biome':
      for (const b of c.biome_is) {
        const id = biomeIds[camel(b)];
        if (id === undefined) throw new Error('biome ' + b);
        e.mask[id >> 6] |= 1n << BigInt(id & 63);
      }
      break;
    case 'noise_threshold':
      e.noise = noiseOf(c.noise);
      e.lo = c.min_threshold;
      e.hi = Math.min(c.max_threshold, 3.4e38);
      break;
    case 'stone_depth':
      e.a = c.offset;
      e.b = c.secondary_depth_range;
      e.flags = (c.surface_type === 'ceiling' ? 1 : 0) | (c.add_surface_depth ? 2 : 0);
      if (c.surface_type === 'ceiling') maxCeiling = Math.max(maxCeiling, 1 + c.offset + (c.add_surface_depth ? 16 : 0) + c.secondary_depth_range);
      break;
    case 'y_above': {
      const an = c.anchor;
      e.a = 'absolute' in an ? an.absolute : 'above_bottom' in an ? -64 + an.above_bottom : 320 - an.below_top;
      e.b = c.surface_depth_multiplier;
      e.flags = c.add_stone_depth ? 1 : 0;
      break;
    }
    case 'water':
      e.a = c.offset;
      e.b = c.surface_depth_multiplier;
      e.flags = c.add_stone_depth ? 1 : 0;
      break;
    case 'not': e.inner = cond(c.invert); break;
    case 'hole': case 'steep': case 'temperature': case 'above_preliminary_surface': break;
    default: throw new Error('condition ' + t);
  }
  conds.push(e);
  return conds.length - 1;
}
function rule(r) {
  const t = sh(r.type);
  const i = nodes.length;
  nodes.push(null);
  switch (t) {
    case 'sequence': {
      const kids = r.sequence.map(rule);
      nodes[i] = { op: 'SR_SEQ', a: children.length, b: kids.length };
      children.push(...kids);
      break;
    }
    case 'condition': {
      const c = cond(r.if_true);
      nodes[i] = { op: 'SR_COND', a: c, b: rule(r.then_run) };
      break;
    }
    case 'block': nodes[i] = { op: 'SR_BLOCK', a: stateOf(r.result_state.Name, r.result_state.Properties), b: 0 }; break;
    case 'bandlands': nodes[i] = { op: 'SR_BANDS', a: 0, b: 0 }; break;
    default: throw new Error('rule ' + t);
  }
  return i;
}
const root = rule(seq[1].then_run);

// Per biome: the rule with the biome conditions decided (a block's biome is known before
// the rule runs), identical subtrees shared; and how deep into the stone a rule can still
// apply for each surface depth (SR_DEPTH_BOUND: deeper blocks are left as they are).
const NB = Object.keys(biomeIds).length;
const SD_MIN = -8, SD_MAX = 16;   // surface depths the bound table covers (beyond: unbounded)
const shared = new Map();
const mk = (n) => {
  const key = JSON.stringify(n);
  if (shared.has(key)) return shared.get(key);
  nodes.push(n);
  shared.set(key, nodes.length - 1);
  return nodes.length - 1;
};
const inBiome = (ci, b) => (conds[ci].mask[b >> 6] >> BigInt(b & 63)) & 1n;
function prune(ni, b) {
  const n = nodes[ni];
  if (n.op === 'SR_SEQ') {
    const kids = [];
    for (let i = 0; i < n.b; i++) {
      const k = prune(children[n.a + i], b);
      if (k >= 0) kids.push(k);
    }
    if (!kids.length) return -1;
    if (kids.length === 1) return kids[0];
    const key = 'seq:' + kids.join(',');
    if (shared.has(key)) return shared.get(key);
    nodes.push({ op: 'SR_SEQ', a: children.length, b: kids.length });
    children.push(...kids);
    shared.set(key, nodes.length - 1);
    return nodes.length - 1;
  }
  if (n.op === 'SR_COND') {
    const c = conds[n.a];
    if (c.type === 'SC_BIOME') return inBiome(n.a, b) ? prune(n.b, b) : -1;
    if (c.type === 'SC_NOT' && conds[c.inner].type === 'SC_BIOME') return inBiome(c.inner, b) ? -1 : prune(n.b, b);
    const k = prune(n.b, b);
    return k < 0 ? -1 : mk({ op: 'SR_COND', a: n.a, b: k });
  }
  return ni;   // block, bandlands
}
// the deepest stoneAbove at which any leaf under ni can apply, for surface depth sd
function bound(ni, sd) {
  const n = nodes[ni];
  if (n.op === 'SR_SEQ') {
    let m = -Infinity;
    for (let i = 0; i < n.b; i++) m = Math.max(m, bound(children[n.a + i], sd));
    return m;
  }
  if (n.op === 'SR_COND') {
    const c = conds[n.a], inner = bound(n.b, sd);
    if (c.type === 'SC_STONE_DEPTH' && !(c.flags & 1))
      return Math.min(inner, 1 + c.a + ((c.flags & 2) ? sd : 0) + c.b);
    return inner;
  }
  return Infinity;
}
const biomeRoot = [], depthBound = [];
for (let b = 0; b < NB; b++) {
  const r = prune(root, b);
  biomeRoot.push(r);
  for (let sd = SD_MIN; sd <= SD_MAX; sd++) {
    const m = r < 0 ? -1 : bound(r, sd);
    depthBound.push(m === Infinity || m > 254 ? 255 : Math.max(0, m));
  }
}

// biome temperatures (Biome#coldEnoughToSnow)
const temps = new Array(Object.keys(biomeIds).length).fill(0.5);
for (const f of fs.readdirSync(path.join(W, 'biome'))) {
  const id = biomeIds[camel(f.replace('.json', ''))];
  if (id !== undefined && id < temps.length) temps[id] = JSON.parse(fs.readFileSync(path.join(W, 'biome', f))).temperature;
}

const fl = (x) => { let s = String(Math.fround(x)); if (!/[.e]/.test(s)) s += '.0'; return s + 'f'; };
const u64 = (v) => '0x' + v.toString(16) + 'ull';
let s = '// GENERATED by tools/worldgen/compile_surface.js from vanilla 1.21.8\'s overworld surface rule.\n' +
  '// DO NOT EDIT.\n#include "mc/world/vanilla/surface.h"\n#include "mc/registry.h"\nnamespace mc {\nnamespace vanilla {\n';
s += `const SrNode SR_NODES[${nodes.length}] = {\n` + nodes.map((n) => `  {${n.op}, ${n.a}, ${n.b}},`).join('\n') + '\n};\n';
s += `const int16_t SR_CHILDREN[${children.length}] = {${children.join(', ')}};\n`;
s += `const SrCond SR_CONDS[${conds.length}] = {\n` + conds.map((c) =>
  `  {${c.type}, ${c.flags}, ${c.a}, ${c.b}, ${fl(c.lo)}, ${fl(c.hi)}, {${u64(c.mask[0])}, ${u64(c.mask[1])}}, ${c.noise}, ${c.inner}},`).join('\n') + '\n};\n';
s += `const uint16_t SR_STATES[${states.length}] = {${states.join(', ')}};\n`;
s += `const int SR_NUM_STATES = ${states.length};\n`;
s += `const uint8_t SR_BAND_STATES[${bandStates.length}] = {${bandStates.join(', ')}};   // ${BANDS.join(', ')}\n`;
s += `const int SR_ROOT = ${root};\n`;
s += `const int16_t SR_BIOME_ROOT[${NB}] = {${biomeRoot.join(', ')}};\n`;
s += `const int SR_DEPTH_SD_MIN = ${SD_MIN}, SR_DEPTH_SD_MAX = ${SD_MAX};\n`;
s += `const uint8_t SR_DEPTH_BOUND[${depthBound.length}] = {${depthBound.join(', ')}};\n`;
s += `const int SR_MAX_CEILING_DEPTH = ${maxCeiling};\n`;
s += `const int SR_NOISE_SURFACE = ${noiseOf('surface')}, SR_NOISE_SECONDARY = ${noiseOf('surface_secondary')}, ` +
  `SR_NOISE_CLAY_BANDS = ${noiseOf('clay_bands_offset')};\n`;
s += `const float BIOME_TEMPERATURE[${temps.length}] = {${temps.map(fl).join(', ')}};\n`;
s += '}  // namespace vanilla\n}  // namespace mc\n';
fs.writeFileSync(OUT, s);
console.log(`per biome: ${biomeRoot.filter((r) => r >= 0).length} rules; ${nodes.length} rule nodes, ${conds.length} conditions, ${states.length} block states, ceiling depth ${maxCeiling}`);
