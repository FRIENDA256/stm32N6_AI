#!/usr/bin/env python3
import struct
import threading
import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

from multimodal_stream_viewer import (
    IR_HEIGHT,
    IR_DISPLAY_HEIGHT,
    IR_DISPLAY_WIDTH,
    IR_WIDTH,
    IRStreamWorker,
    ReceiverMailbox,
    StreamEvent,
    decode_raw_window,
    decimate,
)


class StreamViewerTests(unittest.TestCase):
    def test_decode_interleaved_little_endian_samples(self):
        raw = struct.pack(
            "<hhhhhh",
            1,
            10,
            2,
            20,
            3,
            30,
        )
        self.assertEqual(decode_raw_window(raw, points=3, channels=2), [[1, 2, 3], [10, 20, 30]])

    def test_decimate_preserves_constant_signal(self):
        self.assertEqual(decimate([7] * 100, 10), [7.0] * 10)

    def test_mailbox_keeps_controls_and_latest_bundle(self):
        mailbox = ReceiverMailbox()
        mailbox.put(StreamEvent("connection", "connected"))
        mailbox.put(StreamEvent("bundle", "old"))
        mailbox.put(StreamEvent("bundle", "new"))
        events = mailbox.drain()
        self.assertEqual([(event.kind, event.value) for event in events], [
            ("connection", "connected"),
            ("bundle", "new"),
        ])
        self.assertEqual(mailbox.drain(), [])

    def test_mailbox_keeps_latest_ir_frame(self):
        mailbox = ReceiverMailbox()
        mailbox.put(StreamEvent("ir_frame", "old"))
        mailbox.put(StreamEvent("ir_frame", "new"))
        self.assertEqual(
            [(event.kind, event.value) for event in mailbox.drain()],
            [("ir_frame", "new")],
        )

    def test_ir_temperature_payload_decode(self):
        worker = IRStreamWorker(
            host="127.0.0.1",
            port=5101,
            events=ReceiverMailbox(),
            stop_event=threading.Event(),
            reconnect_delay=0.1,
            connect_timeout=0.1,
            socket_timeout=0.1,
        )
        raw_value = round((30.0 + 273.15) * 64.0)
        payload = struct.pack("<H", raw_value) * (IR_WIDTH * IR_HEIGHT)
        snapshot = worker._decode_temperature(payload, sequence=42)
        self.assertEqual(snapshot.sequence, 42)
        self.assertEqual(snapshot.valid_pixels, IR_WIDTH * IR_HEIGHT)
        self.assertEqual(
            len(snapshot.pixels),
            IR_DISPLAY_WIDTH * IR_DISPLAY_HEIGHT * 3,
        )
        self.assertAlmostEqual(snapshot.low_c, 30.0, places=1)
        self.assertAlmostEqual(snapshot.high_c, 30.0, places=1)


if __name__ == "__main__":
    unittest.main()
