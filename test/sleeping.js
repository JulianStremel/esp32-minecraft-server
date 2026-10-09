'use strict';
// Sleeping: the night passes only when every player sleeps (for 100 ticks); a sleeper
// lies in the bed, sees how many sleep, can leave the bed, and wakes when hurt; beds
// refuse by day and with monsters near.
//   SERVER_BIN=.../mcserver node sleeping.js
const assert = require('assert');
const { Vec3 } = require('vec3');
const { startServer, connectBot, sleep, waitFor, nextChat } = require('./lib');

(async () => {
  const port = 25640 + Math.floor(Math.random() * 100);
  const srv = startServer(['--port', String(port), '--seed', '42', '--ops', 'Ann,Ben', '--no-mobs', '--flat'], { log: process.argv.includes('--log') });
  let a, b;
  try {
    await srv.ready;
    a = await connectBot(port, 'Ann');
    b = await connectBot(port, 'Ben');
    for (const bot of [a, b]) await waitFor(() => bot.blockAt(bot.entity.position.offset(0, -1, 0)), 20000, 'terrain');
    const say = async (bot, c) => { const r = nextChat(bot, /./, 5000).catch(() => null); bot.chat(c); await r; await sleep(250); };
    const p = a.entity.position.floored();
    // two beds side by side, both bots next to them, survival
    await say(a, `/setblock ${p.x + 1} ${p.y} ${p.z} red_bed[part=foot,facing=east]`);
    await say(a, `/setblock ${p.x + 2} ${p.y} ${p.z} red_bed[part=head,facing=east]`);
    await say(a, `/setblock ${p.x + 1} ${p.y} ${p.z + 2} blue_bed[part=foot,facing=east]`);
    await say(a, `/setblock ${p.x + 2} ${p.y} ${p.z + 2} blue_bed[part=head,facing=east]`);
    await say(a, `/tp Ben ${p.x} ${p.y} ${p.z + 2}`);
    await say(a, '/gamemode survival Ann');
    await say(a, '/gamemode survival Ben');
    const bedA = a.blockAt(new Vec3(p.x + 1, p.y, p.z)), bedB = () => b.blockAt(new Vec3(p.x + 1, p.y, p.z + 2));
    const bars = { a: [], b: [] };
    a.on('actionBar', (m) => bars.a.push(m.toString()));
    b.on('actionBar', (m) => bars.b.push(m.toString()));
    // 1. by day: refused
    await say(a, '/time set day');
    await a.activateBlock(bedA);
    await sleep(700);
    console.log(`by day: "${bars.a.at(-1)}", sleeping ${a.isSleeping}`);
    assert(/only at night/.test(bars.a.at(-1) || ''));
    assert(!a.isSleeping);
    // 2. at night with a zombie close: refused
    await say(a, '/time set 14000');
    await say(a, `/summon zombie ${p.x + 4} ${p.y} ${p.z}`);
    await a.activateBlock(bedA);
    await sleep(700);
    console.log(`with a monster near: "${bars.a.at(-1)}"`);
    assert(/monsters nearby/.test(bars.a.at(-1) || ''));
    await say(a, '/difficulty peaceful');   // the zombie goes (peaceful removes monsters)
    await say(a, `/tp Ann ${p.x} ${p.y} ${p.z}`);
    await sleep(500);
    // 3. one of two asleep: the night goes on
    await a.sleep(bedA);
    assert(a.isSleeping, 'Ann lies in the bed');
    await sleep(6000);
    console.log(`Ann asleep, Ben awake for 6 s: time ${a.time.timeOfDay}, Ann sees "${bars.a.at(-1)}"`);
    assert(a.time.timeOfDay > 13000, 'the night did not pass');
    assert(/1\/2 players sleeping/.test(bars.a.at(-1) || ''));
    // 4. both asleep: morning after 100 ticks, both awake
    let woke = 0;
    a.once('wake', () => woke++);
    b.once('wake', () => woke++);
    await b.sleep(bedB());
    console.log(`Ben asleep too: he sees "${bars.b.at(-1)}"`);
    assert(/2\/2 players sleeping/.test(bars.b.at(-1) || ''));
    await waitFor(() => woke === 2, 15000, 'both waking up');
    console.log(`both asleep: morning (time ${a.time.timeOfDay}), both awake`);
    assert(a.time.timeOfDay < 1000);
    // 5. leaving the bed
    await say(a, '/time set 14000');
    await sleep(500);
    await a.sleep(bedA);
    assert(a.isSleeping);
    // "Leave bed" (mineflayer's bot.wake() still sends action 2, which 1.21.6+ took for
    // stop_sprinting; 0 is stop_sleeping)
    a._client.write('entity_action', { entityId: a.entity.id, actionId: 'stop_sleeping', jumpBoost: 0 });
    await waitFor(() => !a.isSleeping, 5000, 'Ann out of bed');
    console.log('left the bed');
    // 6. hurt while asleep: awake
    await say(a, '/difficulty normal');
    await a.sleep(bedA);
    await say(a, `/tp Ben ${p.x + 2} ${p.y + 1} ${p.z + 1}`);
    await sleep(500);
    const ann = Object.values(b.entities).find((e) => e.username === 'Ann');
    b.attack(ann);
    await waitFor(() => !a.isSleeping, 5000, 'Ann woken by the hit');
    console.log('woken by a hit');
    console.log('SLEEPING OK');
  } catch (e) {
    console.error('FAILED:', e);
    process.exitCode = 1;
  } finally {
    if (a) a.quit();
    if (b) b.quit();
    await srv.stop();
  }
})();
