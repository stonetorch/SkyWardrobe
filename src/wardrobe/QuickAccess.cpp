#include "wardrobe/QuickAccess.h"
#include "wardrobe/Panel.h"
#include "wardrobe/Service.h"
#include "ui/IconsFontAwesome6.h"
#include "ui/UiTabs.h"
#include "ui/WindowPlacement.h"
#include <imgui_stdlib.h>
#include <algorithm>
#include <array>

namespace Wardrobe
{
    namespace
    {
        struct Icon
        {
            const char* id;
            const char* glyph;
            const char* name;
        };
        // 仅使用随项目分发的 Font Awesome Free Solid 字体，避免在线资源依赖。
        const Icon icons[] = {
            {"folder", ICON_FA_FOLDER, Tr("Folder", "文件夹")},
            {"shirt", ICON_FA_SHIRT, Tr("Shirt", "上装")},
            {"dress", ICON_FA_PERSON_DRESS, Tr("Dress", "裙装")},
            {"shoes", ICON_FA_SHOE_PRINTS, Tr("Shoes", "鞋子")},
            {"socks", ICON_FA_SOCKS, Tr("Socks", "袜子")},
            {"gloves", ICON_FA_MITTEN, Tr("Gloves", "手套")},
            {"hat", ICON_FA_HAT_COWBOY, Tr("Hat", "帽子")},
            {"crown", ICON_FA_CROWN, Tr("Crown", "皇冠")},
            {"gem", ICON_FA_GEM, Tr("Gem", "宝石")},
            {"ring", ICON_FA_RING, Tr("Ring", "戒指")},
            {"glasses", ICON_FA_GLASSES, Tr("Glasses", "眼镜")},
            {"mask", ICON_FA_MASK, Tr("Mask", "面具")},
            {"star", ICON_FA_STAR, Tr("Star", "星星")},
            {"heart", ICON_FA_HEART, Tr("Heart", "爱心")},
            {"shield", ICON_FA_SHIELD_HALVED, Tr("Shield", "盾牌")},
            {"magic", ICON_FA_WAND_MAGIC_SPARKLES, Tr("Magic", "魔法")},
            {"dragon", ICON_FA_DRAGON, Tr("Dragon", "龙")},
            {"bolt", ICON_FA_BOLT, Tr("Bolt", "闪电")},
            {"fire", ICON_FA_FIRE, Tr("Fire", "火焰")},
            {"snow", ICON_FA_SNOWFLAKE, Tr("Snow", "雪花")},
            {"sun", ICON_FA_SUN, Tr("Sun", "太阳")},
            {"moon", ICON_FA_MOON, Tr("Moon", "月亮")},
            {"leaf", ICON_FA_LEAF, Tr("Leaf", "叶子")},
            {"feather", ICON_FA_FEATHER, Tr("Feather", "羽毛")},
            {"palette", ICON_FA_PALETTE, Tr("Palette", "调色盘")},
            {"user", ICON_FA_USER, Tr("Person", "人物")},
            {"home", ICON_FA_HOUSE, Tr("Home", "房屋")},
            {"bookmark", ICON_FA_BOOKMARK, Tr("Bookmark", "书签")},
            {"layers", ICON_FA_LAYER_GROUP, Tr("Layers", "服装组")},
            {"tag", ICON_FA_TAG, Tr("Tag", "标签")},
            {"archive", ICON_FA_BOX_ARCHIVE, Tr("Archive", "收纳箱")},
            {"dice", ICON_FA_DICE, Tr("Dice", "骰子")},
            {"music", ICON_FA_MUSIC, Tr("Music", "音乐")},
            {"check", ICON_FA_CHECK, Tr("Check", "勾选")}
        };
        const char* actions[] = {"none", "category", "group", "preset", "applyPreset", "useGroup"};
        const char* actionNames[] = {Tr("Empty", "空格子"), Tr("Open category", "跳转分类"),
            Tr("Open outfit group", "查看服装组"), Tr("Select preset", "选择预设"),
            Tr("Apply preset", "应用预设"), Tr("Use outfit group", "使用指定服装组")};
        int editingCell = 0;
        bool openEditor = false;
        Json draft;
        std::string targetSearch, iconSearch;
        ImVec2 detachedPosition{};
        bool hasDetachedPosition = false;

        Json& Options()
        {
            auto& database = Service::Get().store.database;
            if (!database.contains("quickAccess")) database["quickAccess"] = DefaultQuickAccess();
            return database["quickAccess"];
        }

        int ActionIndex(const std::string& action)
        {
            for (int i = 0; i < static_cast<int>(std::size(actions)); ++i)
                if (action == actions[i]) return i;
            return 0;
        }

        const Icon& FindIcon(const std::string& id)
        {
            for (const auto& icon : icons) if (id == icon.id) return icon;
            return icons[0];
        }

        std::string ObjectName(const std::string& id, const Json& metadata)
        {
            const auto alias = metadata.value("alias", "");
            const auto name = alias.empty() ? metadata.value("name", id) : alias;
            return name.empty() ? id : name;
        }

        const Json* Target(const Json& cell)
        {
            const auto action = cell.value("action", "none");
            const auto target = cell.value("target", "");
            auto& service = Service::Get();
            if (action == "preset" || action == "applyPreset")
            {
                const auto& presets = service.store.database["presets"];
                if (!presets.contains(target)) return nullptr;
                const auto& preset = presets[target];
                return service.Group(preset.value("group", "")) ? &preset : nullptr;
            }
            const auto [collection, key] = SplitGroupID(target);
            if (action == "category" && collection != "categories") return nullptr;
            if (action == "group" && collection != "groups") return nullptr;
            if (action == "none") return nullptr;
            return service.Group(target);
        }

        std::string CellName(const Json& cell)
        {
            const auto label = cell.value("label", "");
            if (!label.empty()) return label;
            const auto* target = Target(cell);
            const auto id = cell.value("target", "");
            if (target) return ObjectName(id, *target);
            return id.empty() ? Tr("Empty", "空格子") : id;
        }

        void StartEditor(int index)
        {
            editingCell = index;
            const auto& cells = Options()["cells"];
            draft = index < static_cast<int>(cells.size()) ? cells[index] : Json::object();
            const Json defaults{{"action", "none"}, {"target", ""}, {"icon", "folder"}, {"label", ""}};
            for (const auto& [key, value] : defaults.items())
                if (!draft.contains(key)) draft[key] = value;
            targetSearch.clear(); iconSearch.clear(); openEditor = true;
        }

        void DrawEditor()
        {
            const auto title = Tr("Shortcut editor", "编辑快捷格子");
            if (openEditor) { ImGui::OpenPopup(title); openEditor = false; }
            const auto* viewport = ImGui::GetMainViewport();
            const float scale = ImGui::GetFontSize() / 20.0f;
            const ImVec2 editorSize(570 * scale, 650 * scale);
            const auto center = viewport->GetCenter();
            CAF::Ui::SetNextWindowNearPoint(ImVec2(center.x - editorSize.x / 2 - 16,
                center.y - editorSize.y / 2 - 16), editorSize);
            if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
            ImGui::Text("%s %d", Tr("Cell", "格子"), editingCell + 1);
            ImGui::BeginDisabled(!Service::Get().store.writable);
            auto label = draft.value("label", "");
            ImGui::TextUnformatted(Tr("Label (optional, shown on hover)", "名称（可选，悬停显示）"));
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##label", &label)) draft["label"] = label;
            int action = ActionIndex(draft.value("action", "none"));
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo(Tr("Action", "操作"), &action, actionNames, static_cast<int>(std::size(actions))))
            {
                draft["action"] = actions[action]; draft["target"] = ""; targetSearch.clear();
            }
            if (action != 0)
            {
                ImGui::TextWrapped("%s: %s", Tr("Target", "目标"), CellName(Json{{"action", actions[action]}, {"target", draft["target"]}}).c_str());
                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextWithHint("##target search", Tr("Search name / ID / pinyin", "搜索名称 / ID / 拼音"), &targetSearch);
                ImGui::BeginChild("Targets", ImVec2(0, 145 * scale), ImGuiChildFlags_Borders);
                const auto& database = Service::Get().store.database;
                for (const char* collection : {"categories", "groups", "pluginCategories", "presets"})
                {
                    if ((action == 1 && std::string_view(collection) != "categories") ||
                        (action == 2 && std::string_view(collection) != "groups") ||
                        ((action == 3 || action == 4) && std::string_view(collection) != "presets") ||
                        (action == 5 && std::string_view(collection) == "presets")) continue;
                    for (const auto& [id, metadata] : database[collection].items())
                    {
                        if (!targetSearch.empty() && !SearchScore(targetSearch, id) && !SearchMetadata(targetSearch, metadata) &&
                            !SearchScore(targetSearch, ObjectName(id, metadata), true)) continue;
                        const auto target = std::string_view(collection) == "presets" ? id : std::string(collection) + ":" + id;
                        ImGui::PushID(target.c_str());
                        const auto name = ObjectName(id, metadata);
                        if (ImGui::Selectable((name + "##target").c_str(), draft["target"] == target)) draft["target"] = target;
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", target.c_str());
                        ImGui::PopID();
                    }
                }
                ImGui::EndChild();
                if (!Target(draft)) ImGui::TextColored(ImVec4(1, .5f, .3f, 1), "%s", Tr("Select an available target.", "请选择有效目标。"));
                if (action == 5 || action == 4)
                    ImGui::TextWrapped("%s", Tr("Uses the existing outfit selection: randomly picks items without slot conflicts and applies immediately.",
                        "沿用现有服装组选衣逻辑：随机选取槽位不冲突的服装，并立即应用。"));
            }
            ImGui::SeparatorText(Tr("Icon library", "图标库"));
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##icon search", Tr("Search icons", "搜索图标"), &iconSearch);
            ImGui::BeginChild("Icons", ImVec2(0, 130 * scale), ImGuiChildFlags_Borders);
            int visible = 0;
            const float side = 42 * scale;
            const int count = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + ImGui::GetStyle().ItemSpacing.x) /
                (side + ImGui::GetStyle().ItemSpacing.x)));
            for (const auto& icon : icons)
            {
                if (!iconSearch.empty() && !SearchScore(iconSearch, icon.name) && !SearchScore(iconSearch, icon.id)) continue;
                if (visible++ % count) ImGui::SameLine();
                ImGui::PushID(icon.id);
                const bool selected = draft["icon"] == icon.id;
                if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Button(CAF::Ui::IconFontAvailable() ? icon.glyph : icon.name, ImVec2(side, side))) draft["icon"] = icon.id;
                if (selected) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s (%s)", icon.name, icon.id);
                ImGui::PopID();
            }
            ImGui::EndChild();
            ImGui::BeginDisabled(action != 0 && !Target(draft));
            if (ImGui::Button(Tr("Save", "保存")))
            {
                auto& cells = Options()["cells"];
                while (cells.size() <= static_cast<std::size_t>(editingCell)) cells.push_back(Json::object());
                cells[editingCell] = draft; Service::Get().Save(); ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled(); ImGui::SameLine();
            if (ImGui::Button(Tr("Clear cell", "清空格子")))
            {
                auto& cells = Options()["cells"];
                if (editingCell < static_cast<int>(cells.size())) cells[editingCell] = Json::object();
                Service::Get().Save(); ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled(); ImGui::SameLine();
            if (ImGui::Button(Tr("Cancel", "取消"))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    void DrawQuickAccessSettings()
    {
        auto& options = Options();
        ImGui::SeparatorText(Tr("Shortcut window", "快捷浮窗"));
        ImGui::BeginDisabled(!Service::Get().store.writable);
        bool enabled = options["enabled"], attached = options["attached"];
        bool changed = ImGui::Checkbox(Tr("Show shortcut window", "显示快捷浮窗"), &enabled);
        changed |= ImGui::Checkbox(Tr("Follow main panel", "跟随主面板"), &attached);
        int rows = options["rows"], columns = options["columns"];
        float cellSize = options["cellSize"];
        ImGui::SetNextItemWidth(180);
        if (ImGui::SliderInt(Tr("Rows", "行数"), &rows, 1, 12, "%d", ImGuiSliderFlags_AlwaysClamp)) options["rows"] = rows;
        changed |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetNextItemWidth(180);
        if (ImGui::SliderInt(Tr("Columns", "列数"), &columns, 1, 12, "%d", ImGuiSliderFlags_AlwaysClamp)) options["columns"] = columns;
        changed |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetNextItemWidth(180);
        if (ImGui::SliderFloat(Tr("Square size", "方格大小"), &cellSize, 32, 128, "%.0f", ImGuiSliderFlags_AlwaysClamp)) options["cellSize"] = cellSize;
        changed |= ImGui::IsItemDeactivatedAfterEdit();
        options["enabled"] = enabled; options["attached"] = attached;
        if (changed) Service::Get().Save();
        ImGui::EndDisabled();
        ImGui::TextWrapped("%s", Tr("Click an empty cell or right-click any cell to configure it. Shrinking the grid retains hidden bindings; expanding restores them in order. Square size follows UI scale.",
            "点击空格子或右键任意格子可配置。缩小网格会保留隐藏绑定，扩大后按原顺序恢复。方格大小随界面缩放。"));
    }

    QuickAccessRequest DrawQuickAccess(ImVec2 panelPosition, ImVec2 panelSize)
    {
        QuickAccessRequest request;
        auto& options = Options();
        if (options["enabled"].get<bool>())
        {
            const auto* viewport = ImGui::GetMainViewport();
            const auto& style = ImGui::GetStyle();
            const int rows = options["rows"], columns = options["columns"];
            const float scale = ImGui::GetFontSize() / 20.0f;
            const float side = options["cellSize"].get<float>() * scale;
            const float gap = 8.0f * scale, padding = 12.0f * scale;
            const float gridWidth = columns * side + (columns - 1) * gap;
            const float gridHeight = rows * side + (rows - 1) * gap;
            // 四边留白和横纵间距一致，只在内容超出视口时预留实际滚动条。
            bool scrollX = false, scrollY = false;
            for (int pass = 0; pass < 3; ++pass)
            {
                scrollX = gridWidth + padding * 2 + (scrollY ? style.ScrollbarSize : 0) > viewport->WorkSize.x;
                scrollY = gridHeight + padding * 2 + (scrollX ? style.ScrollbarSize : 0) > viewport->WorkSize.y;
            }
            const float width = gridWidth + padding * 2 + (scrollY ? style.ScrollbarSize : 0);
            const float height = gridHeight + padding * 2 + (scrollX ? style.ScrollbarSize : 0);
            const ImVec2 size(std::min(width, viewport->WorkSize.x), std::min(height, viewport->WorkSize.y));
            ImVec2 position(panelPosition.x + panelSize.x + gap, panelPosition.y + (panelSize.y - size.y) * 0.5f);
            // 优先放在右侧，右侧空间不足时尝试左侧；小屏幕夹到视口内并提供滚动。
            if (position.x + size.x > viewport->WorkPos.x + viewport->WorkSize.x)
                position.x = panelPosition.x - size.x - gap;
            const bool attached = options["attached"];
            if (!attached && hasDetachedPosition) position = detachedPosition;
            position.x = std::clamp(position.x, viewport->WorkPos.x, viewport->WorkPos.x + std::max(0.0f, viewport->WorkSize.x - size.x));
            position.y = std::clamp(position.y, viewport->WorkPos.y, viewport->WorkPos.y + std::max(0.0f, viewport->WorkSize.y - size.y));
            // 自由拖动模式只在出现时恢复位置，逐帧强制定位会抵消 ImGui 的鼠标拖动。
            ImGui::SetNextWindowPos(position, attached ? ImGuiCond_Always : ImGuiCond_Appearing);
            ImGui::SetNextWindowSize(size);
            ImGui::SetNextWindowContentSize(ImVec2(gridWidth, gridHeight));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f * scale);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f * scale);
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
            const auto flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_HorizontalScrollbar | (attached ? ImGuiWindowFlags_NoMove : 0);
            if (ImGui::Begin(Tr("Shortcuts###WardrobeShortcuts", "快捷选项###WardrobeShortcuts"), nullptr, flags))
            {
                detachedPosition = ImGui::GetWindowPos();
                detachedPosition.x = std::clamp(detachedPosition.x, viewport->WorkPos.x,
                    viewport->WorkPos.x + std::max(0.0f, viewport->WorkSize.x - size.x));
                detachedPosition.y = std::clamp(detachedPosition.y, viewport->WorkPos.y,
                    viewport->WorkPos.y + std::max(0.0f, viewport->WorkSize.y - size.y));
                ImGui::SetWindowPos(detachedPosition); hasDetachedPosition = true;
                // 复制格子快照，配置或服务保存不会使本帧遍历中的 JSON 引用失效。
                const auto cells = options["cells"];
                const auto origin = ImGui::GetCursorPos();
                for (int row = 0; row < rows; ++row)
                    for (int column = 0; column < columns; ++column)
                    {
                        ImGui::SetCursorPos(ImVec2(origin.x + column * (side + gap), origin.y + row * (side + gap)));
                        const int index = row * columns + column;
                        const auto cell = index < static_cast<int>(cells.size()) ? cells[index] : Json::object();
                        const auto action = cell.value("action", "none");
                        const bool empty = action == "none", valid = Target(cell) != nullptr;
                        const auto name = CellName(cell);
                        const auto& icon = FindIcon(cell.value("icon", "folder"));
                        ImGui::PushID(index);
                        if (!empty && !valid) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, .5f, .3f, 1));
                        const auto text = empty ? "+" : CAF::Ui::IconFontAvailable() ? icon.glyph : icon.name;
                        if (ImGui::Button((std::string(text) + "##cell").c_str(), ImVec2(side, side)))
                        {
                            if (empty) StartEditor(index);
                            else if (valid) request = {action, cell.value("target", "")};
                            else Service::Get().status = Tr("Shortcut target is missing. Right-click to rebind.", "快捷目标已失效，请右键重新绑定。");
                        }
                        if (!empty && !valid) ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s\n%s", name.c_str(), actionNames[ActionIndex(action)],
                            !empty && !valid ? Tr("Missing target - right-click to edit", "目标失效，右键编辑") : Tr("Right-click to edit", "右键编辑"));
                        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) StartEditor(index);
                        ImGui::PopID();
                    }
                // 右键留白打开整体配置，格子右键仍直接编辑该格；取消跟随后可拖动留白。
                if (ImGui::BeginPopupContextWindow("Grid settings", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
                {
                    DrawQuickAccessSettings(); ImGui::EndPopup();
                }
            }
            ImGui::End();
            ImGui::PopStyleVar(5);
        }
        DrawEditor();
        return request;
    }
}
