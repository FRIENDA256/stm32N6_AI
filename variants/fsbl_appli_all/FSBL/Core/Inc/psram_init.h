/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    psram_init.h
  * @brief   APS256XXN PSRAM memory-mapped initialization for the FSBL.
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef PSRAM_INIT_H
#define PSRAM_INIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

#define FSBL_PSRAM_BASE_ADDR             0x90000000UL
#define FSBL_PSRAM_SIZE_BYTES            0x02000000UL
#define FSBL_PSRAM_STATUS_ADDR            0x341FE000UL
#define FSBL_PSRAM_STATUS_MAGIC           0x5053524DUL
#define FSBL_PSRAM_STATUS_VERSION         1U

#define FSBL_PSRAM_STATUS_NOT_READY       0U
#define FSBL_PSRAM_STATUS_READY           1U
#define FSBL_PSRAM_STATUS_FAILED          2U

typedef struct
{
  uint32_t magic;
  uint32_t version;
  uint32_t status;
  uint32_t step;
  uint32_t error_code;
  uint32_t hal_error;
  uint32_t base_address;
  uint32_t size_bytes;
  uint32_t test_offset;
  uint32_t test_expected;
  uint32_t test_actual;
} FSBL_PSRAM_StatusRecord_t;

HAL_StatusTypeDef FSBL_PSRAM_InitMemoryMapped(void);

#ifdef __cplusplus
}
#endif

#endif /* PSRAM_INIT_H */
