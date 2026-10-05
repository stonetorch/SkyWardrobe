#include "applications/execution/ExecutionPolicy.h"
#include "applications/execution/ExecutionScene.h"
#include <limits>

using namespace CAF::Execution;

// 血量边界严格低于阈值；倒地与低血量独立，关闭两个条件时没有资格。
static_assert([]
{
    Settings settings;
    if (!Vulnerable(settings, 19, 100, false)) return false;
    if (Vulnerable(settings, 20, 100, false)) return false;
    if (Vulnerable(settings, 19, 0, false)) return false;
    if (!Vulnerable(settings, 100, 100, true)) return false;
    settings.lowHealth = false;
    if (Vulnerable(settings, 1, 100, false)) return false;
    if (!Vulnerable(settings, 100, 0, true)) return false;
    settings.bleedout = false;
    return !Vulnerable(settings, 0, 100, true);
}());

int main()
{
    if (!InRange(40000, 200) || InRange(40001, 200) || InRange(-1, 200) ||
        InRange(std::numeric_limits<float>::quiet_NaN(), 200) || InRange(0, 0)) return 1;
    using CAF::Scene::Document;
    // 场景池必须有两个 PLAY；禁止效果目标引用第三个槽位，允许注释中的草稿。
    if (!ValidateScene(Document::Parse("ONCE\n1 PLAY player.hkx\n2 PLAY enemy.hkx\nAT 1 DEATH 2\n")).empty()) return 2;
    if (ValidateScene(Document::Parse("1 PLAY player.hkx\n")).empty()) return 3;
    if (ValidateScene(Document::Parse("1 PLAY player.hkx\n2 PLAY enemy.hkx\n3 PLAY other.hkx\n")).empty()) return 4;
    if (ValidateScene(Document::Parse("1 PLAY player.hkx\n2 PLAY enemy.hkx\nAT 1 DEATH 3\n")).empty()) return 5;
    if (!ValidateScene(Document::Parse("1 PLAY player.hkx\n2 PLAY enemy.hkx\n; 3 PLAY draft.hkx\n")).empty()) return 6;
    if (ValidateScene(Document::Parse("1 PLAY\n2 PLAY enemy.hkx\n")).empty()) return 7;
    return 0;
}
