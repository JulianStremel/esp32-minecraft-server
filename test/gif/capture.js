'use strict';
// Shared parts of the scene recordings (see docs/GIFS.md and record.js):
//   - the server: the board (a fresh NBD world image per scene, the board reset into it,
//     commands through its serial console) or the PC build (--local, a RAM world)
//   - the camera: an operator bot in spectator mode whose pose the scene sets
//   - recording: prismarine-viewer draws what the camera knows in headless Chromium,
//     a Playwright screencast collects the frames, ffmpeg makes the GIF
const { execFileSync, spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { Vec3 } = require('vec3');
const { startServer, connectBot, sleep, waitFor, nextChat } = require('../lib');

// prismarine-viewer's unused headless renderer needs the native `canvas` package
const Module = require('module');
const realLoad = Module._load;
Module._load = function (request, ...rest) {
  if (request === 'canvas') return { createCanvas: () => { throw new Error('canvas is not available'); } };
  return realLoad.call(this, request, ...rest);
};

const ROOT = path.join(__dirname, '..', '..');
const CAMERA = 'Tester';   // an operator in the board's config.h and on the PC server

// ------------------------------------------------------------------ the server
// The board: serve a fresh world image over NBD, reset the board into it, and talk to
// its serial console (commands without the chat's spam limit).
async function startBoard({ host, serial, python, image, nbdPort = 10809, log = false }) {
  if (fs.existsSync(image)) fs.rmSync(image);
  fs.mkdirSync(path.dirname(image), { recursive: true });
  const nbd = spawn(process.env.NBD_PYTHON || 'python',
    ['-I', path.join(ROOT, 'tools', 'nbd_server.py'), '--file', image, '--size', '16G', '--port', String(nbdPort)],
    { stdio: ['ignore', 'pipe', 'pipe'] });
  let nbdOut = '';
  nbd.stdout.on('data', (d) => { nbdOut += d; });
  nbd.stderr.on('data', (d) => { nbdOut += d; });
  await sleep(1500);
  if (nbd.exitCode !== null) throw new Error('NBD server did not start:\n' + nbdOut);
  const mon = spawn(python, [path.join(ROOT, 'tools', 'idf', 'serial_monitor.py'), serial, '--reset'],
    { stdio: ['pipe', 'pipe', 'pipe'] });
  const state = { listening: false, fault: null, lines: [] };
  let buf = '';
  const onData = (d) => {
    buf += d.toString();
    let nl;
    while ((nl = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, nl).replace(/\r$/, '');
      buf = buf.slice(nl + 1);
      state.lines.push(line);
      if (state.lines.length > 400) state.lines.shift();
      if (/listening on port/.test(line)) state.listening = true;
      if (/Guru Meditation|assert failed|\[FATAL\]|^Backtrace:|abort\(\)/.test(line)) state.fault = new Error('firmware: ' + line);
      if (log) console.log('  | ' + line);
    }
  };
  mon.stdout.on('data', onData);
  mon.stderr.on('data', onData);
  await waitFor(() => state.listening || state.fault, 180000, 'the board to boot into the new world');
  if (state.fault) throw state.fault;
  return {
    host,
    port: 25565,
    dashboard: `http://${host}/`,   // builds with MC_DASHBOARD
    console: (line) => mon.stdin.write(line.replace(/^\//, '') + '\n'),
    fault: () => state.fault,
    log: () => state.lines.join('\n'),
    stop: async () => {
      mon.kill();
      nbd.kill();
      await sleep(500);
    },
  };
}

async function startLocal({ log = false }) {
  const port = 25600 + Math.floor(Math.random() * 300);
  const s = startServer(['--port', String(port), '--seed', '42', '--view', '6', '--ops', CAMERA, '--mem', '512',
    '--dashboard', String(port + 1000)], { log });
  await s.ready;
  return {
    host: '127.0.0.1',
    port,
    dashboard: `http://127.0.0.1:${port + 1000}/`,
    console: (line) => s.command(line.replace(/^\//, '')),
    fault: () => null,
    log: () => s.output(),
    stop: () => s.stop(),
  };
}

// ------------------------------------------------------------------ the scene context
const toNotchYaw = (yaw) => (180 / Math.PI) * (Math.PI - yaw);
const toNotchPitch = (pitch) => (180 / Math.PI) * -pitch;

function makeContext(server, camera) {
  const bots = [camera];
  const ctx = {
    Vec3, sleep, waitFor, camera, server,
    // a server console command (overworld); paced so the board's console keeps up
    async cmd(text) {
      server.console(text);
      await sleep(server.host === '127.0.0.1' ? 40 : 120);
    },
    // a command typed by the camera (an operator) where the console cannot reach: the
    // camera's own dimension. Chat is rate-limited by the server (vanilla's spam rule).
    async chat(text, bot = camera) {
      bot.chat(text);
      await sleep(700);
    },
    block: (x, y, z, bot = camera) => bot.blockAt(new Vec3(x, y, z)),
    // the camera at `pos`, looking at `target`
    look(pos, target, bot = camera) {
      const [x, y, z] = pos;
      const dx = target[0] - x, dy = target[1] - (y + 1.62), dz = target[2] - z;
      const yaw = Math.atan2(-dx, -dz), pitch = Math.atan2(dy, Math.sqrt(dx * dx + dz * dz));
      // the server refuses moves over 100 blocks ("moved too quickly") and puts the player
      // back: a long jump goes through /tp
      if (bot.entity.position.distanceTo(new Vec3(x, y, z)) > 50) server.console(`tp ${bot.username} ${x} ${y} ${z}`);
      bot.entity.position = new Vec3(x, y, z);
      bot.entity.yaw = yaw;
      bot.entity.pitch = pitch;
      bot._client.write('position_look', { x, y, z, yaw: toNotchYaw(yaw), pitch: toNotchPitch(pitch),
        flags: { onGround: false, hasHorizontalCollision: false } });   // 1.21.8: flags, not onGround
      bot.emit('move');
      if (bot === camera) ctx.pose = [pos, target];   // the recorder holds the camera there
    },
    // another player (an actor in the scene): survival unless told otherwise
    async addBot(name, { mode = 'survival' } = {}) {
      const b = await connectBot(server.port, name, { host: server.host, checkTimeoutInterval: 600000 });
      bots.push(b);
      await ctx.cmd(`gamemode ${mode} ${name}`);
      return b;
    },
    // waits until the camera has the blocks of a box (its chunks were sent)
    async loaded(x0, z0, x1, z1, y = 64) {
      await waitFor(() => [[x0, z0], [x1, z0], [x0, z1], [x1, z1]].every(([x, z]) => camera.blockAt(new Vec3(x, y, z))),
        120000, 'the scene\'s chunks');
    },
    // a platform of smooth stone with air above it
    async platform(x0, y, z0, x1, z1, height = 6, floor = 'smooth_stone') {
      await ctx.cmd(`fill ${x0} ${y} ${z0} ${x1} ${y + height} ${z1} air`);
      await ctx.cmd(`fill ${x0} ${y - 1} ${z0} ${x1} ${y - 1} ${z1} ${floor}`);
    },
    bots,
  };
  return ctx;
}

// ------------------------------------------------------------------ recording
async function record(ctx, scene, { out, width: gifWidth, height: gifHeight, fps, viewDistance }) {
  // the browser window (a scene may want more room, e.g. the dashboard page) and the GIF
  const width = scene.viewport ? scene.viewport[0] : gifWidth, height = scene.viewport ? scene.viewport[1] : gifHeight;
  if (scene.gifWidth) gifWidth = scene.gifWidth;
  // what is filmed: the camera's view, or a web page (scene.page: the dashboard)
  let url = scene.page ? scene.page(ctx) : null;
  if (!url) {
    const viewerPort = 27000 + Math.floor(Math.random() * 1000);
    require('prismarine-viewer').mineflayer(ctx.camera, { port: viewerPort, firstPerson: true, viewDistance });
    url = `http://127.0.0.1:${viewerPort}`;
  }
  const { chromium } = require('playwright');
  const browser = await chromium.launch({
    args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'],
  });
  const frameDir = fs.mkdtempSync(path.join(os.tmpdir(), 'mcscene-'));
  try {
    const context = await browser.newContext({ viewport: { width, height } });
    const page = await context.newPage();
    // keep the camera where the scene put it (the server may move it, e.g. when someone
    // joins); scenes that fly the camera set it themselves
    const hold = setInterval(() => { if (ctx.pose && !scene.movesCamera) ctx.look(...ctx.pose); }, 250);
    if (process.env.GIF_DEBUG) {
      const p = ctx.camera.entity.position;
      console.log(`    camera ${p.x.toFixed(1)} ${p.y.toFixed(1)} ${p.z.toFixed(1)}, pose ${JSON.stringify(ctx.pose)}, game mode ${ctx.camera.game.gameMode}, dim ${ctx.camera.game.dimension}`);
      ctx.camera.on('forcedMove', () => { const q = ctx.camera.entity.position; console.log(`    forcedMove to ${q.x.toFixed(1)} ${q.y.toFixed(1)} ${q.z.toFixed(1)}`); });
      ctx.camera.on('death', () => console.log('    the camera died'));
      ctx.camera.on('respawn', () => console.log('    the camera respawned'));
    }
    await page.goto(url);
    await page.waitForSelector(scene.page ? 'main' : 'canvas', { timeout: 60000 });
    await sleep(scene.settleMs || 12000);   // the browser meshes the chunks around the camera
    const seconds = scene.seconds;
    const frames = [];
    let t0 = Infinity;
    const cdp = await context.newCDPSession(page);
    cdp.on('Page.screencastFrame', ({ data, metadata, sessionId }) => {
      cdp.send('Page.screencastFrameAck', { sessionId }).catch(() => {});
      const ts = Math.round(metadata.timestamp * 1000);
      if (ts < t0 || ts > t0 + seconds * 1000) return;
      const file = path.join(frameDir, `s${String(frames.length).padStart(5, '0')}.jpg`);
      fs.writeFileSync(file, Buffer.from(data, 'base64'));
      frames.push({ file, ts });
    });
    await cdp.send('Page.startScreencast', { format: 'jpeg', quality: 92, maxWidth: width, maxHeight: height });
    t0 = Date.now();
    const playing = scene.play(ctx).catch((e) => { console.log('  play: ' + e.message); });
    await sleep(seconds * 1000 + 300);
    await cdp.send('Page.stopScreencast').catch(() => {});
    ctx.stopped = true;   // play() loops end
    await Promise.race([playing, sleep(5000)]);
    clearInterval(hold);
    if (!frames.length) throw new Error('no frames captured');
    const list = frames.map((f, i) => {
      const next = i + 1 < frames.length ? frames[i + 1].ts : t0 + seconds * 1000;
      return `file '${f.file.replace(/\\/g, '/')}'\nduration ${(Math.max(1, next - f.ts) / 1000).toFixed(3)}`;
    }).join('\n') + `\nfile '${frames[frames.length - 1].file.replace(/\\/g, '/')}'\n`;
    fs.writeFileSync(path.join(frameDir, 'list.txt'), list);
    const vf = `fps=${fps},scale=${gifWidth}:-1:flags=lanczos,split[a][b];` +
      `[a]palettegen=max_colors=${scene.colors || 128}:stats_mode=diff[p];` +
      '[b][p]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle';
    fs.mkdirSync(path.dirname(out), { recursive: true });
    execFileSync(process.env.FFMPEG || 'ffmpeg', ['-y', '-loglevel', 'error', '-f', 'concat', '-safe', '0', '-i',
      path.join(frameDir, 'list.txt'), '-vf', vf, '-loop', '0', out], { stdio: 'inherit' });
    return { frames: frames.length, mb: fs.statSync(out).size / 1048576 };
  } finally {
    await browser.close().catch(() => {});
    fs.rmSync(frameDir, { recursive: true, force: true });
  }
}

// ------------------------------------------------------------------ one scene
async function runScene(scene, opts) {
  const server = opts.local
    ? await startLocal(opts)
    : await startBoard({ ...opts, image: path.join(ROOT, 'build', 'hw', `gif-${scene.name}.img`) });
  let ctx;
  try {
    const camera = await connectBot(server.port, CAMERA, { host: server.host, checkTimeoutInterval: 600000 });
    camera.physicsEnabled = false;
    ctx = makeContext(server, camera);
    await ctx.cmd(`gamemode spectator ${CAMERA}`);
    await ctx.cmd(`time set ${scene.time ?? 6000}`);
    await ctx.cmd('weather clear');
    if (scene.peaceful) await ctx.cmd('difficulty peaceful');
    await scene.build(ctx);
    if (opts.check) {
      await scene.check(ctx);
      return { checked: true };
    }
    const out = path.resolve(ROOT, opts.outDir || 'docs/images', `${scene.name}.gif`);
    const r = await record(ctx, scene, {
      out, width: opts.width, height: opts.height, fps: scene.fps || opts.fps, viewDistance: scene.viewDistance || 3,
    });
    if (server.fault()) throw server.fault();
    return { out, ...r };
  } finally {
    if (ctx) ctx.bots.forEach((b) => b.quit());
    await sleep(500);
    await server.stop();
  }
}

module.exports = { runScene, ROOT, CAMERA, Vec3, sleep, waitFor, nextChat };
