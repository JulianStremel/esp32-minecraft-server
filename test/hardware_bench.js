'use strict';
// Captures the on-device chunk pipeline benchmark: flash a bench build first
// (idf.py -B build/esp32s3-8-bench -D MC_BENCH=ON ... flash), then
//   node hardware_bench.js --serial COM5 --python <idf python> [--json bench.json]
// The board is reset, its "[bench]" lines are collected until "[bench] done", and the
// stage timings are written as JSON. Flash the normal firmware afterwards.
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');

const args = process.argv.slice(2);
const opt = (name, fallback) => {
  const i = args.indexOf('--' + name);
  return i < 0 ? fallback : args[i + 1];
};
const serialPort = opt('serial');
const python = opt('python', process.env.IDF_PYTHON || 'python');
if (!serialPort) { console.error('usage: node hardware_bench.js --serial <port> [--json out.json]'); process.exit(2); }

const proc = spawn(python, [path.join(__dirname, '..', 'tools', 'idf', 'serial_monitor.py'), serialPort, '--reset'],
  { stdio: ['pipe', 'pipe', 'pipe'] });
const lines = [];
let buf = '';
let finished = false;
const timer = setTimeout(() => finish(new Error('no "[bench] done" within 10 minutes')), 600000);

function parse() {
  const r = { stages: {}, lines };
  for (const l of lines) {
    let m = /^\[bench\] (.+?)\s+([0-9.]+) ms avg\s+([0-9.]+) ms max\s+\((\d+)\)/.exec(l);
    if (m) { r.stages[m[1].trim()] = { avgMs: +m[2], maxMs: +m[3], n: +m[4] }; continue; }
    m = /sending a resident chunk: ([0-9.]+) ms; a new chunk \(generate \+ send\): ([0-9.]+) ms/.exec(l);
    if (m) { r.sendMs = +m[1]; r.newChunkMs = +m[2]; continue; }
    m = /^\[bench\] (generate chunk|ALU-only \(ref\.\))\s+x(\d+), (\d+) workers?.*?([0-9.]+) ms\s+\(\s*([0-9.]+) jobs\/s/.exec(l);
    if (m) {
      r.jobs = r.jobs || {};
      r.jobs[`${m[1]} ${m[3]}w`] = { ms: +m[4], jobsPerS: +m[5] };
    }
  }
  return r;
}

function finish(err) {
  if (finished) return;
  finished = true;
  clearTimeout(timer);
  proc.kill();
  if (err) { console.error(err.message); process.exitCode = 1; return; }
  const r = parse();
  if (opt('json')) fs.writeFileSync(opt('json'), JSON.stringify(r, null, 1) + '\n');
  console.log(`bench: ${Object.keys(r.stages).length} stages, new chunk ${r.newChunkMs} ms`);
}

const onData = (d) => {
  buf += d.toString();
  let nl;
  while ((nl = buf.indexOf('\n')) >= 0) {
    const line = buf.slice(0, nl).replace(/\r$/, '');
    buf = buf.slice(nl + 1);
    if (!line.startsWith('[bench]')) continue;
    console.log(line);
    if (line === '[bench] done') finish();
    else lines.push(line);
  }
};
proc.stdout.on('data', onData);
proc.stderr.on('data', onData);
proc.on('exit', () => finish(new Error('serial monitor exited')));
