/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    psram_diag.h
  * @brief   APS256XXN PSRAM bring-up helpers for FSBL diagnostics.
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __PSRAM_DIAG_H__
#define __PSRAM_DIAG_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/*
 * Diagnostic level:
 *   0: disabled
 *   1: configure and verify APS256 mode registers only
 *   2: level 1 + indirect data test + enter XSPI1 memory-mapped mode
 *   3: level 2 + CPU byte-only memory-mapped probe
 *   4: level 3 + CPU write/read a small memory-mapped window
 *
 * Current APS256XXN direct CPU mmap probe can hang before NonSecure UART starts.
 * Keep level 2 as the safe bootable diagnostic level while tuning the access path.
 */
#define FSBL_PSRAM_DIAG_LEVEL           2U
#define FSBL_PSRAM_TEST_ENABLE          (FSBL_PSRAM_DIAG_LEVEL > 0U)
#define FSBL_PSRAM_BLOCK_ON_FAIL        0U
#define FSBL_PSRAM_TEST_WORDS           1U

#define FSBL_PSRAM_DIAG_RECORD_ADDR     0x2417F000UL
#define FSBL_PSRAM_DIAG_RECORD_MAGIC    0x5053524DUL /* "PSRM" */
#define FSBL_PSRAM_DIAG_RECORD_VERSION  3U

#define FSBL_PSRAM_DIAG_RESULT_RUNNING  0U
#define FSBL_PSRAM_DIAG_RESULT_OK       1U
#define FSBL_PSRAM_DIAG_RESULT_FAIL     2U

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

HAL_StatusTypeDef FSBL_PSRAM_InitMemoryMapped(void);
HAL_StatusTypeDef FSBL_PSRAM_Test(uint32_t words);
HAL_StatusTypeDef FSBL_PSRAM_InitAndTest(void);

#ifdef __cplusplus
}
#endif

#endif /* __PSRAM_DIAG_H__ */
