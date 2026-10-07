'use strict';
const assert = require('assert');
const { startServer, connectBot, waitFor, nextChat, sleep } = require('./lib');

(async () => {
  const port = 25601;
  const srv = startServer(['--port', String(port), '--seed', '42', '--ops', 'Alice', '--view', '4'], { log: !!process.env.LOG });
  await srv.ready;
  let bot;
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
    console.log('SMOKE OK');
  } finally {
    if (bot) bot.quit();
    await sleep(300);
    await srv.stop();
  }
})().catch((e) => { console.error('FAILED:', e); process.exit(1); });
