/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    eth_diagnostics.c
  * @brief   Ethernet runtime diagnostic helpers for the NetX baseline.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "eth_diagnostics.h"
#include "app_console.h"
#include "eth.h"
#include "main.h"
#include "rtl8211.h"

static volatile uint32_t eth1_irq_count;
static uint32_t eth_diag_phy_addr = 0xFFFFFFFFU;

#define ETH_DIAG_PHY_ADDR_EXPECTED 1U
#define ETH_DIAG_PHY_ADDR_FALLBACK 1U
#define ETH_DIAG_LINE_MAX 192U

static void Ethernet_AppendText(char *line, uint32_t *pos, const char *text)
{
  while ((text != NULL) && (*text != '\0') && (*pos < (ETH_DIAG_LINE_MAX - 1U)))
  {
    line[*pos] = *text;
    (*pos)++;
    text++;
  }
}

static void Ethernet_AppendHex32(char *line, uint32_t *pos, uint32_t value)
{
  static const char hex[] = "0123456789ABCDEF";

  if (*pos < (ETH_DIAG_LINE_MAX - 3U))
  {
    line[*pos] = '0';
    (*pos)++;
    line[*pos] = 'x';
    (*pos)++;
  }

  for (uint32_t nibble = 0U; nibble < 8U; nibble++)
  {
    if (*pos >= (ETH_DIAG_LINE_MAX - 1U))
    {
      break;
    }
    uint32_t shift = 28U - (nibble * 4U);
    line[*pos] = hex[(value >> shift) & 0xFU];
    (*pos)++;
  }
}

static uint32_t Ethernet_IsRtl8211At(uint32_t addr)
{
  uint32_t phy_id1;
  uint32_t phy_id2;

  if ((HAL_ETH_ReadPHYRegister(&heth1, addr, RTL8211_PHYID1, &phy_id1) == HAL_OK) &&
      (HAL_ETH_ReadPHYRegister(&heth1, addr, RTL8211_PHYID2, &phy_id2) == HAL_OK))
  {
    if ((phy_id1 == RTL8211_PHYID1_OUI_MSB_DEFAULT) &&
        ((phy_id2 & (RTL8211_PHYID2_OUI_LSB | RTL8211_PHYID2_MODEL_NUM)) ==
         (RTL8211_PHYID2_OUI_LSB_DEFAULT | RTL8211_PHYID2_MODEL_NUM_DEFAULT)))
    {
      return 1U;
    }
  }

  return 0U;
}

static uint32_t Ethernet_GetDiagPhyAddr(void)
{
  if (eth_diag_phy_addr <= 31U)
  {
    return eth_diag_phy_addr;
  }

  /* Match the ST RTL8211 component driver: the expected strap address is 0x01,
     and the official scan intentionally starts from address 1. */
  if (Ethernet_IsRtl8211At(ETH_DIAG_PHY_ADDR_EXPECTED) != 0U)
  {
    eth_diag_phy_addr = ETH_DIAG_PHY_ADDR_EXPECTED;
    return eth_diag_phy_addr;
  }

  for (uint32_t addr = 1U; addr < 32U; addr++)
  {
    if ((addr != ETH_DIAG_PHY_ADDR_EXPECTED) && (Ethernet_IsRtl8211At(addr) != 0U))
    {
      eth_diag_phy_addr = addr;
      return eth_diag_phy_addr;
    }
  }

  eth_diag_phy_addr = ETH_DIAG_PHY_ADDR_FALLBACK;
  return eth_diag_phy_addr;
}

void Ethernet_PrintClockDebug(void)
{
  App_PrintHex32("ETH HCLK Hz: ", HAL_RCC_GetHCLKFreq());
  App_PrintHex32("ETH kernel Hz: ", HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_ETH1));
}

void Ethernet_RecordIrq(void)
{
  eth1_irq_count++;
}

static void Ethernet_PrintRxDescriptorDebug(uint32_t ch)
{
  ETH_DMADescTypeDef *rx_desc;

  if ((ch >= ETH_DMA_RX_CH_CNT) || (heth1.Init.RxDesc[ch] == NULL))
  {
    return;
  }

  rx_desc = heth1.Init.RxDesc[ch];

  App_PrintHex32("  RxDescIdx: ", heth1.RxDescList[ch].RxDescIdx);
  App_PrintHex32("  RxBuildIdx: ", heth1.RxDescList[ch].RxBuildDescIdx);
  App_PrintHex32("  RxBuildCnt: ", heth1.RxDescList[ch].RxBuildDescCnt);
  App_PrintHex32("  RxDataLen: ", heth1.RxDescList[ch].RxDataLength);
  App_PrintHex32("  RXD0 DESC3: ", rx_desc[0].DESC3);
  App_PrintHex32("  RXD1 DESC3: ", rx_desc[1].DESC3);
  App_PrintHex32("  RXD2 DESC3: ", rx_desc[2].DESC3);
  App_PrintHex32("  RXD3 DESC3: ", rx_desc[3].DESC3);
}

static void Ethernet_PrintPhyReg(const char *label, uint32_t reg)
{
  uint32_t value;
  uint32_t phy_addr = Ethernet_GetDiagPhyAddr();

  if (HAL_ETH_ReadPHYRegister(&heth1, phy_addr, reg, &value) == HAL_OK)
  {
    App_PrintHex32(label, value);
  }
  else
  {
    App_Print(label);
    App_Print("read failed\r\n");
  }
}

static void Ethernet_PrintPhyRuntimeDebug(void)
{
  uint32_t phy_addr = Ethernet_GetDiagPhyAddr();

  Ethernet_PrintPhyReg("  PHY BMCR: ", RTL8211_BMCR);
  Ethernet_PrintPhyReg("  PHY BMSR: ", RTL8211_BMSR);
  Ethernet_PrintPhyReg("  PHY ANAR: ", RTL8211_ANAR);
  Ethernet_PrintPhyReg("  PHY ANLPAR: ", RTL8211_ANLPAR);
  Ethernet_PrintPhyReg("  PHY ANER: ", RTL8211_ANER);
  Ethernet_PrintPhyReg("  PHY GBCR: ", RTL8211_GBCR);
  Ethernet_PrintPhyReg("  PHY GBSR: ", RTL8211_GBSR);

  if (HAL_ETH_WritePHYRegister(&heth1, phy_addr, RTL8211_PAGSR, 0x0A43U) == HAL_OK)
  {
    Ethernet_PrintPhyReg("  PHY PA43 PHYCR1: ", RTL8211_PHYCR1_PA43);
    Ethernet_PrintPhyReg("  PHY PA43 PHYCR2: ", RTL8211_PHYCR2_PA43);
    Ethernet_PrintPhyReg("  PHY PA43 PHYSR1: ", RTL8211_PHYSR1_PA43);
    (void)HAL_ETH_WritePHYRegister(&heth1, phy_addr, RTL8211_PAGSR, 0U);
  }
  else
  {
    App_Print("  PHY PA43 select failed\r\n");
  }
}

void Ethernet_PrintRxSummary(void)
{
  ETH_MACConfigTypeDef mac_config;
  uint32_t bmsr = 0U;
  uint32_t phycr2 = 0U;
  uint32_t physr1 = 0U;
  uint32_t phy_addr;
  char line[ETH_DIAG_LINE_MAX];
  uint32_t pos = 0U;

  if (heth1.Instance == NULL)
  {
    return;
  }

  phy_addr = Ethernet_GetDiagPhyAddr();

  (void)HAL_ETH_ReadPHYRegister(&heth1, phy_addr, RTL8211_BMSR, &bmsr);
  if (HAL_ETH_WritePHYRegister(&heth1, phy_addr, RTL8211_PAGSR, 0x0A43U) == HAL_OK)
  {
    (void)HAL_ETH_ReadPHYRegister(&heth1, phy_addr, RTL8211_PHYCR2_PA43, &phycr2);
    (void)HAL_ETH_ReadPHYRegister(&heth1, phy_addr, RTL8211_PHYSR1_PA43, &physr1);
    (void)HAL_ETH_WritePHYRegister(&heth1, phy_addr, RTL8211_PAGSR, 0U);
  }

  Ethernet_AppendText(line, &pos, "ETH: phy=");
  Ethernet_AppendHex32(line, &pos, phy_addr);
  Ethernet_AppendText(line, &pos, " irq=");
  Ethernet_AppendHex32(line, &pos, eth1_irq_count);
  Ethernet_AppendText(line, &pos, " mmc_paok=");
  Ethernet_AppendHex32(line, &pos, heth1.Instance->MMCRPAOKR);
  Ethernet_AppendText(line, &pos, " crc=");
  Ethernet_AppendHex32(line, &pos, heth1.Instance->MMCRCRCEPR);
  Ethernet_AppendText(line, &pos, " bmsr=");
  Ethernet_AppendHex32(line, &pos, bmsr);
  Ethernet_AppendText(line, &pos, " phycr2=");
  Ethernet_AppendHex32(line, &pos, phycr2);
  Ethernet_AppendText(line, &pos, " physr1=");
  Ethernet_AppendHex32(line, &pos, physr1);

  if (HAL_ETH_GetMACConfig(&heth1, &mac_config) == HAL_OK)
  {
    Ethernet_AppendText(line, &pos, " speed=");
    Ethernet_AppendHex32(line, &pos, mac_config.Speed);
    Ethernet_AppendText(line, &pos, " ps=");
    Ethernet_AppendHex32(line, &pos, mac_config.PortSelect);
  }

  if (pos < (ETH_DIAG_LINE_MAX - 2U))
  {
    line[pos++] = '\r';
    line[pos++] = '\n';
  }
  line[pos] = '\0';
  App_Print(line);
}

void Ethernet_PrintRxRuntimeDebug(void)
{
  ETH_MACConfigTypeDef mac_config;

  if (heth1.Instance == NULL)
  {
    return;
  }

  App_Print("ETH RX diag\r\n");
  App_PrintHex32("  IRQ count: ", eth1_irq_count);
  App_PrintHex32("  HAL State: ", (uint32_t)HAL_ETH_GetState(&heth1));
  App_PrintHex32("  HAL Error: ", heth1.ErrorCode);
  Ethernet_PrintPhyRuntimeDebug();

  if (HAL_ETH_GetMACConfig(&heth1, &mac_config) == HAL_OK)
  {
    App_PrintHex32("  MAC speed: ", mac_config.Speed);
    App_PrintHex32("  MAC duplex: ", mac_config.DuplexMode);
    App_PrintHex32("  MAC portselect: ", mac_config.PortSelect);
  }

  App_PrintHex32("  MACCR: ", heth1.Instance->MACCR);
  App_PrintHex32("  MACPFR: ", heth1.Instance->MACPFR);
  App_PrintHex32("  MACRXTXSR: ", heth1.Instance->MACRXTXSR);
  App_PrintHex32("  DMAISR: ", heth1.Instance->DMAISR);
  App_PrintHex32("  MTLRXQDMAMR: ", heth1.Instance->MTLRXQDMAMR);
  App_PrintHex32("  MMCRCRCEPR: ", heth1.Instance->MMCRCRCEPR);
  App_PrintHex32("  MMCRAEPR: ", heth1.Instance->MMCRAEPR);
  App_PrintHex32("  MMCRUPGR: ", heth1.Instance->MMCRUPGR);
  App_PrintHex32("  MMCRPAER: ", heth1.Instance->MMCRPAER);
  App_PrintHex32("  MMCRPAOKR: ", heth1.Instance->MMCRPAOKR);

  App_PrintHex32("  CH0 DMACCR: ", heth1.Instance->DMA_CH[0].DMACCR);
  App_PrintHex32("  CH0 DMACSR: ", heth1.Instance->DMA_CH[0].DMACSR);
  App_PrintHex32("  CH0 DMACIER: ", heth1.Instance->DMA_CH[0].DMACIER);
  App_PrintHex32("  CH0 DMACRXCR: ", heth1.Instance->DMA_CH[0].DMACRXCR);
  App_PrintHex32("  CH0 RXDLAR: ", heth1.Instance->DMA_CH[0].DMACRXDLAR);
  App_PrintHex32("  CH0 RXDTPR: ", heth1.Instance->DMA_CH[0].DMACRXDTPR);
  App_PrintHex32("  CH0 RXRLR: ", heth1.Instance->DMA_CH[0].DMACRXRLR);
  App_PrintHex32("  CH0 CARXDR: ", heth1.Instance->DMA_CH[0].DMACCARXDR);
  App_PrintHex32("  CH0 CARXBR: ", heth1.Instance->DMA_CH[0].DMACCARXBR);
  App_PrintHex32("  MTLQ0 ICSR: ", heth1.Instance->MTL_QUEUE[0].MTLQICSR);
  App_PrintHex32("  MTLQ0 RXOMR: ", heth1.Instance->MTL_QUEUE[0].MTLRXQOMR);
  App_PrintHex32("  MTLQ0 RXMPOCR: ", heth1.Instance->MTL_QUEUE[0].MTLRXQMPOCR);
  App_PrintHex32("  MTLQ0 RXQDR: ", heth1.Instance->MTL_QUEUE[0].MTLRXQDR);

  Ethernet_PrintRxDescriptorDebug(0U);
}
