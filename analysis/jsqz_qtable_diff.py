#!/usr/bin/env python3
"""Compare two 128-byte Exynos JSQZ quantisation-table dumps."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


TABLE_BYTES = 128
WORDS_PER_MATRIX = 16
MATRIX_BYTES = 64


def read_table(path: Path) -> bytes:
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise argparse.ArgumentTypeError(f"cannot read {path}: {exc}") from exc
    if len(data) != TABLE_BYTES:
        raise argparse.ArgumentTypeError(
            f"{path} is {len(data)} bytes; expected exactly {TABLE_BYTES}"
        )
    return data


def describe_matrix(name: str, before: bytes, after: bytes) -> None:
    changed = [
        (i, old, new)
        for i, (old, new) in enumerate(zip(before, after))
        if old != new
    ]
    print(f"{name}: {len(changed)}/{MATRIX_BYTES} coefficient bytes changed")
    for index, old, new in changed:
        print(f"  {name}[{index:02d}]: 0x{old:02x} -> 0x{new:02x}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare two saved 128-byte JSQZ Q-table files."
    )
    parser.add_argument("baseline", type=Path, help="baseline Q-table (for example, grey)")
    parser.add_argument("candidate", type=Path, help="new Q-table to compare")
    args = parser.parse_args()

    try:
        baseline = read_table(args.baseline)
        candidate = read_table(args.candidate)
    except argparse.ArgumentTypeError as exc:
        parser.error(str(exc))

    old_words = struct.unpack("<32I", baseline)
    new_words = struct.unpack("<32I", candidate)
    changed_y = [i for i in range(WORDS_PER_MATRIX) if old_words[i] != new_words[i]]
    changed_c = [
        i for i in range(WORDS_PER_MATRIX)
        if old_words[WORDS_PER_MATRIX + i] != new_words[WORDS_PER_MATRIX + i]
    ]

    print(f"Baseline:  {args.baseline}")
    print(f"Candidate: {args.candidate}")
    print(
        "Changed 32-bit words: "
        f"Y {len(changed_y)}/{WORDS_PER_MATRIX}, "
        f"C {len(changed_c)}/{WORDS_PER_MATRIX}"
    )

    for matrix_name, indices, base in (
        ("Y", changed_y, 0),
        ("C", changed_c, WORDS_PER_MATRIX),
    ):
        for index in indices:
            print(
                f"  {matrix_name} word {index:02d}: "
                f"{old_words[base + index]:08x} -> "
                f"{new_words[base + index]:08x}"
            )

    describe_matrix("Y", baseline[:MATRIX_BYTES], candidate[:MATRIX_BYTES])
    describe_matrix("C", baseline[MATRIX_BYTES:], candidate[MATRIX_BYTES:])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
