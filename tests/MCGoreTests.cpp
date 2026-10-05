#include "features/MCGorePlan.h"

using namespace CAF::MCGore;

// 配方在编译阶段检查，不依赖 Skyrim 或执行测试程序。
static_assert([]
{
    for (std::size_t i = 0; i < choices.size(); ++i)
    {
        const auto& choice = choices[i];
        if (ParseEffect(choice.code) != choice.effect || static_cast<std::size_t>(choice.effect) != i) return false;
        for (std::size_t j = 0; j < i; ++j) if (choices[j].code == choice.code) return false;
        const auto plan = Plan(choice.effect);
        bool equipment = false, blood = false;
        for (const auto& step : plan)
        {
            if (step.time < 0 || step.time > 0.6f) return false;
            if (step.operation == Operation::Equip) equipment = true;
            if (step.operation == Operation::Cast) blood = true;
            if (step.resource == Resource::Form && step.operation != Operation::HideTorso &&
                step.operation != Operation::UncoverHead && (!step.form || step.form > 0xFFFFFF)) return false;
        }
        if (!equipment || !blood) return false;
    }
    return !ParseEffect("DEATH") && !ParseEffect("");
}());

static_assert([]
{
    const auto left = Plan(Effect::HandLeft), right = Plan(Effect::HandRight);
    if (left[0].form == right[0].form || left[2].index != 4 || right[2].index != 5) return false;
    int bursts = 0;
    float last = 0;
    for (const auto& step : left)
        if (step.operation == Operation::Spawn)
        {
            if (step.time <= last) return false;
            last = step.time; ++bursts;
        }
    return bursts == 3 && left[1].resource == Resource::LeftHandStump;
}());

static_assert([]
{
    for (const auto& choice : choices)
    {
        int hidden = 0;
        for (const auto& step : Plan(choice.effect)) if (step.operation == Operation::HideTorso) ++hidden;
        if (hidden != (choice.effect == Effect::TorsoSmash ? 1 : 0)) return false;
    }
    const auto horizontal = Plan(Effect::BisectionHorizontal), vertical = Plan(Effect::BisectionVertical);
    return horizontal[1].form != vertical[1].form && horizontal[2].resource == Resource::BootsHorizontal &&
        vertical[2].resource == Resource::Gloves && vertical[3].resource == Resource::BootsVertical;
}());

// 每个复合步骤都必须能单独执行；原子目录不得偷偷携带延迟或其他操作。
static_assert([]
{
    for (std::size_t i = 0; i < primitives.size(); ++i)
    {
        const auto& primitive = primitives[i];
        if (primitive.step.time != 0 || ParsePrimitive(primitive.code) != &primitive ||
            ParsePrimitive(primitive.command, primitive.argument) != &primitive) return false;
        for (std::size_t j = 0; j < i; ++j) if (primitives[j].code == primitive.code) return false;
    }
    for (const auto& choice : choices)
        for (const auto& step : Plan(choice.effect))
        {
            bool covered = false;
            for (const auto& primitive : primitives)
            {
                const auto& atom = primitive.step;
                if (atom.operation == step.operation && atom.form == step.form && atom.index == step.index &&
                    atom.resource == step.resource && atom.targetSelf == step.targetSelf) covered = true;
            }
            if (!covered) return false;
        }
    return !ParsePrimitive("BLOOD_UNKNOWN") && !ParsePrimitive("GORE_BLOOD", "LARGE");
}());

int main() {}
