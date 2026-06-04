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
#define EXT_RAM_SANITY_OFFSET            0x00000100UL
#define EXT_RAM_BUFFER_OFFSET            0x00100000UL
#define EXT_RAM_BUFFER_BYTES             0x00400000UL
#define EXT_RAM_BUFFER_WORDS             (EXT_RAM_BUFFER_BYTES / sizeof(uint32_t))
#define EXT_RAM_QUICK_WORDS              4096U
#define EXT_RAM_FAIL_WINDOW_WORDS        32U
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
static uint8_t ExtRam_BufferQuickCheck(volatile uint32_t *buffer);
static void ExtRam_BufferFailProbe(volatile uint32_t *buffer, uint32_t fail_index, uint32_t pattern_id);
static uint32_t ExtRam_BufferPattern(uint32_t index, uint32_t pattern_id);
static uint32_t ExtRam_BufferPatternA(uint32_t index);
static uint32_t ExtRam_BufferPatternB(uint32_t index);

void ExtRam_TestReport(void)
{
  const volatile FSBL_PSRAM_DiagRecord_t *diag =
      (const volatile FSBL_PSRAM_DiagRecord_t *)FSBL_PSRAM_DIAG_RECORD_ADDR;

  if (diag->magic != FSBL_PSRAM_DIAG_RECORD_MAGIC)
  {
    UART_WriteString("FSBL PSRAM diag not available\r\n");
    return;
  }

  UART_WriteLine("PSRAM sanity FSBL diag ver=%lu level=%lu result=%lu step=%lu err=%lu hal=0x%08lX\r\n",
                 (unsigned long)diag->version,
                 (unsigned long)diag->diag_level,
                 (unsigned long)diag->result,
                 (unsigned long)diag->step,
                 (unsigned long)diag->error_code,
                 (unsigned long)diag->hal_error);

  UART_WriteLine("PSRAM sanity FSBL indirect len=%lu tx0=%02X %02X %02X %02X rx0=%02X %02X %02X %02X\r\n",
                 (unsigned long)diag->indirect_len,
                 diag->indirect_tx[0], diag->indirect_tx[1], diag->indirect_tx[2], diag->indirect_tx[3],
                 diag->indirect_rx[0], diag->indirect_rx[1], diag->indirect_rx[2], diag->indirect_rx[3]);

  if (diag->version >= 2UL)
  {
    UART_WriteLine("PSRAM sanity FSBL mmap CR=0x%08lX SR=0x%08lX DCR2=0x%08lX CCR=0x%08lX WCCR=0x%08lX\r\n",
                   (unsigned long)diag->xspi_cr_after_mmap,
                   (unsigned long)diag->xspi_sr_after_mmap,
                   (unsigned long)diag->xspi_dcr2_after_mmap,
                   (unsigned long)diag->xspi_ccr_after_mmap,
                   (unsigned long)diag->xspi_wccr_after_mmap);
  }

  if (diag->version >= 3UL)
  {
    UART_WriteLine("PSRAM sanity FSBL path XSPIM_CR=0x%08lX MCE1_REGCR=0x%08lX RISAF11_CFGR=0x%08lX\r\n",
                   (unsigned long)diag->xspim_cr,
                   (unsigned long)diag->mce1_region1_regcr,
                   (unsigned long)diag->risaf11_region1_cfgr);
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
  UART_WriteLine("PSRAM sanity Appli fault CFSR=0x%08lX HFSR=0x%08lX SFSR=0x%08lX BFAR=0x%08lX SFAR=0x%08lX\r\n",
                 (unsigned long)SCB->CFSR,
                 (unsigned long)SCB->HFSR,
                 (unsigned long)SCB->SFSR,
                 (unsigned long)SCB->BFAR,
                 (unsigned long)SCB->SFAR);

  UART_WriteLine("PSRAM sanity Appli path XSPIM_CR=0x%08lX XSPI1_CR=0x%08lX SR=0x%08lX MCE1_REGCR=0x%08lX RISAF11_CFGR=0x%08lX MPU=0x%08lX\r\n",
                 (unsigned long)XSPIM->CR,
                 (unsigned long)XSPI1->CR,
                 (unsigned long)XSPI1->SR,
                 (unsigned long)MCE1_REGION1->REGCR,
                 (unsigned long)RISAF11->REG[0].CFGR,
                 (unsigned long)MPU->CTRL);
}

static void ExtRam_DirectProbe(void)
{
#if EXT_RAM_DIRECT_TEST_ENABLE
  volatile uint8_t *sanity8 = (volatile uint8_t *)(EXT_RAM_BASE_ADDR + EXT_RAM_SANITY_OFFSET);
  volatile uint16_t *sanity16 = (volatile uint16_t *)(EXT_RAM_BASE_ADDR + EXT_RAM_SANITY_OFFSET);
  volatile uint32_t *sanity32 = (volatile uint32_t *)(EXT_RAM_BASE_ADDR + EXT_RAM_SANITY_OFFSET);
  volatile uint32_t *buffer = (volatile uint32_t *)(EXT_RAM_BASE_ADDR + EXT_RAM_BUFFER_OFFSET);
  const uint32_t lane_word0_expected[4] = {
      0x112233A0U, 0x1122A144U, 0x11A23344U, 0xA3223344U};
  const uint32_t lane_word1_expected[4] = {
      0x556677A4U, 0x5566A588U, 0x55A67788U, 0xA7667788U};
  uint32_t i;
  uint32_t value;
  uint32_t checksum;
  uint32_t mismatch_count;
  uint32_t mismatch_xor_or;
  uint32_t first_fail_index = 0U;
  uint32_t first_fail_expected = 0U;
  uint32_t first_fail_actual = 0U;
  uint32_t expected = 0U;
  uint32_t actual = 0U;
  uint32_t lane_mask = 0U;
  uint8_t sanity_ok = 1U;
  uint8_t buffer_ok = 1U;

  UART_WriteLine("PSRAM sanity mmap begin base=0x%08lX sanity=0x%08lX buffer=0x%08lX bytes=%lu\r\n",
                 (unsigned long)EXT_RAM_BASE_ADDR,
                 (unsigned long)(EXT_RAM_BASE_ADDR + EXT_RAM_SANITY_OFFSET),
                 (unsigned long)(EXT_RAM_BASE_ADDR + EXT_RAM_BUFFER_OFFSET),
                 (unsigned long)EXT_RAM_BUFFER_BYTES);

  sanity32[0] = 0x11223344U;
  sanity32[1] = 0x55667788U;
  __DSB();
  sanity8[0] = 0xA5U;
  __DSB();
  if ((sanity8[0] != 0xA5U) || (sanity32[0] != 0x112233A5U) || (sanity32[1] != 0x55667788U))
  {
    sanity_ok = 0U;
  }

  sanity32[0] = 0x11223344U;
  sanity32[1] = 0x55667788U;
  __DSB();
  sanity16[0] = 0xA55AU;
  __DSB();
  if ((sanity16[0] != 0xA55AU) || (sanity32[0] != 0x1122A55AU) || (sanity32[1] != 0x55667788U))
  {
    sanity_ok = 0U;
  }

  sanity32[0] = 0x11223344U;
  sanity32[1] = 0x55667788U;
  __DSB();
  sanity32[0] = 0x5A5A1234U;
  __DSB();
  if ((sanity32[0] != 0x5A5A1234U) || (sanity32[1] != 0x55667788U))
  {
    sanity_ok = 0U;
  }

  for (i = 0U; i < 8U; i++)
  {
    sanity32[0] = 0x11223344U;
    sanity32[1] = 0x55667788U;
    __DSB();
    sanity8[i] = (uint8_t)(0xA0U + i);
    __DSB();

    if (i < 4U)
    {
      if ((sanity8[i] == (uint8_t)(0xA0U + i)) &&
          (sanity32[0] == lane_word0_expected[i]) &&
          (sanity32[1] == 0x55667788U))
      {
        lane_mask |= (1UL << i);
      }
    }
    else
    {
      if ((sanity8[i] == (uint8_t)(0xA0U + i)) &&
          (sanity32[0] == 0x11223344U) &&
          (sanity32[1] == lane_word1_expected[i - 4U]))
      {
        lane_mask |= (1UL << i);
      }
    }
  }

  if (lane_mask != 0xFFU)
  {
    sanity_ok = 0U;
  }

  UART_WriteLine("PSRAM sanity mmap %s lane_mask=0x%02lX word0=0x%08lX word1=0x%08lX\r\n",
                 (sanity_ok != 0U) ? "OK" : "FAIL",
                 (unsigned long)lane_mask,
                 (unsigned long)sanity32[0],
                 (unsigned long)sanity32[1]);

  UART_WriteLine("PSRAM buffer test begin addr=0x%08lX bytes=%lu words=%lu\r\n",
                 (unsigned long)(EXT_RAM_BASE_ADDR + EXT_RAM_BUFFER_OFFSET),
                 (unsigned long)EXT_RAM_BUFFER_BYTES,
                 (unsigned long)EXT_RAM_BUFFER_WORDS);

  if (ExtRam_BufferQuickCheck(buffer) == 0U)
  {
    buffer_ok = 0U;
  }

  checksum = 0U;
  for (i = 0U; i < EXT_RAM_BUFFER_WORDS; i++)
  {
    value = ExtRam_BufferPatternA(i);
    buffer[i] = value;
    checksum ^= value + (i * 0x9E3779B9UL);
  }
  __DSB();
  UART_WriteLine("PSRAM buffer pattern A write checksum=0x%08lX\r\n",
                 (unsigned long)checksum);

  checksum = 0U;
  mismatch_count = 0U;
  mismatch_xor_or = 0U;
  for (i = 0U; i < EXT_RAM_BUFFER_WORDS; i++)
  {
    expected = ExtRam_BufferPatternA(i);
    actual = buffer[i];
    checksum ^= actual + (i * 0x9E3779B9UL);
    if (actual != expected)
    {
      if (mismatch_count == 0U)
      {
        first_fail_index = i;
        first_fail_expected = expected;
        first_fail_actual = actual;
      }
      mismatch_count++;
      mismatch_xor_or |= (expected ^ actual);
    }
  }
  if (mismatch_count != 0U)
  {
    buffer_ok = 0U;
    UART_WriteLine("PSRAM buffer pattern A FAIL first=%lu addr=0x%08lX exp=0x%08lX got=0x%08lX xor=0x%08lX mismatches=%lu xor_or=0x%08lX checksum=0x%08lX\r\n",
                   (unsigned long)first_fail_index,
                   (unsigned long)(EXT_RAM_BASE_ADDR + EXT_RAM_BUFFER_OFFSET + (first_fail_index * sizeof(uint32_t))),
                   (unsigned long)first_fail_expected,
                   (unsigned long)first_fail_actual,
                   (unsigned long)(first_fail_expected ^ first_fail_actual),
                   (unsigned long)mismatch_count,
                   (unsigned long)mismatch_xor_or,
                   (unsigned long)checksum);
    ExtRam_BufferFailProbe(buffer, first_fail_index, 0U);
  }
  else
  {
    UART_WriteLine("PSRAM buffer pattern A OK checksum=0x%08lX\r\n",
                   (unsigned long)checksum);
  }

  if (buffer_ok != 0U)
  {
    checksum = 0U;
    for (i = 0U; i < EXT_RAM_BUFFER_WORDS; i++)
    {
      value = ExtRam_BufferPatternB(i);
      buffer[i] = value;
      checksum ^= value + (i * 0x7F4A7C15UL);
    }
    __DSB();
    UART_WriteLine("PSRAM buffer pattern B write checksum=0x%08lX\r\n",
                   (unsigned long)checksum);

    checksum = 0U;
    mismatch_count = 0U;
    mismatch_xor_or = 0U;
    for (i = 0U; i < EXT_RAM_BUFFER_WORDS; i++)
    {
      expected = ExtRam_BufferPatternB(i);
      actual = buffer[i];
      checksum ^= actual + (i * 0x7F4A7C15UL);
      if (actual != expected)
      {
        if (mismatch_count == 0U)
        {
          first_fail_index = i;
          first_fail_expected = expected;
          first_fail_actual = actual;
        }
        mismatch_count++;
        mismatch_xor_or |= (expected ^ actual);
      }
    }
    if (mismatch_count != 0U)
    {
      buffer_ok = 0U;
      UART_WriteLine("PSRAM buffer pattern B FAIL first=%lu addr=0x%08lX exp=0x%08lX got=0x%08lX xor=0x%08lX mismatches=%lu xor_or=0x%08lX checksum=0x%08lX\r\n",
                     (unsigned long)first_fail_index,
                     (unsigned long)(EXT_RAM_BASE_ADDR + EXT_RAM_BUFFER_OFFSET + (first_fail_index * sizeof(uint32_t))),
                     (unsigned long)first_fail_expected,
                     (unsigned long)first_fail_actual,
                     (unsigned long)(first_fail_expected ^ first_fail_actual),
                     (unsigned long)mismatch_count,
                     (unsigned long)mismatch_xor_or,
                     (unsigned long)checksum);
      ExtRam_BufferFailProbe(buffer, first_fail_index, 1U);
    }
    else
    {
      UART_WriteLine("PSRAM buffer pattern B OK checksum=0x%08lX\r\n",
                     (unsigned long)checksum);
    }
  }

  UART_WriteLine("PSRAM buffer test %s addr=0x%08lX bytes=%lu\r\n",
                 ((sanity_ok != 0U) && (buffer_ok != 0U)) ? "OK" : "FAIL",
                 (unsigned long)(EXT_RAM_BASE_ADDR + EXT_RAM_BUFFER_OFFSET),
                 (unsigned long)EXT_RAM_BUFFER_BYTES);
#else
  UART_WriteString("PSRAM sanity mmap disabled\r\n");
#endif
}

static uint8_t ExtRam_BufferQuickCheck(volatile uint32_t *buffer)
{
  uint32_t i;
  uint32_t expected;
  uint32_t actual;
  uint32_t mismatch_count = 0U;
  uint32_t first_fail_index = 0U;
  uint32_t first_fail_expected = 0U;
  uint32_t first_fail_actual = 0U;

  UART_WriteLine("PSRAM buffer quick begin words=%lu\r\n",
                 (unsigned long)EXT_RAM_QUICK_WORDS);

  for (i = 0U; i < EXT_RAM_QUICK_WORDS; i++)
  {
    expected = ExtRam_BufferPatternA(i);
    buffer[i] = expected;
    __DSB();
    actual = buffer[i];
    if (actual != expected)
    {
      UART_WriteLine("PSRAM buffer quick immediate FAIL index=%lu addr=0x%08lX exp=0x%08lX got=0x%08lX xor=0x%08lX\r\n",
                     (unsigned long)i,
                     (unsigned long)(EXT_RAM_BASE_ADDR + EXT_RAM_BUFFER_OFFSET + (i * sizeof(uint32_t))),
                     (unsigned long)expected,
                     (unsigned long)actual,
                     (unsigned long)(expected ^ actual));
      return 0U;
    }
  }
  UART_WriteString("PSRAM buffer quick immediate OK\r\n");

  for (i = 0U; i < EXT_RAM_QUICK_WORDS; i++)
  {
    expected = ExtRam_BufferPatternA(i);
    actual = buffer[i];
    if (actual != expected)
    {
      if (mismatch_count == 0U)
      {
        first_fail_index = i;
        first_fail_expected = expected;
        first_fail_actual = actual;
      }
      mismatch_count++;
    }
  }

  if (mismatch_count != 0U)
  {
    UART_WriteLine("PSRAM buffer quick retained FAIL first=%lu addr=0x%08lX exp=0x%08lX got=0x%08lX xor=0x%08lX mismatches=%lu\r\n",
                   (unsigned long)first_fail_index,
                   (unsigned long)(EXT_RAM_BASE_ADDR + EXT_RAM_BUFFER_OFFSET + (first_fail_index * sizeof(uint32_t))),
                   (unsigned long)first_fail_expected,
                   (unsigned long)first_fail_actual,
                   (unsigned long)(first_fail_expected ^ first_fail_actual),
                   (unsigned long)mismatch_count);
    return 0U;
  }

  UART_WriteString("PSRAM buffer quick retained OK\r\n");
  return 1U;
}

static void ExtRam_BufferFailProbe(volatile uint32_t *buffer, uint32_t fail_index, uint32_t pattern_id)
{
  uint32_t expected = ExtRam_BufferPattern(fail_index, pattern_id);
  uint32_t read0 = buffer[fail_index];
  uint32_t read1 = buffer[fail_index];
  uint32_t read2 = buffer[fail_index];
  uint32_t read3 = buffer[fail_index];
  uint32_t window_start = (fail_index >= (EXT_RAM_FAIL_WINDOW_WORDS / 2U)) ?
      (fail_index - (EXT_RAM_FAIL_WINDOW_WORDS / 2U)) : 0U;
  uint32_t i;
  uint32_t window_mismatch = 0U;
  uint32_t window_first = 0U;
  uint32_t window_expected = 0U;
  uint32_t window_actual = 0U;

  UART_WriteLine("PSRAM buffer fail reread index=%lu exp=0x%08lX r0=0x%08lX r1=0x%08lX r2=0x%08lX r3=0x%08lX\r\n",
                 (unsigned long)fail_index,
                 (unsigned long)expected,
                 (unsigned long)read0,
                 (unsigned long)read1,
                 (unsigned long)read2,
                 (unsigned long)read3);

  buffer[fail_index] = expected;
  __DSB();
  UART_WriteLine("PSRAM buffer fail single rewrite index=%lu exp=0x%08lX got=0x%08lX\r\n",
                 (unsigned long)fail_index,
                 (unsigned long)expected,
                 (unsigned long)buffer[fail_index]);

  for (i = 0U; i < EXT_RAM_FAIL_WINDOW_WORDS; i++)
  {
    buffer[window_start + i] = ExtRam_BufferPattern(window_start + i, pattern_id);
  }
  __DSB();

  for (i = 0U; i < EXT_RAM_FAIL_WINDOW_WORDS; i++)
  {
    expected = ExtRam_BufferPattern(window_start + i, pattern_id);
    read0 = buffer[window_start + i];
    if (read0 != expected)
    {
      if (window_mismatch == 0U)
      {
        window_first = window_start + i;
        window_expected = expected;
        window_actual = read0;
      }
      window_mismatch++;
    }
  }

  UART_WriteLine("PSRAM buffer fail window start=%lu words=%lu mismatches=%lu first=%lu exp=0x%08lX got=0x%08lX\r\n",
                 (unsigned long)window_start,
                 (unsigned long)EXT_RAM_FAIL_WINDOW_WORDS,
                 (unsigned long)window_mismatch,
                 (unsigned long)window_first,
                 (unsigned long)window_expected,
                 (unsigned long)window_actual);
}

static uint32_t ExtRam_BufferPattern(uint32_t index, uint32_t pattern_id)
{
  return (pattern_id == 0U) ? ExtRam_BufferPatternA(index) : ExtRam_BufferPatternB(index);
}

static uint32_t ExtRam_BufferPatternA(uint32_t index)
{
  return 0xA5A50000UL ^ (index * 0x10203041UL) ^ (index << 7);
}

static uint32_t ExtRam_BufferPatternB(uint32_t index)
{
  return 0x5A5AFFFFUL ^ (index * 0x01010101UL) ^ (index >> 3);
}
