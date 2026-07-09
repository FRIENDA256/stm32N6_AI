/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    ad7606_spi_dma.c
  * @brief   SPI4 DMA receiver and quality monitor for the AD7606 acquisition card.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "ad7606_spi_dma.h"
#include "app_console.h"
#include "gpio.h"
#include "spi.h"

#include <stdio.h>
#include <string.h>

#define AD_SPI_HEADER_SIZE               24U
#define AD_SPI_MAX_FRAME_SIZE            AD7606_SPI4_MAX_FRAME_SIZE
#define AD_SPI_CRC_SIZE                  4U
#define AD_FRAME_MAGIC                   0xAD76U
#define AD_FRAME_TYPE_RAW_SYNC           0x01U
#define AD_RAW_PAYLOAD_HEADER_SIZE       20U
#define AD_RAW_MAX_CHANNELS              8U
#define AD_SPI_DMA_ERROR_BAD_HEADER      1U
#define AD_SPI_DMA_ERROR_BAD_LENGTH      2U
#define AD_SPI_DMA_ERROR_START_HEADER    3U
#define AD_SPI_DMA_ERROR_START_BODY      4U
#define AD_SPI_DMA_ERROR_HAL             5U
#define AD_SPI_QUALITY_REPORT_PERIOD_MS  5000U
#define AD_SPI_IRQ_TO_CS_SETTLE_MS       1U
#define AD_SPI_LED_NO_FRAME_TIMEOUT_MS   1500U
#define AD_SPI_LED_ERROR_HOLD_MS         3000U
#define AD_SPI_LED_GOOD_TOGGLE_MS        500U
#define AD_SPI_LED_ERROR_TOGGLE_MS       100U
#define AD_SPI_VERBOSE_FRAME_LOG         0U
#define AD_SPI_RAW_DUMP_CHANNEL_INDEX    3U
#define AD_SPI_RAW_DUMP_ROWS             8U
#define AD_SPI_RAW_DUMP_CH_SAMPLE_COUNT  32U

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

static void UART_WriteString(const char *text);
static uint16_t ReadLE16(const uint8_t *data);
static int16_t ReadLE16S(const uint8_t *data);
static uint32_t ReadLE32(const uint8_t *data);
static uint64_t ReadLE64(const uint8_t *data);
static uint32_t AD7606_CalcCRC32(const uint8_t *buf, uint32_t len);
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
static void AD7606_SPI4_StartDmaRead(uint32_t irq_count_snapshot);
static void AD7606_SPI4_ProcessDmaFrame(uint32_t irq_count_snapshot);
static void AD7606_SPI4_ReportDmaError(uint32_t irq_count_snapshot, uint8_t error_code, uint32_t hal_error);
static void AD7606_SPI4_SaveLatestFrame(uint32_t irq_count_snapshot,
                                        uint32_t frame_seq,
                                        uint16_t total_len,
                                        uint16_t payload_len,
                                        uint32_t timestamp_ms,
                                        uint32_t sample_counter,
                                        uint8_t frame_type,
                                        uint32_t crc_actual);
static void AD7606_SPI4_DumpRawFrame(uint32_t irq_count_snapshot,
                                     uint32_t frame_seq,
                                     uint16_t total_len,
                                     uint16_t payload_len,
                                     uint32_t timestamp_ms,
                                     uint32_t sample_counter,
                                     uint32_t crc_expected,
                                     uint32_t crc_actual);

static volatile uint32_t ad_irq_count;
static volatile uint8_t ad_irq_pending;
static volatile AD_SPI_DMA_State_t ad_spi_dma_state = AD_SPI_DMA_IDLE;
static volatile uint8_t ad_dma_frame_ready;
static volatile uint8_t ad_dma_error_code;
static volatile uint32_t ad_dma_error_hal;
static volatile uint16_t ad_dma_total_len;
static volatile uint32_t ad_dma_irq_snapshot;
static volatile uint8_t ad_raw_dump_requested;
static volatile uint8_t ad_spi_paused;
static AD_SPI_QualityStats_t ad_spi_quality;
static uint32_t ad_spi_last_good_frame_tick;
static uint32_t ad_spi_last_error_tick;
static uint32_t ad_spi_led_tick;
static uint8_t ad_spi_led_on;
static uint8_t ad_spi_tx_dummy[AD_SPI_MAX_FRAME_SIZE];
static uint8_t ad_spi_rx_frame[AD_SPI_MAX_FRAME_SIZE];
static uint8_t ad_spi_latest_frame[AD_SPI_MAX_FRAME_SIZE];
static AD7606_SPI4_FrameInfo_t ad_spi_latest_info;
static volatile uint32_t ad_spi_latest_update_seq;
static volatile uint8_t ad_spi_latest_valid;

void AD7606_SPI4_Init(void)
{
  uint32_t now_tick = HAL_GetTick();

  memset(ad_spi_tx_dummy, 0xFF, sizeof(ad_spi_tx_dummy));
  memset(ad_spi_rx_frame, 0x00, sizeof(ad_spi_rx_frame));
  memset(ad_spi_latest_frame, 0x00, sizeof(ad_spi_latest_frame));
  memset(&ad_spi_latest_info, 0x00, sizeof(ad_spi_latest_info));

  ad_irq_count = 0U;
  ad_irq_pending = 0U;
  ad_spi_dma_state = AD_SPI_DMA_IDLE;
  ad_dma_frame_ready = 0U;
  ad_dma_error_code = 0U;
  ad_dma_error_hal = 0U;
  ad_dma_total_len = 0U;
  ad_dma_irq_snapshot = 0U;
  ad_raw_dump_requested = 0U;
  ad_spi_paused = 0U;
  ad_spi_latest_update_seq = 0U;
  ad_spi_latest_valid = 0U;

  HAL_GPIO_WritePin(AD_CS_GPIO_Port, AD_CS_Pin, GPIO_PIN_SET);
  SPI4_QualityTest_ClearWindow(now_tick);
  SPI4_StatusLED_Init(now_tick);
  UART_WriteString("SPI4 AD7606 DMA receiver start\r\n");
}

void AD7606_SPI4_Task(uint32_t now_tick)
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

  if (ad_spi_paused != 0U)
  {
    __disable_irq();
    ad_irq_pending = 0U;
    __enable_irq();
    SPI4_StatusLED_Task(now_tick);
    return;
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

  SPI4_QualityTest_Task(now_tick);
  SPI4_StatusLED_Task(now_tick);
}

void AD7606_SPI4_RequestRawDump(void)
{
  ad_raw_dump_requested = 1U;
  UART_WriteString("AD7606 raw dump armed\r\n");
}

void AD7606_SPI4_SetPaused(uint8_t paused)
{
  __disable_irq();
  ad_spi_paused = (paused != 0U) ? 1U : 0U;
  if (ad_spi_paused != 0U)
  {
    ad_irq_pending = 0U;
  }
  __enable_irq();
}

uint8_t AD7606_SPI4_IsIdle(void)
{
  uint8_t idle;

  __disable_irq();
  idle = ((ad_spi_dma_state == AD_SPI_DMA_IDLE) &&
          (ad_dma_frame_ready == 0U) &&
          (ad_dma_error_code == 0U)) ? 1U : 0U;
  __enable_irq();

  return idle;
}

uint32_t AD7606_SPI4_CopyLatestFrame(uint8_t *dest, uint32_t dest_len, AD7606_SPI4_FrameInfo_t *info)
{
  uint32_t total_len;

  if ((dest == NULL) || (ad_spi_latest_valid == 0U))
  {
    return 0U;
  }

  for (uint32_t attempt = 0U; attempt < 8U; attempt++)
  {
    uint32_t seq_before = ad_spi_latest_update_seq;

    if ((seq_before & 1U) != 0U)
    {
      continue;
    }

    total_len = (uint32_t)ad_spi_latest_info.total_len;
    if ((total_len == 0U) || (total_len > AD_SPI_MAX_FRAME_SIZE) || (dest_len < total_len))
    {
      return 0U;
    }

    memcpy(dest, ad_spi_latest_frame, total_len);
    if (info != NULL)
    {
      *info = ad_spi_latest_info;
    }

    if (seq_before == ad_spi_latest_update_seq)
    {
      return total_len;
    }
  }

  return 0U;
}

static void UART_WriteString(const char *text)
{
  if (text != NULL)
  {
    App_Print(text);
  }
}

static uint16_t ReadLE16(const uint8_t *data)
{
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static int16_t ReadLE16S(const uint8_t *data)
{
  return (int16_t)ReadLE16(data);
}

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
    App_Print(line);
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
    App_Print(line);
  }

  if (ad_spi_quality.dma_error_count != 0U)
  {
    len = snprintf(line, sizeof(line), "SPI4 quality last_dma_error code=%lu hal=0x%08lX\r\n",
                   (unsigned long)ad_spi_quality.last_dma_error_code,
                   (unsigned long)ad_spi_quality.last_hal_error);
    if (len > 0)
    {
      App_Print(line);
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
    return;
  }

  if ((total_len < (AD_SPI_HEADER_SIZE + AD_SPI_CRC_SIZE)) || (total_len > AD_SPI_MAX_FRAME_SIZE))
  {
    ad_spi_quality.bad_length_count++;
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
    App_Print(line);
  }
#endif

  if (payload_len != (uint16_t)(total_len - AD_SPI_HEADER_SIZE - AD_SPI_CRC_SIZE))
  {
    ad_spi_quality.payload_length_warning_count++;
  }

  if (crc_actual == crc_expected)
  {
    AD7606_SPI4_SaveLatestFrame(irq_count_snapshot,
                                frame_seq,
                                total_len,
                                payload_len,
                                timestamp_ms,
                                sample_counter,
                                ad_spi_rx_frame[3],
                                crc_actual);
  }

  if ((crc_actual == crc_expected) && (ad_raw_dump_requested != 0U))
  {
    ad_raw_dump_requested = 0U;
    AD7606_SPI4_DumpRawFrame(irq_count_snapshot,
                             frame_seq,
                             total_len,
                             payload_len,
                             timestamp_ms,
                             sample_counter,
                             crc_expected,
                             crc_actual);
  }
}

static void AD7606_SPI4_SaveLatestFrame(uint32_t irq_count_snapshot,
                                        uint32_t frame_seq,
                                        uint16_t total_len,
                                        uint16_t payload_len,
                                        uint32_t timestamp_ms,
                                        uint32_t sample_counter,
                                        uint8_t frame_type,
                                        uint32_t crc_actual)
{
  AD7606_SPI4_FrameInfo_t info;

  if ((total_len == 0U) || (total_len > AD_SPI_MAX_FRAME_SIZE))
  {
    return;
  }

  info.irq_count = irq_count_snapshot;
  info.frame_seq = frame_seq;
  info.timestamp_ms = timestamp_ms;
  info.sample_counter = sample_counter;
  info.crc32 = crc_actual;
  info.total_len = total_len;
  info.payload_len = payload_len;
  info.frame_type = frame_type;
  info.crc_ok = 1U;

  ad_spi_latest_update_seq++;
  memcpy(ad_spi_latest_frame, ad_spi_rx_frame, total_len);
  ad_spi_latest_info = info;
  ad_spi_latest_valid = 1U;
  ad_spi_latest_update_seq++;
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
    App_Print(line);
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
      App_Print(line);
    }
  }
}

static void AD7606_SPI4_DumpRawFrame(uint32_t irq_count_snapshot,
                                     uint32_t frame_seq,
                                     uint16_t total_len,
                                     uint16_t payload_len,
                                     uint32_t timestamp_ms,
                                     uint32_t sample_counter,
                                     uint32_t crc_expected,
                                     uint32_t crc_actual)
{
  const uint8_t *payload = &ad_spi_rx_frame[AD_SPI_HEADER_SIZE];
  const uint8_t *samples = &payload[AD_RAW_PAYLOAD_HEADER_SIZE];
  uint8_t frame_type = ad_spi_rx_frame[3];
  uint16_t points;
  uint8_t channels;
  uint8_t bytes_per_sample;
  uint64_t block_start;
  uint64_t block_end;
  uint32_t expected_payload_len;
  uint32_t dump_rows;
  uint32_t dump_ch_samples;
  int16_t ch4_min = 0;
  int16_t ch4_max = 0;
  int32_t ch4_sum = 0;
  char line[256];
  int len;

  UART_WriteString("AD7606 raw dump begin\r\n");

  len = snprintf(line, sizeof(line),
                 "AD7606 frame irq=%lu seq=%lu type=0x%02X total=%u payload=%u ts=%lu sample=%lu crc=0x%08lX/0x%08lX\r\n",
                 (unsigned long)irq_count_snapshot,
                 (unsigned long)frame_seq,
                 (unsigned int)frame_type,
                 (unsigned int)total_len,
                 (unsigned int)payload_len,
                 (unsigned long)timestamp_ms,
                 (unsigned long)sample_counter,
                 (unsigned long)crc_actual,
                 (unsigned long)crc_expected);
  if (len > 0)
  {
    App_Print(line);
  }

  if ((frame_type != AD_FRAME_TYPE_RAW_SYNC) || (payload_len < AD_RAW_PAYLOAD_HEADER_SIZE))
  {
    UART_WriteString("AD7606 raw dump skipped: not a raw waveform frame\r\n");
    UART_WriteString("AD7606 raw dump end\r\n");
    return;
  }

  points = ReadLE16(&payload[0]);
  channels = payload[2];
  bytes_per_sample = payload[3];
  block_start = ReadLE64(&payload[4]);
  block_end = ReadLE64(&payload[12]);
  expected_payload_len = AD_RAW_PAYLOAD_HEADER_SIZE + ((uint32_t)points * channels * bytes_per_sample);

  len = snprintf(line, sizeof(line),
                 "AD7606 raw points=%u channels=%u bytes_per_sample=%u expected_payload=%lu block=0x%08lX%08lX..0x%08lX%08lX\r\n",
                 (unsigned int)points,
                 (unsigned int)channels,
                 (unsigned int)bytes_per_sample,
                 (unsigned long)expected_payload_len,
                 (unsigned long)(uint32_t)(block_start >> 32),
                 (unsigned long)(uint32_t)block_start,
                 (unsigned long)(uint32_t)(block_end >> 32),
                 (unsigned long)(uint32_t)block_end);
  if (len > 0)
  {
    App_Print(line);
  }

  if ((points == 0U) ||
      (channels <= AD_SPI_RAW_DUMP_CHANNEL_INDEX) ||
      (channels > AD_RAW_MAX_CHANNELS) ||
      (bytes_per_sample != 2U) ||
      (payload_len < expected_payload_len))
  {
    UART_WriteString("AD7606 raw dump invalid format for CH4 signed-16 parse\r\n");
    UART_WriteString("AD7606 raw dump end\r\n");
    return;
  }

  for (uint32_t i = 0U; i < points; i++)
  {
    uint32_t offset = ((i * channels) + AD_SPI_RAW_DUMP_CHANNEL_INDEX) * bytes_per_sample;
    int16_t sample = ReadLE16S(&samples[offset]);

    if (i == 0U)
    {
      ch4_min = sample;
      ch4_max = sample;
    }
    else
    {
      if (sample < ch4_min)
      {
        ch4_min = sample;
      }
      if (sample > ch4_max)
      {
        ch4_max = sample;
      }
    }
    ch4_sum += sample;
  }

  len = snprintf(line, sizeof(line),
                 "AD7606 CH4 stats count=%u min=%ld max=%ld avg=%ld\r\n",
                 (unsigned int)points,
                 (long)ch4_min,
                 (long)ch4_max,
                 (long)(ch4_sum / (int32_t)points));
  if (len > 0)
  {
    App_Print(line);
  }

  UART_WriteString("AD7606 first rows: idx ch1 ch2 ch3 ch4 ch5 ch6 ch7 ch8\r\n");
  dump_rows = (points < AD_SPI_RAW_DUMP_ROWS) ? points : AD_SPI_RAW_DUMP_ROWS;
  for (uint32_t row = 0U; row < dump_rows; row++)
  {
    uint32_t pos = 0U;

    len = snprintf(line, sizeof(line), "AD7606 row[%lu]", (unsigned long)row);
    if (len <= 0)
    {
      continue;
    }
    pos = (uint32_t)len;

    for (uint32_t ch = 0U; (ch < channels) && (ch < AD_RAW_MAX_CHANNELS); ch++)
    {
      uint32_t offset = ((row * channels) + ch) * bytes_per_sample;
      int16_t sample = ReadLE16S(&samples[offset]);

      if (pos < (sizeof(line) - 16U))
      {
        len = snprintf(&line[pos], sizeof(line) - pos, " %ld", (long)sample);
        if (len > 0)
        {
          pos += (uint32_t)len;
        }
      }
    }

    if (pos < (sizeof(line) - 3U))
    {
      line[pos++] = '\r';
      line[pos++] = '\n';
      line[pos] = '\0';
      App_Print(line);
    }
  }

  UART_WriteString("AD7606 CH4 first samples:");
  dump_ch_samples = (points < AD_SPI_RAW_DUMP_CH_SAMPLE_COUNT) ? points : AD_SPI_RAW_DUMP_CH_SAMPLE_COUNT;
  for (uint32_t i = 0U; i < dump_ch_samples; i++)
  {
    uint32_t offset = ((i * channels) + AD_SPI_RAW_DUMP_CHANNEL_INDEX) * bytes_per_sample;
    int16_t sample = ReadLE16S(&samples[offset]);

    len = snprintf(line, sizeof(line), " %ld", (long)sample);
    if (len > 0)
    {
      App_Print(line);
    }
  }
  UART_WriteString("\r\nAD7606 raw dump end\r\n");
}

void AD7606_SPI4_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
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

void AD7606_SPI4_ErrorCallback(SPI_HandleTypeDef *hspi)
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

void AD7606_SPI4_EXTI_RisingCallback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == AD_IRQ_Pin)
  {
    ad_irq_count++;
    ad_irq_pending = 1U;
  }
}
