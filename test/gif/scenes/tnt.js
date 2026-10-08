'use strict';
// A line of TNT on a grass island: a redstone block primes the first one (80-tick
// fuse), and each explosion primes the next ones with a short fuse.
const X0 = 192, Y = 150, Z0 = -184, N = 12, STEP = 2;

module.exports = {
  name: 'tnt',
  order: 2,
  title: 'primed TNT and a chain reaction',
  seconds: 14,
  peaceful: true,
  async build(ctx) {
    const x1 = X0 + (N - 1) * STEP;
    ctx.look([X0 + (N - 1) * STEP / 2, Y + 9, Z0 + 16], [X0 + (N - 1) * STEP / 2, Y - 1, Z0]);
    await ctx.loaded(X0 - 4, Z0 - 4, x1 + 4, Z0 + 4);
    await ctx.cmd(`fill ${X0 - 4} ${Y - 3} ${Z0 - 4} ${x1 + 4} ${Y + 3} ${Z0 + 4} air`);
    await ctx.cmd(`fill ${X0 - 4} ${Y - 3} ${Z0 - 4} ${x1 + 4} ${Y - 2} ${Z0 + 4} dirt`);
    await ctx.cmd(`fill ${X0 - 4} ${Y - 1} ${Z0 - 4} ${x1 + 4} ${Y - 1} ${Z0 + 4} grass_block`);
    for (let i = 0; i < N; i++) await ctx.cmd(`setblock ${X0 + i * STEP} ${Y} ${Z0} tnt`);
    await ctx.waitFor(() => ctx.block(x1, Y, Z0)?.name === 'tnt', 30000, 'the TNT line');
  },
  async play(ctx) {
    await ctx.sleep(1000);
    await ctx.cmd(`setblock ${X0 - 1} ${Y} ${Z0} redstone_block`);
  },
  async check(ctx) {
    await ctx.cmd(`setblock ${X0 - 1} ${Y} ${Z0} redstone_block`);
    const names = () => Array.from({ length: N }, (_, i) => ctx.block(X0 + i * STEP, Y, Z0)?.name);
    await ctx.waitFor(() => names().every((n) => n !== 'tnt'), 30000, 'every TNT primed').catch((e) => {
      throw new Error(e.message + ': ' + names().join(','));
    });
    await ctx.waitFor(() => ctx.block(X0 + (N - 1) * STEP, Y - 1, Z0)?.name === 'air', 30000, 'the last one exploded');
  },
};
