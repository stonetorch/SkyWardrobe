#include "animation/HkxMetadata.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace
{
    void Check(bool condition)
    {
        if (!condition) throw std::runtime_error("HKX metadata regression failed");
    }
    void UInt(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
    {
        for (int i = 0; i < 4; ++i) bytes.at(offset+i) = static_cast<std::uint8_t>(value >> (i*8));
    }
    void String(std::vector<std::uint8_t>& bytes, std::size_t offset, std::string_view value)
    {
        std::copy(value.begin(), value.end(), bytes.begin()+offset);
        bytes.at(offset+value.size()) = 0;
    }
    std::vector<std::uint8_t> Fixture(bool paired)
    {
        // 构造独立的 packfile 边界与 fixup，用于验证解析器不扫描压缩载荷猜名称。
        constexpr std::size_t data = 336, local = 2048, global = 2104, end = 2128;
        std::vector<std::uint8_t> bytes(data+end);
        UInt(bytes, 0, 0x57E0E057); UInt(bytes, 4, 0x10C0C010); UInt(bytes, 12, 8);
        bytes[16] = 8; bytes[17] = 1; bytes[19] = 1;
        UInt(bytes, 20, 3); String(bytes, 40, "hk_2010.2.0-r1"); bytes[62] = bytes[63] = 0xFF;
        UInt(bytes, 84, 208);
        for (int i = 1; i < 7; ++i) UInt(bytes, 84+i*4, 128);
        UInt(bytes, 132, data);
        UInt(bytes, 180, data); UInt(bytes, 184, local); UInt(bytes, 188, global);
        UInt(bytes, 192, global);
        for (int i = 0; i < 3; ++i) UInt(bytes, 196+i*4, end);
        String(bytes, 208, "hkaSplineCompressedAnimation"); String(bytes, 248, "hkaAnimationBinding");
        UInt(bytes, data+0x18, 5); UInt(bytes, data+0x30, 5);
        const std::string_view names[]{ "PairedRoot", "2_", "2_NPC Root [Root]", "NPC", "NPC Root [Root]" };
        UInt(bytes, data+local, 0x28); UInt(bytes, data+local+4, 128);
        UInt(bytes, data+local+8, 64+0x10); UInt(bytes, data+local+12, 400);
        String(bytes, data+400, paired ? "PairedRoot" : "NPC Root [Root]");
        for (std::size_t i = 0; i < 5; ++i)
        {
            UInt(bytes, data+local+16+i*8, 128+i*24);
            UInt(bytes, data+local+20+i*8, 512+i*64);
            String(bytes, data+512+i*64, paired ? names[i] : std::string_view{});
        }
        UInt(bytes, data+global, 0); UInt(bytes, data+global+4, 0); UInt(bytes, data+global+8, 0);
        UInt(bytes, data+global+12, 64); UInt(bytes, data+global+16, 0); UInt(bytes, data+global+20, 40);
        return bytes;
    }
}

int main(int argc, char** argv)
{
    const auto paired = CAF::Animation::ReadHkxMetadata(Fixture(true));
    Check(paired.valid && paired.paired && paired.transformTracks == 5 && paired.roots.size() == 2 &&
        paired.roots[0] == "2_" && paired.roots[1] == "NPC");
    const auto single = CAF::Animation::ReadHkxMetadata(Fixture(false));
    Check(single.valid && !single.paired && single.roots.empty());
    auto corrupt = Fixture(true); corrupt.resize(200);
    Check(!CAF::Animation::ReadHkxMetadata(corrupt).valid);
    corrupt = Fixture(true); UInt(corrupt, 336+2048+4, 2048);
    Check(!CAF::Animation::ReadHkxMetadata(corrupt).valid);
    corrupt = Fixture(true); UInt(corrupt, 336+0x30, 4097);
    Check(!CAF::Animation::ReadHkxMetadata(corrupt).valid);
    corrupt = Fixture(true); corrupt[16] = 4;
    Check(!CAF::Animation::ReadHkxMetadata(corrupt).valid);
    corrupt = Fixture(true); UInt(corrupt, 336+2104+4, 99);
    Check(!CAF::Animation::ReadHkxMetadata(corrupt).valid);
    // 可选只读真实文件入口，与编辑器共用同一解析器；无需加载 Skyrim。
    for (int i = 1; i < argc; ++i)
    {
        std::ifstream file(argv[i], std::ios::binary);
        Check(file.good());
        const std::vector<std::uint8_t> bytes{ std::istreambuf_iterator<char>(file), {} };
        const auto result = CAF::Animation::ReadHkxMetadata(bytes);
        std::cout << argv[i] << ": " << (result.valid ? (result.paired ? "paired" : "single") : result.error)
            << "; tracks=" << result.transformTracks << "; roots=";
        for (const auto& root : result.roots) std::cout << '[' << root << ']';
        std::cout << '\n';
        Check(result.valid);
    }
}
