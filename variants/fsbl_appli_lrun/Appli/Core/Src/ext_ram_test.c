/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    ext_ram_test.c
  * @brief   External RAM diagnostics reported from the Appli stage.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "ext_ram_test.h"
#include "main.h"
#include "usart.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define EXT_RAM_DIRECT_TEST_ENABLE       1U
#define EXT_RAM_BASE_ADDR                0x90000000UL
#define EXT_RAM_PROBE_OFFSET             0x00000100UL
#define FSBL_PSRAM_DIAG_RECORD_ADDR      0x2417F000UL
#define FSBL_PSRAM_DIAG_RECORD_MAGIC     0x5053524DUL /* "PSRM" */

typedef struct
{
  uint32_t magic;
  uint32_t version;
  uint32_t diag_level;
  uint32_t result;
  uint32_t step;
  uint32_t error_code;
  uint32_t hal_error;
  uint32_t test_words;
  uint32_t fail_index;
  uint32_t expected;
  uint32_t actual;
  uint8_t mr0_w[2];
  uint8_t mr0_r[2];
  uint8_t mr4_w[2];
  uint8_t mr4_r[2];
  uint8_t mr8_w[2];
  uint8_t mr8_r[2];
  uint32_t indirect_len;
  uint8_t indirect_tx[16];
  uint8_t indirect_rx[16];
  uint32_t xspi_cr_before_mmap;
  uint32_t xspi_sr_before_mmap;
  uint32_t xspi_dcr1_before_mmap;
  uint32_t xspi_dcr2_before_mmap;
  uint32_t xspi_dcr3_before_mmap;
  uint32_t xspi_dcr4_before_mmap;
  uint32_t xspi_ccr_before_mmap;
  uint32_t xspi_tcr_before_mmap;
  uint32_t xspi_wccr_before_mmap;
  uint32_t xspi_wtcr_before_mmap;
  uint32_t xspi_cr_after_mmap;
  uint32_t xspi_sr_after_mmap;
  uint32_t xspi_dcr1_after_mmap;
  uint32_t xspi_dcr2_after_mmap;
  uint32_t xspi_dcr3_after_mmap;
  uint32_t xspi_dcr4_after_mmap;
  uint32_t xspi_ccr_after_mmap;
  uint32_t xspi_tcr_after_mmap;
  uint32_t xspi_wccr_after_mmap;
  uint32_t xspi_wtcr_after_mmap;
  uint32_t rcc_ahb3enr;
  uint32_t rcc_ahb3ensr;
  uint32_t rcc_ahb5enr;
  uint32_t rcc_ahb5ensr;
  uint32_t xspim_cr;
  uint32_t mce1_clock_enabled;
  uint32_t mce1_cr;
  uint32_t mce1_sr;
  uint32_t mce1_iasr;
  uint32_t mce1_iaddr;
  uint32_t mce1_region1_regcr;
  uint32_t mce1_region1_saddr;
  uint32_t mce1_region1_eaddr;
  uint32_t risaf_clock_enabled;
  uint32_t risaf11_cr;
  uint32_t risaf11_iasr;
  uint32_t risaf11_iaesr;
  uint32_t risaf11_iaddr;
  uint32_t risaf11_region1_cfgr;
  uint32_t risaf11_region1_startr;
  uint32_t risaf11_region1_endr;
  uint32_t risaf11_region1_cidcfgr;
  uint32_t sau_ctrl;
  uint32_t sau_type;
  uint32_t sau_rnr;
  uint32_t sau_rbar;
  uint32_t sau_rlar;
  uint32_t mpu_ctrl;
  uint32_t mpu_type;
} FSBL_PSRAM_DiagRecord_t;

static void UART_WriteString(const char *text);
static void UART_WriteLine(const char *format, ...);
static void ExtRam_ReportAccessPath(void);
static void ExtRam_DirectProbe(void);

void ExtRam_TestReport(void)
{
  const volatile FSBL_PSRAM_DiagRecord_t *diag =
      (const volatile FSBL_PSRAM_DiagRecord_t *)FSBL_PSRAM_DIAG_RECORD_ADDR;

  if (diag->magic != FSBL_PSRAM_DIAG_RECORD_MAGIC)
  {
    UART_WriteString("FSBL PSRAM diag not available\r\n");
    return;
  }

  UART_WriteLine("FSBL PSRAM diag ver=%lu level=%lu result=%lu step=%lu err=%lu hal=0x%08lX\r\n",
                 (unsigned long)diag->version,
                 (unsigned long)diag->diag_level,
                 (unsigned long)diag->result,
                 (unsigned long)diag->step,
                 (unsigned long)diag->error_code,
                 (unsigned long)diag->hal_error);

  UART_WriteLine("FSBL PSRAM MR0 w=%02X %02X r=%02X %02X, MR4 w=%02X %02X r=%02X %02X, MR8 w=%02X %02X r=%02X %02X\r\n",
                 diag->mr0_w[0], diag->mr0_w[1], diag->mr0_r[0], diag->mr0_r[1],
                 diag->mr4_w[0], diag->mr4_w[1], diag->mr4_r[0], diag->mr4_r[1],
                 diag->mr8_w[0], diag->mr8_w[1], diag->mr8_r[0], diag->mr8_r[1]);

  UART_WriteLine("FSBL PSRAM indirect len=%lu tx=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
                 (unsigned long)diag->indirect_len,
                 diag->indirect_tx[0], diag->indirect_tx[1], diag->indirect_tx[2], diag->indirect_tx[3],
                 diag->indirect_tx[4], diag->indirect_tx[5], diag->indirect_tx[6], diag->indirect_tx[7],
                 diag->indirect_tx[8], diag->indirect_tx[9], diag->indirect_tx[10], diag->indirect_tx[11],
                 diag->indirect_tx[12], diag->indirect_tx[13], diag->indirect_tx[14], diag->indirect_tx[15]);

  UART_WriteLine("FSBL PSRAM indirect rx=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
                 diag->indirect_rx[0], diag->indirect_rx[1], diag->indirect_rx[2], diag->indirect_rx[3],
                 diag->indirect_rx[4], diag->indirect_rx[5], diag->indirect_rx[6], diag->indirect_rx[7],
                 diag->indirect_rx[8], diag->indirect_rx[9], diag->indirect_rx[10], diag->indirect_rx[11],
                 diag->indirect_rx[12], diag->indirect_rx[13], diag->indirect_rx[14], diag->indirect_rx[15]);

  if (diag->version >= 2UL)
  {
    UART_WriteLine("FSBL XSPI1 before mmap CR=0x%08lX SR=0x%08lX DCR1=0x%08lX DCR2=0x%08lX DCR3=0x%08lX DCR4=0x%08lX\r\n",
                   (unsigned long)diag->xspi_cr_before_mmap,
                   (unsigned long)diag->xspi_sr_before_mmap,
                   (unsigned long)diag->xspi_dcr1_before_mmap,
                   (unsigned long)diag->xspi_dcr2_before_mmap,
                   (unsigned long)diag->xspi_dcr3_before_mmap,
                   (unsigned long)diag->xspi_dcr4_before_mmap);

    UART_WriteLine("FSBL XSPI1 after mmap CR=0x%08lX SR=0x%08lX DCR1=0x%08lX DCR2=0x%08lX DCR3=0x%08lX DCR4=0x%08lX\r\n",
                   (unsigned long)diag->xspi_cr_after_mmap,
                   (unsigned long)diag->xspi_sr_after_mmap,
                   (unsigned long)diag->xspi_dcr1_after_mmap,
                   (unsigned long)diag->xspi_dcr2_after_mmap,
                   (unsigned long)diag->xspi_dcr3_after_mmap,
                   (unsigned long)diag->xspi_dcr4_after_mmap);
  }

  if (diag->version >= 3UL)
  {
    UART_WriteLine("FSBL path XSPIM_CR=0x%08lX MCE1_CLK=%lu RISAF_CLK=%lu\r\n",
                   (unsigned long)diag->xspim_cr,
                   (unsigned long)diag->mce1_clock_enabled,
                   (unsigned long)diag->risaf_clock_enabled);

    UART_WriteLine("FSBL MCE1 R1 REGCR=0x%08lX SADDR=0x%08lX EADDR=0x%08lX\r\n",
                   (unsigned long)diag->mce1_region1_regcr,
                   (unsigned long)diag->mce1_region1_saddr,
                   (unsigned long)diag->mce1_region1_eaddr);

    UART_WriteLine("FSBL RISAF11 R1 CFGR=0x%08lX START=0x%08lX END=0x%08lX CID=0x%08lX\r\n",
                   (unsigned long)diag->risaf11_region1_cfgr,
                   (unsigned long)diag->risaf11_region1_startr,
                   (unsigned long)diag->risaf11_region1_endr,
                   (unsigned long)diag->risaf11_region1_cidcfgr);
  }

  ExtRam_ReportAccessPath();
  ExtRam_DirectProbe();
}

static void UART_WriteString(const char *text)
{
  if (text != NULL)
  {
    (void)HAL_UART_Transmit(&huart3, (uint8_t *)text, (uint16_t)strlen(text), 100U);
  }
}

static void UART_WriteLine(const char *format, ...)
{
  char line[256];
  va_list args;
  int len;

  va_start(args, format);
  len = vsnprintf(line, sizeof(line), format, args);
  va_end(args);

  if (len > 0)
  {
    if ((uint32_t)len >= sizeof(line))
    {
      len = (int)sizeof(line) - 1;
    }
    (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
  }
}

static void ExtRam_ReportAccessPath(void)
{
  UART_WriteLine("Appli fault CFSR=0x%08lX HFSR=0x%08lX SFSR=0x%08lX BFAR=0x%08lX SFAR=0x%08lX\r\n",
                 (unsigned long)SCB->CFSR,
                 (unsigned long)SCB->HFSR,
                 (unsigned long)SCB->SFSR,
                 (unsigned long)SCB->BFAR,
                 (unsigned long)SCB->SFAR);

  UART_WriteLine("Appli RCC AHB3ENR=0x%08lX AHB3ENSR=0x%08lX AHB5ENR=0x%08lX AHB5ENSR=0x%08lX\r\n",
                 (unsigned long)RCC->AHB3ENR,
                 (unsigned long)RCC->AHB3ENSR,
                 (unsigned long)RCC->AHB5ENR,
                 (unsigned long)RCC->AHB5ENSR);

  UART_WriteLine("Appli path XSPIM_CR=0x%08lX MCE1_CLK=%lu RISAF_CLK=%lu\r\n",
                 (unsigned long)XSPIM->CR,
                 (unsigned long)(((RCC->AHB5ENR & RCC_AHB5ENR_MCE1EN) != 0U) ? 1U : 0U),
                 (unsigned long)(((RCC->AHB3ENR & RCC_AHB3ENR_RISAFEN) != 0U) ? 1U : 0U));

  UART_WriteLine("Appli XSPI1 CR=0x%08lX SR=0x%08lX DCR1=0x%08lX DCR2=0x%08lX DCR3=0x%08lX DCR4=0x%08lX\r\n",
                 (unsigned long)XSPI1->CR,
                 (unsigned long)XSPI1->SR,
                 (unsigned long)XSPI1->DCR1,
                 (unsigned long)XSPI1->DCR2,
                 (unsigned long)XSPI1->DCR3,
                 (unsigned long)XSPI1->DCR4);

  UART_WriteLine("Appli XSPI1 CCR=0x%08lX TCR=0x%08lX WCCR=0x%08lX WTCR=0x%08lX\r\n",
                 (unsigned long)XSPI1->CCR,
                 (unsigned long)XSPI1->TCR,
                 (unsigned long)XSPI1->WCCR,
                 (unsigned long)XSPI1->WTCR);

  UART_WriteLine("Appli MCE1 R1 REGCR=0x%08lX SADDR=0x%08lX EADDR=0x%08lX\r\n",
                 (unsigned long)MCE1_REGION1->REGCR,
                 (unsigned long)MCE1_REGION1->SADDR,
                 (unsigned long)MCE1_REGION1->EADDR);

  UART_WriteLine("Appli RISAF11 R1 CFGR=0x%08lX START=0x%08lX END=0x%08lX CID=0x%08lX\r\n",
                 (unsigned long)RISAF11->REG[0].CFGR,
                 (unsigned long)RISAF11->REG[0].STARTR,
                 (unsigned long)RISAF11->REG[0].ENDR,
                 (unsigned long)RISAF11->REG[0].CIDCFGR);

  UART_WriteLine("Appli MPU CTRL=0x%08lX TYPE=0x%08lX\r\n",
                 (unsigned long)MPU->CTRL,
                 (unsigned long)MPU->TYPE);
}

static void ExtRam_DirectProbe(void)
{
#if EXT_RAM_DIRECT_TEST_ENABLE
  volatile uint8_t *ram8 = (volatile uint8_t *)(EXT_RAM_BASE_ADDR + EXT_RAM_PROBE_OFFSET);
  volatile uint16_t *ram16 = (volatile uint16_t *)(EXT_RAM_BASE_ADDR + EXT_RAM_PROBE_OFFSET);
  volatile uint32_t *ram32 = (volatile uint32_t *)(EXT_RAM_BASE_ADDR + EXT_RAM_PROBE_OFFSET);
  uint8_t expected8 = 0xA5U;
  uint16_t expected16 = 0xA55AU;
  uint32_t expected32 = 0x5A5A1234U;
  uint32_t probe_addr = EXT_RAM_BASE_ADDR + EXT_RAM_PROBE_OFFSET;
  uint32_t residue_word0;
  uint32_t residue_word1;
  uint8_t actual8;
  uint16_t actual16;
  uint32_t actual32;
  uint32_t verify_word0;
  uint32_t verify_word1;
  uint32_t lane_word0;
  uint32_t lane_word1;
  uint32_t lane_index;
  uint8_t lane_value;
  uint8_t lane_read8;
  uint8_t ok = 1U;

  UART_WriteString("EXT RAM direct mmap test begin\r\n");
  UART_WriteLine("EXT RAM probe base=0x%08lX offset=0x%08lX addr=0x%08lX\r\n",
                 (unsigned long)EXT_RAM_BASE_ADDR,
                 (unsigned long)EXT_RAM_PROBE_OFFSET,
                 (unsigned long)probe_addr);

  UART_WriteString("EXT RAM indirect residue read\r\n");
  residue_word0 = ram32[0];
  residue_word1 = ram32[1];
  UART_WriteLine("EXT RAM residue addr=0x%08lX byte0=0x%02X word0=0x%08lX word1=0x%08lX\r\n",
                 (unsigned long)probe_addr,
                 ram8[0],
                 (unsigned long)residue_word0,
                 (unsigned long)residue_word1);

  UART_WriteString("EXT RAM byte write/read probe\r\n");
  ram32[0] = 0x11223344U;
  ram32[1] = 0x55667788U;
  __DSB();
  HAL_Delay(1U);

  ram8[0] = expected8;
  __DSB();
  HAL_Delay(1U);
  actual8 = ram8[0];
  verify_word0 = ram32[0];
  verify_word1 = ram32[1];
  if ((actual8 != expected8) || (verify_word0 != 0x112233A5U) || (verify_word1 != 0x55667788U))
  {
    UART_WriteLine("EXT RAM byte FAIL addr=0x%08lX exp=0x%02X read8=0x%02X word0=0x%08lX word1=0x%08lX\r\n",
                   (unsigned long)probe_addr,
                   expected8,
                   actual8,
                   (unsigned long)verify_word0,
                   (unsigned long)verify_word1);
    ok = 0U;
  }
  else
  {
    UART_WriteLine("EXT RAM byte OK addr=0x%08lX read8=0x%02X word0=0x%08lX word1=0x%08lX\r\n",
                   (unsigned long)probe_addr,
                   actual8,
                   (unsigned long)verify_word0,
                   (unsigned long)verify_word1);
  }

  UART_WriteString("EXT RAM halfword write/read probe\r\n");
  ram32[0] = 0x11223344U;
  ram32[1] = 0x55667788U;
  __DSB();
  HAL_Delay(1U);

  ram16[0] = expected16;
  __DSB();
  HAL_Delay(1U);
  actual16 = ram16[0];
  verify_word0 = ram32[0];
  verify_word1 = ram32[1];
  if ((actual16 != expected16) || (verify_word0 != 0x1122A55AU) || (verify_word1 != 0x55667788U))
  {
    UART_WriteLine("EXT RAM halfword FAIL addr=0x%08lX exp=0x%04X read16=0x%04X word0=0x%08lX word1=0x%08lX\r\n",
                   (unsigned long)probe_addr,
                   expected16,
                   actual16,
                   (unsigned long)verify_word0,
                   (unsigned long)verify_word1);
    ok = 0U;
  }
  else
  {
    UART_WriteLine("EXT RAM halfword OK addr=0x%08lX read16=0x%04X word0=0x%08lX word1=0x%08lX\r\n",
                   (unsigned long)probe_addr,
                   actual16,
                   (unsigned long)verify_word0,
                   (unsigned long)verify_word1);
  }

  UART_WriteString("EXT RAM word write/read probe\r\n");
  ram32[0] = 0x11223344U;
  ram32[1] = 0x55667788U;
  __DSB();
  HAL_Delay(1U);

  ram32[0] = expected32;
  __DSB();
  HAL_Delay(1U);
  actual32 = ram32[0];
  verify_word1 = ram32[1];
  if ((actual32 != expected32) || (verify_word1 != 0x55667788U))
  {
    UART_WriteLine("EXT RAM word FAIL addr=0x%08lX exp=0x%08lX read32=0x%08lX word1=0x%08lX\r\n",
                   (unsigned long)probe_addr,
                   (unsigned long)expected32,
                   (unsigned long)actual32,
                   (unsigned long)verify_word1);
    ok = 0U;
  }
  else
  {
    UART_WriteLine("EXT RAM word OK addr=0x%08lX read32=0x%08lX word1=0x%08lX\r\n",
                   (unsigned long)probe_addr,
                   (unsigned long)actual32,
                   (unsigned long)verify_word1);
  }

  UART_WriteString("EXT RAM byte lane scan begin\r\n");
  for (lane_index = 0U; lane_index < 8U; lane_index++)
  {
    lane_value = (uint8_t)(0xA0U + lane_index);
    ram32[0] = 0x11223344U;
    ram32[1] = 0x55667788U;
    __DSB();
    HAL_Delay(1U);

    ram8[lane_index] = lane_value;
    __DSB();
    HAL_Delay(1U);

    lane_read8 = ram8[lane_index];
    lane_word0 = ram32[0];
    lane_word1 = ram32[1];
    UART_WriteLine("EXT RAM byte lane[%lu] exp=0x%02X read8=0x%02X word0=0x%08lX word1=0x%08lX\r\n",
                   (unsigned long)lane_index,
                   lane_value,
                   lane_read8,
                   (unsigned long)lane_word0,
                   (unsigned long)lane_word1);
  }

  ram32[0] = 0x00000000U;
  ram32[1] = 0x00000000U;
  __DSB();
  UART_WriteLine("EXT RAM direct mmap test %s base=0x%08lX\r\n",
                 (ok != 0U) ? "OK" : "FAIL",
                 (unsigned long)EXT_RAM_BASE_ADDR);
#else
  UART_WriteString("EXT RAM direct mmap test disabled\r\n");
#endif
}
