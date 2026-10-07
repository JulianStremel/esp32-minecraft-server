'use strict';
// A stalled/restarted NBD server must not stall clients or apply a stale login.
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const net = require('net');
const { spawn } = require('child_process');
const { Vec3 } = require('vec3');
const { ROOT, startServer, connectBot, nextChat, sleep, waitFor } = require('./lib');
const port = 25623, nbdPort = 10823;
const vi = n => { const a = []; do { let b = n & 127; n >>>= 7; a.push(n ? b | 128 : b); } while(n); return Buffer.from(a); };
const str = s => Buffer.concat([vi(Buffer.byteLength(s)), Buffer.from(s)]);
const frame = b => Buffer.concat([vi(b.length), b]);
(async () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'mc-storage-io-'));
  const file = path.join(tmp, 'world.img');
  let nbd, srv, probe, replacement, pending;
  async function startNbd() {
    nbd = spawn('python3', [path.join(ROOT, 'tools/nbd_server.py'), '--file', file, '--size', '256M', '--bind', '127.0.0.1', '--port', String(nbdPort)]);
    let log = ''; nbd.stdout.on('data', d => log += d); nbd.stderr.on('data', d => log += d);
    await waitFor(() => log.includes('listening'), 5000, 'NBD start');
  }
  async function stopNbd() {
    if (!nbd || nbd.exitCode !== null || nbd.signalCode !== null) return;
    const p = nbd; p.kill('SIGCONT'); p.kill('SIGTERM');
    await new Promise(r => p.once('exit', r));
  }
  async function command(text, match, timeout = 10000) {
    const response = nextChat(probe, match, timeout); probe.chat(text); return response;
  }
  try {
    await startNbd();
    srv = startServer(['--port', String(port), '--nbd', `127.0.0.1:${nbdPort}`, '--seed', '42', '--workers', '2', '--creative', '--no-mobs', '--view', '3', '--ops', 'Probe']);
    await srv.ready;
    probe = await connectBot(port, 'Probe'); probe.physicsEnabled = false;
    await waitFor(() => probe.blockAt(probe.entity.position.offset(0,-1,0)), 10000, 'terrain');
    await sleep(1000);
    nbd.kill('SIGSTOP');
    // Send a valid login, then abandon its connection while storage is blocked.
    pending = net.connect(port, '127.0.0.1');
    await new Promise((r,j) => { pending.once('connect',r); pending.once('error',j); });
    const pbuf = Buffer.alloc(2); pbuf.writeUInt16BE(port);
    pending.write(Buffer.concat([frame(Buffer.concat([vi(0),vi(754),str('localhost'),pbuf,vi(2)])),frame(Buffer.concat([vi(0),str('Abandoned')]))]));
    await sleep(150); pending.destroy(); await sleep(100);
    // Reuse a player slot before the old storage completion arrives.
    const joining = connectBot(port, 'Replacement');
    const observed = joining.then(b => ({bot:b}), error => ({error}));
    for (let i = 0; i < 4; i++) {
      const t = Date.now(); await command('/tps', /^TPS /, 1500);
      assert(Date.now() - t < 1000, 'game loop blocked behind NBD');
      await sleep(200);
    }
    nbd.kill('SIGCONT');
    const result = await observed;
    if (result.error) throw result.error;
    replacement = result.bot; replacement.physicsEnabled = false;
    assert(!srv.output().includes('Abandoned joined'), 'stale login completion joined a disconnected player');
    console.log('stalled NBD: observer responsive; abandoned login discarded; replacement joined');

    const block = probe.entity.position.floored().offset(2,3,0);
    await command(`/setblock ${block.x} ${block.y} ${block.z} gold_block`, /block/i);
    await waitFor(() => probe.blockAt(block)?.name === 'gold_block', 5000, 'edit');
    await stopNbd();
    const failed = await command('/save-all', /Save failed|Saved the game/, 15000);
    assert(/Save failed/.test(failed), 'unacknowledged writes reported as saved');
    await command('/tps', /^TPS /, 1500);
    await startNbd(); await sleep(2500); // allow the existing reconnect backoff to expire
    assert(/Saved the game/.test(await command('/save-all', /Save failed|Saved the game/, 15000)), 'retry after reconnect failed');
    replacement.quit(); probe.quit(); await sleep(200); await srv.stop();
    srv = startServer(['--port', String(port), '--nbd', `127.0.0.1:${nbdPort}`, '--workers', '2', '--creative', '--no-mobs', '--view', '3', '--ops', 'Probe']);
    await srv.ready;
    probe = await connectBot(port, 'Probe'); probe.physicsEnabled = false;
    await waitFor(() => probe.blockAt(new Vec3(block.x,block.y,block.z))?.name === 'gold_block',10000,'retried edit persisted');
    console.log('STORAGE IO OK: delayed I/O, stale session, failed save, reconnect, durable retry');
  } finally {
    if (pending) pending.destroy();
    if (replacement) replacement.quit(); if (probe) probe.quit();
    if (nbd) nbd.kill('SIGCONT');
    if (srv) { await srv.stop(); if (process.env.LOG) console.log(srv.output()); }
    await stopNbd(); fs.rmSync(tmp,{recursive:true,force:true});
  }
})().catch(e => { console.error(e); process.exitCode = 1; });
