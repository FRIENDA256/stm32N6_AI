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
#include "cacheaxi.h"
#include "gpdma.h"
#include "spi.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include "tiny8ch_1dcnn.h"
#include "tiny8ch_1dcnn_data.h"
#include "fixed_input_8ch_1024.h"
#include "class_names.h"
#include "expected_output.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */
extern DMA_HandleTypeDef handle_GPDMA1_Channel10 ;
extern DMA_HandleTypeDef handle_GPDMA1_Channel11 ;

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
AI_ALIGNED(4) static ai_u8 g_ai_activations[AI_TINY8CH_1DCNN_DATA_ACTIVATIONS_SIZE];
AI_ALIGNED(4) static float g_ai_output[AI_TINY8CH_1DCNN_OUT_1_SIZE];

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
static void SystemIsolation_Config(void);
/* USER CODE BEGIN PFP */
static void App_BreathLed_Task(uint32_t now_tick);
static void App_RunAiSelfTest(void);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static void App_Print(const char *text)
{
  (void)HAL_UART_Transmit(&huart3, (const uint8_t *)text, (uint16_t)strlen(text), HAL_MAX_DELAY);
}

static int32_t App_FloatToMicro(float value)
{
  if (value >= 0.0f)
  {
    return (int32_t)((value * 1000000.0f) + 0.5f);
  }
  return (int32_t)((value * 1000000.0f) - 0.5f);
}

static void App_PrintFixed6(const char *prefix, float value, const char *suffix)
{
  char line[96];
  int32_t scaled = App_FloatToMicro(value);
  uint32_t integer;
  uint32_t fraction;

  if (scaled < 0)
  {
    integer = (uint32_t)((-scaled) / 1000000);
    fraction = (uint32_t)((-scaled) % 1000000);
    (void)snprintf(line, sizeof(line), "%s-%lu.%06lu%s",
                   prefix, (unsigned long)integer, (unsigned long)fraction, suffix);
  }
  else
  {
    integer = (uint32_t)(scaled / 1000000);
    fraction = (uint32_t)(scaled % 1000000);
    (void)snprintf(line, sizeof(line), "%s%lu.%06lu%s",
                   prefix, (unsigned long)integer, (unsigned long)fraction, suffix);
  }
  App_Print(line);
}

static void App_PrintAiError(const char *prefix, ai_error error)
{
  char line[96];

  (void)snprintf(line, sizeof(line), "%s type=0x%08lX code=0x%08lX\r\n",
                 prefix, (unsigned long)error.type, (unsigned long)error.code);
  App_Print(line);
}

static void App_RunAiSelfTest(void)
{
  ai_handle network = AI_HANDLE_NULL;
  ai_handle activations[] = { AI_HANDLE_PTR(g_ai_activations) };
  ai_error error;
  ai_buffer *input;
  ai_buffer *output;
  ai_i32 batch;
  uint32_t top_index = 0U;
  float top_score;
  float max_abs_error = 0.0f;
  char line[128];

  App_Print("AI self-test begin\r\n");

  error = ai_tiny8ch_1dcnn_create_and_init(&network, activations, NULL);
  if (error.type != AI_ERROR_NONE)
  {
    App_PrintAiError("AI create/init FAIL", error);
    return;
  }

  input = ai_tiny8ch_1dcnn_inputs_get(network, NULL);
  output = ai_tiny8ch_1dcnn_outputs_get(network, NULL);
  if ((input == NULL) || (output == NULL))
  {
    App_Print("AI io buffer FAIL\r\n");
    (void)ai_tiny8ch_1dcnn_destroy(network);
    return;
  }

  input[0].data = AI_HANDLE_PTR((void *)g_ai_fixed_input);
  output[0].data = AI_HANDLE_PTR(g_ai_output);

  batch = ai_tiny8ch_1dcnn_run(network, input, output);
  if (batch != 1)
  {
    App_PrintAiError("AI run FAIL", ai_tiny8ch_1dcnn_get_error(network));
    (void)ai_tiny8ch_1dcnn_destroy(network);
    return;
  }

  top_score = g_ai_output[0];
  for (uint32_t i = 0U; i < AI_OUTPUT_CLASS_COUNT; i++)
  {
    float diff = g_ai_output[i] - g_ai_expected_output[i];
    if (diff < 0.0f)
    {
      diff = -diff;
    }
    if (diff > max_abs_error)
    {
      max_abs_error = diff;
    }

    if (g_ai_output[i] > top_score)
    {
      top_score = g_ai_output[i];
      top_index = i;
    }

    (void)snprintf(line, sizeof(line), "AI score[%lu] %s=",
                   (unsigned long)i, g_ai_class_names[i]);
    App_PrintFixed6(line, g_ai_output[i], "\r\n");
  }

  (void)snprintf(line, sizeof(line), "AI top1 index=%lu label=%s expected=%lu score=",
                 (unsigned long)top_index,
                 g_ai_class_names[top_index],
                 (unsigned long)AI_EXPECTED_TOP1_INDEX);
  App_PrintFixed6(line, top_score, "\r\n");
  App_PrintFixed6("AI max_abs_error=", max_abs_error, "\r\n");

  if (top_index == AI_EXPECTED_TOP1_INDEX)
  {
    App_Print("AI self-test OK\r\n");
  }
  else
  {
    App_Print("AI self-test WARN top1 mismatch\r\n");
  }

  (void)ai_tiny8ch_1dcnn_destroy(network);
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
  MX_CACHEAXI_Init();
  SystemIsolation_Config();
  /* USER CODE BEGIN 2 */
  App_Print("FSBL->Appli start\r\n");
  App_RunAiSelfTest();
  heartbeat_tick = HAL_GetTick();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    uint32_t now_tick = HAL_GetTick();

    App_BreathLed_Task(now_tick);

    if ((now_tick - heartbeat_tick) >= 1000U)
    {
      heartbeat_tick += 1000U;
      App_Print("FSBL->Appli heartbeat\r\n");
    }
  }
  /* USER CODE END 3 */
}

/**
  * @brief RIF Initialization Function
  * @param None
  * @retval None
  */
  static void SystemIsolation_Config(void)
{

/* USER CODE BEGIN RIF_Init 0 */

/* USER CODE END RIF_Init 0 */

  /* set all required IPs as secure privileged */
  __HAL_RCC_RIFSC_CLK_ENABLE();

  HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_XSPI1, RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_NPRIV);
  HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_XSPIM, RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_NPRIV);

  /* RISAF Config */
  RISAF_BaseRegionConfig_t risaf_base_config;
  __HAL_RCC_RISAF_CLK_ENABLE();

  /* set up base region configuration for XSPI2*/
  /* region 1 is secure */
  risaf_base_config.EndAddress = 0x7ffffff;
  risaf_base_config.Filtering = RISAF_FILTER_ENABLE;
  risaf_base_config.ReadWhitelist = 255;
  risaf_base_config.WriteWhitelist = 255;
  risaf_base_config.Secure = RIF_ATTRIBUTE_SEC;
  risaf_base_config.PrivWhitelist = RIF_CID_NONE;
  risaf_base_config.StartAddress = 0x0000;
  HAL_RIF_RISAF_ConfigBaseRegion(RISAF12, RISAF_REGION_1, &risaf_base_config);

  /* set up base region configuration for CPUAXI_RAM1*/
  /* region 1 is secure */
  risaf_base_config.EndAddress = 0xfffff;
  HAL_RIF_RISAF_ConfigBaseRegion(RISAF3, RISAF_REGION_1, &risaf_base_config);

  /* set up base region configuration for XSPI1*/
  /* region 1 is secure */
  risaf_base_config.EndAddress = 0x1ffffff;
  HAL_RIF_RISAF_ConfigBaseRegion(RISAF11, RISAF_REGION_1, &risaf_base_config);

  /* set up base region configuration for CPUAXI_RAM0*/
  /* region 1 is secure */
  risaf_base_config.EndAddress = 0x9bfff;
  HAL_RIF_RISAF_ConfigBaseRegion(RISAF2, RISAF_REGION_1, &risaf_base_config);

  /* set up base region configuration for FLEXRAM*/
  /* region 1 is secure */
  risaf_base_config.EndAddress = 0x63fff;
  HAL_RIF_RISAF_ConfigBaseRegion(RISAF7, RISAF_REGION_1, &risaf_base_config);

  /* RIF-Aware IPs Config */

  /* set up GPDMA configuration */
  /* set GPDMA1 channel 10 used by SPI4 */
  if (HAL_DMA_ConfigChannelAttributes(&handle_GPDMA1_Channel10,DMA_CHANNEL_SEC|DMA_CHANNEL_PRIV|DMA_CHANNEL_SRC_SEC|DMA_CHANNEL_DEST_SEC)!= HAL_OK )
  {
    Error_Handler();
  }
  /* set GPDMA1 channel 11 used by SPI4 */
  if (HAL_DMA_ConfigChannelAttributes(&handle_GPDMA1_Channel11,DMA_CHANNEL_SEC|DMA_CHANNEL_PRIV|DMA_CHANNEL_SRC_SEC|DMA_CHANNEL_DEST_SEC)!= HAL_OK )
  {
    Error_Handler();
  }

  /* set up GPIO configuration */
  HAL_GPIO_ConfigPinAttributes(GPIOB,GPIO_PIN_0,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPIOD,GPIO_PIN_8,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPIOD,GPIO_PIN_9,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPIOE,GPIO_PIN_8,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPIOE,GPIO_PIN_12,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPIOE,GPIO_PIN_13,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPIOE,GPIO_PIN_14,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPION,GPIO_PIN_0,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPION,GPIO_PIN_1,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPION,GPIO_PIN_2,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPION,GPIO_PIN_3,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPION,GPIO_PIN_4,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPION,GPIO_PIN_5,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPION,GPIO_PIN_6,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPION,GPIO_PIN_8,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPION,GPIO_PIN_9,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPION,GPIO_PIN_10,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPION,GPIO_PIN_11,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPIOO,GPIO_PIN_0,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPIOO,GPIO_PIN_1,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPIOO,GPIO_PIN_2,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPIOO,GPIO_PIN_3,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPIOO,GPIO_PIN_4,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPIOP,GPIO_PIN_0,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPIOP,GPIO_PIN_1,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPIOP,GPIO_PIN_2,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPIOP,GPIO_PIN_3,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPIOP,GPIO_PIN_4,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPIOP,GPIO_PIN_5,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPIOP,GPIO_PIN_6,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPIOP,GPIO_PIN_7,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPIOP,GPIO_PIN_8,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPIOP,GPIO_PIN_9,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPIOP,GPIO_PIN_10,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPIOP,GPIO_PIN_11,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPIOP,GPIO_PIN_12,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPIOP,GPIO_PIN_13,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPIOP,GPIO_PIN_14,GPIO_PIN_SEC|GPIO_PIN_NPRIV);
  HAL_GPIO_ConfigPinAttributes(GPIOP,GPIO_PIN_15,GPIO_PIN_SEC|GPIO_PIN_NPRIV);

/* USER CODE BEGIN RIF_Init 1 */
  /* Keep the PSRAM memory-mapped probe out of RISAF filtering while validating
     the CPU access path at 0x90000000. The filter can be tightened again after
     direct reads/writes are stable. */
  RISAF_BaseRegionConfig_t psram_risaf_probe_config = {0};
  psram_risaf_probe_config.StartAddress = 0x00000000U;
  psram_risaf_probe_config.EndAddress = 0x01FFFFFFU;
  psram_risaf_probe_config.Filtering = RISAF_FILTER_DISABLE;
  psram_risaf_probe_config.ReadWhitelist = 255U;
  psram_risaf_probe_config.WriteWhitelist = 255U;
  psram_risaf_probe_config.Secure = RIF_ATTRIBUTE_SEC;
  psram_risaf_probe_config.PrivWhitelist = RIF_CID_NONE;
  HAL_RIF_RISAF_ConfigBaseRegion(RISAF11, RISAF_REGION_1, &psram_risaf_probe_config);

/* USER CODE END RIF_Init 1 */
/* USER CODE BEGIN RIF_Init 2 */

/* USER CODE END RIF_Init 2 */

}

/* USER CODE BEGIN 4 */

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
