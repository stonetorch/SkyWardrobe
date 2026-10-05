#include "effects/TimeScaleDuration.h"
#include <iostream>
#include <limits>
#include <stdexcept>

void Check(bool condition)
{
    if (!condition) throw std::runtime_error("Time scale duration regression failed");
}

int main()
{
    CAF::TimeScaleDuration countdown;
    Check(CAF::ValidTimeScaleDuration(0) && CAF::ValidTimeScaleDuration(3600));
    Check(!CAF::ValidTimeScaleDuration(-1) && !CAF::ValidTimeScaleDuration(3601));
    Check(!CAF::ValidTimeScaleDuration(std::numeric_limits<double>::infinity()));

    // 每现实秒只推进 0.2 游戏秒；第 5 秒到期，且仅报告一次到期。
    countdown.Set(1);
    for (int i = 0; i < 4; ++i) Check(!countdown.Advance(0.2f));
    Check(countdown.Advance(0.2f));
    Check(!countdown.Advance(1));

    countdown.Set(1);
    Check(!countdown.Advance(0.25));
    // F 冻结与菜单暂停不消耗时间；长帧可以跨过恢复边界。
    Check(!countdown.Advance(0));
    Check(!countdown.Advance(-1));
    Check(!countdown.Advance(std::numeric_limits<double>::quiet_NaN()));
    Check(countdown.Advance(0.75));

    // 新事件替换旧倒计时；无 DURATION 的事件取消自动恢复。
    countdown.Set(10);
    Check(!countdown.Advance(1));
    countdown.Set(0.5);
    Check(countdown.Advance(0.5));
    countdown.Set(10);
    countdown.Set(0);
    Check(!countdown.Advance(100));
    countdown.Set(1);
    countdown.Reset();
    Check(!countdown.Advance(100));
    std::cout << "Time scale duration, replacement and freeze checks passed\n";
}
