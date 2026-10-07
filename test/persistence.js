'use strict';
// World + player persistence over NBD: build, quit, restart the server, verify.
const assert = require('assert');
const { spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { Vec3 } = require('vec3');
const { startServer, connectBot, waitFor, nextChat, sleep, ROOT } = require('./lib');

// NBD_IMPL=python (default) | nbdkit | qemu  -- the storage backend is plain NBD,
// so any standard server works.
function startNbd(file, port) {
  const impl = process.env.NBD_IMPL || 'python';
  if (impl !== 'python') fs.writeFileSync(file, ''), fs.truncateSync(file, 256 * 1048576);
  const cmd = {
    python: ['python3', [path.join(ROOT, 'tools', 'nbd_server.py'), '--file', file, '--size', '256M', '--port', String(port), '--bind', '127.0.0.1']],
    nbdkit: ['nbdkit', ['-f', '-v', '-i', '127.0.0.1', '-p', String(port), 'file', file]],
    qemu: ['qemu-nbd', ['-t', '-f', 'raw', '-b', '127.0.0.1', '-p', String(port), file]],
  }[impl];
  console.log('NBD server:', impl);
  const p = spawn(cmd[0], cmd[1]);
  let out = '';
  p.stdout.on('data', (d) => { out += d; });
  p.stderr.on('data', (d) => { out += d; });
  return {
    proc: p,
    ready: impl === 'python' ? waitFor(() => out.includes('listening'), 10000, 'nbd server')
      : waitFor(() => new Promise((res) => { const s = require('net').connect(port, '127.0.0.1', () => { s.destroy(); res(true); }); s.on('error', () => res(false)); }), 10000, 'nbd server'),
    stop: () => new Promise((r) => { p.on('exit', r); p.kill('SIGTERM'); }),
    output: () => out,
  };
}

(async () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'mcnbd-'));
  const img = path.join(tmp, 'world.img');
  const nbdPort = 10811, port = 25602;
  const nbd = startNbd(img, nbdPort);
  await nbd.ready;
  const args = ['--port', String(port), '--nbd', `127.0.0.1:${nbdPort}`, '--seed', '777', '--ops', 'Builder', '--creative', '--view', '3', '--no-mobs'];
  let srv = startServer(args, { log: !!process.env.LOG });
  await srv.ready;
  let bot;
  let placedAt, chestAt;
  try {
    bot = await connectBot(port, 'Builder');
    await waitFor(() => bot.blockAt(bot.entity.position.offset(0, -1, 0)), 10000, 'ground');
    const feet = bot.entity.position.floored();
    // 1) /setblock
    bot.chat(`/setblock ${feet.x + 2} ${feet.y + 3} ${feet.z} gold_block`);
    await waitFor(() => { const b = bot.blockAt(new Vec3(feet.x + 2, feet.y + 3, feet.z)); return b && b.name === 'gold_block'; }, 5000, 'gold block');
    // 2) real placement with an item
    await bot.creative.setInventorySlot(36, new (require('prismarine-item')('1.16.5'))(bot.registry.itemsByName.diamond_block.id, 1));
    await sleep(200);
    bot.setQuickBarSlot(0);
    const ground = bot.blockAt(feet.offset(1, -1, 1));
    await bot.placeBlock(ground, new Vec3(0, 1, 0));
    placedAt = feet.offset(1, 0, 1);
    await waitFor(() => bot.blockAt(placedAt).name === 'diamond_block', 5000, 'placed diamond block');
    // 3) a chest with items
    chestAt = feet.offset(-2, 0, 0);
    bot.chat(`/setblock ${chestAt.x} ${chestAt.y} ${chestAt.z} chest`);
    await waitFor(() => bot.blockAt(chestAt).name === 'chest', 5000, 'chest');
    bot.chat('/give Builder emerald 23');
    await waitFor(() => bot.inventory.items().find((i) => i.name === 'emerald'), 5000, 'emeralds');
    const chest = await bot.openContainer(bot.blockAt(chestAt));
    await chest.deposit(bot.registry.itemsByName.emerald.id, null, 10);
    chest.close();
    await sleep(300);
    // 4) move a bit so the position is saved
    bot.chat(`/tp Builder ${feet.x + 0.5} ${feet.y + 5} ${feet.z + 0.5}`);
    await sleep(1000);
    const posBefore = bot.entity.position.clone();
    const invBefore = bot.inventory.items().map((i) => `${i.name}x${i.count}`).sort().join(',');
    console.log('before restart: pos', posBefore.toString(), 'inventory', invBefore);
    bot.quit();
    await sleep(500);
    await srv.stop();
    console.log('server stopped; restarting on the same NBD export');

    srv = startServer(args, { log: !!process.env.LOG });
    await srv.ready;
    assert(srv.output().includes('world: loaded seed=777'), 'world meta loaded from NBD');
    bot = await connectBot(port, 'Builder');
    await waitFor(() => bot.blockAt(placedAt), 10000, 'chunks after restart');
    const posAfter = bot.entity.position;
    console.log('after restart: pos', posAfter.toString());
    assert(posAfter.distanceTo(posBefore) < 1.5, 'position restored');
    assert.strictEqual(bot.blockAt(placedAt).name, 'diamond_block', 'placed block persisted');
    assert.strictEqual(bot.blockAt(new Vec3(feet.x + 2, feet.y + 3, feet.z)).name, 'gold_block', 'setblock persisted');
    await sleep(300);
    const invAfter = bot.inventory.items().map((i) => `${i.name}x${i.count}`).sort().join(',');
    console.log('inventory after restart', invAfter);
    assert.strictEqual(invAfter, invBefore, 'inventory restored');
    const chest2 = await bot.openContainer(bot.blockAt(chestAt));
    const emeralds = chest2.containerItems().filter((i) => i.name === 'emerald').reduce((a, i) => a + i.count, 0);
    chest2.close();
    console.log('emeralds in chest after restart:', emeralds);
    assert.strictEqual(emeralds, 10, 'chest contents persisted');
    const stat = nextChat(bot, /Storage:/);
    bot.chat('/storage');
    console.log((await stat).slice(0, 220));
    const du = fs.statSync(img);
    console.log(`image: ${(du.size / 1048576).toFixed(0)} MiB apparent, ${(du.blocks * 512 / 1024).toFixed(0)} KiB actually used (sparse)`);
    console.log('PERSISTENCE OK');
  } finally {
    if (bot) bot.quit();
    await sleep(300);
    await srv.stop();
    await nbd.stop();
    fs.rmSync(tmp, { recursive: true, force: true });
  }
})().catch((e) => { console.error('FAILED:', e); process.exit(1); });
