#include "camera/Freecam.h"
#include "runtime/Runtime.h"
#include "scene/SceneMath.h"
#include "scene/SceneValidation.h"
#include "scene/ScenePlaybackPlan.h"
#include "scene/SceneSession.h"
#include "scene/LogicStatePolicy.h"
#include "scene/AlignmentCache.h"
#include "scene/AlignmentSolver.h"
#include "animation/ClipPath.h"
#include "animation/IndependentPlayer.h"
#include "animation/PlaybackIsolation.h"
#include "animation/PoseDiagnostics.h"
#include "animation/PoseSampling.h"
#include "effects/ArtBinding.h"
#include "effects/NativeEventProbe.h"
#include "effects/NativeEffects.h"
#include "effects/ScreenEffects.h"
#include "features/MCGore.h"
#include "features/GTSGore.h"
#include "ui/Overlay.h"
#include "applications/execution/Execution.h"

#include <RE/F/FixedStrings.h>
#include <RE/F/FreeCameraState.h>
#include <RE/T/ThirdPersonState.h>
#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>

namespace CAF
{
    namespace
    {
        using AlignmentTransform = Alignment::Transform<RE::NiPoint3, RE::NiMatrix3>;
        struct ActorState
        {
            RE::ActorHandle handle;
            bool frozen = false;
            bool originalAI = true;
            bool ownsRestraint = false;
            bool captured = false;
            bool locked = false;
            RE::NiPoint3 savedPosition{};
            RE::NiPoint3 savedRotation{};
            RE::NiPoint3 lockPosition{};
            RE::NiPoint3 lockRotation{};
            std::string bone;
            std::vector<ActorInfo::BoneInfo> skeleton;
        };
        std::array<ActorState, 10> actors; // A/B 调试与八个场景槽位分别持有状态。
        std::mutex uiMutex;
        std::deque<Command> commands;
        Snapshot snapshot;
        Snapshot working;
        std::optional<Command> armedHitScene;
        std::atomic<std::uint64_t> hitSceneGeneration = 0;
        std::uint64_t nextHitSceneGeneration = 0;
        std::atomic_bool worldLoaded = false;
        bool playerFrozen = false;
        bool ownsPlayerAI = false;
        bool originalPlayerAI = false;
        std::uint32_t restoredControlBits = 0;
        std::mutex cameraMutex;
        bool overrideCamera = false;
        bool sceneCameraPositionHookInstalled = false;
        RE::NiPoint3 cameraPosition{};
        RE::NiPoint3 cameraRotation{};
        struct CameraTarget
        {
            bool following = false, pending = false, focusing = false;
            bool savedFreeRotationEnabled = false;
            RE::NiPoint2 savedFreeRotation{};
            RE::ActorHandle actor;
            std::string bone;
            RE::NiPoint3 offset{}, position{};
        } cameraTarget;
        struct SceneCameraState
        {
            bool active = false;
            bool initialized = false;
            bool restoreFirstPerson = false;
            bool rotationCaptured = false;
            bool translationCaptured = false;
            std::uint64_t dragCommands = 0;
            std::uint64_t panCommands = 0;
            RE::NiQuaternion initialRotation{};
            RE::NiPoint3 position{};
            RE::NiMatrix3 orientation{};
            float pitch = 0;
        } sceneCamera;
        std::atomic<std::int64_t> mouseX = 0, mouseY = 0;
        Scene::Session scene;
        std::chrono::steady_clock::time_point sceneStartupAt{};
        std::uint64_t sceneStartupGeneration = 0;
        bool sceneStartupActive = false;
        std::array<std::uint64_t, 10> scenePlaybackWriteBaseline{};
        std::array<Snapshot::ActorPlaybackStart, 10> actorPlaybackStarts{};
        std::uint64_t nextActorPlaybackGeneration = 0;
        std::optional<Command> pendingPreviewEdit; // UI 合并命令不属于片段运行状态。
        Sampling::ContactSettlementOptions contactSettlementOptions;
        bool samplingComparisonArmed = false;
        constexpr std::array<const char*, 5> comparisonNames{ "loading", "playing-to-target", "played-hold", "zero-hold", "seek-hold" };
        struct PoseProbe
        {
            RE::ActorHandle actor;
            RE::NiPoint3 position{}, rotation{};
            std::uint64_t frame = 0;
            bool active = false, finished = false;
        } poseProbe;
        bool projectBones = false;
        bool projectSceneBones = false;
        Command cameraAnchorPreview;

        void Log(std::string message)
        {
            SKSE::log::info("CAF {}", message);
            working.messages.push_back(std::move(message));
            if (working.messages.size() > 120) working.messages.erase(working.messages.begin());
        }

        class GraphSink final : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* event, RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
            {
                if (event && event->holder)
                    Animation::RecordEvent({ event->holder->GetFormID(), "Graph event", std::string(event->tag.c_str()) + " | " + event->payload.c_str() });
                return RE::BSEventNotifyControl::kContinue;
            }
        } graphSink;

        bool Ready(RE::Actor* actor)
        {
            // 相邻室外 cell 的角色也可同时加载；可用性取决于 3D，不要求 cell 相同。
            return actor && actor->Is3DLoaded() && actor->GetParentCell() && !actor->IsDead();
        }
        void CancelHitScene()
        {
            hitSceneGeneration = 0;
            armedHitScene.reset();
        }
        class HitSceneSink final : public RE::BSTEventSink<RE::TESHitEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* event, RE::BSTEventSource<RE::TESHitEvent>*) override
            {
                const auto generation = hitSceneGeneration.load();
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!generation || !event || !player || event->cause.get() != player || !event->target)
                    return RE::BSEventNotifyControl::kContinue;
                auto* target = event->target->As<RE::Actor>();
                if (target && target != player)
                {
                    // 命中回调只排队句柄；切换演员和播放留到主循环，避免重入伤害处理。
                    Command command; command.action = Action::TriggerHitScene;
                    command.hitActor = target->GetHandle(); command.hitGeneration = generation;
                    Enqueue(std::move(command));
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        } hitSceneSink;
        using PlaybackIsolation::SetRestrained;
        using PlaybackIsolation::StopMovement;
        bool AttachGraphSink(RE::Actor* actor)
        {
            RE::BSAnimationGraphManagerPtr manager;
            if (!actor->GetAnimationGraphManager(manager) || !manager) return false;
            // CommonLib 的便捷函数会直接访问 graphs.front()；这里保护空图并覆盖多张图。
            RE::BSSpinLockGuard lock(manager->GetRuntimeData().updateLock);
            bool attached = false;
            for (const auto& graph : manager->graphs)
            {
                if (!graph) continue;
                graph->GetEventSource<RE::BSAnimationGraphEvent>()->AddEventSink(&graphSink);
                attached = true;
            }
            return attached;
        }
        bool Finite(const RE::NiPoint3& point)
        {
            return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
        }
        void SetTransform(RE::Actor* actor, const RE::NiPoint3& position, const RE::NiPoint3& rotation)
        {
            // 对齐/传送是摆位操作，不能被两帧差分解释为死亡冲量。
            if (actor->GetPosition().GetSquaredDistance(position) > 0.000001f ||
                actor->GetAngle().GetSquaredDistance(rotation) > 0.000000000001f)
                IndependentPlayer::InvalidateMotionHistory(actor->GetHandle());
            if (scene.IsActive() && scene.fragment.holdTransforms)
                for (std::size_t i = 0; i < actors.size(); ++i)
                    if (scene.fragment.active[i] && actors[i].handle == actor->GetHandle())
                    { scene.fragment.heldPositions[i] = position; scene.fragment.heldRotations[i] = rotation; }
            // 固定根变换只纠正实际漂移，避免每帧重复 SetHeading 刷新玩家视角。
            if (actor->GetPosition().GetSquaredDistance(position) > 0.000001f) actor->SetPosition(position, true);
            // OStim 区分玩家和 NPC：玩家使用 SetHeading，避免 SetAngle 的视角抖动。
            if (actor->IsPlayerRef())
            {
                if (std::abs(actor->GetAngle().z - rotation.z) > 0.000001f) actor->SetHeading(rotation.z);
            }
            else if (actor->GetAngle().GetSquaredDistance(rotation) > 0.000000000001f) actor->SetAngle(rotation);
        }
        void RestorePlayer()
        {
            if (ownsPlayerAI)
            {
                if (auto* player = RE::PlayerCharacter::GetSingleton()) player->SetAIDriven(originalPlayerAI);
                ownsPlayerAI = false;
            }
            if (playerFrozen)
            {
                if (auto* map = RE::ControlMap::GetSingleton())
                {
                    auto bits = restoredControlBits;
                    // 手动解冻不提前释放场景租约；原始可用位转交给场景结束恢复。
                    if (scene.controls.PlayerInputs().Owned())
                    {
                        scene.controls.PlayerInputs().TransferRestoreBits(bits);
                        bits = 0;
                    }
                    map->ToggleControls(static_cast<RE::ControlMap::UEFlag>(bits), true, false);
                }
                playerFrozen = false;
                restoredControlBits = 0;
            }
        }
        void RestorePoseProbe()
        {
            if (!poseProbe.active) return;
            if (auto actor = poseProbe.actor.get(); Ready(actor.get()))
                SetTransform(actor.get(), poseProbe.position, poseProbe.rotation);
            poseProbe = {};
        }
        void RestoreActor(ActorState& state, bool immediate = false)
        {
            // 即使 Actor 已卸载，独立播放器仍需释放持有的图和动画资源。
            Animation::Stop(state.handle, immediate);
            if (auto actor = state.handle.get())
            {
                if (state.frozen) actor->EnableAI(state.originalAI);
                // 死亡/击倒后的生命状态不能被旧快照覆盖；仅撤销仍属于 CAF 的 restraint。
                if (state.ownsRestraint && actor->AsActorState()->GetLifeState() == RE::ACTOR_LIFE_STATE::kRestrained)
                    SetRestrained(actor.get(), false);
            }
            state.frozen = false;
            state.locked = false;
            state.ownsRestraint = false;
        }
        RE::ThirdPersonState* ThirdPersonCamera(RE::PlayerCamera* camera)
        {
            return camera ? static_cast<RE::ThirdPersonState*>(camera->GetRuntimeData().cameraStates[RE::CameraStates::kThirdPerson].get()) : nullptr;
        }
        // 调用者持有 cameraMutex；释放聚焦时归还原版自由旋转状态。
        void ClearCameraTarget()
        {
            if (cameraTarget.focusing)
                if (auto* state = ThirdPersonCamera(RE::PlayerCamera::GetSingleton()))
                {
                    state->freeRotationEnabled = cameraTarget.savedFreeRotationEnabled;
                    state->freeRotation = cameraTarget.savedFreeRotation;
                }
            cameraTarget = {};
        }
        void RebuildFaces(float time);
        void FireFragmentLifecycle(Scene::Trigger trigger);
        void DiagnoseAlignment(const Command::SceneEvent& event, std::string_view phase, bool detailed = true);

        bool ReleaseSceneActor(std::size_t slot, bool restorePosition)
        {
            if (!scene.controls.Owns(slot)) return false;
            const bool locked = scene.controls.Saved(slot).locked;
            if (!scene.ReleaseActor(slot, restorePosition, playerFrozen)) return false;
            actors[slot].locked = locked;
            return true;
        }

        void StopScene(bool restorePosition)
        {
            sceneStartupActive = false;
            RestorePoseProbe();
            if (!scene.IsActive() || scene.IsFinishing()) return;
            // 目标恰在场景末尾时，停止会释放姿态；先保留尚未记录的终点证据。
            if (PoseDiagnostics::Enabled() && !scene.fragment.preview && !scene.fragment.seekPending)
                for (std::size_t i = 0; i < scene.fragment.markers.size(); ++i)
                {
                    const auto& event = scene.fragment.markers[i];
                    const bool alignment = event.kind == Command::EventKind::Alignment || event.kind == Command::EventKind::PredictiveAlignment;
                    const float target = event.kind == Command::EventKind::PredictiveAlignment ? event.predictionTime : event.time;
                    if (alignment && event.trigger == Scene::Trigger::Time && scene.fragment.fired[i] && !scene.fragment.diagnosticTargets[i] && scene.fragment.time >= target)
                    { scene.fragment.diagnosticTargets[i] = true; DiagnoseAlignment(event, "playback-target-pre-stop"); }
                }
            scene.BeginFinish();
            scene.fragment.gotoRequested.clear();
            if (!scene.fragment.preview && !scene.fragment.comparison.enabled)
            {
                if (!scene.fragment.logic && !scene.fragment.endFired) FireFragmentLifecycle(Scene::Trigger::End);
                FireFragmentLifecycle(Scene::Trigger::Exit);
            }
            ArtBinding::Clear();
            // 强制自由相机属于本次会话，先退出相机再归还冻结状态，避免原生切换清除恢复值。
            if (scene.forcedFreecam)
            {
                Freecam::Release();
                if (auto* camera = RE::PlayerCamera::GetSingleton())
                {
                    if (scene.originalFreecam)
                    {
                        if (!camera->IsInFreeCameraMode()) camera->ToggleFreeCameraMode(false);
                        if (auto* state = Freecam::State(camera))
                        {
                            state->translation = scene.originalFreePosition;
                            state->rotation = scene.originalFreeRotation;
                        }
                    }
                    else if (scene.originalFirstPerson) camera->ForceFirstPerson();
                    else camera->ForceThirdPerson();
                }
            }
            bool restoreFirstPerson = false;
            {
                std::lock_guard lock(cameraMutex);
                restoreFirstPerson = sceneCamera.restoreFirstPerson;
                sceneCamera = {};
                ClearCameraTarget();
                if (scene.forcedFreecam)
                {
                    overrideCamera = scene.originalCameraOverride;
                    cameraPosition = scene.originalCameraPosition;
                    cameraRotation = scene.originalCameraRotation;
                }
            }
            if (restoreFirstPerson)
                if (auto* camera = RE::PlayerCamera::GetSingleton()) camera->ForceFirstPerson();
            for (std::size_t i = 0; i < actors.size(); ++i)
            {
                if (!scene.controls.Owns(i)) continue;
                actors[i].locked = scene.controls.Saved(i).locked;
                if (auto actor = actors[i].handle.get())
                    SKSE::log::info("CAF_SCENE_MOVEMENT actor={:08X} root-corrections={} max-root-drift={} restraint-owned={}",
                        actor->GetFormID(), scene.fragment.rootCorrections[i], scene.fragment.maxRootDrift[i], scene.controls.OwnsRestraint(i));
            }
            scene.Finish(restorePosition, playerFrozen);
        }
        void ExitFreecam()
        {
            { std::lock_guard lock(cameraMutex); overrideCamera = false; ClearCameraTarget(); }
            Freecam::Release();
        }
        void Reset(bool forget)
        {
            actorPlaybackStarts = {};
            samplingComparisonArmed = false;
            CancelHitScene();
            NativeEventProbe::StopSounds();
            StopScene(!forget);
            MCGore::Reset(forget);
            GTSGore::Reset(forget);
            pendingPreviewEdit.reset();
            scene = {};
            PoseDiagnostics::BeginRun("world reset");
            for (auto& state : actors)
            {
                RestoreActor(state, forget);
                if (forget)
                {
                    if (auto actor = state.handle.get()) actor->RemoveAnimationGraphEventSink(&graphSink);
                    state = {};
                }
            }
            RestorePlayer();
            ExitFreecam();
            if (forget) Animation::Reset();
            { std::lock_guard lock(uiMutex); commands.clear(); }
        }
        void DumpNode(RE::NiAVObject* object, int depth, int& count)
        {
            if (!object || depth > 64 || count++ >= 2048) return;
            SKSE::log::info("{}{} local=({:.2f},{:.2f},{:.2f}) world=({:.2f},{:.2f},{:.2f})",
                std::string(depth * 2, ' '), object->name.c_str(), object->local.translate.x, object->local.translate.y,
                object->local.translate.z, object->world.translate.x, object->world.translate.y, object->world.translate.z);
            if (auto* node = object->AsNode()) for (auto& child : node->children) DumpNode(child.get(), depth + 1, count);
        }

        void CollectBones(RE::NiAVObject* root, std::vector<ActorInfo::BoneInfo>& result)
        {
            struct PendingNode
            {
                RE::NiAVObject* object;
                int depth;
                int parent;
            };
            // 使用显式栈完整读取身体树，不按节点数量或深度截断，也不增长递归调用栈。
            std::vector<PendingNode> pending;
            if (root) pending.push_back({ root, 0, -1 });
            while (!pending.empty())
            {
                auto [object, depth, parent] = pending.back();
                pending.pop_back();
                if (!object->name.empty())
                {
                    const auto current = static_cast<int>(result.size());
                    result.push_back({ object->name.c_str(), depth, parent });
                    parent = current;
                }
                if (auto* node = object->AsNode())
                {
                    // 反向入栈保留深度优先顺序；NiTArray 的 size 不含空槽，需扫描完整容量。
                    for (std::size_t i = node->children.capacity(); i > 0; --i)
                        if (auto* child = node->children[i - 1].get())
                            pending.push_back({ child, depth + 1, parent });
                }
            }
        }

        AlignmentTransform ActorFrame(RE::Actor* actor, const RE::NiPoint3& position, RE::NiPoint3 angles)
        {
            return { position, Alignment::BodyHeading<RE::NiMatrix3>(angles) };
        }

        AlignmentTransform ActorFrame(RE::Actor* actor)
        {
            return ActorFrame(actor, actor->GetPosition(), actor->GetAngle());
        }

        bool AlignmentAngles(RE::Actor* actor, const AlignmentTransform& result, RE::NiPoint3& angles)
        {
            if (!Finite(result.position)) return false;
            for (const auto& row : result.rotation.entry)
                for (const auto value : row) if (!std::isfinite(value)) return false;
            if (actor->IsPlayerRef() && !Alignment::HeadingOnly(result.rotation))
            {
                Log("Alignment requires player pitch/roll: only heading is supported; use an NPC or adjust rot");
                return false;
            }
            angles = Alignment::EngineAngles<RE::NiPoint3>(result.rotation);
            return Finite(angles);
        }

        bool ApplyRelativeAlignment(const Command::Alignment& marker, const AlignmentTransform& relative)
        {
            auto moving = actors[marker.source].handle.get();
            auto fixed = actors[marker.target].handle.get();
            if (!Ready(moving.get()) || !Ready(fixed.get())) return false;
            const auto result = Alignment::Place(ActorFrame(fixed.get()), relative);
            RE::NiPoint3 angles;
            if (!AlignmentAngles(moving.get(), result, angles)) return false;
            SetTransform(moving.get(), result.position, angles);
            return true;
        }

        bool SampleAlignment(const Command::Alignment& marker, Scene::FragmentPlayback::Prediction& result)
        {
            if (marker.source < 0 || marker.source > 9 || marker.target < 0 || marker.target > 9 || marker.source == marker.target ||
                marker.sourceBone.empty() || marker.targetBone.empty() || !Finite(marker.offset) || !Finite(marker.rotation)) return false;
            auto moving = actors[marker.source].handle.get();
            auto fixed = actors[marker.target].handle.get();
            if (!Ready(moving.get()) || !Ready(fixed.get())) return false;
            // 始终使用第三人称身体骨架，避免预览相机切换改变接触点来源。
            auto* sourceRoot = moving->Get3D(false);
            auto* targetRoot = fixed->Get3D(false);
            auto* sourceBone = sourceRoot ? sourceRoot->GetObjectByName(RE::BSFixedString(marker.sourceBone)) : nullptr;
            auto* targetBone = targetRoot ? targetRoot->GetObjectByName(RE::BSFixedString(marker.targetBone)) : nullptr;
            if (!sourceBone || !targetBone) return false;

            // 从角色逻辑坐标提取完整接触点，3D 根偏移和实际体型长度一起参与最终旋转。
            result.sourceContact = Alignment::LocalContact(ActorFrame(moving.get()), sourceBone->world.translate);
            result.targetContact = Alignment::LocalContact(ActorFrame(fixed.get()), targetBone->world.translate);
            result.relative = Alignment::Solve(result.sourceContact, result.targetContact, marker.offset,
                Alignment::RelativeRotation<RE::NiMatrix3>(marker.rotation));
            result.valid = Finite(result.sourceContact) && Finite(result.targetContact) && Finite(result.relative.position);
            if (PoseDiagnostics::Enabled())
            {
                const float time = marker.kind == Command::EventKind::PredictiveAlignment ? marker.predictionTime : marker.time;
                const auto phase = marker.kind == Command::EventKind::PredictiveAlignment ? "predict-sample-assumption" : "align-sample-assumption";
                PoseDiagnostics::Contact(moving.get(), working.frame, phase, marker.sourceBone, time + scene.fragment.offsets[marker.source], result.sourceContact);
                PoseDiagnostics::Contact(fixed.get(), working.frame, phase, marker.targetBone, time + scene.fragment.offsets[marker.target], result.targetContact);
                SKSE::log::info("CAF_TRACE SAMPLE line={} trigger={} target={} relative=({},{},{}) valid={}", marker.line, marker.time, time,
                    result.relative.position.x, result.relative.position.y, result.relative.position.z, result.valid);
            }
            return result.valid;
        }

        void DiagnoseAlignment(const Command::SceneEvent& event, std::string_view phase, bool detailed)
        {
            if (!PoseDiagnostics::Enabled()) return;
            const auto index = static_cast<std::size_t>(&event - scene.fragment.markers.data());
            if (index >= scene.fragment.predictions.size()) return;
            const auto& cached = scene.fragment.predictions[index];
            auto source = actors[event.source].handle.get();
            auto target = actors[event.target].handle.get();
            if (!Ready(source.get()) || !Ready(target.get())) return;
            auto* sourceRoot = source->Get3D(false); auto* targetRoot = target->Get3D(false);
            auto* sourceBone = sourceRoot ? sourceRoot->GetObjectByName(RE::BSFixedString(event.sourceBone)) : nullptr;
            auto* targetBone = targetRoot ? targetRoot->GetObjectByName(RE::BSFixedString(event.targetBone)) : nullptr;
            if (!sourceBone || !targetBone) { Log("Pose diagnostic: contact bone missing"); return; }
            const auto a = Alignment::LocalContact(ActorFrame(source.get()), sourceBone->world.translate);
            const auto b = Alignment::LocalContact(ActorFrame(target.get()), targetBone->world.translate);
            const float sample = event.kind == Command::EventKind::PredictiveAlignment ? event.predictionTime : event.time;
            PoseDiagnostics::Contact(source.get(), working.frame, phase, event.sourceBone, sample + scene.fragment.offsets[event.source], a, detailed);
            PoseDiagnostics::Contact(target.get(), working.frame, phase, event.targetBone, sample + scene.fragment.offsets[event.target], b, detailed);
            // MainUpdate 返回并不保证 Ni 世界变换已传播；残差未消除时接触误差仅作瞬时记录。
            const auto rootResidual = [](RE::NiAVObject* root)
            {
                const auto expected = root->parent ? root->parent->world * root->local : root->local;
                return std::sqrt(expected.translate.GetSquaredDistance(root->world.translate));
            };
            const float sourceRootResidual = rootResidual(sourceRoot), targetRootResidual = rootResidual(targetRoot);
            SKSE::log::info("CAF_TRACE ALIGN_OBSERVATION line={} frame={} phase={} source-root-residual={} target-root-residual={} root-position-consistent={} scene-time={} frames-after-target={}",
                event.line, working.frame, phase, sourceRootResidual, targetRootResidual,
                std::isfinite(sourceRootResidual) && std::isfinite(targetRootResidual) && sourceRootResidual <= 0.05f && targetRootResidual <= 0.05f,
                scene.fragment.time, scene.fragment.diagnosticTargetFrames[index] ? working.frame - scene.fragment.diagnosticTargetFrames[index] : 0);
            const auto expected = targetBone->world.translate + ActorFrame(target.get()).rotation * event.offset;
            Log(std::format("Pose diagnostic {} line {}: scene {:.4f}, source local drift {:.4f}, target local drift {:.4f}, contact error {:.4f}; compare HKX times in CAF_TRACE",
                phase, event.line, scene.fragment.time, std::sqrt(a.GetSquaredDistance(cached.sourceContact)),
                std::sqrt(b.GetSquaredDistance(cached.targetContact)), std::sqrt(sourceBone->world.translate.GetSquaredDistance(expected))));
        }

        bool ApplyAlignment(const Command::Alignment& marker)
        {
            Scene::FragmentPlayback::Prediction result;
            return SampleAlignment(marker, result) && ApplyRelativeAlignment(marker, result.relative);
        }

        bool IsAlignment(const Command::SceneEvent& event)
        {
            return event.kind == Command::EventKind::Alignment || event.kind == Command::EventKind::PredictiveAlignment;
        }

        bool ApplyPrediction(const Command::SceneEvent& event)
        {
            const auto index = static_cast<std::size_t>(&event - scene.fragment.markers.data());
            if (index >= scene.fragment.predictions.size() || !scene.fragment.predictions[index].valid) return false;
            DiagnoseAlignment(event, "before-alignment");
            const bool applied = ApplyRelativeAlignment(event, scene.fragment.predictions[index].relative);
            DiagnoseAlignment(event, "immediate-after-alignment");
            return applied;
        }

        void StoreMarkers(const std::vector<Command::Alignment>& markers)
        {
            const auto previousMarkers = scene.fragment.markers;
            const auto previousPredictions = scene.fragment.predictions;
            const bool previousReady = scene.fragment.preview && scene.fragment.predictionsReady;
            scene.fragment.predictionsReady = false;
            scene.fragment.predicting = false;
            scene.fragment.predictions = {};
            scene.fragment.markers.clear();
            scene.fragment.fired.fill(false);
            for (const auto& marker : markers)
            {
                if (scene.fragment.markers.size() >= 128) break;
                if (SceneEventError(marker).empty()) scene.fragment.markers.push_back(marker);
            }
            std::stable_sort(scene.fragment.markers.begin(), scene.fragment.markers.end(), [](const auto& a, const auto& b) { if (a.trigger != b.trigger) return a.trigger < b.trigger; return a.time < b.time; });
            // 采样时刻、角色或骨骼变化时整组失效；偏移和旋转从缓存几何实时重算。
            std::size_t previousIndex = 0;
            bool reusable = previousReady;
            for (std::size_t index = 0; index < scene.fragment.markers.size() && reusable; ++index)
            {
                if (!IsAlignment(scene.fragment.markers[index])) continue;
                while (previousIndex < previousMarkers.size() && !IsAlignment(previousMarkers[previousIndex])) ++previousIndex;
                if (previousIndex == previousMarkers.size() ||
                    !SceneCache::SameAlignment(scene.fragment.markers[index], previousMarkers[previousIndex])) reusable = false;
                else
                {
                    auto result = previousPredictions[previousIndex++];
                    const auto& event = scene.fragment.markers[index];
                    result.relative = Alignment::Solve(result.sourceContact, result.targetContact, event.offset,
                        Alignment::RelativeRotation<RE::NiMatrix3>(event.rotation));
                    if (!result.valid || !Finite(result.relative.position)) reusable = false;
                    scene.fragment.predictions[index] = result;
                }
            }
            while (previousIndex < previousMarkers.size() && !IsAlignment(previousMarkers[previousIndex])) ++previousIndex;
            scene.fragment.predictionsReady = reusable && previousIndex == previousMarkers.size();
            if (!scene.fragment.predictionsReady) scene.fragment.predictions = {};
            scene.fragment.holdTransforms = scene.IsActive();
            if (scene.fragment.markers.size() != markers.size()) Log("Ignored invalid scene events");
        }

        bool BindArt(const Command::SceneEvent& event)
        {
            ArtBinding::Request request;
            request.actor = actors[event.source].handle;
            request.id = event.bindingID;
            request.resource = event.resource;
            request.bone = event.sourceBone;
            request.offset = event.offset;
            request.rotation = event.rotation;
            request.mode = static_cast<ArtBinding::Mode>(event.bindingMode);
            request.start = event.time;
            request.duration = event.duration;
            return ArtBinding::Bind(request);
        }

        bool EventEnabled(const Command::SceneEvent& event);

        void RebuildArt(float time)
        {
            ArtBinding::Clear();
            std::vector<SceneMath::BindingEvent> history;
            for (const auto& event : scene.fragment.markers)
                history.push_back({ event.trigger == Scene::Trigger::Time && EventEnabled(event) && event.kind == Command::EventKind::ArtBind ? SceneMath::BindingAction::Bind :
                    (event.trigger == Scene::Trigger::Time && EventEnabled(event) && event.kind == Command::EventKind::ArtUnbind ? SceneMath::BindingAction::Unbind : SceneMath::BindingAction::None),
                    event.time, event.source, event.bindingID, event.duration });
            for (std::size_t i = 0; i < history.size(); ++i)
                if (SceneMath::ActiveBinding(history, i, time)) BindArt(scene.fragment.markers[i]);
        }

        Scene::Macro SceneMacros(const std::array<RE::ActorHandle, 2>* pairedActors = nullptr)
        {
            return [pairedActors](std::string_view name, const std::vector<double>& args) -> std::optional<double>
            {
                if (name == "IsPlay" && args.empty()) return scene.IsActive() && !scene.fragment.preview;
                if (name == "GetGender" && args.size() == 1 && args[0] >= 1 && args[0] <= 8 && std::floor(args[0]) == args[0])
                {
                    const auto slot = static_cast<int>(args[0]);
                    auto handle = actors[slot + 1].handle;
                    // 应用启动计划在接管前校验，宏也必须读取本次绑定，不能使用编辑器的旧槽位。
                    if (pairedActors)
                    {
                        if (slot > 2) return {};
                        handle = (*pairedActors)[slot - 1];
                    }
                    if (auto actor = handle.get())
                        if (auto* base = actor->GetActorBase()) return static_cast<double>(base->GetSex());
                }
                return {};
            };
        }
        bool EventEnabled(const Command::SceneEvent& event)
        {
            if (event.condition.empty()) return true;
            if (scene.fragment.script) return event.condition != "0";
            std::string error;
            auto value = Scene::Evaluate(event.condition, scene.context, SceneMacros(), error);
            if (!value) Log(std::format("Scene line {}: {}; condition is false", event.line, error));
            return value && *value != 0;
        }

        void ReleaseSceneCamera();
        void ReleaseSceneCamera()
        {
            std::lock_guard lock(cameraMutex);
            overrideCamera = false;
            ClearCameraTarget();
            sceneCamera = {};
        }
        bool CameraTargetPosition(const CameraTarget& target, RE::NiPoint3& position)
        {
            auto actor = target.actor.get();
            auto* root = actor && Ready(actor.get()) ? actor->Get3D() : nullptr;
            auto* bone = root ? root->GetObjectByName(RE::BSFixedString(target.bone)) : nullptr;
            if (!bone) return false;
            // 偏移在骨骼局部空间解释，每帧重新读取节点，避免缓存上次播放的世界坐标。
            position = bone->world * target.offset;
            return Finite(position);
        }
        std::uint64_t optionGeneration = 0;
        std::size_t immediateTransitions = 0;

        bool SceneIncludesPlayer()
        {
            for (std::size_t i = 0; i < actors.size(); ++i)
                if (scene.controls.Owns(i))
                    if (auto actor = scene.Actor(i).get(); actor && actor->IsPlayerRef()) return true;
            return false;
        }

        bool SessionControlsAllowed(bool allowFrozen = false)
        {
            const bool hasPlayback = std::ranges::any_of(scene.fragment.active, [](bool active) { return active; }) ||
                std::ranges::any_of(scene.heldPlayback.active, [](bool active) { return active; });
            const bool canRestoreFreeze = allowFrozen && scene.timeFreeze.Active() && scene.IsActive() && !scene.IsFinishing();
            return Scene::SessionHotkeysAllowed(canRestoreFreeze ? Scene::SessionPhase::Playing : scene.Phase(), working.worldReady, working.paused,
                SceneIncludesPlayer(), hasPlayback, scene.fragment.preview, scene.fragment.comparison.enabled,
                scene.fragment.seekPending && !canRestoreFreeze);
        }

        void RefreshSessionControls()
        {
            if (!scene.IsActive()) return;
            if (scene.forcedFreecam)
                if (auto* camera = RE::PlayerCamera::GetSingleton(); camera && !camera->IsInFreeCameraMode())
                    Freecam::Enter(camera, true, ReleaseSceneCamera);
            if (auto* main = RE::Main::GetSingleton())
            {
                if (!SceneIncludesPlayer()) scene.timeFreeze.Restore(main->GetRuntimeData().freezeTime);
                else scene.timeFreeze.Refresh(main->GetRuntimeData().freezeTime);
            }
            if (scene.timeFreeze.Active()) working.simulationDelta = 0;
        }

        bool DispatchSceneEvent(const Command::SceneEvent& event)
        {
            if (!EventEnabled(event)) return true;
            if (event.kind == Command::EventKind::UserOption)
            {
                // 只接受真实时间轴播放；LOOP 重复派发更新同一 ID，不在 seek/预测时弹出 HUD。
                if (scene.fragment.preview || scene.fragment.comparison.enabled || scene.fragment.paused || scene.fragment.seekPending) return true;
                scene.fragment.options.Upsert(event.option);
                scene.fragment.options.Move(0, SceneIncludesPlayer());
                return true;
            }
            if (event.kind == Command::EventKind::ModifyValue)
            {
                if (scene.fragment.preview || (event.trigger == Scene::Trigger::Time && (scene.fragment.paused || scene.fragment.seekPending))) return true;
                Scene::Statement statement; statement.text = event.resource; statement.expression = event.expression;
                std::string error;
                if (!Scene::ExecuteValue(statement, scene.context, SceneMacros(), error)) Log(std::format("Scene line {}: {}", event.line, error));
                return error.empty();
            }
            if (event.kind == Command::EventKind::Goto)
            {
                // 仅提交切换意图；调用栈退出旧片段遍历后才替换事件容器。
                if (!scene.fragment.preview && !scene.IsFinishing() && !scene.fragment.paused && !scene.fragment.seekPending)
                    scene.fragment.gotoRequested = event.resource;
                return true;
            }
            if (event.kind == Command::EventKind::EndScene) { if (!scene.fragment.preview) scene.fragment.endRequested = true; return true; }
            if (event.kind == Command::EventKind::TimeScale)
            {
                // 只在真实播放派发，不在预览、预测对照或 seek 重建期间改变游戏时间。
                if (scene.fragment.preview || scene.fragment.comparison.enabled || scene.fragment.predicting ||
                    (event.trigger == Scene::Trigger::Time && (scene.fragment.paused || scene.fragment.seekPending))) return true;
                if (event.resource == "RESTORE") { scene.timeScale.Restore(); Log("Scene time scale restored"); return true; }
                const bool success = scene.timeScale.Apply(event.strength, event.duration);
                Log(std::format("Scene time scale {}: {:.3f}, original-speed duration={:.3f}s", success ? "applied" : "failed", event.strength, event.duration));
                return success;
            }
            if (event.kind == Command::EventKind::ScreenBlood || event.kind == Command::EventKind::CameraShake)
            {
                // 预览与时间轴重建不派发画面副作用，退出事件沿用现有生命周期语义。
                if (scene.fragment.preview || (event.trigger == Scene::Trigger::Time && (scene.fragment.paused || scene.fragment.seekPending))) return true;
                if (event.kind == Command::EventKind::ScreenBlood) return ScreenEffects::Blood(event.count);
                if (event.resource == "SHAKE_CAMERA") return ScreenEffects::Shake(event.strength, event.duration);
                auto actor = actors[event.source].handle.get();
                return actor && ScreenEffects::Shake(event.strength, event.duration, actor.get(),
                    event.resource == "SHAKE_CAMERA_AT_NODE" ? event.sourceBone : std::string{});
            }
            if (event.kind == Command::EventKind::Camera)
            {
                if (scene.forcedFreecam) return true;
                if (scene.fragment.preview || (event.trigger == Scene::Trigger::Time && (scene.fragment.paused || scene.fragment.seekPending))) return true;
                auto* camera = RE::PlayerCamera::GetSingleton();
                if (!camera) return false;
                if (event.resource == "FOLLOW" || event.resource == "POSITION" || event.resource == "FOCUS")
                {
                    if (scene.fragment.logic && !scene.controls.Owns(event.source)) return false;
                    CameraTarget target;
                    target.actor = scene.fragment.logic ? scene.Actor(event.source) : actors[event.source].handle;
                    target.bone = event.sourceBone; target.offset = event.offset;
                    if (!CameraTargetPosition(target, target.position)) return false;
                    target.focusing = event.resource == "FOCUS";
                    target.following = event.resource == "FOLLOW" || (target.focusing && !event.focusOnce);
                    target.pending = true;
                    ReleaseSceneCamera();
                    if (target.focusing)
                    {
                        // 聚焦使用原生第三人称输入，不进入 TFC，也不冻结鼠标朝向。
                        ExitFreecam();
                        if (camera->IsInFreeCameraMode()) camera->ToggleFreeCameraMode(false);
                        camera->ForceThirdPerson();
                        auto* state = ThirdPersonCamera(camera);
                        if (!state) return false;
                        target.savedFreeRotationEnabled = state->freeRotationEnabled;
                        target.savedFreeRotation = state->freeRotation;
                        state->SetFreeRotationMode(true);
                    }
                    std::lock_guard lock(cameraMutex);
                    cameraTarget = std::move(target);
                }
                else if (event.resource == "RELEASE") ReleaseSceneCamera();
                else if (event.resource == "FIRST") { ReleaseSceneCamera(); camera->ForceFirstPerson(); }
                else if (event.resource == "THIRD") { ReleaseSceneCamera(); camera->ForceThirdPerson(); }
                else if (event.resource == "FREE" || event.resource == "EXIT_FREE")
                {
                    const bool free = event.resource == "FREE";
                    if (free) Freecam::Enter(camera, false, ReleaseSceneCamera);
                    else
                    {
                        ReleaseSceneCamera();
                        Freecam::Release(false);
                        if (camera->IsInFreeCameraMode()) camera->ToggleFreeCameraMode(false);
                    }
                }
                else if (event.resource == "RESTORE")
                {
                    ReleaseSceneCamera();
                    if (camera->IsInFreeCameraMode() != scene.originalFreecam) camera->ToggleFreeCameraMode(false);
                    if (!scene.originalFreecam)
                    {
                        if (scene.originalFirstPerson) camera->ForceFirstPerson(); else camera->ForceThirdPerson();
                    }
                    else if (auto* state = Freecam::State(camera))
                    {
                        state->translation = scene.originalFreePosition;
                        state->rotation = scene.originalFreeRotation;
                    }
                    std::lock_guard lock(cameraMutex);
                    overrideCamera = scene.originalCameraOverride;
                    cameraPosition = scene.originalCameraPosition; cameraRotation = scene.originalCameraRotation;
                }
                else return false;
                return true;
            }
            // 释放后保留槽位身份和 context，但不再派发对该角色的片段控制/效果。
            if (!scene.fragment.active[event.source]) return true;
            if ((event.kind == Command::EventKind::Alignment || event.kind == Command::EventKind::PredictiveAlignment ||
                event.kind == Command::EventKind::Hit) && !scene.fragment.active[event.target]) return true;
            if (event.kind == Command::EventKind::PredictiveAlignment) return ApplyPrediction(event);
            if (event.kind == Command::EventKind::Alignment)
            {
                const auto index = static_cast<std::size_t>(&event - scene.fragment.markers.data());
                return scene.fragment.predictions[index].valid ? ApplyPrediction(event) : ApplyAlignment(event);
            }
            if (event.kind == Command::EventKind::ArtBind) return BindArt(event);
            if (event.kind == Command::EventKind::ArtUnbind)
            {
                ArtBinding::Unbind(actors[event.source].handle, event.bindingID);
                return true;
            }
            if (event.kind == Command::EventKind::Facial)
            {
                if (scene.fragment.comparison.enabled) return true;
                std::string error;
                const bool applied = scene.faces.Apply(event.source, actors[event.source].handle, event.facial, error);
                if (!applied) Log("Facial event: " + error);
                return applied;
            }
            // 编辑与重建只采样可逆姿态，不执行声音或死亡副作用。
            if (scene.fragment.preview || (event.trigger == Scene::Trigger::Time && (scene.fragment.paused || scene.fragment.seekPending))) return true;
            auto actor = actors[event.source].handle.get();
            if (event.kind == Command::EventKind::GTSGore)
            {
                if (!GTSGore::Enabled()) return true;
                auto killer = event.target != event.source ? actors[event.target].handle.get() : RE::NiPointer<RE::Actor>{};
                std::string error;
                const bool applied = GTSGore::Apply(actors[event.source].handle, killer.get(), event.resource, event.goreOptions, error);
                if (event.resource == "Crush" && actor && actor->IsDead()) scene.fragment.endRequested = true;
                if (!applied) Log(std::format("Scene line {}: {}", event.line, error));
                return applied;
            }
            if (event.kind == Command::EventKind::Gore || event.kind == Command::EventKind::GorePrimitive)
            {
                if (!MCGore::Enabled()) return true;
                const auto effect = MCGore::ParseEffect(event.resource);
                std::string error;
                const bool applied = event.kind == Command::EventKind::GorePrimitive ?
                    MCGore::ApplyPrimitive(actors[event.source].handle, event.resource, error) :
                    effect && MCGore::Apply(actors[event.source].handle, *effect, error);
                if (!applied)
                { Log(error.empty() ? "Unknown MC gore effect" : error); return false; }
                return true;
            }
            if (event.kind == Command::EventKind::ClearGore)
            {
                MCGore::Clear(actors[event.source].handle);
                Log(std::format("Cleared CAF MC gore for {:08X}", actor ? actor->GetFormID() : 0));
                return true;
            }

            if (!Ready(actor.get())) return false;
            if (event.kind == Command::EventKind::Footstep || event.kind == Command::EventKind::AnimationSound || event.kind == Command::EventKind::Hit)
            {
                NativeEffects::Result result;
                if (event.kind == Command::EventKind::Footstep) result = NativeEffects::Footstep(actor.get(), event.resource);
                else if (event.kind == Command::EventKind::AnimationSound) result = NativeEffects::AnimationSound(actor.get(), event.resource);
                else
                {
                    NativeEffects::HitOptions options;
                    options.mode = static_cast<NativeEffects::HitMode>(event.bindingMode);
                    options.leftHand = event.leftHand; options.bone = event.sourceBone; options.material = event.resource;
                    options.direction = event.offset; options.length = event.duration;
                    const auto target = actors[event.target].handle.get();
                    result = NativeEffects::Hit(actor.get(), target.get(), options, scene.fragment.sounds);
                }
                Log(std::format("Scene line {}: {}", event.line, result.detail));
                return result.success;
            }
            if (event.kind == Command::EventKind::Sound)
            {
                const auto key = SceneMath::ParseFormKey(event.resource);
                auto* data = RE::TESDataHandler::GetSingleton();
                auto* audio = RE::BSAudioManager::GetSingleton();
                RE::BSSoundHandle sound;
                const auto fail = [&](std::string_view reason)
                {
                    Log(std::format("Scene line {}: PLAY_SOUND '{}' failed: {}", event.line, event.resource, reason));
                    return false;
                };
                if (!audio) return fail("audio manager unavailable");
                if (key)
                {
                    auto* descriptor = data ? data->LookupForm<RE::BGSSoundDescriptorForm>(key->localID, key->plugin) : nullptr;
                    if (!descriptor) return fail("SNDR lookup failed (plugin / local FormID)");
                    if (!audio->GetSoundHandle(sound, descriptor)) return fail("SNDR sound handle creation failed");
                }
                else
                {
                    // 声音名称由音频系统解析；SNDR 的 EDID 不保证进入 TESForm 全局名称表。
                    audio->GetSoundHandleByName(sound, event.resource.c_str(), 0x1A);
                }
                if (!sound.IsValid()) return fail("invalid sound handle");
                sound.SetPosition(actor->GetPosition());
                sound.SetObjectToFollow(actor->Get3D());
                if (!sound.Play()) { sound.Stop(); return fail("Play returned false"); }
                scene.fragment.sounds.push_back(sound);
                Log(std::format("Scene line {}: PLAY_SOUND '{}' played via {} on {:08X}; handle={}",
                    event.line, event.resource, key ? "SNDR FormID" : "audio EditorID", actor->GetFormID(), sound.soundID));
                return true;
            }
            if (event.kind == Command::EventKind::Death)
            {
                // 在场景释放/loader 清理前保存终态，两帧交接独立于片段生命周期。
                const bool ragdollHandoff = IndependentPlayer::PrepareDeathHandoff(actors[event.source].handle);
                ReleaseSceneActor(event.source, false);
                auto killer = event.target != event.source ? actors[event.target].handle.get() : RE::NiPointer<RE::Actor>{};
                actor->KillImpl(killer.get(), 0.0f, true, ragdollHandoff);
                //DEATH 只释放死亡角色；其余参与者和片段时间轴继续运行。  原有的 scene.fragment.endRequested = true; 已经禁用
                Log(std::format("Death event for {:08X}: {}", actor->GetFormID(), actor->IsDead() ? "dead" : "engine did not report death"));
                return true;
            }
            return false;
        }

        void RebuildFaces(float time)
        {
            if (scene.fragment.comparison.enabled) return;
            scene.faces.RewindFragment();
            for (const auto& event : scene.fragment.markers)
                if (event.kind == Command::EventKind::Facial && event.trigger == Scene::Trigger::Time &&
                    event.time <= time && EventEnabled(event)) DispatchSceneEvent(event);
        }

        void FireFragmentLifecycle(Scene::Trigger trigger)
        {
            if (trigger == Scene::Trigger::Exit)
            {
                if (scene.fragment.exitFired) return;
                scene.fragment.exitFired = true;
            }
            if (trigger == Scene::Trigger::End) scene.fragment.endFired = true;
            for (const auto& event : scene.fragment.markers)
            {
                if (event.trigger != trigger) continue;
                if (!DispatchSceneEvent(event)) Log("Scene lifecycle event failed");
                // END/EXIT 各自是同刻批次，切换或结束只在批次返回后处理。
            }
        }
        std::optional<Scene::PlaybackPlan> PrepareSegmentPlayback(const Scene::Plan& plan, bool preview, bool hitScene, std::string& error)
        {
            auto playback = Scene::Adapt(plan, error);
            if (!playback) return {};
            if (plan.logic && preview) { error = "LOGIC states cannot be previewed or sought"; return {}; }
            const auto catalog = Animation::Paths();
            std::vector<std::string_view> catalogViews(catalog.begin(), catalog.end());
            for (std::size_t i = 2; i < actors.size(); ++i) if (playback->active[i])
            {
                auto& path = playback->settings[i].path;
                const auto resolved = Animation::ClipPath::Resolve(path, catalogViews);
                if (resolved.status == Animation::ClipPath::Status::Missing || resolved.status == Animation::ClipPath::Status::Ambiguous)
                { error = std::format("Segment {} slot {}: HKX '{}' missing or ambiguous", plan.state, i-1, path); return {}; }
                path = std::string(resolved.path);
                std::replace(path.begin(), path.end(), '/', '\\');
            }
            if (!preview)
                for (const auto& event : playback->events)
                    if (const auto invalid = SceneEventError(event); !invalid.empty()) { error = invalid; return {}; }
            if (const auto invalid = ScenePredictionError(playback->events, plan.duration > 0 ? static_cast<float>(plan.duration) : 3600); !invalid.empty())
            { error = invalid; return {}; }
            if (Scene::RequiresAnimationActors(plan.logic, !plan.immediateTarget.empty()) && std::none_of(playback->active.begin(), playback->active.end(), [](bool enabled) { return enabled; }))
            { error = "Segment requires at least one PLAY: " + plan.state; return {}; }
            if (hitScene && !plan.logic && plan.immediateTarget.empty() && (!playback->active[2] || !playback->active[3] ||
                std::any_of(playback->active.begin()+4, playback->active.end(), [](bool enabled) { return enabled; })))
            { error = "Hit scene segments must play actor 1 and actor 2 only"; return {}; }
            return playback;
        }
        bool SwitchRequestedState(bool loopRestart = false)
        {
            if (scene.fragment.gotoRequested.empty()) return false;
            const auto target = scene.fragment.gotoRequested;
            // LOOP 只重复当前片段；显式 GOTO（包括跳回同名片段）先结束旧片段。
            if (!loopRestart) FireFragmentLifecycle(Scene::Trigger::Exit);
            if (scene.fragment.endRequested) { StopScene(false); return true; }
            auto context = scene.context;
            // 跳转时只准备即将进入的片段，读取此刻的 context 和游戏宏。
            auto plan = Scene::Prepare(scene.scriptDocument, context, SceneMacros(), target, false);
            for (const auto& warning : plan.warnings) Log(std::format("Scene line {}: {}", warning.line, warning.message));
            for (const auto& diagnostic : plan.diagnostics) Log(std::format("Scene line {}: {}", diagnostic.line, diagnostic.message));
            if (!plan.diagnostics.empty()) { StopScene(false); return true; }
            std::string error;
            auto playback = PrepareSegmentPlayback(plan, false, scene.fragment.stopOnEnd, error);
            if (!playback) { Log("GOTO rejected: " + error); StopScene(false); return true; }
            Scene::FragmentDefinition definition;
            definition.playback = std::move(*playback); definition.state = plan.state;
            definition.script = true; definition.loop = plan.loop; definition.duration = static_cast<float>(plan.duration);
            definition.logic = plan.logic; definition.immediateTarget = plan.immediateTarget;
            definition.previewSource = scene.fragment.previewSource; definition.stopOnEnd = scene.fragment.stopOnEnd;
            for (std::size_t i = 0; i < actors.size(); ++i)
                if (definition.playback.active[i])
                {
                    auto actor = actors[i].handle.get();
                    if (!Ready(actor.get())) { StopScene(false); return true; }
                    for (std::size_t j = 0; j < actors.size(); ++j)
                        if (i != j && (definition.playback.active[j] || scene.controls.Owns(j)) &&
                            actors[i].handle == actors[j].handle)
                        { Log("GOTO contains duplicate actors"); StopScene(false); return true; }
                    if (!scene.controls.Owns(i))
                    {
                        if (!scene.AcquireActor(i, actors[i].handle, actors[i].locked, true))
                        { Log("GOTO actor takeover failed"); StopScene(false); return true; }
                        actors[i].locked = false;
                        actors[i].skeleton.clear();
                        CollectBones(actor->Get3D(false), actors[i].skeleton);
                    }
                }
            ArtBinding::Clear();
            if (!scene.StartFragment(definition)) { Log("GOTO fragment start failed: " + target); StopScene(false); }
            else { scene.context = std::move(context); ++optionGeneration; Log("Scene GOTO: " + plan.state); }
            return true;
        }
        void DispatchTimeBatch(float previous, float current)
        {
            const bool requested = Scene::DispatchCrossedBatch(scene.fragment.markers, scene.fragment.fired, previous, current,
                !scene.fragment.gotoRequested.empty() || scene.fragment.endRequested,
                [](const auto& event) { return event.trigger == Scene::Trigger::Time; },
                [&](const auto& event)
                {
                    scene.fragment.time = event.time;
                    if (!DispatchSceneEvent(event)) Log(std::format("Scene event at {:.3f}s failed (type {})", event.time, static_cast<int>(event.kind)));
                    return !scene.fragment.gotoRequested.empty() || scene.fragment.endRequested;
                });
            if (!requested) scene.fragment.time = current;
        }

        void DrainImmediateEntries()
        {
            // 每个中间状态都真实进入并执行 AT 0/EXIT，不能由 Prepare 折叠跳转链。
            while (scene.IsActive() && !scene.IsFinishing() && !scene.fragment.paused && !working.paused)
            {
                if (!scene.fragment.entryFired && (scene.fragment.logic || !scene.fragment.gotoRequested.empty()))
                {
                    scene.fragment.entryFired = true;
                    scene.fragment.logicEnteredFrame = working.frame;
                    DispatchTimeBatch(-0.001f, 0);
                    scene.fragment.previousTime = 0;
                }
                if (scene.fragment.endRequested) { StopScene(false); return; }
                if (scene.fragment.gotoRequested.empty()) return;
                if (!Scene::ConsumeImmediateTransition(immediateTransitions))
                { Log("Scene immediate transition limit exceeded (128 per update)"); StopScene(false); return; }
                SwitchRequestedState();
            }
        }

        bool ApplyRequestedGoto(bool loopRestart = false)
        {
            if (scene.fragment.endRequested) { StopScene(false); return true; }
            if (scene.fragment.gotoRequested.empty()) return false;
            if (!Scene::ConsumeImmediateTransition(immediateTransitions))
            { Log("Scene immediate transition limit exceeded (128 per update)"); StopScene(false); return true; }
            SwitchRequestedState(loopRestart);
            DrainImmediateEntries();
            return true;
        }

        bool ApplySelectedOption(bool atEnd)
        {
            if (scene.fragment.logic && scene.fragment.paused) return false;
            const auto nextState = scene.fragment.options.Transition(SceneIncludesPlayer(), atEnd || scene.fragment.logic);
            if (!nextState) return false;
            scene.fragment.gotoRequested = *nextState;
            return ApplyRequestedGoto();
        }

        bool SceneActorsReady()
        {
            for (std::size_t i = 0; i < actors.size(); ++i)
                if ((scene.fragment.active[i] || scene.heldPlayback.active[i]) && !Ready(scene.Actor(i).get().get())) return false;
            return true;
        }

        void ApplyCachedPreviewTransforms(float target)
        {
            // 从各自起点重放刚体变换；历史组合全程保留矩阵，最后才转回引擎角度。
            std::array<AlignmentTransform, 10> transforms;
            std::array<RE::NiPoint3, 10> angles;
            for (std::size_t i = 0; i < actors.size(); ++i)
                if (scene.fragment.active[i]) if (auto actor = actors[i].handle.get())
                    transforms[i] = ActorFrame(actor.get(), scene.fragment.startPositions[i], scene.fragment.startRotations[i]);
            for (std::size_t index = 0; index < scene.fragment.markers.size(); ++index)
            {
                const auto& event = scene.fragment.markers[index];
                if (!IsAlignment(event) || event.trigger != Scene::Trigger::Time ||
                    event.time > target || !EventEnabled(event) || !scene.fragment.predictions[index].valid) continue;
                transforms[event.source] = Alignment::Place(transforms[event.target], scene.fragment.predictions[index].relative);
                auto actor = actors[event.source].handle.get();
                if (!Ready(actor.get()) || !AlignmentAngles(actor.get(), transforms[event.source], angles[event.source]))
                { Log("Preview alignment history contains an unsupported transform; preview stopped"); StopScene(true); return; }
            }
            // 先验证整组再提交，避免玩家不支持的旋转造成一半角色已经移动。
            for (std::size_t i = 0; i < actors.size(); ++i)
                if (scene.fragment.active[i])
                {
                    auto actor = actors[i].handle.get();
                    if (!Ready(actor.get()) || !AlignmentAngles(actor.get(), transforms[i], angles[i]))
                    { Log("Preview alignment transform cannot be applied; preview stopped"); StopScene(true); return; }
                }
            for (std::size_t i = 0; i < actors.size(); ++i)
                if (scene.fragment.active[i]) if (auto actor = actors[i].handle.get())
                    SetTransform(actor.get(), transforms[i].position, angles[i]);
        }

        void SeekFragmentTo(float target)
        {
            ArtBinding::Clear();
            scene.fragment.RequestSeek(target);
            // 会话缓存就绪后直接重建最终根变换，避免显示历史采样姿态。
            if (scene.fragment.preview && scene.fragment.predictionsReady)
            {
                ApplyCachedPreviewTransforms(scene.fragment.seekTarget);
                if (!scene.IsActive()) return;
                scene.fragment.replayIndex = scene.fragment.markers.size();
            }
            for (std::size_t i = 0; i < actors.size(); ++i)
            {
                if (!scene.fragment.active[i]) continue;
                scene.fragment.settings[i].paused = true;
                Animation::Configure(actors[i].handle, scene.fragment.settings[i]);
                if (!(scene.fragment.preview && scene.fragment.predictionsReady))
                    if (auto ref = actors[i].handle.get(); Ready(ref.get())) SetTransform(ref.get(), scene.fragment.startPositions[i], scene.fragment.startRotations[i]);
            }
        }

        void Execute(const Command& command);

        void UpdateSamplingComparison(const std::vector<Animation::ClipInfo>& clips)
        {
            auto& compare = scene.fragment.comparison;
            const auto& event = scene.fragment.markers[compare.marker];
            const auto begin = [&](int stage)
            {
                compare.stage = stage; compare.elapsed = 0;
                compare.settlement = {}; compare.measurements = {};
                for (std::size_t slot = 0; slot < actors.size(); ++slot) if (scene.fragment.active[slot])
                {
                    if (stage == 1)
                    {
                        compare.roots[slot] = actors[slot].handle.get()->Get3D(false);
                        compare.scales[slot] = compare.roots[slot] ? compare.roots[slot]->world.scale : 0;
                    }
                    for (const auto& clip : clips)
                        if (clip.independent && clip.actor == actors[slot].handle.get()->GetFormID()) compare.writes[slot] = clip.poseWrites;
                    if (stage == 1)
                        IndependentPlayer::BeginSamplingPlayback(actors[slot].handle, scene.fragment.offsets[slot], event.predictionTime + scene.fragment.offsets[slot]);
                    else if (stage == 3 || stage == 4)
                        Animation::Seek(actors[slot].handle, (stage == 3 ? 0 : event.predictionTime) + scene.fragment.offsets[slot]);
                }
                Log(std::format("CAF_SAMPLE_COMPARE BEGIN stage={} target={:.6f}; actor start transforms held; scene effects suppressed", comparisonNames[stage], event.predictionTime));
            };
            const auto summarize = [&]
            {
                const std::array<int, 2> slots{ event.source, event.target };
                for (std::size_t i = 0; i < 2; ++i)
                {
                    const auto& measure = compare.measurements[i];
                    SKSE::log::info("CAF_SAMPLE_COMPARE SUMMARY stage={} actor={:08X} elapsed={} observed={} ready={} stable={} max-driver-position={} max-driver-rotation={} max-contact-drift={} max-contact-rotation={}",
                        comparisonNames[compare.stage], actors[slots[i]].handle.get()->GetFormID(), compare.elapsed,
                        measure.observed, measure.ready, measure.stable, measure.positionError, measure.rotationError, measure.contactDrift, measure.contactRotation);
                }
            };
            if (compare.stage == 0) { begin(1); return; }
            if (working.paused) return;
            for (std::size_t slot = 0; slot < actors.size(); ++slot) if (scene.fragment.active[slot])
            {
                auto* root = actors[slot].handle.get()->Get3D(false);
                if (root != compare.roots[slot] || !root || root->world.scale != compare.scales[slot])
                { Log("CAF_SAMPLE_COMPARE ABORT: body 3D or scale changed during comparison"); StopScene(true); return; }
            }
            compare.elapsed += working.realDelta;
            if (compare.stage == 1)
            {
                bool reached = true;
                for (std::size_t slot = 0; slot < actors.size(); ++slot) if (scene.fragment.active[slot])
                {
                    const auto it = std::find_if(clips.begin(), clips.end(), [&](const auto& clip)
                        { return clip.independent && clip.actor == actors[slot].handle.get()->GetFormID(); });
                    reached = reached && it != clips.end() && it->controlled && it->poseWrites > compare.writes[slot] &&
                        it->time == std::min(event.predictionTime + scene.fragment.offsets[slot], it->duration);
                }
                if (reached)
                {
                    // fence 已在 Havok Update 内停住。此处不再次 Seek，保留连续到达目标的历史。
                    compare.stage = 2; compare.elapsed = 0;
                    compare.settlement = {}; compare.measurements = {};
                    Log("CAF_SAMPLE_COMPARE BEGIN stage=played-hold; stopped at exact target without another Seek");
                }
                else if (compare.elapsed > event.predictionTime + 10)
                { Log("CAF_SAMPLE_COMPARE ABORT: continuous playback did not reach target"); StopScene(true); }
                return;
            }
            const float sampleTime = compare.stage == 3 ? 0 : event.predictionTime;
            const std::array<int, 2> slots{ event.source, event.target };
            const std::array<std::string_view, 2> bones{ event.sourceBone, event.targetBone };
            bool settled = true;
            for (std::size_t i = 0; i < 2; ++i)
            {
                const int slot = slots[i];
                auto actor = actors[slot].handle.get();
                const auto coverage = IndependentPlayer::CheckPoseCoverage(actors[slot].handle, bones[i], sampleTime + scene.fragment.offsets[slot], compare.writes[slot]);
                auto& measure = compare.measurements[i];
                ++measure.observed; measure.ready += coverage.ready;
                measure.positionError = std::max(measure.positionError, coverage.positionError);
                measure.rotationError = std::max(measure.rotationError, coverage.rotationError);
                auto* root = actor->Get3D(false);
                auto* node = root ? root->GetObjectByName(RE::BSFixedString(bones[i])) : nullptr;
                if (!node)
                { Log("CAF_SAMPLE_COMPARE ABORT: contact bone missing"); StopScene(true); return; }
                const auto frame = ActorFrame(actor.get());
                RE::NiTransform contact;
                contact.translate = Alignment::LocalContact(frame, node->world.translate);
                contact.rotate = frame.rotation.Transpose() * node->world.rotate;
                contact.scale = node->world.scale;
                if (!PoseDiagnostics::Math::SameTransform(contact, contact))
                { Log("CAF_SAMPLE_COMPARE ABORT: nonfinite contact geometry"); StopScene(true); return; }
                // 从首次实际覆盖成功建立基线，避免把零帧到目标帧的正常位移统计成抖动。
                if (!measure.hasContact && coverage.ready) { measure.firstContact = contact; measure.hasContact = true; }
                if (measure.hasContact)
                {
                    measure.contactDrift = std::max(measure.contactDrift, PoseDiagnostics::Math::Distance(measure.firstContact.translate, contact.translate));
                    measure.contactRotation = std::max(measure.contactRotation, PoseDiagnostics::Math::RotationError(measure.firstContact.rotate, contact.rotate));
                }
                if (!coverage.ready) compare.settlement[i].Reset();
                const bool stable = coverage.ready && compare.settlement[i].Observe(contact, contactSettlementOptions);
                measure.stable += stable; settled = settled && stable;
                // 前几帧保留跳转瞬态；随后降频，避免诊断 I/O 本身拖慢采样。
                if (measure.observed <= 4 || working.frame % 15 == 0)
                {
                    SKSE::log::info("CAF_SAMPLE_COMPARE OBS stage={} frame={} actor={:08X} time={} ready={} stable={} bone={} reason={} position-error={} rotation-error={} contact=({},{},{}) actor-position=({},{},{}) actor-heading={} body-scale={}",
                        comparisonNames[compare.stage], working.frame, actor->GetFormID(), sampleTime + scene.fragment.offsets[slot], coverage.ready, stable,
                        coverage.bone, coverage.reason, coverage.positionError, coverage.rotationError,
                        contact.translate.x, contact.translate.y, contact.translate.z,
                        actor->GetPosition().x, actor->GetPosition().y, actor->GetPosition().z, actor->GetAngle().z, root->world.scale);
                    PoseDiagnostics::Observe(actor.get(), working.frame, comparisonNames[compare.stage]);
                    if (measure.observed == 1 || working.frame % 60 == 0)
                        PoseDiagnostics::Contact(actor.get(), working.frame, comparisonNames[compare.stage], bones[i],
                            sampleTime + scene.fragment.offsets[slot], contact.translate, false);
                }
            }
            if (compare.elapsed < 2) return;
            if (compare.stage == 2) { summarize(); begin(3); }
            else if (compare.stage == 3)
            {
                if (settled) { summarize(); begin(4); }
                else if (compare.elapsed >= 5)
                { summarize(); Log("CAF_SAMPLE_COMPARE ABORT: zero pose never settled; warm-seek comparison invalid"); StopScene(true); }
            }
            else
            { summarize(); Log("CAF_SAMPLE_COMPARE COMPLETE: actors restored; compare played-hold and seek-hold; this sequential run includes history / elapsed-time differences"); StopScene(true); }
        }

        void UpdateFragment(const std::vector<Animation::ClipInfo>& clips)
        {
            if (!scene.IsActive() || !working.worldReady) return;
            if (scene.timeFreeze.Active()) return;
            DrainImmediateEntries();
            if (!scene.IsActive()) return;
            if (scene.fragment.logic)
            {
                if (scene.fragment.paused || working.paused) return;
                if (ApplySelectedOption(false)) return;
                // 进入所在帧只派发 AT 0，不把进入前的帧增量算到新逻辑状态。
                if (scene.fragment.logicEnteredFrame == working.frame) return;
                // 双精度累加避免无限停留时 float 逐帧增量失去精度；没有 duration 钳制。
                scene.fragment.logicTime += working.simulationDelta;
                const float next = static_cast<float>(scene.fragment.logicTime);
                DispatchTimeBatch(scene.fragment.previousTime, next);
                scene.fragment.previousTime = scene.fragment.time;
                ApplyRequestedGoto();
                return;
            }
            if (scene.fragment.predicting && !working.paused)
                working.playbackWaits.push_back("Scene: predictive pre-sampling total");
            if (!scene.fragment.seekPending && pendingPreviewEdit)
            {
                auto edit = std::move(*pendingPreviewEdit);
                pendingPreviewEdit.reset();
                Execute(edit);
                if (!scene.IsActive()) return;
            }
            const auto logClipState = [&]
            {
                for (std::size_t i = 0; i < actors.size(); ++i)
                {
                    if (!scene.fragment.active[i]) continue;
                    auto actor = actors[i].handle.get();
                    const auto it = std::find_if(clips.begin(), clips.end(), [&](const auto& clip)
                        { return actor && clip.independent && clip.actor == actor->GetFormID() && clip.path == scene.fragment.settings[i].path; });
                    if (it == clips.end()) Log(std::format("Scene actor {}: no matching independent clip", scene.fragment.script ? std::to_string(i-1) : std::string(1,static_cast<char>('A' + i))));
                    else Log(std::format("Scene actor {}: {} | loaded {} controlled {} pose writes {} time {:.3f} / {:.3f}",
                        scene.fragment.script ? std::to_string(i-1) : std::string(1,static_cast<char>('A' + i)), it->status, it->loaded, it->controlled, it->poseWrites, it->time, it->duration));
                }
            };
            if (scene.fragment.seekPending && !scene.fragment.comparison.enabled && !working.paused)
            {
                scene.fragment.sampleWait += working.realDelta;
                if (scene.fragment.sampleWait > 10.0f)
                {
                    Log("Scene sampling timed out: requested pose did not reach a settled real body; scene stopped");
                    if (!scene.fragment.sampleCoverageWait.empty()) Log(scene.fragment.sampleCoverageWait);
                    logClipState();
                    StopScene(true);
                    return;
                }
            }
            float time = -1;
            float duration = 0;
            std::array<bool, 10> loaded{};
            std::array<bool, 10> finished{};
            for (const auto& clip : clips)
            {
                if (!clip.independent || !clip.loaded) continue;
                for (std::size_t i = 0; i < actors.size(); ++i)
                {
                    if (!scene.fragment.active[i]) continue;
                    const auto& state = actors[i];
                    if (auto actor = state.handle.get(); actor && actor->GetFormID() == clip.actor && clip.path == scene.fragment.settings[i].path)
                    {
                        if (scene.fragment.script && scene.fragment.offsets[i] >= clip.duration)
                        { Log("Scene OFFSET must be less than HKX duration"); StopScene(true); return; }
                        // 时长来自已加载的 HKX；姿态控制是否就绪在采样前单独检查。
                        duration = scene.fragment.script ? std::max(duration, clip.duration - scene.fragment.offsets[i]) : (duration == 0 ? clip.duration : std::min(duration, clip.duration));
                        if (!clip.controlled || clip.weight < 0.999f) continue;
                        loaded[i] = true;
                        finished[i] = clip.atEnd;
                        if (time < 0) time = clip.time;
                        else time = std::min(time, clip.time);
                    }
                }
            }
            if (scene.fragment.explicitDuration > 0) duration = scene.fragment.explicitDuration;
            if (duration > 0) scene.fragment.duration = duration;
            bool allLoaded = true;
            for (std::size_t i = 0; i < actors.size(); ++i) if (scene.fragment.active[i] && !loaded[i]) allLoaded = false;
            if (!allLoaded)
            {
                if (!working.paused)
                {
                    for (std::size_t i = 0; i < actors.size(); ++i)
                    {
                        if (!scene.fragment.active[i] || loaded[i]) continue;
                        auto actor = actors[i].handle.get();
                        if (!actor) continue;
                        const auto clip = std::find_if(clips.begin(), clips.end(), [&](const auto& item)
                            { return actor && item.independent && item.actor == actor->GetFormID() && item.path == scene.fragment.settings[i].path; });
                        const auto reason = clip == clips.end() ? "independent clip not registered" :
                            !clip->loaded ? clip->status : !clip->controlled ? "pose control unavailable" : "blend weight below 1";
                        working.playbackWaits.push_back(std::format("Actor {:08X}: scene loading - {}", actor->GetFormID(), reason));
                    }
                }
                if (!working.paused) scene.fragment.loadingWait += working.realDelta;
                if (scene.fragment.loadingWait > 10)
                {
                    Log("Scene stopped: a participating HKX failed to load or lost pose control");
                    logClipState();
                    StopScene(true);
                }
                return;
            }
            scene.fragment.loadingWait = 0;
            if (!working.paused && !scene.fragment.seekPending && !scene.fragment.preview && ApplySelectedOption(false)) return;
            if (!working.paused && !scene.fragment.seekPending && scene.fragment.stopOnEnd && std::all_of(actors.begin(), actors.end(), [&](const auto& state)
                {
                    const auto index = static_cast<std::size_t>(&state - actors.data());
                    return !scene.fragment.active[index] || finished[index];
                }))
            {
                // 命中触发以真实 HKX 完成为准，不能依赖可能停滞的场景模拟时钟。
                if (!scene.fragment.endFired) FireFragmentLifecycle(Scene::Trigger::End);
                if (ApplyRequestedGoto()) return;
                if (!scene.fragment.endRequested && ApplySelectedOption(true)) return;
                Log("Hit scene completed: all participating HKX streams reached their end; stopping scene");
                StopScene(false);
                return;
            }
            if (scene.fragment.comparison.enabled)
            {
                UpdateSamplingComparison(clips);
                return;
            }
            scene.fragment.seekTarget = std::min(scene.fragment.seekTarget, duration);
            if (scene.fragment.preview && scene.fragment.predictionsReady)
            {
                for (std::size_t i = 0; i < actors.size(); ++i)
                {
                    if (!scene.fragment.active[i]) continue;
                    auto actor = actors[i].handle.get();
                    auto* root = actor->Get3D(false);
                    // 换装/重载导致 3D 替换或缩放变化时，不能沿用旧骨架的结果。
                    if (root != scene.fragment.predictionRoots[i] || (root && root->world.scale != scene.fragment.predictionScales[i]))
                    {
                        scene.fragment.predictionsReady = false;
                        scene.fragment.predictions = {};
                        SeekFragmentTo(scene.fragment.previewCursor);
                        Log("Preview alignment cache invalidated: actor 3D or scale changed");
                        break;
                    }
                }
            }
            if (scene.fragment.preview)
            {
                // 编辑播放始终走寻址采样，不派发正式播放的结束/退出事件。
                if (scene.fragment.previewPlaying && !working.paused && scene.fragment.predictionsReady)
                {
                    scene.fragment.previewCursor = std::min(scene.fragment.previewCursor + working.realDelta, duration);
                    if (scene.fragment.previewCursor >= duration) scene.fragment.previewPlaying = false;
                }
                if (!scene.fragment.seekPending && std::abs(scene.fragment.previewCursor - scene.fragment.time) > 0.00001f)
                    SeekFragmentTo(scene.fragment.previewCursor);
            }
            if (!scene.IsActive()) return;
            if (scene.fragment.seekPending)
            {
                if (!scene.fragment.predictionsReady && !scene.fragment.predicting)
                {
                    if (const auto error = ScenePredictionError(scene.fragment.markers, duration); !error.empty())
                    { Log(error); StopScene(true); return; }
                    const bool needed = std::any_of(scene.fragment.markers.begin(), scene.fragment.markers.end(), [](const auto& event)
                        { return IsAlignment(event) && event.trigger == Scene::Trigger::Time; });
                    scene.fragment.predicting = needed;
                    if (needed && !working.paused)
                        working.playbackWaits.push_back("Scene: predictive pre-sampling total");
                    scene.fragment.predictionsReady = !needed;
                    if (!needed)
                        for (std::size_t i = 0; i < actors.size(); ++i) if (scene.fragment.active[i])
                        {
                            scene.fragment.predictionRoots[i] = actors[i].handle.get()->Get3D(false);
                            scene.fragment.predictionScales[i] = scene.fragment.predictionRoots[i] ? scene.fragment.predictionRoots[i]->world.scale : 0;
                        }
                }
                // 每个历史对齐必须使用其自身时刻的姿态，等 Generate 输出并传播到 Ni 树后再执行。
                while (scene.fragment.replayIndex < scene.fragment.markers.size() &&
                    (!IsAlignment(scene.fragment.markers[scene.fragment.replayIndex]) ||
                     !scene.fragment.active[scene.fragment.markers[scene.fragment.replayIndex].source] ||
                     !scene.fragment.active[scene.fragment.markers[scene.fragment.replayIndex].target] ||
                     scene.fragment.markers[scene.fragment.replayIndex].trigger != Scene::Trigger::Time ||
                     (!scene.fragment.predicting && ((scene.fragment.script && scene.fragment.resumeAfterSeek) || !EventEnabled(scene.fragment.markers[scene.fragment.replayIndex]))) ||
                     scene.fragment.markers[scene.fragment.replayIndex].time > (scene.fragment.predicting ? duration : scene.fragment.seekTarget))) ++scene.fragment.replayIndex;
                const bool hasMarker = scene.fragment.replayIndex < scene.fragment.markers.size();
                if (scene.fragment.predicting && !hasMarker)
                {
                    // 已收集所有接触点，直接恢复起点；随后只回零一次，不先多采一遍零帧。
                    scene.fragment.predicting = false;
                    scene.fragment.predictionsReady = true;
                    for (std::size_t i = 0; i < actors.size(); ++i) if (scene.fragment.active[i])
                    {
                        scene.fragment.predictionRoots[i] = actors[i].handle.get()->Get3D(false);
                        scene.fragment.predictionScales[i] = scene.fragment.predictionRoots[i] ? scene.fragment.predictionRoots[i]->world.scale : 0;
                    }
                    if (scene.fragment.preview)
                    {
                        SeekFragmentTo(scene.fragment.seekTarget);
                        return;
                    }
                    scene.fragment.replayIndex = 0;
                    for (std::size_t i = 0; i < actors.size(); ++i)
                        if (scene.fragment.active[i]) if (auto ref = actors[i].handle.get()) SetTransform(ref.get(), scene.fragment.startPositions[i], scene.fragment.startRotations[i]);
                    return;
                }
                const float sampleTime = hasMarker ?
                    (scene.fragment.predicting && scene.fragment.markers[scene.fragment.replayIndex].kind == Command::EventKind::PredictiveAlignment ?
                        scene.fragment.markers[scene.fragment.replayIndex].predictionTime : scene.fragment.markers[scene.fragment.replayIndex].time) : scene.fragment.seekTarget;
                if (!scene.fragment.sampleRequested)
                {
                    working.playbackWaits.push_back(scene.fragment.predicting ?
                        "Scene: requesting next predictive sample" : "Scene: requesting initial playback pose");
                    scene.fragment.sampleTime = sampleTime;
                    scene.fragment.sampleWait = 0;
                    scene.fragment.sampleOutputWait = scene.fragment.sampleDriverWait = scene.fragment.samplePropagationWait = scene.fragment.sampleContactWait = 0;
                    scene.fragment.settledFrame = 0;
                    scene.fragment.sampleContacts = {};
                    scene.fragment.sampleContactRoots = {};
                    scene.fragment.sampleCoverageWait.clear();
                    for (std::size_t i = 0; i < actors.size(); ++i)
                    {
                        if (!scene.fragment.active[i]) continue;
                        for (const auto& clip : clips)
                            if (clip.independent && clip.actor == actors[i].handle.get()->GetFormID()) scene.fragment.sampleWrites[i] = clip.poseWrites;
                        Animation::Seek(actors[i].handle, sampleTime + scene.fragment.offsets[i]);
                    }
                    scene.fragment.sampleRequested = true;
                    return;
                }
                for (std::size_t i = 0; i < actors.size(); ++i)
                {
                    if (!scene.fragment.active[i]) continue;
                    bool sampled = false;
                    for (const auto& clip : clips)
                        if (clip.independent && clip.actor == actors[i].handle.get()->GetFormID() && clip.controlled &&
                            std::abs(clip.time - std::min(scene.fragment.sampleTime + scene.fragment.offsets[i], clip.duration)) < 0.002f && clip.poseWrites > scene.fragment.sampleWrites[i]) sampled = true;
                    if (!sampled)
                    {
                        scene.fragment.sampleOutputWait += working.realDelta;
                        working.playbackWaits.push_back(std::format("Actor {:08X}: sampled pose output not produced", actors[i].handle.get()->GetFormID()));
                        return;
                    }
                }
                // 缓存寻址不再读取骨骼世界坐标，只需确认目标姿态已写出。
                const bool validateBody = scene.fragment.predicting || (hasMarker && !scene.fragment.predictions[scene.fragment.replayIndex].valid) || scene.fragment.resumeAfterSeek;
                if (!(scene.fragment.preview && scene.fragment.predictionsReady) && !validateBody)
                {
                    if (!scene.fragment.settledFrame)
                    {
                        scene.fragment.settledFrame = working.frame;
                        working.playbackWaits.push_back("Scene: waiting for sampled pose propagation");
                        return;
                    }
                    if (working.frame <= scene.fragment.settledFrame + 1)
                    {
                        working.playbackWaits.push_back("Scene: waiting for sampled pose propagation");
                        return;
                    }
                }
                // 只有真正读取接触几何时才验收；缓存预览寻址保持原有快速路径。
                if (validateBody)
                {
                    const auto waitFor = [&](std::string reason)
                    {
                        working.playbackWaits.push_back("Scene sampling: " + reason);
                        if (scene.fragment.sampleCoverageWait != reason)
                        {
                            scene.fragment.sampleCoverageWait = reason;
                            Log("Scene sampling waiting: " + reason);
                        }
                    };
                    const auto checkDriver = [&](int slot, std::string_view bone)
                    {
                        const auto result = IndependentPlayer::CheckPoseCoverage(actors[slot].handle, bone,
                            scene.fragment.sampleTime + scene.fragment.offsets[slot], scene.fragment.sampleWrites[slot]);
                        if (!result.ready)
                        {
                            if (result.reason == "contact world transform not propagated") scene.fragment.samplePropagationWait += working.realDelta;
                            else if (result.reason == "requested independent output not ready") scene.fragment.sampleOutputWait += working.realDelta;
                            else scene.fragment.sampleDriverWait += working.realDelta;
                            waitFor(std::format("actor {:08X}: {} ({})", actors[slot].handle.get()->GetFormID(), result.reason, result.bone));
                            if (working.frame % 30 == 0)
                            {
                                SKSE::log::info("CAF_SAMPLE_WAIT actor={:08X} bone={} position-error={} rotation-error={}",
                                    actors[slot].handle.get()->GetFormID(), result.bone, result.positionError, result.rotationError);
                                SKSE::log::info("CAF_SAMPLE_PIPELINE actor={:08X} requested-time={} written-time={} writes={} after-writes={} weight={} switch-weight={} ragdoll-attached={} precision-checked={} precision-active={} written-driver=({},{},{}) actual-driver=({},{},{}) tracks={}",
                                    actors[slot].handle.get()->GetFormID(), scene.fragment.sampleTime + scene.fragment.offsets[slot], result.writtenTime,
                                    result.poseWrites, scene.fragment.sampleWrites[slot], result.weight, result.switchWeight, result.ragdollAttached, result.precisionChecked, result.precisionActive,
                                    result.writtenDriver.x, result.writtenDriver.y, result.writtenDriver.z,
                                    result.actualDriver.x, result.actualDriver.y, result.actualDriver.z, result.outputTracks);
                            }
                        }
                        return result.ready;
                    };
                    if (hasMarker)
                    {
                        const auto& event = scene.fragment.markers[scene.fragment.replayIndex];
                        const std::array<int, 2> slots{ event.source, event.target };
                        const std::array<std::string_view, 2> bones{ event.sourceBone, event.targetBone };
                        bool settled = true;
                        for (std::size_t i = 0; i < 2; ++i)
                        {
                            const int slot = slots[i];
                            // 等待时也记录整条接触链；覆盖通过后的单次日志会漏掉传播和旧姿态瞬态。
                            if (PoseDiagnostics::Enabled() && working.frame % 30 == 0)
                            {
                                auto actor = actors[slot].handle.get();
                                auto* body = actor->Get3D(false);
                                auto* bone = body ? body->GetObjectByName(RE::BSFixedString(bones[i])) : nullptr;
                                if (bone) PoseDiagnostics::Contact(actor.get(), working.frame, "sample-wait-chain", bones[i],
                                    scene.fragment.sampleTime + scene.fragment.offsets[slot], Alignment::LocalContact(ActorFrame(actor.get()), bone->world.translate), false);
                            }
                            if (!checkDriver(slot, bones[i]))
                            { for (auto& contact : scene.fragment.sampleContacts) contact.Reset(); return; }
                            auto* root = actors[slot].handle.get()->Get3D(false);
                            auto* node = root->GetObjectByName(RE::BSFixedString(bones[i]));
                            if (scene.fragment.sampleContactRoots[i] != root)
                            { scene.fragment.sampleContactRoots[i] = root; scene.fragment.sampleContacts[i].Reset(); }
                            RE::NiTransform contact;
                            // Actor heading 坐标中的实测接触几何包含体型、身体根偏移和合法关节修正。
                            const auto frame = ActorFrame(actors[slot].handle.get().get());
                            contact.translate = Alignment::LocalContact(frame, node->world.translate);
                            contact.rotate = frame.rotation.Transpose() * node->world.rotate;
                            contact.scale = node->world.scale;
                            const bool stable = scene.fragment.sampleContacts[i].Observe(contact, contactSettlementOptions);
                            if (!stable && working.frame % 30 == 0)
                                SKSE::log::info("CAF_SAMPLE_CONTACT_WAIT actor={:08X} bone={} observations={} position-drift={} position-tolerance={} required-observations={} orientation-required=false",
                                    actors[slot].handle.get()->GetFormID(), bones[i], scene.fragment.sampleContacts[i].observations,
                                    scene.fragment.sampleContacts[i].positionDrift, contactSettlementOptions.positionTolerance, contactSettlementOptions.requiredObservations);
                            settled = stable && settled;
                        }
                        if (!settled)
                        {
                            scene.fragment.sampleContactWait += working.realDelta;
                            waitFor(std::format("actual contact position is still settling ({} units, {} observations)",
                                contactSettlementOptions.positionTolerance, contactSettlementOptions.requiredObservations)); return;
                        }
                    }
                    else
                        for (std::size_t i = 0; i < actors.size(); ++i)
                            if (scene.fragment.active[i] && !checkDriver(static_cast<int>(i), {})) return;
                    if (!scene.fragment.sampleCoverageWait.empty()) Log(hasMarker && contactSettlementOptions.enabled ?
                        "Scene sampling ready: real animation drivers covered and contact geometry settled" :
                        "Scene sampling ready: real animation drivers covered before playback");
                    scene.fragment.sampleCoverageWait.clear();
                    SKSE::log::info("CAF_SAMPLE_READY time={:.3f} elapsed={:.3f}s contacts={} stability-check={} required-observations={} position-tolerance={}",
                        scene.fragment.sampleTime, scene.fragment.sampleWait, hasMarker, contactSettlementOptions.enabled,
                        contactSettlementOptions.requiredObservations, contactSettlementOptions.positionTolerance);
                    SKSE::log::info("CAF_SAMPLE_WAIT_BREAKDOWN time={:.3f} elapsed={:.3f}s output={:.3f}s driver={:.3f}s propagation={:.3f}s contact={:.3f}s precision-isolation=automatic",
                        scene.fragment.sampleTime, scene.fragment.sampleWait, scene.fragment.sampleOutputWait, scene.fragment.sampleDriverWait, scene.fragment.samplePropagationWait, scene.fragment.sampleContactWait);
                }
                scene.fragment.sampleRequested = false;
                if (hasMarker)
                {
                    const auto& event = scene.fragment.markers[scene.fragment.replayIndex];
                    const bool predicted = event.kind == Command::EventKind::PredictiveAlignment;
                    if (scene.fragment.predicting)
                    {
                        // 局部姿态与历史根变换无关；预采样只读接触几何，不提前移动真实角色。
                        if (!SampleAlignment(event, scene.fragment.predictions[scene.fragment.replayIndex]))
                        { Log("Scene alignment sampling failed: check contact bones"); StopScene(true); return; }
                        if (predicted) Log(std::format("Predicted alignment {:.3f} -> {:.3f}s for actor {:08X}",
                            event.time, event.predictionTime, actors[event.source].handle.get()->GetFormID()));
                    }
                    else if (!(scene.fragment.predictions[scene.fragment.replayIndex].valid ? ApplyPrediction(event) : ApplyAlignment(event)))
                    { Log("Scene alignment cannot be applied: check actor and rotation"); StopScene(true); return; }
                    ++scene.fragment.replayIndex;
                    return;
                }
                scene.fragment.seekPending = false;
                scene.MarkPlaying();
                scene.fragment.previousTime = scene.fragment.time = scene.fragment.seekTarget;
                if (!scene.fragment.script || !scene.fragment.resumeAfterSeek) RebuildArt(scene.fragment.time);
                if (!scene.fragment.resumeAfterSeek) RebuildFaces(scene.fragment.time);
                if (scene.fragment.resumeAfterSeek)
                {
                    // 两个图都已在零时刻完成姿态输出后，才同时释放正式播放。
                    scene.fragment.resumeAfterSeek = false;
                    scene.fragment.paused = false;
                    for (std::size_t i = 0; i < actors.size(); ++i)
                    {
                        if (!scene.fragment.active[i]) continue;
                        auto actor = actors[i].handle.get();
                        const auto clip = std::find_if(clips.begin(), clips.end(), [&](const auto& item)
                            { return actor && item.independent && item.actor == actor->GetFormID() && item.path == scene.fragment.settings[i].path; });
                        scenePlaybackWriteBaseline[i] = clip == clips.end() ? 0 : clip->poseWrites;
                    }
                    for (std::size_t i = 0; i < actors.size(); ++i)
                    {
                        if (!scene.fragment.active[i]) continue;
                        scene.fragment.settings[i].paused = false;
                        Animation::Configure(actors[i].handle, scene.fragment.settings[i]);
                    }
                    scene.fragment.entryFired = true;
                    DispatchTimeBatch(-0.001f, scene.fragment.time);
                    if (ApplyRequestedGoto()) return;
                    if (scene.fragment.endRequested) StopScene(false);
                }
                return;
            }
            // 场景时钟不跟随最短 HKX；短片段保持末帧，长片段与显式 TIME 统一结束。
            if (scene.fragment.script)
            {
                time = scene.fragment.time + ((!scene.fragment.paused && !working.paused) ? working.simulationDelta : 0.0f);
                time = std::min(time, scene.fragment.duration);
            }
            // 外部动画控制导致时间回退时不重发已执行的不可逆事件。
            if (time + 0.01f < scene.fragment.previousTime) { scene.fragment.previousTime = time; return; }
            DispatchTimeBatch(scene.fragment.previousTime, time);
            if (ApplyRequestedGoto()) return;
            scene.fragment.previousTime = time;
            scene.fragment.time = time;
            if (scene.fragment.endRequested || !SceneActorsReady()) { StopScene(false); return; }
            if (scene.fragment.script && time >= scene.fragment.duration && !scene.fragment.paused)
            {
                if (!scene.fragment.endFired) FireFragmentLifecycle(Scene::Trigger::End);
                if (ApplyRequestedGoto()) return;
                if (scene.fragment.endRequested) { StopScene(false); return; }
                if (ApplySelectedOption(true)) return;
                if (scene.fragment.loop)
                {
                    // LOOP 也开始新一轮片段；重新判断 IF，播放中不做判断。
                    // 重新准备本轮定义，但保留同一片段用户选项的身份与选择。
                    auto options = scene.fragment.options;
                    scene.fragment.gotoRequested = scene.fragment.state;
                    ApplyRequestedGoto(true);
                    if (scene.IsActive() && !scene.IsFinishing()) scene.fragment.options = std::move(options);
                }
                else
                {
                    if (scene.fragment.stopOnEnd)
                    {
                        // 命中触发不能停在末帧；自动清理姿态、隔离和输入租约，但保留结束时角色位置。
                        StopScene(false);
                    }
                    else
                    {
                        scene.fragment.paused = true;
                        for (std::size_t i = 0; i < actors.size(); ++i) if (scene.fragment.active[i])
                        { scene.fragment.settings[i].paused = true; Animation::Configure(actors[i].handle, scene.fragment.settings[i]); }
                    }
                }
            }
        }

        void Execute(const Command& command)
        {
            if (command.action == Action::ForceSessionFreecam || command.action == Action::ToggleSessionTimeFreeze)
            {
                // 队列必须再次验证会话身份；旧播放的按键不能作用于下一次播放。
                if (command.sceneGeneration != sceneStartupGeneration || !SessionControlsAllowed(command.action == Action::ToggleSessionTimeFreeze)) return;
                if (command.action == Action::ForceSessionFreecam)
                {
                    auto* camera = RE::PlayerCamera::GetSingleton();
                    if (!camera || camera->IsInFreeCameraMode()) return;
                    if (Freecam::Enter(camera, true, ReleaseSceneCamera))
                    { scene.forcedFreecam = true; Log("Session freecam forced until playback ends"); }
                }
                else if (auto* main = RE::Main::GetSingleton())
                {
                    scene.timeFreeze.Toggle(main->GetRuntimeData().freezeTime);
                    Log(scene.timeFreeze.Active() ? "Session game time frozen" : "Session game time restored");
                }
                return;
            }
            if (scene.forcedFreecam && (command.action == Action::EnterFreecam || command.action == Action::ExitFreecam ||
                command.action == Action::FirstPerson || command.action == Action::ThirdPerson || command.action == Action::SetCamera ||
                command.action == Action::ReleaseCamera || command.action == Action::DragSceneCamera)) return;
            // 新命令取消临时探针，先恢复原根变换，避免污染下一轮场景的起点。
            RestorePoseProbe();
            const int selectedSlot = command.sceneActor ? 2 + std::clamp(command.slot, 0, 7) : std::clamp(command.slot, 0, 1);
            auto& state = actors[selectedSlot];
            auto actorPtr = state.handle.get();
            auto* actor = actorPtr.get();
            if (command.action == Action::MoveUserOption || command.action == Action::SelectUserOption)
            {
                if (!scene.IsActive() || scene.IsFinishing() || scene.fragment.preview || scene.fragment.comparison.enabled ||
                    (scene.fragment.seekPending && !(scene.fragment.loop && scene.fragment.resumeAfterSeek)) ||
                    (scene.fragment.logic && scene.fragment.paused) || working.paused || command.optionGeneration != optionGeneration) return;
                const bool included = SceneIncludesPlayer();
                scene.fragment.options.Move(0, included);
                if (command.action == Action::MoveUserOption)
                {
                    if (command.text == "WS" && CameraUsesMovementKeys()) return;
                    scene.fragment.options.Move(command.value < 0 ? -1 : 1, included);
                }
                else
                {
                    scene.fragment.options.selected = scene.fragment.options.hovered;
                    if (!scene.fragment.seekPending)
                        ApplySelectedOption(scene.fragment.endFired && scene.fragment.time >= scene.fragment.duration);
                }
                return;
            }
            if (command.action == Action::ReleaseSceneActor)
            {
                // 正式播放的通用释放入口；准备/预览中的采样事务不能被中途拆除。
                if (!scene.IsActive() || scene.IsFinishing() || scene.fragment.preview || scene.fragment.seekPending) return;
                if (ReleaseSceneActor(selectedSlot, command.value != 0))
                {
                    Log(std::format("Scene actor slot {} released", selectedSlot));
                    if (!scene.fragment.logic && std::none_of(scene.fragment.active.begin(), scene.fragment.active.end(), [](bool active) { return active; }) &&
                        std::none_of(scene.heldPlayback.active.begin(), scene.heldPlayback.active.end(), [](bool active) { return active; }))
                        StopScene(false);
                }
                return;
            }
            if (command.action == Action::ResetAll)
            {
                Reset(false); Log("Restore all requested; clips restore on their next update/deactivation"); return;
            }
            if (command.action == Action::SetCameraAnchorPreview)
            {
                cameraAnchorPreview = command;
                return;
            }
            if (scene.IsActive() && scene.fragment.comparison.enabled && (command.action == Action::SeekScene || command.action == Action::SetScenePaused ||
                command.action == Action::SetSceneMarkers || command.action == Action::UpdatePreviewAlignment || command.action == Action::UpdatePreviewFacial || command.action == Action::SetPreviewPlaying))
            { Log("Sampling comparison running; stop it before editing or seeking the scene"); return; }
            if (command.action == Action::SetPhysicsIsolationOptions)
            {
                PhysicsIsolation::SetOptions(command.physicsIsolation);
                Log("Physics isolation options updated for all independent actors, scenes and previews");
                return;
            }
            if (command.action == Action::SetContactSettlementOptions)
            {
                contactSettlementOptions = command.contactSettlement.Sanitized();
                // 设置改变后重新建立基线，不能沿用旧容差或次数积累的放行资格。
                for (auto& contact : scene.fragment.sampleContacts) contact.Reset();
                for (auto& contact : scene.fragment.comparison.settlement) contact.Reset();
                scene.fragment.sampleCoverageWait.clear();
                Log(std::format("Contact stability: enabled={} observations={} tolerance={} game units",
                    contactSettlementOptions.enabled, contactSettlementOptions.requiredObservations, contactSettlementOptions.positionTolerance));
                return;
            }
            if (command.action == Action::SetPoseDiagnostics)
            {
                PoseDiagnostics::SetEnabled(command.value != 0);
                Log(command.value != 0 ? "Pose diagnostics enabled; CAF_TRACE in SKSE log" : "Pose diagnostics disabled");
                return;
            }
            if (command.action == Action::ArmSamplingComparison)
            {
                if (scene.IsActive()) { Log("Stop the current scene before arming sampling comparison"); return; }
                samplingComparisonArmed = command.value != 0;
                if (samplingComparisonArmed) PoseDiagnostics::SetEnabled(true);
                Log(samplingComparisonArmed ? "Sampling comparison armed for next predictive Play / Hit; effects suppressed, actors restored automatically" : "Sampling comparison disarmed");
                return;
            }
            if (command.action == Action::DumpPoseDiagnostics)
            {
                if (!PoseDiagnostics::Enabled()) { Log("Enable pose diagnostics first"); return; }
                for (std::size_t i = 0; i < actors.size(); ++i) if (scene.fragment.active[i])
                    PoseDiagnostics::Observe(actors[i].handle.get().get(), working.frame, "manual", true);
                return;
            }
            if (command.action == Action::ProbePoseRigidity)
            {
                if (!PoseDiagnostics::Enabled() || !scene.IsActive() || !scene.fragment.preview || scene.fragment.seekPending || scene.fragment.previewPlaying || working.paused || !scene.fragment.active[selectedSlot] || !Ready(actor))
                { Log("Rigid probe requires enabled diagnostics and a settled, stopped preview actor"); return; }
                PoseDiagnostics::BeginProbe(actor, working.frame);
                poseProbe = { state.handle, actor->GetPosition(), actor->GetAngle(), working.frame, true, false };
                auto position = poseProbe.position; position.x += 120; position.y += 60;
                auto rotation = poseProbe.rotation; rotation.z += 0.7f;
                SetTransform(actor, position, rotation);
                Log("Rigid probe: temporary +120/+60 position, +0.7 heading; restore after four engine updates");
                return;
            }
            if (scene.IsActive() && scene.fragment.comparison.enabled && (command.action == Action::SeekScene || command.action == Action::SetScenePaused ||
                command.action == Action::SetSceneMarkers || command.action == Action::UpdatePreviewAlignment || command.action == Action::UpdatePreviewFacial || command.action == Action::SetPreviewPlaying))
            { Log("Sampling comparison running; stop it before editing or seeking the scene"); return; }
            if (command.action == Action::SetPoseDiagnostics)
            {
                PoseDiagnostics::SetEnabled(command.value != 0);
                Log(command.value != 0 ? "Pose diagnostics enabled; CAF_TRACE in SKSE log" : "Pose diagnostics disabled");
                return;
            }
            if (command.action == Action::ArmSamplingComparison)
            {
                if (scene.IsActive()) { Log("Stop the current scene before arming sampling comparison"); return; }
                samplingComparisonArmed = command.value != 0;
                if (samplingComparisonArmed) PoseDiagnostics::SetEnabled(true);
                Log(samplingComparisonArmed ? "Sampling comparison armed for next predictive Play / Hit; effects suppressed, actors restored automatically" : "Sampling comparison disarmed");
                return;
            }
            if (command.action == Action::DumpPoseDiagnostics)
            {
                if (!PoseDiagnostics::Enabled()) { Log("Enable pose diagnostics first"); return; }
                for (std::size_t i = 0; i < actors.size(); ++i) if (scene.fragment.active[i])
                    PoseDiagnostics::Observe(actors[i].handle.get().get(), working.frame, "manual", true);
                return;
            }
            if (command.action == Action::ProbePoseRigidity)
            {
                if (!PoseDiagnostics::Enabled() || !scene.IsActive() || !scene.fragment.preview || scene.fragment.seekPending || scene.fragment.previewPlaying || working.paused || !scene.fragment.active[selectedSlot] || !Ready(actor))
                { Log("Rigid probe requires enabled diagnostics and a settled, stopped preview actor"); return; }
                PoseDiagnostics::BeginProbe(actor, working.frame);
                poseProbe = { state.handle, actor->GetPosition(), actor->GetAngle(), working.frame, true, false };
                auto position = poseProbe.position; position.x += 120; position.y += 60;
                auto rotation = poseProbe.rotation; rotation.z += 0.7f;
                SetTransform(actor, position, rotation);
                Log("Rigid probe: temporary +120/+60 position, +0.7 heading; restore after four engine updates");
                return;
            }
            if (command.action == Action::SetBoneProjection)
            {
                if (command.sceneActor) projectSceneBones = command.value != 0;
                else projectBones = command.value != 0;
                return;
            }
            if (!working.worldReady) { Log("Rejected: world is not ready"); return; }
            if (command.action == Action::ArmHitScene)
            {
                const auto document = Scene::Document::Parse(command.text);
                if (!document.diagnostics.empty()) { Log("Hit scene rejected: invalid scene document"); return; }
                std::array<bool, 2> hasPlay{};
                for (const auto& line : document.lines) if (line.statement.kind == Scene::Kind::Play)
                {
                    const auto slot = line.statement.actor;
                    if (slot < 1 || slot > 2) { Log("Hit scene requires PLAY on actor 1 and actor 2 only"); return; }
                    hasPlay[slot-1] = true;
                }
                if (!hasPlay[0] || !hasPlay[1]) { Log("Hit scene requires PLAY on both actor 1 and actor 2"); return; }
                if (!Ready(RE::PlayerCharacter::GetSingleton())) { Log("Hit scene requires a living player"); return; }
                CancelHitScene();
                StopScene(true);
                armedHitScene = command;
                armedHitScene->action = Action::StartScriptScene;
                armedHitScene->value = 0;
                armedHitScene->hitGeneration = ++nextHitSceneGeneration;
                hitSceneGeneration = armedHitScene->hitGeneration;
                Overlay::CloseAllMenus();
                Log("Hit scene armed: next player hit assigns player to actor 1 and living target to actor 2");
                return;
            }
            if (command.action == Action::TriggerHitScene)
            {
                // 代号隔离取消/重新待命前的命中；一次待命最多启动一次场景。
                if (!armedHitScene || command.hitGeneration != armedHitScene->hitGeneration || working.paused) return;
                auto target = command.hitActor.get();
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!Ready(player) || !Ready(target.get()) || target.get() == player) return;
                auto playback = std::move(*armedHitScene);
                playback.requestedAt = command.requestedAt;
                CancelHitScene();
                StopScene(true);
                for (int i = 2; i < 4; ++i)
                {
                    auto& participant = actors[i];
                    RestoreActor(participant, true);
                    if (auto previous = participant.handle.get())
                    {
                        previous->RemoveAnimationGraphEventSink(&graphSink);
                        Animation::Observe(participant.handle, false);
                    }
                    participant = {};
                    participant.handle = i == 2 ? player->GetHandle() : target->GetHandle();
                    AttachGraphSink(participant.handle.get().get());
                    Animation::Observe(participant.handle, true);
                }
                Log(std::format("Hit scene triggered: actor 1 player, actor 2 {} [{:08X}]", target->GetName(), target->GetFormID()));
                Execute(playback);
                return;
            }
            if (command.action == Action::StopScene)
            {
                CancelHitScene();
                StopScene(true);
                Log("Scene stopped");
                return;
            }
            if (command.action == Action::SetPreviewPlaying)
            {
                if (!scene.IsActive() || !scene.fragment.script || !scene.fragment.preview) return;
                scene.fragment.previewCursor = scene.fragment.time;
                scene.fragment.previewPlaying = command.value != 0 && scene.fragment.time < scene.fragment.duration;
                return;
            }
            if (command.action == Action::UpdatePreviewAlignment || command.action == Action::UpdatePreviewFacial)
            {
                if (!scene.IsActive() || !scene.fragment.script || !scene.fragment.preview) return;
                scene.fragment.previewPlaying = false;
                // 数值连续拖动时合并为最新草稿，先让当前采样完成，避免不断重置姿态输出。
                if (scene.fragment.seekPending && !scene.fragment.predictionsReady) { pendingPreviewEdit = command; return; }
                pendingPreviewEdit.reset();
                const auto document = Scene::Document::Parse(command.text);
                if (!document.diagnostics.empty() || command.slot < 0 || command.slot >= static_cast<int>(document.lines.size())) return;
                const auto& statement = document.lines[command.slot].statement;
                if (statement.kind != Scene::Kind::Align && statement.kind != Scene::Kind::Predict && statement.kind != Scene::Kind::Facial) return;
                const auto previous = Scene::Document::Parse(scene.fragment.previewSource);
                if (previous.lines.size() != document.lines.size()) { Log("Apply draft after inserting or deleting lines"); return; }
                for (std::size_t i = 0; i < document.lines.size(); ++i)
                    if (previous.lines[i].raw != document.lines[i].raw)
                    {
                        const auto isAlignment = [](Scene::Kind kind) { return kind == Scene::Kind::Align || kind == Scene::Kind::Predict || kind == Scene::Kind::Facial; };
                        if (!isAlignment(previous.lines[i].statement.kind) || !isAlignment(document.lines[i].statement.kind))
                        { Log("Apply draft before previewing other pending changes"); return; }
                    }
                // 对齐草稿沿用启动时冻结的条件结果；编辑参数也不能重新查询游戏宏。
                Scene::SegmentPlan plan;
                plan.state = scene.fragment.state;
                for (const auto& event : scene.fragment.markers)
                {
                    if (event.line == 0 || event.line > document.lines.size()) return;
                    plan.events.push_back({document.lines[event.line-1].statement, event.condition, event.line});
                }
                for (int i = 2; i < 10; ++i) if (scene.fragment.active[i])
                    plan.clips[i-2] = {scene.fragment.settings[i].path, scene.fragment.offsets[i], scene.fragment.settings[i].rootBone};
                std::string error;
                auto adapted = Scene::Adapt(plan, error);
                if (!adapted) { Log("Preview edit rejected: " + error); return; }
                auto markers = std::move(adapted->events);
                if (markers.size() > 128) { Log("Preview edit rejected: at most 128 events"); return; }
                for (const auto& event : markers)
                    if (const auto invalid = SceneEventError(event); !invalid.empty()) { Log("Preview edit rejected: " + invalid); return; }
                if (const auto invalid = ScenePredictionError(markers, scene.fragment.duration); !invalid.empty()) { Log("Preview edit rejected: " + invalid); return; }
                scene.fragment.previewSource = command.text;
                StoreMarkers(markers);
                scene.fragment.previewPlaying = false;
                scene.fragment.previewCursor = scene.fragment.time;
                if (scene.fragment.predictionsReady) ApplyCachedPreviewTransforms(scene.fragment.time);
                else SeekFragmentTo(scene.fragment.time);
                // 表情输入无需骨骼预采样；沿用冻结 IF 并立即重建当前预览时刻。
                if (!scene.fragment.seekPending) RebuildFaces(scene.fragment.time);
                return;
            }
            if (command.action == Action::SetSceneMarkers)
            {
                if (scene.IsActive() && scene.fragment.script) { Log("Legacy editor cannot alter a script playback plan"); return; }
                if (scene.IsActive() && !scene.fragment.preview) { Log("Edit events in preview mode"); return; }
                if (const auto error = ScenePredictionError(command.alignments, scene.fragment.duration > 0 ? scene.fragment.duration : 3600); !error.empty())
                { Log("Edit rejected: " + error); return; }
                // 旧编辑器也保留最新草稿，让必要的骨架采样完成，避免持续拖动使采样饿死。
                if (scene.IsActive() && scene.fragment.preview && scene.fragment.seekPending && !scene.fragment.predictionsReady)
                { pendingPreviewEdit = command; return; }
                pendingPreviewEdit.reset();
                StoreMarkers(command.alignments);
                if (scene.IsActive() && scene.fragment.paused)
                {
                    if (scene.fragment.predictionsReady) ApplyCachedPreviewTransforms(scene.fragment.time);
                    else SeekFragmentTo(scene.fragment.time);
                }
                return;
            }
            if (command.action == Action::StartScene || command.action == Action::StartScriptScene)
            {
                if (command.bindPair && (scene.IsActive() || armedHitScene || working.paused || !working.worldReady)) return;
                CancelHitScene();
                const bool paired = command.hitGeneration != 0 || command.bindPair;
                const bool script = command.action == Action::StartScriptScene;
                Scene::Plan plan;
                Scene::Document scriptDocument;
                std::array<bool, 10> participants{};
                auto preparedContext = command.context;
                std::vector<Command::SceneEvent> markers = command.alignments;
                std::array<Animation::Settings, 10> preparedSettings;
                std::array<bool, 10> active{};
                std::array<float, 10> offsets{};
                if (script)
                {
                    const auto document = Scene::Document::Parse(command.text);
                    scriptDocument = document;
                    const auto gameMacros = SceneMacros(command.bindPair ? &command.pairedActors : nullptr);
                    const Scene::Macro startMacros = [&](std::string_view name, const std::vector<double>& args) -> std::optional<double>
                    { if (name == "IsPlay" && args.empty()) return command.value == 0; return gameMacros(name, args); };
                    plan = Scene::Prepare(document, preparedContext, startMacros, command.value != 0 ? command.entrySegment : std::string_view{}, command.value != 0);
                    for (const auto& warning : plan.warnings) Log(std::format("Scene line {}: {}; condition is false", warning.line, warning.message));
                    for (const auto& error : plan.diagnostics) Log(std::format("Scene line {}: {}", error.line, error.message));
                    if (!plan.diagnostics.empty()) return;
                    std::string error;
                    auto playback = PrepareSegmentPlayback(plan, command.value != 0, paired, error);
                    if (!playback) { Log("Scene rejected: " + error); return; }
                    markers = std::move(playback->events); active = playback->active;
                    offsets = playback->offsets; preparedSettings = std::move(playback->settings);
                    participants = active;
                }
                else
                {
                    active[0] = active[1] = true; participants = active;
                    preparedSettings[0] = command.pairAnimations[0]; preparedSettings[1] = command.pairAnimations[1];
                }
                if (markers.size() > 128) { Log("Scene rejected: at most 128 events"); return; }
                for (const auto& event : markers)
                    if (const auto error = SceneEventError(event); command.value == 0 && !error.empty())
                    { Log("Scene rejected: " + error); return; }
                if (const auto error = ScenePredictionError(markers); !error.empty()) { Log("Scene rejected: " + error); return; }
                if (paired && !plan.logic && plan.immediateTarget.empty() && (!active[2] || !active[3] ||
                    std::any_of(active.begin()+4, active.end(), [](bool enabled) { return enabled; })))
                { Log("Hit scene rejected: resolved plan must play actor 1 and actor 2 only"); return; }
                RE::Actor* first = nullptr;
                for (std::size_t i = 0; i < actors.size(); ++i) if (participants[i])
                {
                    const auto handle = command.bindPair && i >= 2 && i < 4 ? command.pairedActors[i - 2] : actors[i].handle;
                    auto ref = handle.get();
                    if (!Ready(ref.get()) || (!script && preparedSettings[i].path.empty())) { Log("Scene slot requires a loaded actor and HKX"); return; }
                    if (!first) first = ref.get();
                    for (std::size_t j = 0; j < i; ++j) if (participants[j] &&
                        (command.bindPair && j >= 2 && j < 4 ? command.pairedActors[j - 2] : actors[j].handle) == handle)
                    { Log("Scene requires distinct actors"); return; }
                }
                if (!first && Scene::RequiresAnimationActors(plan.logic, !plan.immediateTarget.empty())) { Log("Scene requires at least one PLAY"); return; }
                StopScene(true);
                if (command.bindPair)
                {
                    for (int i = 2; i < 4; ++i)
                    {
                        auto& participant = actors[i];
                        RestoreActor(participant, true);
                        if (auto previous = participant.handle.get())
                        {
                            previous->RemoveAnimationGraphEventSink(&graphSink);
                            Animation::Observe(participant.handle, false);
                        }
                        participant = {};
                        participant.handle = command.pairedActors[i - 2];
                        AttachGraphSink(participant.handle.get().get());
                        Animation::Observe(participant.handle, true);
                    }
                }
                pendingPreviewEdit.reset();
                scene = {};
                ++optionGeneration;
                // 播放命令进入队列时开始计时，覆盖计划、加载、预采样及初始姿态恢复。
                sceneStartupAt = command.requestedAt == std::chrono::steady_clock::time_point{} ?
                    std::chrono::steady_clock::now() : command.requestedAt;
                ++sceneStartupGeneration;
                sceneStartupActive = command.value == 0;
                scenePlaybackWriteBaseline = {};
                PoseDiagnostics::BeginRun(command.value != 0 ? "preview" : command.bindPair ? "application-play" : command.hitGeneration ? "hit-play" : "editor-play");
                scene.Begin(script ? std::move(preparedContext) : scene.context);
                scene.scriptDocument = std::move(scriptDocument);
                scene.fragment.state = script ? plan.state : "default";
                scene.fragment.logic = script && plan.logic; scene.fragment.gotoRequested = plan.immediateTarget;
                scene.fragment.script = script; scene.fragment.active = active; scene.fragment.offsets = offsets;
                for (std::size_t i = 0; i < active.size(); ++i)
                    if (active[i]) actorPlaybackStarts[i] = {};
                scene.fragment.loop = script && plan.loop; scene.fragment.explicitDuration = static_cast<float>(plan.duration);
                scene.fragment.stopOnEnd = paired;
                scene.fragment.preview = command.value != 0;
                if (script) { scene.fragment.previewSource = command.text; scene.initialContext = command.context; }
                if (auto* camera = RE::PlayerCamera::GetSingleton())
                {
                    scene.originalFirstPerson = camera->IsInFirstPerson(); scene.originalFreecam = camera->IsInFreeCameraMode();
                    if (auto* state = Freecam::State(camera))
                    { scene.originalFreePosition = state->translation; scene.originalFreeRotation = state->rotation; }
                    std::lock_guard lock(cameraMutex);
                    scene.originalCameraOverride = overrideCamera;
                    scene.originalCameraPosition = cameraPosition; scene.originalCameraRotation = cameraRotation;
                }
                if (scene.fragment.preview)
                {
                    // 使用第三人称相机输出，避免游戏 TFC 与界面指针争夺鼠标。
                    ExitFreecam();
                    if (auto* camera = RE::PlayerCamera::GetSingleton())
                    {
                        if (camera->IsInFreeCameraMode()) camera->ToggleFreeCameraMode(false);
                        const bool firstPerson = camera->IsInFirstPerson();
                        if (firstPerson) camera->ForceThirdPerson();
                        std::lock_guard lock(cameraMutex);
                        sceneCamera.active = true;
                        sceneCamera.restoreFirstPerson = firstPerson;
                        sceneCamera.initialized = false;
                    }
                }
                scene.fragment.paused = true;
                scene.fragment.seekPending = true;
                scene.fragment.resumeAfterSeek = !scene.fragment.preview;
                StoreMarkers(markers);
                if (samplingComparisonArmed && !scene.fragment.preview)
                {
                    for (std::size_t index = 0; index < scene.fragment.markers.size(); ++index)
                        if (scene.fragment.markers[index].kind == Command::EventKind::PredictiveAlignment && scene.fragment.markers[index].trigger == Scene::Trigger::Time)
                        {
                            scene.fragment.comparison.enabled = true; scene.fragment.comparison.marker = index;
                            samplingComparisonArmed = false;
                            PoseDiagnostics::SetEnabled(true);
                            break;
                        }
                }
                for (std::size_t i = 0; i < actors.size(); ++i)
                {
                    if (!participants[i]) continue;
                    auto selected = actors[i].handle.get();
                    auto& settings = scene.fragment.settings[i];
                    settings = preparedSettings[i];
                    settings.independent = true;
                    settings.speed = 1;
                    settings.paused = scene.fragment.paused;
                    settings.loop = false;
                    settings.motionMode = 0;
                    settings.autoRelease = false;
                    settings.blendSeconds = 0;
                    if (!scene.AcquireActor(i, actors[i].handle, actors[i].locked,
                        !scene.fragment.preview && !scene.fragment.comparison.enabled))
                    { Log("Scene actor takeover failed"); StopScene(true); return; }
                    actors[i].locked = false;
                    actors[i].skeleton.clear();
                    CollectBones(selected->Get3D(false), actors[i].skeleton);
                    SetTransform(selected.get(), scene.controls.Saved(i).position, scene.controls.Saved(i).rotation);
                }
                auto definition = scene.fragment.Definition();
                scene.fragment = {};
                if (!scene.StartFragment(std::move(definition)))
                { Log("Scene fragment start failed"); StopScene(true); return; }
                DrainImmediateEntries();
                if (scene.IsActive()) Log(scene.fragment.logic ? "Scene LOGIC running without animation sampling" : scene.fragment.preview ? "Scene preview armed at actor start transforms" : "Scene waiting for initial poses before playback");
                return;
            }
            if (command.action == Action::SeekScene || command.action == Action::SetScenePaused)
            {
                if (!scene.IsActive()) return;
                if (scene.fragment.logic)
                {
                    if (command.action == Action::SeekScene) Log("LOGIC state has no playback seek");
                    else scene.fragment.paused = command.value != 0;
                    return;
                }
                if (command.action == Action::SeekScene)
                {
                    if (!std::isfinite(command.value) || command.value < 0) return;
                    scene.fragment.previewPlaying = false;
                    scene.fragment.previewCursor = scene.fragment.duration > 0 ? std::min(command.value,scene.fragment.duration) : command.value;
                    if (scene.fragment.preview && scene.fragment.predictionsReady && std::abs(scene.fragment.previewCursor - scene.fragment.time) < 0.00001f)
                    { ApplyCachedPreviewTransforms(scene.fragment.time); return; }
                    // 连续拖动只更新最新目标，让在途姿态采样完成，避免每帧重置而无法输出。
                    if (!scene.fragment.preview || !scene.fragment.seekPending) SeekFragmentTo(command.value);
                }
                else
                {
                    scene.fragment.paused = scene.fragment.preview || scene.fragment.seekPending || command.value != 0 ||
                        (scene.fragment.script && !scene.fragment.loop && scene.fragment.endFired && scene.fragment.time >= scene.fragment.duration);
                    for (std::size_t i = 0; i < actors.size(); ++i)
                    {
                        if (!scene.fragment.active[i]) continue;
                        scene.fragment.settings[i].paused = scene.fragment.paused;
                        Animation::Configure(actors[i].handle, scene.fragment.settings[i]);
                    }
                }
                return;
            }
            if (command.action == Action::SelectPlayer || command.action == Action::SelectConsole || command.action == Action::SelectForm)
            {
                RE::Actor* selected = nullptr;
                auto selectedRef = RE::Console::GetSelectedRef();
                if (command.action == Action::SelectPlayer) selected = RE::PlayerCharacter::GetSingleton();
                else if (command.action == Action::SelectConsole) selected = selectedRef ? selectedRef->As<RE::Actor>() : nullptr;
                else selected = RE::TESForm::LookupByID<RE::Actor>(command.form);
                if (!Ready(selected)) { Log("Selection failed: actor must be alive with loaded 3D"); return; }
                for (int i = command.sceneActor ? 2 : 0; i < (command.sceneActor ? 10 : 2); ++i)
                    if (i != selectedSlot && actors[i].handle == selected->GetHandle()) { Log("Selection rejected: slots must be different actors"); return; }
                StopScene(true);
                RestoreActor(state, true);
                if (actor) { actor->RemoveAnimationGraphEventSink(&graphSink); Animation::Observe(state.handle, false); }
                state = {};
                state.handle = selected->GetHandle();
                AttachGraphSink(selected);
                Animation::Observe(state.handle, true);
                Log(std::format("Selected {} [{:08X}]", selected->GetName(), selected->GetFormID())); return;
            }
            // 独立调试仍可控制其他角色，但不能换掉已冻结计划中演员的 HKX。
            if (scene.IsActive() && scene.fragment.script && (command.action == Action::Play || command.action == Action::PlayPair ||
                command.action == Action::Configure || command.action == Action::Stop || command.action == Action::Seek))
            {
                for (std::size_t i = 0; i < actors.size(); ++i) if (scene.controls.Owns(i))
                {
                    const bool affected = command.action == Action::PlayPair ?
                        (actors[i].handle == actors[0].handle || actors[i].handle == actors[1].handle) : actors[i].handle == state.handle;
                    if (affected) { Log("Stop the scene before changing a participating actor's animation"); return; }
                }
            }
            auto* camera = RE::PlayerCamera::GetSingleton();
            auto* map = RE::ControlMap::GetSingleton();
            switch (command.action)
            {
            case Action::DrivePlayerAI:
                if (auto* player = RE::PlayerCharacter::GetSingleton(); player && !ownsPlayerAI)
                {
                    auto* controller = player->GetActorRuntimeData().movementController.get();
                    if (!controller) { Log("Player AI drive rejected: no movement controller"); return; }
                    originalPlayerAI = controller->GetAIDriven();
                    player->SetAIDriven(true);
                    ownsPlayerAI = true;
                    Log("Player set AI-driven; original movement control mode captured");
                }
                return;
            case Action::FreezePlayer:
                if (map && !playerFrozen)
                {
                    using Flags = RE::ControlMap::UEFlag;
                    const auto mask = static_cast<std::uint32_t>(Flags::kMovement) | static_cast<std::uint32_t>(Flags::kFighting) | static_cast<std::uint32_t>(Flags::kActivate);
                    restoredControlBits = map->GetRuntimeData().enabledControls.underlying() & mask;
                    if (scene.controls.PlayerInputs().Owned()) restoredControlBits |= scene.controls.PlayerInputs().RestoreBits() & mask;
                    map->ToggleControls(static_cast<Flags>(mask), false, false);
                    playerFrozen = true; Log("Player movement/fighting/activate disabled; looking/menu retained");
                }
                return;
            case Action::RestorePlayer: RestorePlayer(); Log("Player controls restored"); return;
            case Action::EnterFreecam:
                if (camera && map && !camera->IsInFreeCameraMode())
                {
                    if (Freecam::Enter(camera, true, ReleaseSceneCamera))
                    {
                        Log("Freecam entered");
                    }
                }
                return;
            case Action::ExitFreecam: ExitFreecam(); Log("CAF freecam released"); return;
            case Action::FirstPerson: ExitFreecam(); if (camera) camera->ForceFirstPerson(); return;
            case Action::ThirdPerson: ExitFreecam(); if (camera) camera->ForceThirdPerson(); return;
            case Action::ReleaseCamera: ReleaseSceneCamera(); return;
            case Action::DragSceneCamera:
                if (scene.IsActive() && scene.fragment.preview && std::isfinite(command.position.x) && std::isfinite(command.position.y))
                {
                    std::lock_guard lock(cameraMutex);
                    ++sceneCamera.dragCommands;
                    if (!sceneCamera.active || !sceneCamera.initialized) return;
                    const float dx = std::clamp(command.position.x, -200.0f, 200.0f);
                    const float dy = std::clamp(command.position.y, -200.0f, 200.0f);
                    if (command.value == 0)
                    {
                        ++sceneCamera.panCommands;
                        // 左键沿画面右轴和上轴平移，拖动方向与场景移动方向一致。
                        constexpr float panUnitsPerPixel = 0.7f;
                        sceneCamera.position -= sceneCamera.orientation.GetVectorX() * (dx * panUnitsPerPixel);
                        sceneCamera.position += sceneCamera.orientation.GetVectorZ() * (dy * panUnitsPerPixel);
                    }
                    else if (command.value == 1)
                    {
                        constexpr float radiansPerPixel = 0.004f;
                        RE::NiMatrix3 yaw, pitch;
                        yaw.MakeZRotation(dx * radiansPerPixel);
                        const float nextPitch = std::clamp(sceneCamera.pitch + dy * radiansPerPixel, -1.45f, 1.45f);
                        pitch.MakeXRotation(nextPitch - sceneCamera.pitch);
                        sceneCamera.orientation = yaw * sceneCamera.orientation * pitch;
                        sceneCamera.pitch = nextPitch;
                    }
                }
                return;
            case Action::CaptureCamera:
                if (camera && camera->cameraRoot)
                    Log(std::format("Camera world: {:.2f}, {:.2f}, {:.2f}", camera->cameraRoot->world.translate.x, camera->cameraRoot->world.translate.y, camera->cameraRoot->world.translate.z));
                return;
            case Action::SetCamera:
                if (camera && camera->IsInFreeCameraMode() && Finite(command.position) && Finite(command.rotation))
                {
                    std::lock_guard lock(cameraMutex);
                    ClearCameraTarget();
                    cameraPosition = command.position; cameraRotation = command.rotation; overrideCamera = true;
                }
                else Log("Camera override rejected: enter freecam and supply finite coordinates first");
                return;
            default: break;
            }
            if (command.action == Action::StopProbeSounds)
            {
                NativeEventProbe::StopSounds();
                Log("Stopped retained probe sound handles");
                return;
            }
            if (!Ready(actor)) { Log("Rejected: selected actor has no usable 3D or is dead"); return; }
            switch (command.action)
            {
            case Action::ProbeNativeEvent:
            case Action::ProbeFootstep:
            case Action::ProbeSound:
            case Action::ProbeImpact:
            {
                const auto target = command.probeTarget >= 0 && command.probeTarget < 2 ? actors[command.probeTarget].handle.get() : RE::NiPointer<RE::Actor>{};
                Log(NativeEventProbe::Run(command, actor, target.get()));
                break;
            }
            case Action::RestrainNPC:
                if (actor->IsPlayerRef()) { Log("Use player controls for the player"); break; }
                if (actor->AsActorState()->GetLifeState() == RE::ACTOR_LIFE_STATE::kAlive)
                {
                    SetRestrained(actor, true);
                    state.ownsRestraint = actor->AsActorState()->GetLifeState() == RE::ACTOR_LIFE_STATE::kRestrained;
                    StopMovement(actor);
                    Log(std::format("Restrain + stop movement: restraint observed={}", state.ownsRestraint));
                }
                else Log("Restrain skipped: actor is already restrained or not in the alive state");
                break;
            case Action::StopMovement: StopMovement(actor); Log("StopMovement invoked"); break;
            case Action::FreezeNPC:
                if (actor->IsPlayerRef()) { Log("Use player input controls for the player"); break; }
                if (!state.frozen) { state.originalAI = actor->IsAIEnabled(); state.frozen = true; }
                actor->StopCombat(); actor->EnableAI(false); Log("NPC combat stopped and AI disabled"); break;
            case Action::StopCombat: actor->StopCombat(); Log("StopCombat invoked (combat state is not restorable)"); break;
            case Action::RestoreActor: RestoreActor(state); Log("Actor AI / transform lock / clip released"); break;
            case Action::CaptureTransform:
                state.savedPosition = actor->GetPosition(); state.savedRotation = actor->GetAngle(); state.captured = true;
                Log("Actor transform captured"); break;
            case Action::RestoreTransform:
                if (state.captured) { state.locked = false; SetTransform(actor, state.savedPosition, state.savedRotation); }
                else Log("Capture a transform first"); break;
            case Action::SetTransform:
                if (Finite(command.position) && Finite(command.rotation))
                {
                    SetTransform(actor, command.position, command.rotation);
                    state.lockPosition = command.position; state.lockRotation = command.rotation;
                }
                break;
            case Action::LockTransform:
                state.lockPosition = actor->GetPosition(); state.lockRotation = actor->GetAngle(); state.locked = true; break;
            case Action::UnlockTransform: state.locked = false; break;
            case Action::AlignPair:
                if (auto other = actors[1].handle.get(); Ready(other.get()))
                {
                    auto first = actors[0].handle.get();
                    if (!Ready(first.get())) { Log("Alignment requires both actors to be alive with loaded 3D"); break; }
                    const auto pos = first->GetPosition();
                    const auto rot = first->GetAngle();
                    const auto c = std::cos(rot.z), s = std::sin(rot.z);
                    RE::NiPoint3 target{ pos.x + command.position.x * c + command.position.y * s,
                        pos.y - command.position.x * s + command.position.y * c, pos.z + command.position.z };
                    SetTransform(other.get(), target, rot);
                    actors[1].lockPosition = target; actors[1].lockRotation = rot;
                    Log("B aligned to A in one main update");
                }
                break;
            case Action::QueryBone:
                if (auto* node = actor->GetNodeByName(RE::BSFixedString(command.text)))
                {
                    const auto& l = node->local.translate; const auto& w = node->world.translate;
                    RE::NiPoint3 localAngles{}, worldAngles{};
                    node->local.rotate.ToEulerAnglesXYZ(localAngles); node->world.rotate.ToEulerAnglesXYZ(worldAngles);
                    state.bone = std::format("{}\nlocal ({:.2f}, {:.2f}, {:.2f}), scale {:.3f}\nworld ({:.2f}, {:.2f}, {:.2f}), scale {:.3f}\nlocal XYZ ({:.3f}, {:.3f}, {:.3f})\nworld XYZ ({:.3f}, {:.3f}, {:.3f})", command.text,
                        l.x,l.y,l.z,node->local.scale,w.x,w.y,w.z,node->world.scale,localAngles.x,localAngles.y,localAngles.z,worldAngles.x,worldAngles.y,worldAngles.z);
                }
                else state.bone = "Bone not found: " + command.text;
                break;
            case Action::DumpSkeleton: { int count = 0; DumpNode(actor->Get3D(), 0, count); Log(std::format("Skeleton dumped to SKSE log ({} nodes, cap 2048)", count)); } break;
            case Action::ReadSkeleton: state.skeleton.clear(); CollectBones(actor->Get3D(false), state.skeleton); Log(std::format("Read {} skeleton nodes", state.skeleton.size())); break;
            case Action::SendEvent: Log(std::format("NotifyAnimationGraph({}) returned {} (not playback confirmation)", command.text, actor->NotifyAnimationGraph(RE::BSFixedString(command.text)))); break;
            case Action::ReadBool: { bool value{}; const bool ok = actor->GetGraphVariableBool(RE::BSFixedString(command.text), value); Log(std::format("bool {}: success={} value={}", command.text, ok, value)); } break;
            case Action::ReadInt: { int value{}; const bool ok = actor->GetGraphVariableInt(RE::BSFixedString(command.text), value); Log(std::format("int {}: success={} value={}", command.text, ok, value)); } break;
            case Action::ReadFloat: { float value{}; const bool ok = actor->GetGraphVariableFloat(RE::BSFixedString(command.text), value); Log(std::format("float {}: success={} value={}", command.text, ok, value)); } break;
            case Action::WriteBool: Log(std::format("Set bool: {}", actor->SetGraphVariableBool(RE::BSFixedString(command.text), command.value != 0))); break;
            case Action::WriteInt: if (std::isfinite(command.value) && command.value > -2147483648.0f && command.value < 2147483648.0f) Log(std::format("Set int: {}", actor->SetGraphVariableInt(RE::BSFixedString(command.text), static_cast<int>(command.value)))); break;
            case Action::WriteFloat: if (std::isfinite(command.value)) Log(std::format("Set float: {}", actor->SetGraphVariableFloat(RE::BSFixedString(command.text), command.value))); break;
            case Action::Play:
                actorPlaybackStarts[selectedSlot] = { actor->GetFormID(), command.requestedAt, ++nextActorPlaybackGeneration };
                Animation::Play(state.handle, command.animation);
                break;
            case Action::PlayPair:
                if (Ready(actors[0].handle.get().get()) && Ready(actors[1].handle.get().get()))
                {
                    for (std::size_t i = 0; i < 2; ++i)
                    {
                        actorPlaybackStarts[i] = { actors[i].handle.get()->GetFormID(), command.requestedAt, ++nextActorPlaybackGeneration };
                        Animation::Play(actors[i].handle, command.pairAnimations[i]);
                    }
                    Log("A/B armed in the same main update; actual Havok start times must be measured");
                }
                else Log("Pair play requires two loaded actors");
                break;
            case Action::Stop: actorPlaybackStarts[selectedSlot] = {}; Animation::Stop(state.handle); break;
            case Action::Configure: Animation::Configure(state.handle, command.animation); break;
            case Action::Seek: Animation::Seek(state.handle, command.value); break;
            case Action::DumpAnnotations: Animation::DumpAnnotations(state.handle); break;
            default: break;
            }
        }

        void Tick()
        {
            auto* ui = RE::UI::GetSingleton();
            auto* player = RE::PlayerCharacter::GetSingleton();
            working.mainMenu = ui && ui->IsMenuOpen(RE::BSFixedString("Main Menu"));
            working.loading = ui && ui->IsMenuOpen(RE::BSFixedString("Loading Menu"));
            working.paused = !ui || ui->GameIsPaused();
            working.worldReady = worldLoaded && player && player->GetParentCell() && player->Is3DLoaded() && !working.mainMenu && !working.loading;
            static REL::Relocation<float*> realDelta{ RELOCATION_ID(523661, 410200) };
            static REL::Relocation<float*> gameDelta{ RELOCATION_ID(523660, 410199) };
            working.realDelta = std::isfinite(*realDelta) ? std::max(0.0f, *realDelta) : 0;
            working.simulationDelta = working.worldReady && !working.paused && std::isfinite(*gameDelta) ? std::max(0.0f, *gameDelta) : 0;
            ++working.frame;
            immediateTransitions = 0;
            if (poseProbe.finished) RestorePoseProbe();
            if (working.mainMenu && worldLoaded.exchange(false)) Reset(true);
            if (player && player->IsDead()) CancelHitScene();
            std::deque<Command> pending;
            { std::lock_guard lock(uiMutex); pending.swap(commands); }
            for (const auto& command : pending) Execute(command);
            RefreshSessionControls();
            // 在新事件派发前扣除上一帧已减速的游戏时间；菜单与 F 冻结不消耗时长。
            const auto* mainState = RE::Main::GetSingleton();
            if (scene.IsActive() && !(mainState && mainState->GetRuntimeData().freezeTime) &&
                scene.timeScale.Update(working.simulationDelta)) Log("Scene time scale duration elapsed; restored");
            Execution::Update(working.worldReady && !working.paused && !scene.IsActive() && !armedHitScene &&
                !playerFrozen && !ownsPlayerAI);
            if (scene.IsActive() && !SceneActorsReady())
                StopScene(false);
            if (!Facial::Enabled()) scene.faces.ReleaseAll();
            IndependentPlayer::RefreshActorControls();
            // 已释放时只推进短期输入恢复诊断；租约仍持有时才清理移动输入。
            scene.controls.PlayerInputs().Refresh();
            if (scene.IsActive() && working.worldReady) scene.heldPlayback.Refresh(scene.controls);
            auto* worldCamera = working.worldReady && (projectBones || projectSceneBones) ? RE::Main::WorldRootCamera() : nullptr;
            for (std::size_t i = 0; i < actors.size(); ++i)
            {
                auto& state = actors[i];
                auto actor = state.handle.get();
                if (!working.worldReady || !Ready(actor.get())) RestoreActor(state, true);
                if (working.worldReady && Ready(actor.get()))
                {
                    if (scene.IsActive() && scene.controls.Owns(i)) scene.controls.Refresh(i);
                    // AddEventSink 自身去重；定期重绑可覆盖读图/换装备引起的 graph 重建。
                    if (working.frame % 60 == 0) AttachGraphSink(actor.get());
                    // 预测以固定角色根变换为前提，阻止玩家输入或物理漂移破坏本轮采样结果。
                    if (scene.IsActive() && scene.fragment.active[i] && scene.fragment.holdTransforms)
                    {
                        const float drift = PoseDiagnostics::Math::Distance(actor->GetPosition(), scene.fragment.heldPositions[i]);
                        if (std::isfinite(drift) && drift > 0.001f)
                        {
                            ++scene.fragment.rootCorrections[i];
                            scene.fragment.maxRootDrift[i] = std::max(scene.fragment.maxRootDrift[i], drift);
                            // 限频记录纠正前的控制器状态，区分根位移与单纯骨骼抖动。
                            if (scene.fragment.rootCorrections[i] <= 3 || scene.fragment.rootCorrections[i] % 120 == 0)
                            {
                                const auto delta = actor->GetPosition() - scene.fragment.heldPositions[i];
                                const auto* controller = actor->GetCharController();
                                SKSE::log::info("CAF_SCENE_ROOT_DRIFT actor={:08X} sample={} predicting={} collision-enabled={} controller={} drift=({}, {}, {}) controller-flags={} velocity={} push-delta={} delta-pos={}",
                                    actor->GetFormID(), scene.fragment.rootCorrections[i], scene.fragment.predicting, actor->HasCollision(),
                                    controller != nullptr, delta.x, delta.y, delta.z,
                                    controller ? controller->flags.underlying() : 0u,
                                    controller ? controller->outVelocity.Length3() : 0.0f,
                                    controller ? controller->pushDelta.Length3() : 0.0f,
                                    controller ? controller->deltaPos.Length3() : 0.0f);
                            }
                        }
                        SetTransform(actor.get(), scene.fragment.heldPositions[i], scene.fragment.heldRotations[i]);
                    }
                    if (state.locked && !working.paused) SetTransform(actor.get(), state.lockPosition, state.lockRotation);
                }
                auto& info = i < 2 ? working.actors[i] : working.sceneActors[i-2];
                info = {};
                if (actor)
                {
                    info.id = actor->GetFormID(); info.name = actor->GetName(); info.loaded = actor->Is3DLoaded();
                    info.position = actor->GetPosition(); info.rotation = actor->GetAngle(); info.aiEnabled = actor->IsAIEnabled();
                    info.frozen = state.frozen; info.locked = state.locked; info.bone = state.bone; info.skeleton = state.skeleton;
                    info.restrained = actor->AsActorState()->GetLifeState() == RE::ACTOR_LIFE_STATE::kRestrained;
                    if (worldCamera && (i < 2 ? projectBones : projectSceneBones) && Ready(actor.get()))
                    {
                        for (auto& bone : info.skeleton)
                        {
                            auto* node = actor->GetNodeByName(RE::BSFixedString(bone.name));
                            if (!node) continue;
                            bone.cameraDistanceSquared = node->world.translate.GetSquaredDistance(worldCamera->world.translate);
                            // 投影结果只进入快照；渲染线程不访问游戏节点，也不读 Win32 指针坐标。
                            bone.onScreen = worldCamera->WorldPtToScreenPt3(node->world.translate,
                                bone.screenX, bone.screenY, bone.screenDepth, 0.00001f) &&
                                std::isfinite(bone.screenX) && std::isfinite(bone.screenY) &&
                                bone.screenX >= 0 && bone.screenX <= 1 && bone.screenY >= 0 && bone.screenY <= 1;
                        }
                    }
                }
            }
            IndependentPlayer::DispatchNativeAnnotations(working.worldReady && !working.paused && !scene.fragment.preview && !scene.fragment.seekPending && !(scene.IsActive() && scene.fragment.paused));
            Animation::DrainEvents([](const Animation::Event& event) { Log(std::format("[{:08X}] {}: {}", event.actor, event.kind, event.text)); });
            working.clips = Animation::Clips();
            MCGore::Update(working.paused ? 0.0f : working.simulationDelta, working.worldReady);
            GTSGore::Update(working.paused ? 0.0f : working.simulationDelta, working.worldReady);
            // 每帧从实际阻塞分支重新收集原因，避免沿用已结束的等待。
            working.playbackWaits.clear();
            UpdateFragment(working.clips);
            working.actorPlaybackStarts.clear();
            for (auto& start : actorPlaybackStarts)
            {
                if (!start.actor) continue;
                const auto clip = std::find_if(working.clips.begin(), working.clips.end(), [&](const auto& item)
                    { return item.independent && item.actor == start.actor; });
                if (clip != working.clips.end() && clip->loaded && clip->controlled && clip->poseWrites > 0 &&
                    clip->weight >= 0.999f && clip->status == "Playing") { start = {}; continue; }
                if (working.worldReady && !working.paused)
                {
                    working.actorPlaybackStarts.push_back(start);
                    working.playbackWaits.push_back(std::format("Actor {:08X}: playback startup total", start.actor));
                }
            }
            if (sceneStartupActive)
            {
                if (!scene.IsActive() || scene.fragment.preview) sceneStartupActive = false;
                else if (!scene.fragment.seekPending && !scene.fragment.paused &&
                    std::all_of(actors.begin(), actors.end(), [&](const auto& state)
                    {
                        const auto slot = static_cast<std::size_t>(&state - actors.data());
                        if (!scene.fragment.active[slot]) return true;
                        auto actor = state.handle.get();
                        const auto clip = std::find_if(working.clips.begin(), working.clips.end(), [&](const auto& item)
                            { return actor && item.independent && item.actor == actor->GetFormID() && item.path == scene.fragment.settings[slot].path; });
                        return clip != working.clips.end() && clip->loaded && clip->controlled &&
                            clip->poseWrites > scenePlaybackWriteBaseline[slot] && clip->status == "Playing";
                    })) sceneStartupActive = false;
                else if (!working.paused) working.playbackWaits.push_back("Scene: playback startup total");
            }
            if (!working.worldReady || working.paused) working.playbackWaits.clear();
            if (working.worldReady && !working.paused)
                for (const auto& clip : working.clips)
                {
                    if (!clip.independent || clip.status == "Stopped" || clip.status == "Blending out" ||
                        clip.status == "Stop requested / blending out" ||
                        (clip.status == "Paused" && !(scene.IsActive() && scene.fragment.seekPending)) ||
                        clip.status == "Holding final pose")
                        continue;
                    const auto actorLabel = std::format("Actor {:08X}: ", clip.actor);
                    // 总耗时由 Play 命令时间戳计算；clip 状态仅提供分项原因。
                    if (!clip.loaded || !clip.controlled || clip.status == "Pending graph update" ||
                        clip.status == "Waiting for a graph containing this HKX" || clip.status == "Loading HKX" ||
                        clip.status == "Ready / awaiting pose output" || clip.status.starts_with("Interrupted:"))
                        working.playbackWaits.push_back(actorLabel + clip.status);
                    if (clip.loaded && clip.controlled && clip.weight < 0.999f)
                        working.playbackWaits.push_back(actorLabel + "waiting for full blend weight");
                    if (clip.loaded && clip.controlled && clip.poseWrites == 0 && clip.status != "Loading HKX")
                        working.playbackWaits.push_back(actorLabel + "waiting for first writable pose output");
                }
            working.cameraAnchorValid = false;
            working.cameraAnchorOnScreen = false;
            // 锚点按当前时间线姿态采样，POSITION/ONCE 也显示候选位置，不执行相机事件。
            if (working.worldReady && cameraAnchorPreview.value != 0 && cameraAnchorPreview.slot >= 1 && cameraAnchorPreview.slot <= 8)
            {
                CameraTarget target;
                target.actor = actors[cameraAnchorPreview.slot + 1].handle;
                target.bone = cameraAnchorPreview.text;
                target.offset = cameraAnchorPreview.position;
                working.cameraAnchorValid = CameraTargetPosition(target, working.cameraAnchorPosition);
                auto* projectionCamera = RE::Main::WorldRootCamera();
                if (working.cameraAnchorValid && projectionCamera)
                    working.cameraAnchorOnScreen = projectionCamera->WorldPtToScreenPt3(working.cameraAnchorPosition,
                        working.cameraAnchorScreenX, working.cameraAnchorScreenY, working.cameraAnchorScreenDepth, 0.00001f) &&
                        std::isfinite(working.cameraAnchorScreenX) && std::isfinite(working.cameraAnchorScreenY) &&
                        working.cameraAnchorScreenX >= 0 && working.cameraAnchorScreenX <= 1 &&
                        working.cameraAnchorScreenY >= 0 && working.cameraAnchorScreenY <= 1;
            }
            ArtBinding::Update(scene.fragment.time, working.paused ? 0.0f : working.realDelta, working.worldReady);
            working.artBindings = ArtBinding::Status();
            working.sceneContext = scene.context;
            working.scriptScene = scene.fragment.script;
            working.scenePlaying = scene.IsActive();
            working.sceneControlReady = SessionControlsAllowed(true);
            working.sceneForcedFreecam = scene.forcedFreecam;
            working.sceneTimeFrozen = scene.timeFreeze.Active();
            working.sceneStartupAt = sceneStartupAt;
            working.sceneStartupGeneration = sceneStartupGeneration;
            working.scenePhase = scene.Phase();
            working.fragmentState = scene.fragment.state;
            working.hitSceneArmed = armedHitScene.has_value();
            working.scenePaused = scene.fragment.paused;
            working.sceneLogic = scene.fragment.logic;
            working.scenePreview = scene.fragment.preview;
            working.sceneSeeking = scene.fragment.seekPending;
            working.previewPlaying = scene.fragment.previewPlaying;
            working.poseDiagnostics = PoseDiagnostics::Enabled();
            working.poseProbeActive = poseProbe.active;
            working.samplingComparisonArmed = samplingComparisonArmed;
            working.contactSettlement = contactSettlementOptions;
            working.physicsIsolation = PhysicsIsolation::GetOptions();
            working.samplingComparisonStage = scene.IsActive() && scene.fragment.comparison.enabled ? comparisonNames[scene.fragment.comparison.stage] : "";
            working.poseDiagnosticsStatus = PoseDiagnostics::Status();
            working.sceneTime = scene.fragment.time;
            working.sceneDuration = scene.fragment.duration;
            if (!working.worldReady || (player && player->IsDead())) RestorePlayer();
            working.playerFrozen = playerFrozen;
            auto* controller = player ? player->GetActorRuntimeData().movementController.get() : nullptr;
            working.playerAIDriven = controller && controller->GetAIDriven();
            working.mouseX = mouseX; working.mouseY = mouseY;
            auto* camera = RE::PlayerCamera::GetSingleton();
            working.freecam = camera && camera->IsInFreeCameraMode();
            if (camera && camera->cameraRoot) working.cameraPosition = camera->cameraRoot->world.translate;
            {
                std::lock_guard lock(cameraMutex);
                working.cameraOverride = overrideCamera;
                working.sceneCameraActive = sceneCamera.active;
                working.sceneCameraReady = sceneCamera.initialized;
                working.sceneCameraDragCommands = sceneCamera.dragCommands;
                working.sceneCameraPanCommands = sceneCamera.panCommands;
                working.sceneCameraPositionHook = sceneCameraPositionHookInstalled;
            }
            working.userOptions.clear();
            working.optionGeneration = optionGeneration;
            working.optionWS = !CameraUsesMovementKeys();
            working.optionInputReady = (!scene.fragment.logic || !scene.fragment.paused) &&
                (!scene.fragment.seekPending || (scene.fragment.loop && scene.fragment.resumeAfterSeek));
            if (scene.IsActive() && !scene.IsFinishing() && !scene.fragment.preview && !scene.fragment.comparison.enabled &&
                working.worldReady && !working.paused)
            {
                scene.fragment.options.Move(0, SceneIncludesPlayer());
                working.userOptions = scene.fragment.options.Visible(SceneIncludesPlayer());
            }
            working.hoveredOption = scene.fragment.options.hovered;
            working.selectedOption = scene.fragment.options.Selected(SceneIncludesPlayer()) ? scene.fragment.options.selected : "";
            { std::lock_guard lock(uiMutex); snapshot = working; }
        }
        void MainUpdate();
        REL::Relocation<decltype(MainUpdate)> originalMainUpdate;
        void MainUpdate()
        {
            Tick();
            originalMainUpdate();
            if (worldLoaded) MCGore::RefreshBones();
            if (worldLoaded && working.worldReady) IndependentPlayer::ObservePairedBodies(working.frame);
            // 在引擎传播之后检查真实 Ni 树；不把 poseWrites 当作骨骼已生效的证据。
            if (PoseDiagnostics::Enabled() && scene.IsActive() && working.worldReady)
            {
                for (std::size_t i = 0; i < actors.size(); ++i) if (scene.fragment.active[i])
                    PoseDiagnostics::Observe(actors[i].handle.get().get(), working.frame, "post-engine");
                if (working.frame % 30 == 0)
                    for (const auto& clip : IndependentPlayer::Clips())
                        for (std::size_t i = 0; i < actors.size(); ++i)
                            if (scene.fragment.active[i]) if (auto actor = actors[i].handle.get(); actor && actor->GetFormID() == clip.actor)
                                SKSE::log::info("CAF_TRACE frame={} actor={:08X} ACTIVITY scene-time={} sample-requested={} requested-time={} seek-pending={} loaded={} controlled={} weight={} poseWrites={} generate={} rootGenerate={} clip-time={} status={}",
                                    working.frame, clip.actor, scene.fragment.time, scene.fragment.sampleRequested, scene.fragment.sampleTime + scene.fragment.offsets[i], scene.fragment.seekPending,
                                    clip.loaded, clip.controlled, clip.weight, clip.poseWrites, clip.generateCalls, clip.rootGenerateCalls, clip.time, clip.status);
                if (!scene.fragment.seekPending)
                    for (std::size_t i = 0; i < scene.fragment.markers.size(); ++i)
                    {
                        const auto& event = scene.fragment.markers[i];
                        const float target = event.kind == Command::EventKind::PredictiveAlignment ? event.predictionTime : event.time;
                        if (IsAlignment(event) && event.trigger == Scene::Trigger::Time && scene.fragment.fired[i] &&
                            !scene.fragment.diagnosticTargets[i] && scene.fragment.time >= target)
                        {
                            scene.fragment.diagnosticTargets[i] = true;
                            scene.fragment.diagnosticTargetFrames[i] = working.frame;
                            DiagnoseAlignment(event, "playback-target-post-engine");
                        }
                        else if (scene.fragment.diagnosticTargetFrames[i] && working.frame > scene.fragment.diagnosticTargetFrames[i] &&
                            working.frame <= scene.fragment.diagnosticTargetFrames[i] + 8)
                            DiagnoseAlignment(event, "playback-target-followup", false);
                    }
                if (poseProbe.active && working.frame >= poseProbe.frame + 4)
                {
                    PoseDiagnostics::EndProbe(poseProbe.actor.get().get(), working.frame);
                    poseProbe.finished = true;
                }
            }
        }

        void ApplyFreecamOverride(RE::FreeCameraState* state)
        {
            std::lock_guard lock(cameraMutex);
            if (!overrideCamera || !worldLoaded) return;
            state->translation = cameraPosition;
            state->rotation.x = cameraRotation.x;
            state->rotation.y = cameraRotation.z;
            Freecam::SynchronizeRotation(state);
        }
        void SceneCameraRotation(RE::ThirdPersonState*, RE::NiQuaternion&);
        void SceneCameraTranslation(RE::ThirdPersonState*, RE::NiPoint3&);
        void SceneCameraUpdate(RE::ThirdPersonState*, RE::BSTSmartPointer<RE::TESCameraState>&);
        REL::Relocation<decltype(SceneCameraRotation)> originalSceneCameraRotation;
        REL::Relocation<decltype(SceneCameraTranslation)> originalSceneCameraTranslation;
        REL::Relocation<decltype(SceneCameraUpdate)> originalSceneCameraUpdate;

        void InitializeSceneCamera()
        {
            if (!sceneCamera.active || sceneCamera.initialized ||
                !sceneCamera.rotationCaptured || !sceneCamera.translationCaptured) return;
            sceneCamera.orientation = sceneCamera.initialRotation.ToRotation();
            sceneCamera.pitch = std::asin(std::clamp(sceneCamera.orientation.GetVectorY().z, -1.0f, 1.0f));
            sceneCamera.initialized = true;
        }

        void SceneCameraRotation(RE::ThirdPersonState* state, RE::NiQuaternion& rotation)
        {
            originalSceneCameraRotation(state, rotation);
            std::lock_guard lock(cameraMutex);
            if (sceneCamera.active && !sceneCamera.initialized)
            {
                sceneCamera.initialRotation = rotation;
                sceneCamera.rotationCaptured = true;
                InitializeSceneCamera();
            }
            if (sceneCamera.active && sceneCamera.initialized) rotation.SetRotation(sceneCamera.orientation);
        }

        void SceneCameraTranslation(RE::ThirdPersonState* state, RE::NiPoint3& translation)
        {
            originalSceneCameraTranslation(state, translation);
            std::lock_guard lock(cameraMutex);
            if (sceneCamera.active && !sceneCamera.initialized)
            {
                sceneCamera.position = translation;
                sceneCamera.translationCaptured = true;
                InitializeSceneCamera();
            }
            if (sceneCamera.active && sceneCamera.initialized) translation = sceneCamera.position;
        }

        void SceneCameraUpdate(RE::ThirdPersonState* state, RE::BSTSmartPointer<RE::TESCameraState>& next)
        {
            {
                std::lock_guard lock(cameraMutex);
                if (cameraTarget.focusing) state->freeRotationEnabled = true;
            }
            originalSceneCameraUpdate(state, next);
            std::lock_guard lock(cameraMutex);
            if (!sceneCamera.active) return;
            if (!sceneCamera.initialized)
            {
                // 第三人称状态自身始终更新；getter 未被调用时也能取得初始相机姿态。
                sceneCamera.initialRotation = state->rotation;
                sceneCamera.position = state->translation;
                sceneCamera.rotationCaptured = sceneCamera.translationCaptured = true;
                InitializeSceneCamera();
            }
            state->translation = sceneCamera.position;
            state->rotation.SetRotation(sceneCamera.orientation);
        }
        using PlayerCameraUpdateFn = void(*)(RE::PlayerCamera*);
        PlayerCameraUpdateFn originalPlayerCameraUpdate = nullptr;

        void PlayerCameraUpdate(RE::PlayerCamera* camera)
        {
            originalPlayerCameraUpdate(camera);
            Freecam::ObserveCamera(camera);
            if (!camera || !camera->cameraRoot) return;
            RE::NiPoint3 position;
            {
                std::lock_guard lock(cameraMutex);
                if (cameraTarget.following || cameraTarget.pending || cameraTarget.focusing)
                {
                    if (cameraTarget.following && !CameraTargetPosition(cameraTarget, cameraTarget.position))
                    { ClearCameraTarget(); return; }
                    position = cameraTarget.position;
                    if (cameraTarget.focusing)
                    {
                        // 切换到其他原生相机状态后不再覆盖；FOCUS ONCE 仅固定中心，环绕仍逐帧计算。
                        if (!camera->IsInThirdPerson()) { ClearCameraTarget(); return; }
                        auto nativeActor = camera->cameraTarget.get();
                        auto* nativeRoot = nativeActor ? nativeActor->Get3D() : nullptr;
                        auto* strings = RE::FixedStrings::GetSingleton();
                        auto* pivot = nativeRoot && strings ? nativeRoot->GetObjectByName(strings->camera3rd) : nullptr;
                        if (!pivot && nativeRoot) pivot = nativeRoot->GetObjectByName(RE::BSFixedString("Camera3rd [Cam3]"));
                        if (!pivot)
                        {
                            ClearCameraTarget();
                            Log("FOCUS released: native third-person camera bone is unavailable");
                            return;
                        }
                        // 读取原生更新刚生成的实际半径，保留滚轮缩放，不把归一化 zoom 当成世界距离。
                        const float distance = std::sqrt(camera->cameraRoot->world.translate.GetSquaredDistance(pivot->world.translate));
                        auto forward = camera->cameraRoot->world.rotate.GetVectorY();
                        const float length = std::sqrt(forward.SqrLength());
                        if (!std::isfinite(distance) || !std::isfinite(length) || length < 0.0001f) return;
                        position -= forward * (std::max(distance, 10.0f) / length);
                        if (!Finite(position)) return;
                    }
                    // 一次定位只写入本次更新；自由相机保留新位置后仍允许原生移动，朝向始终不写入。
                    cameraTarget.pending = false;
                    if (camera->IsInFreeCameraMode())
                        if (auto* state = Freecam::State(camera)) state->translation = position;
                }
                else
                {
                    if (!sceneCamera.active || !sceneCamera.initialized) return;
                    position = sceneCamera.position;
                }
            }
            // 玩家相机更新会重算跟随位置；在其完成后将编辑平移写回相机节点。
            auto* root = camera->cameraRoot.get();
            root->local.translate = root->parent ? root->parent->world.Invert() * position : position;
            RE::NiUpdateData update{};
            update.flags = RE::NiUpdateData::Flag::kDirty;
            root->UpdateWorldData(&update);
        }
    }

    void Install()
    {
        // OAR f4e7688 / Hooks.h 的主循环 call site；保留已有插件的调用链。
        // Main、loader、movement 与 ragdoll 两个 call hook 各需 14 字节。
        SKSE::AllocTrampoline(70);
        REL::Relocation<std::uintptr_t> main{ RELOCATION_ID(35565, 36564) };
        const auto aeOffset = REL::Module::get().version() >= REL::Version{ 1, 7, 99, 0 } ? 0xC38 : 0xC26;
        originalMainUpdate = SKSE::GetTrampoline().write_call<5>(main.address() + REL::VariantOffset(0x748, aeOffset, 0).offset(), MainUpdate);
        Freecam::Install(ApplyFreecamOverride);
        REL::Relocation<std::uintptr_t> thirdPerson{ RE::VTABLE_ThirdPersonState[0] };
        originalSceneCameraUpdate = thirdPerson.write_vfunc(0x3, SceneCameraUpdate);
        originalSceneCameraRotation = thirdPerson.write_vfunc(0x4, SceneCameraRotation);
        originalSceneCameraTranslation = thirdPerson.write_vfunc(0x5, SceneCameraTranslation);
        Animation::Install();
    }
    bool CameraUsesMovementKeys()
    {
        // 后续新的键盘相机控制模式统一加在这里，HUD 与输入判断共享同一规则。
        auto* camera = RE::PlayerCamera::GetSingleton();
        std::lock_guard lock(cameraMutex);
        return camera && camera->IsInFreeCameraMode() && !overrideCamera && !cameraTarget.following && !cameraTarget.pending;
    }

    void InstallSceneCameraHook()
    {
        REL::Relocation<std::uintptr_t> update{ RELOCATION_ID(49852, 50784) };
        auto* target = reinterpret_cast<void*>(update.address());
        if (MH_CreateHook(target, &PlayerCameraUpdate,
                reinterpret_cast<void**>(&originalPlayerCameraUpdate)) != MH_OK ||
            MH_EnableHook(target) != MH_OK)
            SKSE::log::error("CAF scene camera position hook installation failed");
        else sceneCameraPositionHookInstalled = true;
    }
    void OnMessage(SKSE::MessagingInterface::Message* message)
    {
        switch (message->type)
        {
        case SKSE::MessagingInterface::kDataLoaded:
            Execution::Install();
            if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) events->AddEventSink(&hitSceneSink);
            break;
        case SKSE::MessagingInterface::kPreLoadGame: Execution::Reset(); Reset(true); worldLoaded = false; break;
        case SKSE::MessagingInterface::kPostLoadGame: worldLoaded = message->data != nullptr; break;
        case SKSE::MessagingInterface::kNewGame: Execution::Reset(); Reset(true); worldLoaded = true; break;
        default: break;
        }
    }
    void Enqueue(Command command)
    {
        std::lock_guard lock(uiMutex);
        if (command.action == Action::StartScene || command.action == Action::StartScriptScene ||
            command.action == Action::TriggerHitScene || command.action == Action::Play || command.action == Action::PlayPair)
            command.requestedAt = std::chrono::steady_clock::now();
        if (commands.size() < 128) commands.push_back(std::move(command));
    }
    bool StartPairedScene(std::string source, RE::ActorHandle first, RE::ActorHandle second)
    {
        if (!working.worldReady || working.paused || scene.IsActive() || armedHitScene || !Ready(first.get().get()) ||
            !Ready(second.get().get()) || first == second) return false;
        Command command;
        command.action = Action::StartScriptScene;
        command.text = std::move(source);
        command.pairedActors = {first, second};
        command.bindPair = true;
        command.requestedAt = std::chrono::steady_clock::now();
        Execute(command);
        return scene.IsActive();
    }
    Snapshot ReadSnapshot() { std::lock_guard lock(uiMutex); return snapshot; }
    UserOptionSnapshot ReadUserOptions()
    {
        // HUD 和输入每帧只复制选项，避免连带复制角色骨骼和诊断日志。
        std::lock_guard lock(uiMutex);
        return { snapshot.userOptions, snapshot.hoveredOption, snapshot.selectedOption, snapshot.optionGeneration,
            snapshot.optionWS, snapshot.optionInputReady, snapshot.worldReady, snapshot.paused };
    }
    SceneControlSnapshot ReadSceneControls()
    {
        std::lock_guard lock(uiMutex);
        return { snapshot.sceneControlReady, snapshot.freecam, snapshot.sceneForcedFreecam,
            snapshot.sceneTimeFrozen, snapshot.sceneStartupGeneration };
    }
    void RecordMouse(std::int32_t x, std::int32_t y) { mouseX += x; mouseY += y; }
}
