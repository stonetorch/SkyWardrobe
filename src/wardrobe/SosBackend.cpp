#include "wardrobe/SosBackend.h"
#include "wardrobe/Model.h"
#include "wardrobe/ActualEquipment.h"
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <functional>
#include <memory>

namespace Wardrobe
{
    namespace
    {
        constexpr const char* runtimeOutfit = "__WARDROBE_RUNTIME__";
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

            void Next(RE::BSScript::Variable result = {})
            {
                auto& backend = SosBackend::Get();
                if (!backend.Current(epoch)) return;
                auto* player = RE::PlayerCharacter::GetSingleton();
                auto* skyrim = RE::SkyrimVM::GetSingleton();
                auto vm = skyrim ? skyrim->impl : nullptr;
                auto* ui = RE::UI::GetSingleton();
                if (!player || !player->Is3DLoaded() || !vm || !ui || ui->IsMenuOpen(RE::BSFixedString("Main Menu")))
                {
                    backend.Finish(epoch, "Player is not ready"); return;
                }
                // 第一步探测 native API，只有真实回调返回 enabled 才执行后续外观操作。
                if (stage == 1 && (!result.IsBool() || !result.GetBool()))
                {
                    backend.Finish(epoch, "SOS Revived is missing or disabled; enable it in its MCM"); return;
                }
                const char* function = nullptr;
                RE::BSScript::IFunctionArguments* arguments = nullptr;
                switch (stage++)
                {
                case 0:
                    function = "IsEnabled"; arguments = RE::MakeFunctionArguments(); break;
                case 1:
                    function = clear ? "SetSelectedOutfit" : "OverwriteOutfit";
                    if (clear) arguments = RE::MakeFunctionArguments(std::move(player), std::string{});
                    else arguments = RE::MakeFunctionArguments(std::string(runtimeOutfit), std::move(armors));
                    break;
                case 2:
                    if (clear) { stage = 5; function = "RefreshArmorFor"; arguments = RE::MakeFunctionArguments(std::move(player)); }
                    else { function = "AddActor"; arguments = RE::MakeFunctionArguments(std::move(player)); }
                    break;
                case 3:
                    function = "SetSelectedOutfit";
                    arguments = RE::MakeFunctionArguments(std::move(player), std::string(runtimeOutfit)); break;
                case 4:
                    function = "RefreshArmorFor"; arguments = RE::MakeFunctionArguments(std::move(player)); break;
                case 5:
                    function = "GetSelectedOutfit"; arguments = RE::MakeFunctionArguments(std::move(player)); break;
                default:
                    if (!result.IsString() || std::string(result.GetString()) != (clear ? "" : runtimeOutfit))
                    {
                        backend.Finish(epoch, "SOS selected outfit readback failed"); return;
                    }
                    // SOS 回读成功后才实际穿戴；失败请求不会添加物品或替换玩家装备。
                    if (!clear)
                    {
                        const auto error = ApplyActualEquipment(actualArmors, backend.EquipmentMode());
                        if (!error.empty()) { backend.Finish(epoch, "SOS appearance refreshed; " + error); return; }
                    }
                    backend.Finish(epoch, clear ? "SOS appearance cleared" : "SOS appearance refreshed"); return;
                }
                auto self = shared_from_this();
                RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback(new Completion([self](auto value) { self->Next(value); }));
                if (!vm->DispatchStaticCall(scriptClass, function, arguments, callback))
                    backend.Finish(epoch, std::string("SOS API unavailable: ") + function);
            }
        };
    }

    SosBackend& SosBackend::Get() { static SosBackend backend; return backend; }

    void SosBackend::SetEquipmentMode(ActualEquipmentMode mode)
    {
        equipmentMode_.store(mode);
        if (mode != ActualEquipmentMode::Off) return;
        std::uint64_t epoch;
        { std::lock_guard lock(mutex_); epoch = epoch_; }
        SKSE::GetTaskInterface()->AddTask([this, epoch]
        {
            // 旧世界任务和关闭后重新开启的任务都不能清理当前搭配。
            if (!Current(epoch) || EquipmentMode() != ActualEquipmentMode::Off) return;
            if (auto* player = RE::PlayerCharacter::GetSingleton(); player && player->Is3DLoaded())
            {
                const auto error = ClearTemporaryEquipment();
                if (!error.empty())
                {
                    std::lock_guard lock(mutex_);
                    status_ = error;
                    SKSE::log::warn("{}", error);
                }
            }
        });
    }

    void SosBackend::Request(std::vector<std::string> keys, bool clear)
    {
        {
            std::lock_guard lock(mutex_);
            pending_ = std::move(keys); pendingClear_ = clear; hasPending_ = true;
            status_ = "SOS request queued";
        }
        SKSE::GetTaskInterface()->AddTask([this] { StartNext(); });
    }

    void SosBackend::StartNext()
    {
        auto operation = std::make_shared<Operation>();
        std::vector<std::string> keys;
        {
            std::lock_guard lock(mutex_);
            if (busy_ || !hasPending_) return;
            busy_ = true; hasPending_ = false;
            keys = std::move(pending_); operation->clear = pendingClear_; operation->epoch = epoch_;
            status_ = "Waiting for SOS callbacks";
        }
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
            const auto error = ClearTemporaryEquipment();
            if (!error.empty()) { Finish(operation->epoch, error); return; }
        }
        operation->Next();
    }

    void SosBackend::Finish(std::uint64_t epoch, std::string message)
    {
        {
            std::lock_guard lock(mutex_);
            if (epoch != epoch_) return;
            status_ = std::move(message); busy_ = false;
            SKSE::log::info("{}", status_);
        }
        StartNext();
    }

    bool SosBackend::Current(std::uint64_t epoch) const { std::lock_guard lock(mutex_); return epoch == epoch_; }
    std::string SosBackend::Status() const { std::lock_guard lock(mutex_); return status_; }
    void SosBackend::Reset()
    {
        std::lock_guard lock(mutex_);
        ++epoch_; busy_ = false; hasPending_ = false; pending_.clear(); status_ = "SOS idle";
    }
}
