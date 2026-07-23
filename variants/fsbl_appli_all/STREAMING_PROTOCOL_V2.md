# MMS2 continuous telemetry protocol

This document records the first implemented slice of the multimodal streaming plan.
The firmware keeps TCP port `5000` for control and exposes TCP port `5100` for
continuous paired AD7606 input windows and AI results.

## Wire framing

Every message is a 64-byte big-endian header followed by `payload_bytes` bytes.
No compiler-generated C structure is sent directly.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII `MMS2` |
| 4 | 2 | protocol version, currently `2` |
| 6 | 2 | header size, currently `64` |
| 8 | 2 | stream ID |
| 10 | 2 | payload format |
| 12 | 4 | flags |
| 16 | 4 | boot session ID |
| 20 | 4 | source instance |
| 24 | 8 | per-stream sequence |
| 32 | 8 | source capture start time in microseconds |
| 40 | 8 | source capture end time in microseconds |
| 48 | 4 | payload size |
| 52 | 4 | stream-specific auxiliary value |
| 56 | 4 | payload CRC32 |
| 60 | 4 | CRC32 of header bytes 0 through 59 |

CRC32 uses the reflected polynomial `0xEDB88320`, initial value `0xFFFFFFFF`,
and final XOR `0xFFFFFFFF`, matching Python `zlib.crc32`.

## Implemented payload

### Stream 8: AD7606 source block and AI result

- Current format: `0x0802`
- Payload size: `112 + 8192 = 8304` bytes.
- Sequence: inference run count.
- `aux0`: referenced AD7606 source-frame sequence.
- Mode: ordered producer/consumer queue while connected.

Each message carries the new `8 x 512 x int16` AD7606 source block exactly
once. The AI result in the same message was computed from the contiguous
`8 x 1024 x int16` window ending at that source block. The metadata records
both ranges, so the PC can reconstruct the lossless source stream without
duplicating the 50%-overlapped model input windows.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 2 | schema version, currently `2` |
| 2 | 2 | metadata size, currently `112` |
| 4 | 4 | model ID |
| 8 | 4 | model version |
| 12 | 4 | inference run count |
| 16 | 4 | referenced AD source-frame sequence |
| 20 | 4 | input timestamp in milliseconds |
| 24 | 4 | input sample counter |
| 28 | 4 | current inference time in milliseconds |
| 32 | 4 | average inference time |
| 36 | 4 | maximum inference time |
| 40 | 4 | cumulative inference error count |
| 44 | 4 | signed runtime status |
| 48 | 2 | source points per channel, currently `512` |
| 50 | 1 | source channel count, currently `8` |
| 51 | 1 | source bytes per sample, currently `2` |
| 52 | 4 | source block bytes, currently `8192` |
| 56 | 8 | first source sample/block index |
| 64 | 8 | last source sample/block index |
| 72 | 2 | AI window points per channel, currently `1024` |
| 74 | 2 | reserved |
| 76 | 4 | AI window bytes, currently `16384` |
| 80 | 8 | first AI-window sample/block index |
| 88 | 8 | last AI-window sample/block index |
| 96 | 1 | winning output index |
| 97 | 4 | four signed INT8 outputs |
| 101 | 11 | reserved, zero |
| 112 | 8192 | source AD block, signed-16 little-endian |

### Legacy stream 8 format

- Format: `0x0801`
- Payload size: `96 + 16384 = 16480` bytes.
- Sequence: inference run count.
- `aux0`: referenced AD7606 frame sequence.
- Mode: paired single-slot backpressure while connected.

The first 96 payload bytes are big-endian metadata. They are followed by the
exact signed-16-bit AD input window consumed by the model. Raw samples are
little-endian and point-major/interleaved: point 0 channels 0 through 7, then
point 1 channels 0 through 7, and so on.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 2 | bundle schema version, currently `1` |
| 2 | 2 | metadata size, currently `96` |
| 4 | 4 | model ID |
| 8 | 4 | model version |
| 12 | 4 | inference run count |
| 16 | 4 | referenced AD frame sequence |
| 20 | 4 | input timestamp in milliseconds |
| 24 | 4 | input sample counter |
| 28 | 4 | current inference time in milliseconds |
| 32 | 4 | average inference time |
| 36 | 4 | maximum inference time |
| 40 | 4 | cumulative inference error count |
| 44 | 4 | signed runtime status |
| 48 | 2 | points per channel, currently `1024` |
| 50 | 1 | channel count, currently `8` |
| 51 | 1 | bytes per sample, currently `2` |
| 52 | 4 | raw window bytes, currently `16384` |
| 56 | 8 | first raw sample/block index |
| 64 | 8 | last raw sample/block index |
| 72 | 1 | winning output index |
| 73 | 4 | four signed INT8 outputs |
| 77 | 19 | reserved, zero |
| 96 | 16384 | exact model input raw window |

The outer MMS2 payload CRC covers metadata and raw samples together. The
receiver verifies that `points * channels * bytes_per_sample == raw_bytes`, the
outer sequence matches the AI run count, and `aux0` matches the referenced AD
frame sequence.

While a telemetry client is connected, the AI publisher and telemetry sender
use a single-slot backpressure mailbox. AI does not overwrite a completed
bundle until telemetry has copied it into its private transmit buffer. This
keeps bundle sequences continuous without allocating another 16 KiB queue.
Backpressure is disabled as soon as the client disconnects, so standalone AI
operation continues at its configured target rate.

Stream IDs `1` (`AD7606 raw`) and `6` (`AI result`) remain reserved for future
independent live/lossless profiles but are not emitted by the paired profile.

## Control and tests

Query the endpoint and counters:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\tcp_ir.ps1 -Command "STREAM LIST"
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\tcp_ir.ps1 -Command "STREAM STAT"
```

Receive and validate for 30 seconds:

```powershell
python .\tools\multimodal_stream_receiver.py --duration 30 --fail-on-gap
```

Run continuously and reconnect after cable or application interruptions:

```powershell
python .\tools\multimodal_stream_receiver.py --duration 0 --reconnect
```

Record the raw MMS2 message sequence:

```powershell
python .\tools\multimodal_stream_receiver.py --duration 300 --record .\tools\net_captures\telemetry.mms2
```

The telemetry sender has a dedicated 24-packet NetX pool. AD7606 DMA remains in
its own acquisition thread. The AI pipeline runs above telemetry priority and
publishes a stable seqlock snapshot. A connected slow client can reduce the AI
publication rate through bounded single-slot backpressure, but cannot block
AD7606 acquisition. A disconnected client never throttles AI.
