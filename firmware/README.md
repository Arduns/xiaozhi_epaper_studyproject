# 小智学习机固件改动集（2026-09-01 整理版）

- 基线：github.com/78/xiaozhi-esp32 @ **bb9122a**（2026-08-21）
- 板型：`waveshare/esp32-s3-epaper-3.97`（Kconfig 名 `BOARD_TYPE_WAVESHARE_ESP32_S3_ePaper_3_97`）
- 本目录为**覆盖式改动集**：把 `main/` 下的文件按相同路径覆盖到官方源码树即可。

## 目录结构

```
main/
├── CMakeLists.txt                      # 已打补丁：本板型 REQUIRES 追加 esp_driver_sdmmc、sdmmc
├── display/
│   └── lcd_display.cc                  # 已打补丁：多行聊天区固定 2 行（LV_LABEL_LONG_DOT）
└── boards/waveshare/esp32-s3-epaper-3.97/
    ├── config.h / config.json          # 引脚与编译配置（FATFS UTF-8 长文件名等）
    ├── waveshare-s3-epaper-3.97.cc     # 板级主文件：19 个 MCP 工具 + 联网自动刷新
    ├── custom_lcd_display.h/.cc        # 墨水屏驱动 + 3 列课程表 + 每日一句 + 对话记录
    ├── schedule_manager.h/.cc          # 课程表解析（/schedule/schedule.txt）
    ├── sd_card_manager.h/.cc           # SD 卡挂载与文件读写（SDMMC 4线→1线重试）
    ├── knowledge_base.h/.cc            # 资料库检索（索引常驻 + 原文 LRU 动态加载）
    ├── study_manager.h/.cc             # 学习计划 + 错题本权重
    ├── chat_logger.h/.cc               # 对话记录 / 打卡流水 / 统计
    └── entry_counter.h/.cc             # 词条出现次数计数（低频次优先抽题）
```

## 本版（0901）相对 0831 v3 的新增

1. **联网自动刷新**：WiFi 连接成功 / 时间刚同步 / 跨午夜，自动重新加载课程表并更换每日一句
   （`SetNetworkEventCallback` 包装置位 + 后台任务 `sched_refresh` 每 5 秒检查，网络回调里不做 SD/LVGL 操作）。
2. **新增 3 个 MCP 指令**（总计 19 个）：
   - `self.schedule.today`：语音问"今天/明天/周几有什么课"；
   - `self.schedule.reload`：重新加载 SD 卡课程表并立即刷新屏幕（家长更新 schedule.txt 后用）；
   - `self.quote.refresh`：从资料库换一条每日一句并刷新屏幕（学生说"换一句"时用）。
3. **每日一句接通资料库**：开机默认名言 → 联网/首次刷新后从本地资料库经 `RandomQuote()` 抽取
   （多字段提取 + 提示词过滤），UTF-8 边界安全截断（≤84 字节，约 28 个汉字）。
4. **崩溃防护**：`UpdateScheduleAndQuote()` 入口检查课程表/每日一句标签是否已创建，
   避免空指针引发 Guru Meditation（InstrFetchProhibited）。
5. **课程表区 24px 定制字体**（0901 下午）：全局字体 30px→20px（本板分支），
   课程表表头/3 列内容/Tips 共 7 个 label 单独套用板级字体 `font_xiaozhi_study_24.c`
   （lv_font_conv 按 SD 卡内容精确子集 1825 字生成，约 310KB）。
   注意：字体是定死子集，课程表科目出现新汉字或每日一句素材池扩类时必须重新生成。

## 差异补丁与合并源码

- `xiaozhi-esp32-bb9122a-study-full-diff_20260901_1453.patch`：本目录全部改动相对
  官方 bb9122a 的完整补丁（20 文件，+38112/−115），干净树上 `git apply` 已通过验证。
- 合并好的完整源码见归档外层 `xiaozhi-esp32-bb9122a-study-merged_20260901_1453.zip`，
  解压即可直接编译，无需手动覆盖。

## 编译

```bash
cd xiaozhi-esp32            # 官方 bb9122a 源码树，路径必须纯 ASCII
# 覆盖本改动集（或打补丁/用合并源码）后：
python3 scripts/build.py waveshare/esp32-s3-epaper-3.97   # 推荐，自动合并 config.json
```

手动 `idf.py build` 必须先在 menuconfig 开启 FATFS UTF-8 长文件名
（LFN_HEAP、MAX_LFN=255、API_ENCODING_UTF_8、FS_LOCK=2），否则中文文件名退化为 8.3 短名、词条解析为 0。

详细步骤与排错见 `../docs/编译与使用说明.md`。
