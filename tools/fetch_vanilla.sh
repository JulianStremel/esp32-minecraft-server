#!/bin/sh
# Downloads the official Minecraft 1.21.8 server jar (from Mojang, checked against its
# SHA-1) into tools/vanilla/1.21.8/ and extracts the data pack (registries, tags,
# worldgen) that gen_data.js reads. Nothing from the jar is committed; only the compact
# generated tables are.
set -e
cd "$(dirname "$0")"
V=1.21.8
SHA1=6bce4ef400e4efaa63a13d5e6f6b500be969ef81
mkdir -p vanilla/$V
cd vanilla/$V
if [ ! -f server.jar ]; then
  curl -sS "https://piston-data.mojang.com/v1/objects/$SHA1/server.jar" -o server.jar
fi
echo "$SHA1  server.jar" | sha1sum -c -
# the jar is a bundler: the server and its data are in META-INF/versions/<version>/
unzip -q -o server.jar "META-INF/versions/$V/server-$V.jar"
rm -rf jar
unzip -q -o "META-INF/versions/$V/server-$V.jar" -x '*.class' -d jar
echo "extracted the data pack to tools/vanilla/$V/jar/data/minecraft"
