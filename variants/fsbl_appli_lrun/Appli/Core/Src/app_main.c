#include "app_main.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fixed_input_8ch_1024_int8.h"
#include "gpio.h"
#include "ll_aton_rt_user_api.h"
#include "main.h"
#include "npu_cache.h"
#include "tiny_temporal_mixer_8ch_int8.h"
#include "usart.h"

LL_ATON_DECLARE_NAMED_NN_INSTANCE_AND_INTERFACE(tiny_temporal_mixer_8ch_int8);

#define NPU_AI_OUTPUT_CLASS_COUNT 4U
#define NPU_AI_EXPECTED_TOP1_INDEX 1U
#define NPU_AI_OUTPUT_SCALE_MICRO 134L
#define NPU_AI_OUTPUT_ZERO_POINT (-128)
#define NPU_AI_RUN_TIMEOUT_MS 5000U
#define NPU_AI_CACHE_LINE_BYTES 32U
#define NPU_AI_REPEAT_COUNT 3U
#define NPU_AI_WEIGHTS_BASE 0x71000000UL
#define NPU_AI_WEIGHTS_TOTAL_LEN 98833U
#define NPU_AI_WEIGHTS_PROBE_LEN 256U
#define NPU_AI_WEIGHTS_PROBE_SUM 0x00007E32UL
#define NPU_AI_WEIGHTS_FULL_SUM 0x00BEC661UL
#define NPU_AI_WEIGHTS_TAIL_OFFSET 79872U
#define NPU_AI_WEIGHTS_TAIL_SUM 0x00007E90UL
#define NPU_AI_USE_NPURAM_USER_IO 1U
#define NPU_AI_NPURAM_USER_IO_BASE 0x342E0000UL
#define NPU_AI_NPURAM_POOL_BYTES 458752U

static const char *const g_ai_class_names[NPU_AI_OUTPUT_CLASS_COUNT] = {
  "class_0",
  "class_1",
  "class_2",
  "class_3",
};

static const int8_t g_ai_expected_output_int8[NPU_AI_OUTPUT_CLASS_COUNT] = {
  6, 65, 3, -69,
};

static const int32_t g_ai_expected_output_float_micro[NPU_AI_OUTPUT_CLASS_COUNT] = {
  17962, 25737, 17426, 7909,
};

static const uint8_t g_ai_weights_expected_first16[16] = {
  0x4B, 0x2E, 0x9F, 0xBD, 0x75, 0x37, 0x83, 0x3D,
  0x3E, 0x81, 0xCE, 0xBD, 0xBB, 0x63, 0xB0, 0xBD,
};

#if !NPU_AI_USE_NPURAM_USER_IO
__attribute__((aligned(32))) static int8_t g_npu_input_buffer[LL_ATON_TINY_TEMPORAL_MIXER_8CH_INT8_IN_1_SIZE_BYTES];
__attribute__((aligned(32))) static int8_t g_npu_output_buffer[32];
#endif

static uint32_t g_heartbeat_tick;

static void App_Print(const char *text)
{
  (void)HAL_UART_Transmit(&huart3, (const uint8_t *)text, (uint16_t)strlen(text), HAL_MAX_DELAY);
}

static void App_PrintQuantScore(const char *prefix, int8_t raw, const char *suffix)
{
  char line[128];
  int32_t scaled = ((int32_t)raw - NPU_AI_OUTPUT_ZERO_POINT) * NPU_AI_OUTPUT_SCALE_MICRO;
  uint32_t integer;
  uint32_t fraction;

  if (scaled < 0)
  {
    integer = (uint32_t)((-scaled) / 1000000L);
    fraction = (uint32_t)((-scaled) % 1000000L);
    (void)snprintf(line, sizeof(line), "%sraw=%ld score=-%lu.%06lu%s",
                   prefix, (long)raw, (unsigned long)integer, (unsigned long)fraction, suffix);
  }
  else
  {
    integer = (uint32_t)(scaled / 1000000L);
    fraction = (uint32_t)(scaled % 1000000L);
    (void)snprintf(line, sizeof(line), "%sraw=%ld score=%lu.%06lu%s",
                   prefix, (long)raw, (unsigned long)integer, (unsigned long)fraction, suffix);
  }
  App_Print(line);
}

static int32_t App_FloatToMicro(float value)
{
  float scaled = value * 1000000.0f;

  if (scaled >= 0.0f)
  {
    return (int32_t)(scaled + 0.5f);
  }

  return (int32_t)(scaled - 0.5f);
}

static void App_PrintMicroScore(const char *prefix, int32_t score_micro, const char *suffix)
{
  char line[128];
  uint32_t integer;
  uint32_t fraction;

  if (score_micro < 0)
  {
    integer = (uint32_t)((-score_micro) / 1000000L);
    fraction = (uint32_t)((-score_micro) % 1000000L);
    (void)snprintf(line, sizeof(line), "%sscore=-%lu.%06lu%s",
                   prefix, (unsigned long)integer, (unsigned long)fraction, suffix);
  }
  else
  {
    integer = (uint32_t)(score_micro / 1000000L);
    fraction = (uint32_t)(score_micro % 1000000L);
    (void)snprintf(line, sizeof(line), "%sscore=%lu.%06lu%s",
                   prefix, (unsigned long)integer, (unsigned long)fraction, suffix);
  }
  App_Print(line);
}

static float App_ReadFloat32(const void *addr)
{
  float value;

  (void)memcpy(&value, addr, sizeof(value));
  return value;
}

static void App_CacheCleanAligned(uintptr_t addr, uint32_t size)
{
  uintptr_t start = addr & ~((uintptr_t)NPU_AI_CACHE_LINE_BYTES - 1U);
  uintptr_t end = (addr + size + NPU_AI_CACHE_LINE_BYTES - 1U) &
                  ~((uintptr_t)NPU_AI_CACHE_LINE_BYTES - 1U);

  LL_ATON_Cache_MCU_Clean_Range(start, (uint32_t)(end - start));
}

static void App_CacheInvalidateAligned(uintptr_t addr, uint32_t size)
{
  uintptr_t start = addr & ~((uintptr_t)NPU_AI_CACHE_LINE_BYTES - 1U);
  uintptr_t end = (addr + size + NPU_AI_CACHE_LINE_BYTES - 1U) &
                  ~((uintptr_t)NPU_AI_CACHE_LINE_BYTES - 1U);

  LL_ATON_Cache_MCU_Invalidate_Range(start, (uint32_t)(end - start));
}

static uint32_t App_ByteSum(const volatile uint8_t *data, uint32_t len)
{
  uint32_t checksum = 0U;

  for (uint32_t i = 0U; i < len; i++)
  {
    checksum += data[i];
  }

  return checksum;
}

static void App_PrintMemProbe(const char *label, const void *addr, uint32_t sum_len)
{
  const volatile uint8_t *data = (const volatile uint8_t *)addr;
  char line[192];

  (void)snprintf(line, sizeof(line),
                 "%s addr=0x%08lX first16=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X sum%lu=0x%08lX\r\n",
                 label,
                 (unsigned long)(uintptr_t)addr,
                 data[0], data[1], data[2], data[3],
                 data[4], data[5], data[6], data[7],
                 data[8], data[9], data[10], data[11],
                 data[12], data[13], data[14], data[15],
                 (unsigned long)sum_len,
                 (unsigned long)App_ByteSum(data, sum_len));
  App_Print(line);
}

static void App_PrintNpuRamProbe(const char *label, uint32_t offset, uint32_t sum_len)
{
  App_PrintMemProbe(label, (const void *)(NPU_AI_NPURAM_USER_IO_BASE + offset), sum_len);
}

static void App_PrintRisafAccessState(const char *label)
{
  char line[192];

  (void)snprintf(line, sizeof(line),
                 "%s RISAF2 IASR=0x%08lX IAESR=0x%08lX IADDR=0x%08lX RISAF5 IASR=0x%08lX IAESR=0x%08lX IADDR=0x%08lX RISAF12 IASR=0x%08lX IAESR=0x%08lX IADDR=0x%08lX\r\n",
                 label,
                 (unsigned long)RISAF2->IASR,
                 (unsigned long)RISAF2->IAR[0].IAESR,
                 (unsigned long)RISAF2->IAR[0].IADDR,
                 (unsigned long)RISAF5->IASR,
                 (unsigned long)RISAF5->IAR[0].IAESR,
                 (unsigned long)RISAF5->IAR[0].IADDR,
                 (unsigned long)RISAF12->IASR,
                 (unsigned long)RISAF12->IAR[0].IAESR,
                 (unsigned long)RISAF12->IAR[0].IADDR);
  App_Print(line);
}

static void App_ClearNpuRamPool(void)
{
  void *pool = (void *)NPU_AI_NPURAM_USER_IO_BASE;

  App_CacheInvalidateAligned((uintptr_t)pool, NPU_AI_NPURAM_POOL_BYTES);
  (void)memset(pool, 0, NPU_AI_NPURAM_POOL_BYTES);
  App_CacheCleanAligned((uintptr_t)pool, NPU_AI_NPURAM_POOL_BYTES);
}

static void App_PrintWeightsProbe(void)
{
  volatile const uint8_t *weights = (volatile const uint8_t *)NPU_AI_WEIGHTS_BASE;
  uint32_t checksum = 0U;
  uint32_t full_checksum = 0U;
  uint32_t tail_checksum = 0U;
  uint32_t match = 1U;
  char line[192];

  for (uint32_t i = 0U; i < NPU_AI_WEIGHTS_PROBE_LEN; i++)
  {
    uint8_t value = weights[i];
    checksum += value;
    if ((i < sizeof(g_ai_weights_expected_first16)) && (value != g_ai_weights_expected_first16[i]))
    {
      match = 0U;
    }
  }

  (void)snprintf(line, sizeof(line),
                 "NPU weights probe addr=0x%08lX first16=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X sum256=0x%08lX exp=0x%08lX match=%lu\r\n",
                 (unsigned long)NPU_AI_WEIGHTS_BASE,
                 weights[0], weights[1], weights[2], weights[3],
                 weights[4], weights[5], weights[6], weights[7],
                 weights[8], weights[9], weights[10], weights[11],
                 weights[12], weights[13], weights[14], weights[15],
                 (unsigned long)checksum,
                 (unsigned long)NPU_AI_WEIGHTS_PROBE_SUM,
                 (unsigned long)((match != 0U) && (checksum == NPU_AI_WEIGHTS_PROBE_SUM)));
  App_Print(line);

  for (uint32_t i = 0U; i < NPU_AI_WEIGHTS_TOTAL_LEN; i++)
  {
    full_checksum += weights[i];
  }
  for (uint32_t i = 0U; i < NPU_AI_WEIGHTS_PROBE_LEN; i++)
  {
    tail_checksum += weights[NPU_AI_WEIGHTS_TAIL_OFFSET + i];
  }

  (void)snprintf(line, sizeof(line),
                 "NPU weights full len=%lu sum=0x%08lX exp=0x%08lX tail@%lu sum256=0x%08lX exp=0x%08lX match=%lu\r\n",
                 (unsigned long)NPU_AI_WEIGHTS_TOTAL_LEN,
                 (unsigned long)full_checksum,
                 (unsigned long)NPU_AI_WEIGHTS_FULL_SUM,
                 (unsigned long)NPU_AI_WEIGHTS_TAIL_OFFSET,
                 (unsigned long)tail_checksum,
                 (unsigned long)NPU_AI_WEIGHTS_TAIL_SUM,
                 (unsigned long)((full_checksum == NPU_AI_WEIGHTS_FULL_SUM) &&
                                 (tail_checksum == NPU_AI_WEIGHTS_TAIL_SUM)));
  App_Print(line);
}

static void App_PrintNpuClockState(const char *prefix)
{
  char line[160];

  (void)snprintf(line, sizeof(line),
                 "%s AHB5ENR=0x%08lX AHB2ENR=0x%08lX MEMENR=0x%08lX NPU_CLK=%lu AXISRAM5=%lu RAM5_CR=0x%08lX\r\n",
                 prefix,
                 (unsigned long)RCC->AHB5ENR,
                 (unsigned long)RCC->AHB2ENR,
                 (unsigned long)RCC->MEMENR,
                 (unsigned long)__HAL_RCC_NPU_IS_CLK_ENABLED(),
                 (unsigned long)__HAL_RCC_AXISRAM5_MEM_IS_CLK_ENABLED(),
                 (unsigned long)RAMCFG_SRAM5_AXI->CR);
  App_Print(line);
}

static void App_PrintBufferInfo(const char *prefix, const LL_Buffer_InfoTypeDef *info)
{
  char line[192];

  (void)snprintf(line, sizeof(line),
                 "%s name=%s type=%lu nbits=%lu start=%lu end=%lu limit=%lu scale_u=%ld offset=%ld\r\n",
                 prefix,
                 info->name,
                 (unsigned long)info->type,
                 (unsigned long)info->nbits,
                 (unsigned long)info->offset_start,
                 (unsigned long)info->offset_end,
                 (unsigned long)info->offset_limit,
                 (long)((info->scale != NULL) ? App_FloatToMicro(info->scale[0]) : 0L),
                 (long)((info->offset != NULL) ? info->offset[0] : 0));
  App_Print(line);
}

static void App_ConfigureNpuWeightAccess(void)
{
  RIMC_MasterConfig_t npu_master_config = {0};
  RISAF_BaseRegionConfig_t risaf_config = {0};

  npu_master_config.MasterCID = RIF_CID_0;
  npu_master_config.SecPriv = RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV;
  HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_NPU, &npu_master_config);

  (void)HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_XSPI2,
                                              RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_NPRIV);

  risaf_config.Filtering = RISAF_FILTER_ENABLE;
  risaf_config.Secure = RIF_ATTRIBUTE_SEC;
  risaf_config.PrivWhitelist = RIF_CID_NONE;
  risaf_config.ReadWhitelist = RIF_CID_MASK;
  risaf_config.WriteWhitelist = RIF_CID_MASK;

  risaf_config.StartAddress = 0x00000000U;
  risaf_config.EndAddress = 0x07FFFFFFU;
  HAL_RIF_RISAF_ConfigBaseRegion(RISAF12, RISAF_REGION_1, &risaf_config);

  risaf_config.StartAddress = NPU_AI_NPURAM_USER_IO_BASE;
  risaf_config.EndAddress = NPU_AI_NPURAM_USER_IO_BASE + NPU_AI_NPURAM_POOL_BYTES - 1U;
  HAL_RIF_RISAF_ConfigBaseRegion(RISAF4, RISAF_REGION_1, &risaf_config);
  HAL_RIF_RISAF_ConfigBaseRegion(RISAF5, RISAF_REGION_1, &risaf_config);

  risaf_config.StartAddress = NPU_AI_WEIGHTS_BASE;
  risaf_config.EndAddress = 0x7101FFFFU;
  HAL_RIF_RISAF_ConfigBaseRegion(RISAF4, RISAF_REGION_2, &risaf_config);
  HAL_RIF_RISAF_ConfigBaseRegion(RISAF5, RISAF_REGION_2, &risaf_config);

  RISAF4->IACR = RISAF_IACR_CAEF | RISAF_IACR_IAEF;
  RISAF5->IACR = RISAF_IACR_CAEF | RISAF_IACR_IAEF;
  RISAF12->IACR = RISAF_IACR_CAEF | RISAF_IACR_IAEF;
}

static void App_NpuHwPrepare(void)
{
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

  __HAL_RCC_AXISRAM3_MEM_CLK_ENABLE();
  __HAL_RCC_AXISRAM4_MEM_CLK_ENABLE();
  __HAL_RCC_AXISRAM5_MEM_CLK_ENABLE();
  __HAL_RCC_AXISRAM6_MEM_CLK_ENABLE();
  __HAL_RCC_CACHEAXIRAM_MEM_CLK_ENABLE();
  __HAL_RCC_RAMCFG_CLK_ENABLE();
  __HAL_RCC_NPU_CLK_ENABLE();
  __HAL_RCC_NPU_CLK_SLEEP_ENABLE();

  App_ConfigureNpuWeightAccess();

  RAMCFG_SRAM3_AXI->CR &= ~RAMCFG_CR_SRAMSD;
  RAMCFG_SRAM4_AXI->CR &= ~RAMCFG_CR_SRAMSD;
  RAMCFG_SRAM5_AXI->CR &= ~RAMCFG_CR_SRAMSD;
  RAMCFG_SRAM6_AXI->CR &= ~RAMCFG_CR_SRAMSD;

  HAL_NVIC_SetPriority(NPU0_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(NPU0_IRQn);
  HAL_NVIC_SetPriority(NPU1_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(NPU1_IRQn);
  HAL_NVIC_SetPriority(CACHEAXI_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(CACHEAXI_IRQn);

  npu_cache_init();
  npu_cache_enable();
  npu_cache_invalidate();
}

static void App_RunNpuSelfTest(void)
{
  const LL_Buffer_InfoTypeDef *input_info;
  const LL_Buffer_InfoTypeDef *output_info;
  uint8_t *input_buffer;
  const int8_t *output_buffer;
  LL_ATON_RT_RetValues_t run_status;
  uint32_t input_size;
  uint32_t output_size;
  uint32_t top_index = 0U;
  int8_t top_score;
  uint32_t mismatch_count = 0U;
  uint32_t float_top_index = 0U;
  int32_t float_top_score_micro;
  uint32_t float_mismatch_count = 0U;
  uint32_t output_probe_size;
  uint32_t run_start_tick;
  uint32_t run_loop_count = 0U;
  uint32_t wfe_count = 0U;
  char line[128];

  App_Print("NPU AI self-test begin\r\n");

  App_Print("NPU stage hw prepare\r\n");
  App_NpuHwPrepare();
  App_PrintNpuClockState("NPU stage hw ready");
  App_PrintWeightsProbe();

  App_Print("NPU stage clear activation pool\r\n");
  App_ClearNpuRamPool();

  App_Print("NPU stage runtime init\r\n");
  LL_ATON_RT_RuntimeInit();
  App_Print("NPU stage runtime init done\r\n");

  App_Print("NPU stage network init\r\n");
  LL_ATON_RT_Init_Network(&NN_Instance_tiny_temporal_mixer_8ch_int8);
  App_Print("NPU stage network init done\r\n");

  {
    LL_ATON_User_IO_Result_t input_set_result;
    LL_ATON_User_IO_Result_t output_set_result;
#if NPU_AI_USE_NPURAM_USER_IO
    void *user_input_buffer = (void *)NPU_AI_NPURAM_USER_IO_BASE;
    void *user_output_buffer = (void *)NPU_AI_NPURAM_USER_IO_BASE;
#else
    void *user_input_buffer = g_npu_input_buffer;
    void *user_output_buffer = g_npu_output_buffer;
#endif

    input_set_result = LL_ATON_Set_User_Input_Buffer(&NN_Instance_tiny_temporal_mixer_8ch_int8,
                                                     0U,
                                                     user_input_buffer,
                                                     LL_ATON_TINY_TEMPORAL_MIXER_8CH_INT8_IN_1_SIZE_BYTES);
    output_set_result = LL_ATON_Set_User_Output_Buffer(&NN_Instance_tiny_temporal_mixer_8ch_int8,
                                                       0U,
                                                       user_output_buffer,
                                                       32U);
    (void)snprintf(line, sizeof(line),
                   "NPU user io set mode=%s input=%ld output=%ld in_addr=0x%08lX out_addr=0x%08lX\r\n",
#if NPU_AI_USE_NPURAM_USER_IO
                   "npuram",
#else
                   "cpuram",
#endif
                   (long)input_set_result,
                   (long)output_set_result,
                   (unsigned long)(uintptr_t)user_input_buffer,
                   (unsigned long)(uintptr_t)user_output_buffer);
    App_Print(line);
    if ((input_set_result != LL_ATON_User_IO_NOERROR) ||
        (output_set_result != LL_ATON_User_IO_NOERROR))
    {
      App_Print("NPU user io set FAIL\r\n");
      LL_ATON_RT_DeInit_Network(&NN_Instance_tiny_temporal_mixer_8ch_int8);
      LL_ATON_RT_RuntimeDeInit();
      return;
    }
  }

  App_Print("NPU stage io info\r\n");
  input_info = LL_ATON_Input_Buffers_Info(&NN_Instance_tiny_temporal_mixer_8ch_int8);
  output_info = LL_ATON_Output_Buffers_Info(&NN_Instance_tiny_temporal_mixer_8ch_int8);
  if ((input_info == NULL) || (input_info->name == NULL) ||
      (output_info == NULL) || (output_info->name == NULL))
  {
    App_Print("NPU AI io info FAIL\r\n");
    LL_ATON_RT_DeInit_Network(&NN_Instance_tiny_temporal_mixer_8ch_int8);
    LL_ATON_RT_RuntimeDeInit();
    return;
  }

  input_size = LL_Buffer_len(input_info);
  output_size = LL_Buffer_len(output_info);
  if ((input_size != LL_ATON_TINY_TEMPORAL_MIXER_8CH_INT8_IN_1_SIZE_BYTES) ||
      (input_size != sizeof(g_ai_fixed_input_int8)) ||
      (output_size != NPU_AI_OUTPUT_CLASS_COUNT))
  {
    (void)snprintf(line, sizeof(line), "NPU AI io size FAIL in=%lu fixed=%lu out=%lu\r\n",
                   (unsigned long)input_size,
                   (unsigned long)sizeof(g_ai_fixed_input_int8),
                   (unsigned long)output_size);
    App_Print(line);
    LL_ATON_RT_DeInit_Network(&NN_Instance_tiny_temporal_mixer_8ch_int8);
    LL_ATON_RT_RuntimeDeInit();
    return;
  }

  input_buffer = LL_Buffer_addr_start(input_info);
  output_buffer = (const int8_t *)LL_Buffer_addr_start(output_info);
  (void)snprintf(line, sizeof(line),
                 "NPU io input=0x%08lX bytes=%lu output=0x%08lX bytes=%lu\r\n",
                 (unsigned long)(uintptr_t)input_buffer,
                 (unsigned long)input_size,
                 (unsigned long)(uintptr_t)output_buffer,
                 (unsigned long)output_size);
  App_Print(line);
  App_PrintBufferInfo("NPU input info", input_info);
  App_PrintBufferInfo("NPU output info", output_info);

  App_Print("NPU stage input copy\r\n");
  (void)memcpy(input_buffer, g_ai_fixed_input_int8, input_size);
  App_PrintMemProbe("NPU input after copy", input_buffer, 256U);
  App_PrintMemProbe("NPU input ch1 sample", input_buffer + 1024U, 64U);
  App_CacheCleanAligned((uintptr_t)input_buffer, input_size);
  App_PrintMemProbe("NPU input after clean", input_buffer, 256U);
  App_PrintNpuRamProbe("NPU scratch pre +0x2000", 0x00002000U, 64U);
  App_PrintNpuRamProbe("NPU scratch pre +0x6000", 0x00006000U, 64U);
  App_PrintNpuRamProbe("NPU scratch pre +0xC000", 0x0000C000U, 64U);
  App_PrintNpuRamProbe("NPU scratch pre +0x48000", 0x00048000U, 64U);
  App_PrintRisafAccessState("NPU RISAF pre");

  App_Print("NPU stage run loop\r\n");
  run_start_tick = HAL_GetTick();
  do
  {
    run_status = LL_ATON_RT_RunEpochBlock(&NN_Instance_tiny_temporal_mixer_8ch_int8);
    run_loop_count++;
    if (run_status == LL_ATON_RT_WFE)
    {
      wfe_count++;
      __SEV();
      __WFE();
    }

    if ((HAL_GetTick() - run_start_tick) > NPU_AI_RUN_TIMEOUT_MS)
    {
      (void)snprintf(line, sizeof(line),
                     "NPU AI run timeout status=%ld loops=%lu wfe=%lu elapsed=%lu\r\n",
                     (long)run_status,
                     (unsigned long)run_loop_count,
                     (unsigned long)wfe_count,
                     (unsigned long)(HAL_GetTick() - run_start_tick));
      App_Print(line);
      LL_ATON_RT_DeInit_Network(&NN_Instance_tiny_temporal_mixer_8ch_int8);
      LL_ATON_RT_RuntimeDeInit();
      return;
    }
  } while (run_status != LL_ATON_RT_DONE);

  (void)snprintf(line, sizeof(line),
                 "NPU stage run done loops=%lu wfe=%lu elapsed=%lu\r\n",
                 (unsigned long)run_loop_count,
                 (unsigned long)wfe_count,
                 (unsigned long)(HAL_GetTick() - run_start_tick));
  App_Print(line);

  output_probe_size = output_info->offset_limit - output_info->offset_start;
  if (output_probe_size < output_size)
  {
    output_probe_size = output_size;
  }
  App_CacheInvalidateAligned((uintptr_t)output_buffer, output_probe_size);
  App_PrintMemProbe("NPU output probe", output_buffer, 32U);
  App_PrintNpuRamProbe("NPU scratch post +0x2000", 0x00002000U, 64U);
  App_PrintNpuRamProbe("NPU scratch post +0x6000", 0x00006000U, 64U);
  App_PrintNpuRamProbe("NPU scratch post +0xC000", 0x0000C000U, 64U);
  App_PrintNpuRamProbe("NPU scratch post +0x48000", 0x00048000U, 64U);
  App_PrintRisafAccessState("NPU RISAF post");
  (void)snprintf(line, sizeof(line),
                 "NPU output raw bytes=%02X %02X %02X %02X\r\n",
                 (uint8_t)output_buffer[0],
                 (uint8_t)output_buffer[1],
                 (uint8_t)output_buffer[2],
                 (uint8_t)output_buffer[3]);
  App_Print(line);

  App_Print("NPU adjacent float32 invalid-view begin\r\n");
  float_top_score_micro = App_FloatToMicro(App_ReadFloat32(&((const uint8_t *)output_buffer)[0]));
  for (uint32_t i = 0U; i < NPU_AI_OUTPUT_CLASS_COUNT; i++)
  {
    int32_t score_micro = App_FloatToMicro(App_ReadFloat32(&((const uint8_t *)output_buffer)[i * sizeof(float)]));
    int32_t diff_micro = score_micro - g_ai_expected_output_float_micro[i];

    if (diff_micro < 0)
    {
      diff_micro = -diff_micro;
    }
    if (diff_micro > 2000L)
    {
      float_mismatch_count++;
    }
    if (score_micro > float_top_score_micro)
    {
      float_top_score_micro = score_micro;
      float_top_index = i;
    }

    (void)snprintf(line, sizeof(line), "NPU float[%lu] %s ",
                   (unsigned long)i, g_ai_class_names[i]);
    App_PrintMicroScore(line, score_micro, " ");
    (void)snprintf(line, sizeof(line), "exp=%ld.%06ld\r\n",
                   (long)(g_ai_expected_output_float_micro[i] / 1000000L),
                   (long)(g_ai_expected_output_float_micro[i] % 1000000L));
    App_Print(line);
  }
  (void)snprintf(line, sizeof(line),
                 "NPU float top1 index=%lu label=%s expected=%lu mismatches=%lu\r\n",
                 (unsigned long)float_top_index,
                 g_ai_class_names[float_top_index],
                 (unsigned long)NPU_AI_EXPECTED_TOP1_INDEX,
                 (unsigned long)float_mismatch_count);
  App_Print(line);

  App_Print("NPU output int8 view begin\r\n");
  top_score = output_buffer[0];
  for (uint32_t i = 0U; i < NPU_AI_OUTPUT_CLASS_COUNT; i++)
  {
    if (output_buffer[i] != g_ai_expected_output_int8[i])
    {
      mismatch_count++;
    }

    if (output_buffer[i] > top_score)
    {
      top_score = output_buffer[i];
      top_index = i;
    }

    (void)snprintf(line, sizeof(line), "NPU output[%lu] %s ",
                   (unsigned long)i, g_ai_class_names[i]);
    App_PrintQuantScore(line, output_buffer[i], "\r\n");
  }

  (void)snprintf(line, sizeof(line), "NPU top1 index=%lu label=%s expected=%lu ",
                 (unsigned long)top_index,
                 g_ai_class_names[top_index],
                 (unsigned long)NPU_AI_EXPECTED_TOP1_INDEX);
  App_PrintQuantScore(line, top_score, "\r\n");

  if ((top_index == NPU_AI_EXPECTED_TOP1_INDEX) && (mismatch_count == 0U))
  {
    App_Print("NPU AI self-test OK\r\n");
  }
  else if (top_index == NPU_AI_EXPECTED_TOP1_INDEX)
  {
    (void)snprintf(line, sizeof(line), "NPU AI self-test OK top1 raw_mismatches=%lu\r\n",
                   (unsigned long)mismatch_count);
    App_Print(line);
  }
  else
  {
    (void)snprintf(line, sizeof(line), "NPU AI self-test WARN raw_mismatches=%lu\r\n",
                   (unsigned long)mismatch_count);
    App_Print(line);
  }

  for (uint32_t repeat = 1U; repeat < NPU_AI_REPEAT_COUNT; repeat++)
  {
    uint32_t repeat_top_index = 0U;
    int8_t repeat_top_score;
    LL_ATON_RT_Reset_Network(&NN_Instance_tiny_temporal_mixer_8ch_int8);

    (void)memcpy(input_buffer, g_ai_fixed_input_int8, input_size);
    App_CacheCleanAligned((uintptr_t)input_buffer, input_size);

    run_start_tick = HAL_GetTick();
    run_loop_count = 0U;
    wfe_count = 0U;
    do
    {
      run_status = LL_ATON_RT_RunEpochBlock(&NN_Instance_tiny_temporal_mixer_8ch_int8);
      run_loop_count++;
      if (run_status == LL_ATON_RT_WFE)
      {
        wfe_count++;
        __SEV();
        __WFE();
      }

      if ((HAL_GetTick() - run_start_tick) > NPU_AI_RUN_TIMEOUT_MS)
      {
        (void)snprintf(line, sizeof(line),
                       "NPU repeat[%lu] timeout status=%ld loops=%lu wfe=%lu elapsed=%lu\r\n",
                       (unsigned long)repeat,
                       (long)run_status,
                       (unsigned long)run_loop_count,
                       (unsigned long)wfe_count,
                       (unsigned long)(HAL_GetTick() - run_start_tick));
        App_Print(line);
        break;
      }
    } while (run_status != LL_ATON_RT_DONE);

    App_CacheInvalidateAligned((uintptr_t)output_buffer, output_probe_size);
    repeat_top_score = output_buffer[0];
    for (uint32_t i = 0U; i < NPU_AI_OUTPUT_CLASS_COUNT; i++)
    {
      if (output_buffer[i] > repeat_top_score)
      {
        repeat_top_score = output_buffer[i];
        repeat_top_index = i;
      }
    }

    (void)snprintf(line, sizeof(line),
                   "NPU repeat[%lu] raw=%02X %02X %02X %02X top=%lu expected=%lu loops=%lu wfe=%lu elapsed=%lu\r\n",
                   (unsigned long)repeat,
                   (unsigned int)(uint8_t)output_buffer[0],
                   (unsigned int)(uint8_t)output_buffer[1],
                   (unsigned int)(uint8_t)output_buffer[2],
                   (unsigned int)(uint8_t)output_buffer[3],
                   (unsigned long)repeat_top_index,
                   (unsigned long)NPU_AI_EXPECTED_TOP1_INDEX,
                   (unsigned long)run_loop_count,
                   (unsigned long)wfe_count,
                   (unsigned long)(HAL_GetTick() - run_start_tick));
    App_Print(line);
  }

  LL_ATON_RT_DeInit_Network(&NN_Instance_tiny_temporal_mixer_8ch_int8);
  LL_ATON_RT_RuntimeDeInit();
}

static void App_BreathLed_Task(uint32_t now_tick)
{
  const uint32_t pwm_period_ms = 10U;
  const uint32_t ramp_ms = 1600U;
  uint32_t phase = now_tick % (ramp_ms * 2U);
  uint32_t duty_ms;

  if (phase > ramp_ms)
  {
    phase = (ramp_ms * 2U) - phase;
  }

  duty_ms = (phase * pwm_period_ms) / ramp_ms;
  HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin,
                    ((now_tick % pwm_period_ms) < duty_ms) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void App_Main_Init(void)
{
  App_Print("FSBL->Appli start\r\n");
  App_RunNpuSelfTest();
  g_heartbeat_tick = HAL_GetTick();
}

void App_Main_Task(void)
{
  uint32_t now_tick = HAL_GetTick();

  App_BreathLed_Task(now_tick);

  if ((now_tick - g_heartbeat_tick) >= 1000U)
  {
    g_heartbeat_tick += 1000U;
    App_Print("FSBL->Appli heartbeat\r\n");
  }
}
