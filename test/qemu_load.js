'use strict';
// Load test of the real firmware on the emulated ESP32-S3 (tools/qemu/run.sh):
// virtual players are moved through untouched terrain, so every chunk they need
// has to be generated, lit, compressed and sent by the device. The test runs once
// per worker-pool size (switched at runtime with /workers) and reports TPS, tick
// times, the longest game-loop stall and the chunk throughput.
// One extra "probe" player keeps jumping to fresh spots while the others keep the
// workers busy; the time until the ground under it arrives shows how well urgent work
// gets past the backlog.
//
//   node test/qemu_load.js [--bots 6] [--seconds 30] [--workers 0,2] [--step 3]
//                          [--mttcg | --icount N] [--no-build] [--port 25580] [--env NAME]
//
// --mttcg (default): one host thread per emulated core, so the two cores really run
//   in parallel; times follow the host CPU (compare the phases, not absolute values).
// --icount N: instruction-counted, deterministic timing (2^N ns per instruction), but
//   both emulated cores share one clock: it cannot show a parallel speed-up.
const { connectBot, sleep, nextChat } = require('./lib');
const { startQemu } = require('./qemu');

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
const PROBE_S = parseFloat(opt('probe', '6'));         // seconds between probe jumps (0 = no probe)
const WORKERS = opt('workers', '0,2').split(',').map((s) => parseInt(s));
const PORT = parseInt(opt('port', '25580'));
const NBD_PORT = PORT + 1000;
const ICOUNT = opt('icount', null);
const MODE = ICOUNT ? `icount shift=${ICOUNT}` : 'MTTCG';
// fresh terrain for every phase (the 512 MiB world image allows +-496 blocks)
const CENTERS = [[-300, -300], [300, 300], [-300, 300], [300, -300]];

function parseTps(line) {
  const m = /TPS ([0-9.]+), ([0-9.]+) ms\/tick \(max (\d+)\), max loop stall (\d+) ms/.exec(line);
  if (!m) return null;
  const r = { tps: +m[1], mspt: +m[2], tickMax: +m[3], stall: +m[4], wakeups: NaN, overruns: 0, late: 0, skipped: 0,
    busy: NaN };
  // newer firmware: game-loop wakeups and tick overruns in the last 2 s
  const w = /([0-9.]+) wakeups\/s, (\d+) overruns \((\d+) ticks late, (\d+) skipped\)/.exec(line);
  if (w) Object.assign(r, { wakeups: +w[1], overruns: +w[2], late: +w[3], skipped: +w[4] });
  const b = /workers busy((?: \d+%)+)/.exec(line);
  if (b) {
    const v = b[1].trim().split(' ').map((x) => parseInt(x));
    r.busy = v.reduce((a, x) => a + x, 0) / v.length;
  }
  return r;
}

async function command(op, cmd, reply, ms = 15000) {
  const r = nextChat(op, reply, ms);
  op.chat(cmd);
  return r;
}

// The probe waits for map_chunk packets of the 3x3 chunks around its target spot.
function probeJump(op, probe, x, z) {
  const cx = Math.floor(x) >> 4, cz = Math.floor(z) >> 4;
  const want = new Set();
  for (let dz = -1; dz <= 1; dz++) for (let dx = -1; dx <= 1; dx++) want.add(`${cx + dx},${cz + dz}`);
  const t0 = Date.now();
  const res = { center: null, ring: null };
  const onChunk = (p) => {
    const k = `${p.x},${p.z}`;
    if (!want.has(k)) return;
    want.delete(k);
    if (p.x === cx && p.z === cz) res.center = Date.now() - t0;
    if (want.size === 0) {
      res.ring = Date.now() - t0;
      probe._client.removeListener('map_chunk', onChunk);
    }
  };
  probe._client.on('map_chunk', onChunk);
  op.chat(`/tp ${probe.username} ${x.toFixed(1)} 140 ${z.toFixed(1)}`);
  res.cancel = () => probe._client.removeListener('map_chunk', onChunk);
  return res;
}

async function phase(bots, op, workers, center) {
  const reply = await command(op, `/workers ${workers}`, /^Jobs:/, 60000);
  console.log(`\n== ${workers} worker${workers === 1 ? '' : 's'}: ${reply.slice(6, 120)}`);
  const probe = PROBE_S > 0 ? bots[bots.length - 1] : null;
  const walkers = probe ? bots.slice(0, -1) : bots;
  // spread the players around a fresh area
  walkers.forEach((b, i) => {
    const a = (i / walkers.length) * Math.PI * 2;
    b.dir = [Math.cos(a), Math.sin(a)];
    b.pos = [center[0] + b.dir[0] * 40, center[1] + b.dir[1] * 40];
    op.chat(`/tp ${b.username} ${b.pos[0].toFixed(1)} 140 ${b.pos[1].toFixed(1)}`);
    b.chunks = 0;
  });
  if (probe) op.chat(`/tp ${probe.username} ${center[0]} 140 ${center[1]}`);
  // switching the pool drains it (a stall of its own): let the 2 s measuring window roll over
  await sleep(2500);
  bots.forEach((b) => { b.chunks = 0; });
  const samples = [];
  const probes = [];
  let worst = null;
  const t0 = Date.now();
  let nextMove = t0 + STEP_S * 1000;
  let nextSample = t0 + 2000;
  let nextProbe = t0 + 1500;
  let probeIdx = 0;
  while (Date.now() - t0 < SECONDS * 1000) {
    if (probe && Date.now() >= nextProbe) {
      // spots between the walkers' paths, outside their view (fresh terrain every time)
      nextProbe += PROBE_S * 1000;
      const a = ((probeIdx + 0.5) / walkers.length) * Math.PI * 2;
      const r = 200 + 40 * Math.floor(probeIdx / walkers.length);
      probes.push(probeJump(op, probe, center[0] + Math.cos(a) * r, center[1] + Math.sin(a) * r));
      probeIdx++;
    }
    if (Date.now() >= nextMove) {
      nextMove += STEP_S * 1000;
      for (const b of walkers) {
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
        samples.push({ tps: 0, mspt: 0, tickMax: 0, stall: 10000, wakeups: NaN, overruns: 0, late: 0, skipped: 0,
          busy: NaN, timeout: true });
      }
      try {
        const lag = await command(op, '/lag', /^Slowest loop/, 5000);
        const m = /^Slowest loop: (\d+) ms/.exec(lag);
        if (m && (!worst || +m[1] > worst.ms)) worst = { ms: +m[1], text: lag.slice(14) };
      } catch (e) { /* older firmware without /lag */ }
    }
    await sleep(100);
  }
  await sleep(3000);  // let the last probe finish
  probes.forEach((p) => p.cancel());
  const secs = (Date.now() - t0) / 1000;
  const chunks = bots.reduce((n, b) => n + b.chunks, 0);
  const done = (k) => probes.map((p) => p[k]).filter((v) => v !== null);
  const pavg = (k) => { const v = done(k); return v.length ? v.reduce((a, b) => a + b, 0) / v.length : NaN; };
  const pmax = (k) => done(k).reduce((a, b) => Math.max(a, b), 0);
  const avg = (k) => samples.reduce((n, s) => n + s[k], 0) / Math.max(1, samples.length);
  const avgDef = (k) => { const v = samples.map((s) => s[k]).filter((x) => !isNaN(x)); return v.length ? v.reduce((a, b) => a + b, 0) / v.length : NaN; };
  const sum = (k) => samples.reduce((n, s) => n + s[k], 0);
  const max = (k) => samples.reduce((n, s) => Math.max(n, s[k]), 0);
  const min = (k) => samples.reduce((n, s) => Math.min(n, s[k]), Infinity);
  const r = {
    workers, samples: samples.length, tps: avg('tps'), tpsMin: min('tps'), mspt: avg('mspt'), tickMax: max('tickMax'),
    stallAvg: avg('stall'), stallMax: max('stall'), chunksPerS: chunks / secs,
    probes: probes.length, probeMissed: probes.filter((p) => p.ring === null).length,
    groundAvg: pavg('center'), groundMax: pmax('center'), ringAvg: pavg('ring'), ringMax: pmax('ring'),
    connected: bots.filter((b) => b._client.state === 'play' && !b.ended).length,
    wakeups: avgDef('wakeups'), busy: avgDef('busy'), overruns: sum('overruns'), late: sum('late'), skipped: sum('skipped'),
  };
  console.log(`   TPS ${r.tps.toFixed(1)} (min ${r.tpsMin.toFixed(1)}), ${r.mspt.toFixed(1)} ms/tick (max ${r.tickMax}), ` +
    `loop stall avg ${r.stallAvg.toFixed(0)} ms / max ${r.stallMax} ms, ${r.chunksPerS.toFixed(1)} chunks/s delivered, ` +
    `${r.connected}/${bots.length} players connected`);
  console.log(`   game loop: ${isNaN(r.wakeups) ? '-' : r.wakeups.toFixed(0)} wakeups/s, workers ${isNaN(r.busy) ? '-' : r.busy.toFixed(0)}% busy, ` +
    `${r.overruns} tick overruns (${r.late} ticks late, ${r.skipped} skipped) in the 2 s windows sampled`);
  if (probe) {
    console.log(`   probe: ${r.probes} jumps into fresh terrain, chunk under it after ${r.groundAvg.toFixed(0)} ms avg / ` +
      `${r.groundMax} ms max, all 3x3 after ${r.ringAvg.toFixed(0)} ms avg / ${r.ringMax} ms max` +
      (r.probeMissed ? ` (${r.probeMissed} incomplete)` : ''));
  }
  if (worst) console.log(`   slowest loop seen: ${worst.text}`);
  return r;
}

(async () => {
  console.log(`QEMU load test: ${BOTS} players, ${SECONDS} s per phase, workers ${WORKERS.join(' / ')}, ${MODE}`);
  const q = startQemu({ port: PORT, nbdPort: NBD_PORT, icount: ICOUNT, mttcg: !ICOUNT, build: !flag('no-build'),
    env: opt('env', null) });
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
    console.log('workers |  TPS (min)  | ms/tick | max tick | loop stall avg / max | chunks/s | probe ground avg / max | wakeups/s | busy | overruns (late/skipped)');
    for (const r of results) {
      console.log(`${String(r.workers).padStart(7)} | ${r.tps.toFixed(1).padStart(4)} (${r.tpsMin.toFixed(1).padStart(4)}) | ` +
        `${r.mspt.toFixed(1).padStart(7)} | ${String(r.tickMax).padStart(5)} ms | ` +
        `${r.stallAvg.toFixed(0).padStart(8)} / ${String(r.stallMax).padStart(5)} ms   | ${r.chunksPerS.toFixed(1).padStart(8)} | ` +
        `${isNaN(r.groundAvg) ? '     -' : r.groundAvg.toFixed(0).padStart(6)} / ${String(r.groundMax).padStart(5)} ms   | ` +
        `${isNaN(r.wakeups) ? '        -' : r.wakeups.toFixed(0).padStart(9)} | ${isNaN(r.busy) ? '   -' : (r.busy.toFixed(0) + '%').padStart(4)} | ` +
        `${r.overruns} (${r.late}/${r.skipped})`);
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
