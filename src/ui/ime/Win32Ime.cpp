#include "Win32Ime.h"

#include <algorithm>
#include <utility>

namespace Ime
{
    void Win32Ime::Attach(HWND window)
    {
        window_ = window;
        if (syncMessage_) PostMessageW(window, syncMessage_, 0, 0);
    }

    void Win32Ime::Publish(const Request& request)
    {
        bool changed;
        {
            std::lock_guard lock(mutex_);
            changed = request.backend != requested_.backend || request.generation != requested_.generation ||
                request.editing != requested_.editing || request.caret.x != requested_.caret.x ||
                request.caret.y != requested_.caret.y || request.lineHeight != requested_.lineHeight;
            requested_ = request;
        }
        if (changed && window_ && syncMessage_) PostMessageW(window_, syncMessage_, 0, 0);
    }

    LRESULT Win32Ime::Default(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        return IsWindowUnicode(window) ? DefWindowProcW(window, message, wParam, lParam) :
            DefWindowProcA(window, message, wParam, lParam);
    }

    void Win32Ime::Release(HWND window)
    {
        if (!active_ && !owned_) return;
        // 先停止接收，取消组词期间 IMM 可能重入窗口过程。
        active_ = false;
        const auto context = ImmGetContext(window);
        const auto expected = owned_ ? owned_ : borrowed_;
        if (context && context == expected)
            ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
        if (context) ImmReleaseContext(window, context);
        if (owned_)
        {
            // 若其他组件已经换了上下文，不覆盖它的新关联。
            if (context == owned_) ImmAssociateContext(window, previous_);
            ImmDestroyContext(owned_);
        }
        owned_ = previous_ = borrowed_ = nullptr;
        leadByte_ = 0;
        std::lock_guard lock(mutex_);
        status_.active = status_.composing = status_.contextAvailable = false;
        keys_.End();
    }

    void Win32Ime::Synchronize(HWND window)
    {
        Request next;
        {
            std::lock_guard lock(mutex_);
            next = requested_;
        }
        if (current_.generation != next.generation || current_.backend != next.backend || !next.editing)
            Release(window);
        current_ = next;
        if (!next.editing || !IsNative(next.backend) || GetFocus() != window || suspendedGeneration_ == next.generation) return;
        if (!active_)
        {
            if (next.backend == Backend::ImmPrivate)
            {
                const auto existing = ImmGetContext(window);
                const auto wasOpen = existing ? ImmGetOpenStatus(existing) : FALSE;
                if (existing) ImmReleaseContext(window, existing);
                owned_ = ImmCreateContext();
                if (owned_)
                {
                    previous_ = ImmAssociateContext(window, owned_);
                    ImmSetOpenStatus(owned_, wasOpen);
                }
            }
            const auto context = ImmGetContext(window);
            active_ = context && (next.backend != Backend::ImmPrivate || (owned_ && context == owned_));
            borrowed_ = context;
            if (context) ImmReleaseContext(window, context);
            if (!active_)
            {
                Release(window);
                suspendedGeneration_ = next.generation;
            }
            std::lock_guard lock(mutex_);
            status_.active = status_.contextAvailable = active_;
            status_.associationChanged = false;
            if (!active_) ++status_.failures;
        }
        Position(window);
    }

    void Win32Ime::Position(HWND window)
    {
        if (!active_) return;
        const auto context = ImmGetContext(window);
        if (context != (owned_ ? owned_ : borrowed_))
        {
            if (context) ImmReleaseContext(window, context);
            Release(window);
            suspendedGeneration_ = current_.generation;
            std::lock_guard lock(mutex_);
            status_.associationChanged = true;
            return;
        }
        if (!context) return;
        COMPOSITIONFORM composition{};
        composition.dwStyle = CFS_POINT;
        composition.ptCurrentPos = current_.caret;
        ImmSetCompositionWindow(context, &composition);
        CANDIDATEFORM candidate{};
        candidate.dwStyle = CFS_EXCLUDE;
        candidate.ptCurrentPos = current_.caret;
        candidate.rcArea = { current_.caret.x, current_.caret.y, current_.caret.x + 1,
            current_.caret.y + current_.lineHeight };
        ImmSetCandidateWindow(context, &candidate);
        ImmReleaseContext(window, context);
    }

    void Win32Ime::Queue(std::wstring text)
    {
        if (text.empty()) return;
        std::lock_guard lock(mutex_);
        if (!requested_.editing || requested_.generation != current_.generation) return;
        commits_.push_back({ current_.generation, std::move(text) });
        ++status_.commits;
    }

    std::optional<LRESULT> Win32Ime::Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (syncMessage_ && message == syncMessage_) { Synchronize(window); return 0; }
        if (message == WM_KILLFOCUS || (message == WM_ACTIVATEAPP && !wParam) || message == WM_NCDESTROY)
        {
            Release(window);
            if (message == WM_NCDESTROY) window_ = nullptr;
            return std::nullopt;
        }
        if (message == WM_SETFOCUS) Synchronize(window);
        if (!active_) return std::nullopt;
        if (message == WM_IME_COMPOSITION || message == WM_IME_STARTCOMPOSITION || message == WM_CHAR || message == WM_UNICHAR)
        {
            Position(window);
            if (!active_) return std::nullopt;
        }
        switch (message)
        {
        case WM_IME_STARTCOMPOSITION:
            {
                std::lock_guard lock(mutex_);
                status_.composing = true;
                keys_.Begin();
            }
            Position(window);
            return Default(window, message, wParam, lParam);
        case WM_IME_ENDCOMPOSITION:
            {
                std::lock_guard lock(mutex_);
                status_.composing = false;
                // 消息与游戏轮询不在同一帧：确认/取消键仍须成对吞掉。
                keys_.End();
            }
            return Default(window, message, wParam, lParam);
        case WM_IME_COMPOSITION:
            if (lParam & GCS_RESULTSTR)
            {
                const auto context = ImmGetContext(window);
                if (context)
                {
                    const auto bytes = ImmGetCompositionStringW(context, GCS_RESULTSTR, nullptr, 0);
                    if (bytes > 0 && bytes <= 1024 * 1024 && bytes % sizeof(wchar_t) == 0)
                    {
                        std::wstring text(static_cast<std::size_t>(bytes) / sizeof(wchar_t), L'\0');
                        if (ImmGetCompositionStringW(context, GCS_RESULTSTR, text.data(), bytes) == bytes) Queue(std::move(text));
                    }
                    ImmReleaseContext(window, context);
                }
                std::lock_guard lock(mutex_);
                keys_.Commit();
            }
            // 提交串只读 GCS_RESULTSTR；不再让默认处理生成同一串 WM_IME_CHAR/WM_CHAR。
            lParam &= ~(GCS_RESULTSTR | GCS_RESULTCLAUSE | GCS_RESULTREADSTR | GCS_RESULTREADCLAUSE);
            if (!lParam) return 0;
            return Default(window, message, wParam, lParam);
        case WM_IME_CHAR: return 0;
        case WM_IME_SETCONTEXT:
        case WM_IME_NOTIFY:
            return Default(window, message, wParam, lParam);
        case WM_CHAR:
            {
                std::lock_guard lock(mutex_);
                if (status_.composing) return 0;
            }
            if (wParam >= 32 && wParam != 127)
            {
                if (IsWindowUnicode(window)) Queue(std::wstring(1, static_cast<wchar_t>(wParam)));
                else
                {
                    // ANSI 游戏窗口的普通文字按当前键盘布局代码页解码；IME 提交始终用 W API。
                    DWORD codePage = CP_ACP;
                    GetLocaleInfoW(MAKELCID(LOWORD(reinterpret_cast<UINT_PTR>(GetKeyboardLayout(0))), SORT_DEFAULT),
                        LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER, reinterpret_cast<wchar_t*>(&codePage), sizeof(codePage) / sizeof(wchar_t));
                    char bytes[2] = { leadByte_, static_cast<char>(wParam) };
                    if (wParam > 0xFF)
                    {
                        bytes[0] = static_cast<char>((wParam >> 8) & 0xFF);
                        leadByte_ = bytes[0];
                    }
                    if (!leadByte_ && IsDBCSLeadByteEx(codePage, bytes[1])) { leadByte_ = bytes[1]; return 0; }
                    wchar_t text[2]{};
                    const auto count = MultiByteToWideChar(codePage, 0, leadByte_ ? bytes : bytes + 1, leadByte_ ? 2 : 1, text, 2);
                    leadByte_ = 0;
                    if (count > 0) Queue(std::wstring(text, count));
                }
            }
            return 0;
        case WM_INPUTLANGCHANGE:
            leadByte_ = 0;
            Position(window);
            break;
        case WM_UNICHAR:
            if (wParam == UNICODE_NOCHAR) return TRUE;
            if (wParam >= 32 && wParam <= 0x10FFFF && !(wParam >= 0xD800 && wParam <= 0xDFFF))
            {
                if (wParam <= 0xFFFF) Queue(std::wstring(1, static_cast<wchar_t>(wParam)));
                else Queue({ static_cast<wchar_t>(0xD800 + ((wParam - 0x10000) >> 10)),
                    static_cast<wchar_t>(0xDC00 + ((wParam - 0x10000) & 0x3FF)) });
            }
            return 0;
        }
        return std::nullopt;
    }

    std::vector<Commit> Win32Ime::Drain()
    {
        std::lock_guard lock(mutex_);
        return std::exchange(commits_, {});
    }

    bool Win32Ime::FilterKey(unsigned key, bool down)
    {
        std::lock_guard lock(mutex_);
        return keys_.Filter(key, down);
    }

    Status Win32Ime::Snapshot()
    {
        std::lock_guard lock(mutex_);
        auto result = status_;
        result.suppressKeys = keys_.Suppresses();
        return result;
    }
    void Win32Ime::EndFrame() { std::lock_guard lock(mutex_); keys_.EndFrame(); }
}
