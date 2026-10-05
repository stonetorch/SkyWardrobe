#include "scene/SessionInputPolicy.h"
#include <iostream>
#include <stdexcept>

using namespace CAF::Scene;

void Check(bool condition)
{
    if (!condition) throw std::runtime_error("Session input policy regression failed");
}

int main()
{
    Check(SessionHotkeysAllowed(SessionPhase::Playing, true, false, true, true, false, false, false));
    for (auto phase : {SessionPhase::Idle, SessionPhase::Preparing, SessionPhase::Finishing})
        Check(!SessionHotkeysAllowed(phase, true, false, true, true, false, false, false));
    // 每个资格单独撤销，防止预览、未包含玩家或采样时意外接管相机/时间。
    for (int blocked = 0; blocked < 7; ++blocked)
        Check(!SessionHotkeysAllowed(SessionPhase::Playing, blocked != 0, blocked == 1, blocked != 2,
            blocked != 3, blocked == 4, blocked == 5, blocked == 6));

    TimeFreezeLease lease;
    bool freezeTime = false;
    lease.Toggle(freezeTime);
    Check(lease.Active() && freezeTime);
    // 原生相机切换清除标记后，冻结租约仍可重新施加。
    freezeTime = false;
    lease.Refresh(freezeTime);
    Check(freezeTime);
    lease.Toggle(freezeTime);
    Check(!lease.Active() && !freezeTime);
    lease.Restore(freezeTime);
    Check(!freezeTime);

    // 不解除进入 CAF 冻结之前就存在的冻结状态。
    freezeTime = true;
    lease.Toggle(freezeTime);
    freezeTime = false; // 原生 freecam 退出也可能改写这个标记。
    lease.Restore(freezeTime);
    Check(!lease.Active() && freezeTime);
    freezeTime = false;
    lease.Toggle(freezeTime);
    lease.Restore(freezeTime); // 场景停止时无需等待第二次按 F。
    Check(!lease.Active() && !freezeTime);
    std::cout << "Session hotkey eligibility and time freeze lease checks passed\n";
}
