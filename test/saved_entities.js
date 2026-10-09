'use strict';
// Mobs and dropped items are kept with their chunk: across a restart, while nobody is near
// (stashed), and when their chunk leaves memory; never twice.
//   SERVER_BIN=.../mcserver node saved_entities.js
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { Vec3 } = require('vec3');
const { startServer, connectBot, sleep, waitFor, nextChat } = require('./lib');

(async () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'mcents-'));
  const port = 25640 + Math.floor(Math.random() * 100);
  const args = ['--port', String(port), '--seed', '42', '--ops', 'Keeper', '--no-mobs', '--view', '6', '--file', path.join(tmp, 'world.img'), '--size', '512'];
  let srv = startServer(args, { log: process.argv.includes('--log') });
  let bot;
  try {
    await srv.ready;
    bot = await connectBot(port, 'Keeper');
    const say = async (c, re = /./) => { const r = nextChat(bot, re, 30000).catch(() => null); bot.chat(c); const m = await r; await sleep(300); return m; };
    await waitFor(() => bot.blockAt(bot.entity.position.offset(0, -1, 0)), 20000, 'terrain');
    await say('/gamemode creative');
    const home = bot.entity.position.floored();
    // a pen of glass so they stay where they are
    const y = home.y + 20;
    await say(`/fill ${home.x + 2} ${y - 1} ${home.z - 2} ${home.x + 8} ${y - 1} ${home.z + 4} glass`);
    await say(`/fill ${home.x + 2} ${y} ${home.z - 2} ${home.x + 8} ${y + 1} ${home.z + 4} glass hollow`).catch(() => null);
    await say(`/fill ${home.x + 3} ${y} ${home.z - 1} ${home.x + 7} ${y + 1} ${home.z + 3} air`);
    await say(`/summon sheep ${home.x + 5} ${y} ${home.z + 1}`);
    await say(`/summon cow ${home.x + 4} ${y} ${home.z}`);
    await say(`/tp Keeper ${home.x + 5} ${y + 3} ${home.z + 1}`);
    await sleep(1500);
    await say('/give Keeper diamond 3');
    await waitFor(() => bot.inventory.items().some((i) => i.name === 'diamond'), 5000, 'diamonds');
    await bot.tossStack(bot.inventory.items().find((i) => i.name === 'diamond'));
    await sleep(1500);
    const near = (name) => Object.values(bot.entities).filter((e) => {
      const n = e.name === 'item' ? (e.getDroppedItem && e.getDroppedItem()?.name) : e.name;
      return n === name && e.position.distanceTo(new Vec3(home.x + 5, y, home.z + 1)) < 12;
    }).length;
    const census = () => ({ sheep: near('sheep'), cow: near('cow'), diamond: near('diamond') });
    const expectOne = async (when) => {
      await waitFor(() => { const c = census(); return c.sheep >= 1 && c.cow >= 1 && c.diamond >= 1; }, 20000, `the animals and the diamonds ${when}`);
      await sleep(2000);   // duplicates would show up by now
      const c = census();
      console.log(`${when}: ${JSON.stringify(c)}`);
      assert.deepStrictEqual(c, { sheep: 1, cow: 1, diamond: 1 }, `exactly one of each ${when}`);
    };
    await expectOne('at first');
    // 1. across a restart
    await say('/save-all', /Saved/i);
    bot.quit();
    bot = null;
    await srv.stop();
    srv = startServer(args, { log: process.argv.includes('--log') });
    await srv.ready;
    bot = await connectBot(port, 'Keeper');
    await waitFor(() => bot.blockAt(bot.entity.position.offset(0, -1, 0)), 20000, 'terrain after the restart');
    await expectOne('after a restart');
    // 2. far away (stashed in their chunk) and back
    await say(`/tp Keeper ${home.x + 400} ${y + 40} ${home.z}`);
    await sleep(4000);
    await say(`/tp Keeper ${home.x + 5} ${y + 3} ${home.z + 1}`);
    await expectOne('after leaving and coming back');
    // 3. so far that their chunk leaves memory (the PC server keeps 160 chunks), and back
    const loads = async () => Number((String(await say('/storage', /Storage/)).match(/loads (\d+)/) || [])[1]);
    for (const dx of [800, 1600, 2400]) {
      await say(`/tp Keeper ${home.x + dx} ${y + 40} ${home.z}`);
      await sleep(3000);
    }
    const before = await loads();
    await say(`/tp Keeper ${home.x + 5} ${y + 3} ${home.z + 1}`);
    await expectOne('after their chunk was evicted');
    const after = await loads();
    console.log(`  chunks loaded from storage on the way back: ${after - before}`);
    assert(after > before, 'the chunks came back from storage (they had been evicted)');
    console.log('SAVED ENTITIES OK');
  } catch (e) {
    console.error('FAILED:', e);
    process.exitCode = 1;
  } finally {
    if (bot) bot.quit();
    if (srv) await srv.stop();
    fs.rmSync(tmp, { recursive: true, force: true });
  }
})();
