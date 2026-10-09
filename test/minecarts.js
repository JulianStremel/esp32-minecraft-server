'use strict';
// Rails and minecarts: rails join into a line; a minecart placed on a powered rail against
// a wall is kicked off, runs along the track and lights a lamp beside a detector rail;
// riding (the forward key pushes a slow minecart, sneaking gets out); a chest minecart's
// slots; a hopper minecart picking up an item; a furnace minecart fed coal driving off;
// a TNT minecart primed by a powered activator rail exploding; punches breaking a minecart
// into its item.
//   SERVER_BIN=.../mcserver node minecarts.js
const assert = require('assert');
const { Vec3 } = require('vec3');
const { startServer, connectBot, sleep, waitFor, nextChat } = require('./lib');

(async () => {
  const port = 26040 + Math.floor(Math.random() * 50);
  const srv = startServer(['--port', String(port), '--seed', '42', '--ops', 'Driver,Watcher', '--no-mobs', '--flat'],
                         { log: process.argv.includes('--log') });
  let a, w;
  try {
    await srv.ready;
    a = await connectBot(port, 'Driver');
    w = await connectBot(port, 'Watcher');
    for (const bot of [a, w]) await waitFor(() => bot.blockAt(bot.entity.position.offset(0, -1, 0)), 20000, 'terrain');
    const say = async (c) => { const r = nextChat(a, /./, 5000).catch(() => null); a.chat(c); await r; await sleep(200); };
    await say('/gamemode creative Driver');
    const p = a.entity.position.floored();
    const y = p.y, z = p.z + 3, x0 = p.x + 3;
    const at = (x, yy, zz) => w.blockAt(new Vec3(x, yy, zz));
    const cartsOf = (bot, name) => Object.values(bot.entities).filter((e) => e.name === name);
    await say(`/tp Watcher ${x0 + 6} ${y} ${z + 3}`);
    // 1. the track: a wall, three powered rails on a redstone block, rails, a detector rail
    //    with a lamp beside it, a wall at the end
    await say(`/setblock ${x0 - 1} ${y} ${z} stone`);
    await say(`/setblock ${x0} ${y - 1} ${z} redstone_block`);
    await say(`/fill ${x0} ${y} ${z} ${x0 + 2} ${y} ${z} powered_rail`);
    await say(`/fill ${x0 + 3} ${y} ${z} ${x0 + 24} ${y} ${z} rail`);
    await say(`/setblock ${x0 + 8} ${y} ${z} detector_rail`);
    await say(`/setblock ${x0 + 8} ${y} ${z + 1} redstone_lamp`);
    await say(`/setblock ${x0 + 25} ${y} ${z} stone`);
    await sleep(500);
    const shapes = [x0, x0 + 1, x0 + 5, x0 + 8, x0 + 20].map((x) => at(x, y, z).getProperties().shape);
    console.log(`track shapes: ${shapes.join(', ')}; powered rails: ${[0, 1, 2].map((i) => at(x0 + i, y, z).getProperties().powered).join(', ')}`);
    assert.ok(shapes.every((s) => s === 'east_west'), 'a straight line east-west');
    assert.ok([0, 1, 2].every((i) => at(x0 + i, y, z).getProperties().powered), 'the powered rails are on');
    // 2. a minecart on the first powered rail: kicked off the wall, it runs east
    let lampLit = false;
    w.on('blockUpdate', (old, nb) => { if (nb && nb.position.equals(new Vec3(x0 + 8, y, z + 1)) && nb.getProperties().lit) lampLit = true; });
    await say('/give Driver minecart 4');
    await waitFor(() => a.inventory.items().some((i) => i.name === 'minecart'), 5000, 'minecarts');
    await a.equip(a.inventory.items().find((i) => i.name === 'minecart'), 'hand');
    await a.activateBlock(a.blockAt(new Vec3(x0, y, z)));
    const cart = await waitFor(() => cartsOf(w, 'minecart')[0], 5000, 'the minecart');
    const t0 = Date.now();
    let fastest = 0, last = cart.position.x, lastT = t0;
    while (Date.now() - t0 < 6000) {
      await sleep(250);
      const now = Date.now();
      fastest = Math.max(fastest, (cart.position.x - last) / ((now - lastT) / 1000));
      last = cart.position.x;
      lastT = now;
      if (cart.position.x > x0 + 12 && lampLit) break;
    }
    console.log(`kicked off the wall: at x ${(cart.position.x - x0).toFixed(1)} past the start, fastest ${fastest.toFixed(1)} blocks/s; the lamp lit: ${lampLit}`);
    assert.ok(cart.position.x > x0 + 9, 'ran along the track');
    assert.ok(fastest > 4 && fastest < 8.6, 'at most 8 blocks a second');
    assert.ok(lampLit, 'the detector rail lit the lamp');
    await waitFor(() => !at(x0 + 8, y, z + 1).getProperties().lit, 5000, 'the lamp goes off');
    console.log('the lamp went off after the minecart left');
    // 3. riding: in, the forward key pushes it while slow, sneaking gets out
    await sleep(3000);   // it rolls to a stop
    const stop = cart.position.x;
    a.mount(a.entities[cart.id]);
    let riding = false;
    a._client.on('set_passengers', (pk) => { if (pk.entityId === cart.id) riding = pk.passengers.includes(a.entity.id); });
    await waitFor(() => riding, 3000, 'riding');
    a._client.write('look', { yaw: -90, pitch: 0, flags: { onGround: false } });
    a._client.write('player_input', { inputs: { forward: true } });
    await sleep(2500);
    a._client.write('player_input', { inputs: {} });
    const pushed = Math.abs(cart.position.x - stop);
    console.log(`riding: the forward key pushed it ${pushed.toFixed(2)} blocks in 2.5 s`);
    assert.ok(pushed > 0.5, 'pushed by the rider');
    a._client.write('player_input', { inputs: { shift: true } });
    await waitFor(() => !riding, 3000, 'out');
    a._client.write('player_input', { inputs: {} });
    console.log('sneaking: out of the minecart');
    // 4. a chest minecart: 27 slots
    const z2 = z + 6;
    await say(`/fill ${x0} ${y} ${z2} ${x0 + 12} ${y} ${z2} rail`);
    await say('/give Driver chest_minecart 1');
    await say('/give Driver hopper_minecart 1');
    await say('/give Driver furnace_minecart 1');
    await say('/give Driver coal 2');
    await say('/give Driver tnt_minecart 1');
    await waitFor(() => a.inventory.items().some((i) => i.name === 'tnt_minecart'), 5000, 'items');
    await say(`/tp Driver ${x0 + 5.5} ${y} ${z2 - 2.5}`);
    await sleep(300);
    const placeOn = async (item, x, zz) => {
      await a.equip(a.inventory.items().find((i) => i.name === item), 'hand');
      await a.activateBlock(a.blockAt(new Vec3(x, y, zz)));
      return waitFor(() => cartsOf(a, item)[0], 5000, item);
    };
    const chest = await placeOn('chest_minecart', x0 + 1, z2);
    await sleep(300);
    const opened = new Promise((resolve) => a.once('windowOpen', resolve));
    a.activateEntity(chest);
    const win = await Promise.race([opened, sleep(3000).then(() => null)]);
    assert.ok(win && win.inventoryStart === 27, 'a 27-slot window');
    a.closeWindow(win);
    console.log('chest minecart: a 27-slot window');
    // 5. a hopper minecart takes what the chest above it holds
    const hopper = await placeOn('hopper_minecart', x0 + 4, z2);
    await say('/give Driver diamond 1');
    await say(`/setblock ${x0 + 4} ${y + 1} ${z2} chest`);
    await sleep(300);
    const cwin = new Promise((resolve) => a.once('windowOpen', resolve));
    await a.activateBlock(a.blockAt(new Vec3(x0 + 4, y + 1, z2)));
    const cw = await Promise.race([cwin, sleep(3000).then(() => null)]);
    assert.ok(cw, 'the chest opens');
    const dia = cw.items().find((i) => i.name === 'diamond' && i.slot >= cw.inventoryStart);
    await a.clickWindow(dia.slot, 0, 0);
    await a.clickWindow(0, 0, 0);
    a.closeWindow(cw);
    await sleep(1000);
    const hwin = new Promise((resolve) => a.once('windowOpen', resolve));
    a.activateEntity(hopper);
    const hw = await Promise.race([hwin, sleep(3000).then(() => null)]);
    // (windowOpen comes before the slots' contents)
    if (hw) await waitFor(() => hw.slots.slice(0, 5).some((s) => s && s.name === 'diamond'), 2000, 'the hopper minecart slots').catch(() => null);
    const got = hw && hw.slots.slice(0, 5).find((s) => s && s.name === 'diamond');
    console.log(`hopper minecart: a 5-slot window, holding ${got ? got.count + ' diamond' : 'nothing'}`);
    if (hw) a.closeWindow(hw);
    assert.ok(hw && hw.inventoryStart === 5, 'a 5-slot window');
    assert.ok(got, 'it took the diamond');
    // 6. a furnace minecart fed coal drives off away from the player
    const furnace = await placeOn('furnace_minecart', x0 + 9, z2);
    await say(`/tp Driver ${x0 + 7.5} ${y} ${z2 + 0.5}`);
    await sleep(300);
    await a.equip(a.inventory.items().find((i) => i.name === 'coal'), 'hand');
    const fx = furnace.position.x;
    a.activateEntity(furnace);
    await sleep(1500);
    console.log(`furnace minecart: ${(furnace.position.x - fx).toFixed(2)} blocks east 1.5 s after the coal`);
    assert.ok(furnace.position.x - fx > 1, 'it drives away');
    // 7. a TNT minecart on a powered activator rail: primed, it explodes
    const z3 = z + 10;
    await say(`/setblock ${x0} ${y - 1} ${z3} redstone_block`);
    await say(`/setblock ${x0} ${y} ${z3} activator_rail`);
    await say(`/tp Driver ${x0 + 2.5} ${y} ${z3 - 2.5}`);
    await sleep(300);
    const tnt = await placeOn('tnt_minecart', x0, z3);
    const t1 = Date.now();
    await waitFor(() => !w.entities[tnt.id] && !a.entities[tnt.id], 8000, 'the explosion');
    console.log(`TNT minecart: exploded ${((Date.now() - t1) / 1000).toFixed(1)} s after it was placed on the powered activator rail`);
    // 8. breaking: punches in survival drop the minecart item
    await say('/gamemode survival Driver');
    await say(`/tp Driver ${cart.position.x.toFixed(2)} ${y} ${z - 1.5}`);
    await sleep(400);
    await a.lookAt(cart.position, true);
    for (let i = 0; i < 6 && w.entities[cart.id]; i++) { a.attack(a.entities[cart.id] || cart); await sleep(120); }
    await waitFor(() => !w.entities[cart.id], 3000, 'broken');
    await waitFor(() => Object.values(w.entities).some((e) => e.name === 'item' && e.getDroppedItem && e.getDroppedItem() && e.getDroppedItem().name === 'minecart'), 3000, 'item');
    console.log('broken by punches: the minecart item dropped');
    console.log('MINECARTS OK');
  } finally {
    for (const b of [a, w]) if (b) b.end();
    srv.stop();
  }
})().catch((e) => { console.error(e); process.exit(1); });
