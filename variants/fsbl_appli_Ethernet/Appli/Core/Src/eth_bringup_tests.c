/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    eth_bringup_tests.c
  * @brief   Optional low-level Ethernet test hooks.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "eth_bringup_tests.h"
#include "app_console.h"
#include "eth.h"
#include "gpio.h"
#include "main.h"
#include "rtl8211.h"
#include <string.h>

#define BRINGUP_TEST_HEARTBEAT_PERIOD_MS 1000U

#ifndef APP_ETH_RAW_TX_TEST
#define APP_ETH_RAW_TX_TEST 0
#endif

#define RAW_TX_PHY_ADDR                 0x01U
#define RAW_TX_FRAME_LEN                60U
#define RAW_TX_ETHERTYPE_HI             0x88U
#define RAW_TX_ETHERTYPE_LO             0xB5U
#define RAW_TX_DELAY_CYCLES             1000000U
#define RAW_TX_PRINT_INTERVAL           64U
#define RAW_TX_RTL8211_PAGE_DEFAULT     0x0000U
#define RAW_TX_RTL8211_PAGE_0A43        0x0A43U
#define RAW_TX_RTL8211_PAGE_0D08        0x0D08U
#define RAW_TX_RTL8211_PAGE_0D04        0x0D04U

#if APP_ETH_RAW_TX_TEST
static uint8_t RawTxFrame[RAW_TX_FRAME_LEN] __attribute__((aligned(32)));
static ETH_BufferTypeDef RawTxBuffer;
static ETH_TxPacketConfigTypeDef RawTxConfig;

static void RawDelayCycles(uint32_t cycles)
{
  volatile uint32_t count = cycles;

  while (count > 0U)
  {
    count--;
  }
}

static HAL_StatusTypeDef RawPhyRead(uint32_t reg, uint32_t *value)
{
  return HAL_ETH_ReadPHYRegister(&heth1, RAW_TX_PHY_ADDR, reg, value);
}

static HAL_StatusTypeDef RawPhyWrite(uint32_t reg, uint32_t value)
{
  return HAL_ETH_WritePHYRegister(&heth1, RAW_TX_PHY_ADDR, reg, value);
}

static void RawPrintPhyReg(const char *label, uint32_t reg)
{
  uint32_t value = 0U;

  if (RawPhyRead(reg, &value) == HAL_OK)
  {
    App_PrintHex32(label, value);
  }
}

static void RawSelectPhyPage(uint32_t page)
{
  (void)RawPhyWrite(RTL8211_PAGSR, page);
}

static void RawEnableRtl8211Rxc(void)
{
  uint32_t value = 0U;

  RawSelectPhyPage(RAW_TX_RTL8211_PAGE_0A43);
  if (RawPhyRead(RTL8211_PHYCR2_PA43, &value) == HAL_OK)
  {
    value |= RTL8211_PHYCR2_RXC_ENABLE;
    (void)RawPhyWrite(RTL8211_PHYCR2_PA43, value);
  }
  RawSelectPhyPage(RAW_TX_RTL8211_PAGE_DEFAULT);
}

static void RawDisableRtl8211Eee(void)
{
  uint32_t value = 0U;

  RawSelectPhyPage(RAW_TX_RTL8211_PAGE_0D04);
  if (RawPhyRead(RTL8211_EEELCR_PD04, &value) == HAL_OK)
  {
    value &= (uint32_t)~(RTL8211_EEELCR_LED2_EEE_ENABLE |
                        RTL8211_EEELCR_LED1_EEE_ENABLE |
                        RTL8211_EEELCR_LED0_EEE_ENABLE);
    (void)RawPhyWrite(RTL8211_EEELCR_PD04, value);
  }

  RawSelectPhyPage(RAW_TX_RTL8211_PAGE_0D08);
  if (RawPhyRead(RTL8211_MIICR1_PD08, &value) == HAL_OK)
  {
    value |= RTL8211_MIICR1_TXDLY_ENABLE;
    (void)RawPhyWrite(RTL8211_MIICR1_PD08, value);
  }
  if (RawPhyRead(RTL8211_MIICR2_PD08, &value) == HAL_OK)
  {
    value |= RTL8211_MIICR2_RXDLY_ENABLE;
    (void)RawPhyWrite(RTL8211_MIICR2_PD08, value);
  }

  RawSelectPhyPage(RAW_TX_RTL8211_PAGE_DEFAULT);
}

static void RawLimitPhyTo100MFull(void)
{
  uint32_t value = 0U;

  RawSelectPhyPage(RAW_TX_RTL8211_PAGE_DEFAULT);

  if (RawPhyRead(RTL8211_GBCR, &value) == HAL_OK)
  {
    value &= (uint32_t)~RTL8211_GBCR_1000BT_FD;
    (void)RawPhyWrite(RTL8211_GBCR, value);
  }

  value = (uint32_t)(RTL8211_ANAR_SELECTOR_DEFAULT | RTL8211_ANAR_100BTX_FD);
  (void)RawPhyWrite(RTL8211_ANAR, value);

  if (RawPhyRead(RTL8211_BMCR, &value) == HAL_OK)
  {
    value &= (uint32_t)~(RTL8211_BMCR_SPEED_SEL_LSB |
                        RTL8211_BMCR_SPEED_SEL_MSB |
                        RTL8211_BMCR_DUPLEX_MODE);
    value |= (uint32_t)(RTL8211_BMCR_AN_EN | RTL8211_BMCR_RESTART_AN);
    (void)RawPhyWrite(RTL8211_BMCR, value);
  }
}

static void RawConfigureMac100MFull(void)
{
  ETH_MACConfigTypeDef mac_config;

  if (HAL_ETH_GetMACConfig(&heth1, &mac_config) == HAL_OK)
  {
    mac_config.Speed = ETH_SPEED_100M;
    mac_config.DuplexMode = ETH_FULLDUPLEX_MODE;
    mac_config.PortSelect = ENABLE;
    (void)HAL_ETH_SetMACConfig(&heth1, &mac_config);
  }
}

static void RawBuildFrame(uint32_t seq)
{
  static const uint8_t dst[6] = {0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU};
  static const uint8_t src[6] = {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U};
  static const char prefix[] = "STM32N6 RAW TX SEQ=00000000";
  static const char hex[] = "0123456789ABCDEF";
  uint32_t i;
  uint32_t seq_offset;

  memset(RawTxFrame, 0xA5, sizeof(RawTxFrame));
  memcpy(&RawTxFrame[0], dst, sizeof(dst));
  memcpy(&RawTxFrame[6], src, sizeof(src));
  RawTxFrame[12] = RAW_TX_ETHERTYPE_HI;
  RawTxFrame[13] = RAW_TX_ETHERTYPE_LO;
  memcpy(&RawTxFrame[14], prefix, sizeof(prefix) - 1U);

  seq_offset = 14U + (uint32_t)(sizeof(prefix) - 1U) - 8U;
  for (i = 0U; i < 8U; i++)
  {
    RawTxFrame[seq_offset + i] = (uint8_t)hex[(seq >> ((7U - i) * 4U)) & 0x0FU];
  }
}

static void RawCleanDCacheForTx(void)
{
  if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U)
  {
    SCB_CleanDCache_by_Addr((uint32_t *)RawTxFrame, sizeof(RawTxFrame));
  }
}

static void RawInitTxConfig(void)
{
  memset(&RawTxBuffer, 0, sizeof(RawTxBuffer));
  memset(&RawTxConfig, 0, sizeof(RawTxConfig));

  RawTxBuffer.buffer = RawTxFrame;
  RawTxBuffer.len = RAW_TX_FRAME_LEN;
  RawTxBuffer.next = NULL;

  RawTxConfig.TxDMACh = 0U;
  RawTxConfig.Attributes = ETH_TX_PACKETS_FEATURES_CRCPAD;
  RawTxConfig.Length = RAW_TX_FRAME_LEN;
  RawTxConfig.TxBuffer = &RawTxBuffer;
  RawTxConfig.CRCPadCtrl = ETH_CRC_PAD_INSERT;
  /*
   * Keep pData NULL in this bare-metal TX test. The NetX Ethernet driver
   * provides HAL_ETH_TxFreeCallback(), and it expects an NX_PACKET pointer.
   */
  RawTxConfig.pData = NULL;
}

static void RawPrintTxError(void)
{
  App_Print("RAW TX failed\r\n");
  App_PrintHex32("  HAL err: ", HAL_ETH_GetError(&heth1));
  App_PrintHex32("  DMA err: ", HAL_ETH_GetDMAError(&heth1));
  App_PrintHex32("  DMACSR: ", heth1.Instance->DMA_CH[0].DMACSR);
  App_PrintHex32("  MACRXTXSR: ", heth1.Instance->MACRXTXSR);
}

static void RawPrintRuntime(uint32_t seq)
{
  App_PrintHex32("RAW TX seq: ", seq);
  RawPrintPhyReg("  BMSR: ", RTL8211_BMSR);
  RawSelectPhyPage(RAW_TX_RTL8211_PAGE_0A43);
  RawPrintPhyReg("  PHYSR1: ", RTL8211_PHYSR1_PA43);
  RawSelectPhyPage(RAW_TX_RTL8211_PAGE_DEFAULT);
  App_PrintHex32("  TX good: ", heth1.Instance->MMCTPCGR);
}

static void RawEthernetTxLoop(void)
{
  uint32_t seq = 0U;
  HAL_StatusTypeDef status;

  App_Print("RAW TX test: eth.type=0x88B5 src=02:00:00:00:00:01\r\n");
  App_Print("Wireshark filter: eth.type == 0x88b5 || eth.src == 02:00:00:00:00:01\r\n");

  HAL_ETH_SetMDIOClockRange(&heth1);
  RawEnableRtl8211Rxc();
  RawDisableRtl8211Eee();
  RawLimitPhyTo100MFull();
  RawConfigureMac100MFull();
  RawInitTxConfig();

  RawPrintPhyReg("PHY ID1: ", RTL8211_PHYID1);
  RawPrintPhyReg("PHY ID2: ", RTL8211_PHYID2);
  RawPrintPhyReg("BMSR: ", RTL8211_BMSR);

  for (uint32_t i = 0U; i < 40U; i++)
  {
    RawDelayCycles(RAW_TX_DELAY_CYCLES);
  }

  status = HAL_ETH_Start(&heth1);
  if (status != HAL_OK)
  {
    App_Print("RAW TX HAL_ETH_Start failed\r\n");
    RawPrintTxError();
  }
  else
  {
    App_Print("RAW TX HAL started\r\n");
  }

  while (1)
  {
    RawBuildFrame(seq);
    RawCleanDCacheForTx();

    status = HAL_ETH_Transmit_IT(&heth1, &RawTxConfig);
    if (status != HAL_OK)
    {
      RawPrintTxError();
      RawDelayCycles(RAW_TX_DELAY_CYCLES * 20U);
    }

    if ((seq % RAW_TX_PRINT_INTERVAL) == 0U)
    {
      RawPrintRuntime(seq);
      HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
    }

    seq++;
    RawDelayCycles(RAW_TX_DELAY_CYCLES);
  }
}
#endif /* APP_ETH_RAW_TX_TEST */

void Ethernet_BringupTests_BeforeNetX(void)
{
#if APP_ETH_RAW_TX_TEST
  RawEthernetTxLoop();
#endif
}

void Ethernet_BringupTests_ThreadReturned(void)
{
  HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
  App_Print("ThreadX kernel returned unexpectedly\r\n");
  HAL_Delay(BRINGUP_TEST_HEARTBEAT_PERIOD_MS);
}
