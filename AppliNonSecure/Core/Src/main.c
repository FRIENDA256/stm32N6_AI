/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "main.h"
#include "gpdma.h"
#include "spi.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef enum
{
  AD_SPI_DMA_IDLE = 0,
  AD_SPI_DMA_HEADER,
  AD_SPI_DMA_PAYLOAD,
  AD_SPI_DMA_FRAME_READY,
  AD_SPI_DMA_ERROR
} AD_SPI_DMA_State_t;

typedef struct
{
  uint32_t window_start_tick;
  uint32_t frame_count;
  uint32_t crc_ok_count;
  uint32_t crc_bad_count;
  uint32_t bad_magic_count;
  uint32_t bad_length_count;
  uint32_t payload_length_warning_count;
  uint32_t dma_error_count;
  uint32_t irq_gap_count;
  uint32_t seq_gap_count;
  uint32_t raw_frame_count;
  uint32_t raw_format_error_count;
  uint32_t raw_block_gap_count;
  uint32_t max_irq_delta;
  uint32_t max_seq_delta;
  uint32_t max_raw_block_gap;
  uint32_t min_frame_dt_ms;
  uint32_t max_frame_dt_ms;
  uint32_t min_sample_delta;
  uint32_t max_sample_delta;
  uint32_t bytes_received;
  uint32_t last_dma_error_code;
  uint32_t last_hal_error;
  uint32_t last_irq;
  uint32_t last_seq;
  uint32_t last_timestamp_ms;
  uint32_t last_sample_counter;
  uint64_t last_raw_block_end;
  uint8_t have_last_irq;
  uint8_t have_last_seq;
  uint8_t have_last_timestamp;
  uint8_t have_last_sample;
  uint8_t have_last_raw_block;
} AD_SPI_QualityStats_t;

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
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define AD_SPI_HEADER_SIZE            24U
#define AD_SPI_MAX_FRAME_SIZE         8240U
#define AD_SPI_CRC_SIZE               4U
#define AD_FRAME_MAGIC                0xAD76U
#define AD_FRAME_TYPE_RAW_SYNC        0x01U
#define AD_RAW_PAYLOAD_HEADER_SIZE    20U
#define AD_RAW_MAX_CHANNELS           8U
#define AD_SPI_DMA_ERROR_BAD_HEADER   1U
#define AD_SPI_DMA_ERROR_BAD_LENGTH   2U
#define AD_SPI_DMA_ERROR_START_HEADER 3U
#define AD_SPI_DMA_ERROR_START_BODY   4U
#define AD_SPI_DMA_ERROR_HAL          5U
#define AD_SPI_QUALITY_REPORT_PERIOD_MS 5000U
#define AD_SPI_IRQ_TO_CS_SETTLE_MS    1U
#define AD_SPI_LED_NO_FRAME_TIMEOUT_MS 1500U
#define AD_SPI_LED_ERROR_HOLD_MS      3000U
#define AD_SPI_LED_GOOD_TOGGLE_MS     500U
#define AD_SPI_LED_ERROR_TOGGLE_MS    100U
#define AD_SPI_VERBOSE_FRAME_LOG      0U
#define AD_SPI_VERBOSE_RAW_LOG        0U
#define EXT_RAM_TEST_ENABLE           0U
#define EXT_RAM_BASE_ADDR             0x90000000UL
#define EXT_RAM_TEST_WORDS            1U
#define FSBL_PSRAM_DIAG_RECORD_ADDR   0x2417F000UL
#define FSBL_PSRAM_DIAG_RECORD_MAGIC  0x5053524DUL
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
static const char uart_start_msg[] = "STM32N6_AI AppNS USART3 start\r\n";
static const char uart_heartbeat_msg[] = "STM32N6_AI AppNS heartbeat\r\n";
static volatile uint32_t ad_irq_count;
static volatile uint8_t ad_irq_pending;
static volatile AD_SPI_DMA_State_t ad_spi_dma_state = AD_SPI_DMA_IDLE;
static volatile uint8_t ad_dma_frame_ready;
static volatile uint8_t ad_dma_error_code;
static volatile uint32_t ad_dma_error_hal;
static volatile uint16_t ad_dma_total_len;
static volatile uint32_t ad_dma_irq_snapshot;
static AD_SPI_QualityStats_t ad_spi_quality;
static uint32_t ad_spi_last_good_frame_tick;
static uint32_t ad_spi_last_error_tick;
static uint32_t ad_spi_led_tick;
static uint8_t ad_spi_led_on;
static uint8_t ad_spi_tx_dummy[AD_SPI_MAX_FRAME_SIZE];
static uint8_t ad_spi_rx_frame[AD_SPI_MAX_FRAME_SIZE];
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */
static void UART_WriteString(const char *text);
static uint16_t ReadLE16(const uint8_t *data);
#if AD_SPI_VERBOSE_RAW_LOG
static int16_t ReadLEI16(const uint8_t *data);
#endif
static uint32_t ReadLE32(const uint8_t *data);
static uint64_t ReadLE64(const uint8_t *data);
static uint32_t AD7606_CalcCRC32(const uint8_t *buf, uint32_t len);
#if AD_SPI_VERBOSE_RAW_LOG
static void AD7606_ParseRawFrame(const uint8_t *payload, uint16_t payload_len);
#endif
static void SPI4_QualityTest_ClearWindow(uint32_t now_tick);
static void SPI4_QualityTest_Task(uint32_t now_tick);
static void SPI4_QualityTest_RecordFrame(uint32_t irq_count_snapshot,
                                         uint32_t frame_seq,
                                         uint16_t total_len,
                                         uint16_t payload_len,
                                         uint32_t timestamp_ms,
                                         uint32_t sample_counter,
                                         uint8_t frame_type,
                                         uint8_t crc_ok);
static void SPI4_QualityTest_RecordDmaError(uint8_t error_code, uint32_t hal_error);
static void SPI4_StatusLED_Init(uint32_t now_tick);
static void SPI4_StatusLED_Task(uint32_t now_tick);
static void SPI4_StatusLED_RecordFrame(uint8_t crc_ok);
static void SPI4_StatusLED_RecordError(void);
static void SPI4_StatusLED_Set(uint8_t on);
static void ExtRam_Test(void);
static void FSBL_PSRAM_DiagReport(void);
static void AD7606_SPI4_StartDmaRead(uint32_t irq_count_snapshot);
static void AD7606_SPI4_ProcessDmaFrame(uint32_t irq_count_snapshot);
static void AD7606_SPI4_ReportDmaError(uint32_t irq_count_snapshot, uint8_t error_code, uint32_t hal_error);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
  uint32_t heartbeat_tick;
  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_GPDMA1_Init();
  MX_SPI4_Init();
  MX_USART3_UART_Init();
  /* USER CODE BEGIN 2 */
  SCB->SHCSR |= (SCB_SHCSR_MEMFAULTENA_Msk | SCB_SHCSR_BUSFAULTENA_Msk | SCB_SHCSR_USGFAULTENA_Msk);
  memset(ad_spi_tx_dummy, 0xFF, sizeof(ad_spi_tx_dummy));
  HAL_GPIO_WritePin(AD_CS_GPIO_Port, AD_CS_Pin, GPIO_PIN_SET);
  UART_WriteString(uart_start_msg);
  FSBL_PSRAM_DiagReport();
  ExtRam_Test();
  heartbeat_tick = HAL_GetTick();
  SPI4_QualityTest_ClearWindow(heartbeat_tick);
  SPI4_StatusLED_Init(heartbeat_tick);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    if (ad_dma_frame_ready != 0U)
    {
      uint32_t irq_count_snapshot;

      __disable_irq();
      ad_dma_frame_ready = 0U;
      irq_count_snapshot = ad_dma_irq_snapshot;
      __enable_irq();

      AD7606_SPI4_ProcessDmaFrame(irq_count_snapshot);

      __disable_irq();
      if (ad_spi_dma_state == AD_SPI_DMA_FRAME_READY)
      {
        ad_spi_dma_state = AD_SPI_DMA_IDLE;
      }
      __enable_irq();
    }

    if (ad_dma_error_code != 0U)
    {
      uint8_t error_code;
      uint32_t hal_error;
      uint32_t irq_count_snapshot;

      __disable_irq();
      error_code = ad_dma_error_code;
      hal_error = ad_dma_error_hal;
      irq_count_snapshot = ad_dma_irq_snapshot;
      ad_dma_error_code = 0U;
      ad_dma_error_hal = 0U;
      if (ad_spi_dma_state == AD_SPI_DMA_ERROR)
      {
        ad_spi_dma_state = AD_SPI_DMA_IDLE;
      }
      __enable_irq();

      AD7606_SPI4_ReportDmaError(irq_count_snapshot, error_code, hal_error);
    }

    if (ad_irq_pending != 0U)
    {
      uint8_t start_dma = 0U;
      uint32_t irq_count_snapshot = 0U;

      __disable_irq();
      if ((ad_irq_pending != 0U) && (ad_spi_dma_state == AD_SPI_DMA_IDLE))
      {
        ad_irq_pending = 0U;
        irq_count_snapshot = ad_irq_count;
        ad_dma_irq_snapshot = irq_count_snapshot;
        ad_spi_dma_state = AD_SPI_DMA_HEADER;
        start_dma = 1U;
      }
      __enable_irq();

      if (start_dma != 0U)
      {
        AD7606_SPI4_StartDmaRead(irq_count_snapshot);
      }
    }

    if ((HAL_GetTick() - heartbeat_tick) >= 1000U)
    {
      heartbeat_tick += 1000U;
      UART_WriteString(uart_heartbeat_msg);
    }

    SPI4_QualityTest_Task(HAL_GetTick());
    SPI4_StatusLED_Task(HAL_GetTick());

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/* USER CODE BEGIN 4 */
static void UART_WriteString(const char *text)
{
  if (text != NULL)
  {
    (void)HAL_UART_Transmit(&huart3, (uint8_t *)text, (uint16_t)strlen(text), 100U);
  }
}

static void FSBL_PSRAM_DiagReport(void)
{
  const volatile FSBL_PSRAM_DiagRecord_t *diag =
      (const volatile FSBL_PSRAM_DiagRecord_t *)FSBL_PSRAM_DIAG_RECORD_ADDR;
  char line[192];
  int len;

  if (diag->magic != FSBL_PSRAM_DIAG_RECORD_MAGIC)
  {
    UART_WriteString("FSBL PSRAM diag not available\r\n");
    return;
  }

  len = snprintf(line, sizeof(line),
                 "FSBL PSRAM diag ver=%lu level=%lu result=%lu step=%lu err=%lu hal=0x%08lX\r\n",
                 (unsigned long)diag->version,
                 (unsigned long)diag->diag_level,
                 (unsigned long)diag->result,
                 (unsigned long)diag->step,
                 (unsigned long)diag->error_code,
                 (unsigned long)diag->hal_error);
  if (len > 0)
  {
    (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
  }

  len = snprintf(line, sizeof(line),
                 "FSBL PSRAM MR0 w=%02X %02X r=%02X %02X, MR4 w=%02X %02X r=%02X %02X, MR8 w=%02X %02X r=%02X %02X\r\n",
                 diag->mr0_w[0], diag->mr0_w[1], diag->mr0_r[0], diag->mr0_r[1],
                 diag->mr4_w[0], diag->mr4_w[1], diag->mr4_r[0], diag->mr4_r[1],
                 diag->mr8_w[0], diag->mr8_w[1], diag->mr8_r[0], diag->mr8_r[1]);
  if (len > 0)
  {
    (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
  }

  len = snprintf(line, sizeof(line),
                 "FSBL PSRAM indirect len=%lu tx=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
                 (unsigned long)diag->indirect_len,
                 diag->indirect_tx[0], diag->indirect_tx[1], diag->indirect_tx[2], diag->indirect_tx[3],
                 diag->indirect_tx[4], diag->indirect_tx[5], diag->indirect_tx[6], diag->indirect_tx[7],
                 diag->indirect_tx[8], diag->indirect_tx[9], diag->indirect_tx[10], diag->indirect_tx[11],
                 diag->indirect_tx[12], diag->indirect_tx[13], diag->indirect_tx[14], diag->indirect_tx[15]);
  if (len > 0)
  {
    (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
  }

  len = snprintf(line, sizeof(line),
                 "FSBL PSRAM indirect rx=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
                 diag->indirect_rx[0], diag->indirect_rx[1], diag->indirect_rx[2], diag->indirect_rx[3],
                 diag->indirect_rx[4], diag->indirect_rx[5], diag->indirect_rx[6], diag->indirect_rx[7],
                 diag->indirect_rx[8], diag->indirect_rx[9], diag->indirect_rx[10], diag->indirect_rx[11],
                 diag->indirect_rx[12], diag->indirect_rx[13], diag->indirect_rx[14], diag->indirect_rx[15]);
  if (len > 0)
  {
    (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
  }

  if (diag->version >= 2UL)
  {
    len = snprintf(line, sizeof(line),
                   "FSBL XSPI before mmap CR=0x%08lX SR=0x%08lX DCR1=0x%08lX DCR2=0x%08lX DCR3=0x%08lX DCR4=0x%08lX\r\n",
                   (unsigned long)diag->xspi_cr_before_mmap,
                   (unsigned long)diag->xspi_sr_before_mmap,
                   (unsigned long)diag->xspi_dcr1_before_mmap,
                   (unsigned long)diag->xspi_dcr2_before_mmap,
                   (unsigned long)diag->xspi_dcr3_before_mmap,
                   (unsigned long)diag->xspi_dcr4_before_mmap);
    if (len > 0)
    {
      (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
    }

    len = snprintf(line, sizeof(line),
                   "FSBL XSPI before mmap CCR=0x%08lX TCR=0x%08lX WCCR=0x%08lX WTCR=0x%08lX\r\n",
                   (unsigned long)diag->xspi_ccr_before_mmap,
                   (unsigned long)diag->xspi_tcr_before_mmap,
                   (unsigned long)diag->xspi_wccr_before_mmap,
                   (unsigned long)diag->xspi_wtcr_before_mmap);
    if (len > 0)
    {
      (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
    }

    len = snprintf(line, sizeof(line),
                   "FSBL XSPI after mmap CR=0x%08lX SR=0x%08lX DCR1=0x%08lX DCR2=0x%08lX DCR3=0x%08lX DCR4=0x%08lX\r\n",
                   (unsigned long)diag->xspi_cr_after_mmap,
                   (unsigned long)diag->xspi_sr_after_mmap,
                   (unsigned long)diag->xspi_dcr1_after_mmap,
                   (unsigned long)diag->xspi_dcr2_after_mmap,
                   (unsigned long)diag->xspi_dcr3_after_mmap,
                   (unsigned long)diag->xspi_dcr4_after_mmap);
    if (len > 0)
    {
      (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
    }

    len = snprintf(line, sizeof(line),
                   "FSBL XSPI after mmap CCR=0x%08lX TCR=0x%08lX WCCR=0x%08lX WTCR=0x%08lX\r\n",
                   (unsigned long)diag->xspi_ccr_after_mmap,
                   (unsigned long)diag->xspi_tcr_after_mmap,
                   (unsigned long)diag->xspi_wccr_after_mmap,
                   (unsigned long)diag->xspi_wtcr_after_mmap);
    if (len > 0)
    {
      (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
    }
  }

  if (diag->version >= 3UL)
  {
    len = snprintf(line, sizeof(line),
                   "FSBL RCC AHB3ENR=0x%08lX AHB3ENSR=0x%08lX AHB5ENR=0x%08lX AHB5ENSR=0x%08lX\r\n",
                   (unsigned long)diag->rcc_ahb3enr,
                   (unsigned long)diag->rcc_ahb3ensr,
                   (unsigned long)diag->rcc_ahb5enr,
                   (unsigned long)diag->rcc_ahb5ensr);
    if (len > 0)
    {
      (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
    }

    len = snprintf(line, sizeof(line),
                   "FSBL path XSPIM_CR=0x%08lX MCE1_CLK=%lu RISAF_CLK=%lu\r\n",
                   (unsigned long)diag->xspim_cr,
                   (unsigned long)diag->mce1_clock_enabled,
                   (unsigned long)diag->risaf_clock_enabled);
    if (len > 0)
    {
      (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
    }

    len = snprintf(line, sizeof(line),
                   "FSBL MCE1 CR=0x%08lX SR=0x%08lX IASR=0x%08lX IADDR=0x%08lX\r\n",
                   (unsigned long)diag->mce1_cr,
                   (unsigned long)diag->mce1_sr,
                   (unsigned long)diag->mce1_iasr,
                   (unsigned long)diag->mce1_iaddr);
    if (len > 0)
    {
      (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
    }

    len = snprintf(line, sizeof(line),
                   "FSBL MCE1 R1 REGCR=0x%08lX SADDR=0x%08lX EADDR=0x%08lX\r\n",
                   (unsigned long)diag->mce1_region1_regcr,
                   (unsigned long)diag->mce1_region1_saddr,
                   (unsigned long)diag->mce1_region1_eaddr);
    if (len > 0)
    {
      (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
    }

    len = snprintf(line, sizeof(line),
                   "FSBL RISAF11 CR=0x%08lX IASR=0x%08lX IAESR=0x%08lX IADDR=0x%08lX\r\n",
                   (unsigned long)diag->risaf11_cr,
                   (unsigned long)diag->risaf11_iasr,
                   (unsigned long)diag->risaf11_iaesr,
                   (unsigned long)diag->risaf11_iaddr);
    if (len > 0)
    {
      (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
    }

    len = snprintf(line, sizeof(line),
                   "FSBL RISAF11 R1 CFGR=0x%08lX START=0x%08lX END=0x%08lX CID=0x%08lX\r\n",
                   (unsigned long)diag->risaf11_region1_cfgr,
                   (unsigned long)diag->risaf11_region1_startr,
                   (unsigned long)diag->risaf11_region1_endr,
                   (unsigned long)diag->risaf11_region1_cidcfgr);
    if (len > 0)
    {
      (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
    }

    len = snprintf(line, sizeof(line),
                   "FSBL SAU CTRL=0x%08lX TYPE=0x%08lX RNR=0x%08lX RBAR=0x%08lX RLAR=0x%08lX\r\n",
                   (unsigned long)diag->sau_ctrl,
                   (unsigned long)diag->sau_type,
                   (unsigned long)diag->sau_rnr,
                   (unsigned long)diag->sau_rbar,
                   (unsigned long)diag->sau_rlar);
    if (len > 0)
    {
      (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
    }

    len = snprintf(line, sizeof(line),
                   "FSBL MPU CTRL=0x%08lX TYPE=0x%08lX\r\n",
                   (unsigned long)diag->mpu_ctrl,
                   (unsigned long)diag->mpu_type);
    if (len > 0)
    {
      (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
    }
  }

  if (diag->result == 2UL)
  {
    len = snprintf(line, sizeof(line),
                   "FSBL PSRAM fail index=%lu expected=0x%08lX actual=0x%08lX test_words=%lu\r\n",
                   (unsigned long)diag->fail_index,
                   (unsigned long)diag->expected,
                   (unsigned long)diag->actual,
                   (unsigned long)diag->test_words);
    if (len > 0)
    {
      (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
    }
  }
}

static uint16_t ReadLE16(const uint8_t *data)
{
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

#if AD_SPI_VERBOSE_RAW_LOG
static int16_t ReadLEI16(const uint8_t *data)
{
  return (int16_t)ReadLE16(data);
}
#endif

static uint32_t ReadLE32(const uint8_t *data)
{
  return (uint32_t)data[0] |
         ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) |
         ((uint32_t)data[3] << 24);
}

static uint64_t ReadLE64(const uint8_t *data)
{
  return (uint64_t)ReadLE32(data) | ((uint64_t)ReadLE32(&data[4]) << 32);
}

static uint32_t AD7606_CalcCRC32(const uint8_t *buf, uint32_t len)
{
  uint32_t crc = 0xFFFFFFFFU;

  for (uint32_t i = 0; i < len; i++)
  {
    crc ^= buf[i];
    for (uint32_t bit = 0; bit < 8U; bit++)
    {
      if ((crc & 1U) != 0U)
      {
        crc = (crc >> 1) ^ 0xEDB88320U;
      }
      else
      {
        crc >>= 1;
      }
    }
  }

  return crc ^ 0xFFFFFFFFU;
}

#if AD_SPI_VERBOSE_RAW_LOG
static void AD7606_ParseRawFrame(const uint8_t *payload, uint16_t payload_len)
{
  char line[256];
  uint16_t points;
  uint8_t channels;
  uint8_t bytes_per_sample;
  uint64_t block_start;
  uint64_t block_end;
  uint32_t expected_payload_len;
  int16_t first[AD_RAW_MAX_CHANNELS] = {0};
  int16_t last[AD_RAW_MAX_CHANNELS] = {0};
  int16_t min_value[AD_RAW_MAX_CHANNELS] = {0};
  int16_t max_value[AD_RAW_MAX_CHANNELS] = {0};
  int len;

  if (payload_len < AD_RAW_PAYLOAD_HEADER_SIZE)
  {
    len = snprintf(line, sizeof(line), "RAW payload too short len=%u\r\n", (unsigned int)payload_len);
    if (len > 0)
    {
      (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
    }
    return;
  }

  points = ReadLE16(&payload[0]);
  channels = payload[2];
  bytes_per_sample = payload[3];
  block_start = ReadLE64(&payload[4]);
  block_end = ReadLE64(&payload[12]);

  if ((points == 0U) || (channels == 0U) || (channels > AD_RAW_MAX_CHANNELS) || (bytes_per_sample != 2U))
  {
    len = snprintf(line, sizeof(line), "RAW unsupported points=%u ch=%u bytes=%u\r\n",
                   (unsigned int)points, (unsigned int)channels, (unsigned int)bytes_per_sample);
    if (len > 0)
    {
      (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
    }
    return;
  }

  expected_payload_len = AD_RAW_PAYLOAD_HEADER_SIZE + ((uint32_t)points * channels * bytes_per_sample);
  if (payload_len < expected_payload_len)
  {
    len = snprintf(line, sizeof(line), "RAW length too short len=%u expected=%lu\r\n",
                   (unsigned int)payload_len, (unsigned long)expected_payload_len);
    if (len > 0)
    {
      (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
    }
    return;
  }

  for (uint8_t ch = 0; ch < channels; ch++)
  {
    int16_t sample = ReadLEI16(&payload[AD_RAW_PAYLOAD_HEADER_SIZE + (uint32_t)ch * 2U]);
    first[ch] = sample;
    last[ch] = sample;
    min_value[ch] = sample;
    max_value[ch] = sample;
  }

  for (uint16_t point = 0; point < points; point++)
  {
    for (uint8_t ch = 0; ch < channels; ch++)
    {
      uint32_t offset = AD_RAW_PAYLOAD_HEADER_SIZE + (((uint32_t)point * channels + ch) * 2U);
      int16_t sample = ReadLEI16(&payload[offset]);

      if (sample < min_value[ch])
      {
        min_value[ch] = sample;
      }
      if (sample > max_value[ch])
      {
        max_value[ch] = sample;
      }
      if (point == (uint16_t)(points - 1U))
      {
        last[ch] = sample;
      }
    }
  }

  len = snprintf(line, sizeof(line),
                 "RAW points=%u ch=%u bytes=%u blk_start=0x%08lX%08lX blk_end=0x%08lX%08lX\r\n",
                 (unsigned int)points,
                 (unsigned int)channels,
                 (unsigned int)bytes_per_sample,
                 (unsigned long)(block_start >> 32),
                 (unsigned long)(block_start & 0xFFFFFFFFU),
                 (unsigned long)(block_end >> 32),
                 (unsigned long)(block_end & 0xFFFFFFFFU));
  if (len > 0)
  {
    (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
  }

  len = snprintf(line, sizeof(line), "RAW first=%d,%d,%d,%d,%d,%d,%d,%d\r\n",
                 first[0], first[1], first[2], first[3], first[4], first[5], first[6], first[7]);
  if (len > 0)
  {
    (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
  }

  len = snprintf(line, sizeof(line), "RAW last=%d,%d,%d,%d,%d,%d,%d,%d\r\n",
                 last[0], last[1], last[2], last[3], last[4], last[5], last[6], last[7]);
  if (len > 0)
  {
    (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
  }

  len = snprintf(line, sizeof(line),
                 "RAW min/max=[%d,%d] [%d,%d] [%d,%d] [%d,%d] [%d,%d] [%d,%d] [%d,%d] [%d,%d]\r\n",
                 min_value[0], max_value[0], min_value[1], max_value[1],
                 min_value[2], max_value[2], min_value[3], max_value[3],
                 min_value[4], max_value[4], min_value[5], max_value[5],
                 min_value[6], max_value[6], min_value[7], max_value[7]);
  if (len > 0)
  {
    (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
  }
}
#endif

static void SPI4_QualityTest_ClearWindow(uint32_t now_tick)
{
  ad_spi_quality.window_start_tick = now_tick;
  ad_spi_quality.frame_count = 0U;
  ad_spi_quality.crc_ok_count = 0U;
  ad_spi_quality.crc_bad_count = 0U;
  ad_spi_quality.bad_magic_count = 0U;
  ad_spi_quality.bad_length_count = 0U;
  ad_spi_quality.payload_length_warning_count = 0U;
  ad_spi_quality.dma_error_count = 0U;
  ad_spi_quality.irq_gap_count = 0U;
  ad_spi_quality.seq_gap_count = 0U;
  ad_spi_quality.raw_frame_count = 0U;
  ad_spi_quality.raw_format_error_count = 0U;
  ad_spi_quality.raw_block_gap_count = 0U;
  ad_spi_quality.max_irq_delta = 0U;
  ad_spi_quality.max_seq_delta = 0U;
  ad_spi_quality.max_raw_block_gap = 0U;
  ad_spi_quality.min_frame_dt_ms = 0xFFFFFFFFU;
  ad_spi_quality.max_frame_dt_ms = 0U;
  ad_spi_quality.min_sample_delta = 0xFFFFFFFFU;
  ad_spi_quality.max_sample_delta = 0U;
  ad_spi_quality.bytes_received = 0U;
  ad_spi_quality.last_dma_error_code = 0U;
  ad_spi_quality.last_hal_error = 0U;
}

static void SPI4_QualityTest_Task(uint32_t now_tick)
{
  char line[256];
  uint32_t elapsed_ms = now_tick - ad_spi_quality.window_start_tick;
  uint32_t bytes_per_sec;
  uint32_t min_frame_dt_ms;
  uint32_t min_sample_delta;
  int len;

  if (elapsed_ms < AD_SPI_QUALITY_REPORT_PERIOD_MS)
  {
    return;
  }

  if (elapsed_ms == 0U)
  {
    elapsed_ms = 1U;
  }

  bytes_per_sec = (uint32_t)(((uint64_t)ad_spi_quality.bytes_received * 1000ULL) / elapsed_ms);
  min_frame_dt_ms = (ad_spi_quality.min_frame_dt_ms == 0xFFFFFFFFU) ? 0U : ad_spi_quality.min_frame_dt_ms;
  min_sample_delta = (ad_spi_quality.min_sample_delta == 0xFFFFFFFFU) ? 0U : ad_spi_quality.min_sample_delta;

  len = snprintf(line, sizeof(line),
                 "SPI4 quality win=%lums frame=%lu crc_ok=%lu crc_bad=%lu dma_err=%lu bad_hdr=%lu bad_len=%lu len_warn=%lu Bps=%lu\r\n",
                 (unsigned long)elapsed_ms,
                 (unsigned long)ad_spi_quality.frame_count,
                 (unsigned long)ad_spi_quality.crc_ok_count,
                 (unsigned long)ad_spi_quality.crc_bad_count,
                 (unsigned long)ad_spi_quality.dma_error_count,
                 (unsigned long)ad_spi_quality.bad_magic_count,
                 (unsigned long)ad_spi_quality.bad_length_count,
                 (unsigned long)ad_spi_quality.payload_length_warning_count,
                 (unsigned long)bytes_per_sec);
  if (len > 0)
  {
    (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
  }

  len = snprintf(line, sizeof(line),
                 "SPI4 quality gap irq=%lu max_irq_delta=%lu seq=%lu max_seq_delta=%lu raw_gap=%lu max_raw_gap=%lu raw_bad=%lu dt_ms=[%lu,%lu] sample_delta=[%lu,%lu]\r\n",
                 (unsigned long)ad_spi_quality.irq_gap_count,
                 (unsigned long)ad_spi_quality.max_irq_delta,
                 (unsigned long)ad_spi_quality.seq_gap_count,
                 (unsigned long)ad_spi_quality.max_seq_delta,
                 (unsigned long)ad_spi_quality.raw_block_gap_count,
                 (unsigned long)ad_spi_quality.max_raw_block_gap,
                 (unsigned long)ad_spi_quality.raw_format_error_count,
                 (unsigned long)min_frame_dt_ms,
                 (unsigned long)ad_spi_quality.max_frame_dt_ms,
                 (unsigned long)min_sample_delta,
                 (unsigned long)ad_spi_quality.max_sample_delta);
  if (len > 0)
  {
    (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
  }

  if (ad_spi_quality.dma_error_count != 0U)
  {
    len = snprintf(line, sizeof(line), "SPI4 quality last_dma_error code=%lu hal=0x%08lX\r\n",
                   (unsigned long)ad_spi_quality.last_dma_error_code,
                   (unsigned long)ad_spi_quality.last_hal_error);
    if (len > 0)
    {
      (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
    }
  }

  SPI4_QualityTest_ClearWindow(now_tick);
}

static void SPI4_QualityTest_RecordFrame(uint32_t irq_count_snapshot,
                                         uint32_t frame_seq,
                                         uint16_t total_len,
                                         uint16_t payload_len,
                                         uint32_t timestamp_ms,
                                         uint32_t sample_counter,
                                         uint8_t frame_type,
                                         uint8_t crc_ok)
{
  ad_spi_quality.frame_count++;
  ad_spi_quality.bytes_received += total_len;

  if (crc_ok != 0U)
  {
    ad_spi_quality.crc_ok_count++;
    SPI4_StatusLED_RecordFrame(1U);
  }
  else
  {
    ad_spi_quality.crc_bad_count++;
    SPI4_StatusLED_RecordFrame(0U);
  }

  if (ad_spi_quality.have_last_irq != 0U)
  {
    uint32_t delta = irq_count_snapshot - ad_spi_quality.last_irq;
    if (delta != 1U)
    {
      ad_spi_quality.irq_gap_count++;
      if (delta > ad_spi_quality.max_irq_delta)
      {
        ad_spi_quality.max_irq_delta = delta;
      }
    }
  }
  ad_spi_quality.last_irq = irq_count_snapshot;
  ad_spi_quality.have_last_irq = 1U;

  if (ad_spi_quality.have_last_seq != 0U)
  {
    uint32_t delta = frame_seq - ad_spi_quality.last_seq;
    if (delta != 1U)
    {
      ad_spi_quality.seq_gap_count++;
      if (delta > ad_spi_quality.max_seq_delta)
      {
        ad_spi_quality.max_seq_delta = delta;
      }
    }
  }
  ad_spi_quality.last_seq = frame_seq;
  ad_spi_quality.have_last_seq = 1U;

  if (ad_spi_quality.have_last_timestamp != 0U)
  {
    uint32_t delta = timestamp_ms - ad_spi_quality.last_timestamp_ms;
    if (delta < ad_spi_quality.min_frame_dt_ms)
    {
      ad_spi_quality.min_frame_dt_ms = delta;
    }
    if (delta > ad_spi_quality.max_frame_dt_ms)
    {
      ad_spi_quality.max_frame_dt_ms = delta;
    }
  }
  ad_spi_quality.last_timestamp_ms = timestamp_ms;
  ad_spi_quality.have_last_timestamp = 1U;

  if (ad_spi_quality.have_last_sample != 0U)
  {
    uint32_t delta = sample_counter - ad_spi_quality.last_sample_counter;
    if (delta < ad_spi_quality.min_sample_delta)
    {
      ad_spi_quality.min_sample_delta = delta;
    }
    if (delta > ad_spi_quality.max_sample_delta)
    {
      ad_spi_quality.max_sample_delta = delta;
    }
  }
  ad_spi_quality.last_sample_counter = sample_counter;
  ad_spi_quality.have_last_sample = 1U;

  if ((frame_type == AD_FRAME_TYPE_RAW_SYNC) && (payload_len >= AD_RAW_PAYLOAD_HEADER_SIZE))
  {
    const uint8_t *payload = &ad_spi_rx_frame[AD_SPI_HEADER_SIZE];
    uint16_t points = ReadLE16(&payload[0]);
    uint8_t channels = payload[2];
    uint8_t bytes_per_sample = payload[3];
    uint64_t block_start = ReadLE64(&payload[4]);
    uint64_t block_end = ReadLE64(&payload[12]);
    uint32_t expected_payload_len = AD_RAW_PAYLOAD_HEADER_SIZE + ((uint32_t)points * channels * bytes_per_sample);

    if ((points == 0U) || (channels == 0U) || (channels > AD_RAW_MAX_CHANNELS) ||
        (bytes_per_sample != 2U) || (payload_len < expected_payload_len))
    {
      ad_spi_quality.raw_format_error_count++;
    }
    else
    {
      ad_spi_quality.raw_frame_count++;
      if (ad_spi_quality.have_last_raw_block != 0U)
      {
        if (block_start != (ad_spi_quality.last_raw_block_end + 1ULL))
        {
          uint64_t gap = 0ULL;

          ad_spi_quality.raw_block_gap_count++;
          if (block_start > (ad_spi_quality.last_raw_block_end + 1ULL))
          {
            gap = block_start - ad_spi_quality.last_raw_block_end - 1ULL;
          }
          if (gap > 0xFFFFFFFFULL)
          {
            gap = 0xFFFFFFFFULL;
          }
          if ((uint32_t)gap > ad_spi_quality.max_raw_block_gap)
          {
            ad_spi_quality.max_raw_block_gap = (uint32_t)gap;
          }
        }
      }
      ad_spi_quality.last_raw_block_end = block_end;
      ad_spi_quality.have_last_raw_block = 1U;
    }
  }
}

static void SPI4_QualityTest_RecordDmaError(uint8_t error_code, uint32_t hal_error)
{
  ad_spi_quality.dma_error_count++;
  ad_spi_quality.last_dma_error_code = error_code;
  ad_spi_quality.last_hal_error = hal_error;
  SPI4_StatusLED_RecordError();

  if (error_code == AD_SPI_DMA_ERROR_BAD_HEADER)
  {
    ad_spi_quality.bad_magic_count++;
  }
  else if (error_code == AD_SPI_DMA_ERROR_BAD_LENGTH)
  {
    ad_spi_quality.bad_length_count++;
  }
}

static void SPI4_StatusLED_Init(uint32_t now_tick)
{
  ad_spi_last_good_frame_tick = 0U;
  ad_spi_last_error_tick = 0U;
  ad_spi_led_tick = now_tick;
  ad_spi_led_on = 0U;
  SPI4_StatusLED_Set(0U);
}

static void SPI4_StatusLED_Task(uint32_t now_tick)
{
  uint32_t toggle_period_ms;

  if ((ad_spi_last_good_frame_tick == 0U) ||
      ((now_tick - ad_spi_last_good_frame_tick) > AD_SPI_LED_NO_FRAME_TIMEOUT_MS))
  {
    SPI4_StatusLED_Set(0U);
    ad_spi_led_tick = now_tick;
    return;
  }

  if ((ad_spi_last_error_tick != 0U) &&
      ((now_tick - ad_spi_last_error_tick) <= AD_SPI_LED_ERROR_HOLD_MS))
  {
    toggle_period_ms = AD_SPI_LED_ERROR_TOGGLE_MS;
  }
  else
  {
    toggle_period_ms = AD_SPI_LED_GOOD_TOGGLE_MS;
  }

  if ((now_tick - ad_spi_led_tick) >= toggle_period_ms)
  {
    ad_spi_led_tick = now_tick;
    SPI4_StatusLED_Set((ad_spi_led_on == 0U) ? 1U : 0U);
  }
}

static void SPI4_StatusLED_RecordFrame(uint8_t crc_ok)
{
  if (crc_ok != 0U)
  {
    ad_spi_last_good_frame_tick = HAL_GetTick();
  }
  else
  {
    SPI4_StatusLED_RecordError();
  }
}

static void SPI4_StatusLED_RecordError(void)
{
  ad_spi_last_error_tick = HAL_GetTick();
}

static void SPI4_StatusLED_Set(uint8_t on)
{
  ad_spi_led_on = (on != 0U) ? 1U : 0U;
  HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, (ad_spi_led_on != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void ExtRam_Test(void)
{
#if EXT_RAM_TEST_ENABLE
  volatile uint8_t *ram8 = (volatile uint8_t *)EXT_RAM_BASE_ADDR;
  volatile uint32_t *ram32 = (volatile uint32_t *)EXT_RAM_BASE_ADDR;
  char line[128];
  uint8_t expected8 = 0xA5U;
  uint32_t expected32 = 0x5A5A1234U;
  uint8_t actual8;
  uint32_t actual32;

  UART_WriteString("EXT RAM test begin\r\n");
  HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET);

  UART_WriteString("EXT RAM byte write probe\r\n");
  ram8[0] = expected8;
  __DSB();
  HAL_Delay(1U);

  UART_WriteString("EXT RAM byte read probe\r\n");
  actual8 = ram8[0];

  if (actual8 != expected8)
  {
    int len = snprintf(line, sizeof(line),
                       "EXT RAM byte FAIL addr=0x%08lX exp=0x%02X got=0x%02X\r\n",
                       (unsigned long)EXT_RAM_BASE_ADDR,
                       expected8,
                       actual8);
    if (len > 0)
    {
      (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
    }
    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);
    return;
  }

  UART_WriteString("EXT RAM word write probe\r\n");
  ram32[0] = expected32;
  __DSB();
  HAL_Delay(1U);

  UART_WriteString("EXT RAM word read probe\r\n");
  actual32 = ram32[0];

  if (actual32 != expected32)
  {
    int len = snprintf(line, sizeof(line),
                       "EXT RAM word FAIL addr=0x%08lX exp=0x%08lX got=0x%08lX\r\n",
                       (unsigned long)EXT_RAM_BASE_ADDR,
                       (unsigned long)expected32,
                       (unsigned long)actual32);
    if (len > 0)
    {
      (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
    }
    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);
    return;
  }

  ram32[0] = 0x00000000U;
  __DSB();

  int len = snprintf(line, sizeof(line),
                     "EXT RAM test OK base=0x%08lX bytes=%lu\r\n",
                     (unsigned long)EXT_RAM_BASE_ADDR,
                     (unsigned long)sizeof(uint32_t));
  if (len > 0)
  {
    (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
  }
  HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);
#endif
}

static void AD7606_SPI4_StartDmaRead(uint32_t irq_count_snapshot)
{
  HAL_StatusTypeDef status;

  ad_dma_irq_snapshot = irq_count_snapshot;
  ad_dma_total_len = 0U;
  memset(ad_spi_rx_frame, 0x00, AD_SPI_HEADER_SIZE);

#if AD_SPI_IRQ_TO_CS_SETTLE_MS > 0U
  HAL_Delay(AD_SPI_IRQ_TO_CS_SETTLE_MS);
#endif

  HAL_GPIO_WritePin(AD_CS_GPIO_Port, AD_CS_Pin, GPIO_PIN_RESET);
  status = HAL_SPI_TransmitReceive_DMA(&hspi4, ad_spi_tx_dummy, ad_spi_rx_frame, AD_SPI_HEADER_SIZE);
  if (status != HAL_OK)
  {
    HAL_GPIO_WritePin(AD_CS_GPIO_Port, AD_CS_Pin, GPIO_PIN_SET);
    __disable_irq();
    ad_dma_error_code = AD_SPI_DMA_ERROR_START_HEADER;
    ad_dma_error_hal = HAL_SPI_GetError(&hspi4);
    ad_spi_dma_state = AD_SPI_DMA_ERROR;
    __enable_irq();
  }
}

static void AD7606_SPI4_ProcessDmaFrame(uint32_t irq_count_snapshot)
{
#if AD_SPI_VERBOSE_FRAME_LOG
  char line[224];
#endif
  uint16_t magic = ReadLE16(&ad_spi_rx_frame[0]);
  uint16_t total_len = ReadLE16(&ad_spi_rx_frame[4]);
  uint16_t payload_len = ReadLE16(&ad_spi_rx_frame[6]);
  uint32_t frame_seq = ReadLE32(&ad_spi_rx_frame[8]);
  uint32_t timestamp_ms = ReadLE32(&ad_spi_rx_frame[12]);
  uint32_t sample_counter = ReadLE32(&ad_spi_rx_frame[16]);
  uint32_t crc_offset;
  uint32_t crc_expected;
  uint32_t crc_actual;
#if AD_SPI_VERBOSE_FRAME_LOG
  int len;
#endif

  if (magic != AD_FRAME_MAGIC)
  {
    ad_spi_quality.bad_magic_count++;
#if AD_SPI_VERBOSE_FRAME_LOG
    len = snprintf(line, sizeof(line), "AD7606 DMA bad magic irq=%lu magic=0x%04X header=",
                   (unsigned long)irq_count_snapshot, magic);
    if (len > 0)
    {
      (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
    }
    for (uint32_t i = 0; i < AD_SPI_HEADER_SIZE; i++)
    {
      len = snprintf(line, sizeof(line), "%02X%s", ad_spi_rx_frame[i], (i + 1U == AD_SPI_HEADER_SIZE) ? "\r\n" : " ");
      if (len > 0)
      {
        (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
      }
    }
#endif
    return;
  }

  if ((total_len < (AD_SPI_HEADER_SIZE + AD_SPI_CRC_SIZE)) || (total_len > AD_SPI_MAX_FRAME_SIZE))
  {
    ad_spi_quality.bad_length_count++;
#if AD_SPI_VERBOSE_FRAME_LOG
    len = snprintf(line, sizeof(line), "AD7606 DMA invalid total_len=%u payload_len=%u\r\n",
                   (unsigned int)total_len, (unsigned int)payload_len);
    if (len > 0)
    {
      (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
    }
#endif
    return;
  }

  crc_offset = (uint32_t)total_len - AD_SPI_CRC_SIZE;
  crc_expected = ReadLE32(&ad_spi_rx_frame[crc_offset]);
  crc_actual = AD7606_CalcCRC32(ad_spi_rx_frame, crc_offset);

  SPI4_QualityTest_RecordFrame(irq_count_snapshot,
                               frame_seq,
                               total_len,
                               payload_len,
                               timestamp_ms,
                               sample_counter,
                               ad_spi_rx_frame[3],
                               (crc_actual == crc_expected) ? 1U : 0U);

#if AD_SPI_VERBOSE_FRAME_LOG
  len = snprintf(line, sizeof(line),
                 "AD7606 DMA frame %s irq=%lu type=0x%02X ver=%u seq=%lu total=%u payload=%u ts=%lu sample=%lu crc_exp=0x%08lX crc_calc=0x%08lX\r\n",
                 (crc_actual == crc_expected) ? "CRC_OK" : "CRC_BAD",
                 (unsigned long)irq_count_snapshot,
                 ad_spi_rx_frame[3],
                 (unsigned int)ad_spi_rx_frame[2],
                 (unsigned long)frame_seq,
                 (unsigned int)total_len,
                 (unsigned int)payload_len,
                 (unsigned long)timestamp_ms,
                 (unsigned long)sample_counter,
                 (unsigned long)crc_expected,
                 (unsigned long)crc_actual);
  if (len > 0)
  {
    (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
  }
#endif

  if (payload_len != (uint16_t)(total_len - AD_SPI_HEADER_SIZE - AD_SPI_CRC_SIZE))
  {
    ad_spi_quality.payload_length_warning_count++;
#if AD_SPI_VERBOSE_FRAME_LOG
    len = snprintf(line, sizeof(line), "AD7606 DMA length warning: payload_len=%u expected=%u\r\n",
                   (unsigned int)payload_len,
                   (unsigned int)(total_len - AD_SPI_HEADER_SIZE - AD_SPI_CRC_SIZE));
    if (len > 0)
    {
      (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
    }
#endif
    return;
  }

#if AD_SPI_VERBOSE_RAW_LOG
  if ((crc_actual == crc_expected) && (ad_spi_rx_frame[3] == AD_FRAME_TYPE_RAW_SYNC))
  {
    AD7606_ParseRawFrame(&ad_spi_rx_frame[AD_SPI_HEADER_SIZE], payload_len);
  }
#endif
}

static void AD7606_SPI4_ReportDmaError(uint32_t irq_count_snapshot, uint8_t error_code, uint32_t hal_error)
{
  char line[192];
  int len = snprintf(line, sizeof(line), "SPI4 DMA error irq=%lu code=%u hal=0x%08lX\r\n",
                     (unsigned long)irq_count_snapshot,
                     (unsigned int)error_code,
                     (unsigned long)hal_error);
  SPI4_QualityTest_RecordDmaError(error_code, hal_error);
  if (len > 0)
  {
    (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)len, 100U);
  }

  if (error_code == AD_SPI_DMA_ERROR_BAD_HEADER)
  {
    int pos = snprintf(line, sizeof(line),
                       "SPI4 DMA bad header magic_le=0x%04X ver=%u type=0x%02X total=%u payload=%u bytes=",
                       ReadLE16(ad_spi_rx_frame),
                       (unsigned int)ad_spi_rx_frame[2],
                       (unsigned int)ad_spi_rx_frame[3],
                       (unsigned int)ReadLE16(&ad_spi_rx_frame[4]),
                       (unsigned int)ReadLE16(&ad_spi_rx_frame[6]));

    for (uint32_t i = 0; (i < AD_SPI_HEADER_SIZE) && (pos > 0) && ((uint32_t)pos < sizeof(line)); i++)
    {
      int written = snprintf(&line[pos], sizeof(line) - (uint32_t)pos,
                             "%02X%s",
                             ad_spi_rx_frame[i],
                             (i + 1U == AD_SPI_HEADER_SIZE) ? "\r\n" : " ");
      if (written <= 0)
      {
        break;
      }
      pos += written;
    }

    if (pos > 0)
    {
      (void)HAL_UART_Transmit(&huart3, (uint8_t *)line, (uint16_t)strlen(line), 100U);
    }
  }
}

void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
  if (hspi->Instance != SPI4)
  {
    return;
  }

  if (ad_spi_dma_state == AD_SPI_DMA_HEADER)
  {
    uint16_t magic = ReadLE16(&ad_spi_rx_frame[0]);
    uint16_t total_len = ReadLE16(&ad_spi_rx_frame[4]);
    uint16_t remaining_len;
    HAL_StatusTypeDef status;

    if (magic != AD_FRAME_MAGIC)
    {
      HAL_GPIO_WritePin(AD_CS_GPIO_Port, AD_CS_Pin, GPIO_PIN_SET);
      ad_dma_error_code = AD_SPI_DMA_ERROR_BAD_HEADER;
      ad_dma_error_hal = 0U;
      ad_spi_dma_state = AD_SPI_DMA_ERROR;
      return;
    }

    if ((total_len < (AD_SPI_HEADER_SIZE + AD_SPI_CRC_SIZE)) || (total_len > AD_SPI_MAX_FRAME_SIZE))
    {
      HAL_GPIO_WritePin(AD_CS_GPIO_Port, AD_CS_Pin, GPIO_PIN_SET);
      ad_dma_error_code = AD_SPI_DMA_ERROR_BAD_LENGTH;
      ad_dma_error_hal = 0U;
      ad_spi_dma_state = AD_SPI_DMA_ERROR;
      return;
    }

    ad_dma_total_len = total_len;
    remaining_len = (uint16_t)(total_len - AD_SPI_HEADER_SIZE);
    ad_spi_dma_state = AD_SPI_DMA_PAYLOAD;
    status = HAL_SPI_TransmitReceive_DMA(&hspi4,
                                         &ad_spi_tx_dummy[AD_SPI_HEADER_SIZE],
                                         &ad_spi_rx_frame[AD_SPI_HEADER_SIZE],
                                         remaining_len);
    if (status != HAL_OK)
    {
      HAL_GPIO_WritePin(AD_CS_GPIO_Port, AD_CS_Pin, GPIO_PIN_SET);
      ad_dma_error_code = AD_SPI_DMA_ERROR_START_BODY;
      ad_dma_error_hal = HAL_SPI_GetError(&hspi4);
      ad_spi_dma_state = AD_SPI_DMA_ERROR;
    }
  }
  else if (ad_spi_dma_state == AD_SPI_DMA_PAYLOAD)
  {
    HAL_GPIO_WritePin(AD_CS_GPIO_Port, AD_CS_Pin, GPIO_PIN_SET);
    ad_dma_frame_ready = 1U;
    ad_spi_dma_state = AD_SPI_DMA_FRAME_READY;
  }
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
  if (hspi->Instance != SPI4)
  {
    return;
  }

  HAL_GPIO_WritePin(AD_CS_GPIO_Port, AD_CS_Pin, GPIO_PIN_SET);
  ad_dma_error_code = AD_SPI_DMA_ERROR_HAL;
  ad_dma_error_hal = HAL_SPI_GetError(hspi);
  ad_spi_dma_state = AD_SPI_DMA_ERROR;
}

void HAL_GPIO_EXTI_Rising_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == AD_IRQ_Pin)
  {
    ad_irq_count++;
    ad_irq_pending = 1U;
  }
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
