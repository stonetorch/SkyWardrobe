#include "wardrobe/Model.h"
#include <iostream>
#include <stdexcept>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
}

int main(int argc, char** argv)
{
    using namespace Wardrobe;
    try
    {
        Require(argc == 2 && InitializePinyinSearch(argv[1]), "Load packaged cpp-pinyin dictionary");
        for (const char* query : {"裙子", "qunzi", "qun zi", "qz", "qzi", "qu zi", "QZI"})
            Require(SearchScore(query, "裙子") > 0, "Automatic pinyin and mixed prefixes");
        for (const char* query : {"hei se lian yi qun", "heiselianyiqun", "hslyq", "lianyiqun", "lyq", "hei slyq", "hse lian yq", "heiselyq"})
            Require(SearchScore(query, "黑色连衣裙") > 0, "Full, initials, interior and mixed search");
        Require(SearchScore("qzi", "裙子") == SearchScore("qzi", "裙子"), "Repeated cached lookup");
        ClearPinyinSearchCache();
        Require(SearchScore("qzi", "裙子") > 0, "Cache rebuild");
        Require(SearchScore("qzi", "裙子") < SearchScore("裙子", "裙子"), "Original text ranks first");
        Require(SearchScore("zq", "裙子") == 0, "Reject reversed syllables");
        Require(SearchScore("qnzi", "裙子") == 0, "Reject skipped letters inside a syllable");
        Require(SearchScore("hlyq", "黑色连衣裙") == 0, "Reject skipped syllables");
        Require(SearchScore("qzi", "qunzi") == 0, "English is not automatically fuzzy matched");
        Require(SearchScore("qzi", "qun zi", true) > 0, "Manual pronunciation remains supported");
        Require(SearchScore("lyq", "連衣裙") > 0, "Traditional Chinese");
        Require(SearchScore("chongqing", "重庆") > 0, "Phrase pronunciation");
        Require(SearchScore("dressqzi", "Dress裙子") > 0, "Mixed English and Chinese");
        Require(SearchScore("nqzi", "女裙子") > 0, "Syllable boundaries");
        Require(SearchMetadata("wlf", Json{{"alias", "晚礼服"}}) > 0, "Metadata auto pinyin");
        Require(SearchMetadata("cq", Json{{"name", "重庆"}, {"search", {"zhong qing"}}}) > 0, "Automatic pronunciation retained");
        Require(SearchMetadata("zqing", Json{{"name", "重庆"}, {"pinyin", "zhong qing"}}) > 0, "Manual pronunciation correction");
        Require(!InitializePinyinSearch(std::filesystem::path(argv[1]) / "missing"), "Missing dictionary reported");
        Require(SearchScore("裙子", "裙子") > 0, "Original search survives missing dictionary");
        Require(SearchScore("qzi", "qun zi", true) > 0, "Manual pinyin survives missing dictionary");
        Require(SearchScore("qzi", "裙子") == 0, "Missing dictionary does not retain stale conversion");
        std::cout << "Pinyin search tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
