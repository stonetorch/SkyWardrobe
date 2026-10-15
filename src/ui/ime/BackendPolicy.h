#pragma once

#include <cstdint>
#include <array>

namespace Ime
{
    // 稳定编号用于配置和现场反馈；禁用某一路不会悄悄换到另一条原生实现。
    enum class Backend : int { Automatic, ImmShared, ImmPrivate, External, Disabled };

    constexpr bool IsNative(Backend value)
    {
        return value == Backend::ImmShared || value == Backend::ImmPrivate;
    }

    constexpr Backend Resolve(Backend requested, bool externalLoaded, bool conflict, unsigned disabled)
    {
        const auto allowed = [disabled](Backend value) { return !(disabled & (1u << static_cast<int>(value))); };
        if (conflict) return Backend::Disabled;
        if (requested == Backend::Automatic)
        {
            if (externalLoaded) return allowed(Backend::External) ? Backend::External : Backend::Disabled;
            if (allowed(Backend::ImmShared)) return Backend::ImmShared;
            if (allowed(Backend::ImmPrivate)) return Backend::ImmPrivate;
            return Backend::Disabled;
        }
        if (!allowed(requested)) return Backend::Disabled;
        if (IsNative(requested) && externalLoaded) return Backend::Disabled;
        if (requested == Backend::External && !externalLoaded) return Backend::Disabled;
        return requested;
    }

    class CompositionKeys
    {
    public:
        void Begin() { composing_ = true; }
        void End() { composing_ = false; boundaryFrames_ = 2; }
        void Commit() { boundaryFrames_ = 2; }
        bool Suppresses() const { return composing_ || boundaryFrames_ != 0; }
        void EndFrame() { if (boundaryFrames_) --boundaryFrames_; }
        bool Filter(unsigned key, bool down)
        {
            if (key >= blocked_.size()) return false;
            const bool block = Suppresses() || blocked_[key];
            // 组词期间按下的键，直到对应释放都不交给控件，避免长按 Esc 关闭面板。
            blocked_[key] = down && block;
            return block;
        }

    private:
        bool composing_ = false;
        unsigned boundaryFrames_ = 0;
        std::array<bool, 256> blocked_{};
    };
}
