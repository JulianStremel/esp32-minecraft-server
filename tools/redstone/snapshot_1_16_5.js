#!/usr/bin/env node
// One-off: saves the per-state redstone and piston values that tools/redstone/generate.py
// extracted from the official 1.16.5 jar (with Java 11+) into states-1.16.5.json, keyed by
// block name and properties, so gen_data.js carries them over to the 1.21.8 state
// numbering. Run against the 1.16.5 tables (git show <1.16.5 commit>:lib/mcore/...).
'use strict';
const fs = require('fs');
const path = require('path');
const ROOT = path.join(__dirname, '..', '..');
const md = path.join(path.dirname(require.resolve('minecraft-data/package.json', { paths: [path.join(ROOT, 'tools')] })), 'minecraft-data', 'data', 'pc');
const blocks = JSON.parse(fs.readFileSync(path.join(md, '1.16.2', 'blocks.json')));
const read = (f) => fs.readFileSync(path.join(ROOT, 'lib/mcore/src/mc/data', f), 'utf8');
const arr = (src, name) => {
  const start = src.indexOf('{', src.indexOf(name + '['));
  const end = src.indexOf('}', start);
  return src.slice(start + 1, end).split(',').map((s) => s.trim()).filter((s) => s.length).map(Number);
};
const rs = read('redstone_data.cpp');
const flags = arr(rs, 'REDSTONE_STATE_PROPERTIES'), piston = arr(rs, 'PISTON_STATE_PROPERTIES');
const out = {};
for (const b of blocks) {
  const rows = [];
  for (let s = b.minStateId; s <= b.maxStateId; s++) {
    rows.push([flags[s], piston[s]]);
  }
  out[b.name] = {
    props: b.states.map((p) => [p.name, p.type === 'bool' ? ['true', 'false'] : p.type === 'int' ? Array.from({ length: p.num_values }, (_, i) => String(i)) : p.values]),
    rows,
  };
}
fs.writeFileSync(path.join(__dirname, 'states-1.16.5.json'), JSON.stringify(out));
console.log('states', flags.length, 'blocks', blocks.length);
