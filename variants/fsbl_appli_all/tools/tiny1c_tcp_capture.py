"""Capture and analyze Tiny1-C infrared frames from fsbl_appli_all.

Examples:
  python tools/tiny1c_tcp_capture.py --mode temp
  python tools/tiny1c_tcp_capture.py --mode image
  python tools/tiny1c_tcp_capture.py --mode both --temp-filter median3
  python tools/tiny1c_tcp_capture.py --input-raw tools/net_captures/tiny1c_temp_x.raw --kind temp
  python tools/tiny1c_tcp_capture.py --input-image-raw image.raw --input-temp-raw temp.raw --temp-filter median3
"""

import argparse
import binascii
import csv
import json
import math
import socket
import statistics
import struct
import time
from datetime import datetime
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except Exception:  # pragma: no cover - BMP output still works without Pillow.
    Image = None
    ImageDraw = None
    ImageFont = None


WIDTH = 256
HEIGHT = 192
PIXELS = WIDTH * HEIGHT
FRAME_BYTES = PIXELS * 2
TEMP_SCALE = 64.0
KELVIN_OFFSET = 273.15
BEGIN_MARKER = b"BEGIN_STM32N6_BINARY\r\n"
END_MARKER = b"END_STM32N6_BINARY"


def timestamp_id() -> str:
    return datetime.now().strftime("%Y%m%d_%H%M%S_%f")[:-3]


def clamp_u8(value: float) -> int:
    return max(0, min(255, int(round(value))))


def temp_c(raw: int) -> float:
    return raw / TEMP_SCALE - KELVIN_OFFSET


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return float("nan")
    ordered = sorted(values)
    pos = (len(ordered) - 1) * pct / 100.0
    lo = math.floor(pos)
    hi = math.ceil(pos)
    if lo == hi:
        return ordered[lo]
    frac = pos - lo
    return ordered[lo] * (1.0 - frac) + ordered[hi] * frac


def normalize(value: float, low: float, high: float) -> int:
    if high <= low:
        return 128
    return clamp_u8((value - low) * 255.0 / (high - low))


def temp_palette(level: int) -> tuple[int, int, int]:
    x = level / 255.0
    stops = [
        (0.00, (48, 18, 59)),
        (0.18, (38, 97, 156)),
        (0.36, (36, 155, 184)),
        (0.55, (99, 190, 123)),
        (0.72, (231, 190, 64)),
        (0.88, (220, 96, 39)),
        (1.00, (122, 4, 3)),
    ]
    for (a, ca), (b, cb) in zip(stops, stops[1:]):
        if x <= b:
            f = 0.0 if b == a else (x - a) / (b - a)
            return tuple(clamp_u8(ca[i] + (cb[i] - ca[i]) * f) for i in range(3))
    return stops[-1][1]


def write_bmp_gray8(path: Path, pixels: bytes | list[int], width: int, height: int) -> None:
    row_stride = (width + 3) & ~3
    pixel_data_size = row_stride * height
    palette_size = 256 * 4
    file_header_size = 14
    dib_header_size = 40
    pixel_offset = file_header_size + dib_header_size + palette_size
    file_size = pixel_offset + pixel_data_size
    padding = bytes(row_stride - width)

    with path.open("wb") as f:
        f.write(b"BM")
        f.write(struct.pack("<IHHI", file_size, 0, 0, pixel_offset))
        f.write(
            struct.pack(
                "<IiiHHIIiiII",
                dib_header_size,
                width,
                height,
                1,
                8,
                0,
                pixel_data_size,
                2835,
                2835,
                256,
                0,
            )
        )
        for i in range(256):
            f.write(bytes((i, i, i, 0)))
        row_source = pixels if isinstance(pixels, bytes) else bytes(pixels)
        for y in range(height - 1, -1, -1):
            start = y * width
            f.write(row_source[start : start + width])
            f.write(padding)


def write_bmp_rgb24(path: Path, pixels: list[tuple[int, int, int]], width: int, height: int) -> None:
    row_stride = (width * 3 + 3) & ~3
    pixel_data_size = row_stride * height
    header_size = 14 + 40
    padding = bytes(row_stride - width * 3)

    with path.open("wb") as f:
        f.write(b"BM")
        f.write(struct.pack("<IHHI", header_size + pixel_data_size, 0, 0, header_size))
        f.write(
            struct.pack(
                "<IiiHHIIiiII",
                40,
                width,
                height,
                1,
                24,
                0,
                pixel_data_size,
                2835,
                2835,
                0,
                0,
            )
        )
        for y in range(height - 1, -1, -1):
            start = y * width
            for r, g, b in pixels[start : start + width]:
                f.write(bytes((b, g, r)))
            f.write(padding)


def parse_header_line(header_line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    parts = header_line.strip().split()
    for part in parts:
        if "=" in part:
            key, value = part.split("=", 1)
            fields[key.upper()] = value
    return fields


def recv_until(sock: socket.socket, marker: bytes, timeout_s: float) -> bytes:
    deadline = time.monotonic() + timeout_s
    buf = bytearray()
    while marker not in buf:
        if time.monotonic() > deadline:
            tail = bytes(buf[-240:]).decode("ascii", errors="replace")
            raise TimeoutError(f"timeout waiting for {marker!r}; tail={tail!r}")
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            continue
        if not chunk:
            raise ConnectionError("socket closed before marker was received")
        buf.extend(chunk)
        text_tail = bytes(buf[-256:]).decode("ascii", errors="ignore")
        if "ERR " in text_tail and "BEGIN_STM32N6_BINARY" not in text_tail:
            raise RuntimeError(text_tail.strip())
    return bytes(buf)


def recv_exact(sock: socket.socket, first_bytes: bytes, size: int, timeout_s: float) -> bytes:
    deadline = time.monotonic() + timeout_s
    buf = bytearray(first_bytes)
    while len(buf) < size:
        if time.monotonic() > deadline:
            raise TimeoutError(f"timeout while reading payload: {len(buf)}/{size} bytes")
        try:
            chunk = sock.recv(size - len(buf))
        except socket.timeout:
            continue
        if not chunk:
            raise ConnectionError(f"socket closed while reading payload: {len(buf)}/{size} bytes")
        buf.extend(chunk)
    if len(buf) > size:
        raise ValueError(f"received {len(buf) - size} extra payload bytes")
    return bytes(buf)


def capture_tcp_frame(host: str, port: int, command: str, timeout_s: float) -> tuple[bytes, str, dict[str, str]]:
    with socket.create_connection((host, port), timeout=5.0) as sock:
        sock.settimeout(0.2)
        sock.sendall((command + "\r\n").encode("ascii"))

        header_and_more = recv_until(sock, BEGIN_MARKER, timeout_s)
        marker_index = header_and_more.find(BEGIN_MARKER)
        prefix = header_and_more[:marker_index]
        payload_start = header_and_more[marker_index + len(BEGIN_MARKER) :]
        prefix_text = prefix.decode("ascii", errors="ignore")
        header_lines = [line for line in prefix_text.splitlines() if line.startswith("STM32N6_BIN V1 ")]
        if not header_lines:
            raise ValueError(f"no STM32N6_BIN header before marker; prefix={prefix_text[-300:]!r}")
        header_line = header_lines[-1]
        fields = parse_header_line(header_line)

        byte_count = int(fields["BYTES"])
        expected_crc = int(fields["CRC32"], 16)
        payload = recv_exact(sock, payload_start, byte_count, timeout_s)
        actual_crc = binascii.crc32(payload) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            raise ValueError(f"CRC mismatch: expected 0x{expected_crc:08X}, got 0x{actual_crc:08X}")

        try:
            trailing = recv_until(sock, END_MARKER, 3.0)
            _ = trailing
        except Exception:
            pass

        return payload, header_line, fields


def image_gray_from_raw(data: bytes, width: int, height: int, lane: str) -> tuple[bytes, str]:
    if len(data) != width * height * 2:
        raise ValueError(f"expected {width * height * 2} bytes, got {len(data)}")

    if lane == "even":
        return bytes(data[0::2]), "even"
    if lane == "odd":
        return bytes(data[1::2]), "odd"

    out = bytearray(width * height)
    odd_rows = 0
    for y in range(height):
        row_byte_offset = y * width * 2
        even_score = 0
        odd_score = 0
        for x in range(width):
            even_score += abs(data[row_byte_offset + x * 2] - 128)
            odd_score += abs(data[row_byte_offset + x * 2 + 1] - 128)
        use_odd = odd_score > even_score
        if use_odd:
            odd_rows += 1
        lane_offset = 1 if use_odd else 0
        for x in range(width):
            out[y * width + x] = data[row_byte_offset + x * 2 + lane_offset]
    return bytes(out), f"auto even_rows={height - odd_rows} odd_rows={odd_rows}"


def frame_to_u16(data: bytes, endian: str) -> list[int]:
    if endian == "be":
        return [(data[i] << 8) | data[i + 1] for i in range(0, len(data), 2)]
    return [data[i] | (data[i + 1] << 8) for i in range(0, len(data), 2)]


def median_int(values: list[int]) -> int:
    if not values:
        return 0
    ordered = sorted(values)
    return ordered[len(ordered) // 2]


def temp_u16_from_raw(data: bytes, width: int, height: int, endian_mode: str) -> tuple[list[int], str]:
    if len(data) != width * height * 2:
        raise ValueError(f"expected {width * height * 2} bytes, got {len(data)}")
    if endian_mode in ("le", "be"):
        return frame_to_u16(data, endian_mode), endian_mode

    target_room_raw = int(round((25.0 + KELVIN_OFFSET) * TEMP_SCALE))
    pixels: list[int] = []
    be_rows = 0
    for y in range(height):
        row = data[y * width * 2 : (y + 1) * width * 2]
        row_le = frame_to_u16(row, "le")
        row_be = frame_to_u16(row, "be")
        le_med = median_int(row_le)
        be_med = median_int(row_be)
        use_be = abs(be_med - target_room_raw) < abs(le_med - target_room_raw)
        if use_be:
            pixels.extend(row_be)
            be_rows += 1
        else:
            pixels.extend(row_le)
    return pixels, f"row-auto le_rows={height - be_rows} be_rows={be_rows}"


def repair_temp_speckles(
    values: list[int],
    width: int,
    height: int,
    mode: str,
    threshold: int,
) -> tuple[list[int], str, int]:
    if mode == "none" or len(values) != width * height:
        return list(values), "none", 0

    out = list(values)
    repaired = 0
    for y in range(height):
        y0 = max(0, y - 1)
        y1 = min(height - 1, y + 1)
        for x in range(width):
            idx = y * width + x
            center = values[idx]
            x0 = max(0, x - 1)
            x1 = min(width - 1, x + 1)
            neighbors = []
            for ny in range(y0, y1 + 1):
                for nx in range(x0, x1 + 1):
                    nidx = ny * width + nx
                    if nidx != idx:
                        neighbors.append(values[nidx])
            neighbor_median = median_int(neighbors)
            if neighbor_median == 0:
                continue
            if mode == "median3":
                median = median_int(neighbors + [center])
                if median != center:
                    out[idx] = median
                    repaired += 1
            elif center == 0 or abs(center - neighbor_median) > threshold:
                out[idx] = neighbor_median
                repaired += 1

    if mode == "median3":
        return out, f"median3 repaired={repaired}", repaired
    return out, f"despeckle threshold={threshold} repaired={repaired}", repaired


def count_temp_isolated_outliers(
    values: list[int],
    width: int,
    height: int,
    threshold: int,
) -> dict[str, object]:
    count = 0
    zeros = 0
    examples: list[dict[str, int]] = []
    rows: dict[int, int] = {}
    chunks: dict[int, int] = {}

    if len(values) != width * height:
        return {
            "count": 0,
            "zeros": 0,
            "threshold_raw": threshold,
            "examples": [],
            "top_rows": [],
            "top_spi_chunks": [],
        }

    for y in range(height):
        y0 = max(0, y - 1)
        y1 = min(height - 1, y + 1)
        for x in range(width):
            idx = y * width + x
            center = values[idx]
            if center == 0:
                zeros += 1

            neighbors = []
            x0 = max(0, x - 1)
            x1 = min(width - 1, x + 1)
            for ny in range(y0, y1 + 1):
                for nx in range(x0, x1 + 1):
                    nidx = ny * width + nx
                    if nidx != idx:
                        neighbors.append(values[nidx])

            neighbor_median = median_int(neighbors)
            if neighbor_median == 0:
                continue
            if center == 0 or abs(center - neighbor_median) > threshold:
                count += 1
                rows[y] = rows.get(y, 0) + 1
                chunks[(idx * 2) // 4096] = chunks.get((idx * 2) // 4096, 0) + 1
                if len(examples) < 16:
                    examples.append(
                        {
                            "x": x,
                            "y": y,
                            "raw": int(center),
                            "neighbor_median": int(neighbor_median),
                            "byte_offset": idx * 2,
                        }
                    )

    top_rows = sorted(rows.items(), key=lambda item: (-item[1], item[0]))[:8]
    top_chunks = sorted(chunks.items(), key=lambda item: (-item[1], item[0]))[:8]
    return {
        "count": count,
        "zeros": zeros,
        "threshold_raw": threshold,
        "examples": examples,
        "top_rows": [{"row": row, "count": row_count} for row, row_count in top_rows],
        "top_spi_chunks": [{"chunk": chunk, "count": chunk_count} for chunk, chunk_count in top_chunks],
    }


def detect_trailing_zero_run(values: list[int], width: int, height: int) -> dict[str, int | bool]:
    if len(values) != width * height:
        return {"present": False, "count": 0}

    count = 0
    idx = len(values) - 1
    while idx >= 0 and values[idx] == 0:
        count += 1
        idx -= 1

    if count == 0 or count >= width:
        return {"present": False, "count": count}

    start = len(values) - count
    return {
        "present": True,
        "count": count,
        "start_x": start % width,
        "start_y": start // width,
        "end_x": width - 1,
        "end_y": height - 1,
        "start_byte_offset": start * 2,
    }


def repair_trailing_zero_run(values: list[int], width: int, height: int) -> tuple[list[int], dict[str, int | bool]]:
    tail = detect_trailing_zero_run(values, width, height)
    if not tail["present"]:
        return list(values), tail

    out = list(values)
    start_x = int(tail["start_x"])
    start_y = int(tail["start_y"])
    count = int(tail["count"])

    if start_y <= 0:
        return out, tail

    for i in range(count):
        x = start_x + i
        idx = start_y * width + x
        above_idx = (start_y - 1) * width + x
        if out[idx] == 0 and out[above_idx] != 0:
            out[idx] = out[above_idx]

    return out, tail


def gray_from_u16(values: list[int], percentile_stretch: bool, invert: bool) -> tuple[bytes, dict[str, int]]:
    nonzero = [v for v in values if v != 0]
    if not nonzero:
        low, high = 0, 65535
    elif percentile_stretch:
        ordered = sorted(nonzero)
        low = ordered[int(math.floor((len(ordered) - 1) * 0.01))]
        high = ordered[int(math.ceil((len(ordered) - 1) * 0.99))]
    else:
        low, high = min(nonzero), max(nonzero)
    span = max(1, high - low)
    out = bytearray(len(values))
    for i, value in enumerate(values):
        if value <= low:
            level = 0
        elif value >= high:
            level = 255
        else:
            level = round((value - low) * 255.0 / span)
        if invert:
            level = 255 - level
        out[i] = clamp_u8(level)
    return bytes(out), {"low": int(low), "high": int(high), "nonzero": len(nonzero)}


def temps_from_u16(values: list[int]) -> list[float]:
    return [temp_c(v) if v else float("nan") for v in values]


def temp_stats(values: list[int], repaired: int, endian_summary: str, filter_summary: str) -> dict[str, object]:
    temps = temps_from_u16(values)
    valid = [(idx, temps[idx]) for idx, raw in enumerate(values) if raw != 0 and not math.isnan(temps[idx])]
    if not valid:
        raise ValueError("no valid temperature pixels")
    valid_temps = [t for _, t in valid]
    min_idx, min_c = min(valid, key=lambda item: item[1])
    max_idx, max_c = max(valid, key=lambda item: item[1])
    center_idx = (HEIGHT // 2) * WIDTH + (WIDTH // 2)
    center_c = temps[center_idx]
    return {
        "width": WIDTH,
        "height": HEIGHT,
        "pixels": PIXELS,
        "valid_pixels": len(valid_temps),
        "zero_pixels": PIXELS - len(valid_temps),
        "formula": "temp_c = raw_u16 / 64 - 273.15",
        "endian": endian_summary,
        "filter": filter_summary,
        "repaired_pixels": repaired,
        "min_c": round(min_c, 4),
        "min_xy": [min_idx % WIDTH, min_idx // WIDTH],
        "max_c": round(max_c, 4),
        "max_xy": [max_idx % WIDTH, max_idx // WIDTH],
        "center_c": round(center_c, 4) if not math.isnan(center_c) else None,
        "center_xy": [WIDTH // 2, HEIGHT // 2],
        "mean_c": round(statistics.fmean(valid_temps), 4),
        "median_c": round(statistics.median(valid_temps), 4),
        "p01_c": round(percentile(valid_temps, 1), 4),
        "p05_c": round(percentile(valid_temps, 5), 4),
        "p95_c": round(percentile(valid_temps, 95), 4),
        "p99_c": round(percentile(valid_temps, 99), 4),
    }


def temp_levels(values: list[int], low_c: float, high_c: float) -> list[int]:
    levels: list[int] = []
    for raw in values:
        if raw == 0:
            levels.append(0)
            continue
        levels.append(normalize(temp_c(raw), low_c, high_c))
    return levels


def set_rgb_pixel(
    pixels: list[tuple[int, int, int]],
    width: int,
    height: int,
    x: int,
    y: int,
    color: tuple[int, int, int],
) -> None:
    if 0 <= x < width and 0 <= y < height:
        pixels[y * width + x] = color


def draw_cross_pixels(
    pixels: list[tuple[int, int, int]],
    width: int,
    height: int,
    xy: list[int],
    color: tuple[int, int, int],
    radius: int,
) -> None:
    x, y = xy
    for dx in range(-radius, radius + 1):
        set_rgb_pixel(pixels, width, height, x + dx, y, color)
    for dy in range(-radius, radius + 1):
        set_rgb_pixel(pixels, width, height, x, y + dy, color)
    for dx in range(-3, 4):
        set_rgb_pixel(pixels, width, height, x + dx, y - radius, color)
        set_rgb_pixel(pixels, width, height, x + dx, y + radius, color)
    for dy in range(-3, 4):
        set_rgb_pixel(pixels, width, height, x - radius, y + dy, color)
        set_rgb_pixel(pixels, width, height, x + radius, y + dy, color)


def compose_side_by_side(
    left: list[tuple[int, int, int]],
    right: list[tuple[int, int, int]],
    width: int,
    height: int,
    gap: int = 8,
) -> tuple[list[tuple[int, int, int]], int, int]:
    out_w = width * 2 + gap
    out = [(24, 24, 24)] * (out_w * height)
    for y in range(height):
        for x in range(width):
            out[y * out_w + x] = left[y * width + x]
            out[y * out_w + width + gap + x] = right[y * width + x]
    return out, out_w, height


def draw_cross(draw: ImageDraw.ImageDraw, xy: tuple[int, int], color: tuple[int, int, int], radius: int) -> None:
    x, y = xy
    draw.line((x - radius, y, x + radius, y), fill=color, width=2)
    draw.line((x, y - radius, x, y + radius), fill=color, width=2)
    draw.rectangle((x - radius, y - radius, x + radius, y + radius), outline=color, width=1)


def save_temp_distribution_png(path: Path, values: list[int], stats: dict[str, object], scale: int) -> None:
    if Image is None:
        return
    low_c = float(stats["p01_c"])
    high_c = float(stats["p99_c"])
    levels = temp_levels(values, low_c, high_c)
    heat = Image.new("RGB", (WIDTH, HEIGHT))
    heat.putdata([temp_palette(level) for level in levels])
    heat_big = heat.resize((WIDTH * scale, HEIGHT * scale), Image.Resampling.NEAREST)

    pad = 12
    info_h = 72
    bar_w = 34
    canvas = Image.new("RGB", (heat_big.width + bar_w + pad * 3 + 92, heat_big.height + info_h + pad * 2), (18, 18, 18))
    canvas.paste(heat_big, (pad, pad + info_h))
    draw = ImageDraw.Draw(canvas)
    try:
        font = ImageFont.truetype("arial.ttf", 16)
        small = ImageFont.truetype("arial.ttf", 13)
    except Exception:
        font = None
        small = None

    def scaled_xy(xy: list[int]) -> tuple[int, int]:
        x, y = xy
        return pad + x * scale + scale // 2, pad + info_h + y * scale + scale // 2

    draw_cross(draw, scaled_xy(stats["max_xy"]), (255, 255, 255), 12)
    draw_cross(draw, scaled_xy(stats["min_xy"]), (0, 255, 255), 10)
    draw_cross(draw, scaled_xy(stats["center_xy"]), (0, 255, 0), 8)

    bar_x = pad + heat_big.width + pad
    bar_y = pad + info_h
    for y in range(heat_big.height):
        level = 255 - int(y * 255 / max(1, heat_big.height - 1))
        draw.line((bar_x, bar_y + y, bar_x + bar_w, bar_y + y), fill=temp_palette(level))
    draw.rectangle((bar_x, bar_y, bar_x + bar_w, bar_y + heat_big.height), outline=(230, 230, 230))
    for frac, label in [(0.0, high_c), (0.5, (low_c + high_c) / 2.0), (1.0, low_c)]:
        y = bar_y + int(frac * heat_big.height)
        draw.line((bar_x + bar_w, y, bar_x + bar_w + 6, y), fill=(230, 230, 230))
        draw.text((bar_x + bar_w + 10, y - 8), f"{label:.1f} C", fill=(230, 230, 230), font=small)

    draw.text(
        (pad, pad),
        f"min {stats['min_c']:.2f} C @ {stats['min_xy']}   max {stats['max_c']:.2f} C @ {stats['max_xy']}",
        fill=(255, 255, 255),
        font=font,
    )
    draw.text(
        (pad, pad + 24),
        f"center {stats['center_c']:.2f} C   mean {stats['mean_c']:.2f} C   p1..p99 {low_c:.2f}..{high_c:.2f} C",
        fill=(210, 210, 210),
        font=font,
    )
    draw.text((pad, pad + 48), f"{stats['endian']} | {stats['filter']}", fill=(180, 180, 180), font=small)
    canvas.save(path)


def save_png_gray(path: Path, gray: bytes, width: int, height: int) -> None:
    if Image is None:
        return
    Image.frombytes("L", (width, height), gray).save(path)


def save_png_rgb(path: Path, pixels: list[tuple[int, int, int]], width: int, height: int) -> None:
    if Image is None:
        return
    img = Image.new("RGB", (width, height))
    img.putdata(pixels)
    img.save(path)


def write_temp_csv(path: Path, values: list[int]) -> None:
    temps = temps_from_u16(values)
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["x", "y", "raw_u16", "temp_c"])
        for idx, raw in enumerate(values):
            x = idx % WIDTH
            y = idx // WIDTH
            t = temps[idx]
            writer.writerow([x, y, raw, f"{t:.4f}" if raw else ""])


def save_temp_outputs(
    data: bytes,
    base: Path,
    temp_endian: str,
    temp_filter: str,
    despike_threshold: int,
    invert: bool,
    export_csv: bool,
    scale: int,
    tail_repair_preview: bool,
) -> dict[str, Path]:
    raw_values, endian_summary = temp_u16_from_raw(data, WIDTH, HEIGHT, temp_endian)
    unfiltered_gray, unfiltered_range = gray_from_u16(raw_values, percentile_stretch=True, invert=invert)
    raw_outliers = count_temp_isolated_outliers(raw_values, WIDTH, HEIGHT, despike_threshold)
    tail_repaired_values, tail_zero_run = repair_trailing_zero_run(raw_values, WIDTH, HEIGHT)
    repaired_values, filter_summary, repaired = repair_temp_speckles(
        raw_values, WIDTH, HEIGHT, temp_filter, despike_threshold
    )
    filtered_gray, filtered_range = gray_from_u16(repaired_values, percentile_stretch=True, invert=invert)
    stats = temp_stats(repaired_values, repaired, endian_summary, filter_summary)

    paths = {
        "unfiltered_bmp": base.with_name(base.name + "_temp_unfiltered_gray.bmp"),
        "gray_bmp": base.with_name(base.name + "_temp_gray.bmp"),
        "thermal_bmp": base.with_name(base.name + "_temp_thermal.bmp"),
        "stats_json": base.with_name(base.name + "_temp_stats.json"),
        "summary_txt": base.with_name(base.name + "_temp_summary.txt"),
    }
    write_bmp_gray8(paths["unfiltered_bmp"], unfiltered_gray, WIDTH, HEIGHT)
    write_bmp_gray8(paths["gray_bmp"], filtered_gray, WIDTH, HEIGHT)

    if tail_repair_preview and tail_zero_run["present"]:
        tail_repaired_gray, _ = gray_from_u16(tail_repaired_values, percentile_stretch=True, invert=invert)
        paths["tail_repaired_bmp"] = base.with_name(base.name + "_temp_tail_repaired_gray.bmp")
        write_bmp_gray8(paths["tail_repaired_bmp"], tail_repaired_gray, WIDTH, HEIGHT)

    levels = temp_levels(repaired_values, float(stats["p01_c"]), float(stats["p99_c"]))
    thermal_pixels = [temp_palette(level) for level in levels]
    write_bmp_rgb24(paths["thermal_bmp"], thermal_pixels, WIDTH, HEIGHT)

    if Image is not None:
        paths["gray_png"] = base.with_name(base.name + "_temp_gray.png")
        paths["thermal_png"] = base.with_name(base.name + "_temp_thermal.png")
        paths["distribution_png"] = base.with_name(base.name + "_temp_distribution.png")
        save_png_gray(paths["gray_png"], filtered_gray, WIDTH, HEIGHT)
        save_png_rgb(paths["thermal_png"], thermal_pixels, WIDTH, HEIGHT)
        save_temp_distribution_png(paths["distribution_png"], repaired_values, stats, scale)
        if tail_repair_preview and tail_zero_run["present"]:
            paths["tail_repaired_png"] = base.with_name(base.name + "_temp_tail_repaired_gray.png")
            save_png_gray(paths["tail_repaired_png"], tail_repaired_gray, WIDTH, HEIGHT)

    stats["display_raw_low"] = unfiltered_range["low"]
    stats["display_raw_high"] = unfiltered_range["high"]
    stats["display_filtered_low"] = filtered_range["low"]
    stats["display_filtered_high"] = filtered_range["high"]
    stats["raw_isolated_outliers"] = raw_outliers
    stats["trailing_zero_run"] = tail_zero_run
    paths["stats_json"].write_text(json.dumps(stats, indent=2, ensure_ascii=False), encoding="utf-8")
    summary_lines = [
        "Tiny1-C temperature analysis",
        f"formula: {stats['formula']}",
        f"endian: {stats['endian']}",
        f"filter: {stats['filter']}",
        f"valid pixels: {stats['valid_pixels']} / {stats['pixels']}",
        f"raw isolated outliers: {raw_outliers['count']} (zeros={raw_outliers['zeros']}, threshold={raw_outliers['threshold_raw']})",
        (
            "trailing zero run: "
            f"count={tail_zero_run['count']} "
            f"start=({tail_zero_run.get('start_x', '-')},{tail_zero_run.get('start_y', '-')}) "
            f"offset={tail_zero_run.get('start_byte_offset', '-')}"
        ),
        f"min: {stats['min_c']:.2f} C at {stats['min_xy']}",
        f"max: {stats['max_c']:.2f} C at {stats['max_xy']}",
        f"center: {stats['center_c']:.2f} C at {stats['center_xy']}" if stats["center_c"] is not None else "center: invalid",
        f"mean/median: {stats['mean_c']:.2f} C / {stats['median_c']:.2f} C",
        f"p01/p99 display range: {stats['p01_c']:.2f} C / {stats['p99_c']:.2f} C",
    ]
    if raw_outliers["examples"]:
        first = raw_outliers["examples"][0]
        summary_lines.append(
            "first raw outlier: "
            f"x={first['x']} y={first['y']} raw={first['raw']} "
            f"neighbor_median={first['neighbor_median']} offset={first['byte_offset']}"
        )
    paths["summary_txt"].write_text("\n".join(summary_lines) + "\n", encoding="utf-8")

    if export_csv:
        paths["csv"] = base.with_name(base.name + "_temp_pixels.csv")
        write_temp_csv(paths["csv"], repaired_values)

    return paths


def save_image_outputs(data: bytes, base: Path, image_lane: str, invert: bool) -> tuple[dict[str, Path], str]:
    gray, lane_summary = image_gray_from_raw(data, WIDTH, HEIGHT, image_lane)
    if invert:
        gray = bytes(255 - v for v in gray)
    paths = {
        "gray_bmp": base.with_name(base.name + "_image_y8.bmp"),
    }
    write_bmp_gray8(paths["gray_bmp"], gray, WIDTH, HEIGHT)
    if Image is not None:
        paths["gray_png"] = base.with_name(base.name + "_image_y8.png")
        save_png_gray(paths["gray_png"], gray, WIDTH, HEIGHT)
    return paths, lane_summary


def save_joint_outputs(image_data: bytes, temp_data: bytes, base: Path, args: argparse.Namespace) -> dict[str, Path]:
    image_gray, lane_summary = image_gray_from_raw(image_data, WIDTH, HEIGHT, args.image_lane)
    if args.invert:
        image_gray = bytes(255 - v for v in image_gray)
    image_pixels = [(v, v, v) for v in image_gray]

    raw_values, endian_summary = temp_u16_from_raw(temp_data, WIDTH, HEIGHT, args.temp_endian)
    repaired_values, filter_summary, repaired = repair_temp_speckles(
        raw_values, WIDTH, HEIGHT, args.temp_filter, args.despike_threshold
    )
    stats = temp_stats(repaired_values, repaired, endian_summary, filter_summary)
    levels = temp_levels(repaired_values, float(stats["p01_c"]), float(stats["p99_c"]))
    temp_pixels = [temp_palette(level) for level in levels]

    markers = [
        ("max", stats["max_xy"], (255, 0, 0), 9),
        ("min", stats["min_xy"], (0, 255, 255), 7),
        ("center", stats["center_xy"], (0, 255, 0), 6),
    ]
    for _, xy, color, radius in markers:
        draw_cross_pixels(image_pixels, WIDTH, HEIGHT, xy, color, radius)
        draw_cross_pixels(temp_pixels, WIDTH, HEIGHT, xy, color, radius)

    combined, combined_w, combined_h = compose_side_by_side(image_pixels, temp_pixels, WIDTH, HEIGHT)
    paths = {
        "joint_image_bmp": base.with_name(base.name + "_joint_image_hotspots.bmp"),
        "joint_temp_bmp": base.with_name(base.name + "_joint_temp_hotspots.bmp"),
        "joint_side_bmp": base.with_name(base.name + "_joint_side_by_side.bmp"),
        "joint_json": base.with_name(base.name + "_joint_summary.json"),
        "joint_txt": base.with_name(base.name + "_joint_summary.txt"),
    }
    write_bmp_rgb24(paths["joint_image_bmp"], image_pixels, WIDTH, HEIGHT)
    write_bmp_rgb24(paths["joint_temp_bmp"], temp_pixels, WIDTH, HEIGHT)
    write_bmp_rgb24(paths["joint_side_bmp"], combined, combined_w, combined_h)

    if Image is not None:
        paths["joint_image_png"] = base.with_name(base.name + "_joint_image_hotspots.png")
        paths["joint_temp_png"] = base.with_name(base.name + "_joint_temp_hotspots.png")
        paths["joint_side_png"] = base.with_name(base.name + "_joint_side_by_side.png")
        save_png_rgb(paths["joint_image_png"], image_pixels, WIDTH, HEIGHT)
        save_png_rgb(paths["joint_temp_png"], temp_pixels, WIDTH, HEIGHT)
        save_png_rgb(paths["joint_side_png"], combined, combined_w, combined_h)

    summary = {
        "width": WIDTH,
        "height": HEIGHT,
        "image_lane": lane_summary,
        "formula": "temp_c = raw_u16 / 64 - 273.15",
        "endian": stats["endian"],
        "filter": stats["filter"],
        "marker_colors": {"max": "red", "min": "cyan", "center": "green"},
        "valid_pixels": stats["valid_pixels"],
        "zero_pixels": stats["zero_pixels"],
        "min_c": stats["min_c"],
        "min_xy": stats["min_xy"],
        "max_c": stats["max_c"],
        "max_xy": stats["max_xy"],
        "center_c": stats["center_c"],
        "center_xy": stats["center_xy"],
        "mean_c": stats["mean_c"],
        "median_c": stats["median_c"],
        "display_p01_c": stats["p01_c"],
        "display_p99_c": stats["p99_c"],
    }
    paths["joint_json"].write_text(json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8")
    lines = [
        "Tiny1-C joint image/temp analysis",
        f"image lane: {summary['image_lane']}",
        f"formula: {summary['formula']}",
        f"endian: {summary['endian']}",
        f"filter: {summary['filter']}",
        "markers: red=max, cyan=min, green=center",
        f"max: {summary['max_c']:.2f} C at {summary['max_xy']}",
        f"min: {summary['min_c']:.2f} C at {summary['min_xy']}",
        f"center: {summary['center_c']:.2f} C at {summary['center_xy']}"
        if summary["center_c"] is not None
        else "center: invalid",
        f"mean/median: {summary['mean_c']:.2f} C / {summary['median_c']:.2f} C",
        f"p01/p99 display range: {summary['display_p01_c']:.2f} C / {summary['display_p99_c']:.2f} C",
    ]
    paths["joint_txt"].write_text("\n".join(lines) + "\n", encoding="utf-8")
    return paths


def resolve_kind(kind: str, command: str, input_raw: Path | None, fields: dict[str, str] | None = None) -> str:
    if kind != "auto":
        return kind
    if fields and fields.get("CMD", "").upper() == "0XCC":
        return "temp"
    if "TEMP" in command.upper():
        return "temp"
    if input_raw and "temp" in input_raw.name.lower():
        return "temp"
    return "image"


def process_tiny1c_frame(
    data: bytes,
    out_dir: Path,
    base_name: str,
    kind: str,
    args: argparse.Namespace,
    header_line: str | None = None,
) -> None:
    if len(data) != FRAME_BYTES:
        raise ValueError(f"expected Tiny1C frame {FRAME_BYTES} bytes, got {len(data)}")
    out_dir.mkdir(parents=True, exist_ok=True)
    base = out_dir / base_name
    raw_path = base.with_suffix(".raw")
    raw_path.write_bytes(data)
    print(f"Saved raw : {raw_path}")
    if header_line:
        header_path = base.with_suffix(".txt")
        header_path.write_text(header_line + "\n", encoding="ascii")
        print(f"Saved hdr : {header_path}")

    if kind == "image":
        paths, lane_summary = save_image_outputs(data, base, args.image_lane, args.invert)
        print(f"Image mode: y-only, lane={lane_summary}")
        for path in paths.values():
            print(f"Saved image: {path}")
        return

    paths = save_temp_outputs(
        data,
        base,
        args.temp_endian,
        args.temp_filter,
        args.despike_threshold,
        args.invert,
        args.csv,
        args.scale,
        args.tail_repair_preview,
    )
    print(f"Temp outputs written for base: {base}")
    for key, path in paths.items():
        print(f"Saved {key}: {path}")


def command_for_mode(mode: str) -> list[str]:
    if mode == "both":
        return ["IRGETIMG", "IRGETTEMP"]
    if mode == "image":
        return ["IRGETIMG"]
    return ["IRGETTEMP"]


def main() -> None:
    parser = argparse.ArgumentParser(description="Capture and analyze Tiny1-C frames over the STM32N6 TCP command port.")
    parser.add_argument("--host", default="192.168.6.50")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument(
        "--command",
        choices=[
            "IRGETIMG",
            "IRGETIMAGE",
            "IRGETTEMP",
            "IRGETIMGBASE",
            "IRGETIMAGEBASE",
            "IRGETTEMPBASE",
            "IRNETIMG",
            "IRNETTEMP",
            "IRNETIMGBASE",
            "IRNETTEMPBASE",
        ],
        default=None,
    )
    parser.add_argument("--mode", choices=["image", "temp", "both"], default="temp")
    parser.add_argument("--frames", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument("--out", type=Path, default=Path(__file__).resolve().parent / "net_captures_py")
    parser.add_argument("--input-raw", type=Path, default=None)
    parser.add_argument("--input-image-raw", type=Path, default=None)
    parser.add_argument("--input-temp-raw", type=Path, default=None)
    parser.add_argument("--kind", choices=["auto", "image", "temp"], default="auto")
    parser.add_argument("--image-lane", choices=["auto", "even", "odd"], default="auto")
    parser.add_argument("--temp-endian", choices=["le", "be", "row-auto"], default="le")
    parser.add_argument("--temp-filter", choices=["none", "despeckle", "median3"], default="none")
    parser.add_argument(
        "--tail-repair-preview",
        action="store_true",
        help="Also write a preview image with a trailing zero run filled from the previous row; raw data is unchanged.",
    )
    parser.add_argument("--despike-threshold", type=int, default=512)
    parser.add_argument("--scale", type=int, default=3)
    parser.add_argument("--csv", action="store_true")
    parser.add_argument("--invert", action="store_true")
    parser.add_argument("--no-joint", action="store_true", help="Do not make joint image/temp hotspot outputs for paired frames.")
    args = parser.parse_args()

    if (args.input_image_raw is None) != (args.input_temp_raw is None):
        parser.error("--input-image-raw and --input-temp-raw must be used together")

    if args.input_image_raw is not None and args.input_temp_raw is not None:
        stamp = timestamp_id()
        image_path = args.input_image_raw.resolve()
        temp_path = args.input_temp_raw.resolve()
        image_data = image_path.read_bytes()
        temp_data = temp_path.read_bytes()
        process_tiny1c_frame(image_data, args.out, f"{image_path.stem}_py_{stamp}", "image", args)
        process_tiny1c_frame(temp_data, args.out, f"{temp_path.stem}_py_{stamp}", "temp", args)
        if not args.no_joint:
            joint_paths = save_joint_outputs(image_data, temp_data, args.out / f"tiny1c_joint_py_{stamp}", args)
            print(f"Joint outputs written for base: {args.out / f'tiny1c_joint_py_{stamp}'}")
            for key, path in joint_paths.items():
                print(f"Saved {key}: {path}")
        return

    if args.input_raw is not None:
        raw_path = args.input_raw.resolve()
        data = raw_path.read_bytes()
        kind = resolve_kind(args.kind, args.command or "", raw_path)
        stamp = timestamp_id()
        base_name = f"{raw_path.stem}_py_{stamp}"
        process_tiny1c_frame(data, args.out, base_name, kind, args)
        return

    commands = [args.command] if args.command else command_for_mode(args.mode)
    if args.command is None and args.mode == "both":
        for frame_idx in range(args.frames):
            stamp = timestamp_id()
            suffix = f"_{frame_idx + 1:03d}" if args.frames > 1 else ""
            print(f"Connecting {args.host}:{args.port}, command=IRGETIMG...")
            image_data, image_header, _ = capture_tcp_frame(args.host, args.port, "IRGETIMG", args.timeout)
            print(f"Frame header: {image_header}")
            print(f"CRC OK: 0x{binascii.crc32(image_data) & 0xFFFFFFFF:08X}")
            process_tiny1c_frame(image_data, args.out, f"tiny1c_image_{stamp}{suffix}", "image", args, image_header)

            print(f"Connecting {args.host}:{args.port}, command=IRGETTEMP...")
            temp_data, temp_header, _ = capture_tcp_frame(args.host, args.port, "IRGETTEMP", args.timeout)
            print(f"Frame header: {temp_header}")
            print(f"CRC OK: 0x{binascii.crc32(temp_data) & 0xFFFFFFFF:08X}")
            process_tiny1c_frame(temp_data, args.out, f"tiny1c_temp_{stamp}{suffix}", "temp", args, temp_header)

            if not args.no_joint:
                joint_base = args.out / f"tiny1c_joint_{stamp}{suffix}"
                joint_paths = save_joint_outputs(image_data, temp_data, joint_base, args)
                print(f"Joint outputs written for base: {joint_base}")
                for key, path in joint_paths.items():
                    print(f"Saved {key}: {path}")
        return

    for frame_idx in range(args.frames):
        for command in commands:
            print(f"Connecting {args.host}:{args.port}, command={command}...")
            data, header_line, fields = capture_tcp_frame(args.host, args.port, command, args.timeout)
            kind = resolve_kind(args.kind, command, None, fields)
            stamp = timestamp_id()
            base_name = f"tiny1c_{kind}_{stamp}"
            if args.frames > 1:
                base_name += f"_{frame_idx + 1:03d}"
            print(f"Frame header: {header_line}")
            print(f"CRC OK: 0x{binascii.crc32(data) & 0xFFFFFFFF:08X}")
            process_tiny1c_frame(data, args.out, base_name, kind, args, header_line)


if __name__ == "__main__":
    main()
