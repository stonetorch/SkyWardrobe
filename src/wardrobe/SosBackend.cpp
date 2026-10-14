#include "wardrobe/SosBackend.h"
#include "wardrobe/Model.h"
#include "wardrobe/ActorRuntime.h"
#include "wardrobe/ActualEquipment.h"
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <REX/W32/KERNEL32.h>
#include <functional>
#include <memory>

namespace Wardrobe
{
    namespace
    {

        constexpr const char* scriptClass = "SkyrimOutfitSystemNativeFuncs";

        class Completion final : public RE::BSScript::IStackCallbackFunctor
        {
        public:
            explicit Completion(std::function<void(RE::BSScript::Variable)> callback) : callback_(std::move(callback)) {}
            void operator()(RE::BSScript::Variable result) override
            {
                SKSE::GetTaskInterface()->AddTask([callback = callback_, result]() mutable { callback(result); });
            }
            void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}
        private:
            std::function<void(RE::BSScript::Variable)> callback_;
        };

        struct Operation : std::enable_shared_from_this<Operation>
        {
            std::uint64_t epoch{};
            std::vector<RE::TESObjectARMO*> armors;
            std::vector<RE::TESObjectARMO*> actualArmors;
            bool clear{};
            int stage{};
            std::string actorKey, outfit;
            RE::ActorHandle actorHandle;
            ActualEquipmentMode mode{ActualEquipmentMode::Off};
            bool automatic{}, releaseOnly{}, restoreAfter{}, readOriginal{}, registered{};
            std::uint64_t version{};

            void Next(RE::BSScript::Variable result = {})
            {
                auto& backend = SosBackend::Get();
                if (!backend.Current(epoch)) return;
                if (!backend.CurrentActor(epoch, actorKey, version))
                { backend.Finish(epoch, "SOS request superseded"); return; }
                // 缺失原生插件时禁止进入 VM，连 IsEnabled 探测也不能调用。
                if (!backend.IsInstalled()) return;
                auto actor = actorHandle.get();
                auto* player = actor.get();
                auto* skyrim = RE::SkyrimVM::GetSingleton();
                auto vm = skyrim ? skyrim->impl : nullptr;
                auto* ui = RE::UI::GetSingleton();
                if (!player || !player->Is3DLoaded() || !vm || !ui || ui->IsMenuOpen(RE::BSFixedString("Main Menu")))
                {
                    backend.Finish(epoch, "Actor is not ready"); return;
                }
                // 第一步探测 native API，只有真实回调返回 enabled 才执行后续外观操作。
                if (stage == 1 && (!result.IsBool() || !result.GetBool()))
                {
                    backend.Finish(epoch, "SOS Revived is missing or disabled; enable it in its MCM"); return;
                }
                const char* function = nullptr;
                RE::BSScript::IFunctionArguments* arguments = nullptr;
                // 状态机中每一步捕获同一个角色 handle；界面切换不影响在途请求。
                switch (stage++)
                {
                case 0:
                    function = "IsEnabled"; arguments = RE::MakeFunctionArguments(); break;
                case 1:
                    if (automatic && !OriginalAutoSwitch(actorKey))
                    {
                        readOriginal = true;
                        function = "GetLocationBasedAutoSwitchEnabled"; arguments = RE::MakeFunctionArguments(std::move(player));
                        break;
                    }
                    Next(); return;
                case 2:
                    if (readOriginal)
                    {
                        if (!result.IsBool()) { backend.Finish(epoch, "Cannot read SOS auto-switch setting"); return; }
                        RememberAutoSwitch(actorKey, result.GetBool());
                        readOriginal = false;
                    }
                    if (automatic)
                    {
                        // 自动开关仅对 SOS 已注册角色生效，先注册再关闭。
                        if (!registered)
                        {
                            registered = true; --stage;
                            function = "AddActor"; arguments = RE::MakeFunctionArguments(std::move(player)); break;
                        }
                        function = "SetLocationBasedAutoSwitchEnabled";
                        arguments = RE::MakeFunctionArguments(std::move(player), false); break;
                    }
                    Next(); return;
                case 3:
                    if (releaseOnly) { stage = 9; Next(); return; }
                    function = clear ? "SetSelectedOutfit" : "OverwriteOutfit";
                    if (clear) arguments = RE::MakeFunctionArguments(std::move(player), std::string{});
                    else arguments = RE::MakeFunctionArguments(std::string(outfit), std::move(armors));
                    break;
                case 4:
                    if (clear || registered) { Next(); return; }
                    function = "AddActor"; arguments = RE::MakeFunctionArguments(std::move(player)); break;
                case 5:
                    if (clear) { Next(); return; }
                    function = "SetSelectedOutfit";
                    arguments = RE::MakeFunctionArguments(std::move(player), std::string(outfit)); break;
                case 6:
                    function = "RefreshArmorFor"; arguments = RE::MakeFunctionArguments(std::move(player)); break;
                case 7:
                    function = "GetSelectedOutfit"; arguments = RE::MakeFunctionArguments(std::move(player)); break;
                case 8:
                    if (!result.IsString() || std::string(result.GetString()) != (clear ? "" : outfit))
                    { backend.Finish(epoch, "SOS selected outfit readback failed"); return; }
                    if (!clear)
                    {
                        const auto error = ApplyActualEquipment(actualArmors, mode, player);
                        if (!error.empty()) { backend.Finish(epoch, "SOS appearance refreshed; " + error); return; }
                    }
                    Next(); return;
                case 9:
                    if (restoreAfter)
                        if (const auto original = OriginalAutoSwitch(actorKey))
                        {
                            function = "SetLocationBasedAutoSwitchEnabled";
                            arguments = RE::MakeFunctionArguments(std::move(player), bool(*original)); break;
                        }
                    Next(); return;
                default:
                    if (restoreAfter) RememberAutoSwitch(actorKey, std::nullopt);
                    backend.Finish(epoch, releaseOnly ? automatic ? "SOS automatic switching suspended" : "SOS automatic switching restored" : clear ? "SOS appearance cleared" : "SOS appearance refreshed", true);
                    return;
                }
                auto self = shared_from_this();
                RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback(new Completion([self](auto value) { self->Next(value); }));
                if (!vm->DispatchStaticCall(scriptClass, function, arguments, callback))
                    backend.Finish(epoch, std::string("SOS API unavailable: ") + function);
            }
        };
    }

    SosBackend& SosBackend::Get() { static SosBackend backend; return backend; }

    void SosBackend::DetectInstallation()
    {
        // DataLoaded 时 SKSE 插件已加载；检测进程模块，不把磁盘残留文件当作已安装。
        installed_.store(REX::W32::GetModuleHandleW(L"SkyrimOutfitSystemSE.dll") != nullptr);
        Reset();
        if (!IsInstalled()) SKSE::log::warn("{}", Status());
    }

    void SosBackend::SetEquipmentMode(ActualEquipmentMode mode)
    {
        equipmentMode_.store(mode);
        if (mode == ActualEquipmentMode::Off) CleanupEquipment("player");
    }

    void SosBackend::CleanupEquipment(std::string actor)
    {
        if (!IsInstalled()) return;
        std::uint64_t epoch;
        std::uint64_t version;
        { std::lock_guard lock(mutex_); epoch = epoch_; version = actorVersions_[actor]; }
        SKSE::GetTaskInterface()->AddTask([this, actor = std::move(actor), epoch, version]
        {
            if (!CurrentActor(epoch, actor, version)) return;
            auto* target = ResolveActor(actor);
            if (target && target->Is3DLoaded()) ClearTemporaryEquipment(target);
        });
    }

    void SosBackend::Request(std::vector<std::string> keys, bool clear)
    {
        RequestFor("player", std::move(keys), clear, EquipmentMode());
    }

    void SosBackend::ReleaseActor(std::string actor, bool clear)
    {
        RequestFor(std::move(actor), {}, clear, ActualEquipmentMode::Off, false, !clear);
    }

    void SosBackend::RequestFor(std::string actor, std::vector<std::string> keys, bool clear,
        ActualEquipmentMode mode, bool automatic, bool releaseOnly, std::function<void(bool)> completion)
    {
        if (!IsInstalled()) { if (completion) completion(false); return; }
        std::function<void(bool)> cancelled;
        {
            std::lock_guard lock(mutex_);
            auto it = std::find_if(pending_.begin(), pending_.end(), [&](const auto& request) { return request.actor == actor; });
            if (it != pending_.end()) { cancelled = std::move(it->completion); pending_.erase(it); }
            const auto version = ++actorVersions_[actor];
            pending_.push_back({std::move(actor), std::move(keys), clear, automatic, releaseOnly, mode, std::move(completion), version});
            status_ = "SOS request queued";
        }
        if (cancelled) cancelled(false);
        SKSE::GetTaskInterface()->AddTask([this] { StartNext(); });
    }

    void SosBackend::CancelActor(const std::string& actor)
    {
        std::function<void(bool)> cancelled;
        {
            std::lock_guard lock(mutex_);
            ++actorVersions_[actor];
            auto it = std::find_if(pending_.begin(), pending_.end(), [&](const auto& request) { return request.actor == actor; });
            if (it != pending_.end()) { cancelled = std::move(it->completion); pending_.erase(it); }
        }
        if (cancelled) cancelled(false);
    }

    void SosBackend::StartNext()
    {
        if (!IsInstalled()) return;
        auto operation = std::make_shared<Operation>();
        std::vector<std::string> keys;
        {
            std::lock_guard lock(mutex_);
            if (busy_ || pending_.empty()) return;
            busy_ = true;
            auto request = std::move(pending_.front()); pending_.pop_front();
            keys = std::move(request.keys);
            operation->clear = request.clear; operation->epoch = epoch_;
            operation->actorKey = std::move(request.actor);
            operation->version = request.version;
            operation->mode = request.mode; operation->automatic = request.automatic;
            operation->releaseOnly = request.releaseOnly;
            operation->restoreAfter = !request.automatic;
            completion_ = std::move(request.completion);
            status_ = "Waiting for SOS callbacks";
        }
        auto* actor = ResolveActor(operation->actorKey);
        if (!actor || !actor->Is3DLoaded()) { Finish(operation->epoch, "Actor unavailable: " + operation->actorKey); return; }
        operation->actorHandle = actor->GetHandle();
        operation->outfit = RuntimeOutfitName(operation->actorKey);
        auto* data = RE::TESDataHandler::GetSingleton();
        for (const auto& text : keys)
        {
            const auto key = FormKey::Parse(text);
            auto* form = key && data ? data->LookupForm(key->localFormID, key->plugin) : nullptr;
            auto* armor = form ? form->As<RE::TESObjectARMO>() : nullptr;
            // 缺失模组时拒绝整个应用，避免用户误以为预设已完整展示。
            if (!armor) { Finish(operation->epoch, "Unresolved armor: " + text); return; }
            operation->armors.push_back(armor);
        }
        operation->actualArmors = operation->armors;
        // 清除临时物品独立于 SOS 启用状态；玩家已有服装保留当前穿戴。
        if (operation->clear)
        {
            const auto error = ClearTemporaryEquipment(actor);
            if (!error.empty()) { Finish(operation->epoch, error); return; }
        }
        operation->Next();
    }

    void SosBackend::Finish(std::uint64_t epoch, std::string message, bool success)
    {
        std::function<void(bool)> completion;
        {
            std::lock_guard lock(mutex_);
            if (epoch != epoch_) return;
            completion = std::move(completion_);
            status_ = std::move(message); busy_ = false;
            SKSE::log::info("{}", status_);
        }
        if (completion) completion(success);
        StartNext();
    }

    bool SosBackend::Current(std::uint64_t epoch) const { std::lock_guard lock(mutex_); return epoch == epoch_; }
    bool SosBackend::CurrentActor(std::uint64_t epoch, const std::string& actor, std::uint64_t version) const
    {
        std::lock_guard lock(mutex_);
        const auto it = actorVersions_.find(actor);
        return epoch == epoch_ && it != actorVersions_.end() && it->second == version;
    }
    std::string SosBackend::Status() const { std::lock_guard lock(mutex_); return status_; }
    void SosBackend::Reset()
    {
        std::lock_guard lock(mutex_);
        ++epoch_; busy_ = false; pending_.clear(); completion_ = {}; actorVersions_.clear();
        status_ = IsInstalled() ? "SOS idle" : "Skyrim Outfit System is not installed or loaded; all SkyWardrobe features are disabled";
    }
}
