#!/usr/bin/env node
// Compiles vanilla's structure placement into the tables of
// lib/mcore/src/mc/world/vanilla/structure_data.cpp, run by structures.cpp:
// - structure_set/*.json: the placements (random spread with its salt, spacing,
//   separation, spread type, frequency and its reduction method, exclusion zone;
//   concentric rings with distance, count, spread and preferred biomes) and the weighted
//   structures of each set;
// - structure/*.json: the structure's type, its biomes (tags expanded to a bit set of
//   biome ids), its decoration step and terrain adaptation;
// - loot_table/chests/*.json: the chests' loot (pools of weighted items with their
//   counts); item names resolved when a generator starts.
//   node tools/worldgen/compile_structures.js
'use strict';
const fs = require('fs');
const path = require('path');

const ROOT = path.join(__dirname, '..', '..');
const D = path.join(__dirname, '..', 'vanilla', '1.21.8', 'jar', 'data', 'minecraft');
const W = path.join(D, 'worldgen');
const OUT = path.join(ROOT, 'lib', 'mcore', 'src', 'mc', 'world', 'vanilla', 'structure_data.cpp');
const sh = (id) => id.replace(/^minecraft:/, '');
const camel = (id) => sh(id).split('_').map((w) => w[0].toUpperCase() + w.slice(1)).join('');
const json = (...p) => JSON.parse(fs.readFileSync(path.join(...p)));

const idsH = fs.readFileSync(path.join(ROOT, 'lib/mcore/src/mc/data/biome_ids_gen.h'), 'utf8');
const biomeBlock = idsH.slice(idsH.indexOf('namespace biome {'), idsH.indexOf('}', idsH.indexOf('namespace biome {')));
const biomeIds = {};
for (const m of biomeBlock.matchAll(/constexpr uint8_t (\w+) = (\d+);/g)) biomeIds[m[1]] = +m[2];
const NB = Object.keys(biomeIds).length;
if (NB > 128) throw new Error('more than 128 biomes');

const biomeTag = (tag) => {
  const out = [];
  for (const v of json(D, 'tags', 'worldgen', 'biome', sh(tag) + '.json').values) {
    const id = typeof v === 'string' ? v : v.id;
    if (id.startsWith('#')) out.push(...biomeTag(id.slice(1)));
    else if (biomeIds[camel(id)] !== undefined) out.push(biomeIds[camel(id)]);
  }
  return out;
};
const biomeBits = (spec) => {
  const ids = [].concat(spec).flatMap((b) => (b.startsWith('#') ? biomeTag(b.slice(1)) : [biomeIds[camel(b)]]));
  const bytes = new Array(16).fill(0);
  for (const i of ids) if (i !== undefined) bytes[i >> 3] |= 1 << (i & 7);
  return `{${bytes.join(', ')}}`;
};

// ------------------------------------------------------------ structures
const TYPES = ['desert_pyramid', 'jungle_temple', 'swamp_hut', 'buried_treasure', 'igloo', 'stronghold', 'fortress',
  'mineshaft', 'ocean_monument', 'jigsaw', 'ruined_portal', 'shipwreck', 'ocean_ruin', 'nether_fossil', 'end_city',
  'woodland_mansion'];
const STEPS = ['raw_generation', 'lakes', 'local_modifications', 'underground_structures', 'surface_structures',
  'strongholds', 'underground_ores', 'underground_decoration', 'fluid_springs', 'vegetal_decoration',
  'top_layer_modification'];
const ADAPT = ['none', 'beard_thin', 'beard_box', 'bury', 'encapsulate'];
const structures = [], structIndex = {};
const sets = [], setIndex = {};
const setFiles = fs.readdirSync(path.join(W, 'structure_set')).sort();
setFiles.forEach((f, i) => { setIndex['minecraft:' + f.replace('.json', '')] = i; });
for (const f of setFiles) {
  const s = json(W, 'structure_set', f);
  const p = s.placement;
  const first = structures.length;
  for (const e of s.structures) {
    const st = json(W, 'structure', sh(e.structure) + '.json');
    const type = TYPES.indexOf(sh(st.type));
    if (type < 0) throw new Error('structure type ' + st.type);
    let param = 0;
    if (st.mineshaft_type === 'mesa') param = 1;
    structIndex[e.structure] = structures.length;
    structures.push(`  {${type}, ${STEPS.indexOf(st.step)}, ${ADAPT.indexOf(st.terrain_adaptation || 'none')}, ${param}, ` +
      `${e.weight}, ${biomeBits(st.biomes)}, "${sh(e.structure)}"},`);
  }
  const red = ['default', 'legacy_type_1', 'legacy_type_2', 'legacy_type_3'].indexOf(p.frequency_reduction_method || 'default');
  const ex = p.exclusion_zone ? setIndex[p.exclusion_zone.other_set] : -1;
  const rings = p.type === 'minecraft:concentric_rings';
  sets.push(`  {${rings ? 1 : 0}, ${p.spread_type === 'triangular' ? 1 : 0}, ${red}, ${p.spacing || 0}, ${p.separation || 0}, ` +
    `${p.salt}, ${(p.frequency ?? 1).toFixed(4)}f, ${ex}, ${p.exclusion_zone ? p.exclusion_zone.chunk_count : 0}, ` +
    `${p.distance || 0}, ${p.count || 0}, ${p.spread || 0}, ${rings ? biomeBits(p.preferred_biomes) : '{}'}, ` +
    `${first}, ${s.structures.length}},   // ${f.replace('.json', '')}`);
}

// ------------------------------------------------------------ loot
const items = [], itemIdx = new Map();
const item = (n) => { n = sh(n); if (!itemIdx.has(n)) { itemIdx.set(n, items.length); items.push(n); } return itemIdx.get(n); };
const range = (v) => (typeof v === 'number' ? [v, v] : v.type === 'minecraft:uniform' ? [v.min, v.max]
  : v.type === 'minecraft:binomial' ? [0, v.n] : [1, 1]);
const tables = [], pools = [], entries = [];
for (const f of fs.readdirSync(path.join(D, 'loot_table', 'chests')).sort()) {
  if (!f.endsWith('.json')) continue;
  const t = json(D, 'loot_table', 'chests', f);
  const firstPool = pools.length;
  for (const p of t.pools || []) {
    const firstEntry = entries.length;
    for (const e of p.entries) {
      const w = e.weight ?? 1;
      if (e.type === 'minecraft:empty') { entries.push(`  {-1, ${w}, 0, 0, 0},`); continue; }
      if (e.type !== 'minecraft:item') continue;
      let lo = 1, hi = 1, flags = 0;
      for (const fn of e.functions || []) {
        if (fn.function === 'minecraft:set_count') [lo, hi] = range(fn.count);
        if (fn.function === 'minecraft:enchant_randomly' || fn.function === 'minecraft:enchant_with_levels') flags |= 1;
        if (fn.function === 'minecraft:set_damage') flags |= 2;
      }
      entries.push(`  {${item(e.name)}, ${w}, ${Math.floor(lo)}, ${Math.floor(hi)}, ${flags}},   // ${sh(e.name)}`);
    }
    const [rl, rh] = range(p.rolls);
    pools.push(`  {${Math.floor(rl)}, ${Math.floor(rh)}, ${firstEntry}, ${entries.length - firstEntry}},`);
  }
  tables.push(`  {"${f.replace('.json', '')}", ${firstPool}, ${pools.length - firstPool}},`);
}

let s = `// GENERATED by tools/worldgen/compile_structures.js from the 1.21.8 data pack. DO NOT EDIT.
#include "mc/world/vanilla/structures.h"

namespace mc {
namespace vanilla {
// placement (0 random spread, 1 concentric rings), triangular, frequency reduction (default,
// legacy 1 to 3), spacing, separation, salt, frequency, exclusion set and chunks, rings'
// distance, count and spread, preferred biomes, first structure and count
const StructureSetDef SD_SETS[${sets.length}] = {
${sets.join('\n')}
};
const int SD_NUM_SETS = ${sets.length};
// type, step, terrain adaptation, parameter, weight, biomes
const StructureDef SD_STRUCTURES[${structures.length}] = {
${structures.join('\n')}
};
const int SD_NUM_STRUCTURES = ${structures.length};

const char* const SD_LOOT_ITEMS[${items.length}] = {${items.map((n) => `"${n}"`).join(', ')}};
const int SD_NUM_LOOT_ITEMS = ${items.length};
// item (-1: nothing), weight, count from, to, flags (1 enchanted, 2 damaged)
const LootEntry SD_LOOT_ENTRIES[${entries.length}] = {
${entries.join('\n')}
};
// rolls from, to, first entry, entries
const LootPool SD_LOOT_POOLS[${pools.length}] = {
${pools.join('\n')}
};
const LootTable SD_LOOT_TABLES[${tables.length}] = {
${tables.join('\n')}
};
const int SD_NUM_LOOT_TABLES = ${tables.length};
}  // namespace vanilla
}  // namespace mc
`;
fs.writeFileSync(OUT, s);
console.log(`${sets.length} structure sets, ${structures.length} structures, ${tables.length} loot tables ` +
  `(${pools.length} pools, ${entries.length} entries, ${items.length} items)`);
