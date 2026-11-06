#!/usr/bin/env python3
"""bin -> UF2 для K1986BE92FI UF2-загрузчика.

  arm-none-eabi-objcopy -O binary app.elf app.bin
  python3 tools/bin2uf2.py app.bin -o app.uf2

Образ должен быть слинкован на 0x08003000 (см. README).
"""
import argparse, struct, sys

MAGIC0, MAGIC1, MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
FLAG_FAMILY = 0x00002000
FAMILY_ID = 0xE47E1D64
APP_START, APP_END = 0x08003000, 0x08020000


def convert(data: bytes, base: int, family: int, payload: int) -> bytes:
    if payload % 4 or not (4 <= payload <= 476):
        raise ValueError("payload must be a multiple of 4 in 4..476")
    if base % 4:
        raise ValueError("base must be 4-byte aligned")
    if not data:
        raise ValueError("empty input")
    if base < APP_START or base + len(data) > APP_END:
        raise ValueError(
            f"image 0x{base:08X}..0x{base+len(data):08X} is outside the application "
            f"area 0x{APP_START:08X}..0x{APP_END:08X}")
    nblocks = (len(data) + payload - 1) // payload
    if nblocks > 2048:
        raise ValueError("too many blocks (max 2048); use a larger --payload")
    out = bytearray()
    for i in range(nblocks):
        chunk = data[i * payload:(i + 1) * payload]
        chunk = chunk + b"\xFF" * (payload - len(chunk))
        hdr = struct.pack("<8I", MAGIC0, MAGIC1, FLAG_FAMILY, base + i * payload,
                          payload, i, nblocks, family)
        out += hdr + chunk + b"\x00" * (476 - payload) + struct.pack("<I", MAGIC_END)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("-b", "--base", type=lambda s: int(s, 0), default=APP_START)
    ap.add_argument("-f", "--family", type=lambda s: int(s, 0), default=FAMILY_ID)
    ap.add_argument("-p", "--payload", type=int, default=256)
    a = ap.parse_args()
    try:
        uf2 = convert(open(a.input, "rb").read(), a.base, a.family, a.payload)
    except ValueError as e:
        sys.exit(f"error: {e}")
    open(a.output, "wb").write(uf2)
    print(f"{a.output}: {len(uf2) // 512} blocks, base 0x{a.base:08X}")


if __name__ == "__main__":
    main()
