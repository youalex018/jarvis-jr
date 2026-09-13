#!/usr/bin/env python3
"""Emit a 16-byte-aligned C blob from a .tflite. Do not edit the .c by hand."""
import sys
from pathlib import Path


def main() -> None:
    if len(sys.argv) != 4:
        print("usage: gen_model_c.py <in.tflite> <out.c> <symbol>", file=sys.stderr)
        sys.exit(2)
    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    symbol = sys.argv[3]
    data = src.read_bytes()
    lines = [
        f"// Generated from {src.name} - do not edit by hand.",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        f"const uint8_t {symbol}[] __attribute__((aligned(16))) = {{",
    ]
    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        hexes = ", ".join(f"0x{b:02x}" for b in chunk)
        lines.append(f"    {hexes},")
    lines.append("};")
    lines.append(f"const size_t {symbol}_len = {len(data)};")
    lines.append("")
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text("\n".join(lines), encoding="ascii", newline="\n")


if __name__ == "__main__":
    main()
