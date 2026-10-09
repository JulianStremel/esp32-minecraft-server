'use strict';
// Boats: placed on water from the item, floating, getting in, driving (the rider's
// client moves the boat; a jump of 50 blocks is refused), getting out with the shift key,
// a pig that bumps into a boat getting in, a chest boat's slots, breaking into the item,
// and a boat kept across a restart.
//   SERVER_BIN=.../mcserver node boats.js
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { Vec3 } = require('vec3');
const { startServer, connectBot, sleep, waitFor, nextChat } = require('./lib');

(async () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'mcboat-'));
  const port = 25940 + Math.floor(Math.random() * 50);
  const args = ['--port', String(port), '--seed', '42', '--ops', 'Sailor,Watcher', '--no-mobs', '--flat',
                '--file', path.join(tmp, 'world.img'), '--size', '256'];
  let srv = startServer(args, { log: process.argv.includes('--log') });
  let a, w;
  try {
    await srv.ready;
    a = await connectBot(port, 'Sailor');
    w = await connectBot(port, 'Watcher');
    for (const bot of [a, w]) await waitFor(() => bot.blockAt(bot.entity.position.offset(0, -1, 0)), 20000, 'terrain');
    const say = async (c) => { const r = nextChat(a, /./, 5000).catch(() => null); a.chat(c); await r; await sleep(250); };
    const p = a.entity.position.floored();
    const floor = p.y - 1;
    // a pool 12 x 7, two deep, east of the player; the watcher stands on its far side
    await say(`/fill ${p.x + 2} ${floor - 1} ${p.z - 3} ${p.x + 13} ${floor} ${p.z + 3} water`);
    await say(`/tp Watcher ${p.x + 7} ${p.y} ${p.z + 6}`);
    await say('/gamemode survival Sailor');
    await say('/give Sailor oak_boat 1');
    await waitFor(() => a.inventory.items().some((i) => i.name === 'oak_boat'), 5000, 'boat item');
    await a.equip(a.inventory.items().find((i) => i.name === 'oak_boat'), 'hand');
    // 1. placed on the water it is looked at, the item used up
    await a.lookAt(new Vec3(p.x + 3.5, floor + 0.9, p.z + 0.5), true);
    a.activateItem();
    const boat = await waitFor(() => Object.values(w.entities).find((e) => e.name === 'oak_boat'), 5000, 'the boat');
    await waitFor(() => !a.inventory.items().some((i) => i.name === 'oak_boat'), 3000, 'item used');
    await sleep(1500);
    const surface = floor + 8 / 9;
    console.log(`placed at ${boat.position.x.toFixed(2)} ${boat.position.y.toFixed(2)} ${boat.position.z.toFixed(2)}; floats (water surface at ${surface.toFixed(2)})`);
    assert.ok(Math.abs(boat.position.x - (p.x + 3.5)) < 1.5, 'where it was looked at');
    assert.ok(boat.position.y > surface - 0.6 && boat.position.y < surface + 0.2, 'floats on the surface');
    // 2. getting in: everyone sees the rider
    a.mount(a.entities[boat.id] || boat);
    await waitFor(() => a.vehicle, 3000, 'mounted');
    await waitFor(() => (boat.passengers || []).some((e) => e.username === 'Sailor'), 3000, 'the watcher sees the rider');
    console.log('in the boat: the rider and the watcher both see it');
    // 3. driving: the client moves the boat 6 blocks east, 0.3 a tick
    let x = boat.position.x;
    const y = boat.position.y, z = boat.position.z;
    for (let i = 0; i < 20; i++) {
      x += 0.3;
      a._client.write('vehicle_move', { x, y, z, yaw: -90, pitch: 0, onGround: false });
      await sleep(50);
    }
    await waitFor(() => Math.abs(boat.position.x - x) < 0.3, 3000, 'the watcher sees the boat arrive');
    console.log(`driven to x ${boat.position.x.toFixed(2)} (sent ${x.toFixed(2)})`);
    // a move of 50 blocks is refused: the client gets the boat's position back
    const back = new Promise((resolve) => a._client.once('vehicle_move', resolve));
    a._client.write('vehicle_move', { x: x + 50, y, z, yaw: -90, pitch: 0, onGround: false });
    const reply = await Promise.race([back, sleep(3000).then(() => null)]);
    assert.ok(reply && Math.abs(reply.x - x) < 0.01, 'the jump refused');
    await sleep(300);
    assert.ok(Math.abs(boat.position.x - x) < 0.3, 'the boat stayed');
    console.log(`a jump of 50 blocks: refused, the boat stays at ${boat.position.x.toFixed(2)}`);
    // 4. getting out: the shift key, beside the boat
    // (mineflayer keeps bot.vehicle: it only clears it for a vehicle id of -1, which vanilla
    // never sends; the client reads the boat's new list of passengers)
    let out = false;
    const onPassengers = (pk) => { if (pk.entityId === boat.id && !pk.passengers.includes(a.entity.id)) out = true; };
    a._client.on('set_passengers', onPassengers);
    a._client.write('player_input', { inputs: { shift: true } });
    await waitFor(() => out, 3000, 'dismounted');
    a._client.removeListener('set_passengers', onPassengers);
    a.vehicle = null;
    a._client.write('player_input', { inputs: {} });
    await sleep(500);
    const d = a.entity.position.distanceTo(boat.position);
    console.log(`out of the boat, ${d.toFixed(2)} blocks from it`);
    assert.ok(d > 0.8 && d < 3, 'beside the boat');
    // 5. a pig that bumps into it gets in
    await say(`/summon pig ${boat.position.x.toFixed(2)} ${(boat.position.y + 0.5).toFixed(2)} ${boat.position.z.toFixed(2)}`);
    await waitFor(() => (boat.passengers || []).some((e) => e.name === 'pig'), 5000, 'the pig in the boat');
    console.log('a pig got in');
    // 6. breaking it in survival: five punches, the boat item comes out; the pig gets out
    // (the boat's last list of passengers is empty; mineflayer does not clear pig.vehicle)
    let lastList = null;
    w._client.on('set_passengers', (pk) => { if (pk.entityId === boat.id) lastList = pk.passengers; });
    await a.lookAt(boat.position, true);
    for (let i = 0; i < 6 && w.entities[boat.id]; i++) { a.attack(boat); await sleep(120); }
    await waitFor(() => !w.entities[boat.id], 3000, 'boat broken');
    await waitFor(() => Object.values(w.entities).some((e) => e.name === 'item' && e.getDroppedItem && e.getDroppedItem() && e.getDroppedItem().name === 'oak_boat'), 3000, 'boat item dropped');
    assert.ok(lastList && lastList.length === 0, 'the pig is out');
    assert.ok(Object.values(w.entities).some((e) => e.name === 'pig'), 'the pig is still there');
    console.log('broken: the oak_boat item dropped, the pig is out');
    // 7. a chest boat: sneaking opens its 27 slots; it keeps them across a restart
    await say('/gamemode creative Sailor');
    await say('/give Sailor oak_chest_boat 1');
    await say('/give Sailor diamond 5');
    await waitFor(() => a.inventory.items().some((i) => i.name === 'oak_chest_boat'), 5000, 'chest boat');
    await a.equip(a.inventory.items().find((i) => i.name === 'oak_chest_boat'), 'hand');
    await a.lookAt(new Vec3(p.x + 10.5, floor + 0.9, p.z - 1.5), true);
    a.activateItem();
    const cb = await waitFor(() => Object.values(a.entities).find((e) => e.name === 'oak_chest_boat'), 5000, 'chest boat placed');
    await sleep(800);
    a.setControlState('sneak', true);
    await sleep(300);
    const opened = new Promise((resolve) => a.once('windowOpen', resolve));
    a.useOn(cb);
    const win = await Promise.race([opened, sleep(3000).then(() => null)]);
    a.setControlState('sneak', false);
    assert.ok(win && win.inventoryStart === 27, 'a 27-slot window');
    const dia = win.items().find((i) => i.name === 'diamond' && i.slot >= win.inventoryStart);
    await a.clickWindow(dia.slot, 0, 0);
    await a.clickWindow(4, 0, 0);
    await sleep(300);
    a.closeWindow(win);
    console.log('chest boat: 5 diamonds put in its slot 4');
    await say('/save-all');
    await sleep(500);
    a.end(); w.end(); a = w = null;
    srv.stop();
    await sleep(1000);
    srv = startServer(args, { log: process.argv.includes('--log') });
    await srv.ready;
    a = await connectBot(port, 'Sailor');
    await waitFor(() => a.blockAt(a.entity.position.offset(0, -1, 0)), 20000, 'terrain');
    const cb2 = await waitFor(() => Object.values(a.entities).find((e) => e.name === 'oak_chest_boat'), 8000, 'chest boat after the restart');
    await sleep(500);
    a.setControlState('sneak', true);
    await sleep(300);
    const opened2 = new Promise((resolve) => a.once('windowOpen', resolve));
    a.useOn(cb2);
    const win2 = await Promise.race([opened2, sleep(3000).then(() => null)]);
    a.setControlState('sneak', false);
    const kept = win2 && win2.slots[4];
    console.log(`after a restart: the chest boat is there, slot 4 holds ${kept ? kept.count + ' ' + kept.name : 'nothing'}`);
    assert.ok(kept && kept.name === 'diamond' && kept.count === 5);
    console.log('BOATS OK');
  } finally {
    for (const b of [a, w]) if (b) b.end();
    srv.stop();
    fs.rmSync(tmp, { recursive: true, force: true });
  }
})().catch((e) => { console.error(e); process.exit(1); });
