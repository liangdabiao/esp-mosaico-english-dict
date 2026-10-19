// tests/wd_content_fixture.h —— 由 tools/wordbook/gen_content.py 生成，不要手改。
//
// 跨语言固定向量：这里写的是**服务端（生成器）认为**每条记录应该被解析成什么。
// 设备侧的 wd_content 必须逐字段对上。字段顺序、拼接方式、偏移基准任何一处
// 跑偏，宿主测试当场红 —— 这三样在编译期都看不出来。
#pragma once

#include <stdint.h>

#define WD_FIX_BOOK_COUNT 6
#define WD_FIX_WORD_COUNT 19513
#define WD_FIX_VERSION 2226794780u

typedef struct {
    const char *book;
    const char *word;
    const char *ipa;
    const char *trans;
    const char *sent_en;
    const char *sent_cn;
} wd_fixture_case_t;

static const wd_fixture_case_t kWdFixtureCases[] = {
    { "ChuZhong_3", "cancel", "ˈkænsl", "v 取消，废除；抵消；删去，划掉", "Our flight was cancelled.", "我们的航班取消了。" },
    { "ChuZhong_3", "close", "kloz", "adv 接近地；接近地；adj 密切的，亲密的；近的，接近的；严密的，仔细的；不通风的，闷的；密切的，亲密的；近的，接近的…", "If you need to buy bread or milk, the closest shop is about a mile away.", "如果你需要买面包或牛奶，最近的商店大概有一英里远。" },
    { "ChuZhong_3", "transportation", "", "n 运输，运输系统；运输工具", "The city needs to improve its public transportation (= buses, trains etc ) .", "这座城市需要改善公共交通。" },
    { "GaoZhong_2", "a", "ə", "n 字母A；第一流的；学业成绩达最高标准的评价符号；abbr [物]安（ampere）", "Would you like a sandwich?", "来块三明治好吗？" },
    { "GaoZhong_2", "blue", "blu", "n 蓝色；[复数]（美国海、陆、空三军穿的）蓝色制服；蓝颜料；[the blue(s)][用作单数或复数]布鲁斯（歌曲）（一种…", "She nearly always dresses in blue.", "她几乎总是穿蓝色的衣服。" },
    { "GaoZhong_2", "straightforward", "", "adj 简单的；坦率的；明确的；径直的；adv 直截了当地；坦率地", "This area of law is far from straightforward (= complicated ) .", "法律的这个领域很复杂。" },
    { "CET4_2", "a", "ə", "art 一(个)；每一(个)", "We have a problem.", "我们遇到了一个麻烦。" },
    { "CET4_2", "spite", "spaɪt", "n n.；n 恶意,怨恨,不顾；n vt.；n 刁难,欺侮", "We went out in spite of the rain.", "尽管下着雨，我们还是出去了。" },
    { "CET4_2", "administration", "əd", "n 管理；管理部门", "We’re looking for someone with experience in administration.", "我们正在寻找一个有管理经验的人。" },
    { "CET6_3", "cancel", "ˈkænsl", "vt 取消，撤销，删去", "Our flight was cancelled.", "我们的航班取消了。" },
    { "CET6_3", "overflow", "", "vi 满得外溢，外流，泛滥；充满，洋溢；vt 淹没，从…中溢出，多得无法容纳；n 容纳不下的物；溢出，满出；溢流口，溢流管", "The drains flooded and water overflowed down the main street.", "下水道被淹，水溢到了大街上。" },
    { "CET6_3", "telecommunications", "ˈtɛləkəˈmjʊnəˈkeʃənz", "n 通信，电信", "a new telecommunications system", "新的电信系统" },
    { "KaoYan_2", "paragraph", "ˈpærəɡræf", "n 段落；短评；段落符号；v 将…分段", "the opening paragraphs of the novel", "小说开篇的几个段落" },
    { "KaoYan_2", "straight", "stret", "adj 直的；连续的；笔直的；正直的；整齐的；异性恋的；adv 直接地；不断地；立即；坦率地；n 直；直线；直男，直女，异性恋者", "a long, straight road", "一条笔直的长路" },
    { "KaoYan_2", "straightforward", "", "adj 简单的；坦率的；明确的；径直的；adv 直截了当地；坦率地", "This area of law is far from straightforward (= complicated ) .", "法律的这个领域很复杂。" },
    { "IELTS_2", "abandon", "əˈbændən", "v 离弃；放弃", "How could she abandon her own child?", "她怎么能抛弃自己的孩子？" },
    { "IELTS_2", "be", "bi", "auxiliary verb at；auxiliary verb odds；auxiliary verb with…", "Don’t disturb me while I’m working.", "我工作的时候不要打扰我。" },
    { "IELTS_2", "telecommunication", "", "n 电信，电信学", "If large media and telecommunication companies decide to close their offerings to the web, I am…", "如果大型媒体和电信公司决定停止向万维网提供内容，我很确信万维网会绕过这一点。" },
    { "GaoZhong_2", "forgive", "fɚˈɡɪv", "v 原谅；免除（债务、义务等）", "He didn’t look the sort of man to forgive and forget (= forgive someone and no longer think about it ) .", "他看上去不是那种不记仇的人。" },
};

static const unsigned kWdFixtureCaseCount =
    (unsigned)(sizeof(kWdFixtureCases) / sizeof(kWdFixtureCases[0]));
