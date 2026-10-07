#!/bin/sh
# Downloads the official Minecraft 1.16.5 server jar (from Mojang) into tools/vanilla/
# and extracts the data-pack tag files used by gen_data.js. Nothing from the jar is
# committed; only the compact generated tables are.
set -e
cd "$(dirname "$0")"
mkdir -p vanilla
cd vanilla
if [ ! -f server.jar ]; then
  curl -sS https://launchermeta.mojang.com/mc/game/version_manifest.json -o manifest.json
  URL=$(python3 -c "import json;m=json.load(open('manifest.json'));print([v['url'] for v in m['versions'] if v['id']=='1.16.5'][0])")
  curl -sS "$URL" -o version.json
  SURL=$(python3 -c "import json;print(json.load(open('version.json'))['downloads']['server']['url'])")
  curl -sS "$SURL" -o server.jar
fi
rm -rf data
unzip -q -o server.jar 'data/minecraft/tags/*'
echo "extracted tags to tools/vanilla/data/minecraft/tags"
