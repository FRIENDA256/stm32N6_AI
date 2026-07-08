/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_netxduo.c
  * @author  MCD Application Team
  * @brief   NetXDuo applicative file
  ******************************************************************************
    * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "app_netxduo.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "app_console.h"
#include "app_tcp_command.h"
#include "app_udp_echo.h"
#include "main.h"
#include "eth_diagnostics.h"
#include "gpio.h"
#include <string.h>

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define APP_NETX_IP_ADDRESS              IP_ADDRESS(192, 168, 1, 50)
#define APP_NETX_NET_MASK                IP_ADDRESS(255, 255, 255, 0)
#define APP_NETX_PACKET_PAYLOAD_SIZE     1536U
#define APP_NETX_PACKET_POOL_SIZE        (24U * (APP_NETX_PACKET_PAYLOAD_SIZE + sizeof(NX_PACKET)))
#define APP_NETX_IP_STACK_SIZE           4096U
#define APP_NETX_ARP_CACHE_SIZE          2048U
#define APP_NETX_STATUS_THREAD_STACK     2048U
#define APP_NETX_LINK_THREAD_STACK       2048U
#define APP_NETX_IP_THREAD_PRIORITY      5U
#define APP_NETX_STATUS_THREAD_PRIORITY  10U
#define APP_NETX_LINK_THREAD_PRIORITY    11U
#define APP_NETX_LINK_CHECK_PERIOD       (1U * NX_IP_PERIODIC_RATE)
#define APP_NETX_STATS_PERIOD            5U
#define APP_NETX_ENABLE_PERIODIC_STATS   0U
#define APP_NETX_VERBOSE_DIAG            0U
#define APP_NETX_LINE_MAX                160U

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */
static NX_PACKET_POOL NetXDuoPacketPool;
static NX_IP NetXDuoIpInstance;
static TX_THREAD NetXDuoStatusThread;
static TX_THREAD NetXDuoLinkThread;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */
#if (APP_NETX_ENABLE_PERIODIC_STATS == 1U)
static void NetXDuo_AppendText(char *line, uint32_t *pos, const char *text);
static void NetXDuo_AppendHex32(char *line, uint32_t *pos, ULONG value);
static void NetXDuo_PrintStats(void);
#endif
static UINT NetXDuo_ByteAllocate(TX_BYTE_POOL *byte_pool, UCHAR **memory, ULONG size);
static VOID NetXDuo_StatusThreadEntry(ULONG thread_input);
static VOID NetXDuo_LinkThreadEntry(ULONG thread_input);

/* USER CODE END PFP */

/**
  * @brief  Application NetXDuo Initialization.
  * @param memory_ptr: memory pointer
  * @retval int
  */
UINT MX_NetXDuo_Init(VOID *memory_ptr)
{
  UINT ret = NX_SUCCESS;
  TX_BYTE_POOL *byte_pool = (TX_BYTE_POOL*)memory_ptr;

  /* USER CODE BEGIN MX_NetXDuo_MEM_POOL */
  UCHAR *packet_pool_memory;
  UCHAR *ip_stack_memory;
  UCHAR *arp_cache_memory;
  UCHAR *status_thread_stack;
  UCHAR *link_thread_stack;

  nx_system_initialize();

  ret = NetXDuo_ByteAllocate(byte_pool, &packet_pool_memory, APP_NETX_PACKET_POOL_SIZE);
  if (ret != NX_SUCCESS)
  {
    return ret;
  }

  ret = nx_packet_pool_create(&NetXDuoPacketPool,
                              "NetX Duo packet pool",
                              APP_NETX_PACKET_PAYLOAD_SIZE,
                              packet_pool_memory,
                              APP_NETX_PACKET_POOL_SIZE);
  if (ret != NX_SUCCESS)
  {
    App_PrintHex32("nx_packet_pool_create failed: ", ret);
    return ret;
  }

  ret = NetXDuo_ByteAllocate(byte_pool, &ip_stack_memory, APP_NETX_IP_STACK_SIZE);
  if (ret != NX_SUCCESS)
  {
    return ret;
  }

  ret = nx_ip_create(&NetXDuoIpInstance,
                     "NetX Duo IP",
                     APP_NETX_IP_ADDRESS,
                     APP_NETX_NET_MASK,
                     &NetXDuoPacketPool,
                     nx_stm32_eth_driver,
                     ip_stack_memory,
                     APP_NETX_IP_STACK_SIZE,
                     APP_NETX_IP_THREAD_PRIORITY);
  if (ret != NX_SUCCESS)
  {
    App_PrintHex32("nx_ip_create failed: ", ret);
    return ret;
  }

  ret = NetXDuo_ByteAllocate(byte_pool, &arp_cache_memory, APP_NETX_ARP_CACHE_SIZE);
  if (ret != NX_SUCCESS)
  {
    return ret;
  }

  ret = nx_arp_enable(&NetXDuoIpInstance, arp_cache_memory, APP_NETX_ARP_CACHE_SIZE);
  if (ret != NX_SUCCESS)
  {
    App_PrintHex32("nx_arp_enable failed: ", ret);
    return ret;
  }

  ret = nx_icmp_enable(&NetXDuoIpInstance);
  if (ret != NX_SUCCESS)
  {
    App_PrintHex32("nx_icmp_enable failed: ", ret);
    return ret;
  }

  ret = AppUdpEcho_Start(&NetXDuoIpInstance, byte_pool);
  if (ret != NX_SUCCESS)
  {
    App_PrintHex32("UDP echo start failed: ", ret);
    return ret;
  }

  ret = AppTcpCommand_Start(&NetXDuoIpInstance, &NetXDuoPacketPool, byte_pool);
  if (ret != NX_SUCCESS)
  {
    App_PrintHex32("TCP cmd start failed: ", ret);
    return ret;
  }

  ret = NetXDuo_ByteAllocate(byte_pool, &status_thread_stack, APP_NETX_STATUS_THREAD_STACK);
  if (ret != NX_SUCCESS)
  {
    return ret;
  }

  ret = tx_thread_create(&NetXDuoStatusThread,
                         "NetX Duo status",
                         NetXDuo_StatusThreadEntry,
                         0,
                         status_thread_stack,
                         APP_NETX_STATUS_THREAD_STACK,
                         APP_NETX_STATUS_THREAD_PRIORITY,
                         APP_NETX_STATUS_THREAD_PRIORITY,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
  if (ret != TX_SUCCESS)
  {
    App_PrintHex32("tx_thread_create failed: ", ret);
    return NX_NOT_SUCCESSFUL;
  }

  ret = NetXDuo_ByteAllocate(byte_pool, &link_thread_stack, APP_NETX_LINK_THREAD_STACK);
  if (ret != NX_SUCCESS)
  {
    return ret;
  }

  ret = tx_thread_create(&NetXDuoLinkThread,
                         "NetX Duo link",
                         NetXDuo_LinkThreadEntry,
                         0,
                         link_thread_stack,
                         APP_NETX_LINK_THREAD_STACK,
                         APP_NETX_LINK_THREAD_PRIORITY,
                         APP_NETX_LINK_THREAD_PRIORITY,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
  if (ret != TX_SUCCESS)
  {
    App_PrintHex32("tx_link_thread_create failed: ", ret);
    return NX_NOT_SUCCESSFUL;
  }

  App_Print("NetX init OK\r\n");
  /* USER CODE END MX_NetXDuo_MEM_POOL */

  /* USER CODE BEGIN MX_NetXDuo_Init */
  /* USER CODE END MX_NetXDuo_Init */

  return ret;
}

  /* USER CODE BEGIN 0 */
#if (APP_NETX_ENABLE_PERIODIC_STATS == 1U)
static void NetXDuo_AppendText(char *line, uint32_t *pos, const char *text)
{
  while ((text != NULL) && (*text != '\0') && (*pos < (APP_NETX_LINE_MAX - 1U)))
  {
    line[*pos] = *text;
    (*pos)++;
    text++;
  }
}

static void NetXDuo_AppendHex32(char *line, uint32_t *pos, ULONG value)
{
  static const char hex[] = "0123456789ABCDEF";

  if (*pos < (APP_NETX_LINE_MAX - 3U))
  {
    line[*pos] = '0';
    (*pos)++;
    line[*pos] = 'x';
    (*pos)++;
  }

  for (uint32_t nibble = 0U; nibble < 8U; nibble++)
  {
    if (*pos >= (APP_NETX_LINE_MAX - 1U))
    {
      break;
    }
    uint32_t shift = 28U - (nibble * 4U);
    line[*pos] = hex[(value >> shift) & 0xFU];
    (*pos)++;
  }
}
#endif

static UINT NetXDuo_ByteAllocate(TX_BYTE_POOL *byte_pool, UCHAR **memory, ULONG size)
{
  UINT status;

  if ((byte_pool == NULL) || (memory == NULL))
  {
    return NX_PTR_ERROR;
  }

  status = tx_byte_allocate(byte_pool, (VOID **)memory, size, TX_NO_WAIT);
  if (status != TX_SUCCESS)
  {
    App_PrintHex32("NetX byte allocate failed: ", status);
    return NX_NOT_SUCCESSFUL;
  }

  memset(*memory, 0, size);
  return NX_SUCCESS;
}

#if (APP_NETX_ENABLE_PERIODIC_STATS == 1U)
static void NetXDuo_PrintStats(void)
{
  ULONG ip_tx = 0U;
  ULONG ip_rx = 0U;
  ULONG ip_invalid = 0U;
  ULONG ip_rx_drop = 0U;
  ULONG ip_rx_checksum = 0U;
  ULONG ip_tx_drop = 0U;
  ULONG arp_req_rx = 0U;
  ULONG arp_resp_tx = 0U;
  ULONG arp_invalid = 0U;
  ULONG icmp_unhandled = 0U;
  ULONG icmp_checksum = 0U;
  char line[APP_NETX_LINE_MAX];
  uint32_t pos = 0U;

  (void)nx_ip_info_get(&NetXDuoIpInstance,
                       &ip_tx,
                       NX_NULL,
                       &ip_rx,
                       NX_NULL,
                       &ip_invalid,
                       &ip_rx_drop,
                       &ip_rx_checksum,
                       &ip_tx_drop,
                       NX_NULL,
                       NX_NULL);

  (void)nx_arp_info_get(&NetXDuoIpInstance,
                        NX_NULL,
                        &arp_req_rx,
                        &arp_resp_tx,
                        NX_NULL,
                        NX_NULL,
                        NX_NULL,
                        NX_NULL,
                        &arp_invalid);

  (void)nx_icmp_info_get(&NetXDuoIpInstance,
                         NX_NULL,
                         NX_NULL,
                         NX_NULL,
                         NX_NULL,
                         &icmp_checksum,
                         &icmp_unhandled);

#if (APP_NETX_VERBOSE_DIAG == 1U)
  App_Print("NetX stats\r\n");
  App_PrintHex32("  IP tx: ", ip_tx);
  App_PrintHex32("  IP rx: ", ip_rx);
  App_PrintHex32("  IP invalid: ", ip_invalid);
  App_PrintHex32("  IP rx drop: ", ip_rx_drop);
  App_PrintHex32("  IP rx checksum: ", ip_rx_checksum);
  App_PrintHex32("  IP tx drop: ", ip_tx_drop);
  App_PrintHex32("  ARP req rx: ", arp_req_rx);
  App_PrintHex32("  ARP resp tx: ", arp_resp_tx);
  App_PrintHex32("  ARP invalid: ", arp_invalid);
  App_PrintHex32("  ICMP checksum: ", icmp_checksum);
  App_PrintHex32("  ICMP unhandled: ", icmp_unhandled);
#else
  NetXDuo_AppendText(line, &pos, "NX: ip_rx=");
  NetXDuo_AppendHex32(line, &pos, ip_rx);
  NetXDuo_AppendText(line, &pos, " ip_tx=");
  NetXDuo_AppendHex32(line, &pos, ip_tx);
  NetXDuo_AppendText(line, &pos, " arp_req=");
  NetXDuo_AppendHex32(line, &pos, arp_req_rx);
  NetXDuo_AppendText(line, &pos, " arp_resp=");
  NetXDuo_AppendHex32(line, &pos, arp_resp_tx);
  NetXDuo_AppendText(line, &pos, " drop=");
  NetXDuo_AppendHex32(line, &pos, ip_rx_drop);
  NetXDuo_AppendText(line, &pos, " icmp_unh=");
  NetXDuo_AppendHex32(line, &pos, icmp_unhandled);

  if (pos < (APP_NETX_LINE_MAX - 2U))
  {
    line[pos++] = '\r';
    line[pos++] = '\n';
  }
  line[pos] = '\0';
  App_Print(line);
#endif
}
#endif

static VOID NetXDuo_StatusThreadEntry(ULONG thread_input)
{
  ULONG actual_status = 0U;
  UINT link_was_up = NX_FALSE;
  UINT status;
#if (APP_NETX_ENABLE_PERIODIC_STATS == 1U)
  uint32_t stats_period = 0U;
#endif

  (void)thread_input;

  App_Print("IP: 192.168.1.50/24\r\n");

  for (;;)
  {
    status = nx_ip_status_check(&NetXDuoIpInstance,
                                NX_IP_LINK_ENABLED,
                                &actual_status,
                                NX_NO_WAIT);
    if (status == NX_SUCCESS)
    {
      if (link_was_up == NX_FALSE)
      {
        link_was_up = NX_TRUE;
        App_Print("NetX link: up\r\n");
      }
      HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
#if (APP_NETX_ENABLE_PERIODIC_STATS == 1U)
      stats_period++;
      if (stats_period >= APP_NETX_STATS_PERIOD)
      {
        stats_period = 0U;
        NetXDuo_PrintStats();
#if (APP_NETX_VERBOSE_DIAG == 1U)
        Ethernet_PrintRxRuntimeDebug();
#else
        Ethernet_PrintRxSummary();
#endif
      }
#endif
    }
    else
    {
      if (link_was_up == NX_TRUE)
      {
        link_was_up = NX_FALSE;
        App_Print("NetX link: down\r\n");
      }
#if (APP_NETX_ENABLE_PERIODIC_STATS == 1U)
      stats_period = 0U;
#endif
    }

    tx_thread_sleep(NX_IP_PERIODIC_RATE);
  }
}

static VOID NetXDuo_LinkThreadEntry(ULONG thread_input)
{
  ULONG actual_status = 0U;
  UINT linkdown = 1U;
  UINT status;

  (void)thread_input;

  for (;;)
  {
    status = nx_ip_interface_status_check(&NetXDuoIpInstance,
                                          0,
                                          NX_IP_LINK_ENABLED,
                                          &actual_status,
                                          10);
    if (status == NX_SUCCESS)
    {
      if (linkdown != 0U)
      {
        linkdown = 0U;
        status = nx_ip_driver_direct_command(&NetXDuoIpInstance,
                                             NX_LINK_ENABLE,
                                             &actual_status);
        if ((status != NX_SUCCESS) && (status != NX_ALREADY_ENABLED))
        {
          App_PrintHex32("NX_LINK_ENABLE failed: ", status);
        }
      }
    }
    else
    {
      if (linkdown == 0U)
      {
        linkdown = 1U;
        status = nx_ip_driver_direct_command(&NetXDuoIpInstance,
                                             NX_LINK_DISABLE,
                                             &actual_status);
        if ((status != NX_SUCCESS) && (status != NX_NOT_ENABLED))
        {
          App_PrintHex32("NX_LINK_DISABLE failed: ", status);
        }
      }
    }

    tx_thread_sleep(APP_NETX_LINK_CHECK_PERIOD);
  }
}
/* USER CODE END 0 */

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
