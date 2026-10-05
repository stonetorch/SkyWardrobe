#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace Overlay::InputMethod
{
    enum class Provider { kNone, kSimpleIME, kSkyrimInputMethod, kInputMethodUnlocked, kConflict };
    enum class CharacterSource { kGame, kScaleform };

    constexpr Provider SelectProvider(bool simpleIME, bool skyrimInputMethod, bool unlocked)
    {
        const auto count = static_cast<int>(simpleIME) + static_cast<int>(skyrimInputMethod) + static_cast<int>(unlocked);
        if (count > 1) return Provider::kConflict;
        if (simpleIME) return Provider::kSimpleIME;
        if (skyrimInputMethod) return Provider::kSkyrimInputMethod;
        if (unlocked) return Provider::kInputMethodUnlocked;
        return Provider::kNone;
    }

    constexpr bool IsExternal(Provider provider)
    {
        return provider != Provider::kNone && provider != Provider::kConflict;
    }

    constexpr bool NeedsMenuBridge(Provider provider)
    {
        // IMU 已在游戏事件入队前过滤组词按键，不需要再经过 Scaleform 菜单。
        return provider == Provider::kSimpleIME || provider == Provider::kSkyrimInputMethod;
    }

    constexpr bool CanAcquireTextInput(Provider provider, bool visible, bool focused, bool editing, bool menuOpen)
    {
        return IsExternal(provider) && visible && focused && editing && (!NeedsMenuBridge(provider) || menuOpen);
    }

    constexpr bool AcceptsSource(Provider provider, CharacterSource source)
    {
        // 按来源选择唯一文字通道；不能按字符值去重，否则连续相同汉字会被吞掉。
        if (provider == Provider::kSimpleIME || provider == Provider::kSkyrimInputMethod)
            return source == CharacterSource::kScaleform;
        return source == CharacterSource::kGame;
    }

    constexpr bool UsesProviderPaste(Provider provider)
    {
        return provider == Provider::kSimpleIME || provider == Provider::kSkyrimInputMethod;
    }

    constexpr bool IsPasteFallback(Provider provider, std::uint32_t code, bool ctrl)
    {
        // SimpleIME 关闭 Unicode 粘贴时放行原生 Ctrl+V 字符；SIM 放行的是控制字符 0x16。
        return ctrl && ((provider == Provider::kSimpleIME && (code == 'v' || code == 'V' || code == 0x16)) ||
            (provider == Provider::kSkyrimInputMethod && code == 0x16));
    }

    class TextSession
    {
    public:
        bool SetFocus(bool active, std::uint32_t id)
        {
            active = active && id != 0;
            id = active ? id : 0;
            if (active_ == active && id_ == id) return false;
            active_ = active;
            id_ = id;
            ++generation_;
            return true;
        }

        void Invalidate() { ++generation_; }
        bool Active() const { return active_; }
        std::uint32_t ID() const { return id_; }
        std::uint64_t Generation() const { return generation_; }
        bool Accepts(std::uint64_t generation) const { return active_ && generation == generation_; }

    private:
        bool active_ = false;
        std::uint32_t id_ = 0;
        std::uint64_t generation_ = 0;
    };

    class Utf16Decoder
    {
    public:
        std::vector<std::uint32_t> Push(std::uint32_t unit)
        {
            std::vector<std::uint32_t> result;
            if (unit >= 0xD800 && unit <= 0xDBFF)
            {
                if (high_) result.push_back(0xFFFD);
                high_ = unit;
                return result;
            }
            if (unit >= 0xDC00 && unit <= 0xDFFF)
            {
                result.push_back(high_ ? 0x10000 + ((*high_ - 0xD800) << 10) + unit - 0xDC00 : 0xFFFD);
                high_.reset();
                return result;
            }
            if (high_) result.push_back(0xFFFD);
            high_.reset();
            result.push_back(unit <= 0x10FFFF ? unit : 0xFFFD);
            return result;
        }

        void Reset() { high_.reset(); }

    private:
        std::optional<std::uint32_t> high_;
    };
}
