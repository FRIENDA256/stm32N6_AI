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
#include <string.h>

#define APP_TCP_COMMAND_THREAD_STACK_SIZE 2048U
#define APP_TCP_COMMAND_THREAD_PRIORITY   13U
#define APP_TCP_COMMAND_LISTEN_QUEUE      1U
#define APP_TCP_COMMAND_WINDOW_SIZE       2048UL
#define APP_TCP_COMMAND_RX_BUFFER_SIZE    128U
#define APP_TCP_COMMAND_LOG_INTERVAL      8UL

static NX_TCP_SOCKET TcpCommandSocket;
static TX_THREAD TcpCommandThread;
static NX_IP *TcpCommandIp;
static NX_PACKET_POOL *TcpCommandPacketPool;
static ULONG TcpCommandConnections;
static ULONG TcpCommandRxPackets;

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

static UINT AppTcpCommand_Process(char *request)
{
  char *command = AppTcpCommand_Trim(request);

  if ((command[0] == '\0') || AppTcpCommand_Equals(command, "HELP") || AppTcpCommand_Equals(command, "?"))
  {
    return AppTcpCommand_SendText("OK commands: PING INFO STAT ADRAW HELP QUIT\r\n");
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
