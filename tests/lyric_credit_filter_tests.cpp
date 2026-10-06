#include "../third_party/foobar2000-sdk/foobar2000/foo_speaklyrics/lyric_credit_filter.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

// Expected indexes were checked against the unmodified filter from 7185c54.
// This suite verifies the requested 0.6.1 behavior, including its limitations.
// The later body-preservation tests are saved in the pre-restore source backup.
struct historical_case {
    const char* name;
    std::vector<lyric_credit_filter_line> lines;
    lyric_credit_filter_context context;
    std::size_t expected_body_start;
};

void verify(const historical_case& sample) {
    const auto actual = lyric_credit_filter::find_body_start(sample.lines, sample.context);
    if (actual != sample.expected_body_start || actual > sample.lines.size()) {
        std::cerr << sample.name << ": body index=" << actual
            << ", v0.6.1 expected=" << sample.expected_body_start << "\n";
        std::abort();
    }
}

void test_historical_cases() {
    const historical_case cases[] = {
        { "empty document", {}, {}, 0 },
        { "no credits", {
            {0, L"藕花香 染檐牙"}, {6000, L"惹那诗人纵步随她"}
        }, {}, 0 },
        { "ordinary production fields", {
            {0, L"作词：张三"}, {1000, L"作曲：李四"}, {2000, L"编曲：王五"},
            {10000, L"第一句歌词"}, {16000, L"第二句歌词"}, {22000, L"第三句歌词"}
        }, {}, 3 },
        { "continue past an unknown intro line", {
            {0, L"作词：张三"}, {1000, L"某某乐团现场演奏"}, {2000, L"混音：李四"},
            {10000, L"第一句歌词"}, {16000, L"第二句歌词"}, {22000, L"第三句歌词"}
        }, {}, 3 },
        { "generic bilingual fields", {
            {0, L"乐器1：某某"}, {1000, L"制作统筹Production Coordination：某某"}, {2000, L"PGM：某某"},
            {10000, L"第一句歌词"}, {16000, L"第二句歌词"}, {22000, L"第三句歌词"}
        }, {}, 3 },
        { "spaced title artist header", {
            {0, L"歌曲 - 歌手"}, {1000, L"作词：张三"}, {10000, L"第一句歌词"}
        }, {L"歌曲", L"歌手"}, 2 },
        { "title header without an artist tag", {
            {0, L"歌曲 - 歌手"}, {9000, L"第一句歌词"}
        }, {L"歌曲", L""}, 1 },
        { "rediscover credits after an unmatched compact header", {
            {0, L"鸳鸯戏-张含韵"}, {1000, L"词：家浚"}, {2000, L"第一句歌词"}
        }, {}, 2 },
        { "OP SP credits", {
            {0, L"词：家浚"}, {1000, L"曲：家浚/乐金震"},
            {2000, L"OP/SP：昌禾文化"}, {3000, L"特别鸣谢：快手音乐"},
            {5000, L"一年四季的更替"}, {7000, L"竹篱下的乱花影"}
        }, {}, 4 },
        { "compound label remains unrecognized in v061", {
            {0, L"鸳鸯戏 - 张含韵"}, {1000, L"词曲OP/SP"}, {2000, L"第一句歌词"}
        }, {L"鸳鸯戏", L"张含韵"}, 1 },
        { "incomplete field retains separate names in v061", {
            {0, L"作词："}, {1000, L"张三"}, {2000, L"李四"},
            {9000, L"第一句歌词"}, {14000, L"第二句歌词"}
        }, {}, 1 },
        { "short ordinary lyric after a complete field", {
            {0, L"编曲：某某"}, {1000, L"啊"}, {2000, L"还在等你"}
        }, {}, 1 },
        { "later credit-like words follow v061 scanning", {
            {0, L"作词：某某"}, {5000, L"第一行歌词"}, {10000, L"发行 在时光里"},
            {15000, L"和声 从远方传来"}, {20000, L"设计 一场梦"}
        }, {}, 4 },
        { "credits can be found after an ordinary first line in v061", {
            {0, L"第一句歌词"}, {5000, L"作词：某某"}, {10000, L"第二句歌词"}
        }, {}, 2 },
        { "Furong Yu historical body start", {
            {0, L"芙蓉雨 - 刘珂矣"}, {9220, L"词：刘珂矣/百慕三石"},
            {18450, L"曲：刘珂矣/百慕三石"}, {27670, L"编曲：刘珂矣"},
            {36900, L"藕花香 染檐牙"}, {43360, L"惹那诗人纵步随她"},
            {49740, L"佩声微 琴声儿退"}, {56340, L"斗胆了一池眉叶丹砂"}
        }, {L"芙蓉雨", L"刘珂矣"}, 4 },
        { "Yue Man Xian historical body start", {
            {0, L"月满弦 - 刘珂矣"}, {8240, L"词：刘珂矣"},
            {16480, L"曲：百慕三石/刘珂矣"}, {24730, L"编曲：百慕三石"},
            {32970, L"古塔旁 拾一地金黄"}, {40070, L"雁扫落 满腹旧霜"},
            {46950, L"斑驳的木窗 等候月光"}, {53710, L"是谁路过 诵一首情长"}
        }, {L"月满弦", L"刘珂矣"}, 4 },
        { "preserved opening line ignores whitespace", {
            {0, L"作词：某某"}, {1000, L"编曲：某某"}, {2000, L"古塔旁　拾一地金黄"},
            {3000, L"雁扫落 满腹旧霜"}, {4000, L"斑驳的木窗 等候月光"}
        }, {}, 2 },
        { "embedded preserved words remain ordinary text", {
            {0, L"作词：某某"}, {1000, L"古塔旁 拾一地金黄啊"},
            {2000, L"第一句歌词"}, {3000, L"第二句歌词"}, {4000, L"第三句歌词"}
        }, {}, 1 },
        { "repeated opening line is recognized by v061", {
            {0, L"编曲：某某"}, {1000, L"藕花香 染檐牙"},
            {2000, L"佩声微 琴声儿退"}, {3000, L"第一句歌词"},
            {20000, L"藕花香 染檐牙"}
        }, {}, 1 },
        { "Achugu Niang historical intro boundary", {
            {0, L"阿楚姑娘 (Live) - 马嘉祺/宋亚轩"}, {2280, L"词：梦野"},
            {2990, L"曲：梁凡"}, {3690, L"原唱：梁凡"}, {5100, L"改编编曲：宋涛"},
            {6330, L"吉他：吴星辰/王山"}, {7910, L"贝斯：孟凡荻"}, {8970, L"键盘：谭啸"},
            {9850, L"鼓：于皓丞"}, {10730, L"和声编写：周安妮Annie@字在声"},
            {12840, L"和声：杨一川/周安妮Annie"}, {14780, L"PGM：吴文隽/彭浩楷"},
            {16180, L"音乐总监：栾卓忻@字在声"}, {18060, L"人声编辑：王威@MonKey Music"},
            {20060, L"Live Mix：沈会斌/THREE@MonKey Music"}, {21640, L"宋亚轩："},
            {22340, L"在距离城市很远的地方"}, {26200, L"在我那沃野炊烟的故乡"},
            {32810, L"有一个叫烽火台的村庄"}
        }, {L"阿楚姑娘 (Live)", L"马嘉祺/宋亚轩"}, 15 },
        { "banner alone does not start a credit block in v061", {
            {0, L"www.example.com"}, {10000, L"第一句歌词"}, {20000, L"第二句歌词"}
        }, {}, 0 },
    };
    for (const auto& sample : cases) verify(sample);
}

void test_historical_discovery_limit() {
    historical_case sample = { "credit discovery stops at 40 lines", {}, {}, 0 };
    for (int i = 0; i < 40; ++i) sample.lines.push_back({i * 1000, L"普通歌词"});
    sample.lines.push_back({40000, L"作词：某某"});
    sample.lines.push_back({45000, L"正文歌词"});
    verify(sample);
}

}

int main() {
    test_historical_cases();
    test_historical_discovery_limit();
    std::cout << "lyric_credit_filter v0.6.1 compatibility tests passed\n";
    return 0;
}
