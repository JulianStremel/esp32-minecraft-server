'use strict';
const assert = require('assert');
const { startServer, connectBot, waitFor, nextChat, sleep, kickText } = require('./lib');

(async () => {
  const port = 25601;
  const srv = startServer(['--port', String(port), '--seed', '42', '--ops', 'Alice', '--view', '4'], { log: !!process.env.LOG });
  await srv.ready;
  let bot;
  let bob;
  try {
    bot = await connectBot(port, 'Alice');
    console.log('spawned at', bot.entity.position.toString(), 'gamemode', bot.game.gameMode);
    await waitFor(() => bot.world.getColumnAt(bot.entity.position), 10000, 'chunk at spawn');
    const below = bot.blockAt(bot.entity.position.offset(0, -1, 0));
    console.log('block below feet:', below && below.name);
    assert(below && below.name !== 'air', 'standing on something');
    // chunks around
    await sleep(1500);
    const cols = Object.keys(bot.world.async.columns || {}).length;
    console.log('columns loaded:', cols);
    const seed = nextChat(bot, /Seed/);
    bot.chat('/seed');
    console.log('seed reply:', await seed);
    const hello = nextChat(bot, /hello world/);
    bot.chat('hello world');
    console.log('chat:', await hello);

    // chat limit as in vanilla: one message every 600 ms is fine for any length of time,
    // a burst of more than 10 kicks a non-operator
    bob = await connectBot(port, 'Bob');
    let kicked = null;
    bob.on('kicked', (reason) => { kicked = kickText(reason); });
    const got = [];
    const onMsg = (m) => { const t = m.toString(); if (/<Bob> line \d+/.test(t)) got.push(t); };
    bot.on('message', onMsg);
    for (let i = 1; i <= 14; i++) {
      bob.chat(`line ${i}`);
      await sleep(600);
    }
    await waitFor(() => got.length >= 14, 5000, 'all 14 paced chat lines');
    assert(kicked === null, 'paced chat must not be kicked: ' + kicked);
    for (let i = 1; i <= 12; i++) bob.chat(`burst ${i}`);
    await waitFor(() => kicked !== null, 5000, 'kick for spamming');
    assert(/spam/i.test(kicked), 'kick reason: ' + kicked);
    bot.removeListener('message', onMsg);
    console.log('chat limit: 14 paced lines delivered, burst kicked');
    console.log('SMOKE OK');
  } finally {
    if (bot) bot.quit();
    if (bob) bob.quit();
    await sleep(300);
    await srv.stop();
  }
})().catch((e) => { console.error('FAILED:', e); process.exit(1); });
