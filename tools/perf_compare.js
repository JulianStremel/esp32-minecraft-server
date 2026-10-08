'use strict';
// Compares two perf_suite.js results and flags regressions.
//   node tools/perf_compare.js docs/measurements/perf-baseline.json docs/measurements/perf-<new>.json
// Exits with 1 when a metric crosses its regression limit (see LIMITS).
const fs = require('fs');

const [aPath, bPath] = process.argv.slice(2);
if (!aPath || !bPath) { console.error('usage: node tools/perf_compare.js <before.json> <after.json>'); process.exit(2); }
const A = JSON.parse(fs.readFileSync(aPath, 'utf8'));
const B = JSON.parse(fs.readFileSync(bPath, 'utf8'));

// metric, unit, better direction, limit check (before, after) -> message or null
const pct = (a, b) => (a ? ((b - a) / a) * 100 : 0);
const METRICS = [
  ['tpsAvg', '', 'up', (a, b) => (b < 19.8 ? `TPS ${b.toFixed(2)} < 19.8` : null)],
  ['tpsMin', '', 'up', null],
  ['msptAvg', 'ms', 'down', null],
  ['tickMax', 'ms', 'down', null],
  ['stallMax', 'ms', 'down', (a, b) => (b > 60 ? `stall ${b} ms > 60` : b > a * 1.25 && b - a > 5 ? `stall +${pct(a, b).toFixed(0)}%` : null)],
  ['heapMinKb', 'KB', 'up', (a, b) => (a - b > 300 ? `heap min -${a - b} KB` : null)],
  ['generatedPerS', '/s', 'up', (a, b) => (b < a * 0.9 ? `chunks/s ${pct(a, b).toFixed(0)}%` : null)],
  ['sentPerS', '/s', 'up', null],
  ['busyAvg', '%', 'down', null],
  ['ownChunkMissing', '', 'down', null],
];

let regressions = 0;
const fmt = (v) => (v === null || v === undefined ? '-' : Math.abs(v) >= 100 ? v.toFixed(0) : v.toFixed(2));
console.log(`before: ${A.label} (${A.commit}, ${A.date.slice(0, 16)})   after: ${B.label} (${B.commit}, ${B.date.slice(0, 16)})`);
for (const sc of Object.keys(B.scenarios)) {
  const a = A.scenarios[sc] && A.scenarios[sc].median, b = B.scenarios[sc].median;
  if (!a) { console.log(`\n[${sc}] no baseline`); continue; }
  console.log(`\n[${sc}] ${B.scenarios[sc].desc}${b.othersOnline ? `  (note: ${b.othersOnline} other player(s) online)` : ''}`);
  for (const [k, unit, dir, limit] of METRICS) {
    if (typeof a[k] !== 'number' || typeof b[k] !== 'number') continue;
    const d = b[k] - a[k];
    const worse = dir === 'up' ? d < 0 : d > 0;
    const msg = limit ? limit(a[k], b[k]) : null;
    if (msg) regressions++;
    console.log(`  ${k.padEnd(16)} ${fmt(a[k]).padStart(8)} -> ${fmt(b[k]).padStart(8)} ${unit.padEnd(3)} ` +
      `${(d >= 0 ? '+' : '') + fmt(d)}${a[k] ? ` (${pct(a[k], b[k]) >= 0 ? '+' : ''}${pct(a[k], b[k]).toFixed(1)}%)` : ''}` +
      `${worse && Math.abs(pct(a[k], b[k])) > 5 ? '  worse' : ''}${msg ? `  REGRESSION: ${msg}` : ''}`);
  }
}
if (A.bench && B.bench) {
  console.log('\n[bench] ms per chunk (device micro-benchmark)');
  for (const s of Object.keys(B.bench.stages)) {
    const a = A.bench.stages[s], b = B.bench.stages[s];
    if (!a) { console.log(`  ${s.padEnd(38)} new: ${b.avgMs} ms`); continue; }
    const p = pct(a.avgMs, b.avgMs);
    const reg = p > 10 && b.avgMs - a.avgMs > 0.2;
    if (reg) regressions++;
    console.log(`  ${s.padEnd(38)} ${fmt(a.avgMs).padStart(7)} -> ${fmt(b.avgMs).padStart(7)} ms (${p >= 0 ? '+' : ''}${p.toFixed(1)}%)${reg ? '  REGRESSION' : ''}`);
  }
  if (A.bench.newChunkMs && B.bench.newChunkMs)
    console.log(`  new chunk (generate + send): ${A.bench.newChunkMs} -> ${B.bench.newChunkMs} ms`);
}
console.log(regressions ? `\n${regressions} regression(s)` : '\nno regressions');
process.exitCode = regressions ? 1 : 0;
