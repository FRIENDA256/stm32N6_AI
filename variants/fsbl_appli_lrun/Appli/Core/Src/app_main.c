#include "app_main.h"

#include <stdio.h>
#include <string.h>

#include "main.h"
#include "gpio.h"
#include "usart.h"
#include "tiny8ch_1dcnn.h"
#include "tiny8ch_1dcnn_data.h"
#include "fixed_input_8ch_1024.h"
#include "class_names.h"
#include "expected_output.h"

AI_ALIGNED(4) static ai_u8 g_ai_activations[AI_TINY8CH_1DCNN_DATA_ACTIVATIONS_SIZE];
AI_ALIGNED(4) static float g_ai_output[AI_TINY8CH_1DCNN_OUT_1_SIZE];
static uint32_t g_heartbeat_tick;

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

void App_Main_Init(void)
{
  App_Print("FSBL->Appli start\r\n");
  App_RunAiSelfTest();
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
