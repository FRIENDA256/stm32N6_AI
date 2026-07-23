/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_stream_telemetry.c
  * @brief   Continuous AD7606 and AI telemetry stream over UDP broadcast.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "app_stream_telemetry.h"

#include "app_ai.h"
#include "app_console.h"
#include "app_stream_mode.h"
#include "app_stream_protocol.h"
#include "main.h"

#include <string.h>

#define APP_STREAM_TELEMETRY_THREAD_STACK_SIZE  3072U
#define APP_STREAM_TELEMETRY_THREAD_PRIORITY    16U
/* UDP chunk must stay below Ethernet MTU (1500) minus IP+UDP headers (28). */
#define APP_STREAM_TELEMETRY_TX_CHUNK_SIZE      1400UL
/* Destination: subnet broadcast so the PC needs no static IP on the board. */
#define APP_STREAM_TELEMETRY_DEST_IP \
  IP_ADDRESS(192U, 168U, 6U, 255U)
/* UDP releases each packet immediately after nx_udp_socket_send, so a tiny
   pool of 4 is sufficient — no TX-queue depth or ACK-delay bottleneck. */
#define APP_STREAM_TELEMETRY_PACKET_PAYLOAD     1536U
#define APP_STREAM_TELEMETRY_PACKET_COUNT       4U
#define APP_STREAM_TELEMETRY_PACKET_POOL_SIZE   \
  (APP_STREAM_TELEMETRY_PACKET_COUNT * (APP_STREAM_TELEMETRY_PACKET_PAYLOAD + sizeof(NX_PACKET)))
#define APP_STREAM_TELEMETRY_WAIT_TICKS         NX_NO_WAIT
#define APP_STREAM_TELEMETRY_BLOCK_META_BYTES   112U
#define APP_STREAM_TELEMETRY_BUNDLE_BYTES       \
  (APP_STREAM_TELEMETRY_BLOCK_META_BYTES + APP_AI_SOURCE_FRAME_BYTES)
#define APP_STREAM_TELEMETRY_AI_MODEL_ID        0x544D4938UL
#define APP_STREAM_TELEMETRY_AI_MODEL_VERSION   1U

static NX_UDP_SOCKET StreamTelemetrySocket;
static NX_PACKET_POOL StreamTelemetryPacketPool;
static TX_THREAD StreamTelemetryThread;
static NX_IP *StreamTelemetryIp;
static volatile App_Stream_Telemetry_Status_t StreamTelemetryStatus;
static volatile uint8_t StreamTelemetryPacketPoolReady;
__attribute__((aligned(32))) static UCHAR StreamTelemetryPacketMemory[APP_STREAM_TELEMETRY_PACKET_POOL_SIZE];
__attribute__((aligned(32))) static uint8_t StreamTelemetryBundlePayload[APP_STREAM_TELEMETRY_BUNDLE_BYTES];

static uint32_t AppStreamTelemetry_Lock(void)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  return primask;
}

static void AppStreamTelemetry_Unlock(uint32_t primask)
{
  if (primask == 0U) { __enable_irq(); }
}

static void AppStreamTelemetry_WriteU16BE(uint8_t *output, uint16_t value)
{
  output[0] = (uint8_t)(value >> 8);
  output[1] = (uint8_t)value;
}

static void AppStreamTelemetry_WriteU32BE(uint8_t *output, uint32_t value)
{
  output[0] = (uint8_t)(value >> 24);
  output[1] = (uint8_t)(value >> 16);
  output[2] = (uint8_t)(value >> 8);
  output[3] = (uint8_t)value;
}

static void AppStreamTelemetry_WriteU64BE(uint8_t *output, uint64_t value)
{
  AppStreamTelemetry_WriteU32BE(output, (uint32_t)(value >> 32));
  AppStreamTelemetry_WriteU32BE(output + 4, (uint32_t)value);
}

static UINT AppStreamTelemetry_ByteAllocate(TX_BYTE_POOL *byte_pool,
                                            UCHAR **memory, ULONG size)
{
  UINT status;
  if ((byte_pool == TX_NULL) || (memory == NX_NULL)) { return NX_PTR_ERROR; }
  status = tx_byte_allocate(byte_pool, (VOID **)memory, size, TX_NO_WAIT);
  if (status != TX_SUCCESS)
  {
    App_PrintHex32("Telemetry stack alloc failed: ", status);
    return NX_NOT_SUCCESSFUL;
  }
  (void)memset(*memory, 0, size);
  return NX_SUCCESS;
}

static void AppStreamTelemetry_RefreshPoolStats(void)
{
  ULONG total = 0UL, free = 0UL, empty = 0UL;
  uint32_t primask;
  if (StreamTelemetryPacketPoolReady == 0U) { return; }
  (void)nx_packet_pool_info_get(&StreamTelemetryPacketPool,
                                &total, &free, &empty, NX_NULL, NX_NULL);
  primask = AppStreamTelemetry_Lock();
  StreamTelemetryStatus.packet_pool_total = (uint32_t)total;
  StreamTelemetryStatus.packet_pool_free  = (uint32_t)free;
  StreamTelemetryStatus.packet_pool_empty_requests = (uint32_t)empty;
  if ((uint32_t)free < StreamTelemetryStatus.packet_pool_min_free)
    StreamTelemetryStatus.packet_pool_min_free = (uint32_t)free;
  AppStreamTelemetry_Unlock(primask);
}

/* Send data as a sequence of UDP datagrams, each <= TX_CHUNK_SIZE bytes.
   UDP releases the packet immediately after nx_udp_socket_send, so the pool
   never starves regardless of the remote ACK behaviour. */
static UINT AppStreamTelemetry_SendBytes(const uint8_t *data, uint32_t length)
{
  uint32_t offset = 0U;
  if ((data == NULL) && (length != 0U)) { return NX_PTR_ERROR; }

  while (offset < length)
  {
    NX_PACKET *packet = NX_NULL;
    ULONG chunk = (ULONG)(length - offset);
    UINT status;

    if (App_StreamMode_Get() != APP_STREAM_MODE_ADAI)
      return TX_NOT_DONE;

    if (chunk > APP_STREAM_TELEMETRY_TX_CHUNK_SIZE)
      chunk = APP_STREAM_TELEMETRY_TX_CHUNK_SIZE;

    status = nx_packet_allocate(&StreamTelemetryPacketPool, &packet,
                                NX_UDP_PACKET,
                                APP_STREAM_TELEMETRY_WAIT_TICKS);
    if (status != NX_SUCCESS)
    {
      uint32_t primask = AppStreamTelemetry_Lock();
      StreamTelemetryStatus.allocation_fail_count++;
      AppStreamTelemetry_Unlock(primask);
      return status;
    }

    if (App_StreamMode_Get() != APP_STREAM_MODE_ADAI)
    {
      (void)nx_packet_release(packet);
      return TX_NOT_DONE;
    }

    (void)memcpy(packet->nx_packet_append_ptr, &data[offset], chunk);
    packet->nx_packet_append_ptr += chunk;
    packet->nx_packet_length     += chunk;

    status = nx_udp_socket_send(&StreamTelemetrySocket, packet,
                                APP_STREAM_TELEMETRY_DEST_IP,
                                APP_STREAM_TELEMETRY_PORT);
    if (status != NX_SUCCESS)
    {
      uint32_t primask = AppStreamTelemetry_Lock();
      StreamTelemetryStatus.send_fail_count++;
      AppStreamTelemetry_Unlock(primask);
      (void)nx_packet_release(packet);
      return status;
    }
    offset += (uint32_t)chunk;
  }
  return NX_SUCCESS;
}

static UINT AppStreamTelemetry_SendMessage(const App_Stream_Header_t *header,
                                           const uint8_t *payload)
{
  uint8_t encoded_header[APP_STREAM_PROTOCOL_HEADER_BYTES];
  UINT status;

  AppStreamProtocol_EncodeHeader(encoded_header, header);
  status = AppStreamTelemetry_SendBytes(encoded_header, sizeof(encoded_header));
  if (status == NX_SUCCESS)
    status = AppStreamTelemetry_SendBytes(payload, header->payload_bytes);
  AppStreamTelemetry_RefreshPoolStats();

  if (status == NX_SUCCESS)
  {
    uint32_t primask = AppStreamTelemetry_Lock();
    StreamTelemetryStatus.message_count++;
    StreamTelemetryStatus.payload_bytes += header->payload_bytes;
    StreamTelemetryStatus.wire_bytes += APP_STREAM_PROTOCOL_HEADER_BYTES + header->payload_bytes;
    AppStreamTelemetry_Unlock(primask);
  }
  return status;
}

static void AppStreamTelemetry_EncodeBlockMetadata(uint8_t *payload,
                                                   const App_AI_Bundle_Info_t *bundle_info)
{
  const App_AI_Status_t *ai_status = &bundle_info->ai_status;
  uint32_t average_ms = (ai_status->run_count != 0U) ?
                        (ai_status->inference_total_ms / ai_status->run_count) : 0U;

  (void)memset(payload, 0, APP_STREAM_TELEMETRY_BLOCK_META_BYTES);
  AppStreamTelemetry_WriteU16BE(&payload[0],  2U);
  AppStreamTelemetry_WriteU16BE(&payload[2],  APP_STREAM_TELEMETRY_BLOCK_META_BYTES);
  AppStreamTelemetry_WriteU32BE(&payload[4],  APP_STREAM_TELEMETRY_AI_MODEL_ID);
  AppStreamTelemetry_WriteU32BE(&payload[8],  APP_STREAM_TELEMETRY_AI_MODEL_VERSION);
  AppStreamTelemetry_WriteU32BE(&payload[12], ai_status->run_count);
  AppStreamTelemetry_WriteU32BE(&payload[16], ai_status->last_frame_seq);
  AppStreamTelemetry_WriteU32BE(&payload[20], ai_status->last_timestamp_ms);
  AppStreamTelemetry_WriteU32BE(&payload[24], ai_status->last_sample_counter);
  AppStreamTelemetry_WriteU32BE(&payload[28], ai_status->last_inference_ms);
  AppStreamTelemetry_WriteU32BE(&payload[32], average_ms);
  AppStreamTelemetry_WriteU32BE(&payload[36], ai_status->max_inference_ms);
  AppStreamTelemetry_WriteU32BE(&payload[40], ai_status->run_error_count);
  AppStreamTelemetry_WriteU32BE(&payload[44], (uint32_t)ai_status->last_run_status);
  AppStreamTelemetry_WriteU16BE(&payload[48], bundle_info->source_points);
  payload[50] = bundle_info->source_channels;
  payload[51] = bundle_info->source_bytes_per_sample;
  AppStreamTelemetry_WriteU32BE(&payload[52], bundle_info->source_sample_bytes);
  AppStreamTelemetry_WriteU64BE(&payload[56], bundle_info->source_block_start);
  AppStreamTelemetry_WriteU64BE(&payload[64], bundle_info->source_block_end);
  AppStreamTelemetry_WriteU16BE(&payload[72], bundle_info->window_points);
  AppStreamTelemetry_WriteU16BE(&payload[74], 0U);
  AppStreamTelemetry_WriteU32BE(&payload[76], bundle_info->window_sample_bytes);
  AppStreamTelemetry_WriteU64BE(&payload[80], bundle_info->window_block_start);
  AppStreamTelemetry_WriteU64BE(&payload[88], bundle_info->window_block_end);
  payload[96] = ai_status->last_top_index;
  (void)memcpy(&payload[97], ai_status->last_output, sizeof(ai_status->last_output));
}

static UINT AppStreamTelemetry_SendBundle(const App_AI_Bundle_Info_t *bundle_info,
                                          uint32_t raw_length)
{
  App_Stream_Header_t header;
  const App_AI_Status_t *ai_status = &bundle_info->ai_status;
  UINT status;

  AppStreamTelemetry_EncodeBlockMetadata(StreamTelemetryBundlePayload, bundle_info);
  (void)memset(&header, 0, sizeof(header));
  header.stream_id       = APP_STREAM_ID_AD_AI_BUNDLE;
  header.payload_format  = APP_STREAM_FORMAT_AD_AI_BLOCK_V2;
  header.flags           = APP_STREAM_FLAG_PAYLOAD_CRC_VALID |
                           APP_STREAM_FLAG_SOURCE_TIMESTAMP_VALID |
                           APP_STREAM_FLAG_LATEST_SNAPSHOT |
                           APP_STREAM_FLAG_AI_REFERENCES_AD;
  header.boot_session_id = StreamTelemetryStatus.boot_session_id;
  header.source_instance = APP_STREAM_TELEMETRY_AI_MODEL_ID;
  header.sequence        = ai_status->run_count;
  header.capture_start_us = (uint64_t)ai_status->last_timestamp_ms * 1000ULL;
  header.capture_end_us   = header.capture_start_us;
  header.payload_bytes    = APP_STREAM_TELEMETRY_BLOCK_META_BYTES + raw_length;
  header.aux0             = ai_status->last_frame_seq;
  header.payload_crc32    = AppStreamProtocol_Crc32(StreamTelemetryBundlePayload,
                                                    header.payload_bytes);

  status = AppStreamTelemetry_SendMessage(&header, StreamTelemetryBundlePayload);
  if (status == NX_SUCCESS)
  {
    uint32_t primask = AppStreamTelemetry_Lock();
    StreamTelemetryStatus.bundle_message_count++;
    StreamTelemetryStatus.last_bundle_run = ai_status->run_count;
    StreamTelemetryStatus.last_bundle_ad_sequence = ai_status->last_frame_seq;
    AppStreamTelemetry_Unlock(primask);
  }
  return status;
}

/* UDP thread: bind socket once, then send continuously without waiting for
   any client to connect.  Bundle consumption follows the selected stream mode. */
static VOID AppStreamTelemetry_ThreadEntry(ULONG thread_input)
{
  UINT status;
  uint32_t last_bundle_run = 0U;
  uint8_t  have_bundle_run = 0U;

  (void)thread_input;

  status = nx_udp_socket_bind(&StreamTelemetrySocket, NX_ANY_PORT,
                              NX_IP_PERIODIC_RATE);
  if (status != NX_SUCCESS)
  {
    App_PrintHex32("Telemetry UDP bind failed: ", status);
    return;
  }

  /* Bundle consumer activation is owned by app_stream_mode (enabled in AD+AI
     mode, disabled in IR mode). Do not toggle it here. */
  {
    uint32_t primask = AppStreamTelemetry_Lock();
    StreamTelemetryStatus.listening = 1U;
    StreamTelemetryStatus.connected = 1U;
    AppStreamTelemetry_Unlock(primask);
  }
  App_Print("Telemetry UDP: broadcast 192.168.6.255:5100 AD+AI MMS2\r\n");

  for (;;)
  {
    App_AI_Bundle_Info_t bundle_info;
    uint32_t raw_length;

    /* Hold the mode lease across the complete copy/send operation. */
    if (App_StreamMode_TryAcquireSend(APP_STREAM_MODE_ADAI) == 0U)
    {
      tx_thread_sleep(5U);
      continue;
    }

    raw_length = App_AI_CopyLatestBundle(
      &StreamTelemetryBundlePayload[APP_STREAM_TELEMETRY_BLOCK_META_BYTES],
      APP_AI_SOURCE_FRAME_BYTES,
      &bundle_info);

    if ((raw_length != 0U) &&
        ((have_bundle_run == 0U) ||
         (bundle_info.ai_status.run_count != last_bundle_run)))
    {
      if ((have_bundle_run != 0U) &&
          ((bundle_info.ai_status.run_count - last_bundle_run) > 1U) &&
          ((bundle_info.ai_status.run_count - last_bundle_run) < 0x80000000U))
      {
        uint32_t primask = AppStreamTelemetry_Lock();
        StreamTelemetryStatus.bundle_source_gap_count +=
          bundle_info.ai_status.run_count - last_bundle_run - 1U;
        AppStreamTelemetry_Unlock(primask);
      }
      (void)AppStreamTelemetry_SendBundle(&bundle_info, raw_length);
      last_bundle_run  = bundle_info.ai_status.run_count;
      have_bundle_run  = 1U;
    }

    App_StreamMode_ReleaseSend();
    tx_thread_sleep(1U);
  }
}

UINT AppStreamTelemetry_Start(NX_IP *ip_ptr, TX_BYTE_POOL *byte_pool)
{
  UCHAR *thread_stack;
  UINT status;

  if ((ip_ptr == NX_NULL) || (byte_pool == TX_NULL)) { return NX_PTR_ERROR; }

  StreamTelemetryIp = ip_ptr;
  (void)memset((void *)&StreamTelemetryStatus, 0, sizeof(StreamTelemetryStatus));
  StreamTelemetryPacketPoolReady = 0U;
  StreamTelemetryStatus.boot_session_id =
    HAL_GetUIDw0() ^ HAL_GetUIDw1() ^ HAL_GetUIDw2() ^
    HAL_GetTick() ^ (uint32_t)tx_time_get();
  StreamTelemetryStatus.packet_pool_min_free = APP_STREAM_TELEMETRY_PACKET_COUNT;

  status = nx_packet_pool_create(&StreamTelemetryPacketPool,
                                 "Telemetry TX packet pool",
                                 APP_STREAM_TELEMETRY_PACKET_PAYLOAD,
                                 StreamTelemetryPacketMemory,
                                 sizeof(StreamTelemetryPacketMemory));
  if (status != NX_SUCCESS)
  {
    App_PrintHex32("Telemetry packet pool failed: ", status);
    return status;
  }
  StreamTelemetryPacketPoolReady = 1U;

  status = nx_udp_socket_create(ip_ptr,
                                &StreamTelemetrySocket,
                                "AD AI telemetry",
                                NX_IP_NORMAL,
                                NX_FRAGMENT_OKAY,
                                NX_IP_TIME_TO_LIVE,
                                8U);
  if (status != NX_SUCCESS)
  {
    App_PrintHex32("Telemetry UDP socket failed: ", status);
    return status;
  }

  status = AppStreamTelemetry_ByteAllocate(byte_pool, &thread_stack,
                                           APP_STREAM_TELEMETRY_THREAD_STACK_SIZE);
  if (status != NX_SUCCESS) { return status; }

  status = tx_thread_create(&StreamTelemetryThread,
                            "AD AI telemetry",
                            AppStreamTelemetry_ThreadEntry,
                            0,
                            thread_stack,
                            APP_STREAM_TELEMETRY_THREAD_STACK_SIZE,
                            APP_STREAM_TELEMETRY_THREAD_PRIORITY,
                            APP_STREAM_TELEMETRY_THREAD_PRIORITY,
                            TX_NO_TIME_SLICE,
                            TX_AUTO_START);
  if (status != TX_SUCCESS)
  {
    App_PrintHex32("Telemetry thread failed: ", status);
    return NX_NOT_SUCCESSFUL;
  }

  StreamTelemetryStatus.initialized = 1U;
  AppStreamTelemetry_RefreshPoolStats();
  return NX_SUCCESS;
}

void AppStreamTelemetry_GetStatus(App_Stream_Telemetry_Status_t *status)
{
  uint32_t primask;
  if (status == NULL) { return; }
  AppStreamTelemetry_RefreshPoolStats();
  primask = AppStreamTelemetry_Lock();
  (void)memcpy(status, (const void *)&StreamTelemetryStatus, sizeof(*status));
  AppStreamTelemetry_Unlock(primask);
}
