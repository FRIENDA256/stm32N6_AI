/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    eth_diagnostics.h
  * @brief   Ethernet diagnostic helpers shared by NetX and bare tests.
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef ETH_DIAGNOSTICS_H
#define ETH_DIAGNOSTICS_H

#ifdef __cplusplus
extern "C" {
#endif

void Ethernet_PrintClockDebug(void);
void Ethernet_RecordIrq(void);
void Ethernet_PrintRxSummary(void);
void Ethernet_PrintRxRuntimeDebug(void);

#ifdef __cplusplus
}
#endif

#endif /* ETH_DIAGNOSTICS_H */
