'use strict';
// Records the feature GIFs (docs/GIFS.md), one scene at a time, each in a fresh world:
//
//   node test/gif/record.js --host <board ip> --serial COM5 --python <idf python> [scene ...]
//   SERVER_BIN=.../mcserver node test/gif/record.js --local [scene ...]
//   options: --check (build each scene and verify it, no recording), --list,
//            --out-dir docs/images, --width 640 --height 360 --fps 12, --log
//
// On the board every scene gets its own world image (build/hw/gif-<scene>.img, made
// fresh), served by tools/nbd_server.py; the board is reset over USB so it boots into
// it. Recording needs Playwright's Chromium (cd test && npx playwright install chromium)
// and ffmpeg (on PATH, or FFMPEG=<path>).
const fs = require('fs');
const path = require('path');
const { runScene } = require('./capture');

const args = process.argv.slice(2);
const opt = (name, def) => (args.indexOf('--' + name) >= 0 ? args[args.indexOf('--' + name) + 1] : def);
const flag = (name) => args.includes('--' + name);
const valued = ['host', 'serial', 'python', 'out-dir', 'width', 'height', 'fps'];
const names = args.filter((a, i) => !a.startsWith('--') && !(i > 0 && valued.includes(args[i - 1].slice(2))));

const dir = path.join(__dirname, 'scenes');
const all = fs.readdirSync(dir).filter((f) => f.endsWith('.js')).map((f) => require(path.join(dir, f)))
  .sort((a, b) => (a.order || 99) - (b.order || 99));
if (flag('list')) {
  for (const s of all) console.log(`${s.name.padEnd(16)} ${s.title}`);
  process.exit(0);
}
const scenes = names.length ? names.map((n) => {
  const s = all.find((x) => x.name === n);
  if (!s) throw new Error(`no scene ${n} (--list shows them)`);
  return s;
}) : all;

const opts = {
  local: flag('local'), check: flag('check'), log: flag('log'),
  host: opt('host'), serial: opt('serial'), python: opt('python', process.env.IDF_PYTHON || 'python'),
  outDir: opt('out-dir'), width: parseInt(opt('width', '640')), height: parseInt(opt('height', '360')),
  fps: parseFloat(opt('fps', '12')),
};
if (!opts.local && (!opts.host || !opts.serial)) {
  console.error('usage: record.js --host <board ip> --serial <port> [--python <idf python>] [scene ...] | --local');
  process.exit(2);
}

(async () => {
  const results = [];
  for (const scene of scenes) {
    const t0 = Date.now();
    console.log(`=== ${scene.name}: ${scene.title}`);
    try {
      const r = await runScene(scene, opts);
      const s = ((Date.now() - t0) / 1000).toFixed(0);
      const line = r.checked ? `checked (${s} s)` : `${path.relative(process.cwd(), r.out)}: ${r.frames} frames, ${r.mb.toFixed(1)} MB (${s} s)`;
      console.log('    ' + line);
      results.push([scene.name, 'OK', line]);
    } catch (e) {
      console.log('    FAILED: ' + (e.stack || e.message));
      results.push([scene.name, 'FAILED', e.message]);
    }
  }
  console.log('\n' + results.map(([n, s, l]) => `${n.padEnd(16)} ${s.padEnd(7)} ${l}`).join('\n'));
  process.exit(results.some((r) => r[1] !== 'OK') ? 1 : 0);
})();
