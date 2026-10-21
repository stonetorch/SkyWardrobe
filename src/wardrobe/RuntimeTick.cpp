#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <Windows.h>
#include <dxgi.h>
#include <MinHook.h>
#include "wardrobe/RuntimeTick.h"
#include "wardrobe/Panel.h"

namespace
{
    using PresentFunction = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    PresentFunction originalPresent = nullptr;

    HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* swapChain, UINT interval, UINT flags)
    {
        // FLICK 隐藏全部窗口后不再调用工具绘制；业务轮询仍需每帧处理存盘和自动换装。
        // 沿用迁移前的渲染线程时序，避免与界面操作并发修改 Service。
        Wardrobe::ProcessUiRequests();
        return originalPresent(swapChain, interval, flags);
    }
}

void Wardrobe::InstallRuntimeTick()
{
    auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
    auto* swapChain = renderer ? reinterpret_cast<IDXGISwapChain*>(renderer->GetRuntimeData().renderWindows[0].swapChain) : nullptr;
    if (!swapChain)
    {
        SKSE::log::error("SkyWardrobe business tick cannot find the swap chain");
        return;
    }
    const auto initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) return;
    const auto address = (*reinterpret_cast<void***>(swapChain))[8];
    if (MH_CreateHook(address, &Present, reinterpret_cast<void**>(&originalPresent)) != MH_OK || MH_EnableHook(address) != MH_OK)
        SKSE::log::error("SkyWardrobe business tick installation failed");
}
