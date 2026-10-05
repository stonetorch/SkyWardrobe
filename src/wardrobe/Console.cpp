#include "wardrobe/Service.h"
#include "wardrobe/Panel.h"
#include "ui/Overlay.h"
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <algorithm>
#include <array>
#include <vector>

namespace Wardrobe
{
    namespace
    {
        RE::SCRIPT_PARAMETER parameters[] = {
            { "action", RE::SCRIPT_PARAM_TYPE::kChar, true },
            { "preset", RE::SCRIPT_PARAM_TYPE::kChar, true }
        };

        bool Execute(const RE::SCRIPT_PARAMETER* params, RE::SCRIPT_FUNCTION::ScriptData* data,
            RE::TESObjectREFR* self, RE::TESObjectREFR* containing, RE::Script* script,
            RE::ScriptLocals* locals, double& result, std::uint32_t& offset)
        {
            // 引擎字符串块长度为 uint16；缓冲区覆盖完整长度，避免长参数溢出。
            std::array<char, 65536> action{}, argument{};
            if (!RE::Script::ParseParameters(params, data, offset, self, containing, script, locals, action.data(), argument.data())) return false;
            const auto command = Normalize(action.data());
            if (command.empty() || command == "open") Overlay::Open();
            else if (command == "close") Overlay::CloseAllMenus();
            else if (command == "reload" || command == "clear" || command == "preset") QueueUiCommand(command, argument.data());
            else if (auto* console = RE::ConsoleLog::GetSingleton()) console->Print("wardrobe [open|close|reload|clear|preset \"name or ID\"]");
            result = 1;
            return true;
        }
    }

    void InstallConsole()
    {
        // 每个别名占用一个保留槽位，共用同一个参数解析与执行入口。
        auto* table = RE::SCRIPT_FUNCTION::GetFirstConsoleCommand();
        std::vector<RE::SCRIPT_FUNCTION*> slots;
        for (std::uint32_t i = 0; i < RE::SCRIPT_FUNCTION::Commands::kConsoleCommandsEnd; ++i)
        {
            if (table[i].functionName && Normalize(table[i].functionName).starts_with("unused"))
                slots.push_back(&table[i]);
        }
        // 某些版本保留槽不足时，兼容一个遗留调试命令。
        auto* debugSlot = RE::SCRIPT_FUNCTION::LocateConsoleCommand("SetDebugText");
        if (slots.size() < 3 && debugSlot && std::find(slots.begin(), slots.end(), debugSlot) == slots.end())
            slots.push_back(debugSlot);
        if (slots.empty())
        {
            SKSE::log::error("No reserved console command slot is available"); return;
        }
        // 未使用命令可能没有通用参数编译器，取 vanilla 字符串命令的编译入口。
        const auto* donor = RE::SCRIPT_FUNCTION::LocateConsoleCommand("SetINISetting");
        if (!donor || !donor->compileFunction)
        {
            SKSE::log::error("Cannot find a console string-parameter compiler"); return;
        }

        constexpr std::array<const char*, 3> aliases{ "wardrobe", "wa", "sa" };
        for (std::size_t i = 0; i < std::min(slots.size(), aliases.size()); ++i)
        {
            auto replacement = *slots[i];
            replacement.functionName = aliases[i];
            replacement.shortName = i == 0 ? "sw" : nullptr;
            replacement.helpString = "SkyWardrobe: [open|close|reload|clear|preset \"name\"]";
            replacement.referenceFunction = false;
            replacement.executeFunction = Execute;
            replacement.compileFunction = donor->compileFunction;
            replacement.conditionFunction = nullptr;
            replacement.SetParameters(parameters);
            REL::safe_write(reinterpret_cast<std::uintptr_t>(slots[i]), &replacement, sizeof(replacement));
            SKSE::log::info("Registered console command {}", aliases[i]);
        }
        if (slots.size() < aliases.size())
            SKSE::log::warn("Only registered {} of {} wardrobe console aliases", slots.size(), aliases.size());
    }
}
