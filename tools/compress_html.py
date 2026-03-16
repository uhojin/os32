#!/usr/bin/env python3
"""Compress portal.html to a C byte array for embedding in firmware."""
import gzip
import sys
from pathlib import Path

def main():
    src = Path(__file__).parent.parent / "web" / "portal.html"
    dst = Path(__file__).parent.parent / "main" / "captive_portal_html.c"

    html = src.read_bytes()
    compressed = gzip.compress(html, compresslevel=9)

    with open(dst, "w") as f:
        f.write("// Auto-generated — do not edit. Run tools/compress_html.py\n")
        f.write(f"// Source: web/portal.html ({len(html)} bytes -> {len(compressed)} bytes gzipped)\n\n")
        f.write("#include <stddef.h>\n#include <stdint.h>\n\n")
        f.write(f"const uint8_t captive_portal_html_gz[] = {{\n")
        for i in range(0, len(compressed), 16):
            chunk = compressed[i:i+16]
            f.write("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
        f.write("};\n\n")
        f.write(f"const size_t captive_portal_html_gz_len = {len(compressed)};\n")

    print(f"Compressed {len(html)} -> {len(compressed)} bytes ({100*len(compressed)//len(html)}%)")
    print(f"Written to {dst}")

if __name__ == "__main__":
    main()
