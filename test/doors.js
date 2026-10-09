'use strict';
// Doors placed as vanilla (DoorBlock#getHinge): a door placed to the right of another
// gets its hinge on the right, so the two make a double door; a wall beside a door
// puts the hinge on the wall's side.
//   SERVER_BIN=.../mcserver node doors.js
const assert = require('assert');
const { Vec3 } = require('vec3');
const { startServer, connectBot, sleep, waitFor, nextChat } = require('./lib');

(async () => {
  const port = 25840 + Math.floor(Math.random() * 100);
  const srv = startServer(['--port', String(port), '--seed', '42', '--ops', 'Carpenter', '--no-mobs', '--flat'], { log: process.argv.includes('--log') });
  let bot;
  try {
    await srv.ready;
    bot = await connectBot(port, 'Carpenter');
    await waitFor(() => bot.blockAt(bot.entity.position.offset(0, -1, 0)), 20000, 'terrain');
    const say = async (c) => { const r = nextChat(bot, /./, 5000).catch(() => null); bot.chat(c); await r; await sleep(250); };
    await say('/gamemode creative Carpenter');
    await say('/give Carpenter oak_door 16');
    await waitFor(() => bot.inventory.items().some((i) => i.name === 'oak_door'), 5000, 'doors');
    await bot.equip(bot.inventory.items().find((i) => i.name === 'oak_door'), 'hand');
    const p = bot.entity.position.floored();
    // the doors 3 blocks north of the player, who faces north (east is to the right)
    const place = async (x, z) => {
      await bot.look(0, 0, true);   // yaw 0: north in mineflayer
      const ground = bot.blockAt(new Vec3(x, p.y - 1, z));
      await bot.placeBlock(ground, new Vec3(0, 1, 0)).catch(() => {});
      await sleep(400);
      const d = bot.blockAt(new Vec3(x, p.y, z));
      return d && d.name === 'oak_door' ? d.getProperties() : null;
    };
    const a = await place(p.x, p.z - 3);
    const b = await place(p.x + 1, p.z - 3);
    console.log(`first door: facing ${a && a.facing}, hinge ${a && a.hinge}; the one to its right: hinge ${b && b.hinge}`);
    assert.ok(a && b, 'doors placed');
    assert.strictEqual(a.facing, 'north');
    assert.strictEqual(a.hinge, 'left');
    assert.strictEqual(b.hinge, 'right');
    const upper = bot.blockAt(new Vec3(p.x + 1, p.y + 1, p.z - 3)).getProperties();
    assert.strictEqual(upper.hinge, 'right', 'the upper half matches');
    // a wall on the right: the hinge goes on that side
    await say(`/fill ${p.x - 2} ${p.y} ${p.z - 5} ${p.x - 2} ${p.y + 1} ${p.z - 5} stone`);
    const c = await place(p.x - 3, p.z - 5);
    console.log(`a door with a wall to its right: hinge ${c && c.hinge}`);
    assert.ok(c && c.hinge === 'right');
    console.log('DOORS OK');
  } finally {
    if (bot) bot.end();
    srv.stop();
  }
})().catch((e) => { console.error(e); process.exit(1); });
