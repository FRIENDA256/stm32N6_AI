/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_ir_stream.c
  * @brief   Independent Tiny1C latest-frame MMS2 stream over UDP broadcast.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "app_ir_stream.h"

#include "app_console.h"
#include "app_ir_capture.h"
#include "app_stream_mode.h"
#include "app_stream_protocol.h"
#include "main.h"

#include <string.h>

#define APP_IR_STREAM_THREAD_STACK_SIZE  3072U
#define APP_IR_STREAM_PRIORITY_IDLE      17U
#define APP_IR_STREAM_PRIORITY_ACTIVE    12U
#define APP_IR_STREAM_THREAD_PRIORITY    APP_IR_STREAM_PRIORITY_IDLE
/* UDP chunk must stay below Ethernet MTU (1500) minus IP+UDP headers (28). */
#define APP_IR_STREAM_TX_CHUNK_SIZE      1400UL
/* Destination: subnet broadcast so the PC needs no static IP on the board. */
#define APP_IR_STREAM_DEST_IP \
  IP_ADDRESS(192U, 168U, 6U, 255U)
/* UDP releases each packet immediately after nx_udp_socket_send. */
#define APP_IR_STREAM_PACKET_PAYLOAD     1536U
#define APP_IR_STREAM_PACKET_COUNT       4U
#define APP_IR_STREAM_PACKET_POOL_SIZE \
  (APP_IR_STREAM_PACKET_COUNT * \
   (APP_IR_STREAM_PACKET_PAYLOAD + sizeof(NX_PACKET)))
#define APP_IR_STREAM_WAIT_TICKS         ((TX_TIMER_TICKS_PER_SECOND + 9U) / 10U)
#define APP_IR_STREAM_IDLE_TICKS         2U
/* Ethernet FCS, MMS2 header CRC and sequence-gap checks remain active.
 * The full-frame software payload CRC costs about 61 ms for a 256x192
 * TEMP16 frame, so disable it for the latency-sensitive live IR stream. */
#define APP_IR_STREAM_PAYLOAD_CRC_ENABLED 0U
/* IR stream rate capped at 20 fps.  Single-stream mode (app_stream_mode)
 * guarantees the AD+AI telemetry stream is idle whenever IR mode is active,
 * so the full IP-thread bandwidth is available for IR's 71 UDP datagrams per
 * frame (20 fps = 1420 sends/sec).  No cross-stream packet-pool contention. */
#define APP_IR_STREAM_MAX_FPS            20U
#define APP_IR_STREAM_FRAME_TICKS \
  ((TX_TIMER_TICKS_PER_SECOND + APP_IR_STREAM_MAX_FPS - 1U) / \
   APP_IR_STREAM_MAX_FPS)
#define APP_IR_STREAM_SOURCE_INSTANCE    0x54314354UL
#define APP_IR_STREAM_AUX_DIMENSIONS \
  ((TINY1C_DEFAULT_FRAME_WIDTH << 16) | TINY1C_DEFAULT_FRAME_HEIGHT)

static NX_UDP_SOCKET AppIRStreamSocket;
static NX_PACKET_POOL AppIRStreamPacketPoolObject;
static TX_THREAD AppIRStreamThread;
static NX_IP *AppIRStreamIp;
static NX_PACKET_POOL *AppIRStreamPacketPool;
static volatile App_IRStream_Status_t AppIRStreamStatus;
__attribute__((aligned(32))) static UCHAR
  AppIRStreamPacketMemory[APP_IR_STREAM_PACKET_POOL_SIZE];

static uint32_t AppIRStream_Lock(void)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  return primask;
}

static void AppIRStream_Unlock(uint32_t primask)
{
  if (primask == 0U) { __enable_irq(); }
}

static UINT AppIRStream_ByteAllocate(TX_BYTE_POOL *byte_pool,
                                     UCHAR **memory, ULONG size)
{
  UINT status;
  if ((byte_pool == TX_NULL) || (memory == NX_NULL)) { return NX_PTR_ERROR; }
  status = tx_byte_allocate(byte_pool, (VOID **)memory, size, TX_NO_WAIT);
  if (status != TX_SUCCESS)
  {
    App_PrintHex32("IR stream stack alloc failed: ", status);
    return NX_NOT_SUCCESSFUL;
  }
  (void)memset(*memory, 0, size);
  return NX_SUCCESS;
}

static void AppIRStream_RefreshPoolStats(void)
{
  ULONG total = 0UL, free = 0UL, empty = 0UL;
  uint32_t primask;
  if (AppIRStreamPacketPool == NX_NULL) { return; }
  (void)nx_packet_pool_info_get(AppIRStreamPacketPool,
                                &total, &free, &empty, NX_NULL, NX_NULL);
  primask = AppIRStream_Lock();
  AppIRStreamStatus.packet_pool_total = (uint32_t)total;
  AppIRStreamStatus.packet_pool_free  = (uint32_t)free;
  AppIRStreamStatus.packet_pool_empty_requests = (uint32_t)empty;
  if ((uint32_t)free < AppIRStreamStatus.packet_pool_min_free)
    AppIRStreamStatus.packet_pool_min_free = (uint32_t)free;
  AppIRStream_Unlock(primask);
}

/* Send raw bytes as UDP datagrams (≤ TX_CHUNK_SIZE each). */
static UINT AppIRStream_SendBytes(const uint8_t *data, uint32_t length)
{
  uint32_t offset = 0U;
  if ((data == NULL) && (length != 0U)) { return NX_PTR_ERROR; }

  while (offset < length)
  {
    NX_PACKET *packet = NX_NULL;
    ULONG chunk = (ULONG)(length - offset);
    UINT status;

    if (App_StreamMode_Get() != APP_STREAM_MODE_IR)
      return TX_NOT_DONE;

    if (chunk > APP_IR_STREAM_TX_CHUNK_SIZE)
      chunk = APP_IR_STREAM_TX_CHUNK_SIZE;

    status = nx_packet_allocate(AppIRStreamPacketPool, &packet,
                                NX_UDP_PACKET, APP_IR_STREAM_WAIT_TICKS);
    if (status != NX_SUCCESS)
    {
      uint32_t primask = AppIRStream_Lock();
      AppIRStreamStatus.allocation_fail_count++;
      AppIRStream_Unlock(primask);
      return status;
    }

    if (App_StreamMode_Get() != APP_STREAM_MODE_IR)
    {
      (void)nx_packet_release(packet);
      return TX_NOT_DONE;
    }

    (void)memcpy(packet->nx_packet_append_ptr, &data[offset], chunk);
    packet->nx_packet_append_ptr += chunk;
    packet->nx_packet_length     += chunk;

    status = nx_udp_socket_send(&AppIRStreamSocket, packet,
                                APP_IR_STREAM_DEST_IP, APP_IR_STREAM_PORT);
    if (status != NX_SUCCESS)
    {
      uint32_t primask = AppIRStream_Lock();
      AppIRStreamStatus.send_fail_count++;
      AppIRStream_Unlock(primask);
      (void)nx_packet_release(packet);
      return status;
    }
    offset += (uint32_t)chunk;
  }
  return NX_SUCCESS;
}

/* Pack the MMS2 header and the first chunk of payload into one datagram to
   minimise the datagram count for the 64-byte header. */
static UINT AppIRStream_SendHeaderAndPayload(const uint8_t *header,
                                             uint32_t header_length,
                                             const uint8_t *payload,
                                             uint32_t payload_length)
{
  NX_PACKET *packet = NX_NULL;
  ULONG first_payload_length;
  UINT status;

  if ((header == NULL) ||
      (header_length > APP_IR_STREAM_TX_CHUNK_SIZE) ||
      ((payload == NULL) && (payload_length != 0U)))
    return NX_PTR_ERROR;

  if (App_StreamMode_Get() != APP_STREAM_MODE_IR)
    return TX_NOT_DONE;

  first_payload_length = APP_IR_STREAM_TX_CHUNK_SIZE - header_length;
  if (first_payload_length > payload_length)
    first_payload_length = payload_length;

  status = nx_packet_allocate(AppIRStreamPacketPool, &packet,
                              NX_UDP_PACKET, APP_IR_STREAM_WAIT_TICKS);
  if (status != NX_SUCCESS)
  {
    uint32_t primask = AppIRStream_Lock();
    AppIRStreamStatus.allocation_fail_count++;
    AppIRStream_Unlock(primask);
    return status;
  }

  if (App_StreamMode_Get() != APP_STREAM_MODE_IR)
  {
    (void)nx_packet_release(packet);
    return TX_NOT_DONE;
  }

  (void)memcpy(packet->nx_packet_append_ptr, header, header_length);
  packet->nx_packet_append_ptr += header_length;
  packet->nx_packet_length     += header_length;
  if (first_payload_length != 0U)
  {
    (void)memcpy(packet->nx_packet_append_ptr, payload, first_payload_length);
    packet->nx_packet_append_ptr += first_payload_length;
    packet->nx_packet_length     += first_payload_length;
  }

  status = nx_udp_socket_send(&AppIRStreamSocket, packet,
                              APP_IR_STREAM_DEST_IP, APP_IR_STREAM_PORT);
  if (status != NX_SUCCESS)
  {
    uint32_t primask = AppIRStream_Lock();
    AppIRStreamStatus.send_fail_count++;
    AppIRStream_Unlock(primask);
    (void)nx_packet_release(packet);
    return status;
  }

  return AppIRStream_SendBytes(payload + first_payload_length,
                               payload_length - first_payload_length);
}

static UINT AppIRStream_SendFrame(const App_IRCapture_FrameLease_t *lease)
{
  App_Stream_Header_t header;
  uint8_t encoded_header[APP_STREAM_PROTOCOL_HEADER_BYTES];
  uint32_t send_start_ms, crc_ms = 0U, send_ms;
  UINT status;

  {
    uint32_t primask = AppIRStream_Lock();
    AppIRStreamStatus.send_active    = 1U;
    AppIRStreamStatus.send_sequence  = lease->sequence;
    AppIRStreamStatus.send_start_ms  = HAL_GetTick();
    AppIRStream_Unlock(primask);
  }

  (void)memset(&header, 0, sizeof(header));
  header.stream_id       = APP_STREAM_ID_TINY1C_TEMP16;
  header.payload_format  = APP_STREAM_FORMAT_TINY1C_TEMP16_V1;
  header.flags           = APP_STREAM_FLAG_SOURCE_TIMESTAMP_VALID |
                           APP_STREAM_FLAG_LATEST_SNAPSHOT;
  header.boot_session_id = AppIRStreamStatus.boot_session_id;
  header.source_instance = APP_IR_STREAM_SOURCE_INSTANCE;
  header.sequence        = lease->sequence;
  header.capture_start_us = (uint64_t)lease->timestamp_ms * 1000ULL;
  header.capture_end_us   = header.capture_start_us;
  header.payload_bytes    = lease->length;
  header.aux0             = APP_IR_STREAM_AUX_DIMENSIONS;

#if APP_IR_STREAM_PAYLOAD_CRC_ENABLED
  {
    uint32_t crc_start_ms = HAL_GetTick();
    header.flags |= APP_STREAM_FLAG_PAYLOAD_CRC_VALID;
    header.payload_crc32 = AppStreamProtocol_Crc32(lease->data, lease->length);
    crc_ms = HAL_GetTick() - crc_start_ms;
  }
#endif

  AppStreamProtocol_EncodeHeader(encoded_header, &header);
  send_start_ms = HAL_GetTick();
  if (App_StreamMode_Get() == APP_STREAM_MODE_IR)
  {
    status = AppIRStream_SendHeaderAndPayload(encoded_header,
                                              sizeof(encoded_header),
                                              lease->data,
                                              lease->length);
  }
  else
  {
    status = TX_NOT_DONE;
  }
  send_ms = HAL_GetTick() - send_start_ms;
  AppIRStream_RefreshPoolStats();

  {
    uint32_t primask = AppIRStream_Lock();
    AppIRStreamStatus.last_crc_ms  = crc_ms;
    AppIRStreamStatus.last_send_ms = send_ms;
    if (send_ms > AppIRStreamStatus.max_send_ms)
      AppIRStreamStatus.max_send_ms = send_ms;
    AppIRStreamStatus.send_active = 0U;
    AppIRStream_Unlock(primask);
  }

  if (status == NX_SUCCESS)
  {
    uint32_t primask = AppIRStream_Lock();
    AppIRStreamStatus.message_count++;
    AppIRStreamStatus.payload_bytes  += lease->length;
    AppIRStreamStatus.wire_bytes     += APP_STREAM_PROTOCOL_HEADER_BYTES + lease->length;
    AppIRStreamStatus.last_sequence   = lease->sequence;
    AppIRStreamStatus.last_timestamp_ms = lease->timestamp_ms;
    AppIRStream_Unlock(primask);
  }
  return status;
}

/* UDP thread: bind once then stream continuously — no client connection needed. */
static VOID AppIRStream_ThreadEntry(ULONG thread_input)
{
  UINT status;
  uint32_t last_sequence = 0U;
  uint8_t  have_sequence = 0U;

  (void)thread_input;

  status = nx_udp_socket_bind(&AppIRStreamSocket, NX_ANY_PORT,
                              NX_IP_PERIODIC_RATE);
  if (status != NX_SUCCESS)
  {
    App_PrintHex32("IR stream UDP bind failed: ", status);
    return;
  }

  {
    uint32_t primask = AppIRStream_Lock();
    AppIRStreamStatus.listening = 1U;
    AppIRStreamStatus.connected = 1U;
    AppIRStream_Unlock(primask);
  }
  App_Print("IR UDP: broadcast 192.168.6.255:5101 Tiny1C temp16 MMS2\r\n");

  for (;;)
  {
    App_IRCapture_FrameLease_t lease;
    ULONG frame_start_tick;
    ULONG frame_elapsed_ticks;

    if (App_StreamMode_TryAcquireSend(APP_STREAM_MODE_IR) == 0U)
    {
      /* Frames captured while another mode is selected are intentionally not
         part of this stream, so the next IR session starts a new gap window. */
      have_sequence = 0U;
      tx_thread_sleep(5U);
      continue;
    }

    frame_start_tick = tx_time_get();
    status = App_IRCapture_AcquireLatestFrame(TINY1C_CMD_TEMP, &lease);
    if (status != TX_SUCCESS)
    {
      App_StreamMode_ReleaseSend();
      tx_thread_sleep(APP_IR_STREAM_IDLE_TICKS);
      continue;
    }

    if ((have_sequence != 0U) && (lease.sequence == last_sequence))
    {
      App_IRCapture_ReleaseFrame(&lease);
      App_StreamMode_ReleaseSend();
      tx_thread_sleep(1U);
      continue;
    }

    if ((have_sequence != 0U) &&
        ((lease.sequence - last_sequence) > 1U) &&
        ((lease.sequence - last_sequence) < 0x80000000U))
    {
      uint32_t primask = AppIRStream_Lock();
      AppIRStreamStatus.source_gap_count += lease.sequence - last_sequence - 1U;
      AppIRStream_Unlock(primask);
    }

    (void)AppIRStream_SendFrame(&lease);
    last_sequence = lease.sequence;
    have_sequence = 1U;
    App_IRCapture_ReleaseFrame(&lease);
    App_StreamMode_ReleaseSend();
    frame_elapsed_ticks = tx_time_get() - frame_start_tick;
    if (frame_elapsed_ticks < APP_IR_STREAM_FRAME_TICKS)
    {
      tx_thread_sleep(APP_IR_STREAM_FRAME_TICKS - frame_elapsed_ticks);
    }
    else
    {
      tx_thread_relinquish();
    }
  }
}

UINT AppIRStream_Start(NX_IP *ip_ptr,
                       NX_PACKET_POOL *packet_pool,
                       TX_BYTE_POOL *byte_pool)
{
  UCHAR *thread_stack;
  UINT status;

  if ((ip_ptr == NX_NULL) || (byte_pool == TX_NULL)) { return NX_PTR_ERROR; }

  AppIRStreamIp = ip_ptr;
  (void)packet_pool; /* unused — IR stream has its own pool */
  (void)memset((void *)&AppIRStreamStatus, 0, sizeof(AppIRStreamStatus));
  AppIRStreamStatus.boot_session_id =
    HAL_GetUIDw0() ^ HAL_GetUIDw1() ^ HAL_GetUIDw2() ^
    HAL_GetTick() ^ (uint32_t)tx_time_get() ^ APP_IR_STREAM_PORT;

  status = nx_packet_pool_create(&AppIRStreamPacketPoolObject,
                                 "Tiny1C IR stream pool",
                                 APP_IR_STREAM_PACKET_PAYLOAD,
                                 AppIRStreamPacketMemory,
                                 sizeof(AppIRStreamPacketMemory));
  if (status != NX_SUCCESS)
  {
    App_PrintHex32("IR stream packet pool failed: ", status);
    return status;
  }
  AppIRStreamPacketPool = &AppIRStreamPacketPoolObject;
  AppIRStream_RefreshPoolStats();
  AppIRStreamStatus.packet_pool_min_free = AppIRStreamStatus.packet_pool_free;

  status = nx_udp_socket_create(ip_ptr,
                                &AppIRStreamSocket,
                                "Tiny1C IR stream",
                                NX_IP_NORMAL,
                                NX_FRAGMENT_OKAY,
                                NX_IP_TIME_TO_LIVE,
                                8U);
  if (status != NX_SUCCESS)
  {
    App_PrintHex32("IR stream UDP socket failed: ", status);
    return status;
  }

  status = AppIRStream_ByteAllocate(byte_pool, &thread_stack,
                                    APP_IR_STREAM_THREAD_STACK_SIZE);
  if (status != NX_SUCCESS) { return status; }

  status = tx_thread_create(&AppIRStreamThread,
                            "Tiny1C IR stream",
                            AppIRStream_ThreadEntry,
                            0,
                            thread_stack,
                            APP_IR_STREAM_THREAD_STACK_SIZE,
                            APP_IR_STREAM_THREAD_PRIORITY,
                            APP_IR_STREAM_THREAD_PRIORITY,
                            TX_NO_TIME_SLICE,
                            TX_AUTO_START);
  if (status != TX_SUCCESS)
  {
    App_PrintHex32("IR stream thread failed: ", status);
    return NX_NOT_SUCCESSFUL;
  }

  AppIRStreamStatus.initialized = 1U;
  return NX_SUCCESS;
}

UINT AppIRStream_SetActive(uint8_t active)
{
  UINT new_priority =
    (active != 0U) ? APP_IR_STREAM_PRIORITY_ACTIVE : APP_IR_STREAM_PRIORITY_IDLE;
  UINT old_priority;

  return tx_thread_priority_change(&AppIRStreamThread, new_priority, &old_priority);
}

void AppIRStream_GetStatus(App_IRStream_Status_t *status)
{
  uint32_t primask;
  if (status == NULL) { return; }
  AppIRStream_RefreshPoolStats();
  primask = AppIRStream_Lock();
  (void)memcpy(status, (const void *)&AppIRStreamStatus, sizeof(*status));
  AppIRStream_Unlock(primask);
}
