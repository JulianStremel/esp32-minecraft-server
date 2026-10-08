'use strict';
// Chunk generation and job queue stress test on a real board: 8 players in spectator
// mode fly away from a fresh spot in 8 directions, so every chunk entering their views
// has to be generated, lit, compressed and sent while the queue keeps cancelling what
// they left behind. An operator ("Tester") samples /tps and /workers every 2 s.
//
//   node hardware_stress.js --host 192.168.1.160 [--flyers 8] [--seconds 120] [--speed 11]
//       [--view 32] [--center x,z] [--json out.json] [--serial COM5 --python <idf python>] [--log]
//
// --speed is in blocks/s (spectator flight: about 11, sprint-flying about 22).
// --view is the flyers' client view distance (the server caps it at MC_VIEW_DISTANCE).
// --speed 0 keeps them hovering (an idle load). --json writes the summary for perf_suite.js.
// The firmware needs MC_MAX_ONLINE >= flyers + 1 and Tester as an operator. The flyers
// are raw protocol clients that only count chunks: mineflayer would keep every chunk
// of a view of 32 in memory, 8 times over.
const assert = require('assert');
const fs = require('fs');
const { spawn } = require('child_process');
const path = require('path');
const mc = require('minecraft-protocol');
const { connectBot, nextChat, sleep, waitFor, ROOT } = require('./lib');

const args = process.argv.slice(2);
const opt = (name, fallback) => {
  const i = args.indexOf('--' + name);
  return i < 0 ? fallback : args[i + 1];
};
const host = opt('host');
const port = Number(opt('port', '25565'));
const FLYERS = Number(opt('flyers', '8'));
const SECONDS = Number(opt('seconds', '120'));
const SPEED = Number(opt('speed', '11'));
const VIEW = Number(opt('view', '32'));
const Y = 150;
const serialPort = opt('serial');
const python = opt('python', process.env.IDF_PYTHON || 'python');
if (!host) { console.error('usage: node hardware_stress.js --host <board ip> [--flyers 8] [--seconds 120]'); process.exit(2); }
// a fresh area each run: the world is far larger than anything explored so far
const center = opt('center')
  ? opt('center').split(',').map(Number)
  : [Math.round((Math.random() * 2 - 1) * 40000), Math.round((Math.random() * 2 - 1) * 40000)];

function chatText(json) {
  try {
    const walk = (n) => (typeof n === 'string' ? n : (n.text || '') + (n.extra || []).map(walk).join(''));
    return walk(JSON.parse(json));
  } catch (e) {
    return String(json);
  }
}

// A flying client: confirms teleports, sends its position every tick and counts chunks.
function startFlyer(name) {
  return new Promise((resolve, reject) => {
    const client = mc.createClient({ host, port, username: name, version: '1.16.5', auth: 'offline' });
    const f = { name, client, pos: null, dir: [0, 0], flying: false, chunks: new Set(), received: 0, unloaded: 0,
      samples: 0, missing: 0, kicked: null, ended: false };
    const t = setTimeout(() => reject(new Error(name + ': spawn timeout')), 30000);
    client.on('login', () => {
      client.write('settings', { locale: 'en_US', viewDistance: VIEW, chatFlags: 0, chatColors: true, skinParts: 127,
        mainHand: 1 });
    });
    client.on('position', (p) => {
      const rel = (bit, v, cur) => (p.flags & bit ? cur + v : v);
      f.pos = f.pos ? [rel(1, p.x, f.pos[0]), rel(2, p.y, f.pos[1]), rel(4, p.z, f.pos[2])] : [p.x, p.y, p.z];
      client.write('teleport_confirm', { teleportId: p.teleportId });
      client.write('position', { x: f.pos[0], y: f.pos[1], z: f.pos[2], onGround: false });
      clearTimeout(t);
      resolve(f);
    });
    client.on('map_chunk', (p) => { f.chunks.add(p.x + ',' + p.z); f.received++; });
    client.on('unload_chunk', (p) => { f.chunks.delete(p.chunkX + ',' + p.chunkZ); f.unloaded++; });
    client.on('kick_disconnect', (p) => { f.kicked = chatText(p.reason); });
    client.on('disconnect', (p) => { f.kicked = chatText(p.reason); });
    client.on('end', () => { f.ended = true; });
    client.on('error', (e) => { f.kicked = f.kicked || String(e); });
  });
}

// Serial console of the board (only watched for faults here).
function startMonitor() {
  const proc = spawn(python, [path.join(ROOT, 'tools/idf/serial_monitor.py'), serialPort], { stdio: ['pipe', 'pipe', 'pipe'] });
  const m = { proc, failure: null, warnings: 0 };
  let buf = '';
  const onData = (d) => {
    buf += d.toString();
    let nl;
    while ((nl = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, nl).replace(/\r$/, '');
      buf = buf.slice(nl + 1);
      if (/Guru Meditation|assert failed|\[FATAL\]|^Backtrace:|abort\(\)/.test(line)) {
        m.failure = new Error('firmware fault: ' + line);
        console.log('  device: ' + line);
      }
      if (/ W /.test(line)) m.warnings++;
      if (args.includes('--log')) console.log('  | ' + line);
    }
  };
  proc.stdout.on('data', onData);
  proc.stderr.on('data', onData);
  m.stop = () => new Promise((res) => {
    if (proc.exitCode !== null || proc.signalCode !== null) return res();
    proc.once('exit', () => res());
    proc.kill();
  });
  return m;
}

const num = (re, s) => { const m = re.exec(s); return m ? Number(m[1]) : NaN; };

function parseSample(tps, jobs) {
  const queued = /queued (\d+)\/(\d+)\/(\d+)\/(\d+)/.exec(jobs) || [];
  const wait = /max wait (\d+)\/(\d+)\/(\d+)\/(\d+) ms/.exec(jobs) || [];
  const busy = (/workers busy((?: \d+%)+)/.exec(jobs) || [, ''])[1].trim().split(' ').filter(Boolean).map((x) => parseInt(x));
  return {
    tps: num(/TPS ([0-9.]+)/, tps), mspt: num(/([0-9.]+) ms\/tick/, tps), tickMax: num(/\(max (\d+)\)/, tps),
    stall: num(/max loop stall (\d+) ms/, tps), overruns: num(/(\d+) overruns/, tps), skipped: num(/(\d+) skipped/, tps),
    heap: num(/heap (\d+) KB/, tps), resident: num(/, (\d+) chunks \(/, tps),
    busy: busy.length ? busy.reduce((a, x) => a + x, 0) / busy.length : NaN,
    queued: queued.slice(1).map(Number), wait: wait.slice(1).map(Number),
    generated: num(/generated (\d+)/, jobs), sent: num(/sent (\d+)/, jobs), cancelled: num(/cancelled (\d+)/, jobs),
    promoted: num(/promoted (\d+)/, jobs),
  };
}

let op, monitor;
const flyers = [];

async function command(text, pattern, ms = 15000) {
  const r = nextChat(op, pattern, ms);
  op.chat(text);
  return r;
}

(async () => {
  try {
    if (serialPort) monitor = startMonitor();
    op = await connectBot(port, 'Tester', { host, viewDistance: 'tiny', checkTimeoutInterval: 600000 });
    op.physicsEnabled = false;
    // other players skew the numbers: record them
    const list = await command('/list', /players online/);
    const othersOnline = Math.max(0, Number((/There are (\d+)/.exec(list) || [0, 1])[1]) - 1);
    if (othersOnline) console.log(`note: ${othersOnline} other player(s) online`);
    console.log(`center ${center[0]}, ${center[1]}: ${FLYERS} flyers at ${SPEED} blocks/s, view ${VIEW}, ${SECONDS} s`);
    for (let i = 0; i < FLYERS; i++) flyers.push(await startFlyer('Flyer' + i));
    // spectators, spread on a small circle around a fresh spot, each facing outwards
    for (let i = 0; i < FLYERS; i++) {
      const f = flyers[i];
      const a = (i / FLYERS) * Math.PI * 2;
      f.dir = [Math.cos(a), Math.sin(a)];
      await command(`/gamemode spectator ${f.name}`, /game mode/i);
      await sleep(650);   // vanilla chat rate limit
      const x = center[0] + f.dir[0] * 24, z = center[1] + f.dir[1] * 24;
      await command(`/tp ${f.name} ${x.toFixed(1)} ${Y} ${z.toFixed(1)}`, /Teleported/);
      await waitFor(() => Math.abs(f.pos[0] - x) < 1 && Math.abs(f.pos[2] - z) < 1, 10000, f.name + ' teleport');
      await sleep(650);
    }
    // the operator watches from the centre (its own view is tiny)
    await command(`/tp Tester ${center[0]} ${Y} ${center[1]}`, /Teleported/);
    await sleep(650);
    const base = parseSample(await command('/tps', /^TPS/), await (sleep(650).then(() => command('/workers', /^Jobs:/))));
    flyers.forEach((f) => { f.received = 0; f.flying = true; });

    // flight: one position packet per tick and flyer
    const stepBlocks = SPEED / 20;
    const fly = setInterval(() => {
      for (const f of flyers) {
        if (!f.flying || f.ended) continue;
        f.pos[0] += f.dir[0] * stepBlocks;
        f.pos[2] += f.dir[1] * stepBlocks;
        f.client.write('position', { x: f.pos[0], y: Y, z: f.pos[2], onGround: false });
        // is the chunk the flyer is in already there?
        f.samples++;
        if (!f.chunks.has((Math.floor(f.pos[0]) >> 4) + ',' + (Math.floor(f.pos[2]) >> 4))) f.missing++;
      }
    }, 50);

    const samples = [];
    const t0 = Date.now();
    let unanswered = 0;
    while (Date.now() - t0 < SECONDS * 1000) {
      await sleep(2000);
      let tps, jobs;
      try {
        tps = await command('/tps', /^TPS/, 10000);
        await sleep(650);
        jobs = await command('/workers', /^Jobs:/, 10000);
      } catch (e) {
        unanswered++;
        console.log(`${Math.round((Date.now() - t0) / 1000)}s  no reply: ${e.message}`);
        continue;
      }
      const s = parseSample(tps, jobs);
      s.t = (Date.now() - t0) / 1000;
      s.received = flyers.reduce((a, f) => a + f.received, 0);
      samples.push(s);
      console.log(`${s.t.toFixed(0).padStart(4)}s  TPS ${s.tps.toFixed(1)}  ${s.mspt.toFixed(1)} ms/tick (max ${s.tickMax})  ` +
        `stall ${s.stall} ms  heap ${s.heap} KB  resident ${s.resident}  busy ${s.busy.toFixed(0)}%  ` +
        `queued ${s.queued.join('/')}  wait ${s.wait.join('/')} ms  ` +
        `generated +${s.generated - base.generated}  received ${s.received}  cancelled +${s.cancelled - base.cancelled}`);
      assert(!monitor?.failure, 'firmware fault');
      const gone = flyers.filter((f) => f.ended || f.kicked);
      if (gone.length) throw new Error('flyers disconnected: ' + gone.map((f) => `${f.name} (${f.kicked || 'closed'})`).join(', '));
    }
    clearInterval(fly);

    const last = samples[samples.length - 1];
    const secs = last.t;
    const avg = (k) => samples.reduce((a, s) => a + s[k], 0) / samples.length;
    const max = (k) => Math.max(...samples.map((s) => s[k]));
    const min = (k) => Math.min(...samples.map((s) => s[k]));
    const missing = flyers.reduce((a, f) => a + f.missing, 0) / flyers.reduce((a, f) => a + f.samples, 0);
    const flown = SPEED * secs;
    console.log('\nSummary');
    console.log(`  TPS avg ${avg('tps').toFixed(1)}, min ${min('tps').toFixed(1)}; ms/tick avg ${avg('mspt').toFixed(1)}, ` +
      `longest tick ${max('tickMax')} ms, longest loop stall ${max('stall')} ms; overruns ${samples.reduce((a, s) => a + s.overruns, 0)}, ` +
      `skipped ticks ${samples.reduce((a, s) => a + s.skipped, 0)}`);
    console.log(`  heap min ${min('heap')} KB, resident chunks max ${max('resident')}, workers busy avg ${avg('busy').toFixed(0)}%`);
    console.log(`  generated ${last.generated - base.generated} chunks (${((last.generated - base.generated) / secs).toFixed(1)}/s), ` +
      `sent ${last.sent - base.sent}, received ${last.received} by the flyers, cancelled ${last.cancelled - base.cancelled}, ` +
      `promoted ${last.promoted - base.promoted}`);
    console.log(`  queue max ${[0, 1, 2, 3].map((k) => Math.max(...samples.map((s) => s.queued[k] || 0))).join('/')}, ` +
      `longest wait ${[0, 1, 2, 3].map((k) => Math.max(...samples.map((s) => s.wait[k] || 0))).join('/')} ms (urgent/high/normal/low)`);
    console.log(`  each flyer flew ${flown.toFixed(0)} blocks (${(flown / 16).toFixed(0)} chunks); its own chunk was missing ` +
      `${(missing * 100).toFixed(1)}% of the time`);
    if (monitor) console.log(`  device warnings on the serial console: ${monitor.warnings}`);
    if (opt('json')) {
      // one run's numbers for test/perf_suite.js and tools/perf_compare.js
      const generated = last.generated - base.generated;
      fs.writeFileSync(opt('json'), JSON.stringify({
        flyers: FLYERS, speed: SPEED, view: VIEW, seconds: secs, center, othersOnline,
        tpsAvg: avg('tps'), tpsMin: min('tps'), msptAvg: avg('mspt'), tickMax: max('tickMax'), stallMax: max('stall'),
        overruns: samples.reduce((a, s) => a + s.overruns, 0), skipped: samples.reduce((a, s) => a + s.skipped, 0),
        heapMinKb: min('heap'), residentMax: max('resident'), busyAvg: avg('busy'),
        generatedPerS: generated / secs, sentPerS: (last.sent - base.sent) / secs, receivedPerS: last.received / secs,
        cancelled: last.cancelled - base.cancelled,
        queueMax: [0, 1, 2, 3].map((k) => Math.max(...samples.map((s) => s.queued[k] || 0))),
        waitMaxMs: [0, 1, 2, 3].map((k) => Math.max(...samples.map((s) => s.wait[k] || 0))),
        ownChunkMissing: missing, unanswered,
      }, null, 1));
    }
    // pass: the server kept running and answering, nobody was dropped, memory held
    assert(unanswered <= 1, `${unanswered} status commands were not answered`);
    assert(min('heap') > 1024, 'free heap fell below 1 MB');
    console.log(`HARDWARE STRESS OK (${host}): ${FLYERS} flyers, ${secs.toFixed(0)} s`);
  } finally {
    for (const f of flyers) { f.flying = false; try { f.client.end('done'); } catch (e) { /* closed */ } }
    if (op) {
      op.on('error', () => {});   // the board may reset the closing connection
      op.quit();
    }
    if (monitor) await monitor.stop();
  }
})().catch((err) => { console.error(err); process.exitCode = 1; });
