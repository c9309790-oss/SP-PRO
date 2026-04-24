#!/usr/bin/env python3
import argparse
import json
from pathlib import Path


CTR_PACKAGE_BASE = 0x08003800
RAW_APP_BASE = 0x08004000
VECTOR_ADDR_MIN = 0x08000000
VECTOR_ADDR_MAX = 0x08100000


def ihex_record(record_type: int, address: int, data: bytes) -> str:
    count = len(data)
    checksum_base = count + ((address >> 8) & 0xFF) + (address & 0xFF) + record_type + sum(data)
    checksum = ((~checksum_base + 1) & 0xFF)
    return f":{count:02X}{address:04X}{record_type:02X}{data.hex().upper()}{checksum:02X}"


def read_u32_le(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset:offset + 4], byteorder="little", signed=False)


def looks_like_vector_table(data: bytes, offset: int) -> bool:
    if len(data) < offset + 8:
        return False
    msp = read_u32_le(data, offset)
    reset = read_u32_le(data, offset + 4)
    msp_ok = (msp & 0x2FFE0000) == 0x20000000
    reset_ok = VECTOR_ADDR_MIN <= reset < VECTOR_ADDR_MAX and (reset & 0x1) == 0x1
    return msp_ok and reset_ok


def analyze_bin(src: Path) -> dict:
    data = src.read_bytes()
    report = {
        "path": str(src),
        "size": len(data),
        "kind": "unknown",
        "kind_label": "Unknown BIN",
        "recommended_base": None,
        "recommended_base_hex": "",
        "has_2kb_ota_header": False,
        "vector_offset": None,
        "vector_offset_hex": "",
        "header_crc": None,
        "header_size": None,
        "notes": [],
    }

    if looks_like_vector_table(data, 0):
        report["kind"] = "raw_app"
        report["kind_label"] = "Raw APP BIN"
        report["recommended_base"] = RAW_APP_BASE
        report["has_2kb_ota_header"] = False
        report["vector_offset"] = 0
        report["notes"].append("Cortex-M vector table found at file start.")

    if len(data) >= 0x808 and looks_like_vector_table(data, 0x800):
        header_crc = read_u32_le(data, 0x000)
        header_size = read_u32_le(data, 0x004)
        size_matches = (header_size + 0x800) == len(data)
        report["kind"] = "ctr_package"
        report["kind_label"] = "CTR Package BIN (2KB OTA header)"
        report["recommended_base"] = CTR_PACKAGE_BASE
        report["has_2kb_ota_header"] = True
        report["vector_offset"] = 0x800
        report["header_crc"] = header_crc
        report["header_size"] = header_size
        if size_matches:
            report["notes"].append("Header size matches total file length.")
        else:
            report["notes"].append("Detected 2KB OTA header, but header size does not match file length.")

    if report["recommended_base"] is not None:
        report["recommended_base_hex"] = f"0x{report['recommended_base']:08X}"
    if report["vector_offset"] is not None:
        report["vector_offset_hex"] = f"0x{report['vector_offset']:X}"
    return report


def bin_to_ihex(src: Path, dst: Path, base_addr: int, line_size: int) -> None:
    data = src.read_bytes()
    lines = []
    current_upper = None

    for offset in range(0, len(data), line_size):
        abs_addr = base_addr + offset
        upper = (abs_addr >> 16) & 0xFFFF
        low = abs_addr & 0xFFFF
        chunk = data[offset:offset + line_size]

        if current_upper != upper:
            current_upper = upper
            upper_bytes = bytes([(upper >> 8) & 0xFF, upper & 0xFF])
            lines.append(ihex_record(0x04, 0x0000, upper_bytes))

        lines.append(ihex_record(0x00, low, chunk))

    lines.append(":00000001FF")
    dst.write_text("\n".join(lines) + "\n", encoding="ascii")


def analyze_ihex(src: Path) -> dict:
    upper = 0
    min_addr = None
    max_addr = None
    first_data_addr = None
    data_bytes = 0

    for raw_line in src.read_text(encoding="ascii").splitlines():
        line = raw_line.strip()
        if not line:
            continue
        if not line.startswith(":"):
            raise ValueError(f"Invalid HEX line: {line}")

        count = int(line[1:3], 16)
        address = int(line[3:7], 16)
        record_type = int(line[7:9], 16)
        data = bytes.fromhex(line[9:9 + count * 2])

        if record_type == 0x04:
            if count != 2:
                raise ValueError("Invalid extended linear address record length")
            upper = int.from_bytes(data, "big")
            continue
        if record_type != 0x00:
            continue

        abs_addr = (upper << 16) | address
        if first_data_addr is None:
            first_data_addr = abs_addr
        if min_addr is None or abs_addr < min_addr:
            min_addr = abs_addr
        record_end = abs_addr + count - 1
        if max_addr is None or record_end > max_addr:
            max_addr = record_end
        data_bytes += count

    return {
        "path": str(src),
        "first_data_addr": first_data_addr,
        "first_data_addr_hex": "" if first_data_addr is None else f"0x{first_data_addr:08X}",
        "min_addr": min_addr,
        "min_addr_hex": "" if min_addr is None else f"0x{min_addr:08X}",
        "max_addr": max_addr,
        "max_addr_hex": "" if max_addr is None else f"0x{max_addr:08X}",
        "data_bytes": data_bytes,
    }


def build_report(src: Path, dst: Path, base_addr: int) -> dict:
    bin_report = analyze_bin(src)
    hex_report = analyze_ihex(dst)
    expected_base = bin_report["recommended_base"]
    chosen_matches = expected_base is None or expected_base == base_addr
    hex_matches = hex_report["first_data_addr"] == base_addr
    success = chosen_matches and hex_matches

    result = {
        "input_bin": bin_report,
        "output_hex": hex_report,
        "selected_base": base_addr,
        "selected_base_hex": f"0x{base_addr:08X}",
        "status": "SUCCESS" if success else "FAIL",
        "status_ok": success,
        "chosen_base_matches_recommended": chosen_matches,
        "hex_first_addr_matches_selected": hex_matches,
        "summary": [],
    }

    result["summary"].append(f"BIN kind: {bin_report['kind_label']}")
    if expected_base is not None:
        result["summary"].append(f"Recommended base: {bin_report['recommended_base_hex']}")
    else:
        result["summary"].append("Recommended base: unknown")
    result["summary"].append(f"HEX first address: {hex_report['first_data_addr_hex']}")
    result["summary"].append(f"Selected base: {result['selected_base_hex']}")

    if not chosen_matches and expected_base is not None:
        result["summary"].append("FAIL: selected base does not match recommended base.")
    elif not hex_matches:
        result["summary"].append("FAIL: HEX first address does not match selected base.")
    else:
        result["summary"].append("SUCCESS: address verification passed.")

    return result


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Convert BIN to Intel HEX and verify addresses.")
    parser.add_argument("src", type=Path, help="Input .bin file")
    parser.add_argument("dst", type=Path, help="Output .hex file")
    parser.add_argument("--base", required=True, help="Base flash address, e.g. 0x08003800")
    parser.add_argument("--line-size", type=int, default=16, help="Data bytes per HEX record, default 16")
    parser.add_argument("--report-json", type=Path, help="Optional JSON report output path")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    base_addr = int(args.base, 0)
    bin_to_ihex(args.src, args.dst, base_addr, args.line_size)
    report = build_report(args.src, args.dst, base_addr)

    if args.report_json:
        args.report_json.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")

    print(f"Wrote {args.dst} from {args.src} at base address 0x{base_addr:08X}")
    for line in report["summary"]:
        print(line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
