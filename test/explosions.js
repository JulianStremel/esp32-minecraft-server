'use strict';
// Explosions as a client sees them: a block of 50 TNT set off by a redstone block goes off
// as a chain reaction (every TNT primed by its neighbours' blasts, spread over ticks by the
// time budget); a survival player nearby is hurt and pushed by the explosion packet; the
// crater is dug by TNT that drops what it destroys.
//   SERVER_BIN=.../mcserver node explosions.js
const assert = require('assert');
const { Vec3 } = require('vec3');
const { startServer, connectBot, sleep, waitFor, nextChat } = require('./lib');

(async () => {
  const port = 26140 + Math.floor(Math.random() * 50);
  const srv = startServer(['--port', String(port), '--seed', '42', '--ops', 'Demo,Target', '--no-mobs', '--flat'],
                         { log: process.argv.includes('--log') });
  let a, b;
  try {
    await srv.ready;
    a = await connectBot(port, 'Demo');
    b = await connectBot(port, 'Target');
    for (const bot of [a, b]) await waitFor(() => bot.blockAt(bot.entity.position.offset(0, -1, 0)), 20000, 'terrain');
    const say = async (c) => { const r = nextChat(a, /./, 5000).catch(() => null); a.chat(c); await r; await sleep(200); };
    const p = a.entity.position.floored();
    const x0 = p.x + 8, z0 = p.z, y = p.y;
    await say('/gamemode creative Demo');
    await say('/gamemode survival Target');
    await say(`/tp Demo ${x0 - 14} ${y} ${z0}`);
    await say(`/tp Target ${x0 + 10} ${y} ${z0 + 2}`);   // 6 blocks from the TNT's edge
    await sleep(800);
    await say(`/fill ${x0} ${y} ${z0} ${x0 + 4} ${y + 1} ${z0 + 4} tnt`);
    let blasts = 0, firstAt = 0, lastAt = 0, push = null;
    a._client.on('explosion', () => { blasts++; lastAt = Date.now(); if (!firstAt) firstAt = lastAt; });
    b._client.on('explosion', (pk) => { if (pk.playerKnockback && !push) push = pk.playerKnockback; });
    const health = b.health;
    let lowest = health, died = false;   // (dying respawns it with full health)
    b.on('health', () => { if (b.health < lowest) lowest = b.health; });
    b.on('death', () => { died = true; });
    const t0 = Date.now();
    await say(`/setblock ${x0 - 1} ${y} ${z0 + 2} redstone_block`);
    await waitFor(() => blasts >= 50 || (lastAt && Date.now() - lastAt > 3000), 20000, 'the chain reaction');
    await sleep(500);
    const spread = (lastAt - firstAt) / 1000;
    console.log(`${blasts} explosions, the first ${((firstAt - t0) / 1000).toFixed(1)} s after the redstone block, spread over ${spread.toFixed(1)} s`);
    assert.ok(blasts >= 45, 'nearly every TNT went off');
    let left = 0;
    for (let x = x0; x <= x0 + 4; x++)
      for (let z = z0; z <= z0 + 4; z++)
        for (let yy = y; yy <= y + 1; yy++) left += a.blockAt(new Vec3(x, yy, z)).name === 'tnt' ? 1 : 0;
    assert.strictEqual(left, 0, 'no TNT left');
    const crater = a.blockAt(new Vec3(x0 + 2, y - 1, z0 + 2)).name;
    console.log(`the crater: under the middle ${crater}; the target ${died ? 'died' : 'lost ' + (health - lowest).toFixed(1) + ' health'}, pushed by ${push ? [push.x, push.y, push.z].map((v) => v.toFixed(2)).join(' ') : 'nothing'}`);
    assert.ok(crater === 'air' || crater === 'bedrock', 'a crater');
    assert.ok(died || lowest < health, 'the target was hurt');
    assert.ok(push && push.x > 0, 'pushed away from the blast');
    console.log('EXPLOSIONS OK');
  } finally {
    for (const bot of [a, b]) if (bot) bot.end();
    srv.stop();
  }
})().catch((e) => { console.error(e); process.exit(1); });
