'use strict';
// /workerbar: boss bars for the player who asked: a legend of the job kinds, a line per
// worker with its last second by kind, and the queue by kind; off removes them.
//   SERVER_BIN=.../mcserver node workerbar.js
const assert = require('assert');
const { startServer, connectBot, sleep, waitFor, nextChat } = require('./lib');

(async () => {
  const port = 26240 + Math.floor(Math.random() * 50);
  const srv = startServer(['--port', String(port), '--seed', '42', '--ops', 'Watcher', '--view', '10'],
                         { log: process.argv.includes('--log') });
  let bot, other;
  try {
    await srv.ready;
    bot = await connectBot(port, 'Watcher');
    other = await connectBot(port, 'Bystander');
    await waitFor(() => bot.blockAt(bot.entity.position.offset(0, -1, 0)), 20000, 'terrain');
    const bars = new Map();   // uuid -> title text
    const titles = (b) => [...b.values()];
    bot.on('bossBarCreated', (bb) => bars.set(bb.entityUUID, bb.title.toString()));
    bot.on('bossBarUpdated', (bb) => bars.set(bb.entityUUID, bb.title.toString()));
    bot.on('bossBarDeleted', (bb) => bars.delete(bb.entityUUID));
    let otherBars = 0;
    other.on('bossBarCreated', () => otherBars++);
    const r = nextChat(bot, /Worker display/, 5000);
    bot.chat('/workerbar on');
    console.log((await r).toString());
    await waitFor(() => titles(bars).some((t) => /^queue/.test(t)), 5000, 'the bars');
    // work for the workers: far away, new terrain
    bot.chat('/tp Watcher 3000 120 3000');
    let busy = null;
    await waitFor(() => (busy = titles(bars).find((t) => /^W\d+ +\d+%/.test(t) && !/ 0%/.test(t))), 15000, 'a busy worker');
    await sleep(1100);
    const all = titles(bars);
    const workers = all.filter((t) => /^W\d+/.test(t));
    console.log(all.map((t) => '  ' + t).join('\n'));
    assert.ok(all.some((t) => /workers: send load light save path spawn dash other idle/.test(t)), 'the legend');
    assert.ok(workers.length >= 1, 'a line per worker');
    assert.ok(workers.every((t) => (t.match(/█/g) || []).length === 40), '40 segments a worker');
    assert.ok(all.some((t) => /^queue \d+/.test(t)), 'the queue');
    assert.strictEqual(otherBars, 0, 'only for the player who asked');
    const r2 = nextChat(bot, /Worker display/, 5000);
    bot.chat('/workerbar off');
    await r2;
    await waitFor(() => bars.size === 0, 3000, 'removed');
    console.log('off: the bars are gone');
    console.log('WORKERBAR OK');
  } finally {
    for (const b of [bot, other]) if (b) b.end();
    srv.stop();
  }
})().catch((e) => { console.error(e); process.exit(1); });
