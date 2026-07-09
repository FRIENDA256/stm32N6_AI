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
#define APP_TCP_COMMAND_WINDOW_SIZE       2048UL
#define APP_TCP_COMMAND_RX_BUFFER_SIZE    128U
#define APP_TCP_COMMAND_TX_CHUNK_SIZE     1024UL
#define APP_TCP_COMMAND_LOG_INTERVAL      8UL
#define APP_TCP_COMMAND_AD_WAIT_MS        3000U
#define APP_TCP_COMMAND_AD_POLL_MS        20U
#define APP_TCP_COMMAND_IR_PAUSE_AD7606   1U
#define APP_TCP_COMMAND_IR_AD_WAIT_MS     200U
#define APP_TCP_COMMAND_IR_AD_POLL_MS     2U

static NX_TCP_SOCKET TcpCommandSocket;
static TX_THREAD TcpCommandThread;
static NX_IP *TcpCommandIp;
static NX_PACKET_POOL *TcpCommandPacketPool;
static ULONG TcpCommandConnections;
static ULONG TcpCommandRxPackets;
static uint8_t TcpCommandAdFrame[AD7606_SPI4_MAX_FRAME_SIZE];

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

  status = nx_packet_data_append(packet_ptr,
                                 (VOID *)text,
                                 (ULONG)strlen(text),
                                 TcpCommandPacketPool,
                                 NX_WAIT_FOREVER);
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

    status = nx_packet_data_append(packet_ptr,
                                   (VOID *)&data[offset],
                                   chunk,
                                   TcpCommandPacketPool,
                                   NX_WAIT_FOREVER);
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
  char line[96];
  ULONG pos = 0UL;

  AppTcpCommand_AppendText(line, &pos, sizeof(line), "OK connections=");
  AppTcpCommand_AppendHex32(line, &pos, sizeof(line), TcpCommandConnections);
  AppTcpCommand_AppendText(line, &pos, sizeof(line), " rx_packets=");
  AppTcpCommand_AppendHex32(line, &pos, sizeof(line), TcpCommandRxPackets);
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

static UINT AppTcpCommand_Process(char *request)
{
  char *command = AppTcpCommand_Trim(request);

  if ((command[0] == '\0') || AppTcpCommand_Equals(command, "HELP") || AppTcpCommand_Equals(command, "?"))
  {
    return AppTcpCommand_SendText("OK commands: PING INFO STAT ADRAW ADGET IRPROBE IRVSYNC IRIMG IRTEMP IRDUMP IRCAPIMG IRCAPTEMP IRGETIMG IRGETTEMP IRGETIMGBASE IRGETTEMPBASE IRFASTIMG IRFASTTEMP HELP QUIT\r\n");
  }

  if (AppTcpCommand_Equals(command, "PING"))
  {
    return AppTcpCommand_SendText("PONG\r\n");
  }

  if (AppTcpCommand_Equals(command, "INFO"))
  {
    return AppTcpCommand_SendText("STM32N6 NetX command server\r\nIP=192.168.1.50\r\nUDP_ECHO=5005\r\nTCP_CMD=5000\r\n");
  }

  if (AppTcpCommand_Equals(command, "STAT"))
  {
    return AppTcpCommand_SendStatus();
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

  if ((ip_ptr == NX_NULL) || (packet_pool == NX_NULL))
  {
    return NX_PTR_ERROR;
  }

  TcpCommandIp = ip_ptr;
  TcpCommandPacketPool = packet_pool;

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
