#!/usr/bin/env python3
"""Emit hey_jarvis_model.c from models/hey_jarvis.tflite. Do not edit the .c by hand."""
import sys
from pathlib import Path


def main() -> None:
    if len(sys.argv) != 3:
        print("usage: gen_model_c.py <in.tflite> <out.c>", file=sys.stderr)
        sys.exit(2)
    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    data = src.read_bytes()
    lines = [
        "// Generated from models/hey_jarvis.tflite - do not edit by hand.",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        "const uint8_t hey_jarvis_tflite[] __attribute__((aligned(16))) = {",
    ]
    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        hexes = ", ".join(f"0x{b:02x}" for b in chunk)
        lines.append(f"    {hexes},")
    lines.append("};")
    lines.append(f"const size_t hey_jarvis_tflite_len = {len(data)};")
    lines.append("")
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text("\n".join(lines), encoding="ascii", newline="\n")


if __name__ == "__main__":
    main()
