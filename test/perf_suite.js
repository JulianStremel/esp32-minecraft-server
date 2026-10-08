'use strict';
// Reproducible performance runs on a real board: fixed scenarios of hardware_stress.js,
// each repeated over fixed fresh spots, reduced to the median. Compare two results with
// tools/perf_compare.js.
//
//   node perf_suite.js --host 192.168.1.160 --label phase1-light [--runs 3] [--out file]
//       [--bench bench.json] [--only A,B,idle]
//
// Unmodified terrain is never saved, so a spot costs the same work in every run; each
// repetition uses another spot so that chunks left in the cache by the previous run do
// not help. --bench merges a hardware_bench.js result into the output.
const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const args = process.argv.slice(2);
const opt = (name, fallback) => {
  const i = args.indexOf('--' + name);
  return i < 0 ? fallback : args[i + 1];
};
const host = opt('host');
const label = opt('label');
const RUNS = Number(opt('runs', '3'));
if (!host || !label) { console.error('usage: node perf_suite.js --host <board ip> --label <name> [--runs 3]'); process.exit(2); }
const ROOT = path.join(__dirname, '..');
const out = opt('out', path.join(ROOT, 'docs', 'measurements', `perf-${label}.json`));

// spots far from anything explored, one per repetition (inside a radius-4096 world)
const SCENARIOS = {
  A: { desc: '8 flyers, 11 blocks/s, view 32, 60 s', args: ['--flyers', '8', '--speed', '11', '--view', '32', '--seconds', '60'],
    centers: [[30000, 30000], [30000, -30000], [-30000, 30000], [-30000, -30000], [45000, 0]] },
  B: { desc: '4 flyers, 5 blocks/s, view 12, 60 s', args: ['--flyers', '4', '--speed', '5', '--view', '12', '--seconds', '60'],
    centers: [[15000, 40000], [40000, 15000], [-15000, 40000], [-40000, 15000], [0, 45000]] },
  idle: { desc: '1 hovering player, view 12, 30 s', args: ['--flyers', '1', '--speed', '0', '--view', '12', '--seconds', '30'],
    centers: [[5000, -45000], [-5000, -45000], [45000, -5000], [-45000, -5000], [0, -50000]] },
};
const only = opt('only') ? opt('only').split(',') : Object.keys(SCENARIOS);

function median(xs) {
  const v = xs.filter((x) => typeof x === 'number' && isFinite(x)).sort((a, b) => a - b);
  if (!v.length) return null;
  return v.length % 2 ? v[(v.length - 1) / 2] : (v[v.length / 2 - 1] + v[v.length / 2]) / 2;
}

// element-wise median over runs (numbers and arrays of numbers)
function medianOf(runs) {
  const m = {};
  for (const k of Object.keys(runs[0])) {
    const vals = runs.map((r) => r[k]);
    if (typeof vals[0] === 'number') m[k] = median(vals);
    else if (Array.isArray(vals[0]) && typeof vals[0][0] === 'number') m[k] = vals[0].map((_, i) => median(vals.map((v) => v[i])));
  }
  return m;
}

const commit = (spawnSync('git', ['rev-parse', '--short', 'HEAD'], { cwd: ROOT, encoding: 'utf8' }).stdout || '').trim();
const dirty = (spawnSync('git', ['status', '--porcelain', '--', 'lib', 'src'], { cwd: ROOT, encoding: 'utf8' }).stdout || '').trim() !== '';
const result = { label, date: new Date().toISOString(), commit: commit + (dirty ? '+changes' : ''), runs: RUNS, scenarios: {} };
let failed = 0;
for (const name of only) {
  const sc = SCENARIOS[name];
  const runs = [];
  for (let r = 0; r < RUNS; r++) {
    const c = sc.centers[r % sc.centers.length];
    const tmp = path.join(os.tmpdir(), `mc-perf-${process.pid}-${name}-${r}.json`);
    console.log(`\n=== ${name} run ${r + 1}/${RUNS} (${sc.desc}) at ${c.join(',')}`);
    const p = spawnSync(process.execPath, [path.join(__dirname, 'hardware_stress.js'), '--host', host, ...sc.args,
      '--center', c.join(','), '--json', tmp], { stdio: 'inherit', timeout: 600000 });
    if (p.status !== 0 || !fs.existsSync(tmp)) { failed++; console.log(`=== ${name} run ${r + 1} FAILED`); continue; }
    runs.push(JSON.parse(fs.readFileSync(tmp, 'utf8')));
    fs.unlinkSync(tmp);
    // let the board drop the departed players and settle before the next run
    spawnSync(process.execPath, ['-e', 'setTimeout(()=>{},5000)']);
  }
  if (runs.length) result.scenarios[name] = { desc: sc.desc, median: medianOf(runs), runs };
}
if (opt('bench')) result.bench = JSON.parse(fs.readFileSync(opt('bench'), 'utf8'));
fs.mkdirSync(path.dirname(out), { recursive: true });
fs.writeFileSync(out, JSON.stringify(result, null, 1) + '\n');
console.log(`\nwrote ${path.relative(process.cwd(), out)}${failed ? ` (${failed} run(s) failed)` : ''}`);
process.exitCode = failed ? 1 : 0;
