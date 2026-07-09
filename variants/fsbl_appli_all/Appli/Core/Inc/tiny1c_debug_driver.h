#ifndef TINY1C_DEBUG_DRIVER_H
#define TINY1C_DEBUG_DRIVER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TINY1C_DEFAULT_I2C_ADDR_8BIT      (0x3CU << 1)
#define TINY1C_DEFAULT_SPI_CHUNK_LEN      4096U
#define TINY1C_DEFAULT_FIRST_DUMMY_LEN    512U
#define TINY1C_DEFAULT_CONTINUE_DUMMY_LEN 1U
#define TINY1C_DEFAULT_FRAME_WIDTH        256U
#define TINY1C_DEFAULT_FRAME_HEIGHT       192U
#define TINY1C_DEFAULT_FRAME_LEN          (TINY1C_DEFAULT_FRAME_WIDTH * TINY1C_DEFAULT_FRAME_HEIGHT * 2U)
#define TINY1C_DEFAULT_UART_BIN_CHUNK     128U
#define TINY1C_DEFAULT_WARMUP_MS          5000U
#define TINY1C_DEFAULT_DISCARD_FRAMES     2U
#define TINY1C_DEFAULT_FAST_TEST_FRAMES   30U

#define TINY1C_CMD_IMAGE                  0xAAU
#define TINY1C_CMD_TEMP                   0xCCU
#define TINY1C_CMD_CONTINUE               0x55U

#define TINY1C_FLASH_MAGIC                0x46433154U
#define TINY1C_FLASH_VERSION              1U
#define TINY1C_FLASH_HEADER_LEN           256U
#define TINY1C_FLASH_IMAGE_SLOT_ADDR      0x00300000U
#define TINY1C_FLASH_TEMP_SLOT_ADDR       0x00320000U
#define TINY1C_FLASH_TOTAL_LEN            (TINY1C_FLASH_HEADER_LEN + TINY1C_DEFAULT_FRAME_LEN)

typedef enum
{
  TINY1C_STATUS_OK = 0,
  TINY1C_STATUS_ERROR = 1,
  TINY1C_STATUS_UNSUPPORTED = 2
} tiny1c_status_t;

typedef struct
{
  uint16_t i2c_addr_8bit;
  uint32_t spi_chunk_len;
  uint32_t first_dummy_len;
  uint32_t continue_dummy_len;
  uint32_t frame_width;
  uint32_t frame_height;
  uint32_t frame_len;
  uint32_t uart_bin_chunk;
  uint32_t warmup_ms;
  uint32_t warmup_discard_frames;
  uint32_t vsync_timeout_ms;
  uint32_t vsync_settle_ms;
  uint32_t fast_test_frames;
  uint8_t enable_25fps_cmd;
  uint8_t use_direct_read;
  uint8_t sync_to_vsync;
  uint8_t vsync_wait_pulse_end;
  uint32_t flash_image_slot_addr;
  uint32_t flash_temp_slot_addr;
  uint32_t flash_header_len;
} tiny1c_config_t;

typedef struct
{
  void *ctx;

  void (*delay_ms)(void *ctx, uint32_t ms);
  uint32_t (*tick_ms)(void *ctx);

  int (*uart_write)(void *ctx, const uint8_t *data, uint32_t len, uint32_t timeout_ms);

  int (*i2c_is_ready)(void *ctx, uint16_t addr_8bit, uint32_t trials, uint32_t timeout_ms);
  int (*i2c_mem_read)(void *ctx, uint16_t addr_8bit, uint16_t reg, uint8_t *data, uint16_t len, uint32_t timeout_ms);
  int (*i2c_mem_write)(void *ctx, uint16_t addr_8bit, uint16_t reg, const uint8_t *data, uint16_t len, uint32_t timeout_ms);

  void (*spi_cs_write)(void *ctx, uint8_t high);
  int (*spi_txrx)(void *ctx, const uint8_t *tx, uint8_t *rx, uint32_t len, uint32_t timeout_ms);

  int (*vsync_read)(void *ctx);

  int (*flash_init)(void *ctx);
  int (*flash_erase)(void *ctx, uint32_t addr, uint32_t len);
  int (*flash_write)(void *ctx, uint32_t addr, const uint8_t *data, uint32_t len);
  int (*flash_read)(void *ctx, uint32_t addr, uint8_t *data, uint32_t len);
} tiny1c_port_t;

typedef struct
{
  uint8_t *spi_tx;
  uint8_t *spi_rx;
  uint32_t spi_buf_len;
  uint8_t *frame;
  uint32_t frame_buf_len;
  uint8_t *flash_frame;
  uint32_t flash_frame_buf_len;
} tiny1c_buffers_t;

typedef struct
{
  tiny1c_config_t config;
  tiny1c_port_t port;
  tiny1c_buffers_t buffers;

  uint8_t preview_started;
  uint8_t warmup_discard_pending;
  uint8_t last_frame_command;
  uint8_t frame_valid;
  uint8_t flash_ready;
  uint8_t last_flash_command;
  uint8_t flash_frame_valid;
} tiny1c_t;

void Tiny1C_DefaultConfig(tiny1c_config_t *config);
tiny1c_status_t Tiny1C_Init(tiny1c_t *dev, const tiny1c_config_t *config, const tiny1c_port_t *port, const tiny1c_buffers_t *buffers);

tiny1c_status_t Tiny1C_I2CProbe(tiny1c_t *dev);
tiny1c_status_t Tiny1C_PreviewStart(tiny1c_t *dev);
tiny1c_status_t Tiny1C_EnsurePreviewStarted(tiny1c_t *dev);
tiny1c_status_t Tiny1C_ReadFrame(tiny1c_t *dev, uint8_t command);
tiny1c_status_t Tiny1C_ReadFrameBaseline(tiny1c_t *dev, uint8_t command);
tiny1c_status_t Tiny1C_ReadFrameDirect(tiny1c_t *dev, uint8_t command);
tiny1c_status_t Tiny1C_FastReadTest(tiny1c_t *dev, uint8_t command, uint32_t frame_count, uint8_t direct_mode);
tiny1c_status_t Tiny1C_DumpCurrentFrameHex(tiny1c_t *dev);
tiny1c_status_t Tiny1C_DumpCurrentFrameBinary(tiny1c_t *dev);
tiny1c_status_t Tiny1C_VsyncSample(tiny1c_t *dev, uint32_t sample_ms);

tiny1c_status_t Tiny1C_FlashInitTest(tiny1c_t *dev);
tiny1c_status_t Tiny1C_FlashWriteCurrentFrame(tiny1c_t *dev);
tiny1c_status_t Tiny1C_FlashCaptureAndWriteBoth(tiny1c_t *dev);
tiny1c_status_t Tiny1C_FlashReadBackCurrent(tiny1c_t *dev);
tiny1c_status_t Tiny1C_FlashReadBackBoth(tiny1c_t *dev);
tiny1c_status_t Tiny1C_FlashDumpCurrentBinary(tiny1c_t *dev);
tiny1c_status_t Tiny1C_FlashDumpBothBinary(tiny1c_t *dev);

tiny1c_status_t Tiny1C_ProcessCommand(tiny1c_t *dev, uint8_t command);

uint32_t Tiny1C_Crc32(const uint8_t *data, uint32_t len);
const char *Tiny1C_FrameNameByCommand(uint8_t command);
uint8_t Tiny1C_CommandIsFrame(uint8_t command);

#ifdef __cplusplus
}
#endif

#endif
