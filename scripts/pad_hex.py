#!/usr/bin/env python3
"""Pad the last data record in an Intel HEX file to 16 bytes with 0xFF.

Works around an OpenOCD nrf5 driver issue where the RRAMC write buffer
is not flushed for a trailing partial block, potentially corrupting the
ED25519 signature at the end of the MCUBoot image.
"""

import argparse
import sys


RECORD_SIZE = 16  # Target data-byte count per record


def ihex_checksum(raw_bytes: bytes) -> int:
    return (~sum(raw_bytes) + 1) & 0xFF


def pad_last_data_record(path: str) -> None:
    with open(path, "r") as f:
        lines = f.readlines()

    last_data_idx = None
    for i in range(len(lines) - 1, -1, -1):
        line = lines[i].strip()
        if len(line) < 11 or line[0] != ":":
            continue
        record_type = int(line[7:9], 16)
        if record_type == 0x00:
            last_data_idx = i
            break

    if last_data_idx is None:
        print("No data records found", file=sys.stderr)
        sys.exit(1)

    line = lines[last_data_idx].strip()
    byte_count = int(line[1:3], 16)

    if byte_count >= RECORD_SIZE:
        print(f"Last data record already {byte_count} bytes, nothing to do")
        return

    addr = line[3:7]
    data_hex = line[9 : 9 + byte_count * 2]
    pad_count = RECORD_SIZE - byte_count
    data_hex += "FF" * pad_count

    payload = bytes.fromhex(f"{RECORD_SIZE:02X}{addr}00{data_hex}")
    cs = ihex_checksum(payload)

    new_line = f":{RECORD_SIZE:02X}{addr}00{data_hex}{cs:02X}\n"
    lines[last_data_idx] = new_line

    with open(path, "w") as f:
        f.writelines(lines)

    print(
        f"Padded last data record at :{byte_count:02X}{addr}00 "
        f"with {pad_count} x 0xFF -> :{RECORD_SIZE:02X}{addr}00 "
        f"(checksum {cs:02X})"
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "hexfile",
        nargs="?",
        default="build/merged.hex",
        help="Path to Intel HEX file (default: build/merged.hex)",
    )
    args = parser.parse_args()
    pad_last_data_record(args.hexfile)


if __name__ == "__main__":
    main()
