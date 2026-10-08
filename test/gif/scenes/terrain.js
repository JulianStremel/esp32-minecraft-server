'use strict';
// A flight east over fresh terrain: the board generates the chunks ahead of the camera
// as it goes (seed 42: plains, forest and hills east of spawn).
const START = [200, 112, -184], SPEED = 9;   // blocks per second

function pose(ctx, t) {
  const x = START[0] + SPEED * t;
  const z = START[2] + 6 * Math.sin(t / 3);
  ctx.look([x, START[1], z], [x + 24, START[1] - 22, z + 2 * Math.cos(t / 3)]);
  return x;
}

module.exports = {
  name: 'terrain',
  order: 7,
  title: 'flying over terrain generated on the board',
  seconds: 18,
  peaceful: true,
  movesCamera: true,
  viewDistance: 4,
  fps: 8,
  gifWidth: 400,
  colors: 48,   // terrain changes in every frame: keep the file small
  settleMs: 15000,
  async build(ctx) {
    pose(ctx, 0);
    await ctx.loaded(START[0] - 16, START[2] - 16, START[0] + 32, START[2] + 16, 40);
  },
  async play(ctx) {
    const t0 = Date.now();
    while (!ctx.stopped) {
      pose(ctx, (Date.now() - t0) / 1000);
      await ctx.sleep(50);
    }
  },
  async check(ctx) {
    const t0 = Date.now();
    let x = START[0];
    while (Date.now() - t0 < 10000) {
      x = pose(ctx, (Date.now() - t0) / 1000);
      await ctx.sleep(50);
    }
    await ctx.waitFor(() => ctx.block(Math.floor(x) + 16, 40, START[2]), 30000, 'terrain ahead of the camera');
  },
};
