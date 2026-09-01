#include "knowledge_base.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <string.h>

#include "config.h"
#include "entry_counter.h"
#include "sd_card_manager.h"

#define TAG "KnowledgeBase"

// 检索相关度门槛
#define SEARCH_SCORE_THRESHOLD 8
// Search 中出现次数惩罚系数（每次出现扣 2 分，轻微影响排序）
#define SEARCH_COUNT_PENALTY 2
// RandomEntry 候选池：出现次数 <= min_count + N 的词条进入候选
#define RANDOM_EXTRA_COUNT 2
// 检索阶段 2 参与全文加分的候选数（元数据得分最高的前 N 条才会读卡取原文）
#define KB_RERANK_CANDIDATES 12
// PSRAM 元数据区分块大小（类别/关键词集中存放）
#define KB_META_BLOCK_SIZE (32 * 1024)
// 词条原文 LRU 缓存槽数（须与 knowledge_base.h 中 cache_ 数组大小一致）
//（原文缓存槽数见 knowledge_base.h 的 kRawCacheSize）

static const char* kMarkId = "【编号】";
static const char* kMarkCategory = "【类别】";
static const char* kMarkKeywords = "【关键词】";

KnowledgeBase& KnowledgeBase::GetInstance() {
    static KnowledgeBase instance;
    return instance;
}

// 从 string_view 中提取 marker 之后到行尾的内容（返回 string_view，不拷贝）
static std::string_view ExtractField(std::string_view text, const char* marker, size_t from) {
    size_t pos = text.find(marker, from);
    if (pos == std::string_view::npos) {
        return "";
    }
    pos += strlen(marker);
    size_t end = text.find('\n', pos);
    std::string_view v = text.substr(pos, end == std::string_view::npos ? std::string_view::npos
                                                                        : end - pos);
    // 去掉首尾空白和 \r
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\r')) {
        v.remove_prefix(1);
    }
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) {
        v.remove_suffix(1);
    }
    return v;
}

void KnowledgeBase::Clear() {
    entries_.clear();
    files_.clear();
    for (char* blk : meta_blocks_) {
        free(blk);  // heap_caps_malloc 分配的内存同样用 free 释放
    }
    meta_blocks_.clear();
    meta_used_ = 0;
    for (int i = 0; i < kRawCacheSize; i++) {
        cache_[i].entry_idx = -1;
        cache_[i].raw.clear();
        cache_[i].raw.shrink_to_fit();
        cache_[i].tick = 0;
    }
    cache_tick_ = 0;
    ready_ = false;
    file_count_ = 0;
}

// 把字段内容复制进 PSRAM 元数据区（分块 bump 分配），返回指向该区的 string_view
std::string_view KnowledgeBase::MetaDup(std::string_view s) {
    if (s.empty()) {
        return "";
    }
    if (s.size() > KB_META_BLOCK_SIZE) {
        s = s.substr(0, KB_META_BLOCK_SIZE);  // 防御：单字段超长则截断
    }
    if (meta_blocks_.empty() || meta_used_ + s.size() > KB_META_BLOCK_SIZE) {
        char* blk =
            (char*)heap_caps_malloc(KB_META_BLOCK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (blk == nullptr) {
            blk = (char*)malloc(KB_META_BLOCK_SIZE);
        }
        if (blk == nullptr) {
            ESP_LOGE(TAG, "meta block alloc failed, field dropped");
            return "";  // 内存不足：该字段留空（词条仍可按编号命中）
        }
        meta_blocks_.push_back(blk);
        meta_used_ = 0;
    }
    char* dst = meta_blocks_.back() + meta_used_;
    memcpy(dst, s.data(), s.size());
    meta_used_ += s.size();
    return std::string_view(dst, s.size());
}

bool KnowledgeBase::ParseFile(const std::string& path) {
    auto& sd = SdCardManager::GetInstance();
    long fsize = sd.GetFileSize(path);
    if (fsize <= 0) {
        return false;
    }
    // 临时缓冲区放 PSRAM：整个文件读入后解析出索引即释放，原文不常驻内存。
    // 用 ReadFileInto 直接读进该缓冲区，避免先经内部 RAM 的 std::string 中转
    //（内部 RAM 要留给墨水屏/SDMMC 的 DMA 缓冲区）
    char* buf = (char*)heap_caps_malloc((size_t)fsize + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == nullptr) {
        ESP_LOGW(TAG, "PSRAM alloc failed (%ld bytes), fallback to internal heap", fsize);
        buf = (char*)malloc((size_t)fsize + 1);
    }
    if (buf == nullptr) {
        ESP_LOGE(TAG, "no memory for %s", path.c_str());
        return false;
    }
    size_t len = 0;
    if (!sd.ReadFileInto(path, buf, (size_t)fsize, len) || len == 0) {
        ESP_LOGW(TAG, "read failed: %s", path.c_str());
        free(buf);
        return false;
    }
    buf[len] = '\0';
    std::string_view sv(buf, len);
    uint16_t file_idx = (uint16_t)files_.size();

    int before = (int)entries_.size();
    // 以【编号】切分词条：索引记位置，类别/关键词进 PSRAM 元数据区
    size_t pos = 0;
    while ((int)entries_.size() < KB_MAX_ENTRIES) {
        size_t start = sv.find(kMarkId, pos);
        if (start == std::string_view::npos) break;
        size_t next = sv.find(kMarkId, start + strlen(kMarkId));
        std::string_view id = ExtractField(sv, kMarkId, start);
        // 编号字段可能带有补充说明（如 "YW-SC-001 ★六年级上册必背"），只取第一个词作为编号
        size_t sp = id.find_first_of(" \t");
        if (sp != std::string_view::npos) {
            id = id.substr(0, sp);
        }
        // 文件头使用说明里的字段示例行（如 "【编号】【类别】..."）会形成伪词条：
        // 其"编号"含【】标记或超长，跳过不收录
        if (id.find("【") != std::string_view::npos || id.size() > 32) {
            id = "";
        }
        if (!id.empty()) {
            Entry e;
            e.id = std::string(id);
            e.category = MetaDup(ExtractField(sv, kMarkCategory, start));
            e.keywords = MetaDup(ExtractField(sv, kMarkKeywords, start));
            e.file_idx = file_idx;
            e.offset = (uint32_t)start;
            e.length = (uint32_t)(next == std::string_view::npos ? len - start : next - start);
            entries_.push_back(std::move(e));
        }
        if (next == std::string_view::npos) break;
        pos = next;
    }
    free(buf);  // 临时文件缓冲用完即释放

    int added = (int)entries_.size() - before;
    ESP_LOGI(TAG, "%s: indexed %d entries", path.c_str(), added);
    if (added > 0) {
        files_.push_back(path);  // 只有真正解析到词条才登记文件
    }
    // 只有真正解析到词条才算资料库文件（避免把 README 等普通 txt 计入）
    return added > 0;
}

// 按需从 SD 卡读取词条原文（带 LRU 缓存）
std::string KnowledgeBase::LoadEntryRaw(int entry_idx) {
    if (entry_idx < 0 || entry_idx >= (int)entries_.size()) {
        return "";
    }
    const Entry& e = entries_[entry_idx];
    // 1. 查缓存；同时记下最旧槽位备用
    int oldest = 0;
    for (int i = 0; i < kRawCacheSize; i++) {
        if (cache_[i].entry_idx == entry_idx) {
            cache_[i].tick = ++cache_tick_;
            return cache_[i].raw;
        }
        if (cache_[i].tick < cache_[oldest].tick) {
            oldest = i;
        }
    }
    // 2. 缓存未命中：按偏移从 SD 卡读取
    std::string raw;
    if (e.file_idx < files_.size() && e.length > 0) {
        SdCardManager::GetInstance().ReadFileRange(files_[e.file_idx], e.offset, e.length, raw);
    }
    // 3. 读成功才写入缓存（失败不缓存，避免坏位置被永久缓存为空）
    if (!raw.empty()) {
        cache_[oldest].entry_idx = entry_idx;
        cache_[oldest].raw = raw;
        cache_[oldest].tick = ++cache_tick_;
    }
    return raw;
}

// 判断文件名是否为可解析的资料库 txt（跳过 macOS 生成的 ._ AppleDouble 垃圾文件）
// 注意：扩展名比较必须忽略大小写——SD 卡上的中文长文件名在 FATFS 配置未生效时
// 会退化为 8.3 短名（如 "小学语~1.TXT"），扩展名是大写的。
static bool IsKbFile(const std::string& name) {
    if (name.size() < 5) return false;
    if (name.rfind("._", 0) == 0) return false;  // "._xxx.txt"
    std::string ext = name.substr(name.size() - 4);
    for (auto& c : ext) c = tolower((unsigned char)c);
    return ext == ".txt";
}

int KnowledgeBase::Init() {
    Clear();

    auto& sd = SdCardManager::GetInstance();
    if (!sd.IsReady()) {
        ESP_LOGW(TAG, "SD not ready, local knowledge base disabled");
        return 0;
    }

    // 1. 优先扫描标准目录 /sdcard/knowledge
    auto files = sd.ListDir(SD_KNOWLEDGE_DIR);
    ESP_LOGI(TAG, "----- /knowledge 目录内容（原始文件名） -----");
    for (const auto& name : files) {
        ESP_LOGI(TAG, "  [%s]", name.c_str());
    }
    if (files.empty()) {
        ESP_LOGW(TAG, "  (空)");
    }
    for (const auto& name : files) {
        if (!IsKbFile(name)) continue;
        std::string path = std::string(SD_KNOWLEDGE_DIR) + "/" + name;
        if (ParseFile(path)) {
            file_count_++;
        }
        if ((int)entries_.size() >= KB_MAX_ENTRIES) break;
    }

    // 2. 标准目录没有收获时，自动扫描根目录及其一级子目录，寻找含【编号】词条的文件
    if (entries_.empty()) {
        ESP_LOGW(TAG, "/knowledge 目录中没有资料库，尝试自动扫描 SD 卡其他位置...");
        auto root = sd.ListDir(SD_MOUNT_POINT);
        for (const auto& name : root) {
            if ((int)entries_.size() >= KB_MAX_ENTRIES) break;
            // 跳过系统/工作目录
            if (name == "knowledge" || name == "chatlog" || name == "study" ||
                name == "System Volume Information" || name == ".Trashes") {
                continue;
            }
            std::string base = std::string(SD_MOUNT_POINT) + "/" + name;
            if (IsKbFile(name)) {
                // 根目录下的 txt
                if (ParseFile(base)) file_count_++;
            } else {
                // 当作一级子目录扫描
                auto sub = sd.ListDir(base);
                for (const auto& sname : sub) {
                    if (!IsKbFile(sname)) continue;
                    if (ParseFile(base + "/" + sname)) file_count_++;
                    if ((int)entries_.size() >= KB_MAX_ENTRIES) break;
                }
            }
        }
    }

    ready_ = !entries_.empty();
    if (!ready_) {
        ESP_LOGW(TAG, "==========================================");
        ESP_LOGW(TAG, "SD 卡已挂载，但未找到任何资料库词条！");
        ESP_LOGW(TAG, "请确认：");
        ESP_LOGW(TAG, " 1. txt 文件已拷入 SD 卡（建议放在 /knowledge 目录）");
        ESP_LOGW(TAG, " 2. 文件内词条以【编号】开头（如【编号】YW-MR-001）");
        ESP_LOGW(TAG, " 3. 文件为 UTF-8 编码");
        ESP_LOGW(TAG, "==========================================");
    }
    ESP_LOGI(TAG, "knowledge base ready: %d files, %d entries (index only), "
                  "internal heap free: %u bytes, PSRAM free: %u bytes",
             file_count_, (int)entries_.size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return (int)entries_.size();
}

int KnowledgeBase::Reload() { return Init(); }

// 生成 UTF-8 中文二元组（bigram），用于轻量级中文相关度打分
std::vector<std::string> KnowledgeBase::MakeBigrams(const std::string& s) {
    std::vector<std::string> grams;
    // 先收集 UTF-8 字符边界
    std::vector<size_t> chars;
    for (size_t i = 0; i < s.size();) {
        uint8_t c = (uint8_t)s[i];
        size_t len = 1;
        if ((c & 0x80) == 0) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > s.size()) break;
        chars.push_back(i);
        i += len;
    }
    // 单字也收录（用于关键词如"圆"），相邻两字组成 bigram
    for (size_t i = 0; i < chars.size(); i++) {
        size_t end1 = (i + 1 < chars.size()) ? chars[i + 1] : s.size();
        std::string ch = s.substr(chars[i], end1 - chars[i]);
        // 跳过 ASCII 空白和标点
        if (ch.size() == 1 && (ch[0] == ' ' || ch[0] == ',' || ch[0] == '.' || ch[0] == '?' ||
                               ch[0] == '!' || ch[0] == '\t' || ch[0] == '\n' || ch[0] == '\r')) {
            continue;
        }
        grams.push_back(ch);
        if (i + 1 < chars.size()) {
            size_t end2 = (i + 2 < chars.size()) ? chars[i + 2] : s.size();
            grams.push_back(s.substr(chars[i], end2 - chars[i]));
        }
    }
    return grams;
}

// 阶段 1：元数据打分（编号/关键词/类别——全部常驻内存，不读卡）
int KnowledgeBase::ScoreMeta(const Entry& e, const std::string& query,
                             const std::vector<std::string>& bigrams) {
    int score = 0;
    // 1. 编号精确命中（如 "YW-SC-001"）
    if (!e.id.empty() && query.find(e.id) != std::string::npos) {
        score += 1000;
    }
    // 2. bigram 命中：关键词权重最高，类别次之
    for (const auto& g : bigrams) {
        if (g.empty()) continue;
        if (e.keywords.find(g) != std::string_view::npos) {
            score += (g.size() > 3) ? 20 : 6;  // bigram 命中关键词
        }
        if (e.category.find(g) != std::string_view::npos) {
            score += (g.size() > 3) ? 10 : 3;  // bigram 命中类别
        }
    }
    return score;
}

// 阶段 2：全文打分（对按需加载出的词条原文）
int KnowledgeBase::ScoreRaw(const std::string& raw,
                            const std::vector<std::string>& bigrams) {
    int score = 0;
    for (const auto& g : bigrams) {
        if (g.empty()) continue;
        if (raw.find(g) != std::string::npos) {
            score += (g.size() > 3) ? 4 : 1;  // bigram 命中全文
        }
    }
    return score;
}

std::string KnowledgeBase::Search(const std::string& query, int max_results) {
    if (!ready_ || query.empty()) {
        return "";
    }
    auto bigrams = MakeBigrams(query);

    // 阶段 1：元数据打分（纯内存，不读卡）
    std::vector<std::pair<int, int>> meta;  // (score, index)
    for (int i = 0; i < (int)entries_.size(); i++) {
        int s = ScoreMeta(entries_[i], query, bigrams);
        if (s > 0) {
            meta.emplace_back(s, i);
        }
    }
    if (meta.empty()) {
        return "";
    }
    // 元数据分数降序，取前 N 名进入阶段 2
    std::sort(meta.begin(), meta.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    int cand_n = std::min((int)meta.size(), KB_RERANK_CANDIDATES);

    // 阶段 2：仅对候选词条按需加载原文，叠加全文分与出现次数惩罚
    auto& counter = EntryCounter::GetInstance();
    std::vector<std::pair<int, int>> scored;  // (score, index)
    for (int i = 0; i < cand_n; i++) {
        int idx = meta[i].second;
        int s = meta[i].first;
        s += ScoreRaw(LoadEntryRaw(idx), bigrams);
        // 引入出现次数惩罚：出现越多的词条扣分越多，降低重复率
        s -= counter.GetCount(entries_[idx].id) * SEARCH_COUNT_PENALTY;
        if (s >= SEARCH_SCORE_THRESHOLD) {
            scored.emplace_back(s, idx);
        }
    }
    if (scored.empty()) {
        return "";
    }
    // 按分数降序排序；分数相同则随机打乱（减少固定排序导致的重复）
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) {
                  if (a.first != b.first) return a.first > b.first;
                  // 同分：随机乱序
                  return (esp_random() & 1) != 0;
              });

    std::string out;
    int n = std::min(max_results, (int)scored.size());
    for (int i = 0; i < n; i++) {
        int idx = scored[i].second;
        out += "—— 资料库词条 [" + entries_[idx].id + "] ——\n";
        out += LoadEntryRaw(idx);  // 阶段 2 刚读过，命中缓存
        out += "\n";
        RecordAppearance(entries_[idx].id);  // 记录出现次数，供去重/加权随机
    }
    return out;
}

std::string KnowledgeBase::RandomEntry() {
    if (!ready_ || entries_.empty()) {
        return "";
    }

    auto& counter = EntryCounter::GetInstance();

    // 策略：低频次优先加权随机
    // 1. 找出最小出现次数
    int min_count = INT_MAX;
    for (const auto& e : entries_) {
        int c = counter.GetCount(e.id);
        if (c < min_count) min_count = c;
    }

    // 2. 收集出现次数 <= min_count + RANDOM_EXTRA_COUNT 的候选词条
    std::vector<int> candidates;
    candidates.reserve(entries_.size());
    for (int i = 0; i < (int)entries_.size(); i++) {
        int c = counter.GetCount(entries_[i].id);
        if (c <= min_count + RANDOM_EXTRA_COUNT) {
            candidates.push_back(i);
        }
    }

    // 3. 从候选池中随机抽取
    int idx;
    if (!candidates.empty()) {
        idx = candidates[esp_random() % candidates.size()];
    } else {
        // 兜底：纯随机（理论上不会走到这里）
        idx = (int)(esp_random() % entries_.size());
    }
    std::string raw = LoadEntryRaw(idx);
    if (raw.empty()) {
        return "";
    }
    std::string out = "—— 资料库词条 [" + entries_[idx].id + "] ——\n";
    out += raw;
    out += "\n";
    return out;
}

std::string KnowledgeBase::RandomQuote() {
    if (!ready_ || entries_.empty()) {
        return "";
    }
    // 素材池：语文名言/谚语/歇后语/古诗词 + 英语语法口诀/六上六下单词 + 历史/科学知识点。
    static const char* kQuotePrefixes[] = {
        "YW-MR-", "YW-YY-", "YW-XHY-", "YW-SC-",
        "EN-YF-", "EN-DC-S-", "EN-DC-X-",
        "LS-", "KX-",
    };
    std::vector<int> candidates;
    for (int i = 0; i < (int)entries_.size(); i++) {
        for (const char* prefix : kQuotePrefixes) {
            if (entries_[i].id.rfind(prefix, 0) == 0) {
                candidates.push_back(i);
                break;
            }
        }
    }
    if (candidates.empty()) {
        return "";
    }

    // 含这些字样的内容不上屏（字段标记、标注头、提示词类文字），命中即换一条重抽。
    static const char* kBanned[] = {"【", "】", "资料库", "提示词", "口令", "编号"};

    auto trim = [](std::string& s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    };

    // 各类词条的正文字段名不同，按优先级依次尝试：通用【内容】；古诗词先取【名句】，
    // 没有名句再取【诗词】（其首行是标题，取第二行诗句）；谚语取【谚语】。
    static const char* kContentMarks[] = {"【内容】", "【名句】", "【谚语】", "【诗词】"};

    // 多抽几次：遇到无正文字段、内容过短或命中过滤词的就换下一条。
    for (int attempt = 0; attempt < 10; attempt++) {
        std::string raw = LoadEntryRaw(candidates[esp_random() % candidates.size()]);
        if (raw.empty()) {
            continue;
        }
        std::string q;
        for (const char* mark : kContentMarks) {
            size_t pos = raw.find(mark);
            if (pos == std::string::npos) {
                continue;
            }
            pos += strlen(mark);
            // 收集字段后的前两个非空行（遇到下一个【字段】或原文结束停止）。
            // 单词表词条字段同行是空的，正文在下一行；
            // 语法口诀词条首行是"xx口诀："标题，正文在第二行。
            std::string lines[2];
            int got = 0;
            size_t start = pos;
            while (got < 2 && start < raw.size() && raw.compare(start, 3, "【") != 0) {
                size_t eol = raw.find('\n', start);
                std::string line = raw.substr(start, eol == std::string::npos ? std::string::npos : eol - start);
                trim(line);
                if (!line.empty()) {
                    lines[got++] = line;
                }
                if (eol == std::string::npos) {
                    break;
                }
                start = eol + 1;
            }
            if (got == 0) {
                continue;
            }
            if (std::string(mark) == "【诗词】") {
                // 【诗词】首行是标题（如"宿建德江（唐·孟浩然）"），取第二行诗句。
                q = (got > 1) ? lines[1] : lines[0];
            } else {
                q = lines[0];
                // 首行是标题式（很短或以全角/半角冒号结尾）且有第二行时，把正文拼上。
                bool ends_colon = (q.size() >= 1 && q.back() == ':') ||
                                  (q.size() >= 3 && q.compare(q.size() - 3, 3, "：") == 0);
                if (got > 1 && (q.size() < 12 || ends_colon)) {
                    q += lines[1];
                }
            }
            if (!q.empty()) {
                break;
            }
        }
        if (q.empty()) {
            continue;
        }
        // 过滤：命中标注/提示词类字样或内容过短的，不上屏。
        bool banned = false;
        for (const char* b : kBanned) {
            if (q.find(b) != std::string::npos) {
                banned = true;
                break;
            }
        }
        if (banned || q.size() < 6) {
            continue;
        }
        return q;
    }
    return "";
}

void KnowledgeBase::RecordAppearance(const std::string& entry_id) {
    if (entry_id.empty()) return;
    EntryCounter::GetInstance().Record(entry_id);
}
