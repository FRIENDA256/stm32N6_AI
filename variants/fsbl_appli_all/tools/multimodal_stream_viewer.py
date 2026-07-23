#!/usr/bin/env python3
"""Two-page AD7606 + AI and Tiny1C viewer for STM32N6 MMS2 streams.

The board sends one AD+AI bundle for each inference.  The receiver thread
validates and decodes the stream, while the Tk UI renders only the newest
bundle. Page buttons also switch the board's mutually exclusive stream mode.
"""

from __future__ import annotations

import argparse
import collections
import math
import pathlib
import socket
import struct
import sys
import threading
import time
from array import array
from dataclasses import dataclass
from tkinter import Canvas, PhotoImage, StringVar, Tk, TclError, ttk
from typing import Optional

try:
    from PIL import Image, ImageTk
except ImportError:  # pragma: no cover - exercised only on minimal Python installs
    Image = None
    ImageTk = None

try:
    import numpy as np
except ImportError:  # pragma: no cover - exercised only on minimal Python installs
    np = None

from multimodal_stream_receiver import (
    FLAG_PAYLOAD_CRC_VALID,
    FORMAT_AD_AI_BUNDLE_V1,
    FORMAT_AD_AI_BLOCK_V2,
    FORMAT_TINY1C_TEMP16_V1,
    HEADER_BYTES,
    STREAM_AD_AI_BUNDLE,
    STREAM_TINY1C_TEMP16,
    ADAIBlock,
    ADAIBundle,
    SessionRecorder,
    crc32,
    decode_ad_ai_block,
    decode_ad_ai_bundle,
    decode_header,
    open_stream_socket,
    recv_exact,
)
from tiny1c_tcp_capture import (
    normalize,
    percentile,
    temp_palette,
    temp_u16_from_raw,
)


APP_TITLE = "STM32N6 Multimodal Viewer"
DEFAULT_HOST = "192.168.6.50"
DEFAULT_PORT = 5100
DEFAULT_IR_PORT = 5101
DEFAULT_COMMAND_PORT = 5000
DEFAULT_SAMPLE_RATE = 25600.0
DEFAULT_REFRESH_MS = 50
IR_WIDTH = 256
IR_HEIGHT = 192
IR_DISPLAY_WIDTH = 320
IR_DISPLAY_HEIGHT = 240
IR_PALETTE_RGB = bytes(
    component
    for level in range(256)
    for component in temp_palette(level)
)
PLOT_BACKGROUND = "#182126"
PLOT_GRID = "#304047"
PLOT_TEXT = "#b8c8c7"
PLOT_COLORS = (
    "#39c0ba",
    "#f2c14e",
    "#e07a5f",
    "#81b29a",
    "#7aa2f7",
    "#c084fc",
    "#f78c6c",
    "#9cdcfe",
)
WINDOW_BACKGROUND = "#101518"
PANEL_BACKGROUND = "#151d21"
ACCENT = "#53d6cf"


@dataclass(frozen=True)
class StreamEvent:
    kind: str
    value: object


@dataclass(frozen=True)
class BundleSnapshot:
    bundle: ADAIBundle | ADAIBlock
    received_count: int
    sequence_gaps: int
    source_sequence_gaps: int
    rate_fps: float
    wire_mbps: float


@dataclass(frozen=True)
class IRFrameSnapshot:
    pixels: bytes
    width: int
    height: int
    valid_pixels: int
    low_c: float
    high_c: float
    captured_at: float
    sequence: int
    rate_fps: float


class UDPRecvBuffer:
    """Wrap a bound UDP socket so recv_exact() works the same as on TCP.

    The board sends each MMS2 message as a sequence of ≤1400-byte UDP
    datagrams in order.  On a direct Ethernet link datagrams arrive in
    transmission order, so we can treat the datagram stream as a byte stream
    by simply appending received datagrams into a buffer.
    """

    def __init__(self, sock: socket.socket) -> None:
        self._sock = sock
        self._buf = bytearray()

    def recv_exact(self, n: int) -> bytes:
        while len(self._buf) < n:
            data, _ = self._sock.recvfrom(65536)
            self._buf += data
        result = bytes(self._buf[:n])
        self._buf = self._buf[n:]
        return result

    def resync(self) -> None:
        """Scan forward until a datagram that starts with the MMS2 magic is found.

        UDP datagrams are independent so we can't seek within one; instead we
        discard the current buffer and read whole datagrams until the magic
        appears at position 0 of a datagram (which is always the case for the
        first datagram of every MMS2 message).
        """
        MAGIC = b"MMS2"
        self._buf = bytearray()
        for _ in range(512):          # safety limit
            try:
                data, _ = self._sock.recvfrom(65536)
            except OSError:
                return
            if data[:4] == MAGIC:
                self._buf = bytearray(data)
                return

    def settimeout(self, timeout: float) -> None:
        self._sock.settimeout(timeout)

    def setsockopt(self, *args: object) -> None:  # absorb TCP_NODELAY calls
        pass


def open_udp_recv_socket(port: int, rcvbuf: int = 4 * 1024 * 1024) -> socket.socket:
    """Create a UDP socket bound to *port* on all interfaces."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)  # receive subnet broadcasts
    sock.bind(("", port))
    return sock


class ReceiverMailbox:
    """Keep all control events and only the newest high-rate data bundle."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._control_events: collections.deque[StreamEvent] = collections.deque()
        self._latest_bundle: Optional[StreamEvent] = None
        self._latest_ir_frame: Optional[StreamEvent] = None

    def put(self, event: StreamEvent) -> None:
        with self._lock:
            if event.kind == "bundle":
                self._latest_bundle = event
            elif event.kind == "ir_frame":
                self._latest_ir_frame = event
            else:
                self._control_events.append(event)

    def drain(self) -> list[StreamEvent]:
        with self._lock:
            events = list(self._control_events)
            self._control_events.clear()
            if self._latest_bundle is not None:
                events.append(self._latest_bundle)
                self._latest_bundle = None
            if self._latest_ir_frame is not None:
                events.append(self._latest_ir_frame)
                self._latest_ir_frame = None
            return events


class BoardModeWorker(threading.Thread):
    """Send one mode command without blocking the Tk event loop."""

    def __init__(
        self,
        *,
        host: str,
        port: int,
        mode: str,
        events: ReceiverMailbox,
        stop_event: threading.Event,
        timeout: float,
    ) -> None:
        super().__init__(name=f"Board mode {mode}", daemon=True)
        self.host = host
        self.port = port
        self.mode = mode
        self.events = events
        self.stop_event = stop_event
        self.timeout = timeout

    def run(self) -> None:
        try:
            with socket.create_connection(
                (self.host, self.port),
                timeout=self.timeout,
            ) as sock:
                sock.settimeout(self.timeout)
                linger = (
                    struct.pack("HH", 1, 0)
                    if sys.platform == "win32"
                    else struct.pack("ii", 1, 0)
                )
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, linger)
                sock.sendall(f"MODE {self.mode}\r\n".encode("ascii"))
                response = bytearray()
                while b"\n" not in response and not self.stop_event.is_set():
                    chunk = sock.recv(512)
                    if not chunk:
                        break
                    response.extend(chunk)

            text = response.decode("ascii", errors="replace").strip()
            if not text.startswith(f"OK MODE={self.mode}"):
                raise RuntimeError(text or "board returned an empty response")
            self.events.put(StreamEvent("mode_result", (self.mode, text)))
        except (OSError, RuntimeError) as exc:
            self.events.put(StreamEvent("mode_error", (self.mode, str(exc))))


class ReceiverWorker(threading.Thread):
    def __init__(
        self,
        *,
        host: str,
        port: int,
        events: ReceiverMailbox,
        stop_event: threading.Event,
        reconnect: bool,
        reconnect_delay: float,
        connect_timeout: float,
        socket_timeout: float,
        record_path: Optional[str],
        session_dir: Optional[str] = None,
    ) -> None:
        super().__init__(name="MMS2 receiver", daemon=True)
        self.host = host
        self.port = port
        self.events = events
        self.stop_event = stop_event
        self.reconnect = reconnect
        self.reconnect_delay = reconnect_delay
        self.connect_timeout = connect_timeout
        self.socket_timeout = socket_timeout
        self.record_path = record_path
        self.session_dir = session_dir
        self.received_count = 0
        self.sequence_gaps = 0
        self.last_session: Optional[int] = None
        self.last_sequence: Optional[int] = None
        self.last_source_sequence: Optional[int] = None
        self.source_sequence_gaps = 0
        self.interval_started = time.monotonic()
        self.interval_messages = 0
        self.interval_wire_bytes = 0
        self.rate_fps = 0.0
        self.wire_mbps = 0.0

    def _emit(self, kind: str, value: object) -> None:
        self.events.put(StreamEvent(kind, value))

    def _wait_before_reconnect(self) -> bool:
        return self.stop_event.wait(self.reconnect_delay)

    def _receive_connection(self, stream: UDPRecvBuffer) -> None:
        record_handle = None
        session_recorder = SessionRecorder(self.session_dir) if self.session_dir else None
        if self.record_path:
            record_file = pathlib.Path(self.record_path)
            record_file.parent.mkdir(parents=True, exist_ok=True)
            record_handle = record_file.open("ab")

        try:
            stream.settimeout(self.socket_timeout)
            self._emit("connection", "connected")

            while not self.stop_event.is_set():
                raw_header = stream.recv_exact(HEADER_BYTES)
                try:
                    header = decode_header(raw_header)
                except ValueError as exc:
                    if "bad magic" in str(exc):
                        stream.resync()
                        continue
                    raise
                payload = stream.recv_exact(header.payload_bytes)
                decoded_block = None

                if record_handle is not None:
                    record_handle.write(raw_header)
                    record_handle.write(payload)
                    record_handle.flush()

                if header.flags & FLAG_PAYLOAD_CRC_VALID:
                    actual_crc = crc32(payload)
                    if actual_crc != header.payload_crc32:
                        self._emit(
                            "crc_error",
                            f"payload CRC mismatch seq={header.sequence}",
                        )
                        continue

                if header.stream_id != STREAM_AD_AI_BUNDLE:
                    self._emit(
                        "protocol_error",
                        f"unsupported stream id={header.stream_id} "
                        f"format=0x{header.payload_format:04X}",
                    )
                    continue

                if header.payload_format == FORMAT_AD_AI_BLOCK_V2:
                    bundle = decode_ad_ai_block(payload)
                    decoded_block = bundle
                    source_sequence = bundle.source_frame_sequence
                elif header.payload_format == FORMAT_AD_AI_BUNDLE_V1:
                    bundle = decode_ad_ai_bundle(payload)
                    source_sequence = bundle.input_frame_sequence
                else:
                    self._emit(
                        "protocol_error",
                        f"unsupported AD+AI format=0x{header.payload_format:04X}",
                    )
                    continue
                if header.sequence != bundle.run_count:
                    self._emit(
                        "protocol_error",
                        f"bundle sequence {header.sequence} != run {bundle.run_count}",
                    )
                    continue
                if header.aux0 != source_sequence:
                    self._emit(
                        "protocol_error",
                        f"bundle AD sequence {header.aux0} "
                        f"!= metadata {source_sequence}",
                    )
                    continue

                if header.boot_session_id != self.last_session:
                    self.last_session = header.boot_session_id
                    self.last_sequence = None
                    self.last_source_sequence = None
                if (
                    self.last_sequence is not None
                    and bundle.run_count > self.last_sequence + 1
                ):
                    self.sequence_gaps += bundle.run_count - self.last_sequence - 1
                self.last_sequence = bundle.run_count
                if isinstance(bundle, ADAIBlock):
                    if (
                        self.last_source_sequence is not None
                        and bundle.source_frame_sequence > self.last_source_sequence + 1
                    ):
                        self.source_sequence_gaps += (
                            bundle.source_frame_sequence
                            - self.last_source_sequence
                            - 1
                        )
                    self.last_source_sequence = bundle.source_frame_sequence
                self.received_count += 1
                self.interval_messages += 1
                self.interval_wire_bytes += HEADER_BYTES + header.payload_bytes

                now = time.monotonic()
                interval = now - self.interval_started
                if interval >= 1.0:
                    self.rate_fps = self.interval_messages / interval
                    self.wire_mbps = (
                        self.interval_wire_bytes * 8.0 / interval / 1_000_000.0
                    )
                    self.interval_started = now
                    self.interval_messages = 0
                    self.interval_wire_bytes = 0

                self._emit(
                    "bundle",
                    BundleSnapshot(
                        bundle=bundle,
                        received_count=self.received_count,
                        sequence_gaps=self.sequence_gaps,
                        source_sequence_gaps=self.source_sequence_gaps,
                        rate_fps=self.rate_fps,
                        wire_mbps=self.wire_mbps,
                    ),
                )
                if session_recorder is not None:
                    session_recorder.record_message(
                        raw_header,
                        payload,
                        header,
                        decoded_block,
                    )
        finally:
            if record_handle is not None:
                record_handle.close()
            if session_recorder is not None:
                session_recorder.close()

    def run(self) -> None:
        self._emit("connection", f"binding UDP port {self.port}")
        try:
            sock = open_udp_recv_socket(self.port)
            stream = UDPRecvBuffer(sock)
            self._emit("connection", "connected")
            while not self.stop_event.is_set():
                try:
                    self._receive_connection(stream)
                except (OSError, ValueError, struct.error) as exc:
                    self._emit("connection_error", str(exc))
                    time.sleep(self.reconnect_delay)
        except OSError as exc:
            self._emit("connection_error", str(exc))
        finally:
            self._emit("connection", "disconnected")


class IRStreamWorker(threading.Thread):
    def __init__(
        self,
        *,
        host: str,
        port: int,
        events: ReceiverMailbox,
        stop_event: threading.Event,
        reconnect_delay: float,
        connect_timeout: float,
        socket_timeout: float,
    ) -> None:
        super().__init__(name="Tiny1C MMS2 receiver", daemon=True)
        self.host = host
        self.port = port
        self.events = events
        self.stop_event = stop_event
        self.reconnect_delay = reconnect_delay
        self.connect_timeout = connect_timeout
        self.socket_timeout = socket_timeout
        self.last_session: Optional[int] = None
        self.last_sequence: Optional[int] = None
        self.interval_started = time.monotonic()
        self.interval_messages = 0
        self.interval_wire_bytes = 0
        self.rate_fps = 0.0

    def _emit(self, kind: str, value: object) -> None:
        self.events.put(StreamEvent(kind, value))

    def _decode_temperature(self, payload: bytes, sequence: int) -> IRFrameSnapshot:
        if np is not None:
            values = np.frombuffer(payload, dtype="<u2")
            if values.size != IR_WIDTH * IR_HEIGHT:
                raise ValueError(
                    f"Tiny1C temperature frame has {values.size} pixels, "
                    f"expected {IR_WIDTH * IR_HEIGHT}"
                )

            values = values.reshape((IR_HEIGHT, IR_WIDTH))
            valid_mask = values != 0
            valid = values[valid_mask]
            if valid.size == 0:
                raise ValueError("Tiny1C temperature frame has no valid pixels")

            low_raw, high_raw = np.percentile(valid, (2.0, 98.0))
            if high_raw <= low_raw:
                levels = np.full(values.shape, 128, dtype=np.uint8)
            else:
                levels = np.clip(
                    np.rint(
                        (values.astype(np.float32) - low_raw)
                        * (255.0 / (high_raw - low_raw))
                    ),
                    0,
                    255,
                ).astype(np.uint8)
            levels[~valid_mask] = 0

            source_y = (
                np.arange(IR_DISPLAY_HEIGHT, dtype=np.int32)
                * IR_HEIGHT
                // IR_DISPLAY_HEIGHT
            )
            source_x = (
                np.arange(IR_DISPLAY_WIDTH, dtype=np.int32)
                * IR_WIDTH
                // IR_DISPLAY_WIDTH
            )
            display_levels = levels[source_y[:, None], source_x]
            palette = np.frombuffer(IR_PALETTE_RGB, dtype=np.uint8).reshape((256, 3))
            pixels = palette[display_levels].tobytes()

            return IRFrameSnapshot(
                pixels=pixels,
                width=IR_DISPLAY_WIDTH,
                height=IR_DISPLAY_HEIGHT,
                valid_pixels=int(valid.size),
                low_c=float(low_raw) / 64.0 - 273.15,
                high_c=float(high_raw) / 64.0 - 273.15,
                captured_at=time.monotonic(),
                sequence=sequence,
                rate_fps=self.rate_fps,
            )

        values, _ = temp_u16_from_raw(payload, IR_WIDTH, IR_HEIGHT, "le")
        valid = [value for value in values if value != 0]
        if not valid:
            raise ValueError("Tiny1C temperature frame has no valid pixels")

        low_raw = percentile(valid, 2.0)
        high_raw = percentile(valid, 98.0)
        low_c = low_raw / 64.0 - 273.15
        high_c = high_raw / 64.0 - 273.15
        levels = [
            normalize(value, low_raw, high_raw) if value != 0 else 0
            for value in values
        ]

        pixels = bytearray()
        for display_y in range(IR_DISPLAY_HEIGHT):
            source_y = min(IR_HEIGHT - 1, display_y * IR_HEIGHT // IR_DISPLAY_HEIGHT)
            for display_x in range(IR_DISPLAY_WIDTH):
                source_x = min(IR_WIDTH - 1, display_x * IR_WIDTH // IR_DISPLAY_WIDTH)
                level = levels[source_y * IR_WIDTH + source_x]
                pixels.extend(temp_palette(level))

        return IRFrameSnapshot(
            pixels=bytes(pixels),
            width=IR_DISPLAY_WIDTH,
            height=IR_DISPLAY_HEIGHT,
            valid_pixels=len(valid),
            low_c=low_c,
            high_c=high_c,
            captured_at=time.monotonic(),
            sequence=sequence,
            rate_fps=self.rate_fps,
        )

    def _receive_connection(self, stream: UDPRecvBuffer) -> None:
        stream.settimeout(self.socket_timeout)
        self._emit("ir_connection", "connected")

        while not self.stop_event.is_set():
            raw_header = stream.recv_exact(HEADER_BYTES)
            try:
                header = decode_header(raw_header)
            except ValueError as exc:
                if "bad magic" in str(exc):
                    stream.resync()
                    continue
                raise
            payload = stream.recv_exact(header.payload_bytes)

            if header.flags & FLAG_PAYLOAD_CRC_VALID:
                actual_crc = crc32(payload)
                if actual_crc != header.payload_crc32:
                    raise ValueError(
                        f"IR payload CRC mismatch seq={header.sequence}"
                    )
            if (
                header.stream_id != STREAM_TINY1C_TEMP16
                or header.payload_format != FORMAT_TINY1C_TEMP16_V1
            ):
                raise ValueError(
                    f"unsupported IR stream id={header.stream_id} "
                    f"format=0x{header.payload_format:04X}"
                )
            if header.payload_bytes != IR_WIDTH * IR_HEIGHT * 2:
                raise ValueError(
                    f"IR payload length is {header.payload_bytes}, "
                    f"expected {IR_WIDTH * IR_HEIGHT * 2}"
                )

            if header.boot_session_id != self.last_session:
                self.last_session = header.boot_session_id
                self.last_sequence = None
            if (
                self.last_sequence is not None
                and header.sequence > self.last_sequence + 1
            ):
                self._emit(
                    "ir_gap",
                    header.sequence - self.last_sequence - 1,
                )
            self.last_sequence = header.sequence
            self.interval_messages += 1
            self.interval_wire_bytes += HEADER_BYTES + header.payload_bytes
            now = time.monotonic()
            interval = now - self.interval_started
            if interval >= 1.0:
                self.rate_fps = self.interval_messages / interval
                self.interval_started = now
                self.interval_messages = 0
                self.interval_wire_bytes = 0

            self._emit(
                "ir_frame",
                self._decode_temperature(payload, header.sequence),
            )

    def run(self) -> None:
        self._emit("ir_connection", f"binding UDP port {self.port}")
        try:
            sock = open_udp_recv_socket(self.port)
            stream = UDPRecvBuffer(sock)
            self._emit("ir_connection", "connected")
            while not self.stop_event.is_set():
                try:
                    self._receive_connection(stream)
                except (OSError, ValueError, struct.error) as exc:
                    self._emit("ir_error", str(exc))
                    time.sleep(self.reconnect_delay)
        except OSError as exc:
            self._emit("ir_error", str(exc))
        finally:
            self._emit("ir_connection", "disconnected")


def decode_raw_window(raw_window: bytes, points: int, channels: int) -> list[list[int]]:
    expected = points * channels * 2
    if len(raw_window) != expected:
        raise ValueError(
            f"raw window length is {len(raw_window)}, expected {expected}"
        )

    samples = array("h")
    samples.frombytes(raw_window)
    if sys.byteorder != "little":
        samples.byteswap()

    return [list(samples[channel::channels]) for channel in range(channels)]


def decimate(values: list[int], width: int) -> list[float]:
    if not values:
        return []
    if len(values) <= width:
        return [float(value) for value in values]

    result: list[float] = []
    step = len(values) / float(width)
    for index in range(width):
        start = int(index * step)
        end = max(start + 1, int((index + 1) * step))
        segment = values[start:end]
        result.append(sum(segment) / len(segment))
    return result


def make_ir_photo(
    pixels: bytes,
    width: int,
    height: int,
    display_width: Optional[int] = None,
    display_height: Optional[int] = None,
) -> object:
    if len(pixels) != width * height * 3:
        raise ValueError(
            f"IR RGB length is {len(pixels)}, expected {width * height * 3}"
        )

    # Pillow hands the complete RGB buffer to Tk in one operation. The
    # PhotoImage.put fallback is intentionally kept for environments without
    # Pillow, but is much slower for a live 320x240 stream.
    if Image is not None and ImageTk is not None:
        image = Image.frombytes("RGB", (width, height), pixels)
        target_size = (
            display_width if display_width is not None else width,
            display_height if display_height is not None else height,
        )
        if image.size != target_size:
            resampling = getattr(Image, "Resampling", Image)
            image = image.resize(target_size, resampling.NEAREST)
        return ImageTk.PhotoImage(image)

    photo = PhotoImage(width=width, height=height)
    for y in range(height):
        row = []
        row_offset = y * width * 3
        for x in range(width):
            offset = row_offset + x * 3
            row.append(
                f"#{pixels[offset]:02x}{pixels[offset + 1]:02x}{pixels[offset + 2]:02x}"
            )
        photo.put("{" + " ".join(row) + "}", to=(0, y))
    return photo


class PlotPanel:
    def __init__(self, parent: ttk.Frame, channel: int) -> None:
        self.channel = channel
        self.canvas = Canvas(
            parent,
            background=PLOT_BACKGROUND,
            highlightthickness=1,
            highlightbackground=PLOT_GRID,
            height=150,
        )
        self.canvas.grid(row=channel // 2, column=channel % 2, sticky="nsew", padx=4, pady=4)
        parent.rowconfigure(channel // 2, weight=1)
        parent.columnconfigure(channel % 2, weight=1)
        self._last_values: list[int] = []

    def set_values(self, values: list[int]) -> None:
        self._last_values = values
        self.redraw()

    def redraw(self) -> None:
        width = max(100, self.canvas.winfo_width())
        height = max(80, self.canvas.winfo_height())
        if (width, height) == (1, 1):
            return

        self.canvas.delete("all")
        mid_y = height / 2.0
        self.canvas.create_line(
            0,
            mid_y,
            width,
            mid_y,
            fill=PLOT_GRID,
            dash=(2, 4),
        )
        self.canvas.create_text(
            8,
            8,
            anchor="nw",
            fill=PLOT_TEXT,
            text=f"CH{self.channel + 1}",
            font=("Segoe UI", 9, "bold"),
        )

        if not self._last_values:
            self.canvas.create_text(
                width / 2,
                height / 2,
                fill=PLOT_TEXT,
                text="no data",
            )
            return

        values = decimate(self._last_values, max(100, width - 12))
        low = min(values)
        high = max(values)
        if math.isclose(low, high):
            low -= 1.0
            high += 1.0
        margin = (high - low) * 0.08
        low -= margin
        high += margin

        points: list[float] = []
        for index, value in enumerate(values):
            x = 6.0 + (width - 12.0) * index / max(1, len(values) - 1)
            y = 8.0 + (height - 16.0) * (high - value) / (high - low)
            points.extend((x, y))

        self.canvas.create_line(
            *points,
            fill=PLOT_COLORS[self.channel % len(PLOT_COLORS)],
            width=1,
            smooth=False,
        )
        self.canvas.create_text(
            width - 8,
            8,
            anchor="ne",
            fill=PLOT_TEXT,
            text=f"{int(low + margin)}..{int(high - margin)}",
            font=("Consolas", 8),
        )


class ViewerApp:
    def __init__(self, root: Tk, args: argparse.Namespace) -> None:
        self.root = root
        self.args = args
        self.events = ReceiverMailbox()
        self.stop_event = threading.Event()
        self.worker = ReceiverWorker(
            host=args.host,
            port=args.port,
            events=self.events,
            stop_event=self.stop_event,
            reconnect=True,
            reconnect_delay=args.reconnect_delay,
            connect_timeout=args.connect_timeout,
            socket_timeout=args.socket_timeout,
            record_path=args.record,
            session_dir=args.session_dir,
        )
        self.ir_worker = IRStreamWorker(
            host=args.host,
            port=args.ir_port,
            events=self.events,
            stop_event=self.stop_event,
            reconnect_delay=args.reconnect_delay,
            connect_timeout=args.connect_timeout,
            socket_timeout=args.socket_timeout,
        )

        self.root.title(APP_TITLE)
        self.root.configure(background=WINDOW_BACKGROUND)
        self.root.geometry(args.geometry)
        self.root.minsize(1180, 720)
        self.root.protocol("WM_DELETE_WINDOW", self.close)

        self.mode_status_var = StringVar(value="BOARD MODE --")
        self.status_var = StringVar(value=f"BINDING UDP {args.port}")
        self.rate_var = StringVar(value="--")
        self.bundle_var = StringVar(value="--")
        self.ad_seq_var = StringVar(value="--")
        self.block_var = StringVar(value="--")
        self.ai_var = StringVar(value="--")
        self.output_var = StringVar(value="--")
        self.infer_var = StringVar(value="--")
        self.integrity_var = StringVar(value="gaps 0   CRC 0   protocol 0")
        self.last_error_var = StringVar(value="")

        self.ir_status_var = StringVar(value=f"BINDING UDP {args.ir_port}")
        self.ir_range_var = StringVar(value="--")
        self.ir_rate_var = StringVar(value="--")
        self.ir_sequence_var = StringVar(value="--")
        self.ir_valid_var = StringVar(value="--")
        self.ir_integrity_var = StringVar(value="gaps 0   errors 0")
        self.ir_error_var = StringVar(value="")

        self._ir_image: Optional[object] = None
        self._sequence_gaps = 0
        self._source_sequence_gaps = 0
        self._crc_errors = 0
        self._protocol_errors = 0
        self._connection_errors = 0
        self._ir_sequence_gaps = 0
        self._ir_errors = 0
        self._active_page = "ADAI"
        self._stable_mode = "ADAI"
        self._pending_mode: Optional[str] = None
        self._pending_previous_mode = "ADAI"

        self._build_style()
        self._build_layout()
        self.worker.start()
        if not args.disable_ir:
            self.ir_worker.start()
        else:
            self.ir_status_var.set("DISABLED")
        self.root.after(args.refresh_ms, self._poll_events)
        self.root.after(100, lambda: self._select_page("ADAI"))

    def _build_style(self) -> None:
        style = ttk.Style(self.root)
        try:
            style.theme_use("clam")
        except TclError:
            pass
        style.configure("Root.TFrame", background=WINDOW_BACKGROUND)
        style.configure("Panel.TFrame", background=PANEL_BACKGROUND)
        style.configure(
            "Title.TLabel",
            background=WINDOW_BACKGROUND,
            foreground="#e6f1ef",
            font=("Segoe UI", 16, "bold"),
        )
        style.configure(
            "HeaderStatus.TLabel",
            background=WINDOW_BACKGROUND,
            foreground=PLOT_TEXT,
            font=("Consolas", 10),
        )
        style.configure(
            "Muted.TLabel",
            background=PANEL_BACKGROUND,
            foreground=PLOT_TEXT,
            font=("Segoe UI", 9),
        )
        style.configure(
            "Value.TLabel",
            background=PANEL_BACKGROUND,
            foreground="#e6f1ef",
            font=("Consolas", 11),
        )
        style.configure(
            "Section.TLabel",
            background=PANEL_BACKGROUND,
            foreground=ACCENT,
            font=("Segoe UI", 11, "bold"),
        )
        style.configure(
            "Mode.TButton",
            background=PANEL_BACKGROUND,
            foreground="#d7e5e3",
            borderwidth=1,
            padding=(16, 7),
            font=("Segoe UI", 10),
        )
        style.map(
            "Mode.TButton",
            background=[("active", "#243238")],
            foreground=[("disabled", "#667579")],
        )
        style.configure(
            "ModeActive.TButton",
            background=ACCENT,
            foreground=WINDOW_BACKGROUND,
            borderwidth=1,
            padding=(16, 7),
            font=("Segoe UI", 10, "bold"),
        )
        style.map(
            "ModeActive.TButton",
            background=[("active", "#7ce6df"), ("disabled", "#3a8e89")],
            foreground=[("disabled", "#172124")],
        )

    def _build_layout(self) -> None:
        root_frame = ttk.Frame(self.root, style="Root.TFrame", padding=12)
        root_frame.pack(fill="both", expand=True)

        header = ttk.Frame(root_frame, style="Root.TFrame")
        header.pack(fill="x", pady=(0, 10))
        ttk.Label(
            header,
            text="STM32N6 Multimodal Viewer",
            style="Title.TLabel",
        ).pack(side="left")

        controls = ttk.Frame(header, style="Root.TFrame")
        controls.pack(side="right")
        ttk.Label(
            controls,
            textvariable=self.mode_status_var,
            style="HeaderStatus.TLabel",
        ).pack(side="left", padx=(0, 14))
        self.adai_button = ttk.Button(
            controls,
            text="AD + AI",
            style="ModeActive.TButton",
            command=lambda: self._select_page("ADAI"),
        )
        self.adai_button.pack(side="left", padx=(0, 6))
        self.ir_button = ttk.Button(
            controls,
            text="Infrared",
            style="Mode.TButton",
            command=lambda: self._select_page("IR"),
        )
        self.ir_button.pack(side="left")
        if self.args.disable_ir:
            self.ir_button.configure(state="disabled")

        self.page_container = ttk.Frame(root_frame, style="Root.TFrame")
        self.page_container.pack(fill="both", expand=True)
        self.page_container.rowconfigure(0, weight=1)
        self.page_container.columnconfigure(0, weight=1)

        self.adai_page = ttk.Frame(self.page_container, style="Root.TFrame")
        self.ir_page = ttk.Frame(self.page_container, style="Root.TFrame")
        self.adai_page.grid(row=0, column=0, sticky="nsew")
        self.ir_page.grid(row=0, column=0, sticky="nsew")
        self._build_adai_page()
        self._build_ir_page()
        self._show_page("ADAI")

    def _build_adai_page(self) -> None:
        self.adai_page.columnconfigure(0, weight=1)
        self.adai_page.rowconfigure(0, weight=1)

        body = ttk.Frame(self.adai_page, style="Root.TFrame")
        body.grid(row=0, column=0, sticky="nsew")
        body.columnconfigure(0, weight=4)
        body.columnconfigure(1, weight=1)
        body.rowconfigure(0, weight=1)

        plots_frame = ttk.Frame(body, style="Panel.TFrame", padding=8)
        plots_frame.grid(row=0, column=0, sticky="nsew", padx=(0, 10))
        plots_frame.columnconfigure(0, weight=1)
        plots_frame.columnconfigure(1, weight=1)
        for row in range(4):
            plots_frame.rowconfigure(row, weight=1)
        self.plots = [PlotPanel(plots_frame, channel) for channel in range(8)]

        side = ttk.Frame(body, style="Panel.TFrame", padding=14)
        side.grid(row=0, column=1, sticky="nsew")
        side.columnconfigure(0, weight=1)

        ttk.Label(side, text="AD + AI STREAM", style="Section.TLabel").grid(
            row=0, column=0, sticky="w", pady=(0, 5)
        )
        ttk.Label(
            side,
            textvariable=self.status_var,
            style="Muted.TLabel",
        ).grid(row=1, column=0, sticky="w", pady=(0, 12))
        self._add_metric(side, 2, "Bundle rate", self.rate_var)
        self._add_metric(side, 4, "Bundles", self.bundle_var)
        self._add_metric(side, 6, "AD sequence", self.ad_seq_var)
        self._add_metric(side, 8, "Sample block", self.block_var)
        self._add_metric(side, 10, "AI run", self.ai_var)
        self._add_metric(side, 12, "Inference", self.infer_var)

        ttk.Separator(side).grid(row=14, column=0, sticky="ew", pady=14)
        ttk.Label(side, text="AI OUTPUT", style="Section.TLabel").grid(
            row=15, column=0, sticky="w", pady=(0, 8)
        )
        ttk.Label(
            side,
            textvariable=self.output_var,
            style="Value.TLabel",
            justify="left",
        ).grid(row=16, column=0, sticky="w")

        ttk.Separator(side).grid(row=17, column=0, sticky="ew", pady=14)
        ttk.Label(side, text="INTEGRITY", style="Section.TLabel").grid(
            row=18, column=0, sticky="w", pady=(0, 8)
        )
        ttk.Label(
            side,
            textvariable=self.integrity_var,
            style="Value.TLabel",
            justify="left",
        ).grid(row=19, column=0, sticky="w")
        ttk.Label(
            side,
            textvariable=self.last_error_var,
            style="Muted.TLabel",
            justify="left",
            wraplength=280,
        ).grid(row=20, column=0, sticky="w", pady=(12, 0))

        ttk.Label(
            self.adai_page,
            text=(
                f"AD7606  8ch x 512 points x int16    |    "
                f"{512.0 / self.args.sample_rate * 1000.0:.1f} ms source block    |    "
                "AI window 8ch x 1024 points"
            ),
            style="Muted.TLabel",
        ).grid(row=1, column=0, sticky="ew", pady=(8, 0))

    def _build_ir_page(self) -> None:
        self.ir_page.columnconfigure(0, weight=4)
        self.ir_page.columnconfigure(1, weight=1)
        self.ir_page.rowconfigure(0, weight=1)

        view = ttk.Frame(self.ir_page, style="Panel.TFrame", padding=14)
        view.grid(row=0, column=0, sticky="nsew", padx=(0, 10))
        view.rowconfigure(1, weight=1)
        view.columnconfigure(0, weight=1)
        ttk.Label(
            view,
            text="TINY1C TEMPERATURE",
            style="Section.TLabel",
        ).grid(row=0, column=0, sticky="w", pady=(0, 10))

        canvas_holder = ttk.Frame(view, style="Panel.TFrame")
        canvas_holder.grid(row=1, column=0)
        self.ir_canvas = Canvas(
            canvas_holder,
            width=IR_DISPLAY_WIDTH * 2,
            height=IR_DISPLAY_HEIGHT * 2,
            background=PLOT_BACKGROUND,
            highlightthickness=1,
            highlightbackground=PLOT_GRID,
        )
        self.ir_canvas.pack()
        self.ir_canvas.create_text(
            IR_DISPLAY_WIDTH,
            IR_DISPLAY_HEIGHT,
            fill=PLOT_TEXT,
            text="waiting for infrared stream",
            font=("Segoe UI", 11),
        )

        side = ttk.Frame(self.ir_page, style="Panel.TFrame", padding=14)
        side.grid(row=0, column=1, sticky="nsew")
        side.columnconfigure(0, weight=1)
        ttk.Label(side, text="INFRARED STREAM", style="Section.TLabel").grid(
            row=0, column=0, sticky="w", pady=(0, 5)
        )
        ttk.Label(
            side,
            textvariable=self.ir_status_var,
            style="Muted.TLabel",
        ).grid(row=1, column=0, sticky="w", pady=(0, 16))
        self._add_metric(side, 2, "Temperature range", self.ir_range_var)
        self._add_metric(side, 4, "Frame rate", self.ir_rate_var)
        self._add_metric(side, 6, "Sequence", self.ir_sequence_var)
        self._add_metric(side, 8, "Valid pixels", self.ir_valid_var)

        ttk.Separator(side).grid(row=10, column=0, sticky="ew", pady=14)
        ttk.Label(side, text="INTEGRITY", style="Section.TLabel").grid(
            row=11, column=0, sticky="w", pady=(0, 8)
        )
        ttk.Label(
            side,
            textvariable=self.ir_integrity_var,
            style="Value.TLabel",
        ).grid(row=12, column=0, sticky="w")
        ttk.Label(
            side,
            textvariable=self.ir_error_var,
            style="Muted.TLabel",
            justify="left",
            wraplength=280,
        ).grid(row=13, column=0, sticky="w", pady=(12, 0))

    def _show_page(self, mode: str) -> None:
        self._active_page = mode
        if mode == "IR":
            self.ir_page.tkraise()
            self.adai_button.configure(style="Mode.TButton")
            self.ir_button.configure(style="ModeActive.TButton")
        else:
            self.adai_page.tkraise()
            self.adai_button.configure(style="ModeActive.TButton")
            self.ir_button.configure(style="Mode.TButton")

    def _select_page(self, mode: str) -> None:
        if self._pending_mode is not None:
            return
        if mode == "IR" and self.args.disable_ir:
            return

        self._pending_previous_mode = self._stable_mode
        self._pending_mode = mode
        self._show_page(mode)
        self.mode_status_var.set(f"SWITCHING TO {mode}")
        self.adai_button.configure(state="disabled")
        self.ir_button.configure(state="disabled")
        BoardModeWorker(
            host=self.args.host,
            port=self.args.command_port,
            mode=mode,
            events=self.events,
            stop_event=self.stop_event,
            timeout=self.args.connect_timeout,
        ).start()

    def _finish_mode_switch(self) -> None:
        self.adai_button.configure(state="normal")
        self.ir_button.configure(
            state="disabled" if self.args.disable_ir else "normal"
        )

    @staticmethod
    def _add_metric(
        parent: ttk.Frame,
        row: int,
        label: str,
        variable: StringVar,
    ) -> None:
        ttk.Label(parent, text=label, style="Muted.TLabel").grid(
            row=row,
            column=0,
            sticky="w",
            pady=(0, 2),
        )
        ttk.Label(parent, textvariable=variable, style="Value.TLabel").grid(
            row=row + 1,
            column=0,
            sticky="w",
            pady=(0, 9),
        )

    def _poll_events(self) -> None:
        for event in self.events.drain():
            self._handle_event(event)
        if not self.stop_event.is_set():
            self.root.after(self.args.refresh_ms, self._poll_events)

    def _handle_event(self, event: StreamEvent) -> None:
        if event.kind == "mode_result":
            mode, _ = event.value
            if mode == self._pending_mode:
                self._stable_mode = mode
                self._pending_mode = None
                self.mode_status_var.set(f"BOARD MODE {mode}")
                self._finish_mode_switch()
            return
        if event.kind == "mode_error":
            mode, message = event.value
            if mode == self._pending_mode:
                self._pending_mode = None
                self._show_page(self._pending_previous_mode)
                self.mode_status_var.set("MODE SWITCH FAILED")
                if mode == "IR":
                    self.ir_error_var.set(f"mode switch: {message}")
                else:
                    self.last_error_var.set(f"mode switch: {message}")
                self._finish_mode_switch()
            return
        if event.kind == "connection":
            self.status_var.set(str(event.value).upper())
            return
        if event.kind == "ir_connection":
            self.ir_status_var.set(str(event.value).upper())
            return
        if event.kind == "connection_error":
            self._connection_errors += 1
            self.status_var.set("WAITING FOR BOARD")
            self.last_error_var.set(f"connection: {event.value}")
            self._update_integrity()
            return
        if event.kind == "protocol_error":
            self._protocol_errors += 1
            self.last_error_var.set(f"protocol: {event.value}")
            self._update_integrity()
            return
        if event.kind == "crc_error":
            self._crc_errors += 1
            self.last_error_var.set(f"CRC: {event.value}")
            self._update_integrity()
            return
        if event.kind == "ir_error":
            self._ir_errors += 1
            self.ir_status_var.set("WAITING FOR BOARD")
            self.ir_error_var.set(f"Tiny1C: {event.value}")
            self._update_ir_integrity()
            return
        if event.kind == "ir_gap":
            self._ir_sequence_gaps += int(event.value)
            self.ir_error_var.set(f"sequence gap: {event.value}")
            self._update_ir_integrity()
            return
        if event.kind == "ir_frame":
            snapshot = event.value
            if not isinstance(snapshot, IRFrameSnapshot):
                return
            self._ir_image = make_ir_photo(
                snapshot.pixels,
                snapshot.width,
                snapshot.height,
                IR_DISPLAY_WIDTH * 2,
                IR_DISPLAY_HEIGHT * 2,
            )
            self.ir_canvas.delete("all")
            self.ir_canvas.create_image(0, 0, anchor="nw", image=self._ir_image)
            self.ir_range_var.set(f"{snapshot.low_c:.1f} .. {snapshot.high_c:.1f} C")
            self.ir_rate_var.set(f"{snapshot.rate_fps:5.2f} fps")
            self.ir_sequence_var.set(str(snapshot.sequence))
            self.ir_valid_var.set(
                f"{snapshot.valid_pixels} / {IR_WIDTH * IR_HEIGHT}"
            )
            self.ir_status_var.set(
                f"STREAMING  {self.args.host}:{self.args.ir_port}"
            )
            return
        if event.kind != "bundle":
            return

        snapshot = event.value
        if not isinstance(snapshot, BundleSnapshot):
            return
        bundle = snapshot.bundle
        self._sequence_gaps = snapshot.sequence_gaps
        self._source_sequence_gaps = snapshot.source_sequence_gaps

        if isinstance(bundle, ADAIBlock):
            raw_payload = bundle.raw_frame
            points = bundle.source_points
            channels_count = bundle.source_channels
            ad_sequence = bundle.source_frame_sequence
            block_start = bundle.source_block_start
            block_end = bundle.source_block_end
            window_text = (
                f"AI window {bundle.window_block_start} .. "
                f"{bundle.window_block_end}"
            )
        else:
            raw_payload = bundle.raw_window
            points = bundle.points
            channels_count = bundle.channels
            ad_sequence = bundle.input_frame_sequence
            block_start = bundle.block_start
            block_end = bundle.block_end
            window_text = "AI window equals displayed payload"

        channels = decode_raw_window(raw_payload, points, channels_count)
        for index, values in enumerate(channels):
            if index < len(self.plots):
                self.plots[index].set_values(values)

        if snapshot.rate_fps > 0.0:
            self.rate_var.set(
                f"{snapshot.rate_fps:5.2f} fps   {snapshot.wire_mbps:5.2f} Mbps"
            )
        self.bundle_var.set(str(snapshot.received_count))
        self.ad_seq_var.set(str(ad_sequence))
        self.block_var.set(f"{block_start} .. {block_end}")
        self.ai_var.set(str(bundle.run_count))
        self.infer_var.set(
            f"{bundle.inference_ms} ms   avg {bundle.average_inference_ms} ms   "
            f"max {bundle.max_inference_ms} ms"
        )
        self.output_var.set(
            f"top {bundle.top_index}\n"
            f"out  {bundle.outputs[0]:4d}  {bundle.outputs[1]:4d}  "
            f"{bundle.outputs[2]:4d}  {bundle.outputs[3]:4d}\n"
            f"run errors  {bundle.run_error_count}"
        )
        self.last_error_var.set(window_text)
        self.status_var.set(f"STREAMING  {self.args.host}:{self.args.port}")
        self._update_integrity()

    def _update_integrity(self) -> None:
        self.integrity_var.set(
            f"gaps {self._sequence_gaps}   "
            f"source {self._source_sequence_gaps}   "
            f"CRC {self._crc_errors}   "
            f"protocol {self._protocol_errors}   "
            f"connect {self._connection_errors}"
        )

    def _update_ir_integrity(self) -> None:
        self.ir_integrity_var.set(
            f"gaps {self._ir_sequence_gaps}   errors {self._ir_errors}"
        )

    def close(self) -> None:
        self.stop_event.set()
        self.root.destroy()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="STM32N6 multimodal live viewer")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--ir-port", type=int, default=DEFAULT_IR_PORT)
    parser.add_argument(
        "--command-port",
        type=int,
        default=DEFAULT_COMMAND_PORT,
        help="board TCP command port used by the page mode buttons",
    )
    parser.add_argument(
        "--enable-ir",
        action="store_true",
        help="enable the independent Tiny1C MMS2 stream (enabled by default)",
    )
    parser.add_argument(
        "--disable-ir",
        action="store_true",
        help="disable the independent Tiny1C MMS2 stream",
    )
    parser.add_argument("--sample-rate", type=float, default=DEFAULT_SAMPLE_RATE)
    parser.add_argument("--refresh-ms", type=int, default=DEFAULT_REFRESH_MS)
    parser.add_argument("--reconnect-delay", type=float, default=1.0)
    parser.add_argument("--connect-timeout", type=float, default=5.0)
    parser.add_argument("--socket-timeout", type=float, default=10.0)
    parser.add_argument("--record", help="append raw MMS2 messages to a file")
    parser.add_argument(
        "--session-dir",
        help="write validated AD source blocks and AI metadata into this directory",
    )
    parser.add_argument("--geometry", default="1500x900")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = Tk()
    ViewerApp(root, args)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
