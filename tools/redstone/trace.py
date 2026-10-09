#!/usr/bin/env python3
"""Record deterministic per-tick circuits from the official 1.16.5 server.

The traces hold 1.16.5 block state ids: after recording, run remap_traces.js to translate
them to the 1.21.8 numbering the server uses.

Uses a private temporary world, a tick-function data pack, and loopback-only RCON.
The official jar must already be cached by generate.py. --accept-eula is required
when starting this reference server (https://aka.ms/MinecraftEULA).
Only the test inputs and recorded numeric traces are committed, never the jar/world.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import secrets
import socket
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
SHA1 = '1b557e7b033b583cd9f66746b7a9ab1ec1673ced'
CASES = ROOT / 'tools/redstone/circuits.json'
RESULT = ROOT / 'tools/redstone/traces.json'
INCLUDE = ROOT / 'host/tests/data/redstone_traces.inc'


def function(folder, name, commands):
    (folder / (name + '.mcfunction')).write_text('\n'.join(commands) + '\n')


def make_pack(world, cases):
    pack = world / 'datapacks/redstone_trace'
    functions = pack / 'data/redstone_trace/functions'
    functions.mkdir(parents=True)
    tags = pack / 'data/minecraft/tags/functions'
    tags.mkdir(parents=True)
    (pack / 'pack.mcmeta').write_text(json.dumps({'pack': {'pack_format': 6, 'description': 'Redstone transition oracle'}}))
    (tags / 'load.json').write_text('{"values":["redstone_trace:load"]}')
    (tags / 'tick.json').write_text('{"values":["redstone_trace:tick"]}')
    function(functions, 'load', ['scoreboard objectives add trace dummy', 'scoreboard players set #active trace 0'])
    function(functions, 'tick', [f'execute if score #active trace matches 1 if score #case trace matches {i} run function redstone_trace:case_{i}' for i in range(len(cases))])
    for i, case in enumerate(cases):
        function(functions, f'start_{i}', [
            'scoreboard players set #active trace 0', 'kill @e',
            'fill -16 79 -8 31 86 8 air', 'fill -16 79 -8 31 79 8 stone',
            'data modify storage redstone_trace:result samples set value []',
            f'scoreboard players set #case trace {i}',
            f'scoreboard players set #step trace {-case["settle"]}',
            'scoreboard players set #active trace 1'])
        commands = []
        for action in [dict(tick=-case['settle'], **b) for b in case['setup']] + case['actions']:
            pos = ' '.join(map(str, action['pos']))
            commands.append(f'execute if score #step trace matches {action["tick"]} run setblock {pos} minecraft:{action["state"]}')
        commands += ['execute if score #step trace matches 0.. run function redstone_trace:sample_' + str(i),
                     'scoreboard players add #step trace 1',
                     f'execute if score #step trace matches {case["ticks"] + 1}.. run scoreboard players set #active trace 0']
        function(functions, f'case_{i}', commands)
        sample = []
        for obs in case['observe']:
            pos = ' '.join(map(str, obs['pos']))
            sample.append('scoreboard players set #value trace -1')
            if 'blocks' in obs:
                states = obs['blocks']
            else:
                states = [f'{obs["block"]}[{obs["property"]}={v}]' for v in obs['values']]
            for value, state in enumerate(states):
                sample.append(f'execute if block {pos} minecraft:{state} run scoreboard players set #value trace {value}')
            sample += ['data modify storage redstone_trace:result samples append value 0',
                       'execute store result storage redstone_trace:result samples[-1] int 1 run scoreboard players get #value trace']
        function(functions, f'sample_{i}', sample)


class Rcon:
    def __init__(self, port, password):
        self.socket = socket.create_connection(('127.0.0.1', port), timeout=15)
        self.sequence = 0
        if self.send(password, 3)[0] == -1:
            raise RuntimeError('Reference RCON authentication failed')

    def exact(self, size):
        data = b''
        while len(data) < size:
            chunk = self.socket.recv(size - len(data))
            if not chunk:
                raise RuntimeError('Reference RCON closed')
            data += chunk
        return data

    def send(self, command, kind=2):
        self.sequence += 1
        request = self.sequence
        body = struct.pack('<ii', request, kind) + command.encode() + b'\0\0'
        self.socket.sendall(struct.pack('<i', len(body)) + body)
        # Minecraft splits large RCON replies into 4096-byte messages. A separate
        # read-only command provides an unambiguous end marker for this reply.
        barrier = None
        if kind == 2:
            self.sequence += 1
            barrier = self.sequence
            marker = struct.pack('<ii', barrier, 2) + b'list\0\0'
            self.socket.sendall(struct.pack('<i', len(marker)) + marker)
        chunks = []
        while True:
            size, = struct.unpack('<i', self.exact(4))
            if size < 10 or size > 4 * 1024 * 1024:
                raise RuntimeError('Invalid reference RCON frame size')
            payload = self.exact(size)
            response, response_type = struct.unpack('<ii', payload[:8])
            if barrier is not None and response == barrier:
                return request, b''.join(chunks).decode()
            if response not in (request, -1):
                raise RuntimeError('Unexpected reference RCON response ID')
            chunks.append(payload[8:-2])
            if barrier is None:
                return response, b''.join(chunks).decode()

    def command(self, command):
        return self.send(command)[1]


def state_registry():
    # The checked-in registry and the official state dump agree (generate.py).
    rows = (ROOT / 'tools/vanilla/redstone-states.tsv').read_text().splitlines()
    states = {}
    for row in rows:
        fields = row.split('\t')
        match = re.fullmatch(r'Block\{minecraft:([^}]+)\}(?:\[(.*)\])?', fields[2])
        name, props = match.groups()
        values = dict(part.split('=') for part in props.split(',')) if props else {}
        states.setdefault(name, []).append((int(fields[0]), values))
    registry = (ROOT / 'lib/mcore/src/mc/data/registry_data.cpp').read_text()
    defaults = {m[0]: int(m[2]) for m in re.findall(r'^\{"([a-z_0-9]+)",(\d+),(\d+),(\d+),(\d+),(\d+),', registry, re.M) if m[0] in states and int(m[1]) == states[m[0]][0][0] and int(m[2]) in [ident for ident, _ in states[m[0]]]}
    def state_id(text):
        match = re.fullmatch(r'([^\[]+)(?:\[(.*)\])?', text)
        name, props = match.groups()
        default = next(p for ident, p in states[name] if ident == defaults[name])
        wanted = dict(default)
        if props:
            wanted.update(dict(part.split('=') for part in props.split(',')))
        return next(ident for ident, p in states[name] if p == wanted)
    return state_id


def generate_tests(cases, traces):
    state_id = state_registry()
    output = ['// Generated by tools/redstone/trace.py from the official 1.16.5 server.', '// Jar SHA-1: ' + SHA1]
    for case, trace in zip(cases, traces):
        name = case['name']
        assert trace['name'] == name
        expected = trace['samples']
        assert len(expected) == (case['ticks'] + 1) * len(case['observe'])
        output += [f'TEST(redstone_vanilla_trace_{name}) {{', '    Circuit c;',
                   '    for (int z = -8; z <= 8; ++z) for (int x = -16; x <= 31; ++x)',
                   '        c.s->world.setBlock(0, x, 79, z, bs::Stone, false);']
        for action in case['setup']:
            x, y, z = action['pos']
            output.append(f'    c.s->setBlock({x}, {y}, {z}, {state_id(action["state"])});')
        output.append(f'    c.tick({case["settle"]});')
        output.append('    static const int expected[] = {' + ','.join(map(str, expected)) + '};')
        output.append(f'    for (int tick = 0; tick <= {case["ticks"]}; ++tick) {{')
        for action in case['actions']:
            x, y, z = action['pos']
            output.append(f'        if (tick == {action["tick"]}) c.s->setBlock({x}, {y}, {z}, {state_id(action["state"])});')
        for index, obs in enumerate(case['observe']):
            x, y, z = obs['pos']
            output.append('        {')
            output.append(f'            uint16_t state = c.s->blockAt({x}, {y}, {z});')
            output.append('            int actual = -1;')
            if 'blocks' in obs:
                for value, block in enumerate(obs['blocks']):
                    output.append(f'            if (blockIdOf(state) == blockIdOf({state_id(block)})) actual = {value};')
            else:
                output.append(f'            if (blockIdOf(state) == blockIdOf({state_id(obs["block"])})) {{')
                for value, prop in enumerate(obs['values']):
                    output.append(f'                if (!strcmp(getPropStr(state, "{obs["property"]}"), "{prop}")) actual = {value};')
                output.append('            }')
            output += [f'            int wanted = expected[tick * {len(case["observe"])} + {index}];',
                       f'            if (actual != wanted) printf("    {name} tick %d observation {index}\\n", tick);',
                       '            CHECK_EQ(actual, wanted);', '        }']
        output += ['        c.tick();', '    }', '    CHECK_EQ(c.s->redstone.failures, 0u);', '}']
    INCLUDE.parent.mkdir(exist_ok=True)
    INCLUDE.write_text('\n'.join(output) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--accept-eula', action='store_true')
    parser.add_argument('--from-recorded', action='store_true', help='regenerate C++ tests from checked-in traces')
    args = parser.parse_args()
    cases_bytes = CASES.read_bytes()
    cases = json.loads(cases_bytes)
    if args.from_recorded:
        recorded = json.loads(RESULT.read_text())
        assert recorded['jar_sha1'] == SHA1
        assert recorded['cases_sha256'] == hashlib.sha256(cases_bytes).hexdigest()
        generate_tests(cases, recorded['traces'])
        return
    if not args.accept_eula:
        parser.error('Starting the reference server requires --accept-eula (https://aka.ms/MinecraftEULA)')
    jar = ROOT / 'tools/vanilla/server.jar'
    if not jar.exists() or hashlib.sha1(jar.read_bytes()).hexdigest() != SHA1:
        raise SystemExit('Run tools/redstone/generate.py first to cache the checksum-pinned jar')
    run = Path(tempfile.mkdtemp(prefix='redstone-reference-', dir=ROOT / 'tools/vanilla'))
    make_pack(run / 'world', cases)
    (run / 'eula.txt').write_text('eula=true\n')
    password = secrets.token_hex(16)
    (run / 'server.properties').write_text('\n'.join([
        'server-ip=127.0.0.1', 'server-port=25631', 'online-mode=false', 'enable-rcon=true',
        'rcon.port=25632', 'rcon.password=' + password, 'broadcast-rcon-to-ops=false',
        'level-type=flat', 'generate-structures=false', 'spawn-protection=0', 'view-distance=2',
        'max-tick-time=-1', 'sync-chunk-writes=true',
        'generator-settings={"biome":"minecraft:plains","layers":[{"height":1,"block":"minecraft:bedrock"}],"structures":{"structures":{}}}',
    ]) + '\n')
    print('Reference server logs: ' + str(run / 'server.log'), flush=True)
    with (run / 'server.log').open('w') as log:
        process = subprocess.Popen(['java', '-Xms256M', '-Xmx768M', '-Dlog4j2.formatMsgNoLookups=true', '-jar', str(jar), 'nogui'],
                                   cwd=run, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.PIPE)
        rcon = None
        try:
            deadline = time.monotonic() + 120
            while 'Done (' not in (run / 'server.log').read_text():
                if process.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('Reference server failed to start; inspect ' + str(run / 'server.log'))
                time.sleep(.2)
            rcon = Rcon(25632, password)
            for name in ['doDaylightCycle', 'doWeatherCycle', 'doMobSpawning', 'doPatrolSpawning', 'doTraderSpawning', 'sendCommandFeedback', 'logAdminCommands']:
                rcon.command('gamerule ' + name + ' false')
            rcon.command('gamerule randomTickSpeed 0')
            rcon.command('time set 6000')
            rcon.command('forceload add -16 -16 31 31')
            traces = []
            for i, case in enumerate(cases):
                rcon.command('function redstone_trace:start_' + str(i))
                deadline = time.monotonic() + 30
                while not re.search(r'has 0 ', rcon.command('scoreboard players get #active trace')):
                    if time.monotonic() > deadline:
                        raise RuntimeError('Reference case timed out: ' + case['name'])
                    time.sleep(.1)
                response = rcon.command('data get storage redstone_trace:result samples')
                raw = response[response.index('['):]
                samples = json.loads(raw)
                assert len(samples) == (case['ticks'] + 1) * len(case['observe']), (case['name'], response)
                traces.append(dict(name=case['name'], samples=samples))
                print(case['name'] + ': ' + str(len(samples)) + ' observations', flush=True)
            RESULT.write_text(json.dumps(dict(jar_sha1=SHA1, cases_sha256=hashlib.sha256(cases_bytes).hexdigest(), traces=traces), indent=2) + '\n')
            generate_tests(cases, traces)
        finally:
            if process.poll() is None:
                process.stdin.write(b'stop\n'); process.stdin.flush()
                try: process.wait(timeout=30)
                except subprocess.TimeoutExpired: process.kill(); process.wait()
            if rcon: rcon.socket.close()

if __name__ == '__main__':
    main()
