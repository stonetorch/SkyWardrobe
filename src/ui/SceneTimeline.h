#pragma once
#include "scene/SceneDocument.h"

namespace CAF::SceneEditor
{
    struct TimelineResult
    {
        bool seek = false;
        bool togglePlayback = false;
        float time = 0;
        int selectedLine = -1;
    };

    // 绘制与交互集中在 UI 模块；调用者只接收寻址、选行和播放切换意图。
    TimelineResult DrawTimeline(const Scene::Document& document, float time, float duration, bool playing, std::size_t begin, std::size_t end);
}
