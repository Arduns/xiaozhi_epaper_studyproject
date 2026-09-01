#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <atomic>
#include <ctime>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <utility>
#include "application.h"
#include "button.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "custom_lcd_display.h"
#include "lvgl.h"
#include "mcp_server.h"
#include "wifi_board.h"
#include "axp2101.h"
#include "chat_logger.h"
#include "codecs/box_audio_codec.h"
#include "i2c_device.h"
#include "knowledge_base.h"
#include "power_save_timer.h"
#include "schedule_manager.h"
#include "sd_card_manager.h"
#include "study_manager.h"

#define TAG "waveshare_epaper_3_97"

class Pmic : public Axp2101 {
public:
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        WriteReg(0x22, 0b110);
        WriteReg(0x27, 0x10);
        WriteReg(0x80, 0x01);
        WriteReg(0x90, 0x00);
        WriteReg(0x91, 0x00);
        WriteReg(0x82, (3300 - 1500) / 100);
        WriteReg(0x92, (3300 - 500) / 100);
        WriteReg(0x93, (3300 - 500) / 100);
        WriteReg(0x94, (3300 - 500) / 100);
        WriteReg(0x90, 0x07);
        WriteReg(0x64, 0x03);
        WriteReg(0x61, 0x02);
        WriteReg(0x62, 0x08);
        WriteReg(0x63, 0x01);
    }
};

class WaveshareEsp32s3ePaper3inch97 : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Pmic* pmic_ = nullptr;
    Button boot_button_;
    Button pwr_button_;
    CustomEpdDisplay* display_ = nullptr;
    PowerSaveTimer* power_save_timer_;

    // ---- 课程表/每日一句 动态刷新（2026-09-01 新增） ----
    // network_refresh_pending_：WiFi 连接成功后由网络事件回调置位，
    // 后台刷新任务发现后置位即做整屏刷新（课程表 + 每日一句）。
    std::atomic<bool> network_refresh_pending_{false};
    // last_shown_weekday_：屏幕当前显示内容对应的星期（0=尚未按日期刷新过），
    // 用于检测"跨天"与"时间刚同步"两种需要自动刷新的场景。
    int last_shown_weekday_ = 0;

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 100, 300);
        power_save_timer_->OnShutdownRequest([this]() {
            auto* epd = static_cast<CustomEpdDisplay*>(display_);
            if (epd != nullptr) {
                epd->UpdateScheduleAndQuote();
            }
            vTaskDelay(pdMS_TO_TICKS(3000));
            GetDisplay()->SetChatMessage("system", "OFF");
            vTaskDelay(pdMS_TO_TICKS(1000));
            pmic_->PowerOff();
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {};
        i2c_bus_cfg.i2c_port = (i2c_port_t)0;
        i2c_bus_cfg.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
        i2c_bus_cfg.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
        i2c_bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        i2c_bus_cfg.glitch_ignore_cnt = 7;
        i2c_bus_cfg.intr_priority = 0;
        i2c_bus_cfg.trans_queue_depth = 0;
        i2c_bus_cfg.flags.enable_internal_pullup = 1;
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        pwr_button_.OnClick([this]() {
            auto* epd = static_cast<CustomEpdDisplay*>(display_);
            if (epd != nullptr) {
                epd->UpdateScheduleAndQuote();
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
            GetDisplay()->SetChatMessage("system", "OFF");
            vTaskDelay(pdMS_TO_TICKS(1000));
            pmic_->PowerOff();
        });
    }

    void InitializeAxp2101() {
        ESP_LOGI(TAG, "Init AXP2101");
        pmic_ = new Pmic(i2c_bus_, 0x34);
    }

    void InitializeSdCard() {
        ESP_LOGI(TAG, "Init SD card");
        if (SdCardManager::GetInstance().Init()) {
            int n = KnowledgeBase::GetInstance().Init();
            ESP_LOGI(TAG, "Local knowledge base loaded: %d entries", n);
            ScheduleManager::GetInstance().Load();
        } else {
            ESP_LOGW(TAG, "SD card not available, use network knowledge base only");
        }
    }

    // 从本地资料库随机抽一条词条，取其【内容】字段作为每日一句。
    // 返回空字符串表示本地资料库不可用（此时屏幕保留默认名言，不打扰学生）。
    static std::string PickDailyQuoteFromKb() {
        if (!SdCardManager::GetInstance().IsReady()) return "";
        // RandomQuote 只从名言/谚语/歇后语/古诗词类别抽取，返回干净的【内容】首行，
        // 不含"资料库词条 [编号]"标注，也不会抽到识字库条目。
        std::string q = KnowledgeBase::GetInstance().RandomQuote();
        if (q.empty()) return "";
        // Tips 区域用板级 24px 字体，一行约 33 个汉字、两行约 66 个，限制在 84 字节
        //（约 28 个汉字）足够安全；
        // 截断必须落在 UTF-8 字符边界上，否则半个汉字会让 LVGL 字体解析异常。
        const size_t kMaxQuoteBytes = 84;
        if (q.size() > kMaxQuoteBytes) {
            size_t cut = kMaxQuoteBytes;
            while (cut > 0 && ((uint8_t)q[cut] & 0xC0) == 0x80) cut--;
            q = q.substr(0, cut);
            q += "...";
        }
        return q;
    }

    // 重新加载课程表并按需更换每日一句，然后刷新屏幕。
    // new_quote=true 时强制更换每日一句（跨天场景）。
    void RefreshScheduleAndQuote(bool new_quote) {
        ScheduleManager::GetInstance().Load();
        auto* epd = static_cast<CustomEpdDisplay*>(display_);
        if (epd == nullptr) return;
        if (new_quote || epd->GetDailyQuote().empty()) {
            std::string q = PickDailyQuoteFromKb();
            if (!q.empty()) epd->SetDailyQuote(q);
        }
        epd->UpdateScheduleAndQuote();
        last_shown_weekday_ = ScheduleManager::GetInstance().TodayWeekday();
    }

    // 后台刷新任务：每 5 秒检查一次，满足任一条件即整屏刷新：
    //   1. WiFi 刚连接成功（network_refresh_pending_ 置位）；
    //   2. 时间刚同步完成（SNTP 通常在联网后几秒内完成）；
    //   3. 跨过午夜进入新的一天（换课程表列 + 换每日一句）。
    void StartDisplayRefreshTask() {
        xTaskCreate(
            [](void* arg) {
                auto* self = static_cast<WaveshareEsp32s3ePaper3inch97*>(arg);
                bool time_was_valid = (time(nullptr) >= 1704067200);  // 2024-01-01 之后视为已同步
                while (true) {
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    bool time_valid = (time(nullptr) >= 1704067200);
                    int today = ScheduleManager::GetInstance().TodayWeekday();
                    bool need = false;
                    bool new_quote = false;
                    if (self->network_refresh_pending_.exchange(false)) need = true;
                    if (time_valid && !time_was_valid) need = true;
                    if (today != 0 && today != self->last_shown_weekday_) {
                        need = true;
                        new_quote = true;
                    }
                    time_was_valid = time_valid;
                    if (need) self->RefreshScheduleAndQuote(new_quote);
                }
            },
            "sched_refresh", 4096, this, 2, nullptr);
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.disp.network", "重新配网", PropertyList(),
                           [this](const PropertyList&) -> ReturnValue {
                               EnterWifiConfigMode();
                               return true;
                           });

        mcp_server.AddTool(
            "self.kb.search",
            "检索设备 SD 卡上的本地学习资料库（语文/数学/英语/识字/历史/科学，覆盖小学到初中）。"
            "只要用户的问题与学习有关（名言、古诗、典故、识字、公式、应用题、单词、语法、"
            "对话练习、朝代与历史事件、历史人物、生物、物理、化学、地球与宇宙等），"
            "必须先调用本工具检索，优先使用本地资料库内容回答，并在回答中注明词条编号。"
            "如果返回 NO_LOCAL_HIT（本地未命中或SD卡不可用），不要向学生提及任何检索过程，"
            "直接用你自己的知识自然回答。",
            PropertyList({Property("query", kPropertyTypeString),
                          Property("max_results", kPropertyTypeInteger, 3, 1, 5)}),
            [](const PropertyList& properties) -> ReturnValue {
                auto& sd = SdCardManager::GetInstance();
                if (!sd.IsReady()) {
                    return std::string(
                        "NO_LOCAL_HIT（SD卡不可用）。此为内部信号，不要向学生提及；"
                        "请直接用你的知识回答。");
                }
                auto& kb = KnowledgeBase::GetInstance();
                if (!kb.IsReady()) {
                    return std::string(
                        "NO_LOCAL_HIT（SD卡中未找到资料库文件）。此为内部信号，不要向学生提及；"
                        "请直接用你的知识回答。");
                }
                std::string query = properties["query"].value<std::string>();
                int max_results = properties["max_results"].value<int>();
                std::string result = kb.Search(query, max_results);
                if (result.empty()) {
                    return std::string(
                        "NO_LOCAL_HIT（本地资料库无相关内容）。此为内部信号，不要向学生提及；"
                        "请直接用你的知识回答。");
                }
                return result;
            });

        mcp_server.AddTool("self.kb.status",
                           "查询本地学习资料库状态：SD卡是否可用、已加载的资料库文件数和词条数。",
                           PropertyList(), [](const PropertyList&) -> ReturnValue {
                               auto& sd = SdCardManager::GetInstance();
                               auto& kb = KnowledgeBase::GetInstance();
                               char buf[256];
                               snprintf(buf, sizeof(buf), "%s；资料库文件 %d 个，词条 %d 条",
                                        sd.GetStatusText().c_str(), kb.GetFileCount(),
                                        kb.GetEntryCount());
                               return std::string(buf);
                           });

        mcp_server.AddTool("self.kb.reload",
                           "重新扫描并加载 SD 卡上的学习资料库文件。当用户说更新了资料库、"
                           "换了SD卡时调用。",
                           PropertyList(), [](const PropertyList&) -> ReturnValue {
                               int n = KnowledgeBase::GetInstance().Reload();
                               if (n == 0) {
                                   return std::string("重新加载失败或资料库为空：") +
                                          SdCardManager::GetInstance().GetStatusText();
                               }
                               return std::string("资料库已重新加载，共 ") +
                                      std::to_string(n) + " 条词条";
                           });

        mcp_server.AddTool("self.kb.daily",
                           "从本地学习资料库随机抽取一条词条，用于每天首次对话时推送每日一句。"
                           "本地不可用时返回提示，再改用网络知识。",
                           PropertyList(), [](const PropertyList&) -> ReturnValue {
                               std::string entry = KnowledgeBase::GetInstance().RandomEntry();
                               if (entry.empty()) {
                                   return std::string(
                                       "NO_LOCAL_HIT（本地资料库不可用）。此为内部信号，不要向学生"
                                       "提及；请直接给学生讲一条适合小学生的名言或知识点。");
                               }
                               return entry;
                           });


        mcp_server.AddTool(
            "self.study.checkin",
            "学习打卡：每完成一项学习任务调用一次（不是一天只打一次）。安排任务前先调用 self.study.plan 获取当天各科条数，按条数逐项出题；学生每完成一项（如背完一首古诗、做完一组口算、听写完一组单词）调用一次，"
            "把学习记录保存到 SD 卡。subject 为 语文/数学/英语/历史/科学，item 为具体学习内容，"
            "result 为 完成/正确/错误 等。result 为错误时会自动记入错题本。",
            PropertyList({Property("subject", kPropertyTypeString),
                          Property("item", kPropertyTypeString),
                          Property("result", kPropertyTypeString, "完成")}),
            [](const PropertyList& properties) -> ReturnValue {
                if (!SdCardManager::GetInstance().IsReady()) {
                    return std::string("SD卡不可用，本次打卡未能保存（学习继续，不影响使用）");
                }
                std::string item = properties["item"].value<std::string>();
                std::string result = properties["result"].value<std::string>();
                ChatLogger::GetInstance().LogCheckin(
                    properties["subject"].value<std::string>(), item, result);
                if (result.find("错") != std::string::npos) {
                    StudyManager::GetInstance().AddMistake(item, item);
                    return std::string("打卡成功，已记录到SD卡（该错题已加入错题本）");
                }
                return std::string("打卡成功，已记录到SD卡");
            });

        mcp_server.AddTool(
            "self.study.mistake",
            "记录错题/易错点：当学生答错题目时调用，写入错题本并按权重机制累计"
            "（新错题权重5，重复答错权重+2，上限10）。"
            "entry_id 为资料库词条编号（没有则填空字符串），description 为错误描述。",
            PropertyList({Property("entry_id", kPropertyTypeString, ""),
                          Property("description", kPropertyTypeString)}),
            [](const PropertyList& properties) -> ReturnValue {
                if (!SdCardManager::GetInstance().IsReady()) {
                    return std::string("SD卡不可用，错题未能保存");
                }
                std::string entry_id = properties["entry_id"].value<std::string>();
                std::string desc = properties["description"].value<std::string>();
                ChatLogger::GetInstance().LogMistake(entry_id, desc);
                StudyManager::GetInstance().AddMistake(entry_id, desc);
                return std::string("错题已记录到错题本");
            });

        mcp_server.AddTool(
            "self.study.correct",
            "错题答对：当学生答对了错题本里的题目时调用，降低该错题权重；"
            "连续答对3次后该题自动移出错题本。entry_id 为错题本中的编号或内容。",
            PropertyList({Property("entry_id", kPropertyTypeString)}),
            [](const PropertyList& properties) -> ReturnValue {
                std::string entry_id = properties["entry_id"].value<std::string>();
                int r = StudyManager::GetInstance().MarkCorrect(entry_id);
                if (r == 1) {
                    return std::string("很好！该题权重已降低，再巩固几次就能移出错题本");
                }
                if (r == 0) {
                    return std::string("太棒了！该题已掌握，移出错题本");
                }
                return std::string("错题本中没有找到这条记录（可能从未答错或已移除）");
            });

        mcp_server.AddTool(
            "self.study.mistakebook",
            "查看错题本：返回按权重（复习优先级）排序的错题列表。"
            "安排复习、出练习题前应先查看错题本，优先巩固高权重错题。",
            PropertyList({Property("max_items", kPropertyTypeInteger, 20, 1, 50)}),
            [](const PropertyList& properties) -> ReturnValue {
                return StudyManager::GetInstance().GetMistakeBookText(
                    properties["max_items"].value<int>());
            });

        mcp_server.AddTool(
            "self.study.plan",
            "查看当前学习计划：返回每天语文/数学/英语/历史/科学各学几条。每天安排学习任务前必须先调用本工具获取条数，然后按该条数逐项出题；学生每完成一项调用一次 self.study.checkin 打卡。家长/学生要改计划条数时调用 self.study.set_plan（可单科或全部）。"
            "制定当天学习任务前先调用本工具获取计划条数。",
            PropertyList(), [](const PropertyList&) -> ReturnValue {
                return StudyManager::GetInstance().GetPlanText();
            });

        mcp_server.AddTool(
            "self.study.set_plan",
            "修改学习计划：设置某科（或全部科目）每天学习内容条数（1-9）。"
            "subject 为 语文/数学/英语/历史/科学/全部。例如用户说把数学改成每天5条时调用。"
            "修改保存到 SD 卡 /study/plan_config.txt，立即生效。",
            PropertyList({Property("subject", kPropertyTypeString),
                          Property("count", kPropertyTypeInteger, 3, 1, 9)}),
            [](const PropertyList& properties) -> ReturnValue {
                std::string subject = properties["subject"].value<std::string>();
                int count = properties["count"].value<int>();
                if (!SdCardManager::GetInstance().IsReady()) {
                    return std::string("SD卡不可用，学习计划修改失败");
                }
                if (!StudyManager::GetInstance().SetDailyCount(subject, count)) {
                    return std::string("修改失败：科目只能是 语文/数学/英语/历史/科学/全部，条数 1-9");
                }
                return std::string("学习计划已更新：") + subject + " 每天 " +
                       std::to_string(count) + " 条";
            });

        mcp_server.AddTool(
            "self.study.review",
            "回顾学习记录：返回最近几天打卡记录的明细内容（时间、科目、内容、结果），"
            "用于对话结束后回顾学习情况、安排后续学习。days 默认 3 天。",
            PropertyList({Property("days", kPropertyTypeInteger, 3, 1, 14)}),
            [](const PropertyList& properties) -> ReturnValue {
                std::string detail = StudyManager::GetInstance().GetCheckinDetail(
                    properties["days"].value<int>());
                if (detail.empty()) {
                    return std::string("最近没有打卡记录（或SD卡不可用）");
                }
                return detail;
            });

        mcp_server.AddTool("self.study.summary",
                           "查询最近几天的学习打卡统计，用于安排复习计划和调整学习内容。",
                           PropertyList({Property("days", kPropertyTypeInteger, 7, 1, 30)}),
                           [](const PropertyList& properties) -> ReturnValue {
                               std::string s = ChatLogger::GetInstance().GetCheckinSummary(
                                   properties["days"].value<int>());
                               if (s.empty()) {
                                   return std::string("暂无打卡记录或SD卡不可用");
                               }
                               return s;
                           });

        mcp_server.AddTool(
            "self.sd.ls",
            "查看 SD 卡上指定目录的文件列表（含文件大小）。path 如 /sdcard、/sdcard/knowledge。"
            "当用户问 SD 卡里有什么文件、资料库文件是否拷进去了时使用。",
            PropertyList({Property("path", kPropertyTypeString, "/sdcard")}),
            [](const PropertyList& properties) -> ReturnValue {
                auto& sd = SdCardManager::GetInstance();
                if (!sd.IsReady()) {
                    return std::string("SD卡不可用：") + sd.GetStatusText();
                }
                std::string path = properties["path"].value<std::string>();
                if (path.empty() || path[0] != '/') {
                    path = std::string(SD_MOUNT_POINT) + "/" + path;
                }
                auto entries = sd.ListDir(path);
                if (entries.empty()) {
                    return std::string("目录为空或不存在：") + path;
                }
                std::string out = "目录 " + path + "：\n";
                for (const auto& name : entries) {
                    long size = sd.GetFileSize(path + "/" + name);
                    if (size >= 0) {
                        out += "  " + name + " (" + std::to_string(size) + " 字节)\n";
                    } else {
                        out += "  " + name + "/\n";
                    }
                }
                return out;
            });

        mcp_server.AddTool(
            "self.sd.remount",
            "重新挂载 SD 卡并重新加载本地学习资料库。当用户说重新插拔了SD卡、"
            "SD卡读不出来、识别不到资料库时使用。",
            PropertyList(), [](const PropertyList&) -> ReturnValue {
                auto& sd = SdCardManager::GetInstance();
                sd.Unmount();
                if (!sd.Init()) {
                    return std::string("SD卡重新挂载失败（未插卡或卡片异常）：") +
                           sd.GetStatusText();
                }
                int n = KnowledgeBase::GetInstance().Reload();
                ScheduleManager::GetInstance().Load();
                return std::string("SD卡已重新挂载，资料库加载 ") + std::to_string(n) +
                       " 条词条";
            });

        mcp_server.AddTool(
            "self.sd.read",
            "读取 SD 卡上指定文本文件的开头内容，用于检查资料库文件内容和编码。"
            "path 为完整路径，如 /sdcard/knowledge/小学语文学习资料库.txt。",
            PropertyList({Property("path", kPropertyTypeString),
                          Property("max_bytes", kPropertyTypeInteger, 2000, 100, 8000)}),
            [](const PropertyList& properties) -> ReturnValue {
                auto& sd = SdCardManager::GetInstance();
                if (!sd.IsReady()) {
                    return std::string("SD卡不可用：") + sd.GetStatusText();
                }
                std::string path = properties["path"].value<std::string>();
                if (path.empty() || path[0] != '/') {
                    path = std::string(SD_MOUNT_POINT) + "/" + path;
                }
                std::string content;
                if (!sd.ReadFile(path, content, (size_t)properties["max_bytes"].value<int>())) {
                    return std::string("读取失败（文件不存在或异常）：") + path;
                }
                return std::string("文件 ") + path + " 开头内容：\n" + content;
            });

        mcp_server.AddTool(
            "self.schedule.today",
            "查看课程表：学生问今天、明天、后天或周几有什么课时调用。"
            "day 可填 今天/明天/后天/周一/周二/周三/周四/周五/周六/周日，默认今天。"
            "课程表来自 SD 卡 /schedule/schedule.txt，按节次顺序告诉学生即可。",
            PropertyList({Property("day", kPropertyTypeString, "今天")}),
            [](const PropertyList& properties) -> ReturnValue {
                auto& sched = ScheduleManager::GetInstance();
                int today = sched.TodayWeekday();
                if (today == 0) {
                    return std::string("时间未同步，暂时查不了课程表，联网后自动恢复。");
                }
                std::string day = properties["day"].value<std::string>();
                int wd = today;
                if (day.find("明天") != std::string::npos) {
                    wd = today + 1;
                } else if (day.find("后天") != std::string::npos) {
                    wd = today + 2;
                } else {
                    const char* names[] = {"周一", "周二", "周三", "周四", "周五", "周六", "周日"};
                    for (int i = 0; i < 7; i++) {
                        if (day.find(names[i]) != std::string::npos) {
                            wd = i + 1;
                            break;
                        }
                    }
                }
                while (wd > 7) wd -= 7;
                if (wd > 5) {
                    return ScheduleManager::WeekdayName(wd) + "是周末，没有课，可以自主复习。";
                }
                auto items = sched.GetDaySchedule(wd);
                if (items.empty()) {
                    return ScheduleManager::WeekdayName(wd) +
                           "没有查到课程（课程表文件未配置；家长更新 SD 卡 /schedule/schedule.txt 后，"
                           "可调用 self.schedule.reload 重新加载）";
                }
                std::string out = ScheduleManager::WeekdayName(wd) + "的课程：";
                for (const auto& it : items) {
                    out += (it.period == 0) ? "早读" : ("第" + std::to_string(it.period) + "节");
                    out += it.subject;
                    if (!it.note.empty()) out += "(" + it.note + ")";
                    out += "；";
                }
                return out;
            });

        mcp_server.AddTool(
            "self.schedule.reload",
            "重新从 SD 卡加载课程表文件并立即刷新屏幕上的课程表显示。"
            "家长用电脑更新 /schedule/schedule.txt 后，或学生说刷新课程表时调用。",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                RefreshScheduleAndQuote(false);
                auto& sched = ScheduleManager::GetInstance();
                if (!sched.IsLoaded()) {
                    return std::string("课程表加载失败：") + sched.GetErrorReason() +
                           "（请检查 SD 卡 /schedule/schedule.txt）";
                }
                return std::string("课程表已重新加载并刷新屏幕。\n") + sched.GetTodayScheduleText();
            });

        mcp_server.AddTool(
            "self.quote.refresh",
            "更换屏幕上的每日一句：从本地学习资料库重新随机抽取一条并立即刷新屏幕显示。"
            "学生说换一句、再来一句时调用。",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                std::string q = PickDailyQuoteFromKb();
                if (q.empty()) {
                    return std::string("本地资料库不可用，每日一句未更换");
                }
                auto* epd = static_cast<CustomEpdDisplay*>(display_);
                if (epd != nullptr) {
                    epd->SetDailyQuote(q);
                    epd->UpdateScheduleAndQuote();
                }
                return std::string("每日一句已更换：") + q;
            });
    }

    void InitializeEpdDisplay() {
        custom_epd_spi_t epd_spi_data = {};
        epd_spi_data.cs = EPD_CS_PIN;
        epd_spi_data.dc = EPD_DC_PIN;
        epd_spi_data.rst = EPD_RST_PIN;
        epd_spi_data.busy = EPD_BUSY_PIN;
        epd_spi_data.mosi = EPD_MOSI_PIN;
        epd_spi_data.scl = EPD_SCK_PIN;
        epd_spi_data.spi_host = EPD_SPI_NUM;
        epd_spi_data.buffer_len = 48000;
        display_ = new CustomEpdDisplay(NULL, NULL, EXAMPLE_LCD_WIDTH, EXAMPLE_LCD_HEIGHT,
                                        DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                        DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY, epd_spi_data);
    }

public:
    WaveshareEsp32s3ePaper3inch97()
        : boot_button_(BOOT_BUTTON_GPIO), pwr_button_(VBAT_PWR_GPIO, 1) {
        InitializePowerSaveTimer();
        InitializeI2c();
        InitializeAxp2101();
        InitializeButtons();
        InitializeSdCard();
        InitializeTools();
        InitializeEpdDisplay();
        StartDisplayRefreshTask();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = pmic_->IsCharging();
        discharging = pmic_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        level = pmic_->GetBatteryLevel();
        return true;
    }

    virtual Display* GetDisplay() override { return display_; }

    // 包装网络事件回调：WiFi 连接成功时置位刷新标记，
    // 由后台刷新任务在安全上下文里重新加载课程表并更换每日一句。
    // （回调本身运行在 WiFi 事件任务里，不能做 SD 卡读写和 LVGL 操作。）
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override {
        WifiBoard::SetNetworkEventCallback(
            [this, user_cb = std::move(callback)](NetworkEvent event, const std::string& data) mutable {
                if (event == NetworkEvent::Connected) {
                    network_refresh_pending_.store(true);
                }
                if (user_cb) user_cb(event, data);
            });
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(WaveshareEsp32s3ePaper3inch97);
