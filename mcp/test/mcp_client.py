#!/usr/bin/env python3
"""End-to-end test for the ares-mcp MCP server.

Spawns `ares-mcp mcp`, speaks newline-delimited JSON-RPC 2.0 over stdio, and
drives the full workflow:

    initialize -> notifications/initialized -> tools/list
    -> n64_load -> n64_run -> n64_screenshot -> n64_log -> n64_status
    -> error paths (unknown tool, unknown method)

By default it loads the generated green.z64 and verifies the rendered frame
actually shows the green field + white box (decoding the PNG, which nall
encodes as an uncompressed/stored-block zlib stream). With --rom you can point
it at any ROM (e.g. a commercial one) and --no-check skips the pixel checks.

Usage:
    mcp_client.py [--rom PATH] [--bin PATH] [--no-check] [--frames N]
"""

import argparse
import base64
import json
import os
import re
import struct
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


# --- nall's PNG is simple: 8-bit, RGB (type 2) or RGBA (type 6), filter 0,
# --- IDAT is a sequence of stored (uncompressed) deflate blocks.

def decode_png(data):
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    pos, idat, width, height, colortype = 8, b'', 0, 0, 0
    while pos + 8 <= len(data):
        (length,) = struct.unpack('>I', data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        if ctype == b'IHDR':
            width, height, _depth, colortype = struct.unpack('>IIBB', chunk[:10])
        elif ctype == b'IDAT':
            idat += chunk
        elif ctype == b'IEND':
            break
        pos += 12 + length
    if colortype not in (2, 6):
        raise ValueError(f'unsupported color type {colortype}')
    bpp = 3 if colortype == 2 else 4

    # stored-block inflate
    if idat[:2] != b'\x78\xda':
        raise ValueError('unexpected zlib header')
    raw, i = b'', 2
    while i < len(idat):
        btype = idat[i]; i += 1
        ln = idat[i] | (idat[i + 1] << 8); i += 2
        nln = idat[i] | (idat[i + 1] << 8); i += 2
        if nln != (~ln & 0xFFFF):
            raise ValueError('bad stored block length')
        raw += idat[i:i + ln]; i += ln
        if btype == 1:
            break

    stride = width * bpp
    rows = []
    for y in range(height):
        off = y * (stride + 1)
        ftype = raw[off]
        if ftype != 0:
            raise ValueError(f'nall should write filter 0, got {ftype}')
        rows.append(bytes(raw[off + 1:off + 1 + stride]))
    return width, height, rows, bpp


def pixel(rows, bpp, x, y):
    off = x * bpp
    return rows[y][off:off + 3]


# --- protocol client ----------------------------------------------------------

class Client:
    def __init__(self, proc):
        self.proc = proc
        self.next_id = 0

    def request(self, method, params=None):
        self.next_id += 1
        msg = {'jsonrpc': '2.0', 'id': self.next_id, 'method': method}
        if params is not None:
            msg['params'] = params
        self.proc.stdin.write(json.dumps(msg).encode() + b'\n')
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError('server closed stdout (no response)')
        return json.loads(line)

    def notify(self, method, params=None):
        msg = {'jsonrpc': '2.0', 'method': method}
        if params is not None:
            msg['params'] = params
        self.proc.stdin.write(json.dumps(msg).encode() + b'\n')
        self.proc.stdin.flush()

    def call(self, name, arguments=None):
        r = self.request('tools/call', {'name': name, 'arguments': arguments or {}})
        if 'error' in r:
            raise RuntimeError(f'{name}: JSON-RPC error {r["error"]}')
        result = r['result']
        if result.get('isError'):
            text = next((i['text'] for i in result['content'] if i['type'] == 'text'), '?')
            raise RuntimeError(f'{name}: tool error: {text}')
        return result

    def text_of(self, result):
        return next((i['text'] for i in result['content'] if i['type'] == 'text'), '')


def expect(cond, what):
    if not cond:
        print(f'FAIL: {what}')
        sys.exit(1)
    print(f'  ok: {what}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', default=os.path.join(REPO, 'mcp', 'test', 'green.z64'))
    ap.add_argument('--bin', default=os.path.join(REPO, 'build', 'rundir', 'bin', 'ares-mcp'))
    ap.add_argument('--frames', type=int, default=120)
    ap.add_argument('--no-check', action='store_true', help='skip pixel-content checks (arbitrary ROMs)')
    args = ap.parse_args()

    rom = os.path.abspath(args.rom)
    scratch = tempfile.mkdtemp(prefix='ares-mcp-e2e-')
    print(f'ROM:      {rom}')
    print(f'binary:   {args.bin}')
    print(f'scratch:  {scratch}')

    stderr_file = open(os.path.join(scratch, 'server-stderr.log'), 'w')
    proc = subprocess.Popen(
        [args.bin, 'mcp'],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr_file,
        cwd=scratch,
    )
    client = Client(proc)

    # 1) initialize
    r = client.request('initialize', {
        'protocolVersion': '2025-03-26',
        'capabilities': {},
        'clientInfo': {'name': 'ares-mcp-e2e', 'version': '1.0'},
    })
    expect('result' in r, 'initialize returned a result')
    expect(r['result']['serverInfo']['name'] == 'ares-mcp', 'serverInfo.name == ares-mcp')
    expect(r['result']['protocolVersion'] in ('2024-11-05', '2025-03-26', '2025-06-18'),
           f"protocolVersion {r['result']['protocolVersion']}")
    client.notify('notifications/initialized')

    # 2) tools/list
    r = client.request('tools/list')
    tools = {t['name']: t for t in r['result']['tools']}
    expected = {'n64_status', 'n64_load', 'n64_run', 'n64_screenshot',
                'n64_log', 'n64_record', 'n64_pause', 'n64_resume', 'n64_stop'}
    expect(expected <= set(tools), f'tools/list has all {len(expected)} tools')
    expect(all('inputSchema' in t and t['inputSchema'].get('type') == 'object' for t in tools.values()),
           'every tool has an object inputSchema')

    # 3) load
    result = client.call('n64_load', {'rom': rom, 'save_dir': scratch})
    print(f'  load: {client.text_of(result)}')

    # 4) run
    result = client.call('n64_run', {'frames': args.frames})
    text = client.text_of(result)
    print(f'  run:  {text}')
    expect(f'Ran {args.frames} frames' in text, 'run reported the requested frame count')

    # 5) screenshot
    shot_path = os.path.join(scratch, 'shot.png')
    result = client.call('n64_screenshot', {'path': shot_path})
    image = next((i for i in result['content'] if i['type'] == 'image'), None)
    expect(image is not None, 'screenshot returned an image content item')
    expect(image['mimeType'] == 'image/png', 'mimeType is image/png')
    png = base64.b64decode(image['data'])
    expect(os.path.isfile(shot_path) and open(shot_path, 'rb').read() == png,
           'inline PNG matches the saved file')

    # always: a fully black frame means the core produced no video — that is
    # a boot failure, whatever the ROM is (catches the commercial-ROM stall)
    width, height, rows, bpp = decode_png(png)
    print(f'  frame: {width}x{height} ({bpp} bytes/pixel)')
    nonblack = 0
    for y in range(0, height, 4):
        for x in range(0, width, 4):
            r_, g_, b_ = pixel(rows, bpp, x, y)
            if r_ >= 0x10 or g_ >= 0x10 or b_ >= 0x10:
                nonblack += 1
    expect(nonblack > 0, 'frame is not fully black')

    if not args.no_check:
        # the green ROM fills RDRAM 0x80100000-0x8012C000 (a green band across
        # the top ~93 rows) and draws a white box (120 rows x 240 cols) at
        # ~x208/y192; the rest of the screen stays black
        green = white = black = sampled = 0
        for y in range(0, height, 2):
            for x in range(0, width, 2):
                sampled += 1
                r_, g_, b_ = pixel(rows, bpp, x, y)
                if r_ < 0x40 and g_ > 0xC0 and b_ < 0x40:
                    green += 1
                elif r_ > 0xF0 and g_ > 0xF0 and b_ > 0xF0:
                    white += 1
                elif r_ < 0x10 and g_ < 0x10 and b_ < 0x10:
                    black += 1
        expect(black != sampled, 'frame is not all black')
        expect(green > sampled * 0.10, f'green band rendered (green={green}/{sampled} sampled)')
        expect(white > sampled * 0.03, f'white box rendered (white={white}/{sampled} sampled)')

    # 6) log
    result = client.call('n64_log', {'limit': 50})
    logtext = client.text_of(result)
    expect(logtext.strip() != '(log is empty)', 'log is not empty')
    expect('loaded' in logtext.lower(), 'log mentions the ROM load')
    expect(client.call('n64_log', {'limit': 10, 'filter': 'ZQX_NOT_THERE'})['content'][0]['text'] == '(log is empty)',
           'log filter returns empty for a non-matching filter')

    # 7) status
    result = client.call('n64_status')
    status = json.loads(client.text_of(result))
    print(f'  status: {json.dumps(status)}')
    expect(status['loaded'] is True, 'status.loaded')
    expect(status['frames'] >= args.frames, 'status.frames >= requested')
    expect(status['running'] is False, 'core stopped after n64_run (default stop=true)')

    # 8) pause / resume / stop on a stopped core are graceful errors
    try:
        client.call('n64_pause')
        expect(False, 'n64_pause should fail while stopped')
    except RuntimeError as e:
        expect('not running' in str(e), 'n64_pause on stopped core reports a tool error')

    # 9) error paths
    r = client.request('tools/call', {'name': 'no_such_tool', 'arguments': {}})
    expect(r.get('error', {}).get('code') == -32602, 'unknown tool -> -32602')
    r = client.request('bogus/method', {})
    expect(r.get('error', {}).get('code') == -32601, 'unknown method -> -32601')

    # 10) clean shutdown on EOF
    proc.stdin.close()
    proc.wait(timeout=60)
    expect(proc.returncode == 0, f'server exited cleanly (rc={proc.returncode})')

    print('\nALL OK — MCP end-to-end test passed')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as e:
        print(f'\nFAIL: {e}')
        # dump the last server log lines from the e2e scratch dir
        for f in os.listdir(tempfile.gettempdir()):
            if f.startswith('ares-mcp-e2e-'):
                log = os.path.join(tempfile.gettempdir(), f, 'server-stderr.log')
                if os.path.isfile(log):
                    lines = open(log).readlines()[-15:]
                    if lines:
                        print('--- server stderr (tail) ---')
                        print(''.join(lines), end='')
                    break
        sys.exit(1)
