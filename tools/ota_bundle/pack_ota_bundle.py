#!/usr/bin/env python3
"""
Build, inspect, and extract merged OTA bundle files.

The bundle format is defined in:
    main/ota_ctr_esp32/ota_bundle_format.h
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import struct
import sys
import zlib
from dataclasses import dataclass
from types import SimpleNamespace
from typing import List, Optional


MAGIC = 0x4D41544F  # 'OTAM'
FORMAT_VERSION = 1
HEADER_SIZE = 4096
ALIGNMENT = 4096
MAX_ENTRIES = 2

TYPE_NONE = 0
TYPE_ESP32 = 1
TYPE_CTR = 2

TYPE_NAME = {
    TYPE_NONE: "none",
    TYPE_ESP32: "esp32",
    TYPE_CTR: "ctr",
}

ENTRY_STRUCT = struct.Struct("<III16sI32s32sI28s")
HEADER_PREFIX_STRUCT = struct.Struct("<IHHHHI16s32s")
EXPECTED_ENTRY_SIZE = 128
EXPECTED_HEADER_RESERVED_SIZE = 3776
PACKAGE_MD5_OFFSET = 16
PACKAGE_MD5_SIZE = 16


def looks_like_bundle(path: pathlib.Path) -> bool:
    with path.open("rb") as fh:
        magic = fh.read(4)
    return len(magic) == 4 and struct.unpack("<I", magic)[0] == MAGIC


@dataclass
class BundleEntry:
    payload_type: int
    path: pathlib.Path
    name: str
    version: str
    size: int
    offset: int = 0
    md5: bytes = b""
    crc32: int = 0
    flags: int = 0

    def to_manifest(self) -> dict:
        return {
            "type": TYPE_NAME[self.payload_type],
            "name": self.name,
            "version": self.version,
            "offset": self.offset,
            "size": self.size,
            "md5": self.md5.hex(),
            "crc32": f"{self.crc32:08x}",
            "flags": self.flags,
            "path": str(self.path),
        }


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def compute_file_hashes(path: pathlib.Path) -> tuple[int, bytes]:
    md5 = hashlib.md5()
    crc = 0
    with path.open("rb") as fh:
        while True:
            chunk = fh.read(1024 * 1024)
            if not chunk:
                break
            md5.update(chunk)
            crc = zlib.crc32(chunk, crc)
    return crc & 0xFFFFFFFF, md5.digest()


def sanitize_text(value: str, max_len: int) -> bytes:
    raw = value.encode("ascii", errors="ignore")[: max_len - 1]
    return raw + b"\0" * (max_len - len(raw))


def parse_text_field(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("ascii", errors="ignore")


def build_entries(args: argparse.Namespace) -> List[BundleEntry]:
    raw_entries: List[BundleEntry] = []
    if args.ctr:
        ctr_path = pathlib.Path(args.ctr).resolve()
        if not ctr_path.is_file():
            raise FileNotFoundError(f"CTR payload not found: {ctr_path}")
        if looks_like_bundle(ctr_path):
            raise ValueError(f"CTR payload must be a raw CTR firmware bin, not a merged ota bundle: {ctr_path}")
        raw_entries.append(
            BundleEntry(
                payload_type=TYPE_CTR,
                path=ctr_path,
                name=args.ctr_name or ctr_path.name,
                version=args.ctr_version or "",
                size=ctr_path.stat().st_size,
            )
        )
    if args.esp32:
        esp32_path = pathlib.Path(args.esp32).resolve()
        if not esp32_path.is_file():
            raise FileNotFoundError(f"ESP32 payload not found: {esp32_path}")
        if looks_like_bundle(esp32_path):
            raise ValueError(f"ESP32 payload must be a raw ESP32 app bin, not a merged ota bundle: {esp32_path}")
        raw_entries.append(
            BundleEntry(
                payload_type=TYPE_ESP32,
                path=esp32_path,
                name=args.esp32_name or esp32_path.name,
                version=args.esp32_version or "",
                size=esp32_path.stat().st_size,
            )
        )
    if not raw_entries:
        raise ValueError("At least one payload is required. Use --ctr and/or --esp32.")
    if len(raw_entries) > MAX_ENTRIES:
        raise ValueError(f"Too many payloads. Max supported entries: {MAX_ENTRIES}.")

    offset = HEADER_SIZE
    for entry in raw_entries:
        entry.offset = align_up(offset, args.align)
        entry.crc32, entry.md5 = compute_file_hashes(entry.path)
        offset = entry.offset + entry.size
    return raw_entries


def build_header_bytes(
    entries: List[BundleEntry],
    package_version: str,
    package_size: int,
    package_md5: bytes,
) -> bytes:
    if len(entries) > MAX_ENTRIES:
        raise ValueError(f"Entry count exceeds {MAX_ENTRIES}.")

    header_prefix = HEADER_PREFIX_STRUCT.pack(
        MAGIC,
        FORMAT_VERSION,
        HEADER_SIZE,
        len(entries),
        0,
        package_size,
        package_md5,
        sanitize_text(package_version, 32),
    )

    packed_entries = bytearray()
    for entry in entries:
        packed_entries.extend(
            ENTRY_STRUCT.pack(
                entry.payload_type,
                entry.offset,
                entry.size,
                entry.md5,
                entry.crc32,
                sanitize_text(entry.name, 32),
                sanitize_text(entry.version, 32),
                entry.flags,
                b"\0" * 28,
            )
        )
    while len(packed_entries) < EXPECTED_ENTRY_SIZE * MAX_ENTRIES:
        packed_entries.extend(b"\0" * EXPECTED_ENTRY_SIZE)

    header = bytearray()
    header.extend(header_prefix)
    header.extend(packed_entries)
    if len(header) > HEADER_SIZE:
        raise ValueError("Header overflow.")
    header.extend(b"\0" * (HEADER_SIZE - len(header)))
    return bytes(header)


def copy_file_to_position(src: pathlib.Path, dst_fh, target_offset: int) -> None:
    current = dst_fh.tell()
    if current > target_offset:
        raise ValueError("Target offset is behind current file position.")
    if current < target_offset:
        dst_fh.write(b"\xFF" * (target_offset - current))
    with src.open("rb") as src_fh:
        while True:
            chunk = src_fh.read(1024 * 1024)
            if not chunk:
                break
            dst_fh.write(chunk)


def compute_md5_of_file(path: pathlib.Path) -> bytes:
    md5 = hashlib.md5()
    with path.open("rb") as fh:
        while True:
            chunk = fh.read(1024 * 1024)
            if not chunk:
                break
            md5.update(chunk)
    return md5.digest()


def compute_package_md5(path: pathlib.Path) -> bytes:
    md5 = hashlib.md5()
    processed = 0
    with path.open("rb") as fh:
        while True:
            chunk = fh.read(1024 * 1024)
            if not chunk:
                break
            if processed < HEADER_SIZE:
                mutable = bytearray(chunk)
                chunk_start = processed
                chunk_end = processed + len(chunk)
                field_start = PACKAGE_MD5_OFFSET
                field_end = PACKAGE_MD5_OFFSET + PACKAGE_MD5_SIZE
                overlap_start = max(chunk_start, field_start)
                overlap_end = min(chunk_end, field_end)
                if overlap_start < overlap_end:
                    start = overlap_start - chunk_start
                    end = overlap_end - chunk_start
                    mutable[start:end] = b"\0" * (end - start)
                md5.update(mutable)
            else:
                md5.update(chunk)
            processed += len(chunk)
    return md5.digest()


def pack_bundle(args: argparse.Namespace) -> int:
    output = pathlib.Path(args.out).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)

    entries = build_entries(args)
    package_version = args.package_version or ""

    header = build_header_bytes(entries, package_version, 0, b"\0" * 16)
    with output.open("wb") as out_fh:
        out_fh.write(header)
        for entry in entries:
            copy_file_to_position(entry.path, out_fh, entry.offset)

    package_size = output.stat().st_size
    header_without_md5 = build_header_bytes(entries, package_version, package_size, b"\0" * 16)
    with output.open("r+b") as out_fh:
        out_fh.seek(0)
        out_fh.write(header_without_md5)

    header_package_md5 = compute_package_md5(output)
    final_header = build_header_bytes(entries, package_version, package_size, header_package_md5)
    with output.open("r+b") as out_fh:
        out_fh.seek(0)
        out_fh.write(final_header)

    file_md5 = compute_md5_of_file(output)

    manifest = {
        "format": "ota-bundle",
        "magic": f"0x{MAGIC:08X}",
        "version": FORMAT_VERSION,
        "header_size": HEADER_SIZE,
        "alignment": args.align,
        "package_version": package_version,
        "output": str(output),
        "package_size": package_size,
        "package_md5": file_md5.hex(),
        "file_md5": file_md5.hex(),
        "header_package_md5": header_package_md5.hex(),
        "entries": [entry.to_manifest() for entry in entries],
    }

    if args.info_json:
        info_path = pathlib.Path(args.info_json).resolve()
        info_path.parent.mkdir(parents=True, exist_ok=True)
        info_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    print(json.dumps(manifest, indent=2))
    return 0


def pack_bundle_to_manifest(
    *,
    out: str,
    ctr: Optional[str] = None,
    esp32: Optional[str] = None,
    ctr_version: str = "",
    esp32_version: str = "",
    ctr_name: str = "",
    esp32_name: str = "",
    package_version: str = "",
    align: int = ALIGNMENT,
    info_json: Optional[str] = None,
) -> dict:
    args = SimpleNamespace(
        out=out,
        ctr=ctr,
        esp32=esp32,
        ctr_version=ctr_version,
        esp32_version=esp32_version,
        ctr_name=ctr_name,
        esp32_name=esp32_name,
        package_version=package_version,
        align=align,
        info_json=info_json,
    )

    output = pathlib.Path(args.out).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)

    entries = build_entries(args)
    version = args.package_version or ""

    header = build_header_bytes(entries, version, 0, b"\0" * 16)
    with output.open("wb") as out_fh:
        out_fh.write(header)
        for entry in entries:
            copy_file_to_position(entry.path, out_fh, entry.offset)

    package_size = output.stat().st_size
    header_without_md5 = build_header_bytes(entries, version, package_size, b"\0" * 16)
    with output.open("r+b") as out_fh:
        out_fh.seek(0)
        out_fh.write(header_without_md5)

    header_package_md5 = compute_package_md5(output)
    final_header = build_header_bytes(entries, version, package_size, header_package_md5)
    with output.open("r+b") as out_fh:
        out_fh.seek(0)
        out_fh.write(final_header)

    file_md5 = compute_md5_of_file(output)

    manifest = {
        "format": "ota-bundle",
        "magic": f"0x{MAGIC:08X}",
        "version": FORMAT_VERSION,
        "header_size": HEADER_SIZE,
        "alignment": args.align,
        "package_version": version,
        "output": str(output),
        "package_size": package_size,
        "package_md5": file_md5.hex(),
        "file_md5": file_md5.hex(),
        "header_package_md5": header_package_md5.hex(),
        "entries": [entry.to_manifest() for entry in entries],
    }

    if args.info_json:
        info_path = pathlib.Path(args.info_json).resolve()
        info_path.parent.mkdir(parents=True, exist_ok=True)
        info_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    return manifest


def parse_header(bundle_path: pathlib.Path) -> dict:
    with bundle_path.open("rb") as fh:
        raw_header = fh.read(HEADER_SIZE)
    if len(raw_header) < HEADER_SIZE:
        raise ValueError("Bundle is smaller than fixed header size.")

    prefix_size = HEADER_PREFIX_STRUCT.size
    (
        magic,
        format_version,
        header_size,
        entry_count,
        flags,
        package_size,
        package_md5,
        package_version_raw,
    ) = HEADER_PREFIX_STRUCT.unpack(raw_header[:prefix_size])

    if magic != MAGIC:
        raise ValueError(f"Invalid magic: 0x{magic:08X}")
    if format_version != FORMAT_VERSION:
        raise ValueError(f"Unsupported format version: {format_version}")
    if header_size != HEADER_SIZE:
        raise ValueError(f"Unexpected header size: {header_size}")
    if entry_count < 1 or entry_count > MAX_ENTRIES:
        raise ValueError(f"Invalid entry count: {entry_count}")

    entries = []
    offset = prefix_size
    for index in range(MAX_ENTRIES):
        chunk = raw_header[offset : offset + ENTRY_STRUCT.size]
        offset += ENTRY_STRUCT.size
        (
            payload_type,
            payload_offset,
            payload_size,
            payload_md5,
            payload_crc32,
            name_raw,
            version_raw,
            entry_flags,
            _reserved,
        ) = ENTRY_STRUCT.unpack(chunk)

        if index >= entry_count:
            continue
        if payload_type not in TYPE_NAME:
            raise ValueError(f"Invalid payload type at entry {index}: {payload_type}")
        entries.append(
            {
                "index": index,
                "type": TYPE_NAME[payload_type],
                "type_id": payload_type,
                "offset": payload_offset,
                "size": payload_size,
                "md5": payload_md5.hex(),
                "crc32": f"{payload_crc32:08x}",
                "name": parse_text_field(name_raw),
                "version": parse_text_field(version_raw),
                "flags": entry_flags,
            }
        )

    actual_size = bundle_path.stat().st_size
    file_md5 = compute_md5_of_file(bundle_path).hex()
    actual_header_package_md5 = compute_package_md5(bundle_path).hex()

    return {
        "format": "ota-bundle",
        "path": str(bundle_path),
        "magic": f"0x{magic:08X}",
        "version": format_version,
        "header_size": header_size,
        "entry_count": entry_count,
        "flags": flags,
        "package_size": package_size,
        "actual_size": actual_size,
        "package_md5": file_md5,
        "file_md5": file_md5,
        "header_package_md5": package_md5.hex(),
        "actual_header_package_md5": actual_header_package_md5,
        "package_version": parse_text_field(package_version_raw),
        "entries": entries,
    }


def inspect_bundle(args: argparse.Namespace) -> int:
    bundle_path = pathlib.Path(args.bundle).resolve()
    info = parse_header(bundle_path)
    print(json.dumps(info, indent=2))
    return 0


def inspect_bundle_to_info(bundle: str) -> dict:
    return parse_header(pathlib.Path(bundle).resolve())


def extract_bundle(args: argparse.Namespace) -> int:
    bundle_path = pathlib.Path(args.bundle).resolve()
    info = parse_header(bundle_path)
    out_dir = pathlib.Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    with bundle_path.open("rb") as src_fh:
        for entry in info["entries"]:
            default_name = entry["name"] or f"{entry['type']}.bin"
            out_path = out_dir / default_name
            src_fh.seek(entry["offset"])
            remaining = entry["size"]
            with out_path.open("wb") as out_fh:
                while remaining > 0:
                    chunk = src_fh.read(min(1024 * 1024, remaining))
                    if not chunk:
                        raise ValueError(f"Unexpected EOF while extracting {default_name}")
                    out_fh.write(chunk)
                    remaining -= len(chunk)
            print(f"Extracted {entry['type']} -> {out_path}")
    return 0


def extract_bundle_entries(bundle: str, out_dir: str) -> List[str]:
    bundle_path = pathlib.Path(bundle).resolve()
    info = parse_header(bundle_path)
    out_dir_path = pathlib.Path(out_dir).resolve()
    out_dir_path.mkdir(parents=True, exist_ok=True)

    extracted: List[str] = []
    with bundle_path.open("rb") as src_fh:
        for entry in info["entries"]:
            default_name = entry["name"] or f"{entry['type']}.bin"
            out_path = out_dir_path / default_name
            src_fh.seek(entry["offset"])
            remaining = entry["size"]
            with out_path.open("wb") as out_fh:
                while remaining > 0:
                    chunk = src_fh.read(min(1024 * 1024, remaining))
                    if not chunk:
                        raise ValueError(f"Unexpected EOF while extracting {default_name}")
                    out_fh.write(chunk)
                    remaining -= len(chunk)
            extracted.append(str(out_path))
    return extracted


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Pack, inspect, and extract merged OTA bundle files."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    pack = subparsers.add_parser("pack", help="Create a merged OTA bundle.")
    pack.add_argument("--ctr", help="CTR firmware payload path.")
    pack.add_argument("--esp32", help="ESP32 firmware payload path.")
    pack.add_argument("--ctr-version", default="", help="Optional CTR payload version string.")
    pack.add_argument("--esp32-version", default="", help="Optional ESP32 payload version string.")
    pack.add_argument("--ctr-name", default="", help="Optional CTR payload name override.")
    pack.add_argument("--esp32-name", default="", help="Optional ESP32 payload name override.")
    pack.add_argument("--package-version", default="", help="Optional merged package version string.")
    pack.add_argument("--align", type=int, default=ALIGNMENT, help="Payload alignment in bytes.")
    pack.add_argument("--out", required=True, help="Output merged bundle file path.")
    pack.add_argument("--info-json", help="Optional manifest json output path.")
    pack.set_defaults(func=pack_bundle)

    inspect = subparsers.add_parser("inspect", help="Show merged OTA bundle metadata.")
    inspect.add_argument("bundle", help="Merged OTA bundle file path.")
    inspect.set_defaults(func=inspect_bundle)

    extract = subparsers.add_parser("extract", help="Extract payloads from a merged OTA bundle.")
    extract.add_argument("bundle", help="Merged OTA bundle file path.")
    extract.add_argument("--out-dir", required=True, help="Extraction output directory.")
    extract.set_defaults(func=extract_bundle)

    return parser


def main(argv: Optional[List[str]] = None) -> int:
    parser = make_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
