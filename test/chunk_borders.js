'use strict';
// Across chunk borders, as a client sees it, after the place's chunks left the server's
// memory (as on the board: few chunks kept, a simulation distance of 3, a stored world):
// TNT on the corner of four chunks destroys blocks in all four (what the client shows,
// and what a client joining later gets); a boat driven over chunk borders east, west,
// north and south, into negative coordinates too, is never sent back.
//   SERVER_BIN=.../mcserver node chunk_borders.js
//   node chunk_borders.js --host 192.168.1.160 --user Tester --at 2048,-2048   (a board;
//     --at: a place away from builds, a multiple of 16)
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { Vec3 } = require('vec3');
const { startServer, connectBot, sleep, waitFor, nextChat } = require('./lib');

const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i > 0 ? process.argv[i + 1] : d; };
const host = arg('host', null);
const user = arg('user', 'Prober');
const [ox, oz] = arg('at', '0,0').split(',').map(Number);

(async () => {
  const port = host ? 25565 : 26340 + Math.floor(Math.random() * 50);
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'mcborder-'));
  const srv = host ? { ready: Promise.resolve(), stop() {} }
    : startServer(['--port', String(port), '--seed', '42', '--ops', user, '--no-mobs', '--flat',
                   '--cache', '40', '--sim', '3', '--view', '6', '--file', path.join(tmp, 'w.img'), '--size', '256'],
                  { log: process.argv.includes('--log') });
  const opts = host ? { host } : {};
  let a, late;
  try {
    await srv.ready;
    a = await connectBot(port, user, opts);
    const say = async (c) => { const r = nextChat(a, /./, 5000).catch(() => null); a.chat(c); await r; await sleep(250); };
    await say('/gamemode creative');
    // a level place: a platform at y 100 on the board, the flat ground on the PC
    await say(`/tp ${user} ${ox} ${host ? 110 : 10} ${oz}`);
    await sleep(host ? 4000 : 1000);
    const y = host ? 101 : a.entity.position.floored().y;
    if (host) await say(`/fill ${ox - 44} 98 ${oz - 44} ${ox + 44} 100 ${oz + 44} dirt`);   // (/fill: at most 32768 blocks)
    // away and back: the place's chunks leave the server's memory meanwhile
    const away = async (bx, bz) => {
      await say(`/tp ${user} ${ox + 800} ${y + 40} ${oz + 800}`);
      await sleep(host ? 8000 : 4000);
      await say(`/tp ${user} ${bx} ${y + 8} ${bz}`);
      await sleep(host ? 1500 : 400);
    };
    // 1. TNT on the corner of four chunks
    const cx = ox + 32, cz = oz + 32;
    await say(`/fill ${cx - 8} ${y - 4} ${cz - 8} ${cx + 7} ${y - 1} ${cz + 7} dirt`);
    await say(`/setblock ${cx} ${y} ${cz} tnt`);
    await away(cx - 12, cz - 12);
    await say(`/setblock ${cx} ${y + 1} ${cz} redstone_block`);
    await sleep(5500);
    const count = (bot) => {
      const q = [0, 0, 0, 0];
      for (let x = cx - 8; x <= cx + 7; x++)
        for (let z = cz - 8; z <= cz + 7; z++)
          for (let yy = y - 4; yy <= y - 1; yy++) {
            const b = bot.blockAt(new Vec3(x, yy, z));
            if (b && b.name === 'air') q[(x < cx ? 0 : 1) + (z < cz ? 0 : 2)]++;
          }
      return q;
    };
    const seen = count(a);
    console.log(`TNT on a chunk corner, destroyed per chunk as the client sees it: ${seen.join(' ')}`);
    late = await connectBot(port, user + 'L', opts);
    a.chat(`/tp ${user}L ${cx - 12} ${y + 8} ${cz - 12}`);
    await sleep(host ? 6000 : 2000);
    const later = count(late);
    console.log(`  and as a client that joins afterwards gets it: ${later.join(' ')}`);
    late.end();
    late = null;
    // 2. a boat over chunk borders: a lake 80 blocks across both ways, borders every 16
    const lx = ox, lz = oz;
    await say(`/tp ${user} ${lx} ${y + 3} ${lz}`);
    await sleep(1500);
    await say(`/fill ${lx - 40} ${y - 2} ${lz - 3} ${lx + 40} ${y - 1} ${lz + 3} water`);
    await say(`/fill ${lx - 3} ${y - 2} ${lz - 40} ${lx + 3} ${y - 1} ${lz + 40} water`);
    await sleep(500);
    await away(lx, lz);
    await say(`/tp ${user} ${lx + 5.5} ${y} ${lz + 5.5}`);   // on the bank, the water in reach
    await sleep(host ? 1500 : 500);
    await say(`/give ${user} oak_boat 1`);
    await waitFor(() => a.inventory.items().some((i) => i.name === 'oak_boat'), 5000, 'boat');
    await a.equip(a.inventory.items().find((i) => i.name === 'oak_boat'), 'hand');
    await a.lookAt(new Vec3(lx + 3.5, y - 0.1, lz + 2.5), true);
    console.log(`  (placing: at ${a.entity.position}, aiming at ${a.blockAt(new Vec3(lx + 3, y - 1, lz + 2)).name}` +
      `, holding ${a.heldItem && a.heldItem.name})`);
    a.activateItem();
    const boat = await waitFor(() => Object.values(a.entities).find((e) => e.name === 'oak_boat' &&
      Math.abs(e.position.x - lx) < 5 && Math.abs(e.position.z - lz) < 5), 5000, 'the boat');
    await sleep(1000);
    a.mount(boat);
    let riding = false;
    a._client.on('set_passengers', (pk) => { if (pk.entityId === boat.id) riding = pk.passengers.includes(a.entity.id); });
    await waitFor(() => riding, 3000, 'in the boat');
    const by = boat.position.y;
    const refused = [];
    a._client.on('vehicle_move', (pk) => refused.push([pk.x.toFixed(1), pk.z.toFixed(1)]));
    const drive = async (x0, z0, x1, z1) => {
      const n = Math.ceil(Math.hypot(x1 - x0, z1 - z0) / 0.3);
      for (let i = 1; i <= n; i++) {
        const x = x0 + (x1 - x0) * i / n, z = z0 + (z1 - z0) * i / n;
        a._client.write('vehicle_move', { x, y: by, z, yaw: 0, pitch: 0, onGround: false });
        await sleep(50);
      }
    };
    let px = boat.position.x, pz = boat.position.z;
    const legs = [[38.5, 0.5], [-38.5, 0.5], [0.5, 0.5], [0.5, 38.5], [0.5, -38.5], [0.5, 0.5]];
    for (const [dx, dz] of legs) {
      const x = lx + dx, z = lz + dz;
      const before = refused.length;
      await drive(px, pz, x, z);
      await sleep(300);
      console.log(`boat ${(px - lx).toFixed(1)} ${(pz - lz).toFixed(1)} -> ${dx} ${dz}: ` +
        `${refused.length - before ? 'sent back ' + (refused.length - before) + 'x, first at ' + refused[before].join(',') : 'through'}`);
      px = x; pz = z;
    }
    a._client.write('player_input', { inputs: { shift: true } });
    await sleep(300);
    assert.ok(seen.every((n) => n > 5), 'every chunk shows its part');
    assert.deepStrictEqual(seen, later, 'the same for a client that joins later');
    assert.strictEqual(refused.length, 0, 'never sent back');
    console.log('CHUNK BORDERS OK');
  } finally {
    for (const b of [a, late]) if (b) b.end();
    srv.stop();
    fs.rmSync(tmp, { recursive: true, force: true });
  }
})().catch((e) => { console.error(e); process.exit(1); });
