#ifndef __KNOWLEDGE_BASE_H__
#define __KNOWLEDGE_BASE_H__

#include <stdint.h>
#include <string>
#include <string_view>
#include <vector>

/**
 * 本地知识库（学习资料库）检索
 *
 * 资料库文件为 UTF-8 文本，词条格式（与上传的学习资料库一致）：
 *   【编号】YW-MR-001
 *   【类别】名人名言-学习类
 *   【关键词】学习、勤奋、练习
 *   【内容】书山有路勤为径，学海无涯苦作舟。
 *   【出处】韩愈（唐代文学家）
 *   【启示】……
 *
 * 内存策略（2026-08-28 起：索引常驻 + 原文动态加载）：
 *   - 常驻内存的只有词条索引：编号（内部 RAM 短字符串）、类别/关键词
 *     （PSRAM 元数据区）、原文位置（文件下标 + 字节偏移 + 长度）。
 *   - 词条原文不再常驻：命中时按偏移从 SD 卡读取（ReadFileRange），
 *     并带 16 槽 LRU 缓存，避免反复读卡。
 *   - 词条容量上限由 2000 提升到 KB_MAX_ENTRIES（8000）；
 *     内部 RAM 基本零占用，不影响墨水屏/SDMMC 的 DMA 缓冲区。
 *
 * 检索两阶段：
 *   1. 元数据打分（编号精确 +1000；关键词 bigram+20/单字+6；
 *      类别 bigram+10/单字+3），全部在内存完成，不读卡；
 *   2. 取前 KB_RERANK_CANDIDATES 个候选按需加载原文，叠加全文分
 *      （bigram+4/单字+1）和出现次数惩罚，过阈值后排序输出。
 *
 * 随机性与去重策略：
 *   - RandomEntry 采用"低频次优先加权随机"：先找出出现次数最少的词条集合，
 *     再从中随机抽取，显著降低短期内重复推送同一词条的概率。
 *   - Search 在同分结果中引入随机乱序 + 出现次数轻微惩罚，让不同词条都有展示机会。
 *   - 每次词条被推送后，调用 RecordAppearance 记录出现次数到 SD 卡。
 *
 * 工作流程：
 *  1. SD 卡可用时，Init() 扫描 /sdcard/knowledge/ 目录下的 .txt 文件并解析索引；
 *  2. Search() 两阶段打分排序；
 *  3. SD 卡不存在 / 未命中时返回空字符串，由上层回退网络知识库（静默，不提示学生）。
 */
class KnowledgeBase {
public:
    static KnowledgeBase& GetInstance();

    // 扫描并解析 SD 卡上的资料库索引。返回加载的词条数（0 = 不可用或为空）。
    int Init();

    // 重新加载（更新 SD 卡资料后调用）
    int Reload();

    bool IsReady() const { return ready_; }
    int GetEntryCount() const { return (int)entries_.size(); }
    int GetFileCount() const { return file_count_; }

    // 检索。命中返回拼接好的词条文本；未命中/不可用返回空字符串。
    std::string Search(const std::string& query, int max_results = 3);

    // 随机取一条词条（用于"每日一句"等）。采用低频次优先加权随机，失败返回空字符串。
    std::string RandomEntry();

    // 从适合展示为"每日一句"的类别随机抽取一条：语文名言/谚语/歇后语/古诗词、
    // 英语语法口诀/六上六下单词、历史/科学知识点。
    // 正文字段按优先级提取：【内容】→【名句】→【谚语】→【诗词】（诗词跳过标题行取诗句，
    // 口诀类标题行自动拼正文行），并过滤字段标记、标注头、提示词类字样（命中即重抽，最多 10 次）。
    // 识字库（YW-CJ/CZ）等词条没有正文字段，不会入选。无候选时返回空串。
    std::string RandomQuote();

    // 记录某词条的一次出现（增加出现次数计数并落盘）
    void RecordAppearance(const std::string& entry_id);

private:
    KnowledgeBase() = default;

    struct Entry {
        std::string id;              // 编号（短字符串，SSO 留在内部 RAM 便于比较）
        std::string_view category;   // 类别（指向 PSRAM 元数据区）
        std::string_view keywords;   // 关键词（指向 PSRAM 元数据区）
        uint16_t file_idx = 0;       // 所属资料库文件（files_ 下标）
        uint32_t offset = 0;         // 词条原文在文件中的字节偏移
        uint32_t length = 0;         // 词条原文字节数
    };

    // 词条原文缓存槽（LRU）：避免同一条词条反复读 SD 卡
    struct RawCacheSlot {
        int entry_idx = -1;      // 缓存的词条下标（-1 = 空槽）
        std::string raw;         // 词条原文
        uint32_t tick = 0;       // 最近使用序号（越大越新）
    };

    static constexpr int kRawCacheSize = 16;  // 词条原文 LRU 缓存槽数

    bool ParseFile(const std::string& path);
    // 把字段内容复制进 PSRAM 元数据区，返回指向该区的 string_view
    std::string_view MetaDup(std::string_view s);
    // 按需从 SD 卡读取词条原文（带 LRU 缓存）
    std::string LoadEntryRaw(int entry_idx);
    // 阶段 1：元数据打分（编号/关键词/类别，纯内存）
    static int ScoreMeta(const Entry& e, const std::string& query,
                         const std::vector<std::string>& bigrams);
    // 阶段 2：全文打分（原文按需加载后调用）
    static int ScoreRaw(const std::string& raw, const std::vector<std::string>& bigrams);
    static std::vector<std::string> MakeBigrams(const std::string& s);
    void Clear();

    std::vector<Entry> entries_;
    std::vector<std::string> files_;       // 资料库文件路径（Entry.file_idx 指向这里）
    std::vector<char*> meta_blocks_;       // PSRAM 元数据区（类别/关键词集中存放）
    size_t meta_used_ = 0;                 // 当前元数据块已用字节数
    RawCacheSlot cache_[kRawCacheSize];      // 词条原文 LRU 缓存
    uint32_t cache_tick_ = 0;              // 缓存使用序号
    bool ready_ = false;
    int file_count_ = 0;
};

#endif  // __KNOWLEDGE_BASE_H__
