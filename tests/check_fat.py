#!/usr/bin/env python3
"""Независимый разбор FAT16-образа, который отдаёт загрузчик.
Проверяет то, на что опираются Windows/macOS/Linux при монтировании."""
import struct, sys

img = open(sys.argv[1], "rb").read()
errs = []
def check(cond, msg):
    if not cond:
        errs.append(msg); print("  FAIL", msg)

bs = img[:512]
check(bs[510:512] == b"\x55\xAA", "boot signature 55AA")
check(bs[0] in (0xEB, 0xE9), "jump instruction")
(bps, spc, rsv, nfat, rootent, tot16, media, fatsz, spt, heads, hidden, tot32) = struct.unpack_from("<HBHBHHBHHHII", bs, 11)
total = tot16 or tot32
check(bps == 512, f"bytes/sector={bps}")
check(spc and spc & (spc - 1) == 0, f"sectors/cluster power of two ({spc})")
check(nfat == 2, f"FAT copies={nfat}")
check(media == 0xF8, "media 0xF8")
check(len(img) == total * 512, f"image size {len(img)} == total {total}*512")
root_secs = (rootent * 32 + bps - 1) // bps
data_start = rsv + nfat * fatsz + root_secs
clusters = (total - data_start) // spc
print(f"  total={total} sectors, FAT={fatsz} sectors, clusters={clusters}")
check(4085 <= clusters < 65525, f"FAT16 cluster range ({clusters})")
check(fatsz * bps // 2 >= clusters + 2, "FAT large enough for all clusters")
check(bs[54:62] == b"FAT16   ", "fs type string")
check(bs[38] == 0x29, "extended boot signature")
label = bs[43:54]
print(f"  label={label!r}")

fat0 = img[rsv * 512:(rsv + fatsz) * 512]
fat1 = img[(rsv + fatsz) * 512:(rsv + 2 * fatsz) * 512]
check(fat0 == fat1, "FAT copies identical")
fat = struct.unpack(f"<{len(fat0)//2}H", fat0)
check(fat[0] == 0xFFF8 and fat[1] >= 0xFFF8, "FAT[0], FAT[1] reserved entries")

root = img[(rsv + nfat * fatsz) * 512:(rsv + nfat * fatsz + root_secs) * 512]
entries = {}
vol = None
for i in range(rootent):
    e = root[i * 32:(i + 1) * 32]
    if e[0] == 0: break
    attr = e[11]
    if attr & 0x08 and not attr & 0x02:
        vol = e[:11]; continue
    name = e[:11].decode("ascii")
    cl = struct.unpack_from("<H", e, 26)[0]
    size = struct.unpack_from("<I", e, 28)[0]
    entries[name] = (cl, size)
check(vol == label, "root volume label == boot sector label")
print("  files:", entries)

def read_file(cl, size):
    out = bytearray(); seen = set(); c = cl
    while c < 0xFFF8:
        check(c not in seen and 2 <= c < clusters + 2, f"valid chain cluster {c}")
        if c in seen or not (2 <= c < clusters + 2): break
        seen.add(c)
        off = (data_start + (c - 2) * spc) * 512
        out += img[off:off + spc * 512]
        c = fat[c]
    check(len(out) >= size and len(out) - size < spc * 512, "chain length matches size")
    return bytes(out[:size])

check("INFO_UF2TXT" in entries and "CURRENT UF2" in entries, "required files present")
info = read_file(*entries["INFO_UF2TXT"])
print("  INFO_UF2.TXT:", info.decode().replace("\r\n", " | "))
check(b"Model:" in info and b"Board-ID:" in info and b"UF2 Bootloader" in info, "INFO_UF2.TXT keys (uf2conv --list)")

cur = read_file(*entries["CURRENT UF2"])
check(len(cur) % 512 == 0, "CURRENT.UF2 multiple of 512")
n = len(cur) // 512
last_addr = -1
for i in range(n):
    m0, m1, flags, addr, pay, bno, nb, fam = struct.unpack_from("<8I", cur, i * 512)
    mend = struct.unpack_from("<I", cur, i * 512 + 508)[0]
    ok = (m0, m1, mend) == (0x0A324655, 0x9E5D5157, 0x0AB16F30) and bno == i and nb == n \
        and pay == 256 and addr == 0x08004000 + i * 256 and flags & 0x2000 and fam == 0xE47E1D64
    if not ok:
        check(False, f"UF2 block {i} header"); break
else:
    print(f"  CURRENT.UF2: {n} valid UF2 blocks")

# файлы не должны перекрываться
spans = sorted((c, c + (s + spc * 512 - 1) // (spc * 512)) for c, s in entries.values())
check(all(spans[i][1] <= spans[i + 1][0] for i in range(len(spans) - 1)), "files do not overlap")

print("RESULT:", "OK" if not errs else f"{len(errs)} problem(s)")
sys.exit(1 if errs else 0)
