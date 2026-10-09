'use strict';
// The overworld from y -64 to 319 (1.18+): the chunks below 0 (deepslate, the bedrock
// floor), blocks at both ends kept across a restart, nothing outside, light deep down,
// and the Nether keeping 0..255.
//   SERVER_BIN=.../mcserver node world_depth.js
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { Vec3 } = require('vec3');
const { startServer, connectBot, sleep, waitFor, nextChat, blockLight } = require('./lib');

(async () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'mcdepth-'));
  const img = path.join(tmp, 'world.img');
  const port = 25640 + Math.floor(Math.random() * 100);
  const args = ['--port', String(port), '--seed', '42', '--ops', 'Deep', '--no-mobs', '--file', img, '--size', '512'];
  let srv = startServer(args, { log: process.argv.includes('--log') });
  let bot;
  const say = async (b, c, pattern = /./) => { const r = nextChat(b, pattern, 15000).catch(() => null); b.chat(c); const m = await r; await sleep(300); return m; };
  try {
    await srv.ready;
    bot = await connectBot(port, 'Deep');
    await waitFor(() => bot.blockAt(bot.entity.position.offset(0, -1, 0)), 20000, 'spawn terrain');
    const p = bot.entity.position.floored();
    assert.strictEqual(bot.game.minY, -64, 'the overworld starts at -64');
    assert.strictEqual(bot.game.height, 384, 'and is 384 high');
    // 1. what lies below 0 under the spawn
    let deepslate = 0, bedrock = 0, below = 0;
    for (let dx = 0; dx < 4; dx++)
      for (let y = -64; y < 0; y++) {
        const b = bot.blockAt(new Vec3(p.x + dx, y, p.z));
        assert(b, `block at y ${y} known to the client`);
        below++;
        if (b.name.startsWith('deepslate') || b.name === 'tuff') deepslate++;
        if (b.name === 'bedrock') bedrock++;
      }
    const floor = bot.blockAt(new Vec3(p.x, -64, p.z)).name;
    console.log(`below 0 at spawn: ${deepslate}/${below} deepslate (or its ores, tuff), ${bedrock} bedrock, the floor at -64 is ${floor}`);
    assert.strictEqual(floor, 'bedrock');
    assert(deepslate > below / 3, 'mostly deepslate below 0');
    // 2. both ends of the build height, and nothing beyond them
    for (const [y, ok] of [[-64, true], [319, true], [-65, false], [320, false]]) {
      const m = await say(bot, `/setblock ${p.x + 3} ${y} ${p.z} glowstone`);
      const text = m ? String(m) : '';
      console.log(`  /setblock at y ${y}: ${text}`);
      assert.strictEqual(/out of the world/i.test(text), !ok, `setblock at y ${y}`);
    }
    await waitFor(() => bot.blockAt(new Vec3(p.x + 3, 319, p.z))?.name === 'glowstone', 5000, 'glowstone at 319');
    await waitFor(() => bot.blockAt(new Vec3(p.x + 3, -64, p.z))?.name === 'glowstone', 5000, 'glowstone at -64');
    // 3. light deep down: a torch in a pocket carved at -40
    await say(bot, `/fill ${p.x + 6} -41 ${p.z} ${p.x + 8} -39 ${p.z + 2} air`);
    await say(bot, `/setblock ${p.x + 7} -41 ${p.z + 1} torch`);
    await sleep(1500);
    const lit = blockLight(bot, new Vec3(p.x + 8, -40, p.z + 1));
    console.log(`block light next to a torch at y -41: ${lit}`);
    assert(lit >= 12, 'the torch lights the pocket below 0');
    await say(bot, '/save-all', /save|Saved/i);
    bot.quit();
    bot = null;
    await srv.stop();
    // 4. after a restart
    srv = startServer(args, { log: process.argv.includes('--log') });
    await srv.ready;
    bot = await connectBot(port, 'Deep');
    await waitFor(() => bot.blockAt(new Vec3(p.x + 3, -64, p.z)), 20000, 'the column after the restart');
    const lo = bot.blockAt(new Vec3(p.x + 3, -64, p.z)).name, hi = bot.blockAt(new Vec3(p.x + 3, 319, p.z))?.name;
    const torch = bot.blockAt(new Vec3(p.x + 7, -41, p.z + 1)).name;
    console.log(`after a restart: ${lo} at -64, ${hi} at 319, ${torch} at -41`);
    assert.strictEqual(lo, 'glowstone');
    assert.strictEqual(hi, 'glowstone');
    assert.strictEqual(torch, 'torch');
    // 5. the Nether keeps 0..255
    await say(bot, '/dimension the_nether');
    await waitFor(() => bot.game.dimension === 'the_nether', 20000, 'in the Nether');
    assert.strictEqual(bot.game.minY, 0, 'the Nether starts at 0');
    assert.strictEqual(bot.game.height, 256, 'and is 256 high');
    const q = bot.entity.position.floored();
    const m = await say(bot, `/setblock ${q.x} -5 ${q.z} glowstone`);
    assert(/out of the world/i.test(String(m)), 'nothing below 0 in the Nether');
    console.log('WORLD DEPTH OK');
  } catch (e) {
    console.error('FAILED:', e);
    process.exitCode = 1;
  } finally {
    if (bot) bot.quit();
    if (srv) await srv.stop();
    fs.rmSync(tmp, { recursive: true, force: true });
  }
})();
