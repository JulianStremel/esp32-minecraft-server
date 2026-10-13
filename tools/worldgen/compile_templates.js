#!/usr/bin/env node
// Compiles vanilla's structure templates, template pools and processor lists into
// lib/mcore/src/mc/world/vanilla/template_data.cpp (run by templates.cpp and jigsaw.cpp):
// - structure/**/*.nbt: each template's size, palette (block states as names, resolved
//   when a generator starts) and blocks run-length coded over the template (y, z, x;
//   structure_void left out), its jigsaw blocks (name, target, pool, final state, joint,
//   priorities, orientation), its containers' loot tables and its entities;
// - worldgen/template_pool/**: pools of weighted elements (single, legacy single, list,
//   feature, empty) with their projection and processors, and their fallbacks;
// - worldgen/processor_list: rule, block_rot, protected_blocks and capped processors.
// About 0.6 MB of the 6.2 MB of gzipped NBT the game ships.
//   node tools/worldgen/compile_templates.js
'use strict';
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const nbt = require(path.join(__dirname, '..', '..', 'test', 'node_modules', 'prismarine-nbt'));

const ROOT = path.join(__dirname, '..', '..');
const D = path.join(__dirname, '..', 'vanilla', '1.21.8', 'jar', 'data', 'minecraft');
const W = path.join(D, 'worldgen');
const OUT = path.join(ROOT, 'lib', 'mcore', 'src', 'mc', 'world', 'vanilla', 'template_data.cpp');
const sh = (id) => id.replace(/^minecraft:/, '');
const fl = (v) => { const t = String(+v); return /[.e]/.test(t) ? t + 'f' : t + '.0f'; };
const json = (...p) => JSON.parse(fs.readFileSync(path.join(...p)));

// ------------------------------------------------------------ string pools
const strings = [], stringIdx = new Map();
const str = (s) => { if (!stringIdx.has(s)) { stringIdx.set(s, strings.length); strings.push(s); } return stringIdx.get(s); };
str('');   // 0: none
const states = [], stateIdx = new Map();
const stateKey = (name, props) => {
  const p = props ? Object.entries(props).sort().map(([k, v]) => `${k}=${v}`).join(',') : '';
  return sh(name) + (p ? `[${p}]` : '');
};
const state = (key) => { if (!stateIdx.has(key)) { stateIdx.set(key, states.length); states.push(key); } return stateIdx.get(key); };
const stateFromString = (s) => {   // "minecraft:oak_stairs[facing=east,half=bottom]"
  const m = /^([^\[]+)(?:\[(.*)\])?$/.exec(s);
  const props = {};
  if (m[2]) for (const kv of m[2].split(',')) { const [k, v] = kv.split('='); props[k] = v; }
  return state(stateKey(m[1], Object.keys(props).length ? props : null));
};
state('air');   // 0
const tagBlocks = (tag) => {
  const out = [];
  for (const v of json(D, 'tags', 'block', sh(tag) + '.json').values) {
    const id = typeof v === 'string' ? v : v.id;
    if (id.startsWith('#')) out.push(...tagBlocks(id.slice(1)));
    else out.push(sh(id));
  }
  return [...new Set(out)];
};
const sets = [], setKey = new Map(), setNames = [];   // block sets (tags)
const blockSet = (tag) => {
  if (!setKey.has(tag)) {
    const list = tagBlocks(tag);
    setKey.set(tag, sets.length);
    sets.push({ first: setNames.length, count: list.length });
    setNames.push(...list.map(str));
  }
  return setKey.get(tag);
};

// ------------------------------------------------------------ templates
const bytes = [];   // the blob: palettes' runs
const varint = (v) => { while (v >= 0x80) { bytes.push((v & 0x7F) | 0x80); v >>>= 7; } bytes.push(v); };
const templates = [], templateIdx = new Map();
const palette = [], jigsaws = [], containers = [], entities = [], markers = [];
const ORIENT = ['down_east', 'down_north', 'down_south', 'down_west', 'up_east', 'up_north', 'up_south', 'up_west',
  'west_up', 'east_up', 'north_up', 'south_up'];
function loadTemplate(name) {
  if (templateIdx.has(name)) return templateIdx.get(name);
  const file = path.join(D, 'structure', sh(name) + '.nbt');
  if (!fs.existsSync(file)) { templateIdx.set(name, -1); return -1; }
  const t = nbt.simplify(nbt.parseUncompressed(zlib.gunzipSync(fs.readFileSync(file))));
  const [sx, sy, sz] = t.size;
  if (sx > 255 || sy > 255 || sz > 255) throw new Error('template too big ' + name);
  // several palettes (the shipwrecks' woods): the same blocks, one chosen per placement
  const pals = t.palettes || [t.palette];
  const pal = pals[0];
  const keys = pal.map((p) => stateKey(p.Name, p.Properties));
  const palStart = palette.length;
  for (const pl of pals) pl.forEach((p) => palette.push(state(stateKey(p.Name, p.Properties))));
  const grid = new Int32Array(sx * sy * sz);   // palette index + 1, 0 none
  const jigFirst = jigsaws.length, conFirst = containers.length, entFirst = entities.length, markFirst = markers.length;
  // the jigsaws in the template's own order (StructureTemplate: by y, x, z)
  const jigs = [];
  for (const b of t.blocks) {
    const key = keys[b.state];
    if (key.startsWith('structure_void')) continue;
    if (key.startsWith('structure_block') && b.nbt && b.nbt.mode === 'DATA')   // a data marker ("chest", "Sentry", ...)
      markers.push(`  {${b.pos[0]}, ${b.pos[1]}, ${b.pos[2]}, ${str(b.nbt.metadata)}},`);
    grid[(b.pos[1] * sz + b.pos[2]) * sx + b.pos[0]] = b.state + 1;
    if (b.nbt && key.startsWith('jigsaw')) {
      const n = b.nbt;
      const orient = ORIENT.indexOf(/orientation=([a-z_]+)/.exec(key)[1]);
      const front = ORIENT[orient].split('_')[0];
      const joint = n.joint ? (n.joint === 'rollable' ? 0 : 1) : (front === 'up' || front === 'down' ? 0 : 1);
      jigs.push({ pos: b.pos, s: `  {${b.pos[0]}, ${b.pos[1]}, ${b.pos[2]}, ${orient}, ${joint}, ${n.selection_priority || 0}, ${n.placement_priority || 0}, ` +
        `${str(n.name || '')}, ${str(n.target || '')}, ${str(n.pool || '')}, ${stateFromString(n.final_state || 'minecraft:air')}},` });
    } else if (b.nbt && b.nbt.LootTable) {
      containers.push(`  {${b.pos[0]}, ${b.pos[1]}, ${b.pos[2]}, ${str(sh(b.nbt.LootTable))}},`);
    }
  }
  jigs.sort((a, b) => a.pos[1] - b.pos[1] || a.pos[0] - b.pos[0] || a.pos[2] - b.pos[2]);
  jigsaws.push(...jigs.map((j) => j.s));
  for (const e of t.entities || []) {
    if (!e.nbt || !e.nbt.id) continue;
    entities.push(`  {${e.blockPos[0]}, ${e.blockPos[1]}, ${e.blockPos[2]}, ${str(sh(e.nbt.id))}},`);
  }
  const data = bytes.length;
  let prev = -1, run = 0;
  const flush = () => { if (run) { varint(run); varint(prev); } };
  for (let i = 0; i < grid.length; i++) {
    if (grid[i] === prev) run++;
    else { flush(); prev = grid[i]; run = 1; }
  }
  flush();
  const idx = templates.length;
  templates.push(`  {${str(sh(name))}, ${sx}, ${sy}, ${sz}, ${data}, ${palStart}, ${pal.length}, ${jigFirst}, ${jigsaws.length - jigFirst}, ` +
    `${conFirst}, ${containers.length - conFirst}, ${entFirst}, ${entities.length - entFirst}, ${markFirst}, ${markers.length - markFirst}, ${pals.length}},`);
  templateIdx.set(name, idx);
  return idx;
}

// ------------------------------------------------------------ processor lists
const procLists = [], procListIdx = new Map(), processors = [], rules = [];
const PT = { 'minecraft:rule': 0, 'minecraft:block_rot': 1, 'minecraft:protected_blocks': 2, 'minecraft:capped': 3 };
function predicate(p) {   // type, block/state/set, probability
  switch (p.predicate_type) {
    case 'minecraft:always_true': return [0, 0, 0];
    case 'minecraft:block_match': return [1, str(sh(p.block)), 0];
    case 'minecraft:blockstate_match': return [2, state(stateKey(p.block_state.Name, p.block_state.Properties)), 0];
    case 'minecraft:random_block_match': return [3, str(sh(p.block)), p.probability];
    case 'minecraft:tag_match': return [4, blockSet(p.tag), 0];
    case 'minecraft:random_blockstate_match': return [5, state(stateKey(p.block_state.Name, p.block_state.Properties)), p.probability];
    default: throw new Error('predicate ' + p.predicate_type);
  }
}
function processorList(spec) {
  const key = typeof spec === 'string' ? spec : JSON.stringify(spec);
  if (procListIdx.has(key)) return procListIdx.get(key);
  const list = typeof spec === 'string' ? json(W, 'processor_list', sh(spec) + '.json').processors : spec.processors || [];
  const first = processors.length;
  const add = (p) => {
    const t = PT[p.processor_type];
    if (t === undefined) throw new Error('processor ' + p.processor_type);
    if (t === 0) {
      const rf = rules.length;
      for (const r of p.rules) {
        const ip = predicate(r.input_predicate), lp = predicate(r.location_predicate);
        const pp = r.position_predicate && r.position_predicate.predicate_type === 'minecraft:axis_aligned_linear_pos' ? r.position_predicate : null;
        const loot = r.block_entity_modifier && r.block_entity_modifier.type === 'minecraft:append_loot' ? str(sh(r.block_entity_modifier.loot_table)) : 0;
        rules.push(`  {${ip[0]}, ${lp[0]}, ${pp ? 1 : 0}, ${ip[1]}, ${lp[1]}, ${fl(ip[2])}, ${fl(lp[2])}, ` +
          `${pp ? `${fl(pp.min_chance || 0)}, ${fl(pp.max_chance || 0)}, ${pp.min_dist || 0}, ${pp.max_dist || 0}, ${'xyz'.indexOf(pp.axis || 'y')}` : '0, 0, 0, 0, 1'}, ` +
          `${state(stateKey(r.output_state.Name, r.output_state.Properties))}, ${loot}},`);
      }
      processors.push(`  {0, ${rf}, ${rules.length - rf}, 0, 0.0f},`);
    } else if (t === 1) {
      processors.push(`  {1, 0, 0, ${p.rottable_blocks ? blockSet(p.rottable_blocks.replace('#', '')) + 1 : 0}, ${fl(p.integrity)}},`);
    } else if (t === 2) {
      processors.push(`  {2, 0, 0, ${blockSet(p.value.replace('#', '')) + 1}, 0.0f},`);
    }   // capped (the trail ruins' suspicious gravel and sand): left out, not unlimited
  };
  for (const p of list) add(p);
  const i = procLists.length;
  procLists.push(`  {${first}, ${processors.length - first}},`);
  procListIdx.set(key, i);
  return i;
}
processorList({ processors: [] });   // 0: none

// ------------------------------------------------------------ pools
const pools = [], poolIdx = new Map(), elements = [], listItems = [];
const poolFiles = [];
(function walk(d, rel) {
  for (const f of fs.readdirSync(d).sort()) {
    const p = path.join(d, f);
    if (fs.statSync(p).isDirectory()) walk(p, rel + f + '/');
    else if (f.endsWith('.json')) poolFiles.push(rel + f.replace('.json', ''));
  }
})(path.join(W, 'template_pool'), '');
poolFiles.forEach((n, i) => poolIdx.set('minecraft:' + n, i));
poolIdx.set('minecraft:empty', poolFiles.indexOf('empty'));
const ET = { 'minecraft:empty_pool_element': 0, 'minecraft:single_pool_element': 1, 'minecraft:legacy_single_pool_element': 2,
  'minecraft:list_pool_element': 3, 'minecraft:feature_pool_element': 4 };
function element(e) {   // type, projection, template (or list start), count, processors, feature
  const t = ET[e.element_type];
  const proj = e.projection === 'terrain_matching' ? 1 : 0;
  if (t === 0) return `0, 0, 0, 0, 0, 0`;
  if (t === 4) return `4, ${proj}, 0, 0, 0, ${str(sh(e.feature))}`;
  if (t === 3) {
    const first = listItems.length;
    const subs = e.elements.map(element);
    listItems.push(...subs.map((s) => `  {${s}, 1},`));
    return `3, ${proj}, ${first}, ${subs.length}, 0, 0`;
  }
  const ti = loadTemplate(e.location);
  return `${t}, ${proj}, ${ti < 0 ? 0 : ti}, ${ti < 0 ? 0 : 1}, ${processorList(e.processors)}, 0`;
}
for (const name of poolFiles) {
  const p = json(W, 'template_pool', name + '.json');
  const first = elements.length;
  for (const e of p.elements || []) elements.push(`  {${element(e.element)}, ${e.weight}},`);
  const fb = poolIdx.has(p.fallback) ? poolIdx.get(p.fallback) : -1;
  pools.push(`  {${str(name)}, ${fb}, ${first}, ${elements.length - first}},`);
}
// the templates the other structures place by name (igloos, shipwrecks, ruins, portals,
// fossils, End cities, mansions)
for (const dir of ['igloo', 'shipwreck', 'underwater_ruin', 'ruined_portal', 'fossil', 'nether_fossils', 'end_city', 'woodland_mansion']) {
  (function walk(d, rel) {
    for (const f of fs.readdirSync(d).sort()) {
      const p = path.join(d, f);
      if (fs.statSync(p).isDirectory()) walk(p, rel + f + '/');
      else if (f.endsWith('.nbt')) loadTemplate('minecraft:' + rel + f.replace('.nbt', ''));
    }
  })(path.join(D, 'structure', dir), dir + '/');
}

const c = (s) => JSON.stringify(s);
let out = `// GENERATED by tools/worldgen/compile_templates.js from the 1.21.8 data pack. DO NOT EDIT.
#include "mc/world/vanilla/templates.h"

namespace mc {
namespace vanilla {
const char* const TD_STRINGS[${strings.length}] = {
${strings.map((s) => '  ' + c(s) + ',').join('\n')}
};
const int TD_NUM_STRINGS = ${strings.length};
const char* const TD_STATES[${states.length}] = {
${states.map((s) => '  ' + c(s) + ',').join('\n')}
};
const int TD_NUM_STATES = ${states.length};
const TdSet TD_SETS[${sets.length || 1}] = {${sets.map((s) => `{${s.first}, ${s.count}}`).join(', ')}};
const uint16_t TD_SET_NAMES[${setNames.length || 1}] = {${setNames.join(', ')}};
// name, size x y z, data offset, palette first and count, jigsaws, containers, entities, markers, palettes
const TdTemplate TD_TEMPLATES[${templates.length}] = {
${templates.join('\n')}
};
const int TD_NUM_TEMPLATES = ${templates.length};
const uint16_t TD_PALETTE[${palette.length}] = {${palette.join(', ')}};
// x, y, z, orientation, joint (0 rollable), selection and placement priority, name, target,
// pool, final state
const TdJigsaw TD_JIGSAWS[${jigsaws.length}] = {
${jigsaws.join('\n')}
};
const TdContainer TD_CONTAINERS[${containers.length || 1}] = {
${containers.join('\n')}
};
const TdEntity TD_ENTITIES[${entities.length || 1}] = {
${entities.join('\n')}
};
// data markers: x, y, z, metadata
const TdMarker TD_MARKERS[${markers.length || 1}] = {
${markers.join('\n')}
};
// in (type, block, probability), location (type, block, probability), position (min and max
// chance, min and max distance, axis), output state, appended loot
const TdRule TD_RULES[${rules.length || 1}] = {
${rules.join('\n')}
};
// type (0 rule, 1 block_rot, 2 protected_blocks), rules, block set + 1, integrity
const TdProcessor TD_PROCESSORS[${processors.length || 1}] = {
${processors.join('\n')}
};
const TdProcList TD_PROC_LISTS[${procLists.length}] = {
${procLists.join('\n')}
};
// type (0 empty, 1 single, 2 legacy single, 3 list, 4 feature), projection (1 terrain
// matching), template or list start, count, processors, feature, weight
const TdElement TD_ELEMENTS[${elements.length}] = {
${elements.join('\n')}
};
const TdElement TD_LIST_ITEMS[${listItems.length || 1}] = {
${listItems.join('\n')}
};
// name, fallback, first element, elements
const TdPool TD_POOLS[${pools.length}] = {
${pools.join('\n')}
};
const int TD_NUM_POOLS = ${pools.length};
const uint8_t TD_BLOB[${bytes.length}] = {
`;
for (let i = 0; i < bytes.length; i += 40) out += '  ' + bytes.slice(i, i + 40).join(',') + ',\n';
out += `};
}  // namespace vanilla
}  // namespace mc
`;
fs.writeFileSync(OUT, out);
console.log(`${templates.length} templates, ${bytes.length} bytes of blocks, ${palette.length} palette entries, ` +
  `${states.length} states, ${jigsaws.length} jigsaws, ${containers.length} containers, ${entities.length} entities, ` +
  `${pools.length} pools (${elements.length} elements), ${procLists.length} processor lists (${processors.length} processors, ` +
  `${rules.length} rules), ${strings.length} strings`);
