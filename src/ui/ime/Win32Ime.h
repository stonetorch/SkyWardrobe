#pragma once

#include "BackendPolicy.h"
#include <Windows.h>
#include <imm.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Ime
{
    struct Request
    {
        Backend backend = Backend::Disabled;
        std::uint64_t generation = 0;
        bool editing = false;
        POINT caret{};  // HWND 客户区坐标。
        LONG lineHeight = 20;
    };

    struct Commit
    {
        std::uint64_t generation;
        std::wstring text;
    };

    struct Status
    {
        bool active = false;
        bool composing = false;
        bool suppressKeys = false;
        bool contextAvailable = false;
        bool associationChanged = false;
        unsigned commits = 0;
        unsigned failures = 0;
    };

    // 可移植边界：仅依赖 Win32/IMM，不访问 Skyrim、SKSE 或 ImGui 全局状态。
    // Publish/Drain/FilterKey 在宿主线程调用；Handle 必须由 HWND 的窗口线程调用。
    class Win32Ime
    {
    public:
        void Attach(HWND window);
        void Publish(const Request& request);
        std::optional<LRESULT> Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
        std::vector<Commit> Drain();
        bool FilterKey(unsigned virtualKey, bool down);
        Status Snapshot();
        void EndFrame();

    private:
        void Synchronize(HWND window);
        void Release(HWND window);
        void Position(HWND window);
        void Queue(std::wstring text);
        LRESULT Default(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

        std::atomic<HWND> window_ = nullptr;
        UINT syncMessage_ = RegisterWindowMessageW(L"SkyWardrobe.PortableIme.Sync.v1");
        std::mutex mutex_;
        Request requested_;
        Status status_;
        std::vector<Commit> commits_;
        CompositionKeys keys_;
        // 以下资源只在窗口线程访问；不把借用的 HIMC 当成自有资源销毁。
        Request current_;
        HIMC owned_ = nullptr;
        HIMC previous_ = nullptr;
        HIMC borrowed_ = nullptr;
        bool active_ = false;
        std::optional<std::uint64_t> suspendedGeneration_;
        char leadByte_ = 0;
    };
}
