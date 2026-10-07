'use strict';
// Runs all end-to-end suites sequentially. Build the server first: make -C host server
// (set SERVER_BIN=host/build-san/mcserver to run them against the sanitizer build).
const { spawnSync } = require('child_process');
const path = require('path');

const suites = ['smoke.js', 'gameplay.js', 'persistence.js'];
let failed = 0;
for (const s of suites) {
  console.log(`\n=== ${s}`);
  const r = spawnSync(process.execPath, [path.join(__dirname, s)], { stdio: 'inherit', env: process.env, timeout: 600000 });
  if (r.status !== 0) { failed++; console.log(`=== ${s} FAILED (${r.status})`); }
}
console.log(failed ? `\n${failed} suite(s) failed` : '\nall suites passed');
process.exit(failed ? 1 : 0);
