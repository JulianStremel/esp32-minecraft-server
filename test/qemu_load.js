'use strict';
// Load test of the real firmware on the emulated ESP32-S3 (tools/qemu/run.sh):
// virtual players are moved through untouched terrain, so every chunk they need
// has to be generated, lit, compressed and sent by the device. The test runs once
// per worker-pool size (switched at runtime with /workers) and reports TPS, tick
// times, the longest game-loop stall and the chunk throughput.
//
//   node test/qemu_load.js [--bots 6] [--seconds 30] [--workers 0,2] [--step 3]
//                          [--mttcg | --icount N] [--no-build] [--port 25580]
//
// --mttcg (default): one host thread per emulated core, so the two cores really run
//   in parallel; times follow the host CPU (compare the phases, not absolute values).
// --icount N: instruction-counted, deterministic timing (2^N ns per instruction), but
//   both emulated cores share one clock: it cannot show a parallel speed-up.
const { spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { connectBot, sleep, waitFor, nextChat } = require('./lib');

const ROOT = path.resolve(__dirname, '..');
const args = process.argv.slice(2);
const opt = (name, def) => {
  const i = args.indexOf('--' + name);
  return i >= 0 ? args[i + 1] : def;
};
const flag = (name) => args.includes('--' + name);
const BOTS = parseInt(opt('bots', '6'));
const SECONDS = parseInt(opt('seconds', '30'));
const STEP_S = parseFloat(opt('step', '3'));          // seconds between moves
const STEP_BLOCKS = parseInt(opt('stride', '24'));    // blocks per move
const WORKERS = opt('workers', '0,2').split(',').map((s) => parseInt(s));
const PORT = parseInt(opt('port', '25580'));
const NBD_PORT = PORT + 1000;
const ICOUNT = opt('icount', null);
const MODE = ICOUNT ? `icount shift=${ICOUNT}` : 'MTTCG';
// fresh terrain for every phase (the 512 MiB world image allows +-496 blocks)
const CENTERS = [[-300, -300], [300, 300], [-300, 300], [300, -300]];

function startQemu() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'mcqemu-'));
  const world = path.join(dir, 'world.img');
  const runArgs = [path.join(ROOT, 'tools/qemu/run.sh'), '--headless', '--port', String(PORT), '--nbd-port',
    String(NBD_PORT), '--world', world];
  if (ICOUNT) runArgs.push('--icount', ICOUNT);
  else runArgs.push('--mttcg');
  if (flag('no-build')) runArgs.push('--no-build');
  const proc = spawn('bash', runArgs, { stdio: ['ignore', 'pipe', 'pipe'] });
  const logPath = path.join(dir, 'serial.log');
  const log = fs.createWriteStream(logPath);
  const stats = [];
  let buf = '';
  let listening = false;
  const onData = (d) => {
    log.write(d);
    buf += d.toString();
    let nl;
    while ((nl = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, nl).replace(/\r$/, '');
      buf = buf.slice(nl + 1);
      if (/listening on port/.test(line)) listening = true;
      if (/^\[stat\]/.test(line)) stats.push(line);
      if (/Guru Meditation|assert failed|\[FATAL\]|Backtrace:/.test(line)) console.log('  device: ' + line);
      if (process.env.LOG) console.log('  | ' + line);
    }
  };
  proc.stdout.on('data', onData);
  proc.stderr.on('data', onData);
  return {
    proc, dir, logPath, stats,
    ready: waitFor(() => listening || proc.exitCode !== null, 900000, 'firmware boot').then(() => {
      if (!listening) throw new Error('QEMU exited before the server started (see ' + logPath + ')');
    }),
    stop: () => new Promise((res) => {
      if (proc.exitCode !== null) return res();
      proc.once('exit', () => res());
      proc.kill('SIGTERM');
      setTimeout(() => { try { proc.kill('SIGKILL'); } catch (e) { /* gone */ } res(); }, 10000);
    }),
  };
}

function parseTps(line) {
  const m = /TPS ([0-9.]+), ([0-9.]+) ms\/tick \(max (\d+)\), max loop stall (\d+) ms/.exec(line);
  if (!m) return null;
  return { tps: +m[1], mspt: +m[2], tickMax: +m[3], stall: +m[4] };
}

async function command(op, cmd, reply, ms = 15000) {
  const r = nextChat(op, reply, ms);
  op.chat(cmd);
  return r;
}

async function phase(bots, op, workers, center) {
  const reply = await command(op, `/workers ${workers}`, /^Jobs:/, 60000);
  console.log(`\n== ${workers} worker${workers === 1 ? '' : 's'}: ${reply.slice(6, 120)}`);
  // spread the players around a fresh area
  bots.forEach((b, i) => {
    const a = (i / bots.length) * Math.PI * 2;
    b.dir = [Math.cos(a), Math.sin(a)];
    b.pos = [center[0] + b.dir[0] * 40, center[1] + b.dir[1] * 40];
    op.chat(`/tp ${b.username} ${b.pos[0].toFixed(1)} 140 ${b.pos[1].toFixed(1)}`);
    b.chunks = 0;
  });
  // switching the pool drains it (a stall of its own): let the 2 s measuring window roll over
  await sleep(2500);
  bots.forEach((b) => { b.chunks = 0; });
  const samples = [];
  let worst = null;
  const t0 = Date.now();
  let nextMove = t0 + STEP_S * 1000;
  let nextSample = t0 + 2000;
  while (Date.now() - t0 < SECONDS * 1000) {
    if (Date.now() >= nextMove) {
      nextMove += STEP_S * 1000;
      for (const b of bots) {
        b.pos = [b.pos[0] + b.dir[0] * STEP_BLOCKS, b.pos[1] + b.dir[1] * STEP_BLOCKS];
        op.chat(`/tp ${b.username} ${b.pos[0].toFixed(1)} 140 ${b.pos[1].toFixed(1)}`);
      }
    }
    if (Date.now() >= nextSample) {
      nextSample += 2000;
      try {
        const s = parseTps(await command(op, '/tps', /^TPS/, 10000));
        if (s) samples.push(s);
      } catch (e) {
        samples.push({ tps: 0, mspt: 0, tickMax: 0, stall: 10000, timeout: true });
      }
      try {
        const lag = await command(op, '/lag', /^Slowest loop/, 5000);
        const m = /^Slowest loop: (\d+) ms/.exec(lag);
        if (m && (!worst || +m[1] > worst.ms)) worst = { ms: +m[1], text: lag.slice(14) };
      } catch (e) { /* older firmware without /lag */ }
    }
    await sleep(100);
  }
  const secs = (Date.now() - t0) / 1000;
  const chunks = bots.reduce((n, b) => n + b.chunks, 0);
  const avg = (k) => samples.reduce((n, s) => n + s[k], 0) / Math.max(1, samples.length);
  const max = (k) => samples.reduce((n, s) => Math.max(n, s[k]), 0);
  const min = (k) => samples.reduce((n, s) => Math.min(n, s[k]), Infinity);
  const r = {
    workers, samples: samples.length, tps: avg('tps'), tpsMin: min('tps'), mspt: avg('mspt'), tickMax: max('tickMax'),
    stallAvg: avg('stall'), stallMax: max('stall'), chunksPerS: chunks / secs,
    connected: bots.filter((b) => b._client.state === 'play' && !b.ended).length,
  };
  console.log(`   TPS ${r.tps.toFixed(1)} (min ${r.tpsMin.toFixed(1)}), ${r.mspt.toFixed(1)} ms/tick (max ${r.tickMax}), ` +
    `loop stall avg ${r.stallAvg.toFixed(0)} ms / max ${r.stallMax} ms, ${r.chunksPerS.toFixed(1)} chunks/s delivered, ` +
    `${r.connected}/${bots.length} players connected`);
  if (worst) console.log(`   slowest loop seen: ${worst.text}`);
  return r;
}

(async () => {
  console.log(`QEMU load test: ${BOTS} players, ${SECONDS} s per phase, workers ${WORKERS.join(' / ')}, ${MODE}`);
  const q = startQemu();
  const bots = [];
  let failed = false;
  try {
    await q.ready;
    console.log('firmware is up (serial log: ' + q.logPath + ')');
    for (let i = 0; i < BOTS; i++) {
      const b = await connectBot(PORT, 'Bot' + i, { checkTimeoutInterval: 120000 });
      b.physicsEnabled = false;   // positions come from the server (/tp), nothing falls
      b.ended = false;
      b.on('end', () => { b.ended = true; });
      b._client.on('map_chunk', () => { b.chunks++; });
      b.chunks = 0;
      bots.push(b);
    }
    const op = bots[0];
    for (const b of bots) op.chat(`/gamemode creative ${b.username}`);
    const results = [];
    for (let i = 0; i < WORKERS.length; i++) results.push(await phase(bots, op, WORKERS[i], CENTERS[i % CENTERS.length]));

    console.log(`\nSummary (${MODE}${ICOUNT ? '' : ': compare the rows, absolute times depend on the host'})`);
    console.log('workers |  TPS (min)  | ms/tick | max tick | loop stall avg / max | chunks/s');
    for (const r of results) {
      console.log(`${String(r.workers).padStart(7)} | ${r.tps.toFixed(1).padStart(4)} (${r.tpsMin.toFixed(1).padStart(4)}) | ` +
        `${r.mspt.toFixed(1).padStart(7)} | ${String(r.tickMax).padStart(5)} ms | ` +
        `${r.stallAvg.toFixed(0).padStart(8)} / ${String(r.stallMax).padStart(5)} ms   | ${r.chunksPerS.toFixed(1).padStart(8)}`);
      if (r.connected < bots.length) failed = true;
    }
    if (q.stats.length) console.log('\nlast device status: ' + q.stats[q.stats.length - 1].slice(0, 400));
  } catch (e) {
    console.log('FAILED: ' + e.message);
    failed = true;
  } finally {
    bots.forEach((b) => b.quit());
    await sleep(500);
    await q.stop();
  }
  process.exit(failed ? 1 : 0);
})();
