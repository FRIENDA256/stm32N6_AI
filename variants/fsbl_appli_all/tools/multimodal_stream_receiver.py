#!/usr/bin/env python3
"""Receive and validate STM32N6 MMS2 telemetry frames."""

from __future__ import annotations

import argparse
import csv
import dataclasses
import json
import pathlib
import socket
import struct
import time
import zlib


MAGIC = b"MMS2"
PROTOCOL_VERSION = 2
HEADER_BYTES = 64
SOCKET_RECEIVE_BUFFER = 4 * 1024 * 1024
MAX_PAYLOAD_BYTES = 8 * 1024 * 1024
FLAG_PAYLOAD_CRC_VALID = 1 << 0
STREAM_AD7606_RAW = 1
STREAM_TINY1C_TEMP16 = 2
STREAM_AI_RESULT = 6
STREAM_AD_AI_BUNDLE = 8
FORMAT_TINY1C_TEMP16_V1 = 0x0201
FORMAT_AI_INT8_V1 = 0x0601
FORMAT_AD_AI_BUNDLE_V1 = 0x0801
FORMAT_AD_AI_BLOCK_V2 = 0x0802
HEADER_STRUCT = struct.Struct("!4sHHHHIIIQQQIIII")
AI_RESULT_STRUCT = struct.Struct("!HHIIIIIIIIIIiB4b11x")
AD_AI_BUNDLE_META_STRUCT = struct.Struct("!HH10IiHBBIQQB4b19x")
AD_AI_BLOCK_META_STRUCT = struct.Struct("!HH10IiHBBIQQHHIQQB4b11x")


@dataclasses.dataclass(frozen=True)
class StreamHeader:
    stream_id: int
    payload_format: int
    flags: int
    boot_session_id: int
    source_instance: int
    sequence: int
    capture_start_us: int
    capture_end_us: int
    payload_bytes: int
    aux0: int
    payload_crc32: int
    header_crc32: int


@dataclasses.dataclass(frozen=True)
class AIResult:
    schema_version: int
    class_count: int
    model_id: int
    model_version: int
    run_count: int
    input_frame_sequence: int
    input_timestamp_ms: int
    input_sample_counter: int
    inference_ms: int
    average_inference_ms: int
    max_inference_ms: int
    run_error_count: int
    runtime_status: int
    top_index: int
    outputs: tuple[int, int, int, int]


@dataclasses.dataclass(frozen=True)
class ADAIBundle:
    schema_version: int
    metadata_bytes: int
    model_id: int
    model_version: int
    run_count: int
    input_frame_sequence: int
    input_timestamp_ms: int
    input_sample_counter: int
    inference_ms: int
    average_inference_ms: int
    max_inference_ms: int
    run_error_count: int
    runtime_status: int
    points: int
    channels: int
    bytes_per_sample: int
    raw_bytes: int
    block_start: int
    block_end: int
    top_index: int
    outputs: tuple[int, int, int, int]
    raw_window: bytes


@dataclasses.dataclass(frozen=True)
class ADAIBlock:
    schema_version: int
    metadata_bytes: int
    model_id: int
    model_version: int
    run_count: int
    source_frame_sequence: int
    input_timestamp_ms: int
    input_sample_counter: int
    inference_ms: int
    average_inference_ms: int
    max_inference_ms: int
    run_error_count: int
    runtime_status: int
    source_points: int
    source_channels: int
    source_bytes_per_sample: int
    source_sample_bytes: int
    source_block_start: int
    source_block_end: int
    window_points: int
    window_bytes: int
    window_block_start: int
    window_block_end: int
    top_index: int
    outputs: tuple[int, int, int, int]
    raw_frame: bytes


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def build_header(
    *,
    stream_id: int,
    payload_format: int,
    flags: int,
    boot_session_id: int,
    source_instance: int,
    sequence: int,
    capture_start_us: int,
    capture_end_us: int,
    payload: bytes,
    aux0: int = 0,
) -> bytes:
    payload_crc = crc32(payload)
    prefix = HEADER_STRUCT.pack(
        MAGIC,
        PROTOCOL_VERSION,
        HEADER_BYTES,
        stream_id,
        payload_format,
        flags,
        boot_session_id,
        source_instance,
        sequence,
        capture_start_us,
        capture_end_us,
        len(payload),
        aux0,
        payload_crc,
        0,
    )
    return prefix[:60] + struct.pack("!I", crc32(prefix[:60]))


def decode_header(raw: bytes) -> StreamHeader:
    if len(raw) != HEADER_BYTES:
        raise ValueError(f"header length is {len(raw)}, expected {HEADER_BYTES}")
    fields = HEADER_STRUCT.unpack(raw)
    magic, version, header_bytes = fields[:3]
    if magic != MAGIC:
        raise ValueError(f"bad magic {magic!r}")
    if version != PROTOCOL_VERSION:
        raise ValueError(f"unsupported protocol version {version}")
    if header_bytes != HEADER_BYTES:
        raise ValueError(f"unsupported header length {header_bytes}")
    expected_header_crc = fields[-1]
    actual_header_crc = crc32(raw[:60])
    if actual_header_crc != expected_header_crc:
        raise ValueError(
            f"header CRC mismatch expected=0x{expected_header_crc:08X} "
            f"actual=0x{actual_header_crc:08X}"
        )
    payload_bytes = fields[11]
    if payload_bytes > MAX_PAYLOAD_BYTES:
        raise ValueError(f"payload too large: {payload_bytes}")
    return StreamHeader(
        stream_id=fields[3],
        payload_format=fields[4],
        flags=fields[5],
        boot_session_id=fields[6],
        source_instance=fields[7],
        sequence=fields[8],
        capture_start_us=fields[9],
        capture_end_us=fields[10],
        payload_bytes=payload_bytes,
        aux0=fields[12],
        payload_crc32=fields[13],
        header_crc32=fields[14],
    )


def decode_ai_result(payload: bytes) -> AIResult:
    if len(payload) != AI_RESULT_STRUCT.size:
        raise ValueError(f"AI payload length is {len(payload)}, expected {AI_RESULT_STRUCT.size}")
    fields = AI_RESULT_STRUCT.unpack(payload)
    return AIResult(
        schema_version=fields[0],
        class_count=fields[1],
        model_id=fields[2],
        model_version=fields[3],
        run_count=fields[4],
        input_frame_sequence=fields[5],
        input_timestamp_ms=fields[6],
        input_sample_counter=fields[7],
        inference_ms=fields[8],
        average_inference_ms=fields[9],
        max_inference_ms=fields[10],
        run_error_count=fields[11],
        runtime_status=fields[12],
        top_index=fields[13],
        outputs=(fields[14], fields[15], fields[16], fields[17]),
    )


def decode_ad_ai_bundle(payload: bytes) -> ADAIBundle:
    if len(payload) < AD_AI_BUNDLE_META_STRUCT.size:
        raise ValueError(
            f"AD+AI bundle length is {len(payload)}, "
            f"expected at least {AD_AI_BUNDLE_META_STRUCT.size}"
        )
    fields = AD_AI_BUNDLE_META_STRUCT.unpack(payload[: AD_AI_BUNDLE_META_STRUCT.size])
    metadata_bytes = fields[1]
    raw_bytes = fields[16]
    if metadata_bytes != AD_AI_BUNDLE_META_STRUCT.size:
        raise ValueError(f"unsupported AD+AI metadata length {metadata_bytes}")
    if len(payload) != metadata_bytes + raw_bytes:
        raise ValueError(
            f"AD+AI payload length is {len(payload)}, expected {metadata_bytes + raw_bytes}"
        )
    points, channels, bytes_per_sample = fields[13], fields[14], fields[15]
    expected_raw_bytes = points * channels * bytes_per_sample
    if raw_bytes != expected_raw_bytes:
        raise ValueError(
            f"AD+AI raw layout requires {expected_raw_bytes} bytes, metadata reports {raw_bytes}"
        )
    return ADAIBundle(
        schema_version=fields[0],
        metadata_bytes=metadata_bytes,
        model_id=fields[2],
        model_version=fields[3],
        run_count=fields[4],
        input_frame_sequence=fields[5],
        input_timestamp_ms=fields[6],
        input_sample_counter=fields[7],
        inference_ms=fields[8],
        average_inference_ms=fields[9],
        max_inference_ms=fields[10],
        run_error_count=fields[11],
        runtime_status=fields[12],
        points=points,
        channels=channels,
        bytes_per_sample=bytes_per_sample,
        raw_bytes=raw_bytes,
        block_start=fields[17],
        block_end=fields[18],
        top_index=fields[19],
        outputs=(fields[20], fields[21], fields[22], fields[23]),
        raw_window=payload[metadata_bytes:],
    )


def decode_ad_ai_block(payload: bytes) -> ADAIBlock:
    if len(payload) < AD_AI_BLOCK_META_STRUCT.size:
        raise ValueError(
            f"AD+AI block length is {len(payload)}, "
            f"expected at least {AD_AI_BLOCK_META_STRUCT.size}"
        )
    fields = AD_AI_BLOCK_META_STRUCT.unpack(payload[: AD_AI_BLOCK_META_STRUCT.size])
    metadata_bytes = fields[1]
    raw_bytes = fields[16]
    if metadata_bytes != AD_AI_BLOCK_META_STRUCT.size:
        raise ValueError(f"unsupported AD+AI block metadata length {metadata_bytes}")
    if len(payload) != metadata_bytes + raw_bytes:
        raise ValueError(
            f"AD+AI block payload length is {len(payload)}, "
            f"expected {metadata_bytes + raw_bytes}"
        )
    source_points, source_channels, bytes_per_sample = fields[13], fields[14], fields[15]
    expected_raw_bytes = source_points * source_channels * bytes_per_sample
    if raw_bytes != expected_raw_bytes:
        raise ValueError(
            f"AD+AI source layout requires {expected_raw_bytes} bytes, "
            f"metadata reports {raw_bytes}"
        )
    window_points = fields[19]
    window_bytes = fields[21]
    if window_bytes != window_points * source_channels * bytes_per_sample:
        raise ValueError(
            f"AD+AI window layout requires "
            f"{window_points * source_channels * bytes_per_sample} bytes, "
            f"metadata reports {window_bytes}"
        )
    return ADAIBlock(
        schema_version=fields[0],
        metadata_bytes=metadata_bytes,
        model_id=fields[2],
        model_version=fields[3],
        run_count=fields[4],
        source_frame_sequence=fields[5],
        input_timestamp_ms=fields[6],
        input_sample_counter=fields[7],
        inference_ms=fields[8],
        average_inference_ms=fields[9],
        max_inference_ms=fields[10],
        run_error_count=fields[11],
        runtime_status=fields[12],
        source_points=source_points,
        source_channels=source_channels,
        source_bytes_per_sample=bytes_per_sample,
        source_sample_bytes=raw_bytes,
        source_block_start=fields[17],
        source_block_end=fields[18],
        window_points=window_points,
        window_bytes=window_bytes,
        window_block_start=fields[22],
        window_block_end=fields[23],
        top_index=fields[24],
        outputs=(fields[25], fields[26], fields[27], fields[28]),
        raw_frame=payload[metadata_bytes:],
    )


class SessionRecorder:
    """Persist the validated stream without coupling disk writes to the UI."""

    def __init__(self, directory: str | pathlib.Path) -> None:
        self.directory = pathlib.Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.telemetry_path = self.directory / "telemetry.mms2"
        self.ad_path = self.directory / "ad7606_s16le.bin"
        self.ai_path = self.directory / "ai_results.csv"
        self.manifest_path = self.directory / "session.json"
        self.telemetry = self.telemetry_path.open("ab", buffering=1024 * 1024)
        self.ad = self.ad_path.open("ab", buffering=1024 * 1024)
        self.ai = self.ai_path.open("a", newline="", encoding="utf-8", buffering=1)
        self.ai_writer = csv.writer(self.ai)
        if self.ai_path.stat().st_size == 0:
            self.ai_writer.writerow(
                (
                    "boot_session_id",
                    "run_count",
                    "source_frame_sequence",
                    "input_timestamp_ms",
                    "input_sample_counter",
                    "source_block_start",
                    "source_block_end",
                    "window_block_start",
                    "window_block_end",
                    "ad_offset_bytes",
                    "ad_bytes",
                    "top_index",
                    "output_0",
                    "output_1",
                    "output_2",
                    "output_3",
                    "inference_ms",
                    "average_inference_ms",
                    "max_inference_ms",
                    "run_error_count",
                )
            )
        if not self.manifest_path.exists():
            self.manifest_path.write_text(
                json.dumps(
                    {
                        "protocol": "MMS2",
                        "protocol_version": PROTOCOL_VERSION,
                        "telemetry_format": "AD_AI_BLOCK_V2",
                        "sample_layout": "point-major interleaved signed-16 little-endian",
                        "sample_rate_hz": 25600,
                        "channels": 8,
                        "source_points_per_block": 512,
                        "ai_window_points": 1024,
                        "files": {
                            "telemetry": self.telemetry_path.name,
                            "ad_raw": self.ad_path.name,
                            "ai_results": self.ai_path.name,
                        },
                    },
                    indent=2,
                )
                + "\n",
                encoding="utf-8",
            )
        self.last_flush = time.monotonic()

    def record_message(
        self,
        raw_header: bytes,
        payload: bytes,
        header: StreamHeader,
        block: ADAIBlock | None,
    ) -> None:
        self.telemetry.write(raw_header)
        self.telemetry.write(payload)
        if block is not None:
            ad_offset = self.ad.tell()
            self.ad.write(block.raw_frame)
            self.ai_writer.writerow(
                (
                    header.boot_session_id,
                    block.run_count,
                    block.source_frame_sequence,
                    block.input_timestamp_ms,
                    block.input_sample_counter,
                    block.source_block_start,
                    block.source_block_end,
                    block.window_block_start,
                    block.window_block_end,
                    ad_offset,
                    len(block.raw_frame),
                    block.top_index,
                    *block.outputs,
                    block.inference_ms,
                    block.average_inference_ms,
                    block.max_inference_ms,
                    block.run_error_count,
                )
            )
        now = time.monotonic()
        if now - self.last_flush >= 1.0:
            self.flush()
            self.last_flush = now

    def flush(self) -> None:
        self.telemetry.flush()
        self.ad.flush()
        self.ai.flush()

    def close(self) -> None:
        self.flush()
        self.telemetry.close()
        self.ad.close()
        self.ai.close()


def recv_exact(sock: socket.socket, length: int) -> bytes:
    output = bytearray(length)
    view = memoryview(output)
    offset = 0
    while offset < length:
        received = sock.recv_into(view[offset:])
        if received == 0:
            raise ConnectionError("stream socket closed")
        offset += received
    return bytes(output)


def open_stream_socket(
    host: str,
    port: int,
    connect_timeout: float,
    receive_buffer: int = SOCKET_RECEIVE_BUFFER,
) -> socket.socket:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.settimeout(connect_timeout)
        sock.connect((host, port))
        return sock
    except BaseException:
        sock.close()
        raise


class ReceiverStats:
    def __init__(self) -> None:
        self.started = time.monotonic()
        self.interval_started = self.started
        self.messages = 0
        self.payload_bytes = 0
        self.interval_messages = 0
        self.interval_bytes = 0
        self.crc_errors = 0
        self.protocol_errors = 0
        self.connection_errors = 0
        self.sequence_gaps = 0
        self.stream_counts: dict[int, int] = {}
        self.last_sequence: dict[tuple[int, int], int] = {}
        self.last_ai_print = 0.0
        self.last_ai_top: int | None = None
        self.last_ir_print = 0.0
        self.last_source_frame_sequence: dict[int, int] = {}
        self.source_sequence_gaps = 0

    def record(self, header: StreamHeader) -> None:
        self.messages += 1
        self.payload_bytes += header.payload_bytes
        self.interval_messages += 1
        self.interval_bytes += header.payload_bytes
        self.stream_counts[header.stream_id] = self.stream_counts.get(header.stream_id, 0) + 1
        key = (header.boot_session_id, header.stream_id)
        previous = self.last_sequence.get(key)
        if previous is not None and header.sequence > previous + 1:
            self.sequence_gaps += header.sequence - previous - 1
        self.last_sequence[key] = header.sequence

    def print_interval(self, force: bool = False) -> None:
        now = time.monotonic()
        elapsed = now - self.interval_started
        if not force and elapsed < 1.0:
            return
        rate = 0.0 if elapsed <= 0 else self.interval_bytes * 8.0 / elapsed / 1_000_000.0
        print(
            f"RX msg={self.messages} ad={self.stream_counts.get(STREAM_AD7606_RAW, 0)} "
            f"ir={self.stream_counts.get(STREAM_TINY1C_TEMP16, 0)} "
            f"ai={self.stream_counts.get(STREAM_AI_RESULT, 0)} "
            f"bundle={self.stream_counts.get(STREAM_AD_AI_BUNDLE, 0)} "
            f"rate={rate:.2f}Mbps "
            f"gaps={self.sequence_gaps} source_gaps={self.source_sequence_gaps} "
            f"crc_err={self.crc_errors} "
            f"proto_err={self.protocol_errors} conn_err={self.connection_errors}"
        )
        self.interval_started = now
        self.interval_messages = 0
        self.interval_bytes = 0


def run_receiver(args: argparse.Namespace) -> int:
    stats = ReceiverStats()
    deadline = None if args.duration <= 0 else time.monotonic() + args.duration
    record_file = pathlib.Path(args.record) if args.record else None
    record_handle = None
    session_recorder = SessionRecorder(args.session_dir) if args.session_dir else None
    if record_file is not None:
        record_file.parent.mkdir(parents=True, exist_ok=True)
        record_handle = record_file.open("wb")

    try:
        while deadline is None or time.monotonic() < deadline:
            try:
                print(f"Connecting to {args.host}:{args.port}...")
                with open_stream_socket(
                    args.host,
                    args.port,
                    args.connect_timeout,
                ) as sock:
                    sock.settimeout(args.socket_timeout)
                    print("Connected; receiving MMS2 telemetry")
                    while deadline is None or time.monotonic() < deadline:
                        raw_header = recv_exact(sock, HEADER_BYTES)
                        header = decode_header(raw_header)
                        payload = recv_exact(sock, header.payload_bytes)
                        decoded_block = None
                        if header.flags & FLAG_PAYLOAD_CRC_VALID:
                            actual_crc = crc32(payload)
                            if actual_crc != header.payload_crc32:
                                stats.crc_errors += 1
                                raise ValueError(
                                    f"payload CRC mismatch stream={header.stream_id} "
                                    f"seq={header.sequence} expected=0x{header.payload_crc32:08X} "
                                    f"actual=0x{actual_crc:08X}"
                                )
                        if record_handle is not None:
                            record_handle.write(raw_header)
                            record_handle.write(payload)
                        stats.record(header)

                        if header.stream_id == STREAM_AI_RESULT and header.payload_format == FORMAT_AI_INT8_V1:
                            ai = decode_ai_result(payload)
                            now = time.monotonic()
                            if ai.top_index != stats.last_ai_top or now - stats.last_ai_print >= 2.0:
                                print(
                                    f"AI run={ai.run_count} input_seq={ai.input_frame_sequence} "
                                    f"top={ai.top_index} out={ai.outputs} infer={ai.inference_ms}ms "
                                    f"avg={ai.average_inference_ms}ms errors={ai.run_error_count}"
                                )
                                stats.last_ai_top = ai.top_index
                                stats.last_ai_print = now
                        elif (
                            header.stream_id == STREAM_TINY1C_TEMP16
                            and header.payload_format == FORMAT_TINY1C_TEMP16_V1
                        ):
                            expected_bytes = 256 * 192 * 2
                            if len(payload) != expected_bytes:
                                raise ValueError(
                                    f"Tiny1C payload length is {len(payload)}, "
                                    f"expected {expected_bytes}"
                                )
                            now = time.monotonic()
                            if now - stats.last_ir_print >= 2.0:
                                print(
                                    f"IR TEMP seq={header.sequence} "
                                    f"bytes={len(payload)} "
                                    f"timestamp={header.capture_end_us / 1000.0:.0f}ms"
                                )
                                stats.last_ir_print = now
                        elif (
                            header.stream_id == STREAM_AD_AI_BUNDLE
                            and header.payload_format == FORMAT_AD_AI_BLOCK_V2
                        ):
                            block = decode_ad_ai_block(payload)
                            decoded_block = block
                            if header.sequence != block.run_count:
                                raise ValueError(
                                    f"AD+AI block sequence {header.sequence} does not match AI run "
                                    f"{block.run_count}"
                                )
                            if header.aux0 != block.source_frame_sequence:
                                raise ValueError(
                                    f"AD+AI block aux AD sequence {header.aux0} does not match metadata "
                                    f"{block.source_frame_sequence}"
                                )
                            previous_source = stats.last_source_frame_sequence.get(
                                header.boot_session_id
                            )
                            if (
                                previous_source is not None
                                and block.source_frame_sequence > previous_source + 1
                            ):
                                stats.source_sequence_gaps += (
                                    block.source_frame_sequence - previous_source - 1
                                )
                            stats.last_source_frame_sequence[header.boot_session_id] = (
                                block.source_frame_sequence
                            )
                            now = time.monotonic()
                            if block.top_index != stats.last_ai_top or now - stats.last_ai_print >= 2.0:
                                print(
                                    f"BLOCK run={block.run_count} source_seq={block.source_frame_sequence} "
                                    f"samples={block.source_block_start}..{block.source_block_end} "
                                    f"raw={block.source_channels}x{block.source_points}x"
                                    f"{block.source_bytes_per_sample} "
                                    f"window={block.window_block_start}..{block.window_block_end} "
                                    f"top={block.top_index} out={block.outputs} "
                                    f"infer={block.inference_ms}ms avg={block.average_inference_ms}ms "
                                    f"errors={block.run_error_count}"
                                )
                                stats.last_ai_top = block.top_index
                                stats.last_ai_print = now
                        if session_recorder is not None:
                            session_recorder.record_message(
                                raw_header,
                                payload,
                                header,
                                decoded_block,
                            )
                        stats.print_interval()
            except ValueError as exc:
                stats.protocol_errors += 1
                print(f"Stream interrupted: {exc}")
                if not args.reconnect:
                    break
                if deadline is not None and time.monotonic() >= deadline:
                    break
                time.sleep(args.reconnect_delay)
            except (ConnectionError, OSError) as exc:
                stats.connection_errors += 1
                print(f"Stream interrupted: {exc}")
                if not args.reconnect:
                    break
                if deadline is not None and time.monotonic() >= deadline:
                    break
                time.sleep(args.reconnect_delay)
    except KeyboardInterrupt:
        print("Stopped by user")
    finally:
        stats.print_interval(force=True)
        if record_handle is not None:
            record_handle.close()
        if session_recorder is not None:
            session_recorder.close()

    failed = (
        stats.messages == 0
        or stats.crc_errors > 0
        or stats.protocol_errors > 0
        or stats.connection_errors > 0
    )
    if args.fail_on_gap and (
        stats.sequence_gaps > 0 or stats.source_sequence_gaps > 0
    ):
        failed = True
    return 1 if failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Receive STM32N6 MMS2 AD/AI telemetry")
    parser.add_argument("--host", default="192.168.6.50")
    parser.add_argument("--port", type=int, default=5100)
    parser.add_argument("--duration", type=float, default=30.0, help="seconds; 0 runs until Ctrl+C")
    parser.add_argument("--connect-timeout", type=float, default=5.0)
    parser.add_argument("--socket-timeout", type=float, default=10.0)
    parser.add_argument("--reconnect", action="store_true")
    parser.add_argument("--reconnect-delay", type=float, default=1.0)
    parser.add_argument("--record", help="write the raw MMS2 message sequence to this file")
    parser.add_argument(
        "--session-dir",
        help="write telemetry.mms2, contiguous AD S16LE data, and AI CSV into this directory",
    )
    parser.add_argument("--fail-on-gap", action="store_true")
    return run_receiver(parser.parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
