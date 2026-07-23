#!/usr/bin/env python3
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from multimodal_stream_receiver import (
    AD_AI_BUNDLE_META_STRUCT,
    AI_RESULT_STRUCT,
    FLAG_PAYLOAD_CRC_VALID,
    FORMAT_AI_INT8_V1,
    FORMAT_AD_AI_BUNDLE_V1,
    FORMAT_AD_AI_BLOCK_V2,
    FORMAT_TINY1C_TEMP16_V1,
    STREAM_AD_AI_BUNDLE,
    STREAM_AI_RESULT,
    STREAM_TINY1C_TEMP16,
    AD_AI_BLOCK_META_STRUCT,
    build_header,
    decode_ad_ai_block,
    decode_ad_ai_bundle,
    decode_ai_result,
    decode_header,
)


class StreamProtocolTests(unittest.TestCase):
    def test_header_round_trip(self):
        payload = b"AD7606" * 100
        raw = build_header(
            stream_id=1,
            payload_format=0x0101,
            flags=FLAG_PAYLOAD_CRC_VALID,
            boot_session_id=0x12345678,
            source_instance=4,
            sequence=0x1122334455667788,
            capture_start_us=123000,
            capture_end_us=124000,
            payload=payload,
            aux0=99,
        )
        header = decode_header(raw)
        self.assertEqual(header.boot_session_id, 0x12345678)
        self.assertEqual(header.sequence, 0x1122334455667788)
        self.assertEqual(header.payload_bytes, len(payload))
        self.assertEqual(header.aux0, 99)

    def test_header_crc_rejects_corruption(self):
        raw = bytearray(
            build_header(
                stream_id=1,
                payload_format=0x0101,
                flags=FLAG_PAYLOAD_CRC_VALID,
                boot_session_id=1,
                source_instance=4,
                sequence=1,
                capture_start_us=0,
                capture_end_us=0,
                payload=b"test",
            )
        )
        raw[24] ^= 0x01
        with self.assertRaisesRegex(ValueError, "header CRC mismatch"):
            decode_header(bytes(raw))

    def test_ai_payload_decode(self):
        payload = AI_RESULT_STRUCT.pack(
            1,
            4,
            0x544D4938,
            1,
            77,
            1234,
            5678,
            999,
            4,
            5,
            18,
            0,
            2,
            3,
            38,
            -6,
            13,
            47,
        )
        result = decode_ai_result(payload)
        self.assertEqual(result.run_count, 77)
        self.assertEqual(result.top_index, 3)
        self.assertEqual(result.outputs, (38, -6, 13, 47))
        self.assertEqual(FORMAT_AI_INT8_V1, 0x0601)
        self.assertEqual(STREAM_AI_RESULT, 6)

    def test_ad_ai_bundle_decode(self):
        raw_window = bytes(range(8))
        metadata = AD_AI_BUNDLE_META_STRUCT.pack(
            1,
            AD_AI_BUNDLE_META_STRUCT.size,
            0x544D4938,
            1,
            78,
            1235,
            5679,
            1000,
            5,
            6,
            19,
            0,
            2,
            2,
            2,
            2,
            len(raw_window),
            500,
            501,
            3,
            38,
            -6,
            13,
            47,
        )
        bundle = decode_ad_ai_bundle(metadata + raw_window)
        self.assertEqual(AD_AI_BUNDLE_META_STRUCT.size, 96)
        self.assertEqual(bundle.run_count, 78)
        self.assertEqual(bundle.input_frame_sequence, 1235)
        self.assertEqual(bundle.block_start, 500)
        self.assertEqual(bundle.block_end, 501)
        self.assertEqual(bundle.raw_window, raw_window)
        self.assertEqual(bundle.outputs, (38, -6, 13, 47))
        self.assertEqual(FORMAT_AD_AI_BUNDLE_V1, 0x0801)
        self.assertEqual(STREAM_AD_AI_BUNDLE, 8)

    def test_tiny1c_temperature_stream_ids(self):
        self.assertEqual(STREAM_TINY1C_TEMP16, 2)
        self.assertEqual(FORMAT_TINY1C_TEMP16_V1, 0x0201)

    def test_ad_ai_source_block_decode(self):
        raw_frame = bytes(range(8))
        metadata = AD_AI_BLOCK_META_STRUCT.pack(
            2,
            AD_AI_BLOCK_META_STRUCT.size,
            0x544D4938,
            1,
            79,
            1236,
            5680,
            1100,
            4,
            5,
            19,
            0,
            2,
            2,
            2,
            2,
            len(raw_frame),
            600,
            615,
            4,
            0,
            16,
            600,
            631,
            3,
            38,
            -6,
            13,
            47,
        )
        block = decode_ad_ai_block(metadata + raw_frame)
        self.assertEqual(AD_AI_BLOCK_META_STRUCT.size, 112)
        self.assertEqual(block.schema_version, 2)
        self.assertEqual(block.source_frame_sequence, 1236)
        self.assertEqual(block.source_points, 2)
        self.assertEqual(block.source_block_start, 600)
        self.assertEqual(block.source_block_end, 615)
        self.assertEqual(block.window_points, 4)
        self.assertEqual(block.window_bytes, 16)
        self.assertEqual(block.window_block_end, 631)
        self.assertEqual(block.raw_frame, raw_frame)
        self.assertEqual(block.outputs, (38, -6, 13, 47))
        self.assertEqual(FORMAT_AD_AI_BLOCK_V2, 0x0802)


if __name__ == "__main__":
    unittest.main()
