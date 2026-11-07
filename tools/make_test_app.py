#!/usr/bin/env python3
"""Генерирует тестовый app.bin с правдоподобной таблицей векторов."""
import os, random, struct, sys
out = sys.argv[1] if len(sys.argv) > 1 else "out"
os.makedirs(out, exist_ok=True)
random.seed(7)
body = bytearray(random.randrange(256) for _ in range(70001))
body[0:8] = struct.pack("<II", 0x20007FF0, 0x08004101)
open(os.path.join(out, "app.bin"), "wb").write(body)
