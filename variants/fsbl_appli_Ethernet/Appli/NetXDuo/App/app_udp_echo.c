/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_udp_echo.c
  * @brief   Minimal NetX Duo UDP echo service.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "app_udp_echo.h"
#include "app_console.h"
#include <string.h>

#define APP_UDP_ECHO_THREAD_STACK_SIZE 2048U
#define APP_UDP_ECHO_THREAD_PRIORITY   12U
#define APP_UDP_ECHO_QUEUE_DEPTH       8U
#define APP_UDP_ECHO_LOG_INTERVAL      16UL

static NX_UDP_SOCKET UdpEchoSocket;
static TX_THREAD UdpEchoThread;
static ULONG UdpEchoCount;

static UINT AppUdpEcho_ByteAllocate(TX_BYTE_POOL *byte_pool, UCHAR **memory, ULONG size)
{
  UINT status;

  if ((byte_pool == NX_NULL) || (memory == NX_NULL))
  {
    return NX_PTR_ERROR;
  }

  status = tx_byte_allocate(byte_pool, (VOID **)memory, size, TX_NO_WAIT);
  if (status != TX_SUCCESS)
  {
    App_PrintHex32("UDP echo stack alloc failed: ", status);
    return NX_NOT_SUCCESSFUL;
  }

  memset(*memory, 0, size);
  return NX_SUCCESS;
}

static VOID AppUdpEcho_ThreadEntry(ULONG thread_input)
{
  NX_PACKET *packet_ptr;
  ULONG source_ip;
  UINT source_port;
  UINT status;

  (void)thread_input;

  status = nx_udp_socket_bind(&UdpEchoSocket, APP_UDP_ECHO_PORT, NX_NO_WAIT);
  if (status != NX_SUCCESS)
  {
    App_PrintHex32("nx_udp_socket_bind failed: ", status);
    return;
  }

  App_Print("UDP echo: 192.168.1.50:5005\r\n");

  for (;;)
  {
    packet_ptr = NX_NULL;
    status = nx_udp_socket_receive(&UdpEchoSocket, &packet_ptr, NX_WAIT_FOREVER);
    if (status != NX_SUCCESS)
    {
      App_PrintHex32("UDP echo receive failed: ", status);
      tx_thread_sleep(NX_IP_PERIODIC_RATE);
      continue;
    }

    status = nx_udp_source_extract(packet_ptr, &source_ip, &source_port);
    if (status == NX_SUCCESS)
    {
      status = nx_udp_socket_send(&UdpEchoSocket, packet_ptr, source_ip, source_port);
      if (status == NX_SUCCESS)
      {
        packet_ptr = NX_NULL;
        UdpEchoCount++;
        if ((UdpEchoCount % APP_UDP_ECHO_LOG_INTERVAL) == 0UL)
        {
          App_PrintHex32("UDP echo count: ", UdpEchoCount);
        }
      }
      else
      {
        App_PrintHex32("UDP echo send failed: ", status);
      }
    }
    else
    {
      App_PrintHex32("UDP echo source failed: ", status);
    }

    if (packet_ptr != NX_NULL)
    {
      (void)nx_packet_release(packet_ptr);
    }
  }
}

UINT AppUdpEcho_Start(NX_IP *ip_ptr, TX_BYTE_POOL *byte_pool)
{
  UCHAR *thread_stack;
  UINT status;

  if (ip_ptr == NX_NULL)
  {
    return NX_PTR_ERROR;
  }

  status = nx_udp_enable(ip_ptr);
  if (status != NX_SUCCESS)
  {
    App_PrintHex32("nx_udp_enable failed: ", status);
    return status;
  }

  status = nx_udp_socket_create(ip_ptr,
                                &UdpEchoSocket,
                                "UDP echo",
                                NX_IP_NORMAL,
                                NX_FRAGMENT_OKAY,
                                NX_IP_TIME_TO_LIVE,
                                APP_UDP_ECHO_QUEUE_DEPTH);
  if (status != NX_SUCCESS)
  {
    App_PrintHex32("nx_udp_socket_create failed: ", status);
    return status;
  }

  status = AppUdpEcho_ByteAllocate(byte_pool, &thread_stack, APP_UDP_ECHO_THREAD_STACK_SIZE);
  if (status != NX_SUCCESS)
  {
    return status;
  }

  status = tx_thread_create(&UdpEchoThread,
                            "UDP echo",
                            AppUdpEcho_ThreadEntry,
                            0,
                            thread_stack,
                            APP_UDP_ECHO_THREAD_STACK_SIZE,
                            APP_UDP_ECHO_THREAD_PRIORITY,
                            APP_UDP_ECHO_THREAD_PRIORITY,
                            TX_NO_TIME_SLICE,
                            TX_AUTO_START);
  if (status != TX_SUCCESS)
  {
    App_PrintHex32("UDP echo thread failed: ", status);
    return NX_NOT_SUCCESSFUL;
  }

  return NX_SUCCESS;
}
