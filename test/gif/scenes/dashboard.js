'use strict';
// The status dashboard served by the board (a firmware built with MC_DASHBOARD): players
// join one after another and fly off into new terrain; the page shows it as it happens.
const START = [200, 110, -184];

module.exports = {
  name: 'dashboard',
  order: 8,
  title: 'the status dashboard while players join and explore',
  seconds: 20,
  peaceful: true,
  movesCamera: true,
  settleMs: 4000,
  viewport: [1000, 760],   // the cards, the world and the player list
  gifWidth: 640,
  page: (ctx) => ctx.server.dashboard,
  async build(ctx) {
    ctx.look(START, [START[0] + 10, START[1] - 10, START[2]]);
    await ctx.loaded(START[0] - 8, START[2] - 8, START[0] + 8, START[2] + 8, 40);
  },
  async play(ctx) {
    const names = ['Alex', 'Steve', 'Notch'];
    const flyers = [];
    const t0 = Date.now();
    for (const [i, name] of names.entries()) {
      if (ctx.stopped) break;
      await ctx.sleep(i === 0 ? 1500 : 3000);
      const b = await ctx.addBot(name, { mode: 'spectator' });
      b.physicsEnabled = false;
      flyers.push({ b, dir: [[1, 0], [0, 1], [-1, 0.4]][i], since: Date.now() });
    }
    while (!ctx.stopped) {
      for (const f of flyers) {
        const d = ((Date.now() - f.since) / 1000) * 10;   // 10 blocks per second
        ctx.look([START[0] + f.dir[0] * d, START[1], START[2] + f.dir[1] * d],
          [START[0] + f.dir[0] * (d + 10), START[1] - 5, START[2] + f.dir[1] * (d + 10)], f.b);
      }
      await ctx.sleep(100);
      if (Date.now() - t0 > 60000) break;
    }
  },
  async check(ctx) {
    const http = require('http');
    const body = await new Promise((res, rej) => http.get(ctx.server.dashboard + 'api/status', (r) => {
      let s = '';
      r.on('data', (d) => { s += d; });
      r.on('end', () => res(s));
    }).on('error', rej));
    const j = JSON.parse(body);
    if (!(j.perf && j.players)) throw new Error('no dashboard status');
  },
};
