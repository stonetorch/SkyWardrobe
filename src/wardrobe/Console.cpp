#include "wardrobe/Service.h"
#include "wardrobe/Panel.h"
#include "ui/Overlay.h"
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <array>

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
        // 复用引擎保留的未使用命令槽，不扩展引擎固定长度的命令表。
        auto* table = RE::SCRIPT_FUNCTION::GetFirstConsoleCommand();
        RE::SCRIPT_FUNCTION* slot = nullptr;
        for (std::uint32_t i = 0; i < RE::SCRIPT_FUNCTION::Commands::kConsoleCommandsEnd; ++i)
        {
            if (table[i].functionName && Normalize(table[i].functionName).starts_with("unused")) { slot = &table[i]; break; }
        }
        // SetDebugText 是引擎遗留的调试命令；保留其 opcode 和编译函数以适配 SE / AE。
        if (!slot) slot = RE::SCRIPT_FUNCTION::LocateConsoleCommand("SetDebugText");
        if (!slot)
        {
            SKSE::log::error("No reserved console command slot is available"); return;
        }
        auto replacement = *slot;
        replacement.functionName = "wardrobe";
        replacement.shortName = "sw";
        replacement.helpString = "SkyWardrobe: wardrobe [open|close|reload|clear|preset \"name\"]";
        replacement.referenceFunction = false;
        replacement.executeFunction = Execute;
        replacement.conditionFunction = nullptr;
        replacement.SetParameters(parameters);
        // 未使用命令可能没有通用参数编译器，取 vanilla 字符串命令的编译入口。
        if (const auto* donor = RE::SCRIPT_FUNCTION::LocateConsoleCommand("SetINISetting")) replacement.compileFunction = donor->compileFunction;
        REL::safe_write(reinterpret_cast<std::uintptr_t>(slot), &replacement, sizeof(replacement));
        SKSE::log::info("Registered console command wardrobe (sw)");
    }
}
