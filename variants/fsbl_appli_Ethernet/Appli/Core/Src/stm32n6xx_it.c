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
#include "eth_diagnostics.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

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
static void Fault_BootPrint(const char *text)
{
  if (text == NULL)
  {
    return;
  }

  while (*text != '\0')
  {
    uint32_t wait = 100000U;
    while (((USART3->ISR & USART_ISR_TXE_TXFNF) == 0U) && (wait > 0U))
    {
      wait--;
    }

    if (wait == 0U)
    {
      return;
    }

    USART3->TDR = (uint8_t)(*text);
    text++;
  }
}

static void Fault_BootPrintHex32(const char *label, uint32_t value)
{
  static const char hex[] = "0123456789ABCDEF";
  char line[64];
  uint32_t pos = 0U;

  if (label != NULL)
  {
    while ((label[pos] != '\0') && (pos < (sizeof(line) - 13U)))
    {
      line[pos] = label[pos];
      pos++;
    }
  }

  line[pos++] = '0';
  line[pos++] = 'x';
  for (uint32_t nibble = 0U; nibble < 8U; nibble++)
  {
    uint32_t shift = 28U - (nibble * 4U);
    line[pos++] = hex[(value >> shift) & 0xFU];
  }
  line[pos++] = '\r';
  line[pos++] = '\n';
  line[pos] = '\0';

  Fault_BootPrint(line);
}

static void Fault_DumpCoreRegisters(void)
{
  Fault_BootPrintHex32("CFSR: ", SCB->CFSR);
  Fault_BootPrintHex32("HFSR: ", SCB->HFSR);
  Fault_BootPrintHex32("DFSR: ", SCB->DFSR);
  Fault_BootPrintHex32("AFSR: ", SCB->AFSR);
  Fault_BootPrintHex32("BFAR: ", SCB->BFAR);
  Fault_BootPrintHex32("MMFAR: ", SCB->MMFAR);
}

void HardFault_Handler_C(uint32_t *stack, uint32_t exc_return) __attribute__((used, noinline));
void HardFault_Handler_C(uint32_t *stack, uint32_t exc_return)
{
  Fault_BootPrint("HF\r\n");
  Fault_DumpCoreRegisters();
  Fault_BootPrintHex32("EXR: ", exc_return);

  if (stack != NULL)
  {
    Fault_BootPrintHex32("S_R0: ", stack[0]);
    Fault_BootPrintHex32("S_R1: ", stack[1]);
    Fault_BootPrintHex32("S_R2: ", stack[2]);
    Fault_BootPrintHex32("S_R3: ", stack[3]);
    Fault_BootPrintHex32("S_R12: ", stack[4]);
    Fault_BootPrintHex32("S_LR: ", stack[5]);
    Fault_BootPrintHex32("S_PC: ", stack[6]);
    Fault_BootPrintHex32("S_XPSR: ", stack[7]);
  }

  while (1)
  {
  }
}

/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/
extern ETH_HandleTypeDef heth1;
extern DMA_HandleTypeDef handle_GPDMA1_Channel11;
extern DMA_HandleTypeDef handle_GPDMA1_Channel10;
extern SPI_HandleTypeDef hspi4;
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
  Fault_BootPrint("NMI\r\n");

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
void HardFault_Handler(void) __attribute__((naked));
void HardFault_Handler(void)
{
  __asm volatile
  (
    "tst lr, #4        \n"
    "ite eq            \n"
    "mrseq r0, msp     \n"
    "mrsne r0, psp     \n"
    "mov r1, lr        \n"
    "b HardFault_Handler_C \n"
  );
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */
  Fault_BootPrint("MMF\r\n");
  Fault_DumpCoreRegisters();

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
  Fault_BootPrint("BF\r\n");
  Fault_DumpCoreRegisters();

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
  Fault_BootPrint("UF\r\n");
  Fault_DumpCoreRegisters();

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
  Fault_BootPrint("SF\r\n");
  Fault_DumpCoreRegisters();

  /* USER CODE END SecureFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_SecureFault_IRQn 0 */
    /* USER CODE END W1_SecureFault_IRQn 0 */
  }
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

/******************************************************************************/
/* STM32N6xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32n6xx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles EXTI Line8 interrupt.
  */
void EXTI8_IRQHandler(void)
{
  /* USER CODE BEGIN EXTI8_IRQn 0 */

  /* USER CODE END EXTI8_IRQn 0 */
  HAL_GPIO_EXTI_IRQHandler(AD_IRQ_Pin);
  /* USER CODE BEGIN EXTI8_IRQn 1 */

  /* USER CODE END EXTI8_IRQn 1 */
}

/**
  * @brief This function handles GPDMA1 Channel 10 global interrupt.
  */
void GPDMA1_Channel10_IRQHandler(void)
{
  /* USER CODE BEGIN GPDMA1_Channel10_IRQn 0 */

  /* USER CODE END GPDMA1_Channel10_IRQn 0 */
  HAL_DMA_IRQHandler(&handle_GPDMA1_Channel10);
  /* USER CODE BEGIN GPDMA1_Channel10_IRQn 1 */

  /* USER CODE END GPDMA1_Channel10_IRQn 1 */
}

/**
  * @brief This function handles GPDMA1 Channel 11 global interrupt.
  */
void GPDMA1_Channel11_IRQHandler(void)
{
  /* USER CODE BEGIN GPDMA1_Channel11_IRQn 0 */

  /* USER CODE END GPDMA1_Channel11_IRQn 0 */
  HAL_DMA_IRQHandler(&handle_GPDMA1_Channel11);
  /* USER CODE BEGIN GPDMA1_Channel11_IRQn 1 */

  /* USER CODE END GPDMA1_Channel11_IRQn 1 */
}

/**
  * @brief This function handles SPI4 global interrupt.
  */
void SPI4_IRQHandler(void)
{
  /* USER CODE BEGIN SPI4_IRQn 0 */

  /* USER CODE END SPI4_IRQn 0 */
  HAL_SPI_IRQHandler(&hspi4);
  /* USER CODE BEGIN SPI4_IRQn 1 */

  /* USER CODE END SPI4_IRQn 1 */
}

/**
  * @brief This function handles ETH1 global interrupt.
  */
void ETH1_IRQHandler(void)
{
  /* USER CODE BEGIN ETH1_IRQn 0 */
  Ethernet_RecordIrq();

  /* USER CODE END ETH1_IRQn 0 */
  HAL_ETH_IRQHandler(&heth1);
  /* USER CODE BEGIN ETH1_IRQn 1 */

  /* USER CODE END ETH1_IRQn 1 */
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
