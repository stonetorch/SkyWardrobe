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
            std::vector<std::string> keys;
            LayerSettings layers;
            std::vector<LayerItem> originalSnapshot;
            bool policySet{}, policyReset{}, shieldSet{};

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
                    if (!clear && !policyReset)
                    {
                        policyReset = true; --stage;
                        function = "SetBodySlotPolicyToDefaultForOutfit";
                        arguments = RE::MakeFunctionArguments(std::string(outfit)); break;
                    }
                    if (!clear && !policySet)
                    {
                        // 合成已包含允许透出的原版项，禁止 SOS 再自行透出。
                        policySet = true; --stage;
                        function = "SetAllBodySlotPoliciesForOutfit";
                        arguments = RE::MakeFunctionArguments(std::string(outfit), std::string("XXOO")); break;
                    }
                    if (!clear && !shieldSet)
                    {
                        shieldSet = true; --stage;
                        function = "SetBodySlotPoliciesForOutfit";
                        arguments = RE::MakeFunctionArguments(std::string(outfit), std::uint32_t(9), std::string("XXOO")); break;
                    }
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
                    if (!clear && !releaseOnly) backend.TrackLayers(actorKey, keys, layers, mode, automatic, originalSnapshot);
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
        ActualEquipmentMode mode, bool automatic, bool releaseOnly, std::function<void(bool)> completion, LayerSettings layers)
    {
        if (!IsInstalled()) { if (completion) completion(false); return; }
        std::function<void(bool)> cancelled;
        {
            std::lock_guard lock(mutex_);
            auto it = std::find_if(pending_.begin(), pending_.end(), [&](const auto& request) { return request.actor == actor; });
            if (it != pending_.end()) { cancelled = std::move(it->completion); pending_.erase(it); }
            if (clear) activeLayers_.erase(actor);
            const auto version = ++actorVersions_[actor];
            pending_.push_back({std::move(actor), std::move(keys), clear, automatic, releaseOnly, mode, std::move(completion), version, std::move(layers)});
            status_ = "SOS request queued";
        }
        if (cancelled) cancelled(false);
        SKSE::GetTaskInterface()->AddTask([this] { StartNext(); });
    }

    void SosBackend::TrackLayers(const std::string& actor, std::vector<std::string> keys, LayerSettings layers,
        ActualEquipmentMode mode, bool automatic, std::vector<LayerItem> equipment)
    {
        std::lock_guard lock(mutex_);
        activeLayers_[actor] = {{actor, std::move(keys), false, automatic, false, mode, {}, actorVersions_[actor], std::move(layers)}, std::move(equipment)};
    }

    void SosBackend::RefreshLayers(const std::string& actor, const std::vector<LayerItem>& equipment)
    {
        // 复用本轮角色快照中的装备，避免轮询时重复扫描同一物品栏。
        RequestData request;
        {
            std::lock_guard lock(mutex_);
            const auto it = activeLayers_.find(actor);
            if (busy_ || it == activeLayers_.end() || !pending_.empty() || it->second.equipment == equipment) return;
            request = it->second.request;
            // 轮询合并装备变化，不推进自动换装的随机或顺序进度。
            it->second.equipment = equipment;
        }
        RequestFor(actor, request.keys, false, request.mode, request.automatic, false, {}, request.layers);
    }

    void SosBackend::CancelActor(const std::string& actor)
    {
        std::function<void(bool)> cancelled;
        {
            std::lock_guard lock(mutex_);
            ++actorVersions_[actor];
            activeLayers_.erase(actor);
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
            operation->keys = keys; operation->layers = std::move(request.layers);
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
        std::map<std::string, RE::TESObjectARMO*> forms;
        std::string unresolved;
        const auto resolve = [&](const std::vector<std::string>& list, std::vector<LayerItem>& items)
        {
            for (const auto& text : list)
            {
                const auto key = FormKey::Parse(text);
                auto* form = key && data ? data->LookupForm(key->localFormID, key->plugin) : nullptr;
                auto* armor = form ? form->As<RE::TESObjectARMO>() : nullptr;
                if (!armor) { unresolved = text; return false; }
                forms[text] = armor;
                items.push_back({text, armor->GetName(), armor->GetSlotMask().underlying()});
            }
            return true;
        };
        std::vector<LayerItem> working, backup;
        if (!operation->clear && !operation->releaseOnly)
        {
            if (!resolve(keys, working) || (operation->layers.backupEnabled && !resolve(operation->layers.backup, backup)))
            { Finish(operation->epoch, "Unresolved armor: " + unresolved); return; }
            const auto original = ReadOriginalEquipment(actor);
            operation->originalSnapshot = original;
            for (const auto& item : original)
            {
                const auto key = FormKey::Parse(item.key);
                auto* form = key && data ? data->LookupForm(key->localFormID, key->plugin) : nullptr;
                if (auto* armor = form ? form->As<RE::TESObjectARMO>() : nullptr) forms[item.key] = armor;
            }
            for (const auto& row : ComposeLayers(working, backup, original, operation->layers))
                if (row.disposition == LayerDisposition::Visible)
                {
                    if (!forms.contains(row.item.key)) { Finish(operation->epoch, "Unresolved original armor: " + row.item.key); return; }
                    operation->armors.push_back(forms.at(row.item.key));
                    if (row.source != LayerSource::Original) operation->actualArmors.push_back(forms.at(row.item.key));
                }
        }
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
        ++epoch_; busy_ = false; pending_.clear(); completion_ = {}; actorVersions_.clear(); activeLayers_.clear();
        status_ = IsInstalled() ? "SOS idle" : "Skyrim Outfit System is not installed or loaded; all SkyWardrobe features are disabled";
    }
}
