# 小智学习机完整资料


## 目录索引

```
20260901_小智学习机完整资料归档/
├── README.md                    ← 本文件（归档索引）
├── again.md                     ← 新会话恢复文件，开新会话先读它
├── docs/
│   ├── 会话历史与改造记录.md      ← 全量历史 + 问题速查 + 待验证清单
│   ├── 编译与使用说明.md          ← 编译步骤、19 个 MCP 工具表、日常用法
│   └── SD卡使用说明.md            ← SD 卡目录/文件格式/语音指令速查
├── firmware/                    ← 0901 版固件改动集（覆盖官方 bb9122a 同名路径）
│   └── main/（CMakeLists.txt、display/lcd_display.cc、boards/waveshare/esp32-s3-epaper-3.97/×17）
├── sdcard/                      ← SD 卡内容（整体拷到 FAT32 卡根目录）
│   ├── knowledge/  语数英+识字+历史+科学 6 库共 1525 条
│   ├── prompt/     角色设定提示词 / 速查卡 / 20天口令表
│   ├── study/plan_config.txt
│   └── schedule/schedule.txt   （惠州市水口中心小学 五1班 2025-2026 学年）
└── skills/                      ← 三个配套技能包（导入 Kimi 即可用）
    ├── xiaozhi-study-kb.skill
    ├── xiaozhi-sd-study-data.skill
    └── xiaozhi-esp32-troubleshooting.skill
```

## 0901 版固件新增

- 联网成功 / 时间同步 / 跨天时**自动刷新课程表和每日一句**（无需任何操作）
- 新语音指令：**今天有什么课**（self.schedule.today）、**刷新课程表**（self.schedule.reload）、
  **换一句**（self.quote.refresh）
- 每日一句接通本地资料库（UTF-8 安全截断）；LVGL 崩溃防护（label 空指针检查）
- 真机验证后微调（0901 下午）：字体 30px→20px、布局按字体行高自适应、
  每日一句改 RandomQuote（语数英史科多类别+提示词过滤）、屏幕前缀 "Tips: "

<img width="800" height="480" alt="image" src="https://github.com/user-attachments/assets/19e53db9-76c0-4f51-b47d-4df2638013dd" />
