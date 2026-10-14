#!/usr/bin/env python3
"""Makes an Xbox Live Arcade style container out of a game disc image.

  make-container.py [--kind <hex>] <disc image .iso> <container to write> [<file in the image>...]

--kind is the content kind the container says it is: 000D0000, an Arcade
game, unless another is named (00000002 is add-on content, chimera issue
271).

An Xbox Live Arcade game is one file with no extension: a "LIVE" container
(the console's STFS format) with the game's program, default.xex, and its
data inside. This repository has no such game and may not contain one. So
that the container path can be tested where a disc image is, this writes a
container holding files taken from the image: default.xex unless others are
named. A big game will not get far with only its program, but it is opened,
mounted, loaded and started through the same code an Arcade game goes
through.

The layout written is the read-only one (one hash block for every 170 data
blocks), which is what downloaded games have. The hashes and the signature
are left zero: the emulator checks neither.
"""
import struct
import sys

SECTOR = 2048
BLOCK = 0x1000
PER_LEVEL = (170, 28900, 4913000)


def read_at(f, offset, size):
    f.seek(offset)
    data = f.read(size)
    if len(data) != size:
        raise SystemExit("the disc image is cut short")
    return data


def find_game_partition(f):
    for base in (0, 0xFB20 * SECTOR, 0x20600 * SECTOR, 0x2080000, 0xFD90000):
        f.seek(base + 32 * SECTOR)
        if f.read(20) == b"MICROSOFT*XBOX*MEDIA":
            return base
    raise SystemExit("not an Xbox 360 disc image: no game partition found")


def walk(f, base, sector, size, prefix, out):
    """The image's directory is a binary tree of entries inside its sectors."""
    table = read_at(f, base + sector * SECTOR, size)

    def node(at):
        if at >= len(table):
            return
        left, right, start, length, attributes, name_len = struct.unpack_from("<HHIIBB", table, at)
        if left == 0xFFFF:
            return
        name = table[at + 14:at + 14 + name_len].decode("latin-1")
        if left:
            node(left * 4)
        if attributes & 0x10:
            if length:
                walk(f, base, start, length, prefix + name + "/", out)
        else:
            out[(prefix + name).lower()] = (start, length)
        if right:
            node(right * 4)

    node(0)


def block_to_offset(block):
    """Where a data block is, in blocks from the start of the data area:
    the emulator's StfsContainerDevice::BlockToOffset for one hash block a table."""
    at = block
    for level in PER_LEVEL:
        at += (block + level) // level
        if block < level:
            break
    return at


def level0_table_block(block):
    """Where the hash table that describes a data block is: the emulator's
    BlockToHashBlockNumber(block, 0) for one hash block a table."""
    step0 = PER_LEVEL[0] + 1
    if block < PER_LEVEL[0]:
        return 0
    at = (block // PER_LEVEL[0]) * step0 + (block // PER_LEVEL[1]) + 1
    if block < PER_LEVEL[1]:
        return at
    return at + 1


def main():
    kind = 0x000D0000
    if len(sys.argv) > 2 and sys.argv[1] == "--kind":
        kind = int(sys.argv[2], 16)
        del sys.argv[1:3]
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    wanted = [n.lower() for n in sys.argv[3:]] or ["default.xex"]
    files = []
    with open(sys.argv[1], "rb") as f:
        base = find_game_partition(f)
        root_sector, root_size = struct.unpack("<II", read_at(f, base + 32 * SECTOR + 20, 8))
        listing = {}
        walk(f, base, root_sector, root_size, "", listing)
        for name in wanted:
            if name not in listing:
                raise SystemExit("the disc image has no " + name)
            if "/" in name:
                raise SystemExit("only files of the image's top directory: " + name)
            start, length = listing[name]
            files.append((name, read_at(f, base + start * SECTOR, length)))
        # the title's number, from the program: the container says it too
        title_id = 0
        xex = dict(files).get("default.xex")
        if xex and xex[:4] == b"XEX2":
            count = struct.unpack_from(">I", xex, 20)[0]
            for i in range(count):
                key, value = struct.unpack_from(">II", xex, 24 + i * 8)
                if key == 0x00040006:  # execution info
                    title_id = struct.unpack_from(">I", xex, value + 12)[0]

    # block 0 is the directory; each file follows, its blocks one after another
    chains = {}  # data block -> next data block
    data = {0: b""}
    directory = b""
    next_block = 1
    chains[0] = 0xFFFFFF
    for name, body in files:
        blocks = max(1, (len(body) + BLOCK - 1) // BLOCK)
        raw = name.encode("latin-1")
        entry = raw.ljust(40, b"\0")
        entry += bytes([len(raw) | 0x40])  # contiguous
        entry += blocks.to_bytes(3, "little") * 2
        entry += next_block.to_bytes(3, "little")
        entry += struct.pack(">HIHHHH", 0xFFFF, len(body), 0, 0, 0, 0)
        directory += entry
        for i in range(blocks):
            data[next_block + i] = body[i * BLOCK:(i + 1) * BLOCK]
            chains[next_block + i] = next_block + i + 1 if i + 1 < blocks else 0xFFFFFF
        next_block += blocks
    data[0] = directory
    total = next_block

    area = bytearray((block_to_offset(total - 1) + 1) * BLOCK)
    for block, body in data.items():
        at = block_to_offset(block) * BLOCK
        area[at:at + len(body)] = body
        table = level0_table_block(block) * BLOCK + (block % PER_LEVEL[0]) * 0x18
        # twenty bytes of hash (left zero), then: in use, and the next block
        struct.pack_into(">I", area, table + 0x14, 0x80000000 | chains[block])

    header_size = 0xAD0E
    header = bytearray((header_size + BLOCK - 1) // BLOCK * BLOCK)
    header[0:4] = b"LIVE"
    # the first licence: anybody's, with the bit an Arcade game reads as "bought"
    struct.pack_into(">QII", header, 0x22C, 0xFFFFFFFFFFFFFFFF, 1, 0)
    struct.pack_into(">I", header, 0x340, header_size)
    struct.pack_into(">IIQ", header, 0x344, kind, 2, len(area))  # an Arcade title unless --kind
    struct.pack_into(">IIII", header, 0x354, 0, 0, 0, title_id)
    descriptor = bytes([0x24, 0, 0x01])  # read-only format
    descriptor += struct.pack("<H", 1) + (0).to_bytes(3, "little")
    descriptor += bytes(0x14) + struct.pack(">II", total, 0)
    header[0x379:0x379 + len(descriptor)] = descriptor
    struct.pack_into(">I", header, 0x3A9, 0)  # the volume is STFS
    name = "Chimera test container".encode("utf-16-be")
    header[0x411:0x411 + len(name)] = name
    header[0x1691:0x1691 + len(name)] = name

    with open(sys.argv[2], "wb") as out:
        out.write(header)
        out.write(area)
    print("%s: %d files, %d blocks, title %08X, kind %08X" % (sys.argv[2], len(files), total, title_id, kind))


if __name__ == "__main__":
    main()
