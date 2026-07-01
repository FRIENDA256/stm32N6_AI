/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    eth_bringup_tests.h
  * @brief   Optional low-level Ethernet test hooks.
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef ETH_BRINGUP_TESTS_H
#define ETH_BRINGUP_TESTS_H

#ifdef __cplusplus
extern "C" {
#endif

void Ethernet_BringupTests_BeforeNetX(void);
void Ethernet_BringupTests_ThreadReturned(void);

#ifdef __cplusplus
}
#endif

#endif /* ETH_BRINGUP_TESTS_H */
