"""Pack the six-max catalogue into gzip pieces GitHub Pages can store.

GitHub rejects a single file over 100 MB, and hands.bin is about 115 MB.
Each piece is the gzip of a 16 MiB slice, which stays under the 50 MB warning.
The page downloads the pieces, inflates them, and concatenates them back into
the original file.

Writes data/pages/manifest.json, data/pages/cN.bin.gz, and a copy of summary.json.
"""

import gzip
import json
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "data" / "sixmax" / "hands.bin"
SUMMARY = ROOT / "data" / "sixmax" / "summary.json"
OUT = ROOT / "data" / "pages"
SLICE = 16 * 1024 * 1024


def main() -> None:
    if not SRC.exists():
        raise SystemExit(f"missing {SRC}")
    OUT.mkdir(parents=True, exist_ok=True)
    for old in OUT.glob("c*.bin.gz"):
        old.unlink()
    data = SRC.read_bytes()
    chunks = []
    start = 0
    while start < len(data):
        piece = data[start : start + SLICE]
        name = f"c{len(chunks)}.bin.gz"
        raw = gzip.compress(piece, compresslevel=6, mtime=0)
        (OUT / name).write_bytes(raw)
        chunks.append({"file": name, "bytes": len(raw), "raw": len(piece)})
        start += SLICE
    (OUT / "manifest.json").write_text(
        json.dumps({"format": "SCOOP6H1-gzip", "chunks": [c["file"] for c in chunks]}, indent=2) + "\n",
        encoding="utf-8",
    )
    if SUMMARY.exists():
        shutil.copyfile(SUMMARY, OUT / "summary.json")
    packed = sum(c["bytes"] for c in chunks)
    print(f"raw {len(data):,} bytes -> {packed:,} bytes in {len(chunks)} pieces")
    for c in chunks:
        print(f"  {c['file']}  {c['bytes']:,} gzip  from {c['raw']:,}")


if __name__ == "__main__":
    main()
