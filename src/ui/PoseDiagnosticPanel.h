#include "ui/Text.h"
#pragma once
#include "runtime/Runtime.h"
#include <imgui.h>

namespace CAF::Ui
{
    inline void DrawPoseDiagnostics(const Snapshot& snapshot)
    {
        if (!ImGui::CollapsingHeader(CAF::Ui::Text::kPoseAlignmentDiagnostics)) return;
        bool enabled = snapshot.poseDiagnostics;
        if (ImGui::Checkbox(CAF::Ui::Text::kTraceRealSkeletonAndPoseCoverage, &enabled))
        {
            Command command; command.action = Action::SetPoseDiagnostics; command.value = enabled ? 1.0f : 0.0f;
            Enqueue(std::move(command));
        }
        ImGui::TextWrapped(CAF::Ui::Text::kEnableBeforePreviewPlayOrPlayerHitPlaybackCAF);
        bool compare = snapshot.samplingComparisonArmed;
        ImGui::BeginDisabled(snapshot.scenePlaying);
        if (ImGui::Checkbox(CAF::Ui::Text::kCompareSamplingOnNextPlayHit, &compare))
        {
            Command command; command.action = Action::ArmSamplingComparison; command.value = compare ? 1.0f : 0.0f;
            Enqueue(std::move(command));
        }
        ImGui::EndDisabled();
        ImGui::TextWrapped(CAF::Ui::Text::kOneShotTestUsingTheFirstPredictiveAlignmentPlay);
        if (!snapshot.samplingComparisonStage.empty()) ImGui::Text(CAF::Ui::Text::kSamplingComparisonS, snapshot.samplingComparisonStage.c_str());
        ImGui::BeginDisabled(!snapshot.poseDiagnostics || !snapshot.scenePlaying);
        if (ImGui::Button(CAF::Ui::Text::kDumpCurrentPoseHierarchy))
        {
            Command command; command.action = Action::DumpPoseDiagnostics; Enqueue(std::move(command));
        }
        ImGui::EndDisabled();
        static int slot = 1;
        ImGui::SliderInt(CAF::Ui::Text::kRigidProbeSceneSlot, &slot, 1, 8);
        ImGui::BeginDisabled(!snapshot.poseDiagnostics || !snapshot.scenePreview || snapshot.sceneSeeking || snapshot.previewPlaying || snapshot.poseProbeActive);
        if (ImGui::Button(CAF::Ui::Text::kProbeTranslationHeadingThenRestore))
        {
            Command command; command.action = Action::ProbePoseRigidity;
            command.sceneActor = true; command.slot = slot - 1; Enqueue(std::move(command));
        }
        ImGui::EndDisabled();
        ImGui::TextWrapped(CAF::Ui::Text::kRunOnlyInASettledStoppedPreviewTemporarilyMoves);
        if (snapshot.poseProbeActive) ImGui::TextUnformatted(CAF::Ui::Text::kRigidProbeRunning);
        for (const auto& line : snapshot.poseDiagnosticsStatus) ImGui::TextWrapped("%s", line.c_str());
    }
}
