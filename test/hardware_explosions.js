'use strict';
// What explosions cost on the board: one TNT, then a block of 50 going off as a chain
// reaction, far from spawn; /lag after each (time per blast, the most in one tick, the
// slowest loop pass).
//   node hardware_explosions.js --host 192.168.1.160 [--user Tester] [--at 600,600]
const mineflayer = require('mineflayer');
const { Vec3 } = require('vec3');

const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i > 0 ? process.argv[i + 1] : d; };
const host = arg('host', '192.168.1.160');
const user = arg('user', 'Tester');
const [ax, az] = arg('at', '600,600').split(',').map(Number);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

(async () => {
  const bot = mineflayer.createBot({ host, port: 25565, username: user, version: '1.21.8', auth: 'offline' });
  await new Promise((resolve, reject) => { bot.once('spawn', resolve); bot.once('kicked', reject); bot.once('error', reject); });
  const lines = [];
  bot.on('messagestr', (m) => lines.push(m));
  const say = async (c, wait = 400) => { bot.chat(c); await sleep(wait); };
  const lag = async () => {
    lines.length = 0;
    await say('/lag', 1500);
    return lines.filter((l) => /Explosions:|Slowest loop/.test(l));
  };
  await say('/gamemode creative');
  await say(`/tp ${ax} 200 ${az}`, 1000);
  // dry ground near there: the first column whose top block is not water or leaves
  const topAt = (x, z) => {
    for (let yy = 199; yy > -60; yy--) {
      const b = bot.blockAt(new Vec3(x, yy, z));
      if (!b) return null;
      if (b.name !== 'air' && b.name !== 'cave_air') return { y: yy, name: b.name };
    }
    return null;
  };
  let ground = null, gx = ax, gz = az;
  for (let t = 0; t < 60 && ground === null; t++) {
    await sleep(500);
    for (let r = 0; r <= 40 && ground === null; r += 4)
      for (let dx = -r; dx <= r && ground === null; dx += 4)
        for (const dz of [-r, r]) {
          const top = topAt(ax + dx, az + dz);
          if (top && !/water|leaves|ice/.test(top.name)) { ground = top.y; gx = ax + dx; gz = az + dz; break; }
        }
  }
  if (ground === null) throw new Error('no dry terrain loaded there');
  const y = ground + 1;
  console.log(`dry ground at ${gx} ${ground} ${gz}`);
  await say(`/tp ${gx - 20} ${y + 10} ${gz}`, 1000);
  console.log('before:', (await lag()).join(' || '));
  // 1. one TNT
  await say(`/setblock ${gx} ${y} ${gz} tnt`);
  await say(`/setblock ${gx - 1} ${y} ${gz} redstone_block`, 6000);
  console.log('one TNT:', (await lag()).join(' || '));
  // 2. 50 TNT, 5 x 2 x 5
  const bx = gx + 12;
  // on a block of dirt, so the chain is not over water (TNT in water breaks nothing)
  await say(`/fill ${bx - 5} ${y - 6} ${gz - 5} ${bx + 9} ${y - 1} ${gz + 9} dirt`);
  await say(`/fill ${bx - 5} ${y} ${gz - 5} ${bx + 9} ${y + 6} ${gz + 9} air`);
  await say(`/fill ${bx} ${y} ${gz} ${bx + 4} ${y + 1} ${gz + 4} tnt`);
  await say(`/setblock ${bx - 1} ${y} ${gz + 2} redstone_block`, 12000);
  console.log('50 TNT:', (await lag()).join(' || '));
  bot.end();
})().catch((e) => { console.error(e); process.exit(1); });
