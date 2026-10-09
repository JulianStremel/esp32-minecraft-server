'use strict';
// Knockback as vanilla: a mob hit on flat ground flies back about 0.4 blocks a tick and
// up as LivingEntity#knockback (vy 0.36), falls with gravity 0.08 and drag 0.98 (apex
// about 0.96 blocks, back down after about 10 ticks), and a mob chasing its target
// still flies back instead of walking on at once.
//   SERVER_BIN=.../mcserver node knockback.js
const assert = require('assert');
const { startServer, connectBot, sleep, waitFor, nextChat } = require('./lib');

// vanilla, on the ground: knockback(0.4) gives vy = min(0.4, -0.0784 / 2 + 0.4)
function vanillaArc() {
  let y = 0, vy = 0.3608, apex = 0, t = 0;
  while (t < 60) {
    y += vy; t++;
    vy = (vy - 0.08) * 0.98;
    apex = Math.max(apex, y);
    if (y <= 0) break;
  }
  return { apex, ticks: t };
}

(async () => {
  const port = 25740 + Math.floor(Math.random() * 100);
  const srv = startServer(['--port', String(port), '--seed', '42', '--ops', 'Hitter', '--no-mobs', '--flat'], { log: process.argv.includes('--log') });
  let bot;
  try {
    await srv.ready;
    bot = await connectBot(port, 'Hitter');
    await waitFor(() => bot.blockAt(bot.entity.position.offset(0, -1, 0)), 20000, 'terrain');
    const say = async (c) => { const r = nextChat(bot, /./, 5000).catch(() => null); bot.chat(c); await r; await sleep(250); };
    const p = bot.entity.position.floored();
    const results = {};
    let lastId = 0;
    for (const [type, mode] of [['pig', 'creative'], ['zombie', 'survival']]) {
      await say(`/gamemode ${mode} Hitter`);
      await say(`/difficulty ${type === 'zombie' ? 'easy' : 'peaceful'}`);
      await say('/time set night');   // a zombie in the sun burns: hurt events of its own
      await say(`/summon ${type} ${p.x + 2.5} ${p.y} ${p.z + 0.5}`);
      const mob = await waitFor(() => Object.values(bot.entities).find((e) => e.name === type), 5000, type);
      await sleep(1500);   // settled on the ground
      // hit it when it is in reach and on the ground (a zombie walks up meanwhile); the
      // hurt event tells the hit landed
      let track = [];
      for (let attempt = 0; attempt < 6; attempt++) {
        if (mob.position.distanceTo(bot.entity.position) > 2.8) {   // it wandered: step up to it
          await say(`/tp Hitter ${(mob.position.x - 2).toFixed(2)} ${mob.position.y} ${mob.position.z.toFixed(2)}`);
          await sleep(300);
        }
        await bot.lookAt(mob.position.offset(0, 1, 0), true);
        const y0 = mob.position.y, me = bot.entity.position.clone();
        const d0 = Math.hypot(mob.position.x - me.x, mob.position.z - me.z);
        const t0 = Date.now();
        const got = [];
        const onMove = (e) => {
          if (e === mob) got.push({ t: Date.now() - t0, y: e.position.y - y0, x: Math.hypot(e.position.x - me.x, e.position.z - me.z) - d0 });
        };
        let hurt = false;
        const onHurt = (e) => { if (e === mob) hurt = true; };
        bot.on('entityMoved', onMove);
        bot.on('entityHurt', onHurt);
        bot.attack(mob);
        await sleep(1500);
        bot.removeListener('entityMoved', onMove);
        bot.removeListener('entityHurt', onHurt);
        if (hurt) { track = got; break; }
      }
      const air = track.filter((s) => s.y > 0.01);
      const apex = Math.max(0, ...track.map((s) => s.y));
      const airMs = air.length ? air[air.length - 1].t - air[0].t : 0;
      const away = track.length ? Math.max(...track.filter((s) => s.t < 800).map((s) => s.x)) : 0;
      results[type] = { apex, airMs, away };
      lastId = Math.max(lastId, mob.id);
      console.log(`${type}: apex ${apex.toFixed(2)} blocks, in the air ${airMs} ms, ${away.toFixed(2)} blocks back at most (in 0.8 s)`);
      console.log('  y: ' + track.slice(0, 16).map((s) => s.y.toFixed(2)).join(' '));
      await say(`/kill @e[type=${type}]`);
      await sleep(500);
    }
    // fast clicking: hits while the mob is still invulnerable (10 ticks) do not throw it again
    await say('/gamemode creative Hitter');
    await say(`/tp Hitter ${p.x + 0.5} ${p.y} ${p.z + 0.5}`);
    await say(`/summon pig ${p.x + 2.5} ${p.y} ${p.z + 0.5}`);
    const newest = () => Object.values(bot.entities).filter((e) => e.name === 'pig' && e.id > lastId).pop();
    const pig = await waitFor(newest, 5000, 'pig');
    await sleep(1500);
    await bot.lookAt(pig.position.offset(0, 1, 0), true);
    const base = pig.position.y;
    let top = 0;
    const onMove = (e) => { if (e === pig) top = Math.max(top, e.position.y - base); };
    bot.on('entityMoved', onMove);
    for (let i = 0; i < 3; i++) { bot.attack(pig); await sleep(150); }
    await sleep(1200);
    bot.removeListener('entityMoved', onMove);
    console.log(`3 hits 150 ms apart: apex ${top.toFixed(2)} blocks`);
    assert.ok(top < 1.1, 'fast clicks keep it in the air');
    const v = vanillaArc();
    console.log(`vanilla: apex ${v.apex.toFixed(2)} blocks, in the air ${v.ticks} ticks (${v.ticks * 50} ms)`);
    for (const [type, r] of Object.entries(results)) {
      assert.ok(Math.abs(r.apex - v.apex) < 0.2, `${type}: apex ${r.apex.toFixed(2)} vs ${v.apex.toFixed(2)}`);
      assert.ok(r.airMs < (v.ticks + 4) * 50, `${type}: ${r.airMs} ms in the air`);
      assert.ok(r.away > 0.6, `${type}: flew back ${r.away.toFixed(2)} blocks`);
    }
    console.log('KNOCKBACK OK');
  } finally {
    if (bot) bot.end();
    srv.stop();
  }
})().catch((e) => { console.error(e); process.exit(1); });
