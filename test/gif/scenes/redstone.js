'use strict';
// A repeater chain lighting eight lamps one after another, and a dust line driving a
// sticky piston; the power is switched every 2 s.
const X0 = 192, Y = 150, Z0 = -176, LAMPS = 8;
const RIPPLE_Z = Z0, PISTON_Z = Z0 + 3;

async function power(ctx, on) {
  for (const z of [RIPPLE_Z, PISTON_Z]) await ctx.cmd(`setblock ${X0} ${Y} ${z} ${on ? 'redstone_block' : 'air'}`);
}
const lamp = (ctx, i) => ctx.block(X0 + 2 + 2 * i, Y, RIPPLE_Z);

module.exports = {
  name: 'redstone',
  order: 1,
  title: 'repeater chain lighting lamps in sequence, sticky piston',
  seconds: 16,
  async build(ctx) {
    ctx.look([X0 + LAMPS + 1, Y + 4, Z0 + 9], [X0 + LAMPS + 1, Y, Z0 + 1.5]);
    await ctx.loaded(X0 - 2, Z0 - 2, X0 + 2 * LAMPS + 3, Z0 + 5);
    await ctx.cmd(`fill ${X0 - 2} ${Y - 1} ${Z0 - 2} ${X0 + 2 * LAMPS + 3} ${Y + 2} ${Z0 + 5} air`);
    await ctx.cmd(`fill ${X0 - 1} ${Y - 1} ${Z0 - 1} ${X0 + 2 * LAMPS + 1} ${Y - 1} ${Z0 + 4} smooth_stone`);
    // a repeater takes its input from the side its `facing` names: west, so the signal runs east
    for (let i = 0; i < LAMPS; i++) {
      await ctx.cmd(`setblock ${X0 + 1 + 2 * i} ${Y} ${RIPPLE_Z} repeater[facing=west,delay=2]`);
      await ctx.cmd(`setblock ${X0 + 2 + 2 * i} ${Y} ${RIPPLE_Z} redstone_lamp`);
    }
    await ctx.cmd(`fill ${X0 + 1} ${Y} ${PISTON_Z} ${X0 + 12} ${Y} ${PISTON_Z} redstone_wire`);
    await ctx.cmd(`setblock ${X0 + 13} ${Y} ${PISTON_Z} sticky_piston[facing=east]`);
    await ctx.cmd(`setblock ${X0 + 14} ${Y} ${PISTON_Z} gold_block`);
    await ctx.waitFor(() => ctx.block(X0 + 14, Y, PISTON_Z)?.name === 'gold_block' && lamp(ctx, LAMPS - 1)?.name === 'redstone_lamp',
      30000, 'the circuit');
  },
  async play(ctx) {
    let on = false;
    while (!ctx.stopped) {
      on = !on;
      await power(ctx, on);
      await ctx.sleep(2000);
    }
  },
  async check(ctx) {
    for (let cycle = 0; cycle < 2; cycle++) {
      await power(ctx, true);
      await ctx.waitFor(() => lamp(ctx, LAMPS - 1)?.getProperties().lit === true, 10000, 'last lamp on');
      for (let i = 0; i < LAMPS; i++) if (lamp(ctx, i)?.getProperties().lit !== true) throw new Error(`lamp ${i} is off`);
      await ctx.waitFor(() => ctx.block(X0 + 15, Y, PISTON_Z)?.name === 'gold_block', 10000, 'gold block pushed');
      await power(ctx, false);
      await ctx.waitFor(() => lamp(ctx, LAMPS - 1)?.getProperties().lit === false, 10000, 'last lamp off');
      await ctx.waitFor(() => ctx.block(X0 + 14, Y, PISTON_Z)?.name === 'gold_block', 10000, 'gold block pulled back');
    }
  },
};
