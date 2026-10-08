'use strict';
// Water and lava poured on top of a stone staircase flow down it (lava slower), and a
// sand and gravel tower falls when its support is removed.
const X0 = 192, Y = 150, Z0 = -184, STEPS = 6;

module.exports = {
  name: 'fluids',
  order: 3,
  title: 'water and lava flowing down steps, falling sand and gravel',
  seconds: 16,
  peaceful: true,
  async build(ctx) {
    ctx.look([X0 + 7, Y + 10, Z0 + 20], [X0 + 7, Y + 1, Z0 + 3]);
    await ctx.loaded(X0 - 2, Z0 - 2, X0 + 18, Z0 + 10);
    await ctx.cmd(`fill ${X0 - 2} ${Y - 1} ${Z0 - 2} ${X0 + 18} ${Y + 12} ${Z0 + 10} air`);
    await ctx.cmd(`fill ${X0 - 2} ${Y - 1} ${Z0 - 2} ${X0 + 18} ${Y - 1} ${Z0 + 10} smooth_stone`);
    // two staircases going down eastwards, 3 blocks wide, one for water, one for lava
    for (const [z0, z1] of [[Z0, Z0 + 2], [Z0 + 5, Z0 + 7]]) {
      for (let s = 0; s < STEPS; s++) {
        const h = STEPS - 1 - s;
        if (h > 0) await ctx.cmd(`fill ${X0 + 2 * s} ${Y} ${z0} ${X0 + 2 * s + 1} ${Y + h - 1} ${z1} stone`);
      }
      // walls along the sides keep the fluid on the steps
      await ctx.cmd(`fill ${X0} ${Y} ${z0 - 1} ${X0 + 2 * STEPS + 2} ${Y + STEPS} ${z0 - 1} glass`);
      await ctx.cmd(`fill ${X0} ${Y} ${z1 + 1} ${X0 + 2 * STEPS + 2} ${Y + STEPS} ${z1 + 1} glass`);
    }
    // sand and gravel on a support block
    await ctx.cmd(`setblock ${X0 + 16} ${Y + 4} ${Z0 + 3} stone`);
    await ctx.cmd(`fill ${X0 + 16} ${Y + 5} ${Z0 + 3} ${X0 + 16} ${Y + 7} ${Z0 + 3} sand`);
    await ctx.cmd(`fill ${X0 + 16} ${Y + 8} ${Z0 + 3} ${X0 + 16} ${Y + 10} ${Z0 + 3} gravel`);
    await ctx.cmd(`fill ${X0 + 16} ${Y} ${Z0 + 3} ${X0 + 16} ${Y + 3} ${Z0 + 3} air`);
    await ctx.waitFor(() => ctx.block(X0 + 16, Y + 10, Z0 + 3)?.name === 'gravel', 30000, 'the tower');
  },
  async pour(ctx) {
    await ctx.cmd(`setblock ${X0} ${Y + STEPS - 1} ${Z0 + 1} water`);
    await ctx.cmd(`setblock ${X0} ${Y + STEPS - 1} ${Z0 + 6} lava`);
  },
  async play(ctx) {
    await ctx.sleep(1000);
    await module.exports.pour(ctx);
    await ctx.sleep(7000);
    await ctx.cmd(`setblock ${X0 + 16} ${Y + 4} ${Z0 + 3} air`);
  },
  async check(ctx) {
    await module.exports.pour(ctx);
    const bottom = X0 + 2 * STEPS;
    await ctx.waitFor(() => ctx.block(bottom, Y, Z0 + 1)?.name === 'water', 30000, 'water at the bottom');
    await ctx.waitFor(() => ctx.block(bottom - 2, Y, Z0 + 6)?.name === 'lava', 60000, 'lava near the bottom');
    await ctx.cmd(`setblock ${X0 + 16} ${Y + 4} ${Z0 + 3} air`);
    await ctx.waitFor(() => ctx.block(X0 + 16, Y, Z0 + 3)?.name === 'sand', 20000, 'sand landed');
  },
};
