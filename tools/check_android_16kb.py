#!/usr/bin/env python3
"""Check actual 64-bit ELF LOAD segments and APK native-library ZIP alignment."""
import argparse
from pathlib import Path
import struct
import sys
import zipfile

PAGE_SIZE = 16384


def check_elf(data):
    if len(data) < 64 or data[:4] != b"\x7fELF" or data[5] not in (1, 2):
        raise ValueError("invalid ELF header")
    # Android's 16 KB requirement applies to the 64-bit ABIs.
    if data[4] == 1:
        return None
    if data[4] != 2:
        raise ValueError("unknown ELF class")
    endian = "<" if data[5] == 1 else ">"
    machine = struct.unpack_from(endian + "H", data, 18)[0]
    if machine not in (62, 183):
        raise ValueError("unexpected Android 64-bit ELF machine: " + str(machine))
    offset = struct.unpack_from(endian + "Q", data, 32)[0]
    entry_size, count = struct.unpack_from(endian + "HH", data, 54)
    if entry_size < 56 or count == 0 or offset + entry_size * count > len(data):
        raise ValueError("invalid ELF program-header table")
    loads = []
    for index in range(count):
        segment = struct.unpack_from(endian + "IIQQQQQQ", data, offset + index * entry_size)
        if segment[0] == 1:  # PT_LOAD
            loads.append(segment)
    if not loads:
        raise ValueError("ELF has no LOAD segments")
    minimum = min(segment[7] for segment in loads)
    if minimum < PAGE_SIZE:
        raise ValueError("ELF LOAD alignment is 0x%x; requires at least 0x4000" % minimum)
    if any(segment[2] % PAGE_SIZE != segment[3] % PAGE_SIZE for segment in loads):
        raise ValueError("ELF LOAD file offsets and virtual addresses are not 16 KB congruent")
    return minimum


def apk_data_offset(archive, info):
    archive.seek(info.header_offset)
    header = archive.read(30)
    if len(header) != 30 or header[:4] != b"PK\x03\x04":
        raise ValueError("invalid ZIP local header")
    name_size, extra_size = struct.unpack_from("<HH", header, 26)
    return info.header_offset + 30 + name_size + extra_size


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=Path, help=".so, native-library directory, APK or AAB")
    parser.add_argument("--quiet", action="store_true", help="print failures and the summary only")
    args = parser.parse_args()
    checked, skipped, failures = 0, 0, 0

    def inspect(label, data, zip_offset=None):
        nonlocal checked, skipped, failures
        try:
            alignment = check_elf(data)
            if alignment is None:
                skipped += 1
                return
            if zip_offset is not None and zip_offset % PAGE_SIZE:
                raise ValueError("uncompressed APK ZIP data offset is not 16 KB aligned")
            checked += 1
            if not args.quiet:
                print("PASS %s (ELF 0x%x)" % (label, alignment))
        except (ValueError, struct.error) as error:
            failures += 1
            print("FAIL %s: %s" % (label, error), file=sys.stderr)

    for target in args.paths:
        if not target.exists():
            parser.error("missing input: " + str(target))
        if target.is_dir():
            for library in sorted(target.rglob("*.so")):
                inspect(str(library), library.read_bytes())
        elif target.suffix.lower() in (".apk", ".aab"):
            with zipfile.ZipFile(target) as bundle, target.open("rb") as raw:
                for info in bundle.infolist():
                    if not info.filename.endswith(".so") or "/lib/" not in "/" + info.filename:
                        continue
                    offset = None
                    if target.suffix.lower() == ".apk" and info.compress_type == zipfile.ZIP_STORED:
                        offset = apk_data_offset(raw, info)
                    inspect(str(target) + "!" + info.filename, bundle.read(info), offset)
        else:
            inspect(str(target), target.read_bytes())
    if checked == 0 and failures == 0:
        failures += 1
        print("FAIL no Android 64-bit native libraries found", file=sys.stderr)
    print("[android-16kb] %d compatible 64-bit libraries, %d skipped 32-bit libraries, %d failures" %
          (checked, skipped, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
