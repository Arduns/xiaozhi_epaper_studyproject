#ifndef __CUSTOM_LCD_DISPLAY_H__
#define __CUSTOM_LCD_DISPLAY_H__

#include <driver/gpio.h>
#include <string>
#include "lcd_display.h"

typedef enum {
    DRIVER_COLOR_WHITE = 0xff,
    DRIVER_COLOR_BLACK = 0x00,
    FONT_BACKGROUND = DRIVER_COLOR_WHITE,
} COLOR_IMAGE;

typedef struct {
    uint8_t cs;
    uint8_t dc;
    uint8_t rst;
    uint8_t busy;
    uint8_t mosi;
    uint8_t scl;
    int spi_host;
    int buffer_len;
} custom_epd_spi_t;

class CustomEpdDisplay : public LcdDisplay {
public:
    CustomEpdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                     int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                     bool swap_xy, custom_epd_spi_t _epd_spi_data);
    ~CustomEpdDisplay();

    void SetChatMessage(const char* role, const char* content) override;
    void UpdateScheduleAndQuote();
    void SetDailyQuote(const std::string& quote) { daily_quote_ = quote; }
    std::string GetDailyQuote() const { return daily_quote_; }

    void EPD_Init();
    void EPD_Clear();
    void EPD_Display();
    void EPD_Sleep();
    void EPD_DisplayPartBaseImage();
    void EPD_Init_Partial();
    void EPD_DisplayPart();
    void EPD_DrawColorPixel(uint16_t x, uint16_t y, uint8_t color);

private:
    const custom_epd_spi_t epd_spi_data;
    const int Width;
    const int Height;
    spi_device_handle_t spi;
    uint8_t* buffer = NULL;

    lv_obj_t* schedule_title_[3] = {nullptr, nullptr, nullptr};
    lv_obj_t* schedule_col_[3] = {nullptr, nullptr, nullptr};
    lv_obj_t* quote_label_ = nullptr;
    std::string daily_quote_;

    static void lvgl_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* color_p);
    void spi_gpio_init();
    void spi_port_init();
    void read_busy();
    void set_cs_1() { gpio_set_level((gpio_num_t)epd_spi_data.cs, 1); }
    void set_cs_0() { gpio_set_level((gpio_num_t)epd_spi_data.cs, 0); }
    void set_dc_1() { gpio_set_level((gpio_num_t)epd_spi_data.dc, 1); }
    void set_dc_0() { gpio_set_level((gpio_num_t)epd_spi_data.dc, 0); }
    void set_rst_1() { gpio_set_level((gpio_num_t)epd_spi_data.rst, 1); }
    void set_rst_0() { gpio_set_level((gpio_num_t)epd_spi_data.rst, 0); }
    void SPI_SendByte(uint8_t data);
    void EPD_SendData(uint8_t data);
    void EPD_SendCommand(uint8_t command);
    void writeBytes(uint8_t* buffer, int len);
    void writeBytes(const uint8_t* buffer, int len);
    void EPD_SetWindows(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend);
    void EPD_SetCursor(uint16_t Xstart, uint16_t Ystart);
    void EPD_TurnOnDisplay();
    void EPD_TurnOnDisplayPart();
};

#endif
