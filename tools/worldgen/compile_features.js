#!/usr/bin/env node
// Compiles the overworld biomes' features (biome/*.json "features": eleven decoration steps
// of placed features; placed_feature/*.json: placement modifiers; configured_feature/*.json:
// the feature and its configuration) into the tables of
// lib/mcore/src/mc/world/vanilla/feature_data.cpp, run by features.cpp. Block states and
// tags are written as names (tags expanded from the data pack) and resolved when a
// generator starts. Feature types features.cpp does not have are kept as FC_SKIP.
//   node tools/worldgen/compile_features.js
'use strict';
const fs = require('fs');
const path = require('path');

const ROOT = path.join(__dirname, '..', '..');
const D = path.join(__dirname, '..', 'vanilla', '1.21.8', 'jar', 'data', 'minecraft');
const W = path.join(D, 'worldgen');
const OUT = path.join(ROOT, 'lib', 'mcore', 'src', 'mc', 'world', 'vanilla', 'feature_data.cpp');
const sh = (id) => id.replace(/^minecraft:/, '');
const read = (sub, id) => JSON.parse(fs.readFileSync(path.join(W, sub, sh(id) + '.json')));
const camel = (id) => sh(id).split('_').map((w) => w[0].toUpperCase() + w.slice(1)).join('');

const idsH = fs.readFileSync(path.join(ROOT, 'lib/mcore/src/mc/data/biome_ids_gen.h'), 'utf8');
const biomeBlock = idsH.slice(idsH.indexOf('namespace biome {'), idsH.indexOf('}', idsH.indexOf('namespace biome {')));
const biomeIds = {};
for (const m of biomeBlock.matchAll(/constexpr uint8_t (\w+) = (\d+);/g)) biomeIds[m[1]] = +m[2];
const NB = Object.keys(biomeIds).length;

// ------------------------------------------------------------ pools
const states = [], stateKey = new Map();          // "name[k=v,...]"
const stateIdx = (st) => {
  const props = st.Properties ? Object.entries(st.Properties).sort().map(([k, v]) => `${k}=${v}`).join(',') : '';
  const key = sh(st.Name) + (props ? `[${props}]` : '');
  if (!stateKey.has(key)) { stateKey.set(key, states.length); states.push(key); }
  return stateKey.get(key);
};
const tagBlocks = (tag) => {
  const t = JSON.parse(fs.readFileSync(path.join(D, 'tags', 'block', sh(tag) + '.json')));
  const out = [];
  for (const v of t.values) {
    const id = typeof v === 'string' ? v : v.id;
    if (id.startsWith('#')) out.push(...tagBlocks(id.slice(1)));
    else out.push(sh(id));
  }
  return out;
};
const sets = [], setNames = [], setKey = new Map();   // block sets: names
const blockSet = (blocks) => {
  let list = [];
  for (const b of [].concat(blocks)) {
    if (b.startsWith('#')) list.push(...tagBlocks(b.slice(1)));
    else list.push(sh(b));
  }
  list = [...new Set(list)].sort();
  const key = list.join(',');
  if (!setKey.has(key)) { setKey.set(key, sets.length); sets.push({ first: setNames.length, count: list.length }); setNames.push(...list); }
  return setKey.get(key);
};
const lists = [];   // int16 pool for lists of indices / pairs
const list = (arr) => { const i = lists.length; lists.push(...arr); return i; };

// int, float and height providers
const provs = [];
const prov = (p, kind) => {
  const e = { type: 'FP_CONSTANT', a: 0, b: 0, c: 0, fa: 0, fb: 0 };
  const anchor = (an) => ('absolute' in an ? an.absolute : 'above_bottom' in an ? -64 + an.above_bottom : 320 - an.below_top);
  if (typeof p === 'number') {
    e.a = Math.round(p);
    e.fa = p;
  } else if (p.absolute !== undefined || p.above_bottom !== undefined || p.below_top !== undefined) {
    e.a = anchor(p);   // a height anchor as a constant
    e.fa = e.a;
  } else {
    const t = sh(p.type);
    switch (t) {
      case 'constant':
        e.a = typeof p.value === 'number' ? Math.round(p.value) : anchor(p.value);
        e.fa = typeof p.value === 'number' ? p.value : e.a;
        break;
      case 'uniform':
        e.type = 'FP_UNIFORM';
        if (kind === 'float') { e.fa = p.min_inclusive; e.fb = p.max_exclusive; }
        else if (kind === 'height') { e.a = anchor(p.min_inclusive); e.b = anchor(p.max_inclusive); }
        else { e.a = p.min_inclusive; e.b = p.max_inclusive; }
        break;
      case 'biased_to_bottom':
        e.type = 'FP_BIASED';
        if (kind === 'height') { e.a = anchor(p.min_inclusive); e.b = anchor(p.max_inclusive); e.c = p.inner || 1; }
        else { e.a = p.min_inclusive; e.b = p.max_inclusive; e.c = 1; }
        break;
      case 'very_biased_to_bottom':
        e.type = 'FP_VERY_BIASED'; e.a = anchor(p.min_inclusive); e.b = anchor(p.max_inclusive); e.c = p.inner || 1;
        break;
      case 'trapezoid':
        e.type = 'FP_TRAPEZOID';
        if (kind === 'float') { e.fa = p.min; e.fb = p.max; e.c = 0; e.a = 0; e.b = 0; e.fc = p.plateau; }
        else { e.a = anchor(p.min_inclusive); e.b = anchor(p.max_inclusive); e.c = p.plateau || 0; }
        break;
      case 'weighted_list': {
        e.type = 'FP_WEIGHTED';
        const pairs = [];
        for (const d of p.distribution) pairs.push(prov(d.data, kind), d.weight);
        e.a = list(pairs); e.b = p.distribution.length;
        break;
      }
      case 'clamped':
        e.type = 'FP_CLAMPED'; e.a = prov(p.source, kind); e.b = p.min_inclusive; e.c = p.max_inclusive;
        break;
      case 'clamped_normal':
        e.type = 'FP_UNIFORM'; e.a = p.min_inclusive; e.b = p.max_inclusive;
        break;
      default: throw new Error('provider ' + t);
    }
  }
  provs.push(e);
  return provs.length - 1;
};

// block state providers
const sprovs = [];
const sprov = (p) => {
  const t = p.type ? sh(p.type) : 'rule_based_state_provider';   // a disk's has no type
  const e = { type: 'FS_SIMPLE', a: 0, b: 0, c: 0, f: 0 };
  switch (t) {
    case 'simple_state_provider': e.a = stateIdx(p.state); break;
    case 'rotated_block_provider': e.a = stateIdx(p.state); break;
    case 'weighted_state_provider': {
      e.type = 'FS_WEIGHTED';
      const pairs = [];
      for (const d of p.entries) pairs.push(stateIdx(d.data), d.weight);
      e.a = list(pairs); e.b = p.entries.length;
      break;
    }
    case 'noise_threshold_provider': {   // the default below the threshold, else low or high states
      e.type = 'FS_NOISE_THRESHOLD';
      e.a = stateIdx(p.default_state);
      e.b = list(p.low_states.map(stateIdx).concat(p.high_states.map(stateIdx)));
      e.c = p.low_states.length * 256 + p.high_states.length;
      e.f = p.threshold; e.g = p.high_chance; e.h = p.scale;
      break;
    }
    case 'noise_provider': case 'dual_noise_provider': {
      e.type = 'FS_NOISE';
      e.a = list(p.states.map(stateIdx)); e.b = p.states.length; e.h = p.scale;
      break;
    }
    case 'randomized_int_state_provider': e.a = sprovs.length; return sprov(p.source);
    case 'rule_based_state_provider': {   // the first rule's state (or the fallback): disks
      e.type = 'FS_RULES';
      e.a = p.fallback ? sprov(p.fallback) : -1;
      const pairs = [];
      for (const r of p.rules || []) pairs.push(pred(r.if_true), sprov(r.then));
      e.b = list(pairs); e.c = (p.rules || []).length;
      break;
    }
    default: throw new Error('state provider ' + t);
  }
  sprovs.push(e);
  return sprovs.length - 1;
};

// block predicates
const preds = [];
const pred = (p) => {
  const t = sh(p.type || p.predicate_type);
  const e = { type: 'FB_TRUE', dx: 0, dy: 0, dz: 0, a: 0, b: 0 };
  if (p.offset) [e.dx, e.dy, e.dz] = p.offset;
  switch (t) {
    case 'true': break;
    case 'matching_blocks': e.type = 'FB_BLOCKS'; e.a = blockSet(p.blocks); break;
    case 'matching_block_tag': e.type = 'FB_BLOCKS'; e.a = blockSet('#' + sh(p.tag)); break;
    case 'tag_match': e.type = 'FB_BLOCKS'; e.a = blockSet('#' + sh(p.tag)); break;
    case 'block_match': e.type = 'FB_BLOCKS'; e.a = blockSet(p.block); break;
    case 'matching_fluids': e.type = 'FB_FLUIDS'; e.a = [].concat(p.fluids).some((f) => f.includes('lava')) ? 2 : 1; break;
    case 'replaceable': e.type = 'FB_REPLACEABLE'; break;
    case 'solid': e.type = 'FB_SOLID'; break;
    case 'would_survive': e.type = 'FB_SURVIVE'; e.a = stateIdx(p.state); break;
    case 'inside_world_bounds': break;
    case 'has_sturdy_face': e.type = 'FB_SOLID'; break;
    case 'not': e.type = 'FB_NOT'; e.a = pred(p.predicate); break;
    case 'all_of': e.type = 'FB_ALL'; e.a = list(p.predicates.map(pred)); e.b = p.predicates.length; break;
    case 'any_of': e.type = 'FB_ANY'; e.a = list(p.predicates.map(pred)); e.b = p.predicates.length; break;
    default: e.type = 'FB_TRUE';   // unknown: allowed
  }
  preds.push(e);
  return preds.length - 1;
};

// placement modifiers and placed features
const mods = [], placed = [], placedKey = new Map();
const HEIGHTMAPS = { WORLD_SURFACE_WG: 0, WORLD_SURFACE: 0, OCEAN_FLOOR_WG: 1, OCEAN_FLOOR: 1, MOTION_BLOCKING: 2, MOTION_BLOCKING_NO_LEAVES: 3 };
const mod = (m) => {
  const t = sh(m.type);
  const e = { type: 'FM_' + t.toUpperCase(), a: 0, b: 0, c: 0, f: 0 };
  switch (t) {
    case 'count': e.a = prov(m.count, 'int'); break;
    case 'rarity_filter': e.a = m.chance; break;
    case 'in_square': case 'biome': break;
    case 'height_range': e.a = prov(m.height, 'height'); break;
    case 'heightmap': e.a = HEIGHTMAPS[m.heightmap]; break;
    case 'block_predicate_filter': e.a = pred(m.predicate); break;
    case 'surface_water_depth_filter': e.a = m.max_water_depth; break;
    case 'environment_scan':
      e.a = pred(m.target_condition); e.b = m.allowed_search_condition ? pred(m.allowed_search_condition) : -1;
      e.c = (m.direction_of_search === 'up' ? 1 : -1) * m.max_steps;
      break;
    case 'surface_relative_threshold_filter':
      e.a = HEIGHTMAPS[m.heightmap]; e.b = m.min_inclusive ?? -100000; e.c = m.max_inclusive ?? 100000;
      break;
    case 'noise_based_count': e.a = m.noise_to_count_ratio; e.f = m.noise_factor; e.b = Math.round((m.noise_offset || 0) * 1000); break;
    case 'noise_threshold_count': e.a = m.below_noise; e.b = m.above_noise; e.f = m.noise_level; break;
    case 'random_offset': e.a = prov(m.xz_spread, 'int'); e.b = prov(m.y_spread, 'int'); break;
    default: throw new Error('modifier ' + t);
  }
  mods.push(e);
  return mods.length - 1;
};
const cfgs = [], cfgKey = new Map(), skipped = {};
// the generator's tree kinds (Generator::TreeKind): oak 0, birch 1, spruce 2, jungle 3, acacia 4, dark oak 5
const LOG_KIND = { oak_log: 0, birch_log: 1, spruce_log: 2, jungle_log: 3,
  acacia_log: 4, dark_oak_log: 5, mangrove_log: 0, cherry_log: 1, pale_oak_log: 5 };
function placedOf(p) {
  if (typeof p === 'string') {
    if (placedKey.has(p)) return placedKey.get(p);
    const i = placed.length;
    placedKey.set(p, i);
    placed.push(null);
    const pf = read('placed_feature', p);
    placed[i] = { cfg: cfgOf(pf.feature), first: -1, count: pf.placement.length, name: sh(p) };
    const ms = pf.placement.map(mod);
    placed[i].first = ms.length ? ms[0] : 0;
    return i;
  }
  const i = placed.length;
  placed.push(null);
  const c = cfgOf(p.feature);
  const ms = (p.placement || []).map(mod);
  placed[i] = { cfg: c, first: ms.length ? ms[0] : 0, count: ms.length, name: '' };
  return i;
}
function cfgOf(cf) {
  const key = typeof cf === 'string' ? cf : null;
  if (key && cfgKey.has(key)) return cfgKey.get(key);
  const c = typeof cf === 'string' ? read('configured_feature', cf) : cf;
  const t = sh(c.type), k = c.config || {};
  const i = cfgs.length;
  if (key) cfgKey.set(key, i);
  cfgs.push(null);
  const e = { type: 'FC_' + t.toUpperCase(), a: 0, b: 0, c: 0, d: 0, e: 0, f: 0, name: key ? sh(key) : '' };
  switch (t) {
    case 'random_patch': case 'flower':
      e.type = 'FC_RANDOM_PATCH'; e.a = k.tries ?? 128; e.b = k.xz_spread ?? 7; e.c = k.y_spread ?? 3; e.d = placedOf(k.feature);
      break;
    case 'simple_block': e.a = sprov(k.to_place); break;
    case 'ore': case 'scattered_ore': {
      e.type = 'FC_ORE'; e.a = k.size; e.f = k.discard_chance_on_air_exposure;
      const pairs = [];
      for (const tg of k.targets) pairs.push(pred(tg.target), stateIdx(tg.state));
      e.b = list(pairs); e.c = k.targets.length;
      break;
    }
    case 'tree': {
      const log = sh(k.trunk_provider.state ? k.trunk_provider.state.Name : 'oak_log');
      const placer = sh(k.trunk_placer.type);
      e.a = LOG_KIND[log] ?? 0;
      if (placer === 'fancy_trunk_placer') e.b = 1;           // tall oaks
      if (placer === 'giant_trunk_placer' || placer === 'mega_jungle_trunk_placer') e.b = 2;
      e.c = Math.round(k.trunk_placer.base_height + (k.trunk_placer.height_rand_a || 0) / 2 + (k.trunk_placer.height_rand_b || 0) / 2);
      break;
    }
    case 'random_selector': {
      const pairs = [];
      for (const f of k.features) pairs.push(placedOf(f.feature), Math.round(f.chance * 1e6));
      e.a = list(pairs); e.b = k.features.length; e.c = placedOf(k.default);
      break;
    }
    case 'simple_random_selector': e.a = list(k.features.map(placedOf)); e.b = k.features.length; break;
    case 'random_boolean_selector': e.a = placedOf(k.feature_true); e.b = placedOf(k.feature_false); break;
    case 'seagrass': e.f = k.probability; break;
    case 'kelp': case 'freeze_top_layer': break;
    case 'disk':
      e.a = sprov(k.state_provider); e.b = pred(k.target); e.c = prov(k.radius, 'int'); e.d = k.half_height;
      break;
    case 'spring_feature':
      e.a = stateIdx(k.state); e.b = k.requires_block_below ? 1 : 0; e.c = k.rock_count; e.d = k.hole_count;
      e.e = blockSet(k.valid_blocks);
      break;
    case 'block_column': {
      const pairs = [];
      for (const l of k.layers) pairs.push(prov(l.height, 'int'), sprov(l.provider));
      e.a = list(pairs); e.b = k.layers.length; e.c = pred(k.allowed_placement); e.d = k.direction === 'down' ? -1 : 1;
      break;
    }
    default:
      e.type = 'FC_SKIP';
      skipped[t] = (skipped[t] || 0) + 1;
  }
  cfgs[i] = e;
  return i;
}

// the biomes' lists, in step order
const biomeLists = new Array(NB).fill(null).map(() => []);
for (const f of fs.readdirSync(path.join(W, 'biome'))) {
  const id = biomeIds[camel(f.replace('.json', ''))];
  const b = JSON.parse(fs.readFileSync(path.join(W, 'biome', f)));
  if (id === undefined || !JSON.stringify(b.carvers || '').includes('minecraft:cave')) continue;   // the overworld's
  b.features.forEach((step, si) => step.forEach((p) => biomeLists[id].push([si, placedOf(p)])));
}
const biomeFirst = [], biomeCount = [], biomePairs = [];
for (const l of biomeLists) { biomeFirst.push(biomePairs.length / 2); biomeCount.push(l.length); for (const [s, p] of l) biomePairs.push(s, p); }

// ------------------------------------------------------------ output
const fl = (x) => { let s = String(Math.fround(x || 0)); if (!/[.e]/.test(s)) s += '.0'; return s + 'f'; };
let s = '// GENERATED by tools/worldgen/compile_features.js from vanilla 1.21.8\'s overworld biomes and their\n' +
  '// placed and configured features. DO NOT EDIT.\n#include "mc/world/vanilla/features.h"\n#include "mc/world/generator.h"\nnamespace mc {\nnamespace vanilla {\n';
s += `const char* const FD_STATES[${states.length}] = {${states.map((x) => JSON.stringify(x)).join(', ')}};\n`;
s += `const int FD_NUM_STATES = ${states.length};\n`;
s += `const char* const FD_SET_NAMES[${setNames.length}] = {${setNames.map((x) => JSON.stringify(x)).join(', ')}};\n`;
s += `const FdSet FD_SETS[${sets.length}] = {${sets.map((x) => `{${x.first}, ${x.count}}`).join(', ')}};\n`;
s += `const int FD_NUM_SETS = ${sets.length};\n`;
s += `const int32_t FD_LISTS[${lists.length || 1}] = {${lists.join(', ') || 0}};\n`;
s += `const FdProv FD_PROVS[${provs.length}] = {\n` + provs.map((p) => `  {${p.type}, ${p.a}, ${p.b}, ${p.c}, ${fl(p.fa)}, ${fl(p.fb)}, ${fl(p.fc)}},`).join('\n') + '\n};\n';
s += `const FdStateProv FD_SPROVS[${sprovs.length}] = {\n` + sprovs.map((p) => `  {${p.type}, ${p.a}, ${p.b}, ${p.c}, ${fl(p.f)}, ${fl(p.g)}, ${fl(p.h)}},`).join('\n') + '\n};\n';
s += `const FdPred FD_PREDS[${preds.length}] = {\n` + preds.map((p) => `  {${p.type}, ${p.dx}, ${p.dy}, ${p.dz}, ${p.a}, ${p.b}},`).join('\n') + '\n};\n';
s += `const FdMod FD_MODS[${mods.length}] = {\n` + mods.map((m) => `  {${m.type}, ${m.a}, ${m.b}, ${m.c}, ${fl(m.f)}},`).join('\n') + '\n};\n';
s += `const FdCfg FD_CFGS[${cfgs.length}] = {\n` + cfgs.map((c) => `  {${c.type}, ${c.a}, ${c.b}, ${c.c}, ${c.d}, ${c.e}, ${fl(c.f)}},   // ${c.name}`).join('\n') + '\n};\n';
s += `const int FD_NUM_CFGS = ${cfgs.length};\n`;
s += `const FdPlaced FD_PLACED[${placed.length}] = {\n` + placed.map((p) => `  {${p.cfg}, ${p.first}, ${p.count}},   // ${p.name}`).join('\n') + '\n};\n';
s += `const int FD_NUM_PLACED = ${placed.length};\n`;
s += `const int16_t FD_BIOME_FIRST[${NB}] = {${biomeFirst.join(', ')}};\n`;
s += `const int16_t FD_BIOME_COUNT[${NB}] = {${biomeCount.join(', ')}};\n`;
s += `const int16_t FD_BIOME_PAIRS[${biomePairs.length}] = {${biomePairs.join(', ')}};   // step, placed feature\n`;
s += '}  // namespace vanilla\n}  // namespace mc\n';
fs.writeFileSync(OUT, s);
console.log(`${placed.length} placed features, ${cfgs.length} configured, ${mods.length} modifiers, ${states.length} states, ` +
  `${sets.length} block sets; not done: ${JSON.stringify(skipped)}`);
