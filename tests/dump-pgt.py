#! /usr/bin/python3

#
# This script extracts the complete page table hierarchy from an existing
# dump file in a format that is accepted as test case data.
#
# It requires pykdumpfile, which is maintained here:
#   https://github.com/ptesarik/pykdumpfile
#

import sys
from argparse import ArgumentParser
import kdumpfile

class Compress:
    def __init__(self):
        self.prev = list()
        self.repeat = 1

    def output(self, s):
        print(s)

    def restart(self):
        if len(self.prev) >= 2:
            diff = self.prev[0] - self.prev[1]
            if not diff:
                self.output('{:016X}*{:d}'.format(self.prev[0], self.repeat + 1))
                self.prev.clear()
            elif self.repeat > 1:
                first = self.prev[0] - diff * self.repeat
                self.output('{:016X}*{:d}{:+X}'.format(first, self.repeat + 1, diff))
                self.prev.clear()
        if len(self.prev) >= 2:
            self.output('{:016X}'.format(self.prev[1]))
            self.prev.pop()
        self.repeat = 1

    def insert(self, val):
        if len(self.prev) >= 2:
            diff = self.prev[0] - self.prev[1]
            if val - self.prev[0] == diff:
                self.repeat += 1
            else:
                self.restart()
        self.prev.insert(0, val)

    def flush(self):
        self.restart()
        if len(self.prev):
            self.output('{:016X}'.format(self.prev[0]))

class PageDumper:
    _byteordermap = {
        kdumpfile.BIG_ENDIAN: 'big',
        kdumpfile.LITTLE_ENDIAN: 'little',
    }

    def __init__(self, kdump):
        self.kdump = kdump
        self.byteorder = self._byteordermap[kdump.attr[kdumpfile.ATTR_BYTE_ORDER]]
        self.pagesize = kdump.attr[kdumpfile.ATTR_PAGE_SIZE]
        self.pte_size = kdump.attr['arch.pteval_size']
        self.compress = Compress()
        self.subtables = list()

    # TODO: x86_64-specific
    def ispgt(self, val):
        # PRESENT and not PSE
        return val & (1 << 0) and not val & (1 << 7)

    # TODO: x86_64-specific
    def val2addr(self, val):
        return val & -self.pagesize & ~(1 << 63)

    def dump(self, addr):
        table = ctx.read(kdumpfile.MACHPHYSADDR, addr, self.pagesize)
        for off in range(0, self.pagesize, self.pte_size):
            val = int.from_bytes(table[off:off+8], self.byteorder)
            if self.ispgt(val):
                self.subtables.append(self.val2addr(val))
            self.compress.insert(val)
        self.compress.flush()

seen = set()
def dump(ctx, addr, level):
    if addr in seen:
        return False
    seen.add(addr)
    dumper = PageDumper(ctx)
    print('@0x{:X}'.format(addr))
    dumper.dump(addr)
    print()
    if level < 4:
        for addr in dumper.subtables:
            dump(ctx, addr, level + 1)
    return True

parser = ArgumentParser(
    description='Dump a complete page table hierarchy')
parser.register('type', 'numeric', lambda s: int(s, base=0))
parser.add_argument('filename')
parser.add_argument('rootpgt', type='numeric')
args = parser.parse_args()

ctx = kdumpfile.Context()
ctx.open(args.filename)
dump(ctx, args.rootpgt, 1)
