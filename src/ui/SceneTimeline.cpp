#include "ui/Text.h"
#include "ui/SceneTimeline.h"
#include "ui/TimelineMath.h"
#include <imgui.h>
#include <cmath>
#include <cstdio>

namespace CAF::SceneEditor
{
    TimelineResult DrawTimeline(const Scene::Document& document, float time, float duration, bool playing, std::size_t begin, std::size_t end)
    {
        TimelineResult result;
        static float step = 0.01f;
        static float zoom = 1;
        static float center = 0;
        static float dragTime = 0;
        static bool dragging = false;
        static float exact = 0;
        static bool exactActive = false;
        if (ImGui::Button(playing ? CAF::Ui::Text::kPausePreview : CAF::Ui::Text::kPlayPreview)) result.togglePlayback = true;
        ImGui::SameLine();
        if (ImGui::Button("|<")) { result.seek = true; result.time = 0; }
        ImGui::SameLine();
        if (ImGui::Button(CAF::Ui::Text::kStep)) { result.seek = true; result.time = StepTime(time, -step, duration); }
        ImGui::SameLine();
        if (ImGui::Button(CAF::Ui::Text::kStep2)) { result.seek = true; result.time = StepTime(time, step, duration); }
        ImGui::SameLine();
        if (ImGui::Button(">|")) { result.seek = true; result.time = duration; }
        ImGui::SetNextItemWidth(100);
        ImGui::InputFloat(CAF::Ui::Text::kStepSeconds, &step, 0, 0, "%.4f");
        step = std::clamp(step, 0.0001f, 10.0f);
        ImGui::SameLine();
        if (!exactActive) exact = time;
        ImGui::SetNextItemWidth(120);
        // 数值控件禁止 EnterReturnsTrue；编辑期间保留草稿，回车或失焦后提交寻址。
        ImGui::InputFloat(CAF::Ui::Text::kPositionSeconds, &exact, 0, 0, "%.4f");
        exactActive = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit() && std::isfinite(exact))
        { result.seek = true; result.time = StepTime(exact, 0, duration); }
        ImGui::SetNextItemWidth(180);
        if (ImGui::SliderFloat(CAF::Ui::Text::kTimelineZoom, &zoom, 1, 100, "%.1fx", ImGuiSliderFlags_AlwaysClamp)) center = time;
        const float span = duration / zoom;
        if (zoom > 1)
        {
            ImGui::SameLine();
            if (ImGui::Button(CAF::Ui::Text::kCenterOnCursor)) center = time;
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat(CAF::Ui::Text::kViewCenter, &center, 0, duration, "%.3f s", ImGuiSliderFlags_AlwaysClamp);
        }
        ImGui::TextDisabled(CAF::Ui::Text::kCtrlClickASliderToTypeANumberPress);
        const float first = std::clamp(center-span/2, 0.0f, std::max(0.0f, duration-span));
        const float last = first+span;
        const auto origin = ImGui::GetCursorScreenPos();
        const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
        ImGui::InvisibleButton("##scene_timeline", ImVec2(width, 92));
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        const auto& io = ImGui::GetIO();
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin, ImVec2(origin.x+width, origin.y+92), IM_COL32(28,32,40,255), 3);
        const auto xAt = [&](float value) { return origin.x+(value-first)/std::max(span, 0.00001f)*width; };
        for (int i = 0; i <= 10; ++i)
        {
            const float value = first+span*i/10;
            const float x = origin.x+width*i/10;
            draw->AddLine(ImVec2(x, origin.y+22), ImVec2(x, origin.y+86), IM_COL32(70,75,85,180));
            char label[32]; std::snprintf(label, sizeof(label), "%.3f", value);
            draw->AddText(ImVec2(std::clamp(x-18, origin.x, origin.x+std::max(0.0f,width-48)),origin.y+3), IM_COL32(190,195,205,255), label);
        }
        int hit = -1;
        float hitTime = 0, best = 8;
        draw->PushClipRect(origin, ImVec2(origin.x+width,origin.y+92), true);
        for (int i = static_cast<int>(begin); i < static_cast<int>(std::min(end, document.lines.size())); ++i)
        {
            const auto& event = document.lines[i].statement;
            if (event.trigger == Scene::Trigger::Immediate) continue;
            const bool lifecycle = event.trigger == Scene::Trigger::End || event.trigger == Scene::Trigger::Exit;
            const bool predicted = !lifecycle && event.kind == Scene::Kind::Predict;
            const auto color = predicted ? IM_COL32(255,205,70,255) : IM_COL32(80,200,255,255);
            if (predicted) draw->AddLine(ImVec2(xAt(static_cast<float>(event.time)),origin.y+57), ImVec2(xAt(static_cast<float>(event.prediction)),origin.y+57),color,2);
            for (int endpoint = 0; endpoint < (predicted ? 2 : 1); ++endpoint)
            {
                const float value = lifecycle ? duration : static_cast<float>(endpoint ? event.prediction : event.time);
                if (value < first || value > last) continue;
                const ImVec2 point(xAt(value),origin.y+(event.trigger == Scene::Trigger::Exit ? 81 : endpoint ? 69 : 45));
                if (endpoint) draw->AddCircle(point,5,color); else draw->AddCircleFilled(point,5,color);
                const float distance = std::hypot(io.MousePos.x-point.x, io.MousePos.y-point.y);
                if (hovered && distance < best) { best = distance; hit = i; hitTime = value; }
            }
        }
        const float shown = dragging ? dragTime : time;
        if (shown >= first && shown <= last)
            draw->AddLine(ImVec2(xAt(shown),origin.y+22),ImVec2(xAt(shown),origin.y+91),IM_COL32(255,100,90,255),2);
        draw->PopClipRect();
        if (hit >= 0) ImGui::SetTooltip(CAF::Ui::Text::kLineDFS,hit+1,hitTime);
        // 按住鼠标持续寻址；Shift 将拖动位移缩小到十分之一。
        if (active && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            dragging = true;
            dragTime = hit >= 0 ? hitTime : TimelineTime(io.MousePos.x,origin.x,width,first,last);
            result.selectedLine = hit;
            result.seek = true; result.time = dragTime;
        }
        else if (active && ImGui::IsMouseDown(ImGuiMouseButton_Left) && dragging && io.MouseDelta.x != 0)
        {
            dragTime = StepTime(dragTime, io.MouseDelta.x/width*span*(io.KeyShift ? 0.1f : 1.0f), duration);
            result.seek = true; result.time = dragTime;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) dragging = false;
        ImGui::TextDisabled(CAF::Ui::Text::kBlueEventYellowPredictiveTriggerContactRedPositionShift);
        return result;
    }
}
