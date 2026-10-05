#include "ui/Text.h"
#include "ui/Overlay.h"
#include "config/PlaybackSettings.h"
#include "applications/execution/Execution.h"
#include "ui/PoseDiagnosticPanel.h"
#include "runtime/Runtime.h"
#include "facial/FacialControl.h"
#include "ui/UiTabs.h"
#include "ui/SceneEditor.h"
#include "scene/SceneMath.h"
#include "scene/SceneValidation.h"
#include "scene/SceneFile.h"
#include "ui/IconsFontAwesome6.h"

#include <imgui.h>
#include <Windows.h>
#include <locale>

#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <string>
#include <vector>

namespace
{
    // 拆成标签页后，原本 DrawDebug 内的局部 static 提升到这里，供各页面共享。
    struct PanelState
    {
        CAF::Snapshot snapshot;         // 每帧刷新一次的运行时快照
        std::vector<std::string> paths; // 启动时扫描到的 HKX 目录

        int slot = 0;                   // 当前编辑的 actor 槽位（A/B）
        char form[16] = "";
        char event[128] = "IdleForceDefaultState";
        char variable[128] = "bAnimationDriven";
        char bone[128] = "NPC R Hand [RHnd]";
        char nodes[2][128] = { "CAF_ControlSlot", "CAF_ControlSlot" };
        int selectedPaths[2]{};
        std::string observedPaths[2];
        bool showVanillaClips = true;
        float value = 0;
        float seek = 0;
        float offset[3]{ 80, 0, 0 };
        float position[3]{};
        float rotation[3]{};
        float cameraPosition[3]{};
        float cameraRotation[3]{};
        std::array<CAF::Animation::Settings, 2> animations;
        std::array<std::string, 2> scenePaths;
        std::vector<CAF::Command::Alignment> alignments;
        float sceneSeek = 0;
        bool sceneSeekDragging = false;
        std::string sceneFileStatus;
        char sceneName[64] = "default";
        std::string hitSceneName;
        std::string hitSceneStatus;
        bool showBones = false;
        int pickMarker = -1;
        int editingMarker = -1;
        bool pickMovingBone = true;
    };
    PanelState panel;
    CAF::Ui::TabState debugTabs;

    // 命令同时携带 A/B 两份动画设置，由游戏线程按槽位取用。
    void Send(CAF::Action action, std::string text = {}, float number = 0)
    {
        CAF::Command command;
        command.action = action; command.slot = panel.slot; command.text = std::move(text); command.value = number;
        for (int i = 0; i < 2; ++i)
        {
            command.pairAnimations[i] = panel.animations[i]; command.pairAnimations[i].node = panel.nodes[i];
            const auto selected = panel.selectedPaths[i];
            if (selected > 0 && selected <= static_cast<int>(panel.paths.size()))
            {
                command.pairAnimations[i].path = panel.paths[selected - 1];
            }
            else if (command.pairAnimations[i].independent)
                command.pairAnimations[i].path = panel.observedPaths[i];
        }
        command.animation = command.pairAnimations[panel.slot];
        CAF::Enqueue(std::move(command));
    }

    // 按钮标签统一走 Ui::Label：图标字体缺失时自动退回纯文字。
    void ActionButton(const char* icon, const char* label, CAF::Action action)
    {
        if (ImGui::Button(CAF::Ui::Label(icon, label).c_str()))
        {
            Send(action);
        }
    }

    // 带命令载荷的变体，供 Graph 页读写变量等需要额外参数的按钮使用。
    void ActionButton(const char* icon, const char* label, CAF::Action action, std::string text, float number = 0)
    {
        if (ImGui::Button(CAF::Ui::Label(icon, label).c_str()))
        {
            Send(action, std::move(text), number);
        }
    }

    std::string SceneHKXPath(int slot)
    {
        if (!panel.scenePaths[slot].empty()) return panel.scenePaths[slot];
        const auto selected = panel.selectedPaths[slot];
        if (selected > 0 && selected <= static_cast<int>(panel.paths.size())) return panel.paths[selected - 1];
        return panel.observedPaths[slot];
    }

    void SendScene(CAF::Action action, float value = 0)
    {
        CAF::Command command;
        command.action = action;
        command.value = value;
        command.alignments = panel.alignments;
        for (int i = 0; i < 2; ++i)
        {
            command.pairAnimations[i] = panel.animations[i];
            command.pairAnimations[i].path = SceneHKXPath(i);
        }
        CAF::Enqueue(std::move(command));
    }

    std::filesystem::path ScenePath()
    {
        // 仅允许单个预设名，避免名称被解释为目录或绝对路径。
        std::string name = panel.sceneName;
        if (name.empty()) name = "default";
        for (auto& c : name)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
        return std::filesystem::path("Data/SKSE/Plugins/CustomAnimationFramework/Scenes") / (name + ".scene");
    }

    float EditTime(const CAF::Command::SceneEvent& event)
    {
        return event.kind == CAF::Command::EventKind::PredictiveAlignment ? event.predictionTime : event.time;
    }

    void SaveScene()
    {
        const std::array<std::string, 2> paths{ SceneHKXPath(0), SceneHKXPath(1) };
        if (paths[0].empty() || paths[1].empty() || panel.alignments.size() > 128)
        { panel.sceneFileStatus = CAF::Ui::Text::kSelectBothHKXPathsAtMostEvents; return; }
        for (const auto& event : panel.alignments)
        {
            if (auto error = CAF::SceneEventError(event); !error.empty())
            { panel.sceneFileStatus = CAF::Ui::Text::kCannotSave + error; return; }
        }
        if (auto error = CAF::ScenePredictionError(panel.alignments); !error.empty())
        { panel.sceneFileStatus = CAF::Ui::Text::kCannotSave + error; return; }
        std::error_code error;
        std::filesystem::create_directories(ScenePath().parent_path(), error);
        if (error) { panel.sceneFileStatus = error.message(); return; }
        // 同目录临时文件写完并关闭成功后才替换旧预设，失败保留旧文件。
        const auto path = ScenePath();
        const auto temporary = std::filesystem::path(path.wstring() + L".tmp");
        std::ofstream file(temporary, std::ios::trunc);
        file.imbue(std::locale::classic());
        file << "CAF_SCENE 4\n";
        file << std::setprecision(std::numeric_limits<float>::max_digits10);
        for (const auto& path : paths) file << std::quoted(path) << '\n';
        file << panel.alignments.size() << '\n';
        for (const auto& marker : panel.alignments)
            file << static_cast<int>(marker.kind) << ' ' << std::quoted(marker.resource) << ' ' << marker.time << ' ' << marker.source << ' ' << marker.target << ' '
                 << std::quoted(marker.sourceBone) << ' ' << std::quoted(marker.targetBone) << ' '
                 << marker.offset.x << ' ' << marker.offset.y << ' ' << marker.offset.z << ' '
                 << marker.rotation.x << ' ' << marker.rotation.y << ' ' << marker.rotation.z << ' '
                 << marker.bindingID << ' ' << marker.bindingMode << ' ' << marker.duration << ' ' << marker.predictionTime << '\n';
        file.close();
        if (!file) { panel.sceneFileStatus = CAF::Ui::Text::kSceneWriteFailedPreviousPresetPreserved; return; }
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            panel.sceneFileStatus = std::format(CAF::Ui::Text::kSceneReplaceFailedWindowsPreviousPresetPreserved, GetLastError());
            return;
        }
        panel.sceneFileStatus = CAF::Ui::Text::kSceneSaved + path.filename().string();
    }

    void LoadScene()
    {
        auto path = ScenePath();
        std::error_code error;
        if (!std::filesystem::exists(path, error) && std::string(panel.sceneName) == "default")
            path = "Data/SKSE/Plugins/CustomAnimationFramework.scene";
        const auto size = std::filesystem::file_size(path, error);
        if (error || size > 1024 * 1024) { panel.sceneFileStatus = CAF::Ui::Text::kPresetMissingOrExceedsMiB; return; }
        std::ifstream file(path);
        file.imbue(std::locale::classic());
        std::string magic;
        int version = 0;
        std::array<std::string, 2> paths;
        std::size_t count = 0;
        if (!(file >> magic >> version) || magic != "CAF_SCENE" || (version < 1 || version > 4) ||
            !(file >> std::quoted(paths[0]) >> std::quoted(paths[1]) >> count) || count > 128)
        { panel.sceneFileStatus = CAF::Ui::Text::kSceneFileMissingOrInvalid; return; }
        std::vector<CAF::Command::Alignment> markers;
        for (std::size_t i = 0; i < count; ++i)
        {
            CAF::Command::Alignment marker;
            int kind = 0;
            if (version >= 2 && (!(file >> kind >> std::quoted(marker.resource)) || kind < 0 || kind > (version == 2 ? 2 : (version == 3 ? 4 : 5))))
            { panel.sceneFileStatus = CAF::Ui::Text::kSceneEventTypeIsInvalid; return; }
            marker.kind = static_cast<CAF::Command::EventKind>(kind);
            if (marker.kind == CAF::Command::EventKind::Sound && !CAF::SceneMath::ParseFormKey(marker.resource))
            { panel.sceneFileStatus = CAF::Ui::Text::kSoundKeyMustBePluginEsmLocalHexFormID; return; }
            if (!(file >> marker.time >> marker.source >> marker.target >> std::quoted(marker.sourceBone) >> std::quoted(marker.targetBone)
                >> marker.offset.x >> marker.offset.y >> marker.offset.z
                >> marker.rotation.x >> marker.rotation.y >> marker.rotation.z) ||
                (!std::isfinite(marker.time) || marker.time < 0 || marker.time > 3600) ||
                !std::isfinite(marker.offset.x) || !std::isfinite(marker.offset.y) || !std::isfinite(marker.offset.z) ||
                !std::isfinite(marker.rotation.x) || !std::isfinite(marker.rotation.y) || !std::isfinite(marker.rotation.z) || marker.source < 0 || marker.source > 1 || marker.target != 1 - marker.source)
            { panel.sceneFileStatus = CAF::Ui::Text::kSceneMarkerIsInvalid; return; }
            if (version >= 3 && !(file >> marker.bindingID >> marker.bindingMode >> marker.duration))
            { panel.sceneFileStatus = CAF::Ui::Text::kBindingPayloadIsInvalid; return; }
            if (version >= 4 && !(file >> marker.predictionTime))
            { panel.sceneFileStatus = CAF::Ui::Text::kPredictionPayloadIsInvalid; return; }
            if (auto error = CAF::SceneEventError(marker); !error.empty())
            { panel.sceneFileStatus = CAF::Ui::Text::kInvalidEvent + error; return; }
            markers.push_back(std::move(marker));
        }
        file >> std::ws;
        if (!file.eof() || paths[0].empty() || paths[1].empty() || paths[0].size() > 1024 || paths[1].size() > 1024)
        { panel.sceneFileStatus = CAF::Ui::Text::kInvalidHKXPathsOrTrailingSceneData; return; }
        panel.scenePaths = std::move(paths);
        panel.alignments = std::move(markers);
        panel.pickMarker = panel.editingMarker = -1;
        SendScene(CAF::Action::StopScene);
        SendScene(CAF::Action::SetSceneMarkers, 1);
        panel.sceneFileStatus = CAF::Ui::Text::kSceneLoaded;
    }

    void DrawActors()
    {
        ImGui::RadioButton(CAF::Ui::Text::kActorA, &panel.slot, 0); ImGui::SameLine(); ImGui::RadioButton(CAF::Ui::Text::kActorB, &panel.slot, 1);
        ActionButton(ICON_FA_USER, CAF::Ui::Text::kUsePlayer, CAF::Action::SelectPlayer); ImGui::SameLine();
        ActionButton(ICON_FA_CROSSHAIRS, CAF::Ui::Text::kUseConsoleSelection, CAF::Action::SelectConsole);
        ImGui::InputText(CAF::Ui::Text::kActorRefFormIDHex, panel.form, sizeof(panel.form), ImGuiInputTextFlags_CharsHexadecimal);
        if (ImGui::Button(CAF::Ui::Label(ICON_FA_HASHTAG, CAF::Ui::Text::kResolveFormID).c_str()))
        {
            CAF::Command command; command.action = CAF::Action::SelectForm; command.slot = panel.slot;
            command.form = static_cast<RE::FormID>(std::strtoul(panel.form, nullptr, 16)); CAF::Enqueue(command);
        }
        for (int i = 0; i < 2; ++i)
        {
            const auto& actor = panel.snapshot.actors[i];
            ImGui::Text(CAF::Ui::Text::kCSXDSAISFrozenS, 'A' + i, actor.name.c_str(), actor.id,
                actor.loaded ? CAF::Ui::Text::kLoaded : CAF::Ui::Text::kAbsent, actor.aiEnabled ? CAF::Ui::Text::kOn : CAF::Ui::Text::kOff, actor.frozen ? CAF::Ui::Text::kYes : CAF::Ui::Text::kNo, actor.locked ? CAF::Ui::Text::kYes : CAF::Ui::Text::kNo);
            ImGui::Text(CAF::Ui::Text::kPositionFFFAnglesFFF, actor.position.x, actor.position.y, actor.position.z,
                actor.rotation.x, actor.rotation.y, actor.rotation.z);
            ImGui::Text(CAF::Ui::Text::kRestrainedS, actor.restrained ? CAF::Ui::Text::kYes : CAF::Ui::Text::kNo);
        }
    }

    void DrawControl()
    {
        ActionButton(ICON_FA_SNOWFLAKE, CAF::Ui::Text::kNPCStopCombatDisableAI, CAF::Action::FreezeNPC);
        ActionButton(ICON_FA_HAND_FIST, CAF::Ui::Text::kNPCSoftRestrainStopMovement, CAF::Action::RestrainNPC);
        ActionButton(ICON_FA_PERSON_WALKING, CAF::Ui::Text::kStopMovementOnly, CAF::Action::StopMovement);
        ActionButton(ICON_FA_SHIELD, CAF::Ui::Text::kStopCombatOnly, CAF::Action::StopCombat); ImGui::SameLine();
        ActionButton(ICON_FA_ROTATE_LEFT, CAF::Ui::Text::kRestoreSelectedActor, CAF::Action::RestoreActor);
        ActionButton(ICON_FA_SNOWFLAKE, CAF::Ui::Text::kFreezePlayerInput, CAF::Action::FreezePlayer); ImGui::SameLine();
        ActionButton(ICON_FA_ROTATE_LEFT, CAF::Ui::Text::kRestorePlayerInput, CAF::Action::RestorePlayer);
        ActionButton(ICON_FA_ROBOT, CAF::Ui::Text::kSetPlayerAIDriven, CAF::Action::DrivePlayerAI);
        ImGui::Text(CAF::Ui::Text::kPlayerInputFreezeS, panel.snapshot.playerFrozen ? CAF::Ui::Text::kActive : CAF::Ui::Text::kInactive);
        ImGui::Text(CAF::Ui::Text::kPlayerMovementControllerAIDrivenS, panel.snapshot.playerAIDriven ? CAF::Ui::Text::kYes : CAF::Ui::Text::kNo);
        ImGui::TextWrapped(CAF::Ui::Text::kAIInputFreezeDoesNotGuaranteeExclusiveGraphOwnership);
    }

    void DrawConfiguration()
    {
        ImGui::SeparatorText(CAF::Ui::Text::kPhysicsIsolationGlobal);
        ImGui::TextWrapped(CAF::Ui::Text::kTheseOptionsApplyLiveToEveryIndependentActorIncluding);
        auto options = panel.snapshot.physicsIsolation;
        bool changed = ImGui::Checkbox(CAF::Ui::Text::kEnablePhysicsIsolation, &options.enabled);
        changed |= ImGui::Checkbox(CAF::Ui::Text::kFreezeControllerSimulation, &options.simulation);
        changed |= ImGui::Checkbox(CAF::Ui::Text::kSuppressControllerGravity, &options.gravity);
        changed |= ImGui::Checkbox(CAF::Ui::Text::kSuppressSupportStepsGroundTilt, &options.support);
        changed |= ImGui::Checkbox(CAF::Ui::Text::kSuppressPhysicalPushing, &options.pushing);
        changed |= ImGui::Checkbox(CAF::Ui::Text::kFreezeRigidBodyMassGravityVelocities, &options.rigidBody);
        changed |= ImGui::Checkbox(CAF::Ui::Text::kBlockVanillaMovementDelta, &options.movement);
        changed |= ImGui::Checkbox(CAF::Ui::Text::kFilterActorToActorBodyCollisions, &options.actorCollisions);
        changed |= ImGui::Checkbox(CAF::Ui::Text::kDisableFootIKGroundAlignment, &options.footIK);
        changed |= ImGui::Checkbox(CAF::Ui::Text::kDisableHeadTracking, &options.headTracking);
        changed |= ImGui::Checkbox(CAF::Ui::Text::kFilterStaggerRecoilBlockReactions, &options.reactions);
        changed |= ImGui::Checkbox(CAF::Ui::Text::kDisablePrecisionActorPhysicsViaAPI, &options.precision);
        changed |= ImGui::Checkbox(CAF::Ui::Text::kDisableWholeActorCollisionDuringScenes, &options.sceneCollision);
        changed |= ImGui::Checkbox(CAF::Ui::Text::kAlsoIsolateCAFOriginalMotionModes, &options.includeMotionModes);
        ImGui::TextWrapped(CAF::Ui::Text::kSimulationSupportAndPushingClearCachedVelocitiesRigidBody);
        if (ImGui::Button(CAF::Ui::Text::kRestoreIsolationDefaults))
        {
            options = {};
            changed = true;
        }
        if (changed)
        {
            CAF::Command command;
            command.action = CAF::Action::SetPhysicsIsolationOptions;
            command.physicsIsolation = options;
            CAF::Enqueue(std::move(command));
        }
    }

    void DrawAnimation()
    {
        ImGui::RadioButton(CAF::Ui::Text::kActorA2, &panel.slot, 0); ImGui::SameLine();
        ImGui::RadioButton(CAF::Ui::Text::kActorB2, &panel.slot, 1);
        auto& animation = panel.animations[panel.slot];
        auto& node = panel.nodes[panel.slot];
        auto& selectedPath = panel.selectedPaths[panel.slot];
        ImGui::Text(CAF::Ui::Text::kEditingAnimationSettingsForActorC, 'A' + panel.slot);
        ImGui::Text(CAF::Ui::Text::kStartupHKXCatalogZuFileS, panel.paths.size());
        ImGui::TextWrapped(CAF::Ui::Text::kPlaceLooseHKXFilesUnderDataMeshesActorsCharacter);
        ImGui::Checkbox(CAF::Ui::Text::kIndependentPoseOutputNoBehaviorPatch, &animation.independent);
        if (!animation.independent) ImGui::InputText(CAF::Ui::Text::kExactClipNodeLegacyExperiment, node, sizeof(node));
        const char* current = selectedPath > 0 && selectedPath <= static_cast<int>(panel.paths.size()) ? panel.paths[selectedPath - 1].c_str() :
            (animation.independent ? (panel.observedPaths[panel.slot].empty() ? CAF::Ui::Text::kSelectHKXOrUseAnObservedClipBelow : panel.observedPaths[panel.slot].c_str()) : CAF::Ui::Text::kKeepCurrentBinding);
        if (ImGui::BeginCombo(CAF::Ui::Text::kAnimation, current))
        {
            if (ImGui::Selectable(animation.independent ? CAF::Ui::Text::kUseSelectedObservedPath : CAF::Ui::Text::kKeepCurrentBinding, selectedPath == 0)) selectedPath = 0;
            for (int i = 0; i < static_cast<int>(panel.paths.size()); ++i)
                if (ImGui::Selectable(panel.paths[i].c_str(), selectedPath == i + 1)) selectedPath = i + 1;
            ImGui::EndCombo();
        }
        ActionButton(ICON_FA_PLAY, CAF::Ui::Text::kPlaySelected, CAF::Action::Play); ImGui::SameLine();
        ActionButton(ICON_FA_STOP, CAF::Ui::Text::kStopSelected, CAF::Action::Stop);
        ActionButton(ICON_FA_USERS, CAF::Ui::Text::kPlayABInSameTick, CAF::Action::PlayPair);
        ImGui::TextWrapped(animation.independent ?
            CAF::Ui::Text::kTheOriginalGraphKeepsRunningCAFSamplesItsOwn :
            CAF::Ui::Text::kLegacyDiagnosticReplacesAnExistingNamedClipVanillaTransitions);
        bool changed = ImGui::SliderFloat(CAF::Ui::Text::kPlaybackSpeed, &animation.speed, 0.05f, 4.0f, "%.2fx");
        changed |= ImGui::Checkbox(CAF::Ui::Text::kPausePlayback, &animation.paused);
        changed |= ImGui::Checkbox(CAF::Ui::Text::kLoop, &animation.loop);
        if (animation.independent)
        {
            changed |= ImGui::Combo(CAF::Ui::Text::kMotionOutput, &animation.motionMode, CAF::Ui::Text::kSuppressGraphMotionUseCAFAnimationMotionKeepOriginal);
            ImGui::TextWrapped(CAF::Ui::Text::kSuppressGraphMotionAlsoAppliesPhysicsIsolationCAFOriginal);
            changed |= ImGui::SliderFloat(CAF::Ui::Text::kBlendInOutSeconds, &animation.blendSeconds, 0, 2, "%.2f");
            changed |= ImGui::Checkbox(CAF::Ui::Text::kReleaseAutomaticallyAtEnd, &animation.autoRelease);
        }
        else changed |= ImGui::Checkbox(CAF::Ui::Text::kIgnoreRootMotionExperimentalFlagX20, &animation.ignoreMotion);
        if (changed) Send(CAF::Action::Configure);
        ImGui::InputFloat(CAF::Ui::Text::kSeekSeconds, &panel.seek);
        if (ImGui::Button(CAF::Ui::Label(ICON_FA_FORWARD, animation.independent ? CAF::Ui::Text::kSeekSilently : CAF::Ui::Text::kSeekExperimental).c_str())) Send(CAF::Action::Seek, {}, panel.seek);
        ImGui::SameLine(); ActionButton(ICON_FA_FILE_EXPORT, CAF::Ui::Text::kDumpRawAnnotations, CAF::Action::DumpAnnotations);
        ImGui::TextWrapped(CAF::Ui::Text::kIndependentAnnotationsForwardNativeFootstepsAndSoundPlayToGraph);
        ImGui::Checkbox(CAF::Ui::Text::kShowVanillaClipDiagnostics, &panel.showVanillaClips);
        float times[2]{}; bool controlled[2]{};
        for (const auto& clip : panel.snapshot.clips)
        {
            if (!clip.independent && !panel.showVanillaClips) continue;
            ImGui::PushID(static_cast<int>(clip.actor)); ImGui::PushID(clip.node.c_str());
            if (!clip.independent && ImGui::SmallButton(CAF::Ui::Label(ICON_FA_ARROW_RIGHT_TO_BRACKET, (CAF::Ui::Text::kUse + clip.node).c_str()).c_str()))
            {
                if (clip.actor == panel.snapshot.actors[0].id) panel.slot = 0;
                else if (clip.actor == panel.snapshot.actors[1].id) panel.slot = 1;
                std::snprintf(panel.nodes[panel.slot], sizeof(panel.nodes[panel.slot]), "%s", clip.node.c_str());
                panel.observedPaths[panel.slot] = clip.path;
                panel.selectedPaths[panel.slot] = 0;
            }
            if (clip.independent)
            {
                ImGui::Separator();
                ImGui::TextWrapped(CAF::Ui::Text::kIndependentXS, clip.actor, CAF::Ui::Text::Diagnostic(clip.status).c_str());
                ImGui::Text(CAF::Ui::Text::kPoseWritesLluWeightFAdvancedFs, static_cast<unsigned long long>(clip.poseWrites), clip.weight, clip.elapsed);
                ImGui::Text(CAF::Ui::Text::kGenerateLluRootLluMappedDTracksDMapping,
                    static_cast<unsigned long long>(clip.generateCalls), static_cast<unsigned long long>(clip.rootGenerateCalls),
                    clip.mappedBones, clip.transformTracks, clip.mappingEntries, clip.skeletonBones);
                ImGui::TextWrapped("%s", clip.outputDetails.c_str());
            }
            ImGui::Text(CAF::Ui::Text::kXBindingUSSFFEndS, clip.actor, clip.binding,
                clip.controlled ? CAF::Ui::Text::kCONTROLLED : (clip.independent ? CAF::Ui::Text::kNotWriting : CAF::Ui::Text::kObserved), clip.loaded ? CAF::Ui::Text::kLoaded : CAF::Ui::Text::kNotLoaded, clip.time, clip.duration, clip.atEnd ? CAF::Ui::Text::kYes : CAF::Ui::Text::kNo);
            ImGui::TextWrapped("%s", clip.path.c_str());
            ImGui::Text(CAF::Ui::Text::kExtractedMotionRawHavokUnitsFFF, clip.extractedTranslation.x, clip.extractedTranslation.y, clip.extractedTranslation.z);
            for (int i = 0; i < 2; ++i) if (clip.actor == panel.snapshot.actors[i].id && clip.controlled) { times[i] = clip.time; controlled[i] = true; }
            ImGui::PopID(); ImGui::PopID();
        }
        if (controlled[0] && controlled[1]) ImGui::Text(CAF::Ui::Text::kABSampledLocalTimeDifferenceFSLoop, times[0] - times[1]);
    }

    void BonePicker(const char* label, int actorSlot, std::string& selected)
    {
        if (ImGui::BeginCombo(label, selected.empty() ? CAF::Ui::Text::kSelectBone : selected.c_str()))
        {
            const auto& skeleton = panel.snapshot.actors[actorSlot].skeleton;
            for (int i = 0; i < static_cast<int>(skeleton.size()); ++i)
            {
                ImGui::PushID(i);
                ImGui::Indent(skeleton[i].depth * 8.0f);
                if (ImGui::Selectable(skeleton[i].name.c_str(), selected == skeleton[i].name)) selected = skeleton[i].name;
                ImGui::Unindent(skeleton[i].depth * 8.0f);
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }

    void DrawProjectedBones()
    {
        if (!panel.showBones) return;
        const auto& io = ImGui::GetIO();
        auto* draw = ImGui::GetBackgroundDrawList();
        const auto origin = ImGui::GetMainViewport()->Pos;
        const bool overEditor = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow);
        int hoveredActor = -1;
        int hoveredBone = -1;
        float nearest = 100.0f;
        float nearestCamera = std::numeric_limits<float>::max();
        struct ProjectedNode { int actor; int bone; ImVec2 point; float cameraDistance; };
        std::vector<ProjectedNode> nodes;
        for (int actor = 0; actor < 2; ++actor)
        {
            const auto& bones = panel.snapshot.actors[actor].skeleton;
            const auto color = actor == 0 ? IM_COL32(80, 200, 255, 210) : IM_COL32(255, 190, 80, 210);
            for (int i = 0; i < static_cast<int>(bones.size()); ++i)
            {
                const auto& bone = bones[i];
                if (!bone.onScreen) continue;
                const ImVec2 point(origin.x + bone.screenX * io.DisplaySize.x,
                    origin.y + CAF::SceneMath::ScreenY(bone.screenY) * io.DisplaySize.y);
                if (bone.parent >= 0 && bone.parent < static_cast<int>(bones.size()) && bones[bone.parent].onScreen)
                {
                    const auto& parent = bones[bone.parent];
                    draw->AddLine(point, ImVec2(origin.x + parent.screenX * io.DisplaySize.x,
                        origin.y + CAF::SceneMath::ScreenY(parent.screenY) * io.DisplaySize.y), color, 1.0f);
                }
                nodes.push_back({ actor, i, point, bone.cameraDistanceSquared });
            }
        }
        // 远处节点先画，重叠时近处节点盖在上面并优先响应点击。
        std::sort(nodes.begin(), nodes.end(), [](const auto& a, const auto& b) { return a.cameraDistance > b.cameraDistance; });
        for (const auto& node : nodes)
        {
            draw->AddCircleFilled(node.point, 3.5f,
                node.actor == 0 ? IM_COL32(80, 200, 255, 210) : IM_COL32(255, 190, 80, 210));
            if (overEditor) continue;
            const auto dx = io.MousePos.x - node.point.x;
            const auto dy = io.MousePos.y - node.point.y;
            const auto distance = dx * dx + dy * dy;
            if (distance < nearest || (distance == nearest && node.cameraDistance < nearestCamera))
            {
                nearest = distance;
                nearestCamera = node.cameraDistance;
                hoveredActor = node.actor;
                hoveredBone = node.bone;
            }
        }
        if (hoveredActor >= 0)
        {
            const auto& bone = panel.snapshot.actors[hoveredActor].skeleton[hoveredBone];
            const ImVec2 point(origin.x + bone.screenX * io.DisplaySize.x,
                origin.y + CAF::SceneMath::ScreenY(bone.screenY) * io.DisplaySize.y);
            draw->AddCircle(point, 8.0f, IM_COL32(255, 255, 255, 255), 16, 2.0f);
            ImGui::SetTooltip("%c: %s", 'A' + hoveredActor, bone.name.c_str());
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && panel.pickMarker >= 0 &&
                panel.pickMarker < static_cast<int>(panel.alignments.size()))
            {
                auto& marker = panel.alignments[panel.pickMarker];
                const auto wantedActor = panel.pickMovingBone ? marker.source : marker.target;
                if (hoveredActor == wantedActor)
                {
                    (panel.pickMovingBone ? marker.sourceBone : marker.targetBone) = bone.name;
                    SendScene(CAF::Action::SetSceneMarkers, 1);
                    panel.pickMarker = -1;
                }
            }
        }
    }

    // 命中触发：待命后由游戏线程在玩家命中目标时播放所选预设。
    void DrawSceneTrigger()
    {
        CAF::Ui::DrawPoseDiagnostics(panel.snapshot);
        ImGui::SeparatorText(CAF::Ui::Text::kPlayerHitTrigger);
        if (ImGui::BeginCombo(CAF::Ui::Text::kHitScene, panel.hitSceneName.empty() ? CAF::Ui::Text::kSelectSavedScene : panel.hitSceneName.c_str()))
        {
            const auto files = CAF::Scene::Files();
            if (files.empty()) ImGui::TextDisabled(CAF::Ui::Text::kNoSavedScenesFound);
            for (const auto& name : files)
                if (ImGui::Selectable(name.c_str(), panel.hitSceneName == name))
                {
                    panel.hitSceneName = name;
                    panel.hitSceneStatus.clear();
                }
            ImGui::EndCombo();
        }
        ImGui::BeginDisabled(panel.hitSceneName.empty() || !panel.snapshot.worldReady);
        if (ImGui::Button(CAF::Ui::Label(ICON_FA_PLAY, CAF::Ui::Text::kPlayOnPlayerHit).c_str()))
        {
            // 播放时读取所选文件并冻结脚本，命中待命不依赖场景编辑器的草稿。
            if (auto document = CAF::Scene::Load(panel.hitSceneName, panel.hitSceneStatus))
            {
                CAF::Command command; command.action = CAF::Action::ArmHitScene;
                command.text = document->Source();
                CAF::Enqueue(std::move(command));
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!panel.snapshot.hitSceneArmed);
        ActionButton(ICON_FA_STOP, CAF::Ui::Text::kCancelWaiting, CAF::Action::StopScene);
        ImGui::EndDisabled();
        ImGui::TextWrapped(CAF::Ui::Text::kClosesAllCAFMenusAndWaitsForTheNext);
        if (panel.snapshot.hitSceneArmed) ImGui::TextUnformatted(CAF::Ui::Text::kWaitingForAPlayerHit);
        if (!panel.hitSceneStatus.empty()) ImGui::TextWrapped("%s", CAF::Ui::Text::Diagnostic(panel.hitSceneStatus).c_str());
    }

    void DrawScene()
    {
        ImGui::SeparatorText(CAF::Ui::Text::kManualABScenePlayback);
        ImGui::Checkbox(CAF::Ui::Text::kShowSkeletonInWorld, &panel.showBones);
        DrawProjectedBones();
        ImGui::TextWrapped(CAF::Ui::Text::kSelectTwoActorsOnTheActorsTabThenAssign);
        ImGui::TextWrapped(CAF::Ui::Text::kEditPreviewCameraDragOnTheWorldOutsideUI);
        for (int i = 0; i < 2; ++i)
        {
            ImGui::PushID(i);
            ImGui::Text(CAF::Ui::Text::kActorCSX, 'A' + i, panel.snapshot.actors[i].name.c_str(), panel.snapshot.actors[i].id);
            const auto path = SceneHKXPath(i);
            if (ImGui::BeginCombo("HKX", path.empty() ? CAF::Ui::Text::kSelectHKX : path.c_str()))
            {
                for (const auto& candidate : panel.paths)
                    if (ImGui::Selectable(candidate.c_str(), path == candidate)) panel.scenePaths[i] = candidate;
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button(CAF::Ui::Label(ICON_FA_BONE, CAF::Ui::Text::kReadSkeleton).c_str()))
            {
                CAF::Command command; command.action = CAF::Action::ReadSkeleton; command.slot = i;
                CAF::Enqueue(std::move(command));
            }
            ImGui::Text(CAF::Ui::Text::kZuNamedNodes, panel.snapshot.actors[i].skeleton.size());
            ImGui::PopID();
        }
        if (ImGui::Button(CAF::Ui::Label(ICON_FA_PLAY, CAF::Ui::Text::kPlayScene).c_str())) SendScene(CAF::Action::StartScene);
        ImGui::SameLine();
        if (ImGui::Button(CAF::Ui::Label(ICON_FA_PEN_TO_SQUARE, CAF::Ui::Text::kEditScenePaused).c_str())) SendScene(CAF::Action::StartScene, 1.0f);
        ImGui::SameLine();
        if (ImGui::Button(CAF::Ui::Label(ICON_FA_STOP, CAF::Ui::Text::kStopScene).c_str())) SendScene(CAF::Action::StopScene);
        ImGui::Separator();
        ImGui::BeginDisabled(!panel.snapshot.scenePreview || panel.snapshot.sceneSeeking);
        if (ImGui::Button(CAF::Ui::Label(ICON_FA_PLUS, CAF::Ui::Text::kAddAlignmentAtCurrentTime).c_str()))
        {
            if (panel.alignments.size() < 128)
            {
                CAF::Command::Alignment marker;
                marker.time = panel.snapshot.scenePlaying ? panel.snapshot.sceneTime : panel.sceneSeek;
                if (!panel.snapshot.actors[1].skeleton.empty()) marker.sourceBone = panel.snapshot.actors[1].skeleton.front().name;
                if (!panel.snapshot.actors[0].skeleton.empty()) marker.targetBone = panel.snapshot.actors[0].skeleton.front().name;
                panel.alignments.push_back(std::move(marker));
                panel.editingMarker = static_cast<int>(panel.alignments.size()) - 1;
                SendScene(CAF::Action::SetSceneMarkers, 1);
            }
            else panel.sceneFileStatus = CAF::Ui::Text::kSceneSupportsUpToEvents;
        }
        ImGui::SameLine();
        if (ImGui::Button(CAF::Ui::Label(ICON_FA_VOLUME_HIGH, CAF::Ui::Text::kAddSound).c_str()) && panel.alignments.size() < 128)
        {
            CAF::Command::SceneEvent event;
            event.kind = CAF::Command::EventKind::Sound;
            event.time = panel.snapshot.sceneTime;
            panel.alignments.push_back(event);
            panel.editingMarker = static_cast<int>(panel.alignments.size()) - 1;
        }
        ImGui::SameLine();
        if (ImGui::Button(CAF::Ui::Label(ICON_FA_SKULL, CAF::Ui::Text::kAddDeath).c_str()) && panel.alignments.size() < 128)
        {
            CAF::Command::SceneEvent event;
            event.kind = CAF::Command::EventKind::Death;
            event.time = panel.snapshot.sceneTime;
            panel.alignments.push_back(event);
            panel.editingMarker = static_cast<int>(panel.alignments.size()) - 1;
            SendScene(CAF::Action::SetSceneMarkers, 1);
        }
        if (ImGui::Button(CAF::Ui::Label(ICON_FA_WAND_MAGIC_SPARKLES, CAF::Ui::Text::kAddPredictiveAlignment).c_str()) && panel.alignments.size() < 128 && panel.snapshot.sceneTime > 0)
        {
            CAF::Command::SceneEvent event;
            event.kind = CAF::Command::EventKind::PredictiveAlignment;
            event.predictionTime = panel.snapshot.sceneTime;
            event.time = std::max(0.0f, event.predictionTime - 1.0f);
            if (!panel.snapshot.actors[1].skeleton.empty()) event.sourceBone = panel.snapshot.actors[1].skeleton.front().name;
            if (!panel.snapshot.actors[0].skeleton.empty()) event.targetBone = panel.snapshot.actors[0].skeleton.front().name;
            panel.alignments.push_back(event);
            panel.editingMarker = static_cast<int>(panel.alignments.size()) - 1;
            SendScene(CAF::Action::SetSceneMarkers, 1);
        }
        ImGui::TextWrapped(CAF::Ui::Text::kPredictiveAlignmentSeekToTheFutureContactTimeThen);
        if (ImGui::Button(CAF::Ui::Label(ICON_FA_LINK, CAF::Ui::Text::kAddArtBindingExperimental).c_str()) && panel.alignments.size() < 128)
        {
            CAF::Command::SceneEvent event;
            event.kind = CAF::Command::EventKind::ArtBind;
            event.time = panel.snapshot.sceneTime;
            event.sourceBone = "NPC R Hand [RHnd]";
            panel.alignments.push_back(event);
            panel.editingMarker = static_cast<int>(panel.alignments.size()) - 1;
        }
        ImGui::SameLine();
        if (ImGui::Button(CAF::Ui::Label(ICON_FA_LINK_SLASH, CAF::Ui::Text::kAddArtUnbind).c_str()) && panel.alignments.size() < 128)
        {
            CAF::Command::SceneEvent event;
            event.kind = CAF::Command::EventKind::ArtUnbind;
            event.time = panel.snapshot.sceneTime;
            panel.alignments.push_back(event);
            panel.editingMarker = static_cast<int>(panel.alignments.size()) - 1;
            SendScene(CAF::Action::SetSceneMarkers, 1);
        }
        ImGui::EndDisabled();
        if (auto error = CAF::ScenePredictionError(panel.alignments, panel.snapshot.sceneDuration > 0 ? panel.snapshot.sceneDuration : 3600); !error.empty())
            ImGui::TextWrapped(CAF::Ui::Text::kPredictionDraftSLastValidPreviewIsRetainedUntil, error.c_str());
        for (const auto& binding : panel.snapshot.artBindings) ImGui::TextWrapped("%s", binding.c_str());
        for (int i = 0; i < static_cast<int>(panel.alignments.size()); ++i)
        {
            auto& marker = panel.alignments[i];
            ImGui::PushID(i);
            ImGui::Separator();
            constexpr const char* kindNames[]{ CAF::Ui::Text::kAlignment, CAF::Ui::Text::kSound, CAF::Ui::Text::kDeath, CAF::Ui::Text::kArtBindingExperimental, CAF::Ui::Text::kArtUnbind, CAF::Ui::Text::kPredictiveAlignment };
            const char* kindName = kindNames[static_cast<int>(marker.kind)];
            ImGui::Text("%s %d", kindName, i + 1);
            ImGui::SameLine();
            ImGui::BeginDisabled(!panel.snapshot.scenePreview);
            if (ImGui::SmallButton(CAF::Ui::Label(ICON_FA_ARROW_RIGHT_TO_BRACKET, CAF::Ui::Text::kJumpEdit).c_str()))
            {
                panel.editingMarker = i;
                panel.pickMarker = -1;
                SendScene(CAF::Action::SeekScene, EditTime(marker));
            }
            ImGui::EndDisabled();
            // 前瞻事件在目标时刻编辑；触发时间只决定何时提前移动角色。
            const bool editable = panel.snapshot.scenePreview && panel.editingMarker == i &&
                (panel.snapshot.sceneSeeking || std::abs(panel.snapshot.sceneTime - EditTime(marker)) < 0.002f ||
                 !CAF::SceneEventError(marker).empty() ||
                 !CAF::ScenePredictionError(panel.alignments, panel.snapshot.sceneDuration > 0 ? panel.snapshot.sceneDuration : 3600).empty());
            ImGui::BeginDisabled(!panel.snapshot.scenePreview);
            if (ImGui::SmallButton(CAF::Ui::Label(ICON_FA_TRASH, CAF::Ui::Text::kDelete).c_str()))
            {
                panel.alignments.erase(panel.alignments.begin() + i);
                panel.pickMarker = -1;
                SendScene(CAF::Action::SetSceneMarkers, 1);
                panel.editingMarker = -1;
                ImGui::EndDisabled();
                ImGui::PopID();
                --i;
                continue;
            }
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!editable);
            const bool timeChanged = ImGui::DragFloat(CAF::Ui::Text::kAtSeconds, &marker.time, 0.01f, 0, std::max(0.001f, panel.snapshot.sceneDuration), "%.3f", ImGuiSliderFlags_AlwaysClamp);
            bool changed = timeChanged;
            if (marker.kind == CAF::Command::EventKind::PredictiveAlignment)
            {
                changed |= ImGui::DragFloat(CAF::Ui::Text::kPredictedContactSeconds, &marker.predictionTime, 0.01f, 0,
                    std::max(0.001f, panel.snapshot.sceneDuration), "%.3f", ImGuiSliderFlags_AlwaysClamp);
                ImGui::TextWrapped(CAF::Ui::Text::kEditingTheFutureContactPoseAtTriggerTimeThe);
            }
            int source = marker.source;
            changed |= ImGui::Combo((marker.kind == CAF::Command::EventKind::Alignment || marker.kind == CAF::Command::EventKind::PredictiveAlignment) ? CAF::Ui::Text::kMoveActor : CAF::Ui::Text::kSubjectActor, &source, "A\0B\0");
            if (source != marker.source)
            {
                marker.source = source;
                marker.target = 1 - source;
                marker.sourceBone.clear();
                marker.targetBone.clear();
            }
            if ((marker.kind == CAF::Command::EventKind::Alignment || marker.kind == CAF::Command::EventKind::PredictiveAlignment))
            {
                ImGui::Text(CAF::Ui::Text::kAlignActorCToActorC, 'A' + marker.source, 'A' + marker.target);
                const auto oldSource = marker.sourceBone;
                const auto oldTarget = marker.targetBone;
                BonePicker(CAF::Ui::Text::kMovingBone, marker.source, marker.sourceBone);
                ImGui::SameLine();
                if (ImGui::SmallButton(CAF::Ui::Label(ICON_FA_CROSSHAIRS, CAF::Ui::Text::kPickMovingInWorld).c_str())) { panel.showBones = true; panel.pickMarker = i; panel.pickMovingBone = true; }
                BonePicker(CAF::Ui::Text::kTargetBone, marker.target, marker.targetBone);
                ImGui::SameLine();
                if (ImGui::SmallButton(CAF::Ui::Label(ICON_FA_CROSSHAIRS, CAF::Ui::Text::kPickTargetInWorld).c_str())) { panel.showBones = true; panel.pickMarker = i; panel.pickMovingBone = false; }
                changed |= marker.sourceBone != oldSource || marker.targetBone != oldTarget;
                changed |= ImGui::DragFloat3(CAF::Ui::Text::kPosTargetRightForwardUp, &marker.offset.x, 0.1f);
                changed |= ImGui::DragFloat3(CAF::Ui::Text::kRotPitchRollYawRadians, &marker.rotation.x, 0.005f);
                if (ImGui::SmallButton(CAF::Ui::Text::kFaceEachOther)) { marker.rotation = { 0, 0, 3.141592654f }; changed = true; }
                ImGui::SameLine();
                if (ImGui::SmallButton(CAF::Ui::Text::kSameDirection)) { marker.rotation = {}; changed = true; }
                ImGui::TextWrapped(CAF::Ui::Text::kPosUsesTargetActorAxesAndGameUnitsRot);
            }
            else if (marker.kind == CAF::Command::EventKind::ArtBind || marker.kind == CAF::Command::EventKind::ArtUnbind)
            {
                changed |= ImGui::InputInt(CAF::Ui::Text::kBindingIDPerActor, &marker.bindingID);
                if (marker.kind == CAF::Command::EventKind::ArtBind)
                {
                    char resource[256]{};
                    std::snprintf(resource, sizeof(resource), "%s", marker.resource.c_str());
                    if (ImGui::InputText(CAF::Ui::Text::kARTOPluginEsmLocalHexID, resource, sizeof(resource)))
                    { marker.resource = resource; changed = true; }
                    const auto oldBone = marker.sourceBone;
                    BonePicker(CAF::Ui::Text::kAttachBone, marker.source, marker.sourceBone);
                    ImGui::SameLine();
                    if (ImGui::SmallButton(CAF::Ui::Label(ICON_FA_CROSSHAIRS, CAF::Ui::Text::kPickAttachBone).c_str())) { panel.showBones = true; panel.pickMarker = i; panel.pickMovingBone = true; }
                    changed |= oldBone != marker.sourceBone;
                    changed |= ImGui::Combo(CAF::Ui::Text::kBindingBackend, &marker.bindingMode, CAF::Ui::Text::kNativeArtObjectExperimentalNIFCloneFromARTOExperimental);
                    changed |= ImGui::DragFloat3(CAF::Ui::Text::kBoneLocalOffset, &marker.offset.x, 0.1f);
                    changed |= ImGui::DragFloat3(CAF::Ui::Text::kRotationXYZRadians, &marker.rotation.x, 0.005f);
                    changed |= ImGui::DragFloat(CAF::Ui::Text::kDurationUntilUnbindStop, &marker.duration, 0.05f, 0, 3600, "%.3f", ImGuiSliderFlags_AlwaysClamp);
                    ImGui::TextWrapped(CAF::Ui::Text::kNativeModeUsesSkyrimEffectsNIFCloneTestsBone);
                }
                else ImGui::TextWrapped(CAF::Ui::Text::kRemovesTheBindingWithThisIDOnSubjectActor);
            }
            else if (marker.kind == CAF::Command::EventKind::Sound)
            {
                char resource[256]{};
                std::snprintf(resource, sizeof(resource), "%s", marker.resource.c_str());
                if (ImGui::InputText(CAF::Ui::Text::kSoundPluginEsmLocalHexID, resource, sizeof(resource)))
                {
                    marker.resource = resource;
                    changed = true;
                }
                if (!CAF::SceneMath::ParseFormKey(marker.resource)) ImGui::TextWrapped(CAF::Ui::Text::kEnterASNDRRecordAsPluginEsmABCLocal);
            }
            else ImGui::TextWrapped(CAF::Ui::Text::kDuringPlaySceneThisActorIsKilledWithThe);
            if (changed)
            {
                const bool resample = timeChanged || (marker.kind == CAF::Command::EventKind::Alignment || marker.kind == CAF::Command::EventKind::PredictiveAlignment) ||
                    marker.kind == CAF::Command::EventKind::ArtBind || marker.kind == CAF::Command::EventKind::ArtUnbind;
                SendScene(CAF::Action::SetSceneMarkers, resample ? 1.0f : 0.0f);
                if (resample) SendScene(CAF::Action::SeekScene, EditTime(marker));
            }
            ImGui::EndDisabled();
            if (auto error = CAF::SceneEventError(marker); !error.empty()) ImGui::TextWrapped(CAF::Ui::Text::kDraftS, error.c_str());
            ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::InputText(CAF::Ui::Text::kPresetNameLettersNumbersAnd, panel.sceneName, sizeof(panel.sceneName));
        if (ImGui::Button(CAF::Ui::Label(ICON_FA_FILE_CIRCLE_PLUS, CAF::Ui::Text::kNewPreset).c_str()))
        {
            SendScene(CAF::Action::StopScene);
            panel.alignments.clear();
            panel.scenePaths = {};
            panel.editingMarker = panel.pickMarker = -1;
            panel.sceneFileStatus = CAF::Ui::Text::kNewPresetSelectActorsHKXAndChooseAName;
        }
        ImGui::SameLine();
        if (ImGui::Button(CAF::Ui::Label(ICON_FA_FLOPPY_DISK, CAF::Ui::Text::kSaveScene).c_str())) SaveScene();
        ImGui::SameLine();
        if (ImGui::Button(CAF::Ui::Label(ICON_FA_FOLDER_OPEN, CAF::Ui::Text::kLoadScene).c_str())) LoadScene();
        ImGui::SameLine();
        if (ImGui::BeginCombo(CAF::Ui::Text::kSavedPresets, panel.sceneName))
        {
            std::error_code error;
            std::vector<std::string> names;
            std::filesystem::directory_iterator iterator(ScenePath().parent_path(), error), end;
            while (!error && iterator != end)
            {
                if (iterator->path().extension() == ".scene") names.push_back(iterator->path().stem().string());
                iterator.increment(error);
            }
            std::sort(names.begin(), names.end());
            for (const auto& name : names)
                if (ImGui::Selectable(name.c_str(), name == panel.sceneName))
                { std::snprintf(panel.sceneName, sizeof(panel.sceneName), "%s", name.c_str()); LoadScene(); }
            ImGui::EndCombo();
        }
        ImGui::TextWrapped("%s", panel.sceneFileStatus.c_str());
        if (panel.pickMarker >= 0 && panel.pickMarker < static_cast<int>(panel.alignments.size())) ImGui::Text(CAF::Ui::Text::kClickActorCSSBoneInTheWorld,
            'A' + (panel.pickMovingBone ? panel.alignments[panel.pickMarker].source : panel.alignments[panel.pickMarker].target),
            panel.pickMovingBone ? CAF::Ui::Text::kMoving : CAF::Ui::Text::kTarget);
        ImGui::TextWrapped(CAF::Ui::Text::kPresetsDataSKSEPluginsCustomAnimationFrameworkScenesNameSceneOffset);
    }

    bool DrawSceneTimeline()
    {
        if (!panel.snapshot.scenePlaying || !panel.snapshot.scenePreview) return false;
        const auto* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
            viewport->Pos.y + viewport->Size.y * 0.88f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 1));
        ImGui::SetNextWindowSize(ImVec2(640, 200), ImGuiCond_FirstUseEver);
        const bool open = ImGui::Begin(CAF::Ui::Text::kCAFSceneTimeline);
        const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
        if (open)
        {
            ImGui::Text(CAF::Ui::Text::kPausedPreviewFFSS, panel.snapshot.sceneTime,
                panel.snapshot.sceneDuration, panel.snapshot.sceneSeeking ? CAF::Ui::Text::kSampling : "");
            ImGui::Text(CAF::Ui::Text::kCameraSLLluAllLluPositionHookS, panel.snapshot.sceneCameraReady ? CAF::Ui::Text::kReady :
                (panel.snapshot.sceneCameraActive ? CAF::Ui::Text::kWaitingForThirdPersonOutput : CAF::Ui::Text::kInactive),
                static_cast<unsigned long long>(panel.snapshot.sceneCameraPanCommands),
                static_cast<unsigned long long>(panel.snapshot.sceneCameraDragCommands),
                panel.snapshot.sceneCameraPositionHook ? CAF::Ui::Text::kReady : CAF::Ui::Text::kUnavailable);
            if (!panel.sceneSeekDragging) panel.sceneSeek = panel.snapshot.sceneTime;
            if (ImGui::SliderFloat(CAF::Ui::Text::kTime, &panel.sceneSeek, 0, std::max(0.001f, panel.snapshot.sceneDuration), "%.3f s"))
            {
                panel.editingMarker = panel.pickMarker = -1;
                panel.sceneSeekDragging = true;
            }
            // 拖动中只更新 UI；松开后采样一次，避免每帧重置双人姿态重建。
            if (panel.sceneSeekDragging && ImGui::IsItemDeactivatedAfterEdit())
            {
                SendScene(CAF::Action::SeekScene, panel.sceneSeek);
                panel.sceneSeekDragging = false;
            }
            const auto start = ImGui::GetCursorScreenPos();
            const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
            ImGui::InvisibleButton(CAF::Ui::Text::kEventMarkers, ImVec2(width, 38));
            auto* draw = ImGui::GetWindowDrawList();
            constexpr ImU32 colors[]{ IM_COL32(70, 190, 255, 255), IM_COL32(100, 220, 120, 255),
                IM_COL32(240, 80, 80, 255), IM_COL32(210, 150, 255, 255), IM_COL32(255, 180, 60, 255), IM_COL32(255, 230, 80, 255) };
            int hovered = -1;
            float nearest = 100;
            for (int i = 0; i < static_cast<int>(panel.alignments.size()); ++i)
            {
                const auto& marker = panel.alignments[i];
                if (marker.time > panel.snapshot.sceneDuration) continue;
                const float x = start.x + 6 + (width - 12) * marker.time / std::max(0.001f, panel.snapshot.sceneDuration);
                const float y = start.y + 8 + (i % 3) * 11;
                draw->AddCircleFilled(ImVec2(x, y), 4, colors[static_cast<int>(marker.kind)]);
                if (marker.kind == CAF::Command::EventKind::PredictiveAlignment)
                {
                    const float targetX = start.x + 6 + (width - 12) * marker.predictionTime / std::max(0.001f, panel.snapshot.sceneDuration);
                    draw->AddLine(ImVec2(x, y), ImVec2(targetX, y), colors[5]);
                    draw->AddCircle(ImVec2(targetX, y), 5, colors[5]);
                }
                const auto mouse = ImGui::GetIO().MousePos;
                float distance = (mouse.x - x) * (mouse.x - x) + (mouse.y - y) * (mouse.y - y);
                if (marker.kind == CAF::Command::EventKind::PredictiveAlignment)
                {
                    const float targetX = start.x + 6 + (width - 12) * marker.predictionTime / std::max(0.001f, panel.snapshot.sceneDuration);
                    distance = std::min(distance, (mouse.x - targetX) * (mouse.x - targetX) + (mouse.y - y) * (mouse.y - y));
                }
                if (ImGui::IsItemHovered() && distance < nearest) { nearest = distance; hovered = i; }
            }
            if (hovered >= 0)
            {
                const auto& marker = panel.alignments[hovered];
                ImGui::SetTooltip(CAF::Ui::Text::kEventDTriggerFSEditFSActor, hovered + 1, marker.time, EditTime(marker), 'A' + marker.source);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    panel.editingMarker = hovered;
                    panel.pickMarker = -1;
                    SendScene(CAF::Action::SeekScene, EditTime(marker));
                }
            }
            ImGui::TextWrapped(CAF::Ui::Text::kClickAnEventMarkerToJumpEditBlueAlign);
        }
        ImGui::End();
        return hovered;
    }

    void DrawGraph()
    {
        if (ImGui::CollapsingHeader(CAF::Ui::Text::kNativeSoundAttackImpactProbes, ImGuiTreeNodeFlags_DefaultOpen))
        {
            static int eventIndex = 0;
            static char sound[128] = "WPNSwingBladeMedium";
            static char material[128] = "";
            static char impactBone[128] = "NPC Spine2 [Spn2]";
            static bool leftHand = false;
            static int target = 1;
            static int materialIndex = 0;
            static float direction[3]{ 0, 0, -1 };
            constexpr const char* events[]{ "FootLeft", "FootRight", "FootSprintLeft", "FootSprintRight", "FootScuffLeft", "FootScuffRight", "weaponSwing", "weaponLeftSwing", "preHitFrame", "HitFrame" };
            ImGui::TextWrapped(CAF::Ui::Text::kSourceSelectedABActorWorksWithoutAnIndependent);
            ImGui::Combo(CAF::Ui::Text::kNativeEvent, &eventIndex, events, static_cast<int>(std::size(events)));
            ActionButton(ICON_FA_PAPER_PLANE, CAF::Ui::Text::kProbeGraphINPUT, CAF::Action::SendEvent, events[eventIndex]);
            ImGui::SameLine();
            ActionButton(ICON_FA_PAPER_PLANE, CAF::Ui::Text::kProbeAnnotationOUTPUT, CAF::Action::ProbeNativeEvent, events[eventIndex]);
            if (eventIndex < 6)
                ActionButton(ICON_FA_VOLUME_HIGH, CAF::Ui::Text::kProbeFootstepManager, CAF::Action::ProbeFootstep, events[eventIndex]);
            else
                ImGui::TextWrapped(CAF::Ui::Text::kAttackEventsCanTriggerRealCombatEffectsWhenNative);
            ImGui::InputText(CAF::Ui::Text::kSOUNSNDREditorIDOrFormKey, sound, sizeof(sound));
            ActionButton(ICON_FA_VOLUME_HIGH, CAF::Ui::Text::kPlayDirectSOUNSNDR, CAF::Action::ProbeSound, sound);
            ImGui::SameLine();
            ActionButton(ICON_FA_PAPER_PLANE, CAF::Ui::Text::kProbeSoundPlayOUTPUT, CAF::Action::ProbeNativeEvent, std::string("SoundPlay.") + sound);
            ImGui::TextWrapped(CAF::Ui::Text::kSoundPlayPayloadExpectsAVanillaSOUNEditorIDEG);
            ActionButton(ICON_FA_STOP, CAF::Ui::Text::kStopDirectMappedProbeSounds, CAF::Action::StopProbeSounds);
            ImGui::TextDisabled(CAF::Ui::Text::kINPUTAndDirectSOUNSNDRHadNoEffectIn);
            ImGui::Separator();
            ImGui::Checkbox(CAF::Ui::Text::kUseLeftHandWeapon, &leftHand);
            constexpr const char* targets[]{ "A", "B" };
            ImGui::Combo(CAF::Ui::Text::kImpactTarget, &target, targets, 2);
            constexpr const char* materialNames[]{ CAF::Ui::Text::kSkin, CAF::Ui::Text::kMeat, CAF::Ui::Text::kMetalSolid, CAF::Ui::Text::kWood, CAF::Ui::Text::kStoneHeavy };
            constexpr RE::MATERIAL_ID materials[]{ RE::MATERIAL_ID::kSkin, RE::MATERIAL_ID::kMeat, RE::MATERIAL_ID::kMetalSolid, RE::MATERIAL_ID::kWood, RE::MATERIAL_ID::kStoneHeavy };
            ImGui::Combo(CAF::Ui::Text::kMaterialPresetEmptyOverride, &materialIndex, materialNames, 5);
            ImGui::InputText(CAF::Ui::Text::kMATTEditorIDOrFormKey, material, sizeof(material));
            ImGui::InputText(CAF::Ui::Text::kTargetImpactBone, impactBone, sizeof(impactBone));
            ImGui::InputFloat3(CAF::Ui::Text::kEffectRayDirection, direction);
            const auto probe = [&](const char* label, float mode)
            {
                if (!ImGui::Button(label)) return;
                CAF::Command command;
                command.action = CAF::Action::ProbeImpact;
                command.slot = panel.slot;
                command.text = material;
                command.form = static_cast<RE::FormID>(materials[materialIndex]);
                command.probeNode = impactBone;
                command.probeTarget = target;
                command.probeLeftHand = leftHand;
                command.position = { direction[0], direction[1], direction[2] };
                command.value = mode;
                CAF::Enqueue(std::move(command));
            };
            probe(CAF::Ui::Text::kInspectWeaponMaterialMapping, 0);
            probe(CAF::Ui::Text::kPlayMappedImpactSounds, 1);
            probe(CAF::Ui::Text::kProbeVanillaImpactEffect, 2);
            ImGui::TextWrapped(CAF::Ui::Text::kSoundMappingUsesYourExplicitMATTSelectionNotDetected);
            ImGui::Separator();
        }
        ImGui::InputText(CAF::Ui::Text::kEventName, panel.event, sizeof(panel.event));
        ActionButton(ICON_FA_PAPER_PLANE, CAF::Ui::Text::kSendGraphEvent, CAF::Action::SendEvent, panel.event);
        ImGui::InputText(CAF::Ui::Text::kVariableName, panel.variable, sizeof(panel.variable));
        ImGui::InputFloat(CAF::Ui::Text::kValue, &panel.value);
        ActionButton(ICON_FA_MAGNIFYING_GLASS, CAF::Ui::Text::kReadBool, CAF::Action::ReadBool, panel.variable); ImGui::SameLine();
        ActionButton(ICON_FA_MAGNIFYING_GLASS, CAF::Ui::Text::kReadInt, CAF::Action::ReadInt, panel.variable); ImGui::SameLine();
        ActionButton(ICON_FA_MAGNIFYING_GLASS, CAF::Ui::Text::kReadFloat, CAF::Action::ReadFloat, panel.variable);
        ActionButton(ICON_FA_PEN, CAF::Ui::Text::kWriteBool, CAF::Action::WriteBool, panel.variable, panel.value); ImGui::SameLine();
        ActionButton(ICON_FA_PEN, CAF::Ui::Text::kWriteInt, CAF::Action::WriteInt, panel.variable, panel.value); ImGui::SameLine();
        ActionButton(ICON_FA_PEN, CAF::Ui::Text::kWriteFloat, CAF::Action::WriteFloat, panel.variable, panel.value);
    }

    void DrawSkeleton()
    {
        ActionButton(ICON_FA_CAMERA, CAF::Ui::Text::kCaptureTransform, CAF::Action::CaptureTransform); ImGui::SameLine();
        ActionButton(ICON_FA_CLOCK_ROTATE_LEFT, CAF::Ui::Text::kRestoreCapturedTransform, CAF::Action::RestoreTransform);
        if (ImGui::Button(CAF::Ui::Label(ICON_FA_COPY, CAF::Ui::Text::kCopyActorTransformToFields).c_str()))
        {
            const auto& actor = panel.snapshot.actors[panel.slot];
            panel.position[0] = actor.position.x; panel.position[1] = actor.position.y; panel.position[2] = actor.position.z;
            panel.rotation[0] = actor.rotation.x; panel.rotation[1] = actor.rotation.y; panel.rotation[2] = actor.rotation.z;
        }
        ImGui::InputFloat3(CAF::Ui::Text::kWorldXYZ, panel.position);
        ImGui::InputFloat3(CAF::Ui::Text::kAnglesXYZRadians, panel.rotation);
        if (ImGui::Button(CAF::Ui::Label(ICON_FA_LOCATION_CROSSHAIRS, CAF::Ui::Text::kSetActorTransform).c_str()))
        {
            CAF::Command command; command.action = CAF::Action::SetTransform; command.slot = panel.slot;
            command.position = { panel.position[0], panel.position[1], panel.position[2] };
            command.rotation = { panel.rotation[0], panel.rotation[1], panel.rotation[2] }; CAF::Enqueue(command);
        }
        ActionButton(ICON_FA_LOCK, CAF::Ui::Text::kLockCurrentTransformEachTick, CAF::Action::LockTransform); ImGui::SameLine();
        ActionButton(ICON_FA_LOCK_OPEN, CAF::Ui::Text::kUnlock, CAF::Action::UnlockTransform);
        ImGui::InputFloat3(CAF::Ui::Text::kBOffsetFromARightForwardUp, panel.offset);
        if (ImGui::Button(CAF::Ui::Label(ICON_FA_ARROWS_TO_DOT, CAF::Ui::Text::kAlignBToA).c_str()))
        {
            CAF::Command command; command.action = CAF::Action::AlignPair; command.slot = 0;
            command.position = { panel.offset[0], panel.offset[1], panel.offset[2] }; CAF::Enqueue(command);
        }
        ImGui::InputText(CAF::Ui::Text::kBoneNodeName, panel.bone, sizeof(panel.bone));
        if (ImGui::Button(CAF::Ui::Label(ICON_FA_BONE, CAF::Ui::Text::kQueryBoneTransform).c_str())) Send(CAF::Action::QueryBone, panel.bone);
        ImGui::SameLine(); ActionButton(ICON_FA_FILE_LINES, CAF::Ui::Text::kDumpSkeletonToLog, CAF::Action::DumpSkeleton);
        ImGui::TextWrapped("%s", panel.snapshot.actors[panel.slot].bone.c_str());
    }

    void DrawCamera()
    {
        ImGui::Text(CAF::Ui::Text::kFreecamSTransformOverrideS, panel.snapshot.freecam ? CAF::Ui::Text::kActive : CAF::Ui::Text::kInactive,
            panel.snapshot.cameraOverride ? CAF::Ui::Text::kActive : CAF::Ui::Text::kInactive);
        ImGui::Text(CAF::Ui::Text::kWorldFFF, panel.snapshot.cameraPosition.x, panel.snapshot.cameraPosition.y, panel.snapshot.cameraPosition.z);
        ActionButton(ICON_FA_VIDEO, CAF::Ui::Text::kEnterFreecam, CAF::Action::EnterFreecam); ImGui::SameLine();
        ActionButton(ICON_FA_VIDEO_SLASH, CAF::Ui::Text::kExitCAFFreecam, CAF::Action::ExitFreecam);
        ActionButton(ICON_FA_STREET_VIEW, CAF::Ui::Text::kFirstPerson, CAF::Action::FirstPerson); ImGui::SameLine();
        ActionButton(ICON_FA_USER, CAF::Ui::Text::kThirdPerson, CAF::Action::ThirdPerson);
        if (ImGui::Button(CAF::Ui::Label(ICON_FA_COPY, CAF::Ui::Text::kCopyCameraPosition).c_str()))
        {
            panel.cameraPosition[0] = panel.snapshot.cameraPosition.x;
            panel.cameraPosition[1] = panel.snapshot.cameraPosition.y;
            panel.cameraPosition[2] = panel.snapshot.cameraPosition.z;
            Send(CAF::Action::CaptureCamera);
        }
        ImGui::InputFloat3(CAF::Ui::Text::kCameraXYZ, panel.cameraPosition);
        ImGui::InputFloat3(CAF::Ui::Text::kCameraPitchUnusedYawRadians, panel.cameraRotation);
        if (ImGui::Button(CAF::Ui::Label(ICON_FA_CHECK, CAF::Ui::Text::kApplyFreecamTransformEachUpdate).c_str()))
        {
            CAF::Command command; command.action = CAF::Action::SetCamera;
            command.position = { panel.cameraPosition[0], panel.cameraPosition[1], panel.cameraPosition[2] };
            command.rotation = { panel.cameraRotation[0], panel.cameraRotation[1], panel.cameraRotation[2] }; CAF::Enqueue(command);
        }
        ImGui::SameLine(); ActionButton(ICON_FA_LINK_SLASH, CAF::Ui::Text::kReleaseOverride, CAF::Action::ReleaseCamera);
        ImGui::TextWrapped(CAF::Ui::Text::kCloseThisOverlayToTestNativeFreecamKeyboardMouse);
    }

    // 全局播放参数独立于场景编辑器，避免把运行时策略混入场景内容。
    void DrawPlaybackSettings()
    {
        ImGui::SeparatorText(CAF::Ui::Text::kPredictiveSamplingStability);
        auto settlement = panel.snapshot.contactSettlement;
        bool changed = ImGui::Checkbox(CAF::Ui::Text::kCheckSampledContactStability, &settlement.enabled);
        ImGui::BeginDisabled(!settlement.enabled);
        changed |= ImGui::DragInt(CAF::Ui::Text::kStableObservations, &settlement.requiredObservations, 1.0f, 1, 120, "%d", ImGuiSliderFlags_AlwaysClamp);
        changed |= ImGui::DragFloat(CAF::Ui::Text::kContactPositionTolerance, &settlement.positionTolerance, 0.01f, 0.0f, 10000.0f, CAF::Ui::Text::kFUnits, ImGuiSliderFlags_AlwaysClamp);
        ImGui::EndDisabled();
        if (changed)
        {
            CAF::Command command; command.action = CAF::Action::SetContactSettlementOptions; command.contactSettlement = settlement;
            CAF::Enqueue(std::move(command));
            CAF::Config::SavePlaybackSettings(panel.snapshot.physicsIsolation, settlement);
        }
        ImGui::TextWrapped(CAF::Ui::Text::kDefaultsObservationsWithinGameUnitsOfTheSameBaseline);
    }

    void DrawHistory()
    {
        ImGui::BeginChild(CAF::Ui::Text::kCAFEventHistory, ImVec2(0, 0), ImGuiChildFlags_Borders);
        for (auto it = panel.snapshot.messages.rbegin(); it != panel.snapshot.messages.rend(); ++it) ImGui::TextWrapped("%s", it->c_str());
        ImGui::EndChild();
    }

    void DrawStatus()
    {
        ImGui::SeparatorText(CAF::Ui::Text::kFacialCompatibility);
        ImGui::Text(CAF::Ui::Text::kFacialAnimationS, CAF::Facial::Enabled() ? CAF::Ui::Text::kEnabled : CAF::Ui::Text::kDisabled);
        ImGui::Text(CAF::Ui::Text::kFaceGenBackendS, CAF::Ui::Text::DisplayName(CAF::Facial::Backend()).data());
        ImGui::Text(CAF::Ui::Text::kMFGFixModuleS, CAF::Facial::HasMfgFix() ? CAF::Ui::Text::kLoaded2 : CAF::Ui::Text::kNotLoaded2);
        ImGui::TextWrapped(CAF::Facial::HasMfgFix() ?
            CAF::Ui::Text::kCAFWritesNativeFaceGenInputsTheLoadedMFGFix :
            CAF::Ui::Text::kCAFUsesVanillaFaceGenExpressionsAreSupportedModifierAnd);
        ImGui::TextWrapped(CAF::Ui::Text::kBackendDetectionReportsLoadedDLLsItDoesNotVerify);
    }

    const CAF::Ui::Page kPages[]
    {
        { ICON_FA_USERS, CAF::Ui::Text::kActors, &DrawActors },
        { ICON_FA_HAND, CAF::Ui::Text::kControl, &DrawControl },
        { ICON_FA_PERSON_RUNNING, CAF::Ui::Text::kAnimation, &DrawAnimation },
        { ICON_FA_GEARS, CAF::Ui::Text::kConfiguration, &DrawConfiguration },
        { ICON_FA_FILM, CAF::Ui::Text::kScene, &CAF::SceneEditor::Draw },
        { ICON_FA_BOLT, CAF::Ui::Text::kSceneTrigger, &DrawSceneTrigger },
        { ICON_FA_FILM, CAF::Ui::Text::kLegacyScene, &DrawScene },
        { ICON_FA_DIAGRAM_PROJECT, CAF::Ui::Text::kGraph, &DrawGraph },
        { ICON_FA_BONE, CAF::Ui::Text::kSkeleton, &DrawSkeleton },
        { ICON_FA_CAMERA, CAF::Ui::Text::kCamera, &DrawCamera },
        { ICON_FA_CLOCK_ROTATE_LEFT, CAF::Ui::Text::kHistory, &DrawHistory },
        { ICON_FA_SLIDERS, CAF::Ui::Text::kPlaybackSettings, &DrawPlaybackSettings },
        { ICON_FA_SLIDERS, CAF::Ui::Text::kInterface, &Overlay::DrawInterfaceSettings },
        { ICON_FA_BOLT, CAF::Ui::Text::kExecutions, &CAF::Execution::DrawSettings },
        { ICON_FA_CIRCLE_INFO, CAF::Ui::Text::kStatus, &DrawStatus },
    };
    // 「Legacy scene」页在 kPages 中的下标；骨骼投影只随该页的复选框开关，插入新页时需同步。
    constexpr int kLegacyScenePage = 5;
}

namespace CAF
{
    bool DrawSceneWindows()
    {
        // 与主窗口折叠状态独立；辅助窗口使用本帧运行时快照。
        panel.snapshot = ReadSnapshot();
        const bool editorHovered = SceneEditor::DrawWindows();
        return editorHovered | (!panel.snapshot.scriptScene && DrawSceneTimeline());
    }

    void DrawDebug()
    {
        panel.snapshot = ReadSnapshot();
        panel.paths = Animation::Paths();
        const auto& state = panel.snapshot;

        // 顶部常驻状态区：切页时仍然可见，Restore ALL 也始终可点。
        ImGui::Text(CAF::Ui::Text::kWorldSPausedSMainMenuSLoadingS, state.worldReady ? CAF::Ui::Text::kReady : CAF::Ui::Text::kUnavailable,
            state.paused ? CAF::Ui::Text::kYes : CAF::Ui::Text::kNo, state.mainMenu ? CAF::Ui::Text::kYes : CAF::Ui::Text::kNo, state.loading ? CAF::Ui::Text::kYes : CAF::Ui::Text::kNo);
        ImGui::Text(CAF::Ui::Text::kTickLluRealDtFSimulationDtF, static_cast<unsigned long long>(state.frame), state.realDelta, state.simulationDelta);
        ImGui::Text(CAF::Ui::Text::kMouseCumulativeLldLld, static_cast<long long>(state.mouseX), static_cast<long long>(state.mouseY));
        if (ImGui::Button(CAF::Ui::Label(ICON_FA_ROTATE_LEFT, CAF::Ui::Text::kRestoreALLCAFControls).c_str())) Send(Action::ResetAll);
        ImGui::SameLine();
        ImGui::TextDisabled(CAF::Ui::Text::kCommandsRunOnTheGameUpdateThread);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(CAF::Ui::Text::kClipStatusIsSampledInHavokHooksGameValidation);
        }
        ImGui::Separator();

        Ui::DrawTabs("CAF debug tabs", debugTabs, kPages);
        static bool projectionRequested = false;
        const bool wantProjection = debugTabs.active == kLegacyScenePage && panel.showBones;
        if (projectionRequested != wantProjection)
        {
            SendScene(Action::SetBoneProjection, wantProjection ? 1.0f : 0.0f);
            projectionRequested = wantProjection;
        }
    }
}
