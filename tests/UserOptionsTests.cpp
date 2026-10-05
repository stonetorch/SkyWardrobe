#include "scene/UserOptions.h"

using namespace CAF::Scene;

constexpr bool VerifyUserOptions()
{
    UserOptions options;
    options.Upsert({"first", "Continue", "next", false, false});
    options.Upsert({"second", "Exit", "exit", true, true});
    if (options.Visible(false).size() != 1 || options.Visible(true).size() != 2) return false;
    options.Move(0, true);
    if (options.hovered != "first") return false;
    options.selected = options.hovered;
    if (options.Transition(true, false) || options.Transition(false, true)) return false;
    if (options.Transition(true, true) != "next") return false;

    // 循环重发保持展示顺序和选择身份，跳转读取最后写入的参数。
    for (int i = 0; i < 8; ++i)
        options.Upsert({"first", "Updated", "updated", true, false});
    if (options.items.size() != 2 || options.items[0].id != "first" || options.items[1].id != "second") return false;
    if (options.hovered != "first" || options.selected != "first" || options.items[0].text != "Updated") return false;
    if (options.Transition(false, false) || options.Transition(false, true) != "updated") return false;
    options.Upsert({"first", "Now", "immediate", true, true});
    if (options.Transition(false, false) != "immediate") return false;

    options.Move(-1, false);
    if (options.hovered != "second" || options.selected != "first") return false;
    options.Move(1, false);
    if (options.hovered != "first") return false;
    options.Upsert({"first", "Hidden", "hidden", false, false});
    options.Move(0, false);
    if (options.hovered != "second" || options.Transition(false, true)) return false;
    options.Upsert({"second", "Hidden too", "exit", false, true});
    options.Move(0, false);
    if (!options.hovered.empty() || !options.Visible(false).empty()) return false;

    options = {};
    return options.items.empty() && options.hovered.empty() && options.selected.empty();
}

static_assert(VerifyUserOptions());

int main() {}
