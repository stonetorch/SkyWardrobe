#include "scene/SessionLifecycle.h"

using CAF::Scene::SessionLifecycle;
using CAF::Scene::SessionPhase;

// 准备完成前不进入播放；播放中切换片段会重新准备，且不能重复开始会话。
constexpr bool TransitionPreservesSession()
{
    SessionLifecycle session;
    if (session.IsActive() || session.PrepareFragment() || session.BeginFinish()) return false;
    if (!session.Begin() || session.Begin() || session.Phase() != SessionPhase::Preparing) return false;
    session.MarkPlaying();
    if (session.Phase() != SessionPhase::Playing) return false;
    if (!session.PrepareFragment() || !session.IsActive()) return false;
    session.MarkPlaying();
    if (!session.BeginFinish() || session.BeginFinish() || session.PrepareFragment()) return false;
    session.MarkPlaying();
    if (!session.IsFinishing()) return false;
    session.Finish();
    return !session.IsActive() && session.Begin();
}

static_assert(TransitionPreservesSession());

// 取消准备和重复清理都能回到 Idle，不允许停止过程中重新播放。
constexpr bool CancelPreparation()
{
    SessionLifecycle session;
    if (!session.Begin() || !session.BeginFinish()) return false;
    session.MarkPlaying();
    if (session.Phase() != SessionPhase::Finishing) return false;
    session.Finish();
    session.Finish();
    session.MarkPlaying();
    return session.Phase() == SessionPhase::Idle && !session.IsActive();
}

static_assert(CancelPreparation());
int main() { return 0; }
