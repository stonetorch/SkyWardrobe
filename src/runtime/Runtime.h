#pragma once

#include "animation/Animation.h"
#include "animation/ContactSettlementOptions.h"
#include "animation/PhysicsIsolationOptions.h"
#include "scene/SceneDocument.h"
#include "scene/PlaybackEvent.h"
#include "scene/SessionLifecycle.h"
#include "features/GTSGoreOptions.h"
#include <array>
#include <chrono>

namespace CAF
{
    enum class Action
    {
        SelectPlayer, SelectConsole, SelectForm, FreezeNPC, RestrainNPC, StopMovement, RestoreActor, StopCombat,
        FreezePlayer, DrivePlayerAI, RestorePlayer, CaptureTransform, RestoreTransform, SetTransform,
        LockTransform, UnlockTransform, AlignPair, QueryBone, DumpSkeleton,
        SendEvent, ReadBool, ReadInt, ReadFloat, WriteBool, WriteInt, WriteFloat,
        ProbeNativeEvent, ProbeFootstep, ProbeSound, ProbeImpact, StopProbeSounds,
        Play, PlayPair, Stop, Configure, Seek, DumpAnnotations,
        StartScene, StartScriptScene, ArmHitScene, TriggerHitScene, StopScene, SeekScene, SetScenePaused, SetSceneMarkers, ReadSkeleton, SetBoneProjection, ReleaseSceneActor,
        UpdatePreviewAlignment, UpdatePreviewFacial, SetPreviewPlaying, SetPoseDiagnostics, DumpPoseDiagnostics, ProbePoseRigidity, ArmSamplingComparison, SetCameraAnchorPreview,
        EnterFreecam, ExitFreecam, FirstPerson, ThirdPerson, CaptureCamera, SetCamera,
        ForceSessionFreecam, ToggleSessionTimeFreeze,
        MoveUserOption, SelectUserOption, ReleaseCamera, ResetAll, DragSceneCamera, SetContactSettlementOptions, SetPhysicsIsolationOptions
    };
    struct Command
    {
        Action action{};
        std::chrono::steady_clock::time_point requestedAt{};
        int slot = 0;
        bool sceneActor = false; // 新编辑器独立的 1..8 槽位，不改变 Debug A/B。
        Scene::Context context;
        RE::FormID form = 0;
        RE::ActorHandle hitActor;
        // 应用提供双角色绑定；运行时先验证场景，再接管槽位及播放生命周期。
        std::array<RE::ActorHandle, 2> pairedActors;
        bool bindPair = false;
        std::uint64_t hitGeneration = 0;
        std::uint64_t optionGeneration = 0;
        std::uint64_t sceneGeneration = 0;
        std::string text;
        std::string entrySegment; // 预览入口；正式播放默认从首片段开始。
        float value = 0;
        Sampling::ContactSettlementOptions contactSettlement;
        PhysicsIsolation::Options physicsIsolation;
        std::string probeNode;
        int probeTarget = 1;
        bool probeLeftHand = false;
        RE::NiPoint3 position{};
        RE::NiPoint3 rotation{};
        Animation::Settings animation;
        std::array<Animation::Settings, 2> pairAnimations;
        using EventKind = Scene::EventKind;
        using SceneEvent = Scene::SceneEvent;
        using Alignment = SceneEvent;
        std::vector<SceneEvent> alignments;
    };
    struct ActorInfo
    {
        RE::FormID id = 0;
        std::string name;
        bool loaded = false;
        bool frozen = false;
        bool restrained = false;
        bool locked = false;
        bool aiEnabled = false;
        RE::NiPoint3 position{};
        RE::NiPoint3 rotation{};
        std::string bone;
        struct BoneInfo
        {
            std::string name;
            int depth = 0;
            int parent = -1;
            float screenX = 0;
            float screenY = 0;
            float screenDepth = 0;
            float cameraDistanceSquared = 0;
            bool onScreen = false;
        };
        std::vector<BoneInfo> skeleton;
    };
    struct UserOptionSnapshot
    {
        std::vector<Scene::UserOption> userOptions;
        std::string hoveredOption, selectedOption;
        std::uint64_t optionGeneration = 0;
        bool optionWS = false, optionInputReady = false;
        bool worldReady = false, paused = true;
    };
    struct SceneControlSnapshot
    {
        bool ready = false, freecam = false, forcedFreecam = false, timeFrozen = false;
        std::uint64_t generation = 0;
    };
    struct Snapshot
    {
        struct ActorPlaybackStart
        {
            RE::FormID actor = 0;
            std::chrono::steady_clock::time_point startedAt{};
            std::uint64_t generation = 0;
        };
        bool cameraAnchorValid = false, cameraAnchorOnScreen = false;
        RE::NiPoint3 cameraAnchorPosition{};
        float cameraAnchorScreenX = 0, cameraAnchorScreenY = 0, cameraAnchorScreenDepth = 0;
        std::vector<Scene::UserOption> userOptions;
        std::string hoveredOption, selectedOption;
        std::uint64_t optionGeneration = 0;
        bool optionWS = false;
        bool optionInputReady = false;
        bool worldReady = false;
        bool paused = true;
        bool mainMenu = false;
        bool loading = false;
        bool playerFrozen = false;
        bool playerAIDriven = false;
        bool freecam = false;
        bool cameraOverride = false;
        bool sceneCameraActive = false;
        bool sceneCameraReady = false;
        std::uint64_t sceneCameraDragCommands = 0;
        std::uint64_t sceneCameraPanCommands = 0;
        bool sceneCameraPositionHook = false;
        float realDelta = 0;
        float simulationDelta = 0;
        std::uint64_t frame = 0;
        std::int64_t mouseX = 0;
        std::int64_t mouseY = 0;
        RE::NiPoint3 cameraPosition{};
        std::array<ActorInfo, 2> actors;
        std::array<ActorInfo, 8> sceneActors;
        Scene::Context sceneContext;
        bool scriptScene = false;
        std::vector<Animation::ClipInfo> clips;
        bool scenePlaying = false;
        bool sceneControlReady = false, sceneForcedFreecam = false, sceneTimeFrozen = false;
        Scene::SessionPhase scenePhase = Scene::SessionPhase::Idle;
        std::string fragmentState;
        std::vector<std::string> playbackWaits;
        std::vector<ActorPlaybackStart> actorPlaybackStarts;
        std::chrono::steady_clock::time_point sceneStartupAt{};
        std::uint64_t sceneStartupGeneration = 0;
        bool hitSceneArmed = false;
        bool scenePaused = false;
        bool sceneLogic = false;
        bool scenePreview = false;
        bool sceneSeeking = false;
        bool previewPlaying = false;
        bool poseDiagnostics = false;
        bool poseProbeActive = false;
        bool samplingComparisonArmed = false;
        Sampling::ContactSettlementOptions contactSettlement;
        PhysicsIsolation::Options physicsIsolation;
        std::string samplingComparisonStage;
        std::vector<std::string> poseDiagnosticsStatus;
        float sceneTime = 0;
        float sceneDuration = 0;
        std::vector<std::string> artBindings;
        std::vector<std::string> messages;
    };
    void Install();
    void InstallSceneCameraHook();
    void OnMessage(SKSE::MessagingInterface::Message* message);
    void Enqueue(Command command);
    // 主线程应用入口：不打断正在运行的场景，完成后自动释放控制。
    bool StartPairedScene(std::string source, RE::ActorHandle first, RE::ActorHandle second);
    Snapshot ReadSnapshot();
    UserOptionSnapshot ReadUserOptions();
    SceneControlSnapshot ReadSceneControls();
    bool CameraUsesMovementKeys();
    void RecordMouse(std::int32_t x, std::int32_t y);
    void DrawDebug();
    bool DrawSceneWindows();
}
