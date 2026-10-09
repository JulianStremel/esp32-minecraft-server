'use strict';
// A mob whose ground is removed falls (it does not keep walking on the air its path
// was planned over).
//   SERVER_BIN=.../mcserver node mob_ground_removed.js     (or --host <board ip>: as Tester)
const assert = require('assert');
const { startServer, connectBot, sleep, waitFor, nextChat } = require('./lib');

(async () => {
  const hi = process.argv.indexOf('--host'), host = hi > 0 ? process.argv[hi + 1] : null;
  const port = host ? 25565 : 25640 + Math.floor(Math.random() * 100);
  const name = host ? 'Tester' : 'Watcher';
  const srv = host ? null : startServer(['--port', String(port), '--seed', '42', '--ops', 'Watcher', '--no-mobs'], { log: process.argv.includes('--log') });
  let bot;
  try {
    if (srv) await srv.ready;
    bot = await connectBot(port, name, host ? { host, checkTimeoutInterval: 600000 } : {});
    const command = async (c, pattern = /./) => { const r = nextChat(bot, pattern, 15000).catch(() => null); bot.chat(c); await r; await sleep(300); };
    await waitFor(() => bot.blockAt(bot.entity.position.offset(0, -1, 0)), 20000, 'spawn terrain');
    await command('/gamemode creative');
    const p = bot.entity.position.floored();
    // a stone floor high up, the bot on a pillar next to it (survival: the zombie chases it)
    const y = p.y + 30;
    await command(`/fill ${p.x - 30} ${y} ${p.z - 3} ${p.x - 3} ${y} ${p.z + 3} stone`);
    await command(`/fill ${p.x - 1} ${y} ${p.z - 1} ${p.x + 1} ${y} ${p.z + 1} stone`);
    await command(`/tp ${name} ${p.x + 0.5} ${y + 1} ${p.z + 0.5}`);
    await sleep(1500);
    let mine = null;
    const onSpawn = (e) => { if (e.name === 'zombie' && Math.abs(e.position.x - (p.x - 25.5)) < 1.5) mine = e; };
    bot.on('entitySpawn', onSpawn);
    await command(`/summon zombie ${p.x - 25.5} ${y + 1} ${p.z + 0.5}`);
    await waitFor(() => mine, 5000, 'the zombie');
    bot.off('entitySpawn', onSpawn);
    const zombie = () => (mine && bot.entities[mine.id]) || null;
    const track = setInterval(() => { const z = zombie(); if (z) heights.push(z.position.y.toFixed(1)); }, 250);
    const heights = [];
    await command('/gamemode survival');
    await sleep(1500);   // a path is planned and followed
    const before = zombie().position.clone();
    console.log(`zombie walking at ${before}`);
    // the floor under it and ahead of it goes away
    bot.on('messagestr', (m) => console.log('  chat:', m));
    await command(`/fill ${p.x - 30} ${y} ${p.z - 3} ${p.x - 3} ${y} ${p.z + 3} air`);
    await sleep(2000);
    const after = zombie() && zombie().position;
    const under = after && bot.blockAt(after.offset(0, -1, 0));
    clearInterval(track);
    console.log(`two seconds later: ${after}, block under it ${under && under.name}; heights ${heights.join(' ')}`);
    assert(!after || after.y < y - 5, 'the zombie still walks at the height of the removed floor');
    console.log('MOB GROUND REMOVED OK');
  } catch (e) {
    console.error('FAILED:', e);
    process.exitCode = 1;
  } finally {
    if (bot) bot.quit();
    if (srv) await srv.stop();
  }
})();
