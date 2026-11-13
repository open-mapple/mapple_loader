#!/usr/bin/env python3
# The ILI7807S's touch firmware (FW_NF_ILI7807S_*.bin, pulled off YOUR phone) as the payload item "ilifw" mtkloader
# sends (mtkili.h has the layout). The file is a vendor wrapper ("FwUp", up to the first ':') around Intel HEX with
# ILITEK's own records: 0xAF names a block (start, end, number), 0xB0 moves one in the chip's memory. Laid out as
# ILITEK's driver does (read, not copied): blocks AP 1, DATA 2, TUNING 3 are what a normal start loads; AP goes to 0,
# DATA to 0x20610, TUNING right after DATA, unless a 0xB0 record says otherwise. Each block ends in the CRC32 (MSB
# first, polynomial 0x04C11DB7, start 0xFFFFFFFF, no final xor) of the rest, which is checked here.
# The output is the vendor's code: it goes into the boot image, never into a repository.
# usage: mkilifw.py FIRMWARE OUT
import struct
import sys

AP, DATA, TUNING = 1, 2, 3
DLM_START = 0x20610
MAX = 160 * 1024
MAGIC = b"ILFW"
HEAD = 64


def crc32_msb(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) if crc & 0x80000000 else crc << 1
            crc &= 0xFFFFFFFF
    return crc


def fnv1a(data):
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def parse(raw):
    text = raw[raw.index(b":"):].decode("ascii", "replace")
    image = bytearray(b"\xff" * MAX)
    blocks, fixed, ext = {}, {}, 0
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith(":") or len(line) < 11:
            continue
        n, addr, kind = int(line[1:3], 16), int(line[3:7], 16), int(line[7:9], 16)
        data = line[9:9 + 2 * n]
        if kind == 0x04:
            ext = int(data[0:4], 16)
        elif kind == 0x02:
            ext = int(data[0:4], 16) >> 12
        elif kind == 0xAF:
            blocks[int(data[12:14], 16)] = (int(data[0:6], 16), int(data[6:12], 16))
        elif kind == 0xB0:
            fixed[int(data[6:8], 16)] = int(data[0:6], 16)
        elif kind == 0x00:
            at = addr + (ext << 16)
            if at + n > MAX:
                sys.exit("mkilifw: data past %d KB at %#x" % (MAX // 1024, at))
            image[at:at + n] = bytes.fromhex(data)
    return image, blocks, fixed


def member(parts):
    """parts: (chip address, block bytes, crc) for AP, DATA, TUNING -> the item's bytes"""
    table, body, off = b"", b"", HEAD
    for mem, data, crc in parts:
        table += struct.pack("<4I", mem, len(data), crc, off)
        pad = data + b"\0" * (-len(data) % 16)
        body += pad
        off += len(pad)
    rest = table + b"\0" * (HEAD - 16 - len(table)) + body
    return MAGIC + struct.pack("<3I", 16 + len(rest), fnv1a(rest), len(parts)) + rest


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: mkilifw.py FIRMWARE OUT")
    image, blocks, fixed = parse(open(sys.argv[1], "rb").read())
    for b in (AP, DATA, TUNING):
        if b not in blocks:
            sys.exit("mkilifw: no block %d in %s" % (b, sys.argv[1]))
    mem = {AP: fixed.get(AP, 0), DATA: fixed.get(DATA, DLM_START)}
    mem[TUNING] = fixed.get(TUNING, mem[DATA] + blocks[DATA][1] - blocks[DATA][0] + 1)
    parts = []
    for b, name in ((AP, "AP"), (DATA, "DATA"), (TUNING, "TUNING")):
        start, end = blocks[b]
        body = bytes(image[start:end + 1])
        crc = crc32_msb(body[:-4])
        if crc != int.from_bytes(body[-4:], "big"):
            sys.exit("mkilifw: block %s's CRC %08x is not the one it carries (%s)" % (name, crc, body[-4:].hex()))
        parts.append((mem[b], body, crc))
        print("mkilifw: %-6s hex %06x..%06x -> chip %05x, %6d bytes, CRC %08x (as it carries)" %
              (name, start, end, mem[b], len(body), crc))
    open(sys.argv[2], "wb").write(member(parts))


if __name__ == "__main__":
    main()
