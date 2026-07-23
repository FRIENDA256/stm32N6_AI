/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_stream_mode.h
  * @brief   Single-stream output mode selector (AD+AI vs IR).
  *
  * Only one telemetry stream is transmitted at a time so the NetX IP thread is
  * never contended by both the AD7606/AI bundle stream (port 5100) and the
  * Tiny1C temperature stream (port 5101).  Switching modes atomically retunes
  * the IR capture task, AD7606 acquisition and AI processing state.
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef APP_STREAM_MODE_H
#define APP_STREAM_MODE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "tx_api.h"

#include <stdint.h>

typedef enum
{
  APP_STREAM_MODE_ADAI = 0, /* default: AD7606 waveforms + AI results */
  APP_STREAM_MODE_IR   = 1, /* Tiny1C temperature frames */
  APP_STREAM_MODE_SWITCHING = 2
} App_StreamMode_t;

/* Set the default mode (ADAI) and apply its side effects. Call once after all
   subsystems (IR capture + AI + stream threads) have started. */
UINT App_StreamMode_Init(void);

/* Atomic read of the current mode. */
App_StreamMode_t App_StreamMode_Get(void);

/* Stop new sends, wait for in-flight acquisition/processing to become idle,
   then pause the inactive pipeline and start the selected one. */
UINT App_StreamMode_Set(App_StreamMode_t mode);

/* A stream must hold a send lease for the complete MMS2 message. Switching
   first blocks new leases and then waits for all existing leases to drain. */
uint8_t App_StreamMode_TryAcquireSend(App_StreamMode_t mode);
void App_StreamMode_ReleaseSend(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_STREAM_MODE_H */
