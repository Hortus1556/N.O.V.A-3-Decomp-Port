"""Minimal ELF32 (ARM) reader for libGloftNAHM.so."""
import struct, zipfile

APK = 'E:/nova3/NOVA-3-v1-0-1d.apk'
LIB = 'lib/armeabi-v7a/libGloftNAHM.so'


class Elf:
    def __init__(self, data):
        self.d = d = data
        shoff, = struct.unpack_from('<I', d, 0x20)
        phoff, = struct.unpack_from('<I', d, 0x1c)
        phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from('<HHHHH', d, 0x2a)
        self.phdrs = [struct.unpack_from('<8I', d, phoff + i * phentsize) for i in range(phnum)]
        raw = [struct.unpack_from('<10I', d, shoff + i * shentsize) for i in range(shnum)]
        names = raw[shstrndx][4]
        # name -> (addr, offset, size, type, flags)
        self.sections = {self.cstr(names + s[0]): (s[3], s[4], s[5], s[1], s[2]) for s in raw}

    def cstr(self, off):
        return self.d[off:self.d.index(b'\0', off)].decode('latin1')

    def sec(self, name):
        addr, off, size, _, _ = self.sections[name]
        return addr, self.d[off:off + size]

    def symbols(self):
        """Yield (name, value, size, type, bind, shndx) from .dynsym."""
        _, st = self.sec('.dynsym')
        stro = self.sections['.dynstr'][1]
        for i in range(len(st) // 16):
            nm, val, sz, info, _, shn = struct.unpack_from('<IIIBBH', st, i * 16)
            yield self.cstr(stro + nm), val, sz, info & 15, info >> 4, shn

    def read(self, addr, n):
        """Read n bytes at a virtual address (file-backed segments only)."""
        for typ, off, va, _, filesz, memsz, _, _ in self.phdrs:
            if typ == 1 and va <= addr < va + memsz:
                o = addr - va
                return self.d[off + o:off + o + n] if o + n <= filesz else None
        return None

    def u32(self, addr):
        return struct.unpack('<I', self.read(addr, 4))[0]


def load():
    return Elf(zipfile.ZipFile(APK).read(LIB))
