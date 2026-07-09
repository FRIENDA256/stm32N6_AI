/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_tcp_command.c
  * @brief   Minimal NetX Duo TCP command service.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "app_tcp_command.h"
#include "ad7606_spi_dma.h"
#include "app_console.h"
#include "tiny1c_port_stm32_hal.h"
#include <stdio.h>
#include <string.h>

#define APP_TCP_COMMAND_THREAD_STACK_SIZE 2048U
#define APP_TCP_COMMAND_THREAD_PRIORITY   13U
#define APP_TCP_COMMAND_LISTEN_QUEUE      1U
#define APP_TCP_COMMAND_WINDOW_SIZE       32768UL
#define APP_TCP_COMMAND_RX_BUFFER_SIZE    128U
#define APP_TCP_COMMAND_TX_CHUNK_SIZE     1400UL
#define APP_TCP_COMMAND_LOG_INTERVAL      8UL
#define APP_TCP_COMMAND_AD_WAIT_MS        3000U
#define APP_TCP_COMMAND_AD_POLL_MS        20U
#define APP_TCP_COMMAND_IR_PAUSE_AD7606   1U
#define APP_TCP_COMMAND_IR_AD_WAIT_MS     200U
#define APP_TCP_COMMAND_IR_AD_POLL_MS     2U
#define APP_TCP_COMMAND_THR_DEFAULT_MIB   32U
#define APP_TCP_COMMAND_THR_MAX_MIB       256U
#define APP_TCP_COMMAND_UDP_DEFAULT_PORT  5010U
#define APP_TCP_COMMAND_UDP_DEFAULT_MS    5000U
#define APP_TCP_COMMAND_UDP_MAX_MS        60000U
#define APP_TCP_COMMAND_UDP_DEFAULT_SIZE  1472U
#define APP_TCP_COMMAND_UDP_MIN_SIZE      32U
#define APP_TCP_COMMAND_UDP_MAX_SIZE      1472U
#define APP_TCP_COMMAND_UDP_HEADER_SIZE   16U

static NX_TCP_SOCKET TcpCommandSocket;
static NX_UDP_SOCKET TcpCommandUdpThroughputSocket;
static TX_THREAD TcpCommandThread;
static NX_IP *TcpCommandIp;
static NX_PACKET_POOL *TcpCommandPacketPool;
static TX_BYTE_POOL *TcpCommandBytePool;
static ULONG TcpCommandConnections;
static ULONG TcpCommandRxPackets;
static uint8_t TcpCommandAdFrame[AD7606_SPI4_MAX_FRAME_SIZE];

typedef struct
{
  ULONG packets;
  ULONG alloc_ticks;
  ULONG fill_ticks;
  ULONG send_ticks;
} AppTcpCommand_ThroughputStats;

static ULONG AppTcpCommand_MsToTicks(uint32_t ms);

static void AppTcpCommand_IrCaptureBegin(void)
{
#if (APP_TCP_COMMAND_IR_PAUSE_AD7606 == 1U)
  ULONG start_tick;
  ULONG timeout_ticks;
  ULONG poll_ticks;

  AD7606_SPI4_SetPaused(1U);

  start_tick = tx_time_get();
  timeout_ticks = AppTcpCommand_MsToTicks(APP_TCP_COMMAND_IR_AD_WAIT_MS);
  poll_ticks = AppTcpCommand_MsToTicks(APP_TCP_COMMAND_IR_AD_POLL_MS);

  while (AD7606_SPI4_IsIdle() == 0U)
  {
    if ((tx_time_get() - start_tick) >= timeout_ticks)
    {
      App_Print("AD7606 pause wait timeout before Tiny1C capture\r\n");
      break;
    }
    tx_thread_sleep(poll_ticks);
  }
#endif
}

static void AppTcpCommand_IrCaptureEnd(void)
{
#if (APP_TCP_COMMAND_IR_PAUSE_AD7606 == 1U)
  AD7606_SPI4_SetPaused(0U);
#endif
}

static UINT AppTcpCommand_ByteAllocate(TX_BYTE_POOL *byte_pool, UCHAR **memory, ULONG size)
{
  UINT status;

  if ((byte_pool == NX_NULL) || (memory == NX_NULL))
  {
    return NX_PTR_ERROR;
  }

  status = tx_byte_allocate(byte_pool, (VOID **)memory, size, TX_NO_WAIT);
  if (status != TX_SUCCESS)
  {
    App_PrintHex32("TCP cmd stack alloc failed: ", status);
    return NX_NOT_SUCCESSFUL;
  }

  memset(*memory, 0, size);
  return NX_SUCCESS;
}

static ULONG AppTcpCommand_MsToTicks(uint32_t ms)
{
  ULONG ticks = (ULONG)(((uint64_t)ms * TX_TIMER_TICKS_PER_SECOND + 999ULL) / 1000ULL);

  return (ticks == 0UL) ? 1UL : ticks;
}

static void AppTcpCommand_AppendText(char *line, ULONG *pos, ULONG max_len, const char *text)
{
  while ((text != NX_NULL) && (*text != '\0') && (*pos < (max_len - 1UL)))
  {
    line[*pos] = *text;
    (*pos)++;
    text++;
  }
}

static void AppTcpCommand_AppendHex32(char *line, ULONG *pos, ULONG max_len, ULONG value)
{
  static const char hex[] = "0123456789ABCDEF";

  AppTcpCommand_AppendText(line, pos, max_len, "0x");
  for (ULONG nibble = 0UL; nibble < 8UL; nibble++)
  {
    ULONG shift = 28UL - (nibble * 4UL);
    if (*pos >= (max_len - 1UL))
    {
      break;
    }
    line[*pos] = hex[(value >> shift) & 0xFUL];
    (*pos)++;
  }
}

static char AppTcpCommand_ToUpper(char c)
{
  if ((c >= 'a') && (c <= 'z'))
  {
    return (char)(c - ('a' - 'A'));
  }
  return c;
}

static char *AppTcpCommand_Trim(char *text)
{
  char *end;

  while ((*text == ' ') || (*text == '\t') || (*text == '\r') || (*text == '\n'))
  {
    text++;
  }

  end = text + strlen(text);
  while ((end > text) &&
         ((end[-1] == ' ') || (end[-1] == '\t') || (end[-1] == '\r') || (end[-1] == '\n')))
  {
    end--;
  }
  *end = '\0';

  return text;
}

static const char *AppTcpCommand_SkipSpaces(const char *text)
{
  while ((*text == ' ') || (*text == '\t'))
  {
    text++;
  }
  return text;
}

static UINT AppTcpCommand_Equals(const char *left, const char *right)
{
  while ((*left != '\0') && (*right != '\0'))
  {
    if (AppTcpCommand_ToUpper(*left) != AppTcpCommand_ToUpper(*right))
    {
      return NX_FALSE;
    }
    left++;
    right++;
  }

  return ((*left == '\0') && (*right == '\0')) ? NX_TRUE : NX_FALSE;
}

static UINT AppTcpCommand_MatchCommand(const char *text, const char *command, const char **args)
{
  const char *left = text;
  const char *right = command;

  while ((*left != '\0') && (*right != '\0'))
  {
    if (AppTcpCommand_ToUpper(*left) != AppTcpCommand_ToUpper(*right))
    {
      return NX_FALSE;
    }
    left++;
    right++;
  }

  if (*right != '\0')
  {
    return NX_FALSE;
  }

  if ((*left != '\0') && (*left != ' ') && (*left != '\t'))
  {
    return NX_FALSE;
  }

  if (args != NULL)
  {
    *args = AppTcpCommand_SkipSpaces(left);
  }

  return NX_TRUE;
}

static UINT AppTcpCommand_ParseU32(const char **cursor, uint32_t *value)
{
  const char *text;
  uint32_t result = 0U;
  uint8_t have_digit = 0U;

  if ((cursor == NULL) || (value == NULL))
  {
    return NX_PTR_ERROR;
  }

  text = AppTcpCommand_SkipSpaces(*cursor);
  while ((*text >= '0') && (*text <= '9'))
  {
    result = (result * 10U) + (uint32_t)(*text - '0');
    have_digit = 1U;
    text++;
  }

  if (have_digit == 0U)
  {
    return NX_NOT_SUCCESSFUL;
  }

  *cursor = text;
  *value = result;
  return NX_SUCCESS;
}

static UINT AppTcpCommand_ParseIPv4(const char **cursor, ULONG *addr)
{
  const char *text;
  uint32_t octet[4];

  if ((cursor == NULL) || (addr == NULL))
  {
    return NX_PTR_ERROR;
  }

  text = AppTcpCommand_SkipSpaces(*cursor);
  for (uint32_t i = 0U; i < 4U; i++)
  {
    uint32_t value = 0U;
    uint8_t have_digit = 0U;

    while ((*text >= '0') && (*text <= '9'))
    {
      value = (value * 10U) + (uint32_t)(*text - '0');
      if (value > 255U)
      {
        return NX_NOT_SUCCESSFUL;
      }
      have_digit = 1U;
      text++;
    }

    if (have_digit == 0U)
    {
      return NX_NOT_SUCCESSFUL;
    }

    octet[i] = value;

    if (i < 3U)
    {
      if (*text != '.')
      {
        return NX_NOT_SUCCESSFUL;
      }
      text++;
    }
  }

  *cursor = text;
  *addr = IP_ADDRESS(octet[0], octet[1], octet[2], octet[3]);
  return NX_SUCCESS;
}

static void AppTcpCommand_WriteLE16(uint8_t *data, uint16_t value)
{
  data[0] = (uint8_t)(value & 0xFFU);
  data[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void AppTcpCommand_WriteLE32(uint8_t *data, uint32_t value)
{
  data[0] = (uint8_t)(value & 0xFFU);
  data[1] = (uint8_t)((value >> 8) & 0xFFU);
  data[2] = (uint8_t)((value >> 16) & 0xFFU);
  data[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static ULONG AppTcpCommand_TicksToMs(ULONG ticks)
{
  return (ticks * 1000UL) / TX_TIMER_TICKS_PER_SECOND;
}

static UINT AppTcpCommand_ReservePacketPayload(NX_PACKET *packet_ptr, ULONG payload_size)
{
  ULONG available;

  if (packet_ptr == NX_NULL)
  {
    return NX_PTR_ERROR;
  }

  available = (ULONG)(packet_ptr->nx_packet_data_end - packet_ptr->nx_packet_append_ptr);
  if (payload_size > available)
  {
    return NX_SIZE_ERROR;
  }

  packet_ptr->nx_packet_append_ptr += payload_size;
  packet_ptr->nx_packet_length += payload_size;

  return NX_SUCCESS;
}

static UINT AppTcpCommand_PreparePacketPayload(NX_PACKET *packet_ptr, ULONG payload_size, uint8_t fill)
{
  UCHAR *payload;
  ULONG available;

  if (packet_ptr == NX_NULL)
  {
    return NX_PTR_ERROR;
  }

  available = (ULONG)(packet_ptr->nx_packet_data_end - packet_ptr->nx_packet_append_ptr);
  if (payload_size > available)
  {
    return NX_SIZE_ERROR;
  }

  payload = packet_ptr->nx_packet_append_ptr;
  memset(payload, fill, payload_size);
  packet_ptr->nx_packet_append_ptr += payload_size;
  packet_ptr->nx_packet_length += payload_size;

  return NX_SUCCESS;
}

static UINT AppTcpCommand_CopyPacketPayload(NX_PACKET *packet_ptr, const uint8_t *data, ULONG payload_size)
{
  UCHAR *payload;
  ULONG available;

  if ((packet_ptr == NX_NULL) || ((data == NULL) && (payload_size != 0UL)))
  {
    return NX_PTR_ERROR;
  }

  available = (ULONG)(packet_ptr->nx_packet_data_end - packet_ptr->nx_packet_append_ptr);
  if (payload_size > available)
  {
    return NX_SIZE_ERROR;
  }

  payload = packet_ptr->nx_packet_append_ptr;
  if (payload_size != 0UL)
  {
    memcpy(payload, data, payload_size);
  }
  packet_ptr->nx_packet_append_ptr += payload_size;
  packet_ptr->nx_packet_length += payload_size;

  return NX_SUCCESS;
}

static UINT AppTcpCommand_SendThroughputPacket(ULONG payload_size,
                                               uint8_t fill,
                                               uint8_t fill_payload,
                                               AppTcpCommand_ThroughputStats *stats)
{
  NX_PACKET *packet_ptr;
  UINT status;
  ULONG tick;

  if (TcpCommandPacketPool == NX_NULL)
  {
    return NX_PTR_ERROR;
  }

  tick = tx_time_get();
  status = nx_packet_allocate(TcpCommandPacketPool, &packet_ptr, NX_TCP_PACKET, NX_WAIT_FOREVER);
  if (stats != NULL)
  {
    stats->alloc_ticks += tx_time_get() - tick;
  }
  if (status != NX_SUCCESS)
  {
    return status;
  }

  tick = tx_time_get();
  status = (fill_payload != 0U) ?
    AppTcpCommand_PreparePacketPayload(packet_ptr, payload_size, fill) :
    AppTcpCommand_ReservePacketPayload(packet_ptr, payload_size);
  if (stats != NULL)
  {
    stats->fill_ticks += tx_time_get() - tick;
  }
  if (status == NX_SUCCESS)
  {
    tick = tx_time_get();
    status = nx_tcp_socket_send(&TcpCommandSocket, packet_ptr, NX_WAIT_FOREVER);
    if (stats != NULL)
    {
      stats->send_ticks += tx_time_get() - tick;
    }
    if (status == NX_SUCCESS)
    {
      packet_ptr = NX_NULL;
      if (stats != NULL)
      {
        stats->packets++;
      }
    }
  }

  if (packet_ptr != NX_NULL)
  {
    (void)nx_packet_release(packet_ptr);
  }

  return status;
}

static UINT AppTcpCommand_SendText(const char *text)
{
  NX_PACKET *packet_ptr;
  UINT status;

  if ((TcpCommandPacketPool == NX_NULL) || (text == NX_NULL))
  {
    return NX_PTR_ERROR;
  }

  status = nx_packet_allocate(TcpCommandPacketPool, &packet_ptr, NX_TCP_PACKET, NX_WAIT_FOREVER);
  if (status != NX_SUCCESS)
  {
    return status;
  }

  status = AppTcpCommand_CopyPacketPayload(packet_ptr,
                                           (const uint8_t *)text,
                                           (ULONG)strlen(text));
  if (status == NX_SUCCESS)
  {
    status = nx_tcp_socket_send(&TcpCommandSocket, packet_ptr, NX_WAIT_FOREVER);
    if (status == NX_SUCCESS)
    {
      packet_ptr = NX_NULL;
    }
  }

  if (packet_ptr != NX_NULL)
  {
    (void)nx_packet_release(packet_ptr);
  }

  return status;
}

static uint32_t AppTcpCommand_Crc32(const uint8_t *data, uint32_t len)
{
  uint32_t crc = 0xFFFFFFFFU;

  if (data == NULL)
  {
    return 0U;
  }

  for (uint32_t i = 0U; i < len; i++)
  {
    crc ^= data[i];
    for (uint32_t bit = 0U; bit < 8U; bit++)
    {
      if ((crc & 1U) != 0U)
      {
        crc = (crc >> 1U) ^ 0xEDB88320U;
      }
      else
      {
        crc >>= 1U;
      }
    }
  }

  return crc ^ 0xFFFFFFFFU;
}

static UINT AppTcpCommand_SendBytes(const uint8_t *data, uint32_t len)
{
  uint32_t offset = 0U;

  if ((TcpCommandPacketPool == NX_NULL) || ((data == NULL) && (len != 0U)))
  {
    return NX_PTR_ERROR;
  }

  while (offset < len)
  {
    NX_PACKET *packet_ptr;
    ULONG chunk = (ULONG)(len - offset);
    UINT status;

    if (chunk > APP_TCP_COMMAND_TX_CHUNK_SIZE)
    {
      chunk = APP_TCP_COMMAND_TX_CHUNK_SIZE;
    }

    status = nx_packet_allocate(TcpCommandPacketPool, &packet_ptr, NX_TCP_PACKET, NX_WAIT_FOREVER);
    if (status != NX_SUCCESS)
    {
      return status;
    }

    status = AppTcpCommand_CopyPacketPayload(packet_ptr, &data[offset], chunk);
    if (status == NX_SUCCESS)
    {
      status = nx_tcp_socket_send(&TcpCommandSocket, packet_ptr, NX_WAIT_FOREVER);
      if (status == NX_SUCCESS)
      {
        packet_ptr = NX_NULL;
      }
    }

    if (packet_ptr != NX_NULL)
    {
      (void)nx_packet_release(packet_ptr);
    }

    if (status != NX_SUCCESS)
    {
      return status;
    }

    offset += (uint32_t)chunk;
  }

  return NX_SUCCESS;
}

static UINT AppTcpCommand_SendBinaryFrame(const char *header, const uint8_t *data, uint32_t len)
{
  UINT status;

  status = AppTcpCommand_SendText(header);
  if (status != NX_SUCCESS)
  {
    return status;
  }

  status = AppTcpCommand_SendBytes(data, len);
  if (status != NX_SUCCESS)
  {
    return status;
  }

  return AppTcpCommand_SendText("\r\nEND_STM32N6_BINARY\r\n");
}

static UINT AppTcpCommand_SendStatus(void)
{
  ULONG pool_total = 0UL;
  ULONG pool_free = 0UL;
  ULONG pool_empty = 0UL;
  ULONG byte_available = 0UL;
  ULONG byte_fragments = 0UL;
  ULONG interface_capability = 0UL;
  CHAR *byte_pool_name = NX_NULL;
  char line[256];
  ULONG pos = 0UL;

  if (TcpCommandPacketPool != NX_NULL)
  {
    (void)nx_packet_pool_info_get(TcpCommandPacketPool,
                                  &pool_total,
                                  &pool_free,
                                  &pool_empty,
                                  NX_NULL,
                                  NX_NULL);
  }

  if (TcpCommandBytePool != TX_NULL)
  {
    (void)tx_byte_pool_info_get(TcpCommandBytePool,
                                &byte_pool_name,
                                &byte_available,
                                &byte_fragments,
                                TX_NULL,
                                TX_NULL,
                                TX_NULL);
  }

#ifdef NX_ENABLE_INTERFACE_CAPABILITY
  if (TcpCommandIp != NX_NULL)
  {
    (void)nx_ip_interface_capability_get(TcpCommandIp, 0U, &interface_capability);
  }
#endif

  AppTcpCommand_AppendText(line, &pos, sizeof(line), "OK connections=");
  AppTcpCommand_AppendHex32(line, &pos, sizeof(line), TcpCommandConnections);
  AppTcpCommand_AppendText(line, &pos, sizeof(line), " rx_packets=");
  AppTcpCommand_AppendHex32(line, &pos, sizeof(line), TcpCommandRxPackets);
  AppTcpCommand_AppendText(line, &pos, sizeof(line), " pool_total=");
  AppTcpCommand_AppendHex32(line, &pos, sizeof(line), pool_total);
  AppTcpCommand_AppendText(line, &pos, sizeof(line), " pool_free=");
  AppTcpCommand_AppendHex32(line, &pos, sizeof(line), pool_free);
  AppTcpCommand_AppendText(line, &pos, sizeof(line), " pool_empty=");
  AppTcpCommand_AppendHex32(line, &pos, sizeof(line), pool_empty);
  AppTcpCommand_AppendText(line, &pos, sizeof(line), " byte_avail=");
  AppTcpCommand_AppendHex32(line, &pos, sizeof(line), byte_available);
  AppTcpCommand_AppendText(line, &pos, sizeof(line), " byte_frag=");
  AppTcpCommand_AppendHex32(line, &pos, sizeof(line), byte_fragments);
  AppTcpCommand_AppendText(line, &pos, sizeof(line), " if_cap=");
  AppTcpCommand_AppendHex32(line, &pos, sizeof(line), interface_capability);
  AppTcpCommand_AppendText(line, &pos, sizeof(line), "\r\n");
  line[pos] = '\0';

  return AppTcpCommand_SendText(line);
}

static UINT AppTcpCommand_SendTiny1CResult(const char *name, uint8_t tiny1c_command)
{
  char line[80];
  ULONG pos = 0UL;
  tiny1c_status_t status;

  status = Tiny1C_STM32_ProcessCommand(tiny1c_command);
  if (status == TINY1C_STATUS_OK)
  {
    AppTcpCommand_AppendText(line, &pos, sizeof(line), "OK ");
  }
  else if (status == TINY1C_STATUS_UNSUPPORTED)
  {
    AppTcpCommand_AppendText(line, &pos, sizeof(line), "ERR unsupported ");
  }
  else
  {
    AppTcpCommand_AppendText(line, &pos, sizeof(line), "ERR failed ");
  }

  AppTcpCommand_AppendText(line, &pos, sizeof(line), name);
  AppTcpCommand_AppendText(line, &pos, sizeof(line), "\r\n");
  line[pos] = '\0';

  return AppTcpCommand_SendText(line);
}

static UINT AppTcpCommand_SendTiny1CCaptureDumpResult(const char *name, uint8_t capture_command)
{
  char line[80];
  ULONG pos = 0UL;
  tiny1c_status_t status;

  AppTcpCommand_IrCaptureBegin();
  status = Tiny1C_STM32_ProcessCommand(capture_command);
  AppTcpCommand_IrCaptureEnd();
  if (status == TINY1C_STATUS_OK)
  {
    status = Tiny1C_STM32_ProcessCommand((uint8_t)'b');
  }

  if (status == TINY1C_STATUS_OK)
  {
    AppTcpCommand_AppendText(line, &pos, sizeof(line), "OK ");
  }
  else if (status == TINY1C_STATUS_UNSUPPORTED)
  {
    AppTcpCommand_AppendText(line, &pos, sizeof(line), "ERR unsupported ");
  }
  else
  {
    AppTcpCommand_AppendText(line, &pos, sizeof(line), "ERR failed ");
  }

  AppTcpCommand_AppendText(line, &pos, sizeof(line), name);
  AppTcpCommand_AppendText(line, &pos, sizeof(line), "\r\n");
  line[pos] = '\0';

  return AppTcpCommand_SendText(line);
}

static UINT AppTcpCommand_SendAD7606Binary(void)
{
  AD7606_SPI4_FrameInfo_t info;
  uint32_t frame_len;
  uint32_t transport_crc;
  ULONG start_tick;
  ULONG timeout_ticks;
  ULONG poll_ticks;
  char header[256];
  int len;

  start_tick = tx_time_get();
  timeout_ticks = AppTcpCommand_MsToTicks(APP_TCP_COMMAND_AD_WAIT_MS);
  poll_ticks = AppTcpCommand_MsToTicks(APP_TCP_COMMAND_AD_POLL_MS);

  do
  {
    frame_len = AD7606_SPI4_CopyLatestFrame(TcpCommandAdFrame, sizeof(TcpCommandAdFrame), &info);
    if (frame_len != 0U)
    {
      break;
    }

    tx_thread_sleep(poll_ticks);
  } while ((tx_time_get() - start_tick) < timeout_ticks);

  if (frame_len == 0U)
  {
    return AppTcpCommand_SendText("ERR AD7606 no valid frame timeout\r\n");
  }

  transport_crc = AppTcpCommand_Crc32(TcpCommandAdFrame, frame_len);
  len = snprintf(header,
                 sizeof(header),
                 "STM32N6_BIN V1 SOURCE=AD7606 TYPE=0x%02X IRQ=%lu SEQ=%lu TOTAL=%u PAYLOAD=%u TS=%lu SAMPLE=%lu BYTES=%lu CRC32=0x%08lX\r\nBEGIN_STM32N6_BINARY\r\n",
                 (unsigned int)info.frame_type,
                 (unsigned long)info.irq_count,
                 (unsigned long)info.frame_seq,
                 (unsigned int)info.total_len,
                 (unsigned int)info.payload_len,
                 (unsigned long)info.timestamp_ms,
                 (unsigned long)info.sample_counter,
                 (unsigned long)frame_len,
                 (unsigned long)transport_crc);
  if ((len <= 0) || ((uint32_t)len >= sizeof(header)))
  {
    return AppTcpCommand_SendText("ERR AD7606 header failed\r\n");
  }

  return AppTcpCommand_SendBinaryFrame(header, TcpCommandAdFrame, frame_len);
}

static UINT AppTcpCommand_SendTiny1CBinary(const char *name, uint8_t frame_command, uint8_t baseline_mode)
{
  const uint8_t *frame;
  uint32_t frame_len;
  uint8_t actual_command;
  uint32_t crc32;
  tiny1c_status_t status;
  char header[192];
  int len;

  AppTcpCommand_IrCaptureBegin();
  status = (baseline_mode != 0U) ?
    Tiny1C_STM32_CaptureFrameBaseline(frame_command) :
    Tiny1C_STM32_CaptureFrame(frame_command);
  AppTcpCommand_IrCaptureEnd();
  if (status != TINY1C_STATUS_OK)
  {
    return AppTcpCommand_SendText("ERR Tiny1C capture failed\r\n");
  }

  status = Tiny1C_STM32_GetLatestFrame(&frame, &frame_len, &actual_command, &crc32);
  if (status != TINY1C_STATUS_OK)
  {
    return AppTcpCommand_SendText("ERR Tiny1C no frame\r\n");
  }

  len = snprintf(header,
                 sizeof(header),
                 "STM32N6_BIN V1 SOURCE=TINY1C KIND=%s MODE=%s CMD=0x%02X WIDTH=%lu HEIGHT=%lu BYTES=%lu CRC32=0x%08lX\r\nBEGIN_STM32N6_BINARY\r\n",
                 name,
                 (baseline_mode != 0U) ? "baseline" : "direct",
                 (unsigned int)actual_command,
                 (unsigned long)TINY1C_DEFAULT_FRAME_WIDTH,
                 (unsigned long)TINY1C_DEFAULT_FRAME_HEIGHT,
                 (unsigned long)frame_len,
                 (unsigned long)crc32);
  if ((len <= 0) || ((uint32_t)len >= sizeof(header)))
  {
    return AppTcpCommand_SendText("ERR Tiny1C header failed\r\n");
  }

  return AppTcpCommand_SendBinaryFrame(header, frame, frame_len);
}

static UINT AppTcpCommand_SendTcpThroughput(const char *args, uint8_t fill_payload)
{
  const char *cursor = args;
  AppTcpCommand_ThroughputStats stats = {0};
  uint32_t mib = APP_TCP_COMMAND_THR_DEFAULT_MIB;
  uint32_t total_bytes;
  uint32_t sent_bytes = 0U;
  ULONG start_tick;
  ULONG elapsed_ticks;
  ULONG elapsed_ms;
  char line[240];
  int len;

  cursor = AppTcpCommand_SkipSpaces(cursor);
  if (*cursor != '\0')
  {
    if (AppTcpCommand_ParseU32(&cursor, &mib) != NX_SUCCESS)
    {
      return AppTcpCommand_SendText("ERR usage: TCPTHR [MiB]\r\n");
    }
  }

  if (mib == 0U)
  {
    mib = APP_TCP_COMMAND_THR_DEFAULT_MIB;
  }
  if (mib > APP_TCP_COMMAND_THR_MAX_MIB)
  {
    mib = APP_TCP_COMMAND_THR_MAX_MIB;
  }

  total_bytes = mib * 1024U * 1024U;

  len = snprintf(line,
                 sizeof(line),
                 "STM32N6_THR V1 MODE=TCP FILL=%s BYTES=%lu CHUNK=%lu\r\nBEGIN_STM32N6_THR\r\n",
                 (fill_payload != 0U) ? "pattern" : "none",
                 (unsigned long)total_bytes,
                 (unsigned long)APP_TCP_COMMAND_TX_CHUNK_SIZE);
  if ((len <= 0) || ((uint32_t)len >= sizeof(line)))
  {
    return AppTcpCommand_SendText("ERR TCPTHR header failed\r\n");
  }

  if (AppTcpCommand_SendText(line) != NX_SUCCESS)
  {
    return NX_NOT_SUCCESSFUL;
  }

  start_tick = tx_time_get();
  while (sent_bytes < total_bytes)
  {
    uint32_t chunk = total_bytes - sent_bytes;
    UINT status;

    if (chunk > APP_TCP_COMMAND_TX_CHUNK_SIZE)
    {
      chunk = APP_TCP_COMMAND_TX_CHUNK_SIZE;
    }

    status = AppTcpCommand_SendThroughputPacket(chunk,
                                                (uint8_t)(sent_bytes >> 8),
                                                fill_payload,
                                                &stats);
    if (status != NX_SUCCESS)
    {
      return status;
    }

    sent_bytes += chunk;
  }

  elapsed_ticks = tx_time_get() - start_tick;
  elapsed_ms = (elapsed_ticks * 1000UL) / TX_TIMER_TICKS_PER_SECOND;
  if (elapsed_ms == 0UL)
  {
    elapsed_ms = 1UL;
  }

  len = snprintf(line,
                 sizeof(line),
                 "\r\nEND_STM32N6_THR MODE=TCP FILL=%s BYTES=%lu MS=%lu PKTS=%lu ALLOC_MS=%lu FILL_MS=%lu SEND_MS=%lu\r\n",
                 (fill_payload != 0U) ? "pattern" : "none",
                 (unsigned long)sent_bytes,
                 (unsigned long)elapsed_ms,
                 (unsigned long)stats.packets,
                 (unsigned long)AppTcpCommand_TicksToMs(stats.alloc_ticks),
                 (unsigned long)AppTcpCommand_TicksToMs(stats.fill_ticks),
                 (unsigned long)AppTcpCommand_TicksToMs(stats.send_ticks));
  if ((len <= 0) || ((uint32_t)len >= sizeof(line)))
  {
    return NX_NOT_SUCCESSFUL;
  }

  return AppTcpCommand_SendText(line);
}

static UINT AppTcpCommand_SendUdpThroughput(const char *args, uint8_t fill_payload)
{
  const char *cursor = args;
  AppTcpCommand_ThroughputStats stats = {0};
  ULONG dest_ip;
  uint32_t port = APP_TCP_COMMAND_UDP_DEFAULT_PORT;
  uint32_t duration_ms = APP_TCP_COMMAND_UDP_DEFAULT_MS;
  uint32_t payload_size = APP_TCP_COMMAND_UDP_DEFAULT_SIZE;
  ULONG start_tick;
  ULONG duration_ticks;
  ULONG elapsed_ticks;
  ULONG elapsed_ms;
  uint32_t seq = 0U;
  uint32_t sent_packets = 0U;
  uint32_t sent_bytes = 0U;
  uint32_t send_errors = 0U;
  UINT status;
  char line[240];
  int len;

  cursor = AppTcpCommand_SkipSpaces(cursor);
  if (AppTcpCommand_ParseIPv4(&cursor, &dest_ip) != NX_SUCCESS)
  {
    return AppTcpCommand_SendText("ERR usage: UDPTHR <PC_IP> [port] [ms] [payload]\r\n");
  }

  cursor = AppTcpCommand_SkipSpaces(cursor);
  if (*cursor != '\0')
  {
    if (AppTcpCommand_ParseU32(&cursor, &port) != NX_SUCCESS)
    {
      return AppTcpCommand_SendText("ERR UDPTHR port\r\n");
    }
  }

  cursor = AppTcpCommand_SkipSpaces(cursor);
  if (*cursor != '\0')
  {
    if (AppTcpCommand_ParseU32(&cursor, &duration_ms) != NX_SUCCESS)
    {
      return AppTcpCommand_SendText("ERR UDPTHR duration\r\n");
    }
  }

  cursor = AppTcpCommand_SkipSpaces(cursor);
  if (*cursor != '\0')
  {
    if (AppTcpCommand_ParseU32(&cursor, &payload_size) != NX_SUCCESS)
    {
      return AppTcpCommand_SendText("ERR UDPTHR payload\r\n");
    }
  }

  if (port > 65535U)
  {
    return AppTcpCommand_SendText("ERR UDPTHR port range\r\n");
  }
  if (duration_ms == 0U)
  {
    duration_ms = APP_TCP_COMMAND_UDP_DEFAULT_MS;
  }
  if (duration_ms > APP_TCP_COMMAND_UDP_MAX_MS)
  {
    duration_ms = APP_TCP_COMMAND_UDP_MAX_MS;
  }
  if (payload_size < APP_TCP_COMMAND_UDP_MIN_SIZE)
  {
    payload_size = APP_TCP_COMMAND_UDP_MIN_SIZE;
  }
  if (payload_size > APP_TCP_COMMAND_UDP_MAX_SIZE)
  {
    payload_size = APP_TCP_COMMAND_UDP_MAX_SIZE;
  }

  status = nx_udp_socket_create(TcpCommandIp,
                                &TcpCommandUdpThroughputSocket,
                                "UDP throughput",
                                NX_IP_NORMAL,
                                NX_DONT_FRAGMENT,
                                NX_IP_TIME_TO_LIVE,
                                0U);
  if (status != NX_SUCCESS)
  {
    App_PrintHex32("UDPTHR socket create failed: ", status);
    return AppTcpCommand_SendText("ERR UDPTHR socket create\r\n");
  }

  status = nx_udp_socket_bind(&TcpCommandUdpThroughputSocket, NX_ANY_PORT, NX_WAIT_FOREVER);
  if (status != NX_SUCCESS)
  {
    (void)nx_udp_socket_delete(&TcpCommandUdpThroughputSocket);
    App_PrintHex32("UDPTHR bind failed: ", status);
    return AppTcpCommand_SendText("ERR UDPTHR bind\r\n");
  }

  len = snprintf(line,
                 sizeof(line),
                 "OK UDPTHR START FILL=%s PORT=%lu MS=%lu PAYLOAD=%lu\r\n",
                 (fill_payload != 0U) ? "pattern" : "none",
                 (unsigned long)port,
                 (unsigned long)duration_ms,
                 (unsigned long)payload_size);
  if ((len > 0) && ((uint32_t)len < sizeof(line)))
  {
    (void)AppTcpCommand_SendText(line);
  }

  duration_ticks = AppTcpCommand_MsToTicks(duration_ms);
  start_tick = tx_time_get();
  do
  {
    NX_PACKET *packet_ptr;
    UCHAR *payload;
    ULONG tick;

    tick = tx_time_get();
    status = nx_packet_allocate(TcpCommandPacketPool, &packet_ptr, NX_UDP_PACKET, NX_WAIT_FOREVER);
    stats.alloc_ticks += tx_time_get() - tick;
    if (status == NX_SUCCESS)
    {
      tick = tx_time_get();
      status = (fill_payload != 0U) ?
        AppTcpCommand_PreparePacketPayload(packet_ptr, payload_size, (uint8_t)(seq >> 8)) :
        AppTcpCommand_ReservePacketPayload(packet_ptr, payload_size);
      stats.fill_ticks += tx_time_get() - tick;
      if (status == NX_SUCCESS)
      {
        payload = packet_ptr->nx_packet_prepend_ptr;
        payload[0] = (uint8_t)'N';
        payload[1] = (uint8_t)'6';
        payload[2] = (uint8_t)'T';
        payload[3] = (uint8_t)'P';
        AppTcpCommand_WriteLE32(&payload[4], seq);
        AppTcpCommand_WriteLE32(&payload[8], (uint32_t)tx_time_get());
        AppTcpCommand_WriteLE16(&payload[12], (uint16_t)payload_size);
        AppTcpCommand_WriteLE16(&payload[14], APP_TCP_COMMAND_UDP_HEADER_SIZE);

        tick = tx_time_get();
        status = nx_udp_socket_send(&TcpCommandUdpThroughputSocket,
                                    packet_ptr,
                                    dest_ip,
                                    (UINT)port);
        stats.send_ticks += tx_time_get() - tick;
        if (status == NX_SUCCESS)
        {
          packet_ptr = NX_NULL;
          sent_packets++;
          sent_bytes += payload_size;
          stats.packets++;
        }
      }

      if (packet_ptr != NX_NULL)
      {
        (void)nx_packet_release(packet_ptr);
      }
    }

    if (status != NX_SUCCESS)
    {
      send_errors++;
    }

    seq++;
    elapsed_ticks = tx_time_get() - start_tick;
  } while (elapsed_ticks < duration_ticks);

  elapsed_ticks = tx_time_get() - start_tick;
  elapsed_ms = (elapsed_ticks * 1000UL) / TX_TIMER_TICKS_PER_SECOND;
  if (elapsed_ms == 0UL)
  {
    elapsed_ms = 1UL;
  }

  (void)nx_udp_socket_unbind(&TcpCommandUdpThroughputSocket);
  (void)nx_udp_socket_delete(&TcpCommandUdpThroughputSocket);

  len = snprintf(line,
                 sizeof(line),
                 "END UDPTHR FILL=%s PACKETS=%lu BYTES=%lu ERR=%lu MS=%lu ALLOC_MS=%lu FILL_MS=%lu SEND_MS=%lu\r\n",
                 (fill_payload != 0U) ? "pattern" : "none",
                 (unsigned long)sent_packets,
                 (unsigned long)sent_bytes,
                 (unsigned long)send_errors,
                 (unsigned long)elapsed_ms,
                 (unsigned long)AppTcpCommand_TicksToMs(stats.alloc_ticks),
                 (unsigned long)AppTcpCommand_TicksToMs(stats.fill_ticks),
                 (unsigned long)AppTcpCommand_TicksToMs(stats.send_ticks));
  if ((len <= 0) || ((uint32_t)len >= sizeof(line)))
  {
    return NX_NOT_SUCCESSFUL;
  }

  return AppTcpCommand_SendText(line);
}

static UINT AppTcpCommand_Process(char *request)
{
  char *command = AppTcpCommand_Trim(request);
  const char *args;

  if ((command[0] == '\0') || AppTcpCommand_Equals(command, "HELP") || AppTcpCommand_Equals(command, "?"))
  {
    return AppTcpCommand_SendText("OK commands: PING INFO STAT TCPTHR TCPTHRZ UDPTHR UDPTHRZ ADRAW ADGET IRPROBE IRVSYNC IRIMG IRTEMP IRDUMP IRCAPIMG IRCAPTEMP IRGETIMG IRGETTEMP IRGETIMGBASE IRGETTEMPBASE IRFASTIMG IRFASTTEMP HELP QUIT\r\n");
  }

  if (AppTcpCommand_Equals(command, "PING"))
  {
    return AppTcpCommand_SendText("PONG\r\n");
  }

  if (AppTcpCommand_Equals(command, "INFO"))
  {
    return AppTcpCommand_SendText("STM32N6 NetX command server\r\nIP=192.168.1.50\r\nUDP_ECHO=5005\r\nTCP_CMD=5000\r\nTCPTHR/TCPTHRZ=[MiB]\r\nUDPTHR/UDPTHRZ=<PC_IP> [port] [ms] [payload]\r\n");
  }

  if (AppTcpCommand_Equals(command, "STAT"))
  {
    return AppTcpCommand_SendStatus();
  }

  if (AppTcpCommand_MatchCommand(command, "TCPTHRZ", &args) == NX_TRUE)
  {
    return AppTcpCommand_SendTcpThroughput(args, 0U);
  }

  if (AppTcpCommand_MatchCommand(command, "TCPTHR", &args) == NX_TRUE)
  {
    return AppTcpCommand_SendTcpThroughput(args, 1U);
  }

  if (AppTcpCommand_MatchCommand(command, "UDPTHRZ", &args) == NX_TRUE)
  {
    return AppTcpCommand_SendUdpThroughput(args, 0U);
  }

  if (AppTcpCommand_MatchCommand(command, "UDPTHR", &args) == NX_TRUE)
  {
    return AppTcpCommand_SendUdpThroughput(args, 1U);
  }

  if (AppTcpCommand_Equals(command, "ADRAW") || AppTcpCommand_Equals(command, "AD7606 RAW"))
  {
    AD7606_SPI4_RequestRawDump();
    return AppTcpCommand_SendText("OK AD7606 raw dump armed\r\n");
  }

  if (AppTcpCommand_Equals(command, "ADGET") ||
      AppTcpCommand_Equals(command, "AD7606GET") ||
      AppTcpCommand_Equals(command, "ADNET"))
  {
    return AppTcpCommand_SendAD7606Binary();
  }

  if (AppTcpCommand_Equals(command, "IRPROBE"))
  {
    return AppTcpCommand_SendTiny1CResult("IRPROBE", (uint8_t)'i');
  }

  if (AppTcpCommand_Equals(command, "IRVSYNC"))
  {
    return AppTcpCommand_SendTiny1CResult("IRVSYNC", (uint8_t)'v');
  }

  if (AppTcpCommand_Equals(command, "IRIMG") || AppTcpCommand_Equals(command, "IRIMAGE"))
  {
    return AppTcpCommand_SendTiny1CResult("IRIMG", (uint8_t)'s');
  }

  if (AppTcpCommand_Equals(command, "IRTEMP"))
  {
    return AppTcpCommand_SendTiny1CResult("IRTEMP", (uint8_t)'t');
  }

  if (AppTcpCommand_Equals(command, "IRDUMP") || AppTcpCommand_Equals(command, "IRBIN"))
  {
    return AppTcpCommand_SendTiny1CResult("IRDUMP", (uint8_t)'b');
  }

  if (AppTcpCommand_Equals(command, "IRCAPIMG") || AppTcpCommand_Equals(command, "IRCAPIMAGE"))
  {
    return AppTcpCommand_SendTiny1CCaptureDumpResult("IRCAPIMG", (uint8_t)'s');
  }

  if (AppTcpCommand_Equals(command, "IRCAPTEMP"))
  {
    return AppTcpCommand_SendTiny1CCaptureDumpResult("IRCAPTEMP", (uint8_t)'t');
  }

  if (AppTcpCommand_Equals(command, "IRGETIMG") ||
      AppTcpCommand_Equals(command, "IRGETIMAGE") ||
      AppTcpCommand_Equals(command, "IRNETIMG"))
  {
    return AppTcpCommand_SendTiny1CBinary("image", TINY1C_CMD_IMAGE, 0U);
  }

  if (AppTcpCommand_Equals(command, "IRGETTEMP") || AppTcpCommand_Equals(command, "IRNETTEMP"))
  {
    return AppTcpCommand_SendTiny1CBinary("temp", TINY1C_CMD_TEMP, 0U);
  }

  if (AppTcpCommand_Equals(command, "IRGETIMGBASE") ||
      AppTcpCommand_Equals(command, "IRGETIMAGEBASE") ||
      AppTcpCommand_Equals(command, "IRNETIMGBASE"))
  {
    return AppTcpCommand_SendTiny1CBinary("image", TINY1C_CMD_IMAGE, 1U);
  }

  if (AppTcpCommand_Equals(command, "IRGETTEMPBASE") || AppTcpCommand_Equals(command, "IRNETTEMPBASE"))
  {
    return AppTcpCommand_SendTiny1CBinary("temp", TINY1C_CMD_TEMP, 1U);
  }

  if (AppTcpCommand_Equals(command, "IRFASTIMG"))
  {
    return AppTcpCommand_SendTiny1CResult("IRFASTIMG", (uint8_t)'k');
  }

  if (AppTcpCommand_Equals(command, "IRFASTTEMP"))
  {
    return AppTcpCommand_SendTiny1CResult("IRFASTTEMP", (uint8_t)'l');
  }

  if (AppTcpCommand_Equals(command, "QUIT") || AppTcpCommand_Equals(command, "EXIT"))
  {
    (void)AppTcpCommand_SendText("BYE\r\n");
    return NX_NOT_CONNECTED;
  }

  return AppTcpCommand_SendText("ERR unknown command\r\n");
}

static void AppTcpCommand_CloseConnection(void)
{
  (void)nx_tcp_socket_disconnect(&TcpCommandSocket, NX_NO_WAIT);
  (void)nx_tcp_server_socket_unaccept(&TcpCommandSocket);
}

static VOID AppTcpCommand_ThreadEntry(ULONG thread_input)
{
  NX_PACKET *packet_ptr;
  UINT status;
  char buffer[APP_TCP_COMMAND_RX_BUFFER_SIZE];
  ULONG copied;

  (void)thread_input;

  status = nx_tcp_server_socket_listen(TcpCommandIp,
                                       APP_TCP_COMMAND_PORT,
                                       &TcpCommandSocket,
                                       APP_TCP_COMMAND_LISTEN_QUEUE,
                                       NX_NULL);
  if (status != NX_SUCCESS)
  {
    App_PrintHex32("TCP cmd listen failed: ", status);
    return;
  }

  App_Print("TCP cmd: 192.168.1.50:5000\r\n");

  for (;;)
  {
    status = nx_tcp_server_socket_accept(&TcpCommandSocket, NX_WAIT_FOREVER);
    if (status != NX_SUCCESS)
    {
      App_PrintHex32("TCP cmd accept failed: ", status);
      tx_thread_sleep(NX_IP_PERIODIC_RATE);
      continue;
    }

    TcpCommandConnections++;
    if ((TcpCommandConnections % APP_TCP_COMMAND_LOG_INTERVAL) == 0UL)
    {
      App_PrintHex32("TCP cmd connections: ", TcpCommandConnections);
    }

    for (;;)
    {
      packet_ptr = NX_NULL;
      status = nx_tcp_socket_receive(&TcpCommandSocket, &packet_ptr, NX_WAIT_FOREVER);
      if (status != NX_SUCCESS)
      {
        break;
      }

      copied = 0UL;
      status = nx_packet_data_extract_offset(packet_ptr,
                                             0,
                                             buffer,
                                             sizeof(buffer) - 1U,
                                             &copied);
      (void)nx_packet_release(packet_ptr);

      if (status != NX_SUCCESS)
      {
        App_PrintHex32("TCP cmd extract failed: ", status);
        break;
      }

      buffer[copied] = '\0';
      TcpCommandRxPackets++;

      status = AppTcpCommand_Process(buffer);
      if (status != NX_SUCCESS)
      {
        break;
      }
    }

    AppTcpCommand_CloseConnection();

    status = nx_tcp_server_socket_relisten(TcpCommandIp, APP_TCP_COMMAND_PORT, &TcpCommandSocket);
    if (status != NX_SUCCESS)
    {
      App_PrintHex32("TCP cmd relisten failed: ", status);
      tx_thread_sleep(NX_IP_PERIODIC_RATE);
      (void)nx_tcp_server_socket_listen(TcpCommandIp,
                                        APP_TCP_COMMAND_PORT,
                                        &TcpCommandSocket,
                                        APP_TCP_COMMAND_LISTEN_QUEUE,
                                        NX_NULL);
    }
  }
}

UINT AppTcpCommand_Start(NX_IP *ip_ptr, NX_PACKET_POOL *packet_pool, TX_BYTE_POOL *byte_pool)
{
  UCHAR *thread_stack;
  UINT status;

  if ((ip_ptr == NX_NULL) || (packet_pool == NX_NULL) || (byte_pool == TX_NULL))
  {
    return NX_PTR_ERROR;
  }

  TcpCommandIp = ip_ptr;
  TcpCommandPacketPool = packet_pool;
  TcpCommandBytePool = byte_pool;

  status = nx_tcp_enable(ip_ptr);
  if ((status != NX_SUCCESS) && (status != NX_ALREADY_ENABLED))
  {
    App_PrintHex32("nx_tcp_enable failed: ", status);
    return status;
  }

  status = nx_tcp_socket_create(ip_ptr,
                                &TcpCommandSocket,
                                "TCP command",
                                NX_IP_NORMAL,
                                NX_FRAGMENT_OKAY,
                                NX_IP_TIME_TO_LIVE,
                                APP_TCP_COMMAND_WINDOW_SIZE,
                                NX_NULL,
                                NX_NULL);
  if (status != NX_SUCCESS)
  {
    App_PrintHex32("nx_tcp_socket_create failed: ", status);
    return status;
  }

  status = AppTcpCommand_ByteAllocate(byte_pool, &thread_stack, APP_TCP_COMMAND_THREAD_STACK_SIZE);
  if (status != NX_SUCCESS)
  {
    return status;
  }

  status = tx_thread_create(&TcpCommandThread,
                            "TCP command",
                            AppTcpCommand_ThreadEntry,
                            0,
                            thread_stack,
                            APP_TCP_COMMAND_THREAD_STACK_SIZE,
                            APP_TCP_COMMAND_THREAD_PRIORITY,
                            APP_TCP_COMMAND_THREAD_PRIORITY,
                            TX_NO_TIME_SLICE,
                            TX_AUTO_START);
  if (status != TX_SUCCESS)
  {
    App_PrintHex32("TCP cmd thread failed: ", status);
    return NX_NOT_SUCCESSFUL;
  }

  return NX_SUCCESS;
}
