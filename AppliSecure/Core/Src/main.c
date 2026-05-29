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

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

#define VECT_TAB_NS_OFFSET  0x00400
#define VTOR_TABLE_NS_START_ADDR (SRAM2_AXI_BASE_NS|VECT_TAB_NS_OFFSET)
#define VTOR_TABLE_NS_START_ADDR_S (SRAM2_AXI_BASE_S|VECT_TAB_NS_OFFSET)
#define SECURE_LED_GPIO_PORT GPIOO
#define SECURE_LED_GPIO_PIN  GPIO_PIN_1
#define SECURE_FAULT_SHORT_DELAY_LOOP 20000000UL
#define SECURE_FAULT_LONG_DELAY_LOOP  160000000UL
#define SECURE_ERR_NS_MSP             1UL
#define SECURE_ERR_NS_RESET_THUMB     2UL
#define SECURE_ERR_NS_RESET_RANGE     3UL
#define SECURE_ERR_UNEXPECTED         4UL
#define NS_APP_START         SRAM2_AXI_BASE_NS
#define NS_APP_END           (SRAM2_AXI_BASE_NS + 0x00100000UL)

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
static uint32_t ns_msp_secure_alias = 0U;
static uint32_t ns_reset_secure_alias = 0U;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
static void NonSecure_Init(void);
static void SystemIsolation_Config(void);
/* USER CODE BEGIN PFP */
static void Secure_CaptureNsVector(void);
static uint32_t Secure_IsValidNsMsp(uint32_t ns_msp);
static uint32_t Secure_IsValidNsReset(uint32_t ns_reset);
static void Secure_DebugLed_Init(void);
static void Secure_DebugLed_Delay(uint32_t loop);
static void Secure_DebugLed_ErrorPattern(uint32_t pulses);
static void Secure_DebugLed_Fatal(uint32_t pulses);

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
  SCB->SHCSR |= (SCB_SHCSR_BUSFAULTENA_Msk | SCB_SHCSR_SECUREFAULTENA_Msk);

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/
  HAL_Init();

  /* USER CODE BEGIN Init */
  Secure_DebugLed_Init();
  HAL_GPIO_WritePin(SECURE_LED_GPIO_PORT, SECURE_LED_GPIO_PIN, GPIO_PIN_RESET);
  Secure_CaptureNsVector();

  /* USER CODE END Init */

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  SystemIsolation_Config();
  /* USER CODE BEGIN 2 */
  HAL_GPIO_WritePin(SECURE_LED_GPIO_PORT, SECURE_LED_GPIO_PIN, GPIO_PIN_RESET);

  /* USER CODE END 2 */

  /* Secure SysTick should rather be suspended before calling non-secure  */
  /* in order to avoid wake-up from sleep mode entered by non-secure      */
  /* The Secure SysTick shall be resumed on non-secure callable functions */
  HAL_SuspendTick();

  /*************** Setup and jump to non-secure *******************************/

  NonSecure_Init();

  /* Non-secure software does not return, this code is not executed */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief  Non-secure call function
  *         This function is responsible for Non-secure initialization and switch
  *         to non-secure state
  * @retval None
  */
static void NonSecure_Init(void)
{
  funcptr_NS NonSecure_ResetHandler;
  uint32_t ns_msp;
  uint32_t ns_reset;

  SCB_NS->VTOR = VTOR_TABLE_NS_START_ADDR;

  ns_msp = (*(uint32_t *)VTOR_TABLE_NS_START_ADDR);
  ns_reset = (*((uint32_t *)((VTOR_TABLE_NS_START_ADDR) + 4U)));

  if ((Secure_IsValidNsMsp(ns_msp) == 0U) &&
      (Secure_IsValidNsMsp(ns_msp_secure_alias) != 0U))
  {
    ns_msp = ns_msp_secure_alias;
    ns_reset = ns_reset_secure_alias;
  }

  if (Secure_IsValidNsMsp(ns_msp) == 0U)
  {
    Secure_DebugLed_Fatal(SECURE_ERR_NS_MSP);
  }

  if ((ns_reset & 0x1U) == 0U)
  {
    Secure_DebugLed_Fatal(SECURE_ERR_NS_RESET_THUMB);
  }

  if (Secure_IsValidNsReset(ns_reset) == 0U)
  {
    Secure_DebugLed_Fatal(SECURE_ERR_NS_RESET_RANGE);
  }

  /* Set non-secure main stack (MSP_NS) */
  __TZ_set_MSP_NS(ns_msp);

  /* Get non-secure reset handler */
  NonSecure_ResetHandler = (funcptr_NS)ns_reset;

  /* Start non-secure state software application */
  NonSecure_ResetHandler();
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

  /*RISUP configuration*/
  HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_XSPI2 , RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_NPRIV);
  HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_XSPIM , RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_NPRIV);
  HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_USART3, RIF_ATTRIBUTE_NSEC | RIF_ATTRIBUTE_NPRIV);
  HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RCC_PERIPH_INDEX_GPIOD, RIF_ATTRIBUTE_NSEC | RIF_ATTRIBUTE_NPRIV);
  HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RCC_PERIPH_INDEX_GPIOO, RIF_ATTRIBUTE_NSEC | RIF_ATTRIBUTE_NPRIV);

  /* RIF-Aware IPs Config */
  RISAF_BaseRegionConfig_t risaf_base_config = {0};
  __HAL_RCC_RISAF_CLK_ENABLE();

  risaf_base_config.EndAddress = 0xfffff;
  risaf_base_config.Filtering = RISAF_FILTER_ENABLE;
  risaf_base_config.ReadWhitelist = 255;
  risaf_base_config.WriteWhitelist = 255;
  risaf_base_config.Secure = RIF_ATTRIBUTE_NSEC;
  risaf_base_config.PrivWhitelist = RIF_CID_NONE;
  risaf_base_config.StartAddress = 0x0000;
  HAL_RIF_RISAF_ConfigBaseRegion(RISAF3, RISAF_REGION_1, &risaf_base_config);

  risaf_base_config.EndAddress = 0x9bfff;
  risaf_base_config.Secure = RIF_ATTRIBUTE_SEC;
  HAL_RIF_RISAF_ConfigBaseRegion(RISAF2, RISAF_REGION_1, &risaf_base_config);

  risaf_base_config.EndAddress = 0x63fff;
  HAL_RIF_RISAF_ConfigBaseRegion(RISAF7, RISAF_REGION_1, &risaf_base_config);

  /* set up GPIO configuration */
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOO_CLK_ENABLE();
  HAL_GPIO_ConfigPinAttributes(GPIOD,GPIO_PIN_8,GPIO_PIN_NSEC);
  HAL_GPIO_ConfigPinAttributes(GPIOD,GPIO_PIN_9,GPIO_PIN_NSEC);
  HAL_GPIO_ConfigPinAttributes(GPIOO,GPIO_PIN_1,GPIO_PIN_NSEC);

  /* USER CODE BEGIN RIF_Init 1 */

  /* USER CODE END RIF_Init 1 */
  /* USER CODE BEGIN RIF_Init 2 */

  /* USER CODE END RIF_Init 2 */

}

/* USER CODE BEGIN 4 */
static void Secure_CaptureNsVector(void)
{
  ns_msp_secure_alias = (*(volatile uint32_t *)VTOR_TABLE_NS_START_ADDR_S);
  ns_reset_secure_alias = (*((volatile uint32_t *)((VTOR_TABLE_NS_START_ADDR_S) + 4U)));
}

static uint32_t Secure_IsValidNsMsp(uint32_t ns_msp)
{
  return ((ns_msp >= NS_APP_START) && (ns_msp <= NS_APP_END)) ? 1U : 0U;
}

static uint32_t Secure_IsValidNsReset(uint32_t ns_reset)
{
  uint32_t ns_reset_addr = ns_reset & ~0x1U;

  return ((ns_reset_addr >= VTOR_TABLE_NS_START_ADDR) &&
          (ns_reset_addr < NS_APP_END)) ? 1U : 0U;
}

static void Secure_DebugLed_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOO_CLK_ENABLE();

  GPIO_InitStruct.Pin = SECURE_LED_GPIO_PIN;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(SECURE_LED_GPIO_PORT, &GPIO_InitStruct);
}

static void Secure_DebugLed_Delay(uint32_t loop)
{
  volatile uint32_t delay = loop;

  while (delay-- != 0U)
  {
    __NOP();
  }
}

static void Secure_DebugLed_ErrorPattern(uint32_t pulses)
{
  uint32_t index;

  for (index = 0U; index < pulses; index++)
  {
    HAL_GPIO_WritePin(SECURE_LED_GPIO_PORT, SECURE_LED_GPIO_PIN, GPIO_PIN_SET);
    Secure_DebugLed_Delay(SECURE_FAULT_SHORT_DELAY_LOOP);
    HAL_GPIO_WritePin(SECURE_LED_GPIO_PORT, SECURE_LED_GPIO_PIN, GPIO_PIN_RESET);
    Secure_DebugLed_Delay(SECURE_FAULT_SHORT_DELAY_LOOP);
  }

  Secure_DebugLed_Delay(SECURE_FAULT_LONG_DELAY_LOOP);
}

static void Secure_DebugLed_Fatal(uint32_t pulses)
{
  __disable_irq();
  Secure_DebugLed_Init();
  HAL_GPIO_WritePin(SECURE_LED_GPIO_PORT, SECURE_LED_GPIO_PIN, GPIO_PIN_RESET);

  while (1)
  {
    Secure_DebugLed_ErrorPattern(pulses);
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
  Secure_DebugLed_Init();
  HAL_GPIO_WritePin(SECURE_LED_GPIO_PORT, SECURE_LED_GPIO_PIN, GPIO_PIN_RESET);
  while (1)
  {
    Secure_DebugLed_ErrorPattern(SECURE_ERR_UNEXPECTED);
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
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
