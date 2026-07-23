/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_ai.c
  * @brief   First AI vertical slice: AD7606 window to STM32N6 ATON inference.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "app_ai.h"

#include "ad7606_spi_dma.h"
#include "app_console.h"
#include "main.h"
#include "npu_cache.h"
#include "tiny_temporal_mixer_8ch_int8.h"

#include "ll_aton_rt_user_api.h"

#include <string.h>

LL_ATON_DECLARE_NAMED_NN_INSTANCE_AND_INTERFACE(tiny_temporal_mixer_8ch_int8);

#define APP_AI_THREAD_STACK_SIZE       4096U
#define APP_AI_THREAD_PRIORITY         9U
#define APP_AI_THREAD_SLEEP_TICKS      1U
#define APP_AI_RUN_TIMEOUT_MS          5000U
#define APP_AI_FRAME_BUDGET_MS         20U
#define APP_AI_CACHE_LINE_BYTES        32U
#define APP_AI_CHANNELS                8U
#define APP_AI_WINDOW_SAMPLES         1024U
#define APP_AI_FRAME_SAMPLES           AD7606_SPI4_AI_FRAME_POINTS
#define APP_AI_FRAME_BYTES             AD7606_SPI4_AI_FRAME_BYTES
#define APP_AI_RAW_SAMPLE_BYTES       AD7606_SPI4_AI_WINDOW_BYTES
#define APP_AI_INPUT_BYTES             (APP_AI_CHANNELS * APP_AI_WINDOW_SAMPLES)
#define APP_AI_OUTPUT_BYTES            4U
#define APP_AI_INPUT_MULTIPLIER        37903L
#define APP_AI_INPUT_SHIFT             23U
#define APP_AI_INPUT_ROUNDING          (1L << (APP_AI_INPUT_SHIFT - 1U))
#define APP_AI_NPURAM_BASE             0x342E0000UL
#define APP_AI_NPURAM_BYTES            0x00056000UL
#define APP_AI_WEIGHT_BASE             0x71000000UL
#define APP_AI_WEIGHT_END              0x7101FFFFUL
#define APP_AI_WEIGHT_PROBE_BYTES      256U
#define APP_AI_WEIGHT_PROBE_SUM        0x00007BB1UL
#define APP_AI_NPU_IRQ_PRIORITY        7U
#define APP_AI_BUNDLE_QUEUE_DEPTH      8U
#define APP_AI_MAX_BURST_FRAMES        8U

#if (APP_AI_INPUT_BYTES != LL_ATON_TINY_TEMPORAL_MIXER_8CH_INT8_IN_1_SIZE_BYTES)
#error "AD7606 preprocessing window does not match the generated model input"
#endif

#if (APP_AI_INPUT_WINDOW_BYTES != APP_AI_RAW_SAMPLE_BYTES)
#error "Published AD+AI bundle window does not match the AD7606 raw window"
#endif

#if ((APP_AI_FRAME_BYTES * 2U) != APP_AI_RAW_SAMPLE_BYTES)
#error "AD7606 frame geometry does not form the required 1024-point AI window"
#endif

#if (APP_AI_OUTPUT_BYTES != LL_ATON_TINY_TEMPORAL_MIXER_8CH_INT8_OUT_1_SIZE_BYTES)
#error "AI status output size does not match the generated model output"
#endif

static TX_THREAD AppAIThread;
static volatile App_AI_Status_t AppAIStatus;
static volatile uint8_t AppAIBundleConsumerActive;
__attribute__((aligned(32))) static uint8_t AppAIRawSamples[APP_AI_RAW_SAMPLE_BYTES];

typedef struct
{
  App_AI_Bundle_Info_t bundle_info;
  uint8_t raw_frame[APP_AI_SOURCE_FRAME_BYTES];
} AppAI_BundleSlot_t;

__attribute__((aligned(32), section(".ad7606_ai_queue")))
static AppAI_BundleSlot_t AppAIBundleQueue[APP_AI_BUNDLE_QUEUE_DEPTH];
static volatile uint32_t AppAIBundleQueueHead;
static volatile uint32_t AppAIBundleQueueTail;
static volatile uint32_t AppAIBundleQueueCount;
static uint32_t AppAILastInferenceFrameSeq;
static uint8_t AppAIHaveInferenceFrame;
static volatile uint8_t AppAIProcessingActive;
static volatile uint8_t AppAIProcessingBusy;

static uint32_t AppAI_StatusLock(void)
{
  uint32_t primask = __get_PRIMASK();

  __disable_irq();
  return primask;
}

static void AppAI_StatusUnlock(uint32_t primask)
{
  if (primask == 0U)
  {
    __enable_irq();
  }
}

static void AppAI_SetProcessingBusy(uint8_t busy)
{
  uint32_t primask = AppAI_StatusLock();

  AppAIProcessingBusy = (busy != 0U) ? 1U : 0U;
  AppAI_StatusUnlock(primask);
}

static void AppAI_BundleQueueResetLocked(void)
{
  AppAIBundleQueueHead = 0U;
  AppAIBundleQueueTail = 0U;
  AppAIBundleQueueCount = 0U;
  AppAIStatus.output_queue_depth = 0U;
}

static void AppAI_BundleEnqueue(const uint8_t *raw_frame,
                                const AD7606_SPI4_RawInfo_t *source_raw_info,
                                const AD7606_SPI4_RawInfo_t *window_raw_info)
{
  App_AI_Bundle_Info_t bundle_info;
  uint32_t queue_index;
  uint32_t primask;

  if ((raw_frame == NULL) ||
      (source_raw_info == NULL) ||
      (window_raw_info == NULL))
  {
    return;
  }

  primask = AppAI_StatusLock();
  if (AppAIBundleConsumerActive == 0U)
  {
    AppAI_StatusUnlock(primask);
    return;
  }
  if (AppAIBundleQueueCount >= APP_AI_BUNDLE_QUEUE_DEPTH)
  {
    AppAIStatus.output_queue_overflow_count++;
    AppAI_StatusUnlock(primask);
    return;
  }
  queue_index = AppAIBundleQueueHead;
  AppAI_StatusUnlock(primask);

  App_AI_GetStatus(&bundle_info.ai_status);
  bundle_info.source_points = source_raw_info->points;
  bundle_info.source_channels = source_raw_info->channels;
  bundle_info.source_bytes_per_sample = source_raw_info->bytes_per_sample;
  bundle_info.source_sample_bytes = source_raw_info->sample_bytes;
  bundle_info.source_block_start = source_raw_info->block_start;
  bundle_info.source_block_end = source_raw_info->block_end;
  bundle_info.window_points = window_raw_info->points;
  bundle_info.window_sample_bytes = window_raw_info->sample_bytes;
  bundle_info.window_block_start = window_raw_info->block_start;
  bundle_info.window_block_end = window_raw_info->block_end;

  (void)memcpy(AppAIBundleQueue[queue_index].raw_frame,
               raw_frame,
               APP_AI_SOURCE_FRAME_BYTES);
  AppAIBundleQueue[queue_index].bundle_info = bundle_info;
  __DMB();

  primask = AppAI_StatusLock();
  AppAIBundleQueueHead =
    (queue_index + 1U) % APP_AI_BUNDLE_QUEUE_DEPTH;
  AppAIBundleQueueCount++;
  AppAIStatus.output_queue_depth = AppAIBundleQueueCount;
  if (AppAIBundleQueueCount > AppAIStatus.output_queue_high_water)
  {
    AppAIStatus.output_queue_high_water = AppAIBundleQueueCount;
  }
  AppAI_StatusUnlock(primask);
}

static void AppAI_YieldAfterBurst(uint32_t *burst_frames)
{
  if ((burst_frames != NULL) &&
      (*burst_frames >= APP_AI_MAX_BURST_FRAMES))
  {
    *burst_frames = 0U;
    tx_thread_sleep(1U);
  }
}

static UINT AppAI_ByteAllocate(TX_BYTE_POOL *byte_pool, UCHAR **memory, ULONG size)
{
  UINT status;

  if ((byte_pool == TX_NULL) || (memory == TX_NULL))
  {
    return TX_PTR_ERROR;
  }

  status = tx_byte_allocate(byte_pool, (VOID **)memory, size, TX_NO_WAIT);
  if (status != TX_SUCCESS)
  {
    App_PrintHex32("AI byte allocate failed: ", status);
    return status;
  }

  (void)memset(*memory, 0, size);
  return TX_SUCCESS;
}

static void AppAI_CacheCleanAligned(uintptr_t addr, uint32_t size)
{
  uintptr_t start = addr & ~((uintptr_t)APP_AI_CACHE_LINE_BYTES - 1U);
  uintptr_t end = (addr + size + APP_AI_CACHE_LINE_BYTES - 1U) &
                  ~((uintptr_t)APP_AI_CACHE_LINE_BYTES - 1U);

  LL_ATON_Cache_MCU_Clean_Range(start, (uint32_t)(end - start));
}

static void AppAI_CacheInvalidateAligned(uintptr_t addr, uint32_t size)
{
  uintptr_t start = addr & ~((uintptr_t)APP_AI_CACHE_LINE_BYTES - 1U);
  uintptr_t end = (addr + size + APP_AI_CACHE_LINE_BYTES - 1U) &
                  ~((uintptr_t)APP_AI_CACHE_LINE_BYTES - 1U);

  LL_ATON_Cache_MCU_Invalidate_Range(start, (uint32_t)(end - start));
}

static int8_t AppAI_ConvertSample(int16_t sample)
{
  int32_t scaled = (int32_t)sample * APP_AI_INPUT_MULTIPLIER;
  int32_t value;

  /* Training uses raw/32768 followed by the model input scale 0.00675407844.
     The fixed-point factor 37903/2^23 implements raw/221.3176. */
  if (scaled >= 0)
  {
    value = (scaled + APP_AI_INPUT_ROUNDING) >> APP_AI_INPUT_SHIFT;
  }
  else
  {
    value = -(((-scaled) + APP_AI_INPUT_ROUNDING) >> APP_AI_INPUT_SHIFT);
  }

  if (value > 127)
  {
    value = 127;
  }
  else if (value < -128)
  {
    value = -128;
  }

  return (int8_t)value;
}

static int16_t AppAI_ReadLE16S(const uint8_t *data)
{
  uint16_t value = (uint16_t)data[0] | ((uint16_t)data[1] << 8);

  return (int16_t)value;
}

static void AppAI_CopyRawWindowToInput(const uint8_t *raw_samples,
                                       const AD7606_SPI4_RawInfo_t *raw_info,
                                       int8_t *input_buffer)
{
  if ((raw_samples == NULL) || (raw_info == NULL) || (input_buffer == NULL) ||
      (raw_info->channels != APP_AI_CHANNELS) ||
      (raw_info->points != APP_AI_WINDOW_SAMPLES) ||
      (raw_info->bytes_per_sample != 2U))
  {
    return;
  }

  for (uint32_t channel = 0U; channel < APP_AI_CHANNELS; channel++)
  {
    for (uint32_t point = 0U; point < APP_AI_WINDOW_SAMPLES; point++)
    {
      uint32_t raw_offset = ((point * APP_AI_CHANNELS) + channel) * raw_info->bytes_per_sample;
      input_buffer[(channel * APP_AI_WINDOW_SAMPLES) + point] =
        AppAI_ConvertSample(AppAI_ReadLE16S(&raw_samples[raw_offset]));
    }
  }
}

static uint8_t AppAI_CheckWeights(void)
{
  const volatile uint8_t *weights = (const volatile uint8_t *)APP_AI_WEIGHT_BASE;
  uint32_t sum = 0U;

  for (uint32_t i = 0U; i < APP_AI_WEIGHT_PROBE_BYTES; i++)
  {
    sum += weights[i];
  }

  return (sum == APP_AI_WEIGHT_PROBE_SUM) ? 1U : 0U;
}

static void AppAI_ConfigureHardware(void)
{
  RIMC_MasterConfig_t npu_master_config = {0};
  RISAF_BaseRegionConfig_t risaf_config = {0};

  __HAL_RCC_RIFSC_CLK_ENABLE();
  __HAL_RCC_RISAF_CLK_ENABLE();

  (void)HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_NPU,
                                              RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_NPRIV);
  (void)HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RCC_PERIPH_INDEX_NPURAM0,
                                              RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_NPRIV);
  (void)HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RCC_PERIPH_INDEX_NPURAM1,
                                              RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_NPRIV);
  (void)HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RCC_PERIPH_INDEX_NPURAM2,
                                              RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_NPRIV);
  (void)HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RCC_PERIPH_INDEX_NPURAM3,
                                              RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_NPRIV);
  (void)HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_XSPI2,
                                              RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_NPRIV);

  __HAL_RCC_AXISRAM3_MEM_CLK_ENABLE();
  __HAL_RCC_AXISRAM4_MEM_CLK_ENABLE();
  __HAL_RCC_AXISRAM5_MEM_CLK_ENABLE();
  __HAL_RCC_AXISRAM6_MEM_CLK_ENABLE();
  __HAL_RCC_CACHEAXIRAM_MEM_CLK_ENABLE();
  __HAL_RCC_RAMCFG_CLK_ENABLE();
  __HAL_RCC_NPU_CLK_ENABLE();
  __HAL_RCC_NPU_CLK_SLEEP_ENABLE();

  npu_master_config.MasterCID = RIF_CID_0;
  npu_master_config.SecPriv = RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV;
  HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_NPU, &npu_master_config);

  risaf_config.Filtering = RISAF_FILTER_ENABLE;
  risaf_config.Secure = RIF_ATTRIBUTE_SEC;
  risaf_config.PrivWhitelist = RIF_CID_NONE;
  risaf_config.ReadWhitelist = RIF_CID_MASK;
  risaf_config.WriteWhitelist = RIF_CID_MASK;

  risaf_config.StartAddress = 0x00000000U;
  risaf_config.EndAddress = 0x07FFFFFFU;
  HAL_RIF_RISAF_ConfigBaseRegion(RISAF12, RISAF_REGION_1, &risaf_config);

  risaf_config.StartAddress = APP_AI_NPURAM_BASE;
  risaf_config.EndAddress = APP_AI_NPURAM_BASE + APP_AI_NPURAM_BYTES - 1U;
  HAL_RIF_RISAF_ConfigBaseRegion(RISAF4, RISAF_REGION_1, &risaf_config);
  HAL_RIF_RISAF_ConfigBaseRegion(RISAF5, RISAF_REGION_1, &risaf_config);

  risaf_config.StartAddress = APP_AI_WEIGHT_BASE;
  risaf_config.EndAddress = APP_AI_WEIGHT_END;
  HAL_RIF_RISAF_ConfigBaseRegion(RISAF4, RISAF_REGION_2, &risaf_config);
  HAL_RIF_RISAF_ConfigBaseRegion(RISAF5, RISAF_REGION_2, &risaf_config);

  RISAF4->IACR = RISAF_IACR_CAEF | RISAF_IACR_IAEF;
  RISAF5->IACR = RISAF_IACR_CAEF | RISAF_IACR_IAEF;
  RISAF12->IACR = RISAF_IACR_CAEF | RISAF_IACR_IAEF;

  RAMCFG_SRAM3_AXI->CR &= ~RAMCFG_CR_SRAMSD;
  RAMCFG_SRAM4_AXI->CR &= ~RAMCFG_CR_SRAMSD;
  RAMCFG_SRAM5_AXI->CR &= ~RAMCFG_CR_SRAMSD;
  RAMCFG_SRAM6_AXI->CR &= ~RAMCFG_CR_SRAMSD;

  HAL_NVIC_SetPriority(NPU0_IRQn, APP_AI_NPU_IRQ_PRIORITY, 0U);
  HAL_NVIC_EnableIRQ(NPU0_IRQn);
  HAL_NVIC_SetPriority(NPU1_IRQn, APP_AI_NPU_IRQ_PRIORITY, 0U);
  HAL_NVIC_EnableIRQ(NPU1_IRQn);
  HAL_NVIC_SetPriority(CACHEAXI_IRQn, APP_AI_NPU_IRQ_PRIORITY, 0U);
  HAL_NVIC_EnableIRQ(CACHEAXI_IRQn);

  npu_cache_init();
  npu_cache_enable();
  npu_cache_invalidate();

  {
    uint32_t primask = AppAI_StatusLock();

    AppAIStatus.npu_clock_hz = HAL_RCC_GetNPUClockFreq();
    AppAIStatus.npuram_clock_hz = HAL_RCC_GetNPURAMSClockFreq();
    AppAI_StatusUnlock(primask);
  }
}

static void AppAI_InitializeActivationMemory(void)
{
  void *activation = (void *)APP_AI_NPURAM_BASE;

  AppAI_CacheInvalidateAligned((uintptr_t)activation, APP_AI_NPURAM_BYTES);
  (void)memset(activation, 0, APP_AI_NPURAM_BYTES);
  AppAI_CacheCleanAligned((uintptr_t)activation, APP_AI_NPURAM_BYTES);
}

static uint8_t AppAI_RunInference(int8_t *input_buffer,
                                  int8_t *output_buffer,
                                  uint32_t *elapsed_ms,
                                  int32_t *last_run_status)
{
  const LL_ATON_RT_RetValues_t done = LL_ATON_RT_DONE;
  LL_ATON_RT_RetValues_t run_status;
  uint32_t start_tick = HAL_GetTick();

  AppAI_CacheCleanAligned((uintptr_t)input_buffer, APP_AI_INPUT_BYTES);

  do
  {
    run_status = LL_ATON_RT_RunEpochBlock(&NN_Instance_tiny_temporal_mixer_8ch_int8);
    if (run_status == LL_ATON_RT_WFE)
    {
      __SEV();
      __WFE();
      tx_thread_relinquish();
    }

    if (run_status == done)
    {
      break;
    }

    if ((HAL_GetTick() - start_tick) > APP_AI_RUN_TIMEOUT_MS)
    {
      *elapsed_ms = HAL_GetTick() - start_tick;
      *last_run_status = (int32_t)run_status;
      return 0U;
    }
  } while (1);

  AppAI_CacheInvalidateAligned((uintptr_t)output_buffer, APP_AI_OUTPUT_BYTES);
  *elapsed_ms = HAL_GetTick() - start_tick;
  *last_run_status = (int32_t)run_status;
  return 1U;
}

static void AppAI_RecordRun(const AD7606_SPI4_FrameInfo_t *frame_info,
                            const int8_t *output_buffer,
                            uint32_t inference_ms,
                            int32_t run_status)
{
  uint32_t primask;
  uint8_t top_index = 0U;

  for (uint32_t i = 1U; i < APP_AI_OUTPUT_BYTES; i++)
  {
    if (output_buffer[i] > output_buffer[top_index])
    {
      top_index = (uint8_t)i;
    }
  }

  primask = AppAI_StatusLock();
  if ((AppAIHaveInferenceFrame != 0U) &&
      (frame_info->frame_seq != (AppAILastInferenceFrameSeq + 1U)))
  {
    AppAIStatus.inference_gap_count++;
  }
  AppAILastInferenceFrameSeq = frame_info->frame_seq;
  AppAIHaveInferenceFrame = 1U;
  AppAIStatus.run_count++;
  AppAIStatus.last_frame_seq = frame_info->frame_seq;
  AppAIStatus.last_timestamp_ms = frame_info->timestamp_ms;
  AppAIStatus.last_sample_counter = frame_info->sample_counter;
  AppAIStatus.last_inference_ms = inference_ms;
  AppAIStatus.inference_total_ms += inference_ms;
  if (inference_ms > AppAIStatus.max_inference_ms)
  {
    AppAIStatus.max_inference_ms = inference_ms;
  }
  if (inference_ms > APP_AI_FRAME_BUDGET_MS)
  {
    AppAIStatus.deadline_miss_count++;
  }
  AppAIStatus.last_run_status = run_status;
  AppAIStatus.last_top_index = top_index;
  (void)memcpy((void *)AppAIStatus.last_output, output_buffer, APP_AI_OUTPUT_BYTES);
  AppAI_StatusUnlock(primask);
}

static VOID AppAI_ThreadEntry(ULONG thread_input)
{
  const LL_Buffer_InfoTypeDef *input_info;
  const LL_Buffer_InfoTypeDef *output_info;
  AD7606_SPI4_FrameInfo_t frame_info;
  AD7606_SPI4_RawInfo_t raw_info;
  AD7606_SPI4_FrameInfo_t history_frame_info;
  AD7606_SPI4_RawInfo_t history_raw_info;
  AD7606_SPI4_RawInfo_t window_raw_info;
  uint32_t copied_bytes;
  uint32_t inference_ms;
  uint32_t burst_frames = 0U;
  int32_t run_status;
  uint8_t have_history = 0U;
  int8_t *input_buffer;
  int8_t *output_buffer;

  (void)thread_input;
  App_Print("AI thread start\r\n");
  AppAI_ConfigureHardware();
  if (AppAI_CheckWeights() == 0U)
  {
    uint32_t primask = AppAI_StatusLock();

    AppAIStatus.fault = 1U;
    AppAIStatus.initialized = 1U;
    AppAI_StatusUnlock(primask);
    App_Print("AI weights invalid at 0x71000000\r\n");
    for (;;)
    {
      tx_thread_sleep(TX_TIMER_TICKS_PER_SECOND);
    }
  }

  {
    uint32_t primask = AppAI_StatusLock();

    AppAIStatus.weights_ok = 1U;
    AppAI_StatusUnlock(primask);
  }

  AppAI_InitializeActivationMemory();
  LL_ATON_RT_RuntimeInit();
  LL_ATON_RT_Init_Network(&NN_Instance_tiny_temporal_mixer_8ch_int8);

  input_info = LL_ATON_Input_Buffers_Info(&NN_Instance_tiny_temporal_mixer_8ch_int8);
  output_info = LL_ATON_Output_Buffers_Info(&NN_Instance_tiny_temporal_mixer_8ch_int8);
  if ((input_info == NULL) || (input_info->name == NULL) ||
      (output_info == NULL) || (output_info->name == NULL) ||
      (LL_Buffer_len(input_info) != APP_AI_INPUT_BYTES) ||
      (LL_Buffer_len(output_info) != APP_AI_OUTPUT_BYTES))
  {
    uint32_t primask = AppAI_StatusLock();

    App_Print("AI model IO invalid\r\n");
    AppAIStatus.fault = 1U;
    AppAIStatus.initialized = 1U;
    AppAI_StatusUnlock(primask);
    for (;;)
    {
      tx_thread_sleep(TX_TIMER_TICKS_PER_SECOND);
    }
  }

  input_buffer = (int8_t *)LL_Buffer_addr_start(input_info);
  output_buffer = (int8_t *)LL_Buffer_addr_start(output_info);

  {
    uint32_t primask = AppAI_StatusLock();

    AppAIStatus.initialized = 1U;
    AppAIStatus.ready = 1U;
    AppAI_StatusUnlock(primask);
  }
  App_Print("AI ready model=tiny_temporal_mixer_8ch_int8 source=AD7606\r\n");

  for (;;)
  {
    uint8_t *frame_destination = (have_history == 0U) ?
                                 &AppAIRawSamples[0U] :
                                 &AppAIRawSamples[APP_AI_FRAME_BYTES];

    if (AppAIProcessingActive == 0U)
    {
      have_history = 0U;
      burst_frames = 0U;
      tx_thread_sleep(5U);
      continue;
    }

    AppAI_SetProcessingBusy(1U);
    if (AppAIProcessingActive == 0U)
    {
      AppAI_SetProcessingBusy(0U);
      have_history = 0U;
      burst_frames = 0U;
      continue;
    }

    copied_bytes = AD7606_SPI4_DequeueAIFrame(frame_destination,
                                              APP_AI_FRAME_BYTES,
                                              &frame_info,
                                              &raw_info);
    if ((copied_bytes == 0U) ||
        (raw_info.bytes_per_sample != APP_AI_INPUT_SAMPLE_BYTES) ||
        (raw_info.channels != APP_AI_CHANNELS) ||
        (raw_info.points != APP_AI_FRAME_SAMPLES))
    {
      if (copied_bytes != 0U)
      {
        uint32_t primask = AppAI_StatusLock();
        AppAIStatus.copy_error_count++;
        AppAIStatus.window_reset_count++;
        AppAI_StatusUnlock(primask);
        have_history = 0U;
      }
      AppAI_SetProcessingBusy(0U);
      tx_thread_sleep(APP_AI_THREAD_SLEEP_TICKS);
      continue;
    }

    {
      uint32_t primask = AppAI_StatusLock();
      AppAIStatus.input_frame_count++;
      AppAI_StatusUnlock(primask);
    }
    burst_frames++;

    if (have_history == 0U)
    {
      history_frame_info = frame_info;
      history_raw_info = raw_info;
      have_history = 1U;

      {
        uint32_t primask = AppAI_StatusLock();
        AppAIStatus.warmup_count++;
        AppAI_StatusUnlock(primask);
      }
      AppAI_SetProcessingBusy(0U);
      AppAI_YieldAfterBurst(&burst_frames);
      continue;
    }

    {
      uint32_t frame_delta = frame_info.frame_seq - history_frame_info.frame_seq;
      uint8_t continuous = ((frame_delta == 1U) &&
                            (raw_info.block_start == (history_raw_info.block_end + 1ULL))) ?
                           1U : 0U;

      if (continuous == 0U)
      {
        uint32_t primask = AppAI_StatusLock();

        AppAIStatus.input_source_gap_count++;
        AppAIStatus.window_reset_count++;
        if ((frame_delta > 1U) && (frame_delta < 0x80000000U))
        {
          AppAIStatus.skip_count += frame_delta - 1U;
        }
        AppAI_StatusUnlock(primask);

        (void)memcpy(&AppAIRawSamples[0U],
                     &AppAIRawSamples[APP_AI_FRAME_BYTES],
                     APP_AI_FRAME_BYTES);
        history_frame_info = frame_info;
        history_raw_info = raw_info;
        {
          uint32_t primask = AppAI_StatusLock();
          AppAIStatus.warmup_count++;
          AppAI_StatusUnlock(primask);
        }
        AppAI_SetProcessingBusy(0U);
        AppAI_YieldAfterBurst(&burst_frames);
        continue;
      }
    }

    window_raw_info.points = APP_AI_WINDOW_SAMPLES;
    window_raw_info.channels = APP_AI_CHANNELS;
    window_raw_info.bytes_per_sample = APP_AI_INPUT_SAMPLE_BYTES;
    window_raw_info.sample_bytes = APP_AI_INPUT_WINDOW_BYTES;
    window_raw_info.block_start = history_raw_info.block_start;
    window_raw_info.block_end = raw_info.block_end;

    AppAI_CopyRawWindowToInput(AppAIRawSamples, &window_raw_info, input_buffer);
    if (AppAI_RunInference(input_buffer, output_buffer, &inference_ms, &run_status) == 0U)
    {
      uint32_t primask = AppAI_StatusLock();

      AppAIStatus.run_error_count++;
      AppAIStatus.last_run_error_ms = inference_ms;
      AppAIStatus.last_run_status = run_status;
      AppAI_StatusUnlock(primask);
    }
    else
    {
      AppAI_RecordRun(&frame_info, output_buffer, inference_ms, run_status);
      AppAI_BundleEnqueue(&AppAIRawSamples[APP_AI_FRAME_BYTES],
                          &raw_info,
                          &window_raw_info);
    }

    LL_ATON_RT_Reset_Network(&NN_Instance_tiny_temporal_mixer_8ch_int8);
    (void)memcpy(&AppAIRawSamples[0U],
                 &AppAIRawSamples[APP_AI_FRAME_BYTES],
                 APP_AI_FRAME_BYTES);
    history_frame_info = frame_info;
    history_raw_info = raw_info;
    AppAI_SetProcessingBusy(0U);
    AppAI_YieldAfterBurst(&burst_frames);
  }
}

UINT App_AI_Start(TX_BYTE_POOL *byte_pool)
{
  UCHAR *thread_stack;
  UINT status;

  (void)memset((void *)&AppAIStatus, 0, sizeof(AppAIStatus));
  (void)memset(AppAIRawSamples, 0, sizeof(AppAIRawSamples));
  AppAIBundleConsumerActive = 0U;
  AppAIBundleQueueHead = 0U;
  AppAIBundleQueueTail = 0U;
  AppAIBundleQueueCount = 0U;
  AppAILastInferenceFrameSeq = 0U;
  AppAIHaveInferenceFrame = 0U;
  AppAIProcessingActive = 1U;
  AppAIProcessingBusy = 0U;
  AppAIStatus.target_rate_hz = APP_AI_TARGET_RATE_HZ;

  status = AppAI_ByteAllocate(byte_pool, &thread_stack, APP_AI_THREAD_STACK_SIZE);
  if (status != TX_SUCCESS)
  {
    return status;
  }

  status = tx_thread_create(&AppAIThread,
                            "AI AD7606 worker",
                            AppAI_ThreadEntry,
                            0,
                            thread_stack,
                            APP_AI_THREAD_STACK_SIZE,
                            APP_AI_THREAD_PRIORITY,
                            APP_AI_THREAD_PRIORITY,
                            TX_NO_TIME_SLICE,
                            TX_AUTO_START);
  if (status != TX_SUCCESS)
  {
    App_PrintHex32("AI thread create failed: ", status);
    return status;
  }

  App_Print("AI thread created\r\n");
  return TX_SUCCESS;
}

void App_AI_GetStatus(App_AI_Status_t *status)
{
  uint32_t primask;

  if (status == NULL)
  {
    return;
  }

  primask = AppAI_StatusLock();
  (void)memcpy(status, (const void *)&AppAIStatus, sizeof(*status));
  AppAI_StatusUnlock(primask);
}

UINT App_AI_SetProcessingActive(uint8_t active, ULONG wait_ticks)
{
  ULONG start_tick;
  uint32_t primask = AppAI_StatusLock();

  AppAIProcessingActive = (active != 0U) ? 1U : 0U;
  AppAI_StatusUnlock(primask);

  if (active != 0U)
  {
    return TX_SUCCESS;
  }

  if (AppAIProcessingBusy == 0U)
  {
    return TX_SUCCESS;
  }

  start_tick = tx_time_get();
  for (;;)
  {
    if (AppAIProcessingBusy == 0U)
    {
      return TX_SUCCESS;
    }
    if ((wait_ticks != TX_WAIT_FOREVER) &&
        ((tx_time_get() - start_tick) >= wait_ticks))
    {
      return TX_NOT_DONE;
    }
    tx_thread_sleep(1U);
  }
}

void App_AI_SetBundleConsumerActive(uint8_t active)
{
  uint32_t primask = AppAI_StatusLock();
  uint8_t new_active = (active != 0U) ? 1U : 0U;

  if (new_active != AppAIBundleConsumerActive)
  {
    AppAIBundleConsumerActive = new_active;
    AppAI_BundleQueueResetLocked();
  }
  AppAI_StatusUnlock(primask);
}

uint32_t App_AI_CopyLatestBundle(uint8_t *raw_window,
                                 uint32_t raw_capacity,
                                 App_AI_Bundle_Info_t *bundle_info)
{
  uint32_t queue_index;
  uint32_t primask;

  if ((raw_window == NULL) || (bundle_info == NULL) ||
      (raw_capacity < APP_AI_SOURCE_FRAME_BYTES))
  {
    return 0U;
  }

  primask = AppAI_StatusLock();
  if (AppAIBundleQueueCount == 0U)
  {
    AppAI_StatusUnlock(primask);
    return 0U;
  }
  queue_index = AppAIBundleQueueTail;
  AppAI_StatusUnlock(primask);

  (void)memcpy(raw_window,
               AppAIBundleQueue[queue_index].raw_frame,
               APP_AI_SOURCE_FRAME_BYTES);
  *bundle_info = AppAIBundleQueue[queue_index].bundle_info;
  __DMB();

  primask = AppAI_StatusLock();
  if ((AppAIBundleQueueCount == 0U) ||
      (AppAIBundleQueueTail != queue_index))
  {
    AppAI_StatusUnlock(primask);
    return 0U;
  }
  AppAIBundleQueueTail =
    (queue_index + 1U) % APP_AI_BUNDLE_QUEUE_DEPTH;
  AppAIBundleQueueCount--;
  AppAIStatus.output_queue_depth = AppAIBundleQueueCount;
  AppAI_StatusUnlock(primask);

  return APP_AI_SOURCE_FRAME_BYTES;
}
