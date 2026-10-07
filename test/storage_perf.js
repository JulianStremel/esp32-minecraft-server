'use strict';
// Repeatable native NBD workload. SERVER_BIN selects the before/after executable.
// node test/storage_perf.js OUTPUT.json [service-delay-ms=10] [seconds=30]
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');
const { startServer, connectBot, nextChat, sleep, waitFor, ROOT } = require('./lib');
(async () => {
  const out = process.argv[2];
  if (!out || fs.existsSync(out)) throw Error('Supply a new output JSON path');
  const delay = Number(process.argv[3] || 10), seconds = Number(process.argv[4] || 30);
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'mc-storage-perf-'));
  const nbd = spawn('python3', [path.join(ROOT, 'tools/nbd_server.py'), '--file', path.join(tmp, 'world.img'), '--size', '256M', '--bind', '127.0.0.1', '--port', '10821', '--delay-ms', String(delay)]);
  let nbdLog = ''; nbd.stdout.on('data', d => nbdLog += d); nbd.stderr.on('data', d => nbdLog += d);
  let srv; const bots = [];
  try {
    await waitFor(() => nbdLog.includes('listening'), 5000, 'NBD');
    srv = startServer(['--port', '25621', '--nbd', '127.0.0.1:10821', '--seed', '42', '--workers', '2', '--view', '4', '--creative', '--no-mobs', '--ops', 'Probe']);
    await srv.ready;
    for (let i = 0; i < 6; i++) {
      const b = await connectBot(25621, i ? 'Bot' + i : 'Probe'); b.physicsEnabled = false; bots.push(b);
    }
    await sleep(3000);
    let chunks = 0;
    bots.forEach(b => b._client.on('map_chunk', () => chunks++));
    const samples = [], t0 = Date.now(); let step = 0;
    const move = () => {
      if (Date.now() - t0 >= seconds * 1000) return;
      for (let i = 0; i < bots.length; i++) {
        const a = i * Math.PI / 3, r = 64 + step * 32;
        bots[0].chat(`/tp ${bots[i].username} ${Math.round(120 + Math.cos(a) * r)} 100 ${Math.round(-120 + Math.sin(a) * r)}`);
      }
      step++;
    };
    move();
    const movement = setInterval(move, 2000);
    try {
      while (Date.now() - t0 < seconds * 1000) {
        await sleep(200);
        const t = Date.now(), reply = nextChat(bots[0], /^TPS /, 15000);
        bots[0].chat('/tps');
        const text = await reply;
        samples.push({ atMs: t - t0, latencyMs: Date.now() - t, text });
      }
    } finally { clearInterval(movement); }
    const elapsedMs = Date.now() - t0;
    const sorted = samples.map(s => s.latencyMs).sort((a,b) => a-b);
    const tps = samples.map(s => Number(s.text.match(/^TPS ([\d.]+)/)[1]));
    const stalls = samples.map(s => Number(s.text.match(/max loop stall (\d+)/)[1]));
    const result = { binary: process.env.SERVER_BIN || 'host/build/mcserver', delayMs: delay, seconds, bots: 6, workers: 2, elapsedMs, chunks, chunksPerSecond: chunks * 1000 / elapsedMs, moves: step * 6,
      commandMs: { p50: sorted[Math.floor(sorted.length * .5)], p95: sorted[Math.floor(sorted.length * .95)], max: sorted.at(-1) },
      tps: { min: Math.min(...tps), mean: tps.reduce((a,b)=>a+b,0)/tps.length }, maxReportedLoopMs: Math.max(...stalls), samples };
    fs.writeFileSync(out, JSON.stringify(result, null, 2) + '\n');
    console.log(JSON.stringify({...result, samples: samples.length}, null, 2));
  } finally {
    bots.forEach(b => b.quit()); await sleep(200);
    if (srv) { await srv.stop(); fs.writeFileSync(out + '.server.log', srv.output()); }
    nbd.kill('SIGTERM'); await new Promise(r => nbd.exitCode !== null || nbd.signalCode !== null ? r() : nbd.once('exit', r));
    fs.rmSync(tmp, {recursive:true, force:true});
  }
})().catch(e => {console.error(e); process.exitCode = 1;});
