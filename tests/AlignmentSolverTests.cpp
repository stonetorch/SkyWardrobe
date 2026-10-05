#include <cstddef>
#include <RE/N/NiMatrix3.h>
#include <RE/N/NiPoint3.h>
#include "scene/AlignmentSolver.h"
#include "animation/PoseDiagnosticMath.h"
#include "animation/PoseSampling.h"
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>

namespace
{
    using Point = RE::NiPoint3;
    using Matrix = RE::NiMatrix3;
    using Transform = CAF::Alignment::Transform<Point, Matrix>;
    namespace Alignment = CAF::Alignment;
    constexpr float pi = std::numbers::pi_v<float>;

    void Require(bool result, const char* message)
    {
        if (!result) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
    }
    bool Near(const Point& a, const Point& b, float tolerance = 0.003f)
    {
        return std::abs(a.x - b.x) < tolerance && std::abs(a.y - b.y) < tolerance && std::abs(a.z - b.z) < tolerance;
    }
    bool Near(const Matrix& a, const Matrix& b)
    {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                if (std::abs(a.entry[i][j] - b.entry[i][j]) > 0.00001f) return false;
        return true;
    }
    Matrix EngineRotation(const Point& angles)
    {
        Matrix result;
        result.SetEulerAnglesXYZ(angles);
        return result;
    }
    Point WorldContact(const Transform& actor, const Point& local)
    {
        return actor.position + actor.rotation * local;
    }
    Transform EngineRoundTrip(const Transform& actor)
    {
        return { actor.position, EngineRotation(Alignment::EngineAngles<Point>(actor.rotation)) };
    }

    void InvarianceAndContact()
    {
        const std::array<Point, 7> starts{{ {}, { 0, 0, pi }, { .3f, -.8f, 1.2f },
            { -.9f, .4f, -2.1f }, { 0, pi / 2, .8f }, { .5f, -pi / 2, -.3f }, { pi, 0, 0 } }};
        const std::array<Point, 5> rotations{{ {}, { 0, 0, pi }, { .7f, -.4f, 1.3f },
            { 0, pi / 2, 0 }, { pi, .2f, -.5f } }};
        const Point offset{ 7, -11, 23 };
        const Matrix sourceRootRotation = EngineRotation({ .4f, .7f, -.2f });
        const Matrix targetRootRotation = EngineRotation({ -.8f, .1f, 1.4f });
        // 非零根偏移、骨架根旋转、不同体型比例全部包含在角色局部接触点中。
        for (const float sourceScale : { .6f, 1.0f, 1.7f })
            for (const float targetScale : { .8f, 1.3f })
            {
                const Point a = Point{ 13, -4, 8 } + sourceRootRotation * (Point{ 21, 16, 83 } * sourceScale);
                const Point b = Point{ -6, 19, 3 } + targetRootRotation * (Point{ -15, 34, 62 } * targetScale);
                for (const auto& rot : rotations)
                {
                    const Matrix relativeRotation = Alignment::RelativeRotation<Matrix>(rot);
                    const auto reference = Alignment::Solve(a, b, offset, relativeRotation);
                    for (std::size_t i = 0; i < starts.size(); ++i)
                        for (std::size_t j = 0; j < starts.size(); ++j)
                        {
                            const Transform source{ { 900.0f * i, -350.0f * j, 43 }, EngineRotation(starts[i]) };
                            const Transform target{ { -470.0f * j, 610.0f * i, -37 }, EngineRotation(starts[j]) };
                            const auto sampledA = Alignment::LocalContact(source, WorldContact(source, a));
                            const auto sampledB = Alignment::LocalContact(target, WorldContact(target, b));
                            const auto relative = Alignment::Solve(sampledA, sampledB, offset, relativeRotation);
                            Require(Near(relative.position, reference.position), "independent initial transforms must not affect relative translation");
                            Require(Near(relative.rotation, reference.rotation), "independent initial transforms must not affect relative rotation");
                            const auto result = EngineRoundTrip(Alignment::Place(target, relative));
                            Require(Near(WorldContact(result, a), WorldContact(target, b + offset)), "contact remains fixed while editing all rotation axes");
                            Require(Near(target.rotation.Transpose() * result.rotation, relativeRotation), "relative orientation survives world placement and engine conversion");
                        }
                }
            }
    }

    void EditingAndHistory()
    {
        const Point sourceContact{ 17, -9, 80 }, targetContact{ -12, 30, 56 };
        const Transform target{ { 120, -500, 83 }, EngineRotation({ .4f, -.3f, 1.7f }) };
        const auto original = Alignment::Solve(sourceContact, targetContact, Point{}, Alignment::RelativeRotation<Matrix>(Point{ 0, 0, pi }));
        const auto source = EngineRoundTrip(Alignment::Place(target, original));
        const Point offset{ -8, 19, 3 }, angles{ .2f, .6f, -1.1f };
        const auto cached = Alignment::Solve(sourceContact, targetContact, offset, Alignment::RelativeRotation<Matrix>(angles));
        const auto resampled = Alignment::Solve(Alignment::LocalContact(source, WorldContact(source, sourceContact)),
            Alignment::LocalContact(target, WorldContact(target, targetContact)), offset, Alignment::RelativeRotation<Matrix>(angles));
        Require(Near(cached.position, resampled.position) && Near(cached.rotation, resampled.rotation), "cached parameter editing equals a fresh sample");

        // A 相对 B，B 再相对 C；预览内存重放与正式播放逐次写回欧拉角必须一致。
        const auto first = Alignment::Solve(Point{ 5, 2, 31 }, Point{ -3, 12, 74 }, Point{ 9, 0, -6 },
            Alignment::RelativeRotation<Matrix>(Point{ .8f, -.7f, 1.9f }));
        const auto previewB = Alignment::Place(target, first);
        const auto previewA = Alignment::Place(previewB, cached);
        const auto playbackB = EngineRoundTrip(previewB);
        const auto playbackA = EngineRoundTrip(Alignment::Place(playbackB, cached));
        Require(Near(previewA.position, playbackA.position) && Near(previewA.rotation, playbackA.rotation), "preview and playback compose the same alignment history");
        Require(Near(WorldContact(playbackA, sourceContact), WorldContact(playbackB, targetContact + offset)), "chained alignment preserves contact after multi-axis target rotation");
        const auto replay = Alignment::Place(Alignment::Place(target, first), cached);
        Require(Near(replay.position, previewA.position) && Near(replay.rotation, previewA.rotation), "backward seek rebuilds from original transforms without accumulation");
    }

    void RotationConvention()
    {
        const auto facing = Alignment::RelativeRotation<Matrix>(Point{ 0, 0, pi });
        Require(Near(facing * Point{ 0, 1, 0 }, Point{ 0, -1, 0 }), "face each other reverses forward");
        Require(Near(facing * Point{ 0, 0, 1 }, Point{ 0, 0, 1 }), "face each other preserves up");
        Require(Near(Alignment::RelativeRotation<Matrix>(Point{ 0, 0, pi / 2 }) * Point{ 0, 1, 0 }, Point{ 1, 0, 0 }), "positive yaw follows Skyrim heading convention");
        for (float x : { -.9f, 0.0f, .6f, pi })
            for (float y : { -pi / 2, -pi / 2 + .00001f, -.3f, 0.0f, .8f, pi / 2 - .00001f, pi / 2 })
                for (float z : { -2.4f, 0.0f, 1.3f, pi })
                {
                    const auto matrix = EngineRotation({ x, y, z });
                    Require(Near(EngineRotation(Alignment::EngineAngles<Point>(matrix)), matrix), "engine Euler round trip including both gimbal-lock endpoints");
                }
        Require(Alignment::HeadingOnly(facing), "player accepts a facing yaw");
        Require(!Alignment::HeadingOnly(EngineRotation({ .01f, 0, 0 })), "player must reject pitch instead of silently dropping it");
        Require(!Alignment::HeadingOnly(EngineRotation({ pi, 0, 0 })), "player must reject upside-down transforms");
    }
}

int main()
{
    // NPC 瞄准俯仰不应改变同一身体姿态的接触缓存；实际身体倾斜仍包含在接触点里。
    {
        const Point position{ 300, -500, 80 }, local{ 25, 10, 40 };
        const auto heading = Alignment::BodyHeading<Matrix>(Point{ 0, 0, 0.7f });
        const auto aiming = Alignment::BodyHeading<Matrix>(Point{ 1.05f, -0.4f, 0.7f });
        const Point world = position + heading * local;
        Require(Near(heading, aiming), "NPC aiming pitch and roll do not tilt the body heading basis");
        Require(Near(Alignment::LocalContact(Transform{ position, aiming }, world), local), "aiming changes preserve measured body contact");
        Require(!Near(Alignment::LocalContact(Transform{ position, EngineRotation({ 1.05f, -0.4f, 0.7f }) }, world), local), "regression reproduces fake contact drift with full logical rotation");
    }
    {
        RE::NiTransform expected, actual;
        expected.translate = { -70.143f, 105.054f, 17.624f };
        actual = expected; actual.scale = 1.25f;
        Require(CAF::Sampling::MatchesDriver(expected, actual), "body scale differences do not reject pose coverage");
        actual.translate = { 63.893f, 3.788f, 40.767f };
        Require(!CAF::Sampling::MatchesDriver(expected, actual), "written pose counters cannot accept the observed old COM pose");
        actual = expected; actual.rotate.MakeZRotation(0.2f);
        Require(!CAF::Sampling::MatchesDriver(expected, actual), "driver rotation mismatch blocks sampling even with correct translation");
        actual = expected; actual.rotate.MakeZRotation(0.02f);
        Require(CAF::Sampling::MatchesDriver(expected, actual, true), "minor engine root tilt does not indefinitely block sampling");
        Require(!CAF::Sampling::MatchesDriver(expected, actual), "COM still rejects a minor pose mismatch");
        actual.rotate.MakeZRotation(0.2f);
        Require(!CAF::Sampling::MatchesDriver(expected, actual, true), "large body root pose errors still block sampling");
        Require(CAF::Sampling::BodyDriver({ "NPC Root [Root]", "Camera3rd [Cam3]", "NPC COM [COM ]", "Head" }, { -1, 0, 0, 2 }) == 2,
            "rewind coverage uses the body branch rather than the camera sibling");
        Require(CAF::Sampling::BodyDriver({ "Root", "Camera", "Body", "Spine", "Head" }, { -1, 0, 0, 2, 3 }) == 2,
            "nonstandard skeleton selects the branch carrying the body hierarchy");
        Require(CAF::Sampling::IsPoseReaction("staggerStart") && CAF::Sampling::IsPoseReaction("recoilStart"), "independent playback filters external pose reactions");
        Require(!CAF::Sampling::IsPoseReaction("Death") && !CAF::Sampling::IsPoseReaction("SoundPlay.NPCHumanHit") &&
            !CAF::Sampling::IsPoseReaction("weaponSwing") && !CAF::Sampling::IsPoseReaction("staggerStop"),
            "death, native effects and reaction cleanup events remain available");
        CAF::Sampling::ContactSettlement settlement;
        Require(!settlement.Observe(expected) && !settlement.Observe(expected) && settlement.Observe(expected), "contact geometry requires multiple settled observations");
        actual = expected; actual.translate.x += 0.3f;
        Require(settlement.Observe(actual), "small contact changes stay within the original stable baseline");
        actual.rotate.MakeZRotation(0.7f); actual.scale = 1.2f;
        Require(settlement.Observe(actual), "contact orientation and scale changes alone do not block positional alignment");
        actual.translate.x += 0.3f;
        Require(!settlement.Observe(actual), "accumulating contact drift resets settlement");
        settlement.Reset(); actual = expected; actual.translate.x += 150;
        Require(!settlement.Observe(expected) && !settlement.Observe(actual), "large old-pose movement cannot pass settlement");
        actual.translate.x = std::numeric_limits<float>::quiet_NaN();
        Require(!settlement.Observe(actual) && !settlement.Observe(actual), "invalid geometry never settles");

        CAF::Sampling::ContactSettlementOptions options;
        options.requiredObservations = 1;
        settlement.Reset();
        Require(settlement.Observe(expected, options), "one observation accepts the first valid contact");
        options.requiredObservations = 2; options.positionTolerance = 0.25f;
        settlement.Reset(); actual = expected; actual.translate.x += 0.25f;
        Require(!settlement.Observe(expected, options) && settlement.Observe(actual, options), "configured tolerance includes its boundary");
        actual.translate.x += 0.25f;
        Require(!settlement.Observe(actual, options), "configured tolerance still rejects accumulated baseline drift");
        options.positionTolerance = 0;
        settlement.Reset();
        Require(!settlement.Observe(expected, options) && settlement.Observe(expected, options), "zero tolerance accepts identical contact positions");
        options.enabled = false; actual = expected; actual.translate.x += 150;
        Require(settlement.Observe(actual, options), "disabled stability check does not wait for moving contacts");
        actual.translate.x = std::numeric_limits<float>::quiet_NaN();
        Require(!settlement.Observe(actual, options), "disabled stability check still rejects nonfinite geometry");
        options.requiredObservations = -1; options.positionTolerance = std::numeric_limits<float>::infinity();
        const auto sanitized = options.Sanitized();
        Require(sanitized.requiredObservations == 1 && sanitized.positionTolerance == 0.5f, "invalid settlement options are sanitized");
    }
    // 实际诊断使用的残差函数：刚体跟随、世界固定、父链断开和非有限值必须可区分。
    {
        namespace Math = CAF::PoseDiagnostics::Math;
        RE::NiTransform before, after, child;
        before.translate = { 200, -300, 40 };
        after.translate = { -50, 600, 90 }; after.rotate.MakeZRotation(0.7f);
        child.translate = { 10, 20, 30 }; child.scale = 1.3f;
        const auto worldBefore = before * child;
        const auto worldAfter = after * child;
        Require(Math::SameTransform((after * before.Invert()) * worldBefore, worldAfter), "rigid follower passes diagnostic residual");
        Require(!Math::SameTransform(worldBefore, worldAfter), "world-fixed bone is detected after actor relocation");
        auto broken = worldAfter; broken.translate.x += 2;
        Require(!Math::SameTransform(after * child, broken), "parent-local-world discontinuity is detected");
        auto delayed = child; delayed.rotate.MakeZRotation(0.4f);
        Require(!Math::SameTransform(child, delayed), "pose rotation overwrite is detected without translation changes");
        broken.translate.x = std::numeric_limits<float>::quiet_NaN();
        Require(!Math::SameTransform(broken, broken), "nonfinite transforms never count as successful coverage");
    }
    InvarianceAndContact();
    EditingAndHistory();
    RotationConvention();
    std::cout << "PASS alignment invariance, contact pivot, cache/history and engine rotation conversion\n";
}
