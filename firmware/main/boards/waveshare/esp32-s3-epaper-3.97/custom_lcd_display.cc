#include "custom_lcd_display.h"
#include "chat_logger.h"
#include "schedule_manager.h"
#include <esp_lcd_panel_io.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <stdio.h>
#include <vector>
#include "board.h"
#include "config.h"
#include "esp_lvgl_port.h"
#include "settings.h"

// 板级 24px 学习屏字体：只覆盖课程表 + 每日一句实际用到的 1825 个字符
//（由 lv_font_conv 按 sdcard 内容精确子集生成，font_xiaozhi_study_24.c 自动构建进本板）
LV_FONT_DECLARE(font_xiaozhi_study_24);

#define TAG "CustomEpdDisplay"
#define BYTES_PER_PIXEL (LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565))
#define BUFF_SIZE (EXAMPLE_LCD_WIDTH * EXAMPLE_LCD_HEIGHT * BYTES_PER_PIXEL)

void CustomEpdDisplay::lvgl_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* color_p) {
    assert(disp != NULL);
    CustomEpdDisplay* driver = (CustomEpdDisplay*)lv_display_get_user_data(disp);
    uint16_t* buffer = (uint16_t*)color_p;
    driver->EPD_Clear();
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            uint8_t color = (*buffer < 0x7fff) ? DRIVER_COLOR_BLACK : DRIVER_COLOR_WHITE;
            driver->EPD_DrawColorPixel(x, y, color);
            buffer++;
        }
    }
    driver->EPD_DisplayPart();
    lv_disp_flush_ready(disp);
}

CustomEpdDisplay::CustomEpdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                   int width, int height, int offset_x, int offset_y, bool mirror_x,
                                   bool mirror_y, bool swap_xy, custom_epd_spi_t _epd_spi_data)
    : LcdDisplay(panel_io, panel, width, height),
      epd_spi_data(_epd_spi_data),
      Width(width),
      Height(height) {
    ESP_LOGI(TAG, "Initialize SPI");
    spi_port_init();
    spi_gpio_init();
    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 2;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);
    lvgl_port_lock(0);
    buffer = (uint8_t*)heap_caps_malloc(epd_spi_data.buffer_len, MALLOC_CAP_SPIRAM);
    assert(buffer);
    display_ = lv_display_create(width, height);
    lv_display_set_flush_cb(display_, lvgl_flush_cb);
    lv_display_set_user_data(display_, this);
    uint8_t* buffer_1 = NULL;
    buffer_1 = (uint8_t*)heap_caps_malloc(BUFF_SIZE, MALLOC_CAP_SPIRAM);
    assert(buffer_1);
    lv_display_set_buffers(display_, buffer_1, NULL, BUFF_SIZE, LV_DISPLAY_RENDER_MODE_FULL);
    ESP_LOGI(TAG, "EPD init");
    EPD_Init();
    ESP_LOGI(TAG, "EPD Clear");
    EPD_Clear();
    EPD_Display();
    lvgl_port_unlock();
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }
    ESP_LOGI(TAG, "ui start");
    SetupUI();

    lvgl_port_lock(0);
    if (emoji_box_ != nullptr) lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    if (emoji_image_ != nullptr) lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    if (emoji_label_ != nullptr) lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);

    const int margin_x = 4;
    const int col_gap = 6;
    // 顶部状态栏是 LV_SIZE_CONTENT 自适应高度（随字体和主题内边距变化），
    // 写死 32 会导致课程表表头被"聆听中"等状态文字压住，这里取实际高度。
    int top_bar_h = 32;
    if (status_bar_ != nullptr) {
        lv_obj_update_layout(lv_screen_active());
        int h = lv_obj_get_height(status_bar_);
        if (h > top_bar_h) top_bar_h = h;
    }
    // 课程表/每日一句用板级 24px 字体（嫌 20px 太小，0901 改），其余界面仍是全局 20px。
    // 行高一律从字体实测，写死像素值会被字体改版打脸。
    int line_h = 24;  // 全局 20px 字体的行高（底栏估算用）
    if (status_label_ != nullptr) {
        const lv_font_t* f = lv_obj_get_style_text_font(status_label_, LV_PART_MAIN);
        if (f != nullptr && lv_font_get_line_height(f) > 0) {
            line_h = lv_font_get_line_height(f);
        }
    }
    const int sched_line_h = lv_font_get_line_height(&font_xiaozhi_study_24);  // 24px 字体实测 29
    const int title_h = sched_line_h + 2;      // 表头"今天·周一"一行
    const int quote_h = sched_line_h * 2 + 6;  // Tips 最多两行
    int bottom_bar_h = line_h * 2 + 8;     // 底栏对话两行（与基类公式一致）
    if (bottom_bar_ != nullptr) {
        int bh = lv_obj_get_height(bottom_bar_);  // 以实际底栏高度为准，避免 Tips 压对话
        if (bh > 0) bottom_bar_h = bh;
    }
    const int avail_width = Width - margin_x * 2;
    const int col_width = (avail_width - col_gap * 2) / 3;
    const int col_y = top_bar_h + 2;
    int content_h = Height - col_y - title_h - quote_h - bottom_bar_h - 6;
    if (content_h < sched_line_h * 3) content_h = sched_line_h * 3;  // 至少保住 3 行课程

    const char* titles[3] = {"今天", "明天", "后天"};
    int col_x[3] = {margin_x, margin_x + col_width + col_gap, margin_x + (col_width + col_gap) * 2};

    for (int i = 0; i < 3; i++) {
        schedule_title_[i] = lv_label_create(lv_screen_active());
        lv_obj_set_style_text_font(schedule_title_[i], &font_xiaozhi_study_24, 0);
        lv_obj_set_width(schedule_title_[i], col_width);
        lv_obj_set_height(schedule_title_[i], title_h);
        lv_obj_set_style_text_color(schedule_title_[i], lv_color_black(), 0);
        lv_obj_set_style_text_align(schedule_title_[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(schedule_title_[i], LV_LABEL_LONG_DOT);
        lv_obj_set_pos(schedule_title_[i], col_x[i], col_y);
        lv_label_set_text(schedule_title_[i], titles[i]);

        schedule_col_[i] = lv_label_create(lv_screen_active());
        lv_obj_set_style_text_font(schedule_col_[i], &font_xiaozhi_study_24, 0);
        lv_obj_set_width(schedule_col_[i], col_width);
        lv_obj_set_height(schedule_col_[i], content_h);
        lv_obj_set_style_text_color(schedule_col_[i], lv_color_black(), 0);
        lv_label_set_long_mode(schedule_col_[i], LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(schedule_col_[i], LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_pos(schedule_col_[i], col_x[i], col_y + title_h + 2);
    }

    quote_label_ = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(quote_label_, &font_xiaozhi_study_24, 0);
    lv_obj_set_width(quote_label_, Width - margin_x * 2);
    lv_obj_set_height(quote_label_, quote_h);
    lv_obj_set_style_text_color(quote_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(quote_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(quote_label_, LV_LABEL_LONG_WRAP);
    lv_obj_align(quote_label_, LV_ALIGN_BOTTOM_MID, 0, -bottom_bar_h - 4);

    lvgl_port_unlock();
    UpdateScheduleAndQuote();
}

CustomEpdDisplay::~CustomEpdDisplay() {}

void CustomEpdDisplay::SetChatMessage(const char* role, const char* content) {
    LcdDisplay::SetChatMessage(role, content);
    ChatLogger::GetInstance().Log(role, content);
}

void CustomEpdDisplay::UpdateScheduleAndQuote() {
    // 防御：标签尚未创建（构造中途）时不做任何 LVGL 操作，
    // 避免空指针导致的 Guru Meditation（InstrFetchProhibited）崩溃。
    if (schedule_title_[0] == nullptr || schedule_col_[0] == nullptr || quote_label_ == nullptr) {
        return;
    }
    auto& sched = ScheduleManager::GetInstance();
    int today = sched.TodayWeekday();
    if (today == 0) {
        lvgl_port_lock(0);
        for (int i = 0; i < 3; i++) {
            lv_label_set_text(schedule_col_[i], "时间未同步\n请联网同步");
        }
        lv_label_set_text(quote_label_, "Tips: 请联网后查看课程表");
        lvgl_port_unlock();
        return;
    }

    int days[3] = {0, 0, 0};
    int d = today;
    for (int i = 0; i < 3; i++) {
        while (d > 5) d = 1;
        days[i] = d;
        d++;
    }

    const char* wk_names[] = {"", "周一", "周二", "周三", "周四", "周五", "周六", "周日"};
    std::string title_texts[3];
    for (int i = 0; i < 3; i++) {
        title_texts[i] = std::string(i == 0 ? "今天·" : (i == 1 ? "明天·" : "后天·")) + wk_names[days[i]];
    }

    std::string col_texts[3];
    for (int i = 0; i < 3; i++) {
        col_texts[i] = sched.GetDayScheduleText(days[i]);
    }

    std::string quote = daily_quote_;
    if (quote.empty()) {
        quote = "书山有路勤为径，学海无涯苦作舟。";
    }

    lvgl_port_lock(0);
    for (int i = 0; i < 3; i++) {
        lv_label_set_text(schedule_title_[i], title_texts[i].c_str());
        lv_label_set_text(schedule_col_[i], col_texts[i].c_str());
    }
    lv_label_set_text(quote_label_, (std::string("Tips: ") + quote).c_str());
    lvgl_port_unlock();
}

void CustomEpdDisplay::spi_gpio_init() {
    int rst = epd_spi_data.rst;
    int cs = epd_spi_data.cs;
    int dc = epd_spi_data.dc;
    int busy = epd_spi_data.busy;
    gpio_config_t gpio_conf = {};
    gpio_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_conf.mode = GPIO_MODE_OUTPUT;
    gpio_conf.pin_bit_mask = (0x1ULL << rst) | (0x1ULL << dc) | (0x1ULL << cs);
    gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));
    gpio_conf.mode = GPIO_MODE_INPUT;
    gpio_conf.pin_bit_mask = (0x1ULL << busy);
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));
    set_rst_1();
}

void CustomEpdDisplay::spi_port_init() {
    int mosi = epd_spi_data.mosi;
    int scl = epd_spi_data.scl;
    int spi_host = epd_spi_data.spi_host;
    esp_err_t ret;
    spi_bus_config_t buscfg = {};
    buscfg.miso_io_num = -1;
    buscfg.mosi_io_num = mosi;
    buscfg.sclk_io_num = scl;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = 4096;
    spi_device_interface_config_t devcfg = {};
    devcfg.spics_io_num = -1;
    devcfg.clock_speed_hz = 20 * 1000 * 1000;
    devcfg.mode = 0;
    devcfg.queue_size = 7;
    ret = spi_bus_initialize((spi_host_device_t)spi_host, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);
    ret = spi_bus_add_device((spi_host_device_t)spi_host, &devcfg, &spi);
    ESP_ERROR_CHECK(ret);
}

void CustomEpdDisplay::read_busy() {
    int busy = epd_spi_data.busy;
    while (gpio_get_level((gpio_num_t)busy) == 1) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void CustomEpdDisplay::SPI_SendByte(uint8_t data) {
    esp_err_t ret;
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 8;
    t.tx_buffer = &data;
    ret = spi_device_polling_transmit(spi, &t);
    assert(ret == ESP_OK);
}

void CustomEpdDisplay::EPD_SendData(uint8_t data) {
    set_dc_1();
    set_cs_0();
    SPI_SendByte(data);
    set_cs_1();
}

void CustomEpdDisplay::EPD_SendCommand(uint8_t command) {
    set_dc_0();
    set_cs_0();
    SPI_SendByte(command);
    set_cs_1();
}

void CustomEpdDisplay::writeBytes(uint8_t* buffer, int len) {
    set_dc_1();
    set_cs_0();
    const int MAX_SPI_TRANSFER = 4096;
    int remaining = len;
    int offset = 0;
    while (remaining > 0) {
        int chunk_size = (remaining > MAX_SPI_TRANSFER) ? MAX_SPI_TRANSFER : remaining;
        esp_err_t ret;
        spi_transaction_t t;
        memset(&t, 0, sizeof(t));
        t.length = 8 * chunk_size;
        t.tx_buffer = buffer + offset;
        ret = spi_device_polling_transmit(spi, &t);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPI transmit failed at offset %d, chunk %d: %s", offset, chunk_size,
                     esp_err_to_name(ret));
            break;
        }
        remaining -= chunk_size;
        offset += chunk_size;
    }
    set_cs_1();
}

void CustomEpdDisplay::writeBytes(const uint8_t* buffer, int len) {
    set_dc_1();
    set_cs_0();
    const int MAX_SPI_TRANSFER = 4096;
    int remaining = len;
    int offset = 0;
    while (remaining > 0) {
        int chunk_size = (remaining > MAX_SPI_TRANSFER) ? MAX_SPI_TRANSFER : remaining;
        esp_err_t ret;
        spi_transaction_t t;
        memset(&t, 0, sizeof(t));
        t.length = 8 * chunk_size;
        t.tx_buffer = buffer + offset;
        ret = spi_device_polling_transmit(spi, &t);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPI transmit failed at offset %d, chunk %d: %s", offset, chunk_size,
                     esp_err_to_name(ret));
            break;
        }
        remaining -= chunk_size;
        offset += chunk_size;
    }
    set_cs_1();
}

void CustomEpdDisplay::EPD_SetWindows(uint16_t Xstart, uint16_t Ystart, uint16_t Xend,
                                      uint16_t Yend) {
    EPD_SendCommand(0x44);
    EPD_SendData((Xstart * 8) & 0xFF);
    EPD_SendData(((Xstart * 8) >> 8) & 0xFF);
    EPD_SendData((Xend * 8) & 0xFF);
    EPD_SendData(((Xend * 8) >> 8) & 0xFF);
    EPD_SendCommand(0x45);
    EPD_SendData(Yend & 0xFF);
    EPD_SendData((Yend >> 8) & 0xFF);
    EPD_SendData(Ystart & 0xFF);
    EPD_SendData((Ystart >> 8) & 0xFF);
}

void CustomEpdDisplay::EPD_SetCursor(uint16_t Xstart, uint16_t Ystart) {
    EPD_SendCommand(0x4E);
    EPD_SendData((Xstart * 8) & 0xFF);
    EPD_SendData(((Xstart * 8) >> 8) & 0xFF);
    EPD_SendCommand(0x4F);
    EPD_SendData(Ystart & 0xFF);
    EPD_SendData((Ystart >> 8) & 0xFF);
}

void CustomEpdDisplay::EPD_TurnOnDisplay() {
    EPD_SendCommand(0x22);
    EPD_SendData(0xF7);
    EPD_SendCommand(0x20);
    read_busy();
}

void CustomEpdDisplay::EPD_TurnOnDisplayPart() {
    EPD_SendCommand(0x22);
    EPD_SendData(0xFF);
    EPD_SendCommand(0x20);
    read_busy();
}

void CustomEpdDisplay::EPD_Init() {
    set_rst_1();
    vTaskDelay(pdMS_TO_TICKS(50));
    set_rst_0();
    vTaskDelay(pdMS_TO_TICKS(2));
    set_rst_1();
    vTaskDelay(pdMS_TO_TICKS(50));
    read_busy();
    EPD_SendCommand(0x12);
    read_busy();
    EPD_SendCommand(0x18);
    EPD_SendData(0x80);
    EPD_SendCommand(0x0C);
    EPD_SendData(0xAE);
    EPD_SendData(0xC7);
    EPD_SendData(0xC3);
    EPD_SendData(0xC0);
    EPD_SendData(0x80);
    EPD_SendCommand(0x01);
    EPD_SendData((Height - 1) % 256);
    EPD_SendData((Height - 1) / 256);
    EPD_SendData(0x02);
    EPD_SendCommand(0x3C);
    EPD_SendData(0x01);
    EPD_SendCommand(0x11);
    EPD_SendData(0x01);
    EPD_SendCommand(0x44);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData((Width - 1) % 256);
    EPD_SendData((Width - 1) / 256);
    EPD_SendCommand(0x45);
    EPD_SendData((Height - 1) % 256);
    EPD_SendData((Height - 1) / 256);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendCommand(0x4E);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendCommand(0x4F);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    read_busy();
}

void CustomEpdDisplay::EPD_Clear() {
    int buffer_len = epd_spi_data.buffer_len;
    memset(buffer, 0xff, buffer_len);
}

void CustomEpdDisplay::EPD_Display() {
    int buffer_len = epd_spi_data.buffer_len;
    EPD_SendCommand(0x24);
    assert(buffer);
    writeBytes(buffer, buffer_len);
    EPD_TurnOnDisplay();
}

void CustomEpdDisplay::EPD_DisplayPartBaseImage() {
    int buffer_len = epd_spi_data.buffer_len;
    EPD_SendCommand(0x24);
    assert(buffer);
    writeBytes(buffer, buffer_len);
    EPD_SendCommand(0x26);
    writeBytes(buffer, buffer_len);
    EPD_TurnOnDisplay();
}

void CustomEpdDisplay::EPD_Init_Partial() {
    set_rst_1();
    vTaskDelay(pdMS_TO_TICKS(50));
    set_rst_0();
    vTaskDelay(pdMS_TO_TICKS(2));
    set_rst_1();
    vTaskDelay(pdMS_TO_TICKS(50));
    read_busy();
    EPD_SendCommand(0x18);
    EPD_SendData(0x80);
    EPD_SendCommand(0x3C);
    EPD_SendData(0x80);
    EPD_SetWindows(0, Width - 1, Height - 1, 0);
    EPD_SetCursor(0, Height - 1);
    read_busy();
}

void CustomEpdDisplay::EPD_DisplayPart() {
    EPD_SendCommand(0x24);
    assert(buffer);
    writeBytes(buffer, 48000);
    EPD_TurnOnDisplayPart();
}

void CustomEpdDisplay::EPD_Sleep() {
    EPD_SendCommand(0x10);
    EPD_SendData(0x01);
    vTaskDelay(pdMS_TO_TICKS(10));
    set_rst_0();
    set_cs_0();
    set_dc_0();
}

void CustomEpdDisplay::EPD_DrawColorPixel(uint16_t x, uint16_t y, uint8_t color) {
    if (x >= Width || y >= Height) {
        ESP_LOGE("EPD", "Out of bounds pixel: (%d,%d)", x, y);
        return;
    }
    uint16_t index = y * Width / 8 + (x >> 3);
    uint8_t bit = 7 - (x & 0x07);
    if (color == DRIVER_COLOR_WHITE) {
        buffer[index] |= (0x01 << bit);
    } else {
        buffer[index] &= ~(0x01 << bit);
    }
}
