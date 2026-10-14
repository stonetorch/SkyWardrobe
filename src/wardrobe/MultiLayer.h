#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Wardrobe
{
    struct LayerItem
    {
        std::string key, name;
        std::uint32_t slots{};
        bool temporary{};
        bool operator==(const LayerItem&) const = default;
    };
    struct LayerSettings
    {
        std::vector<std::string> backup;
        bool backupEnabled = true, originalEnabled = false;
        std::uint32_t hiddenSlots{};
    };
    enum class LayerSource { Working, Backup, Original };
    enum class LayerDisposition { Visible, Covered, Hidden, Disabled, Temporary };
    struct LayerResult
    {
        LayerItem item;
        LayerSource source;
        LayerDisposition disposition;
        std::string coveredBy;
    };
    inline std::vector<LayerResult> ComposeLayers(const std::vector<LayerItem>& working,
        const std::vector<LayerItem>& backup, const std::vector<LayerItem>& original, const LayerSettings& settings)
    {
        std::vector<LayerResult> result;
        // 从高层向下选择整件装备，隐藏只作用于原版来源。
        const auto append = [&](const auto& items, LayerSource source, bool enabled)
        {
            for (const auto& item : items)
            {
                LayerResult row{item, source, LayerDisposition::Visible, {}};
                if (source == LayerSource::Original && item.temporary) row.disposition = LayerDisposition::Temporary;
                else if (!enabled) row.disposition = LayerDisposition::Disabled;
                else if (source == LayerSource::Original && (item.slots & settings.hiddenSlots)) row.disposition = LayerDisposition::Hidden;
                else
                    for (const auto& selected : result)
                        if (selected.disposition == LayerDisposition::Visible &&
                            (selected.item.key == item.key || (selected.item.slots & item.slots)))
                        { row.disposition = LayerDisposition::Covered; row.coveredBy = selected.item.name; break; }
                result.push_back(std::move(row));
            }
        };
        append(working, LayerSource::Working, true);
        append(backup, LayerSource::Backup, settings.backupEnabled);
        append(original, LayerSource::Original, settings.originalEnabled);
        return result;
    }
    inline std::array<float, 3> AllocateLayerHeights(float budget, float rowHeight,
        const std::array<int, 3>& counts, const std::array<bool, 3>& open)
    {
        std::array<float, 3> result{}, wanted{};
        // 短列表先完整显示，长列表按工作层优先的权重共享剩余空间。
        for (int i = 0; i < 3; ++i)
        {
            wanted[i] = open[i] ? std::max(1, counts[i]) * rowHeight + rowHeight : 0;
            result[i] = std::min(wanted[i], rowHeight * 2);
        }
        float used = result[0] + result[1] + result[2];
        if (used > budget && used > 0)
        { for (auto& height : result) height *= std::max(0.0f, budget) / used; return result; }
        for (int pass = 0; pass < 4; ++pass)
        {
            const float remaining = budget - used;
            float weight = 0;
            for (int i = 0; i < 3; ++i) if (result[i] < wanted[i]) weight += i == 0 ? 2.0f : 1.0f;
            if (weight == 0 || remaining <= 0) break;
            for (int i = 0; i < 3; ++i)
            {
                const auto extra = std::min(wanted[i] - result[i], remaining * (i == 0 ? 2.0f : 1.0f) / weight);
                if (extra > 0) { result[i] += extra; used += extra; }
            }
        }
        return result;
    }
}
