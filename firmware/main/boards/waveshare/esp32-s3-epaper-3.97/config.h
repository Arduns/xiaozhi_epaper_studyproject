#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_I2S_GPIO_MCLK  GPIO_NUM_13
#define AUDIO_I2S_GPIO_WS    GPIO_NUM_47
#define AUDIO_I2S_GPIO_BCLK  GPIO_NUM_14
#define AUDIO_I2S_GPIO_DIN   GPIO_NUM_21
#define AUDIO_I2S_GPIO_DOUT  GPIO_NUM_48

#define AUDIO_CODEC_PA_PIN       GPIO_NUM_39
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_41
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_42
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR

#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define VBAT_PWR_GPIO           GPIO_NUM_1

#define EPD_SPI_NUM        SPI3_HOST

#define EPD_DC_PIN    GPIO_NUM_9
#define EPD_CS_PIN    GPIO_NUM_10
#define EPD_SCK_PIN   GPIO_NUM_11
#define EPD_MOSI_PIN  GPIO_NUM_12
#define EPD_RST_PIN   GPIO_NUM_46
#define EPD_BUSY_PIN  GPIO_NUM_3

#define EXAMPLE_LCD_WIDTH   800
#define EXAMPLE_LCD_HEIGHT  480
 
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY  false

#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0

/* ============ SD 卡（板载 SDMMC 卡槽，本地知识库） ============ */
#define SD_MOUNT_POINT        "/sdcard"
// 资料库目录：把 *.txt 资料库文件放到 SD 卡的 /knowledge 目录下
#define SD_KNOWLEDGE_DIR      "/sdcard/knowledge"
// 对话记录目录：按天生成 chatlog/YYYYMMDD.txt
#define SD_CHATLOG_DIR        "/sdcard/chatlog"
// 学习打卡记录目录：按天生成 study/checkin_YYYYMMDD.txt
#define SD_STUDY_DIR          "/sdcard/study"
// 词条索引上限（2026-08-28 起词条原文改为按需从 SD 卡动态加载，
// 内存中只保留 编号/类别/关键词 索引，上限可由 2000 提高到 8000）
#define KB_MAX_ENTRIES        8000

#endif // _BOARD_CONFIG_H_
