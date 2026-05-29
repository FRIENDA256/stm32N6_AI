/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32n6xx_it.c
  * @brief   Interrupt Service Routines.
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
#include "stm32n6xx_it.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define FAULT_LED_GPIO_PORT GPIOO
#define FAULT_LED_GPIO_PIN  GPIO_PIN_1
#define FAULT_LED_SHORT_ON_DELAY 20000000UL
#define FAULT_LED_LONG_ON_DELAY  120000000UL
#define FAULT_LED_GAP_DELAY      20000000UL
#define FAULT_LED_REPEAT_DELAY   160000000UL
#define FAULT_LED_NMI         5UL
#define FAULT_LED_HARDFAULT   6UL
#define FAULT_LED_MEMMANAGE   7UL
#define FAULT_LED_BUSFAULT    8UL
#define FAULT_LED_USAGEFAULT  9UL
#define SECFAULT_LED_INVEP    1UL
#define SECFAULT_LED_INVIS    2UL
#define SECFAULT_LED_INVER    3UL
#define SECFAULT_LED_AUVIOL   4UL
#define SECFAULT_LED_INVTRAN  5UL
#define SECFAULT_LED_LSPERR   6UL
#define SECFAULT_LED_LSERR    7UL
#define SECFAULT_LED_UNKNOWN  8UL
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static void FaultDelay(uint32_t loop)
{
  volatile uint32_t delay = loop;

  while (delay-- != 0U)
  {
    __NOP();
  }
}

static void FaultLedInit(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOO_CLK_ENABLE();

  GPIO_InitStruct.Pin = FAULT_LED_GPIO_PIN;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(FAULT_LED_GPIO_PORT, &GPIO_InitStruct);
  HAL_GPIO_WritePin(FAULT_LED_GPIO_PORT, FAULT_LED_GPIO_PIN, GPIO_PIN_RESET);
}

static void FaultSignal(uint32_t on_delay)
{
  HAL_GPIO_WritePin(FAULT_LED_GPIO_PORT, FAULT_LED_GPIO_PIN, GPIO_PIN_SET);
  FaultDelay(on_delay);
  HAL_GPIO_WritePin(FAULT_LED_GPIO_PORT, FAULT_LED_GPIO_PIN, GPIO_PIN_RESET);
  FaultDelay(FAULT_LED_GAP_DELAY);
}

static void FaultBlink(uint32_t pulses)
{
  FaultLedInit();

  while (1)
  {
    uint32_t index;

    for (index = 0U; index < pulses; index++)
    {
      FaultSignal(FAULT_LED_SHORT_ON_DELAY);
    }

    FaultDelay(FAULT_LED_REPEAT_DELAY);
  }
}

static void SecureFaultBlinkCode(uint32_t code)
{
  FaultLedInit();

  while (1)
  {
    uint32_t index;

    FaultSignal(FAULT_LED_LONG_ON_DELAY);

    for (index = 0U; index < code; index++)
    {
      FaultSignal(FAULT_LED_SHORT_ON_DELAY);
    }

    FaultDelay(FAULT_LED_REPEAT_DELAY);
  }
}

static void SecureFaultBlink(void)
{
  uint32_t sfsr = SAU->SFSR;

  if ((sfsr & SAU_SFSR_INVEP_Msk) != 0U)
  {
    SecureFaultBlinkCode(SECFAULT_LED_INVEP);
  }
  if ((sfsr & SAU_SFSR_INVIS_Msk) != 0U)
  {
    SecureFaultBlinkCode(SECFAULT_LED_INVIS);
  }
  if ((sfsr & SAU_SFSR_INVER_Msk) != 0U)
  {
    SecureFaultBlinkCode(SECFAULT_LED_INVER);
  }
  if ((sfsr & SAU_SFSR_AUVIOL_Msk) != 0U)
  {
    SecureFaultBlinkCode(SECFAULT_LED_AUVIOL);
  }
  if ((sfsr & SAU_SFSR_INVTRAN_Msk) != 0U)
  {
    SecureFaultBlinkCode(SECFAULT_LED_INVTRAN);
  }
  if ((sfsr & SAU_SFSR_LSPERR_Msk) != 0U)
  {
    SecureFaultBlinkCode(SECFAULT_LED_LSPERR);
  }
  if ((sfsr & SAU_SFSR_LSERR_Msk) != 0U)
  {
    SecureFaultBlinkCode(SECFAULT_LED_LSERR);
  }

  SecureFaultBlinkCode(SECFAULT_LED_UNKNOWN);
}

/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/

/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */
  FaultBlink(FAULT_LED_NMI);

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
   while (1)
  {
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  */
void HardFault_Handler(void)
{
  /* USER CODE BEGIN HardFault_IRQn 0 */
  FaultBlink(FAULT_LED_HARDFAULT);

  /* USER CODE END HardFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_HardFault_IRQn 0 */
    /* USER CODE END W1_HardFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */
  FaultBlink(FAULT_LED_MEMMANAGE);

  /* USER CODE END MemoryManagement_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_MemoryManagement_IRQn 0 */
    /* USER CODE END W1_MemoryManagement_IRQn 0 */
  }
}

/**
  * @brief This function handles Prefetch fault, memory access fault.
  */
void BusFault_Handler(void)
{
  /* USER CODE BEGIN BusFault_IRQn 0 */
  FaultBlink(FAULT_LED_BUSFAULT);

  /* USER CODE END BusFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_BusFault_IRQn 0 */
    /* USER CODE END W1_BusFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void)
{
  /* USER CODE BEGIN UsageFault_IRQn 0 */
  FaultBlink(FAULT_LED_USAGEFAULT);

  /* USER CODE END UsageFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_UsageFault_IRQn 0 */
    /* USER CODE END W1_UsageFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Secure fault.
  */
void SecureFault_Handler(void)
{
  /* USER CODE BEGIN SecureFault_IRQn 0 */
  SecureFaultBlink();

  /* USER CODE END SecureFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_SecureFault_IRQn 0 */
    /* USER CODE END W1_SecureFault_IRQn 0 */
  }
}

/**
  * @brief This function handles System service call via SWI instruction.
  */
void SVC_Handler(void)
{
  /* USER CODE BEGIN SVCall_IRQn 0 */

  /* USER CODE END SVCall_IRQn 0 */
  /* USER CODE BEGIN SVCall_IRQn 1 */

  /* USER CODE END SVCall_IRQn 1 */
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/**
  * @brief This function handles Pendable request for system service.
  */
void PendSV_Handler(void)
{
  /* USER CODE BEGIN PendSV_IRQn 0 */

  /* USER CODE END PendSV_IRQn 0 */
  /* USER CODE BEGIN PendSV_IRQn 1 */

  /* USER CODE END PendSV_IRQn 1 */
}

/**
  * @brief This function handles System tick timer.
  */
void SysTick_Handler(void)
{
  /* USER CODE BEGIN SysTick_IRQn 0 */

  /* USER CODE END SysTick_IRQn 0 */
  HAL_IncTick();
  /* USER CODE BEGIN SysTick_IRQn 1 */

  /* USER CODE END SysTick_IRQn 1 */
}

/******************************************************************************/
/* STM32N6xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32n6xx.s).                    */
/******************************************************************************/

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
