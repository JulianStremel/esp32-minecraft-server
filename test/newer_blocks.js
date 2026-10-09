'use strict';
// Blocks of 1.17 to 1.21 a player uses: copper waxed with honeycomb and scraped with an
// axe (a door's two halves together), candles stacked, lit and blown out, a candle cake.
//   SERVER_BIN=.../mcserver node newer_blocks.js
const assert = require('assert');
const { Vec3 } = require('vec3');
const { startServer, connectBot, sleep, waitFor, nextChat } = require('./lib');

(async () => {
  const port = 25640 + Math.floor(Math.random() * 100);
  const srv = startServer(['--port', String(port), '--seed', '42', '--ops', 'Smith', '--no-mobs', '--flat'], { log: process.argv.includes('--log') });
  let bot;
  try {
    await srv.ready;
    bot = await connectBot(port, 'Smith');
    await waitFor(() => bot.blockAt(bot.entity.position.offset(0, -1, 0)), 20000, 'terrain');
    const say = async (c) => { const r = nextChat(bot, /./, 5000).catch(() => null); bot.chat(c); await r; await sleep(250); };
    await say('/gamemode creative');
    const p = bot.entity.position.floored();
    const at = (dx, dy, dz) => new Vec3(p.x + dx, p.y + dy, p.z + dz);
    const name = (v) => bot.blockAt(v)?.name;
    const hold = async (item) => {
      await say(`/clear Smith`);
      if (item) {
        await say(`/give Smith ${item} 8`);
        await waitFor(() => bot.inventory.items().some((i) => i.name === item), 5000, item);
        await bot.equip(bot.inventory.items().find((i) => i.name === item), 'hand');
      }
      await sleep(200);
    };
    const use = async (v) => { await bot.activateBlock(bot.blockAt(v)); await sleep(400); };
    // copper: wax on, wax off, scrape one stage
    const c = at(2, 0, 0);
    await say(`/setblock ${c.x} ${c.y} ${c.z} copper_block`);
    await hold('honeycomb');
    // (a random tick may oxidize it a stage first: waxing keeps whatever stage it has)
    const stage = name(c);
    await use(c);
    console.log(`${stage} + honeycomb: ${name(c)}`);
    assert.strictEqual(name(c), 'waxed_' + stage);
    await hold('iron_axe');
    await use(c);
    console.log(`  + axe: ${name(c)}`);
    assert.strictEqual(name(c), stage);
    await say(`/setblock ${c.x} ${c.y} ${c.z} oxidized_cut_copper_slab[type=top]`);
    await use(c);
    console.log(`oxidized_cut_copper_slab + axe: ${name(c)} (${bot.blockAt(c).getProperties().type})`);
    assert.strictEqual(name(c), 'weathered_cut_copper_slab');
    assert.strictEqual(bot.blockAt(c).getProperties().type, 'top');
    // a copper door: waxing takes sneaking (else the door opens), and both halves change
    const d = at(4, 0, 0);
    await say(`/setblock ${d.x} ${d.y} ${d.z} exposed_copper_door[half=lower,facing=east]`);
    await say(`/setblock ${d.x} ${d.y + 1} ${d.z} exposed_copper_door[half=upper,facing=east]`);
    await hold('honeycomb');
    bot.setControlState('sneak', true);
    await sleep(300);
    // (random ticks may have aged it a stage meanwhile: copper nearby that is further
    // along makes that likelier, as in vanilla)
    const before = name(d);
    await use(d);
    bot.setControlState('sneak', false);
    console.log(`${before} + honeycomb (sneaking): ${name(d)} / ${name(d.offset(0, 1, 0))}`);
    assert(/^(exposed|weathered)_copper_door$/.test(before));
    assert.strictEqual(name(d), 'waxed_' + before);
    assert.strictEqual(name(d.offset(0, 1, 0)), 'waxed_' + before);
    // candles: stack, light, blow out
    const k = at(2, 0, 2);
    await say(`/setblock ${k.x} ${k.y} ${k.z} red_candle`);
    await hold('red_candle');
    await use(k);
    await use(k);
    const candles = bot.blockAt(k).getProperties().candles;
    console.log(`red_candle + 2 candles: ${candles} candles`);
    assert.strictEqual(Number(candles), 3);
    await hold('flint_and_steel');
    await use(k);
    assert.strictEqual(bot.blockAt(k).getProperties().lit, true, 'lit');
    await hold(null);
    await use(k);
    console.log(`  lit with flint and steel, then blown out: lit=${bot.blockAt(k).getProperties().lit}`);
    assert.strictEqual(bot.blockAt(k).getProperties().lit, false);
    // a candle cake: lit, blown out, eaten (the candle drops)
    const cake = at(4, 0, 2);
    await say(`/setblock ${cake.x} ${cake.y} ${cake.z} cake`);
    await hold('white_candle');
    await use(cake);
    console.log(`cake + white_candle: ${name(cake)}`);
    assert.strictEqual(name(cake), 'white_candle_cake');
    await hold('flint_and_steel');
    await use(cake);
    assert.strictEqual(bot.blockAt(cake).getProperties().lit, true);
    await hold(null);
    await use(cake);
    assert.strictEqual(bot.blockAt(cake).getProperties().lit, false);
    let dropped = false;
    bot.on('itemDrop', (e) => { if (e.position.distanceTo(cake) < 3) dropped = true; });
    await use(cake);
    await waitFor(() => dropped, 3000, 'the candle dropping');
    console.log(`  lit, blown out, eaten: ${name(cake)} with bites=${bot.blockAt(cake).getProperties().bites}, the candle dropped`);
    assert.strictEqual(name(cake), 'cake');
    assert.strictEqual(Number(bot.blockAt(cake).getProperties().bites), 1);
    console.log('NEWER BLOCKS OK');
  } catch (e) {
    console.error('FAILED:', e);
    process.exitCode = 1;
  } finally {
    if (bot) bot.quit();
    await srv.stop();
  }
})();
