#include "wardrobe/Panel.h"
#include "wardrobe/SafeMode.h"
#include "wardrobe/Service.h"
#include "wardrobe/SosBackend.h"
#include "wardrobe/QuickAccess.h"
#include "ui/Overlay.h"
#include "ui/IconsFontAwesome6.h"
#include "ui/UiTabs.h"
#include <imgui.h>
#include "ui/FlickUi.h"
#include <array>
#include <cmath>
#include <mutex>
#include <numeric>
#include <sstream>

namespace Wardrobe
{
    namespace
    {
        std::string search, sidebarSearch, viewID, presetSearch;
        enum class View { All, Favorites, Uncategorized, Working, Category, Plugin, Group };
        View view = View::All;
        bool byPart = false, automatic = true;
        bool selectMetadata = false;
        int columns = 4;
        std::string editItem, editAlias, editNote, editSearch;
        Json editCategories;
        int editPart{};
        bool editFavorite{}, openEdit{};
        std::string categoryID, categoryName, categorySearch;
        std::string presetName, presetKeys;
        std::string pluginFilter;
        std::string selectedPlugin;
        std::string pluginAlias, pluginKeys;
        std::mutex commandMutex;
        std::vector<std::pair<std::string, std::string>> commands;

        #include "wardrobe/PanelManual.inc"

        enum class QuickAction { None, Add, Part, Remove };
        QuickAction quickAction = QuickAction::None;
        std::string quickGroup, editingGroup, groupName, groupAlias, groupNote, groupSearch;
        int quickPart = 0;
        bool openGroupEditor = false;
        std::string referenceSearch, reverseReferenceSearch, quickTargetSearch, itemCategorySearch;
        QuickAction pendingQuickAction = QuickAction::None;

        bool MatchesGroupSearch(const std::string& id, const Json& metadata, const std::string& query)
        {
            return query.empty() || SearchScore(query, id) > 0 || SearchMetadata(query, metadata) > 0;
        }

        std::string CurrentGroup()
        {
            if (view == View::Category) return "categories:" + viewID;
            if (view == View::Plugin) return "pluginCategories:" + viewID;
            if (view == View::Group) return "groups:" + viewID;
            return {};
        }

        void NavigateGroup(const std::string& id)
        {
            const auto [collection, key] = SplitGroupID(id);
            if (!Service::Get().Group(id)) return;
            view = collection == "categories" ? View::Category : collection == "pluginCategories" ? View::Plugin : View::Group;
            viewID = key;
        }

        std::string Join(const Json& values)
        {
            std::string result;
            for (const auto& value : values) if (value.is_string())
            {
                if (!result.empty()) result += '\n';
                result += value.get<std::string>();
            }
            return result;
        }

        std::vector<std::string> Split(const std::string& value)
        {
            std::vector<std::string> result;
            std::istringstream stream(value);
            std::string line;
            while (std::getline(stream, line))
            {
                const auto first = line.find_first_not_of(" \t\r");
                if (first == std::string::npos) continue;
                line = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
                if (std::find(result.begin(), result.end(), line) == result.end()) result.push_back(line);
            }
            return result;
        }

        const char* PartLabel(int part)
        {
            static const char* labels[] = {
                Tr("Unknown", "未标记"), Tr("Body", "全身"), Tr("Top", "上装"), Tr("Bottom", "下装"),
                Tr("Underwear", "内衣"), Tr("Bra", "胸衣"), Tr("Panties", "内裤"), Tr("Shoes", "鞋"),
                Tr("Stockings", "袜"), Tr("Gloves", "手套"), Tr("Head", "头饰"), Tr("Hair", "头发"),
                Tr("Neck", "项链"), Tr("Cloak", "披风"), Tr("Accessory", "配饰"),
                Tr("Custom 1", "自定义 1"), Tr("Custom 2", "自定义 2"), Tr("Custom 3", "自定义 3"),
                Tr("Custom 4", "自定义 4"), Tr("Custom 5", "自定义 5"), Tr("Custom 6", "自定义 6"),
                Tr("Custom 7", "自定义 7"), Tr("Custom 8", "自定义 8")
            };
            return labels[std::clamp(part, 0, static_cast<int>(std::size(labels)) - 1)];
        }

        std::string PartName(int part)
        {
            const auto& database = Service::Get().store.database;
            if (database.contains("partNames") && database["partNames"].is_object())
            {
                const auto found = database["partNames"].find(PartIDs[part]);
                if (found != database["partNames"].end() && found->is_string() && !found->get<std::string>().empty()) return found->get<std::string>();
            }
            return PartLabel(part);
        }

        std::string ObjectName(const std::string& id, const Json& metadata)
        {
            const auto alias = metadata.value("alias", "");
            const auto name = alias.empty() ? metadata.value("name", id) : alias;
            return name.empty() ? id : name;
        }

        // 标签独占一行，输入框只使用当前可用宽度，避免长中文标签被窗口右边缘裁切。
        bool EditText(const char* label, std::string& value, float height = 0.0f)
        {
            FlickUi::PushID(label);
            FlickUi::TextWrapped("%s", label);
            FlickUi::SetNextItemWidth(-1.0f);
            const bool changed = height > 0.0f
                ? FlickUi::InputTextMultiline("##value", &value, ImVec2(-1.0f, height))
                : FlickUi::InputText("##value", &value);
            FlickUi::PopID();
            return changed;
        }

        void StartEdit(const ClothingEntry& entry)
        {
            auto& service = Service::Get();
            const auto metadata = entry.metadata;
            editItem = entry.key;
            editAlias = service.Name(entry);
            editNote = metadata.value("note", "");
            editSearch = Join(metadata.value("search", Json::array()));
            if (metadata.contains("pinyin")) editSearch += (editSearch.empty() ? "" : "\n") + metadata["pinyin"].get<std::string>();
            editCategories = metadata.value("categories", Json::array());
            itemCategorySearch.clear();
            editPart = service.Part(entry);
            editFavorite = metadata.value("favorite", false);
            openEdit = true;
        }

        void DrawItemEditor()
        {
            if (openEdit) { FlickUi::OpenPopup(Tr("Item editor", "服装信息")); openEdit = false; }
            const auto* viewport = FlickUi::GetMainViewport();
            const float scale = FlickUi::GetFontSize() / 18.0f;
            FlickUi::SetNextWindowSize(ImVec2(std::min(900.0f * scale, viewport->WorkSize.x * 0.9f),
                std::min(650.0f * scale, viewport->WorkSize.y * 0.85f)), ImGuiCond_Appearing);
            if (!FlickUi::BeginPopupModal(Tr("Item editor", "服装信息"), nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
            auto& service = Service::Get();
            FlickUi::TextUnformatted(editItem.c_str());
            EditText(Tr("Alias", "别名"), editAlias);
            EditText(Tr("Note", "备注"), editNote, 70);
            EditText(Tr("Search keys / pinyin (one per line)", "搜索词 / 拼音（每行一个）"), editSearch, 75);
            FlickUi::Checkbox(Tr("Favorite", "收藏"), &editFavorite);
            if (FlickUi::BeginCombo(Tr("Part", "部件"), PartName(editPart).c_str()))
            {
                for (int i = 0; i < static_cast<int>(std::size(PartIDs)); ++i)
                    if (FlickUi::Selectable(PartName(i).c_str(), editPart == i)) editPart = i;
                FlickUi::EndCombo();
            }
            FlickUi::SeparatorText(Tr("Categories", "分类"));
            FlickUi::SetNextItemWidth(-1.0f);
            FlickUi::InputTextWithHint("##item category search", Tr("Search categories / pinyin...", "搜索分类 / 拼音…"), &itemCategorySearch);
            // 两栏使用帧开始时的快照；分类修改留在编辑草稿中，保存后才写入服装数据。
            const auto selectedCategories = editCategories.get<std::vector<std::string>>();
            if (FlickUi::BeginTable("Item categories", 2, ImGuiTableFlags_SizingStretchSame))
            {
                for (int column = 0; column < 2; ++column)
                {
                    FlickUi::TableNextColumn();
                    FlickUi::TextUnformatted(column == 0 ? Tr("Available — click to add", "未分类 · 点击添加") : Tr("Assigned — click to remove", "已分类 · 点击移除"));
                    FlickUi::PushID(column);
                    FlickUi::BeginChild("Category list", ImVec2(0, std::max(80.0f, FlickUi::GetContentRegionAvail().y - FlickUi::GetFrameHeightWithSpacing())), ImGuiChildFlags_Borders);
                    for (const auto& [id, category] : service.store.database["categories"].items())
                    {
                        const bool selected = std::find(selectedCategories.begin(), selectedCategories.end(), id) != selectedCategories.end();
                        if (selected != (column == 1) || !MatchesGroupSearch(id, category, itemCategorySearch)) continue;
                        if (FlickUi::Selectable((ObjectName(id, category) + "##" + id).c_str()))
                        {
                            auto values = editCategories.get<std::vector<std::string>>();
                            std::erase(values, id);
                            if (!selected) values.push_back(id);
                            editCategories = values;
                        }
                        if (FlickUi::IsItemHovered()) FlickUi::SetTooltip("%s", id.c_str());
                    }
                    FlickUi::EndChild();
                    FlickUi::PopID();
                }
                FlickUi::EndTable();
            }
            FlickUi::BeginDisabled(!service.store.writable);
            if (FlickUi::Button(Tr("Save", "保存")))
            {
                auto& item = service.store.database["items"][editItem];
                if (!item.is_object()) item = Json::object();
                item["alias"] = editAlias; item["note"] = editNote; item["favorite"] = editFavorite;
                item["search"] = Split(editSearch); item["categories"] = editCategories; item["part"] = PartIDs[editPart];
                if (service.Save()) FlickUi::CloseCurrentPopup();
            }
            FlickUi::SameLine();
            if (FlickUi::Button(Tr("Reset user metadata", "恢复别名包 / 原始数据")))
            {
                service.store.database["items"].erase(editItem);
                if (service.Save()) FlickUi::CloseCurrentPopup();
            }
            FlickUi::EndDisabled();
            FlickUi::SameLine();
            if (FlickUi::Button(Tr("Cancel", "取消"))) FlickUi::CloseCurrentPopup();
            FlickUi::EndPopup();
        }

        // 名字区固定占两行高度，卡片整体比单行名字时高一行；行距与高度必须共用同一组常量。
        constexpr float CardTitleLine = 1.05f;
        constexpr float CardTextHeight = 3.4f;

        float CardHeight()
        {
            return FlickUi::GetFontSize() * (CardTextHeight + CardTitleLine);
        }

        // 通过宿主测量 UTF-8 文本，优先在单词边界断行；过窄时允许不绘制字符。
        const char* WrapTitleLine(float size, const char* begin, const char* end, float width)
        {
            const char* fit = FlickUi::FitText(begin, end, width, size);
            if (fit >= end) return end;
            const char* space = fit;
            while (space > begin && space[-1] != ' ' && space[-1] != '\t') --space;
            return space > begin + 1 ? space - 1 : fit;
        }

        void DrawCard(const ClothingEntry& entry, float width)
        {
            auto& service = Service::Get();
            const auto metadata = entry.metadata;
            const bool favorite = metadata.value("favorite", false);
            const bool selected = quickAction == QuickAction::Add ? service.InGroup(entry, quickGroup)
                : quickAction == QuickAction::Part ? entry.part == quickPart
                : quickAction == QuickAction::Remove ? service.InGroup(entry, CurrentGroup()) : service.InWorking(entry.key);
            const auto title = std::string(favorite ? "★ " : "") + service.Name(entry);
            const auto note = metadata.value("note", "");
            FlickUi::PushID(entry.key.c_str());
            const auto position = FlickUi::GetCursorScreenPos();
            const float font = FlickUi::GetFontSize(), lineHeight = font * CardTitleLine, height = CardHeight();
            const ImVec2 end(position.x + width, position.y + height);
            const auto mouse = FlickUi::GetMousePos();
            const bool hoverRect = mouse.x >= position.x && mouse.x < end.x && mouse.y >= position.y && mouse.y < end.y;
            auto* draw = FlickUi::GetWindowDrawList();
            draw->AddRectFilled(position, end, FlickUi::GetColorU32(selected ? ImGuiCol_HeaderActive : hoverRect ? ImGuiCol_HeaderHovered : ImGuiCol_FrameBg), 5);
            if (selected) draw->AddRect(position, end, quickAction == QuickAction::None ? FlickUi::GetColorU32(ImGuiCol_CheckMark)
                : FlickUi::GetColorU32(ImVec4(1.0f, 0.65f, 0.15f, 1.0f)), 5, 0, 3);
            const float titleWidth = std::max(0.0f, width - 16.0f);
            const auto* titleBegin = title.c_str();
            const auto* const titleEnd = titleBegin + title.size();
            const auto* lineEnd = WrapTitleLine(font, titleBegin, titleEnd, titleWidth);
            FlickUi::DrawTextAt(ImVec2(position.x + 9, position.y + 7), font, FlickUi::GetStyleColorVec4(ImGuiCol_Text), titleBegin, lineEnd);
            if (lineEnd < titleEnd)
            {
                while (lineEnd < titleEnd && (*lineEnd == ' ' || *lineEnd == '\t' || *lineEnd == '\r' || *lineEnd == '\n')) ++lineEnd;
                FlickUi::DrawTextAt(ImVec2(position.x + 9, position.y + 7 + lineHeight), font, FlickUi::GetStyleColorVec4(ImGuiCol_Text),
                    lineEnd, WrapTitleLine(font, lineEnd, titleEnd, titleWidth));
            }
            // 文字在 UTF-8 字符边界截断；悬浮提示仍显示完整备注。
            const float noteSize = font * 0.8f;
            FlickUi::DrawTextAt(ImVec2(position.x + 9, position.y + font * 1.6f + lineHeight), noteSize,
                FlickUi::GetStyleColorVec4(ImGuiCol_TextDisabled), note.c_str(), FlickUi::FitText(note.c_str(), note.c_str() + note.size(), titleWidth, noteSize));
            const auto part = PartName(service.Part(entry));
            FlickUi::DrawTextAt(ImVec2(position.x + 9, position.y + font * 2.6f + lineHeight), font * 0.7f,
                FlickUi::GetStyleColorVec4(ImGuiCol_TextDisabled), part.c_str(), FlickUi::FitText(part.c_str(), part.c_str() + part.size(), titleWidth, font * 0.7f));
            // 最后提交交互控件，确保文字绘制不覆盖右键菜单和拖动所需的 last-item 状态。
            FlickUi::SetCursorScreenPos(position);
            const bool clicked = FlickUi::InvisibleButton("card", ImVec2(std::max(30.0f, width), height));
            const bool hovered = FlickUi::IsItemHovered();
            const bool doubleClicked = FlickUi::CardDoubleClicked(entry.key, clicked);
            // 双击的第二次点击只应用搭配，避免把第一次选中的服装再次移除。
            if (clicked && quickAction != QuickAction::None && service.store.writable)
            {
                // 快捷状态消费点击，不改变活动工作搭配，也不触发双击应用。
                if (quickAction == QuickAction::Add) service.SetGroupItem(quickGroup, entry, !selected);
                else if (quickAction == QuickAction::Remove) service.SetGroupItem(CurrentGroup(), entry, false);
                else
                {
                    auto& item = service.store.database["items"][entry.key];
                    if (!item.is_object()) item = Json::object();
                    item["part"] = PartIDs[quickPart]; service.Save();
                }
            }
            if (clicked && quickAction == QuickAction::None && !doubleClicked)
            {
                if (selected) service.Remove(entry.key);
                else service.Add(entry.key);
            }
            if (quickAction == QuickAction::None && hovered && doubleClicked) service.Apply();
            if (hovered)
            {
                FlickUi::BeginTooltip();
                FlickUi::TextUnformatted(title.c_str());
                FlickUi::TextWrapped("%s", note.c_str());
                FlickUi::TextDisabled("%s", entry.originalName.c_str());
                FlickUi::TextDisabled("%s / slots %08X", entry.key.c_str(), entry.slots);
                FlickUi::EndTooltip();
            }
            if (FlickUi::BeginPopupContextItem("Card actions"))
            {
                FlickUi::BeginDisabled(!service.store.writable);
                if (FlickUi::MenuItem(Tr("Favorite", "收藏"), nullptr, favorite))
                {
                    auto& item = service.store.database["items"][entry.key];
                    if (!item.is_object()) item = Json::object();
                    item["favorite"] = !favorite; service.Save();
                }
                FlickUi::EndDisabled();
                if (FlickUi::MenuItem(Tr("Edit metadata", "编辑别名 / 备注 / 分类 / 部件"))) StartEdit(entry);
                if (FlickUi::MenuItem(Tr("Remove from working outfit", "从工作搭配移除"), nullptr, false, selected)) service.Remove(entry.key);
                FlickUi::BeginDisabled(!service.store.writable);
                if (FlickUi::MenuItem(Tr("Add to backup outfit", "添加到后备搭配"))) service.AddBackup(entry.key);
                FlickUi::EndDisabled();
                if (FlickUi::MenuItem(Tr("Apply working outfit", "应用当前工作搭配"))) service.Apply();
                FlickUi::EndPopup();
            }
            FlickUi::PopID();
        }

        bool MatchesView(const ClothingEntry& entry)
        {
            auto& service = Service::Get();
            const auto metadata = entry.metadata;
            switch (view)
            {
            case View::Favorites: return metadata.value("favorite", false);
            case View::Working: return service.InWorking(entry.key);
            case View::Plugin: return Normalize(entry.plugin) == Normalize(viewID);
            case View::Group: return service.InGroup(entry, CurrentGroup());
            case View::Category:
            {
                const auto categories = metadata.value("categories", std::vector<std::string>{});
                return std::find(categories.begin(), categories.end(), viewID) != categories.end();
            }
            case View::Uncategorized: return metadata.value("categories", std::vector<std::string>{}).empty();
            default: return true;
            }
        }

        void StartGroupEdit(const std::string& id)
        {
            const auto* metadata = Service::Get().Group(id);
            if (!metadata) return;
            editingGroup = id;
            groupName = metadata->value("name", SplitGroupID(id).second);
            groupAlias = metadata->value("alias", "");
            groupNote = metadata->value("note", "");
            groupSearch = Join(metadata->value("search", Json::array()));
            referenceSearch.clear();
            reverseReferenceSearch.clear();
            openGroupEditor = true;
        }

        void DrawGroupEditor()
        {
            if (openGroupEditor) { FlickUi::OpenPopup(Tr("Group metadata", "服装组信息")); openGroupEditor = false; }
            const auto* viewport = FlickUi::GetMainViewport();
            const float scale = FlickUi::GetFontSize() / 18.0f;
            FlickUi::SetNextWindowSize(ImVec2(std::min(900.0f * scale, viewport->WorkSize.x * 0.9f),
                std::min(780.0f * scale, viewport->WorkSize.y * 0.85f)), ImGuiCond_Appearing);
            if (!FlickUi::BeginPopupModal(Tr("Group metadata", "服装组信息"), nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
            auto& service = Service::Get();
            const auto [collection, id] = SplitGroupID(editingGroup);
            EditText(Tr("Name", "名称"), groupName);
            EditText(Tr("Alias", "别名"), groupAlias);
            EditText(Tr("Note", "备注"), groupNote, 60);
            EditText(Tr("Search keys", "搜索词（每行一个）"), groupSearch, 60);
            FlickUi::BeginDisabled(!service.store.writable || !service.Group(editingGroup));
            if (FlickUi::Button(Tr("Save", "保存")))
            {
                auto& metadata = service.store.database[collection][id];
                metadata["name"] = groupName; metadata["alias"] = groupAlias;
                metadata["note"] = groupNote; metadata["search"] = Split(groupSearch);
                service.Save(); FlickUi::CloseCurrentPopup();
            }
            if (collection == "pluginCategories")
            {
                if (FlickUi::Button(Tr("Use plugin name", "直接显示模组名"))) { groupName = id; groupAlias.clear(); }
                FlickUi::SameLine();
                if (FlickUi::Button(Tr("Regenerate alias template", "重新生成别名包模板"))) service.GenerateAliasTemplate(id, true);
                FlickUi::SameLine();
                if (FlickUi::Button(Tr("Unregister", "取消注册")))
                { service.store.database[collection][id]["enabled"] = false; service.Save(); FlickUi::CloseCurrentPopup(); }
            }
            FlickUi::SeparatorText(Tr("References", "引用服装组"));
            // 两个引用方向共用一套两栏布局：左栏点击建立引用，右栏点击解除引用。
            // 剩余高度两个列表各分一半，否则第一段列表会吃掉全部空间。
            const float referenceListHeight = std::max(80.0f, (FlickUi::GetContentRegionAvail().y
                - (FlickUi::GetFrameHeightWithSpacing() + FlickUi::GetTextLineHeightWithSpacing() * 2.0f) * 2.0f) * 0.5f);
            // 本组自己的引用列表；反向列表需要逐条查看对方是否引用本组。
            const auto refs = service.Group(editingGroup) ? service.Group(editingGroup)->value("references", std::vector<std::string>{}) : std::vector<std::string>{};
            // reverse 为真时编辑"对方引用本组"，为假时编辑"本组引用对方"。
            const auto drawGroupReferences = [&](const char* tableID, std::string& query, bool reverse)
            {
                FlickUi::PushID(tableID);
                FlickUi::SetNextItemWidth(-1.0f);
                FlickUi::InputTextWithHint("##search", Tr("Search groups / pinyin...", "搜索服装组 / 拼音…"), &query);
                // 每帧冻结两栏成员，点击后下一帧移动，避免同一次点击在另一栏再次处理。
                std::vector<std::string> linked;
                for (const char* field : {"categories", "pluginCategories", "groups"})
                    for (const auto& [otherID, metadata] : service.store.database[field].items())
                    {
                        const auto target = std::string(field) + ":" + otherID;
                        if (target == editingGroup) continue;
                        // 正向看本组的引用列表，反向看对方是否把本组列为引用。
                        const auto owners = reverse ? metadata.value("references", std::vector<std::string>{}) : refs;
                        const auto& wanted = reverse ? editingGroup : target;
                        if (std::find(owners.begin(), owners.end(), wanted) != owners.end()) linked.push_back(target);
                    }
                if (FlickUi::BeginTable("Group references", 2, ImGuiTableFlags_SizingStretchSame))
                {
                    for (int column = 0; column < 2; ++column)
                    {
                        FlickUi::TableNextColumn();
                        FlickUi::TextUnformatted(column == 0
                            ? (reverse ? Tr("Not referencing this group — click to add", "未引用本组 · 点击加入")
                                       : Tr("Available — click to reference", "未引用 · 点击添加"))
                            : (reverse ? Tr("Referencing this group — click to remove", "已引用本组 · 点击移除")
                                       : Tr("Referenced — click to remove", "已引用 · 点击移除")));
                        FlickUi::PushID(column);
                        FlickUi::BeginChild("Reference list", ImVec2(0, referenceListHeight), ImGuiChildFlags_Borders);
                        for (const char* field : {"categories", "pluginCategories", "groups"})
                            for (const auto& [otherID, metadata] : service.store.database[field].items())
                            {
                                const auto target = std::string(field) + ":" + otherID;
                                if (target == editingGroup) continue;
                                const bool referenced = std::find(linked.begin(), linked.end(), target) != linked.end();
                                if (referenced != (column == 1) || !MatchesGroupSearch(otherID, metadata, query)) continue;
                                if (FlickUi::Selectable((ObjectName(otherID, metadata) + "##" + target).c_str()))
                                {
                                    // 反向写入对方记录的引用，正向写入本组记录的引用。
                                    auto& owner = reverse ? service.store.database[field][otherID] : service.store.database[collection][id];
                                    const auto& member = reverse ? editingGroup : target;
                                    auto updated = owner.value("references", std::vector<std::string>{});
                                    std::erase(updated, member);
                                    if (!referenced) updated.push_back(member);
                                    owner["references"] = updated;
                                    service.Save();
                                }
                                if (FlickUi::IsItemHovered()) FlickUi::SetTooltip("%s", target.c_str());
                            }
                        FlickUi::EndChild();
                        FlickUi::PopID();
                    }
                    FlickUi::EndTable();
                }
                FlickUi::PopID();
            };
            drawGroupReferences("Reference", referenceSearch, false);
            FlickUi::SeparatorText(Tr("Add to other groups", "把自己加入对方的引用"));
            drawGroupReferences("Reverse reference", reverseReferenceSearch, true);
            FlickUi::EndDisabled();
            if (FlickUi::Button(Tr("Close", "关闭"))) FlickUi::CloseCurrentPopup();
            FlickUi::EndPopup();
        }

        void DrawGroupActions()
        {
            auto& service = Service::Get();
            const auto current = CurrentGroup();
            if (service.Group(current))
            {
                if (FlickUi::Button(Tr("Edit group metadata", "编辑服装组信息"))) StartGroupEdit(current);
                FlickUi::SameLine(); if (FlickUi::Button(Tr("Use outfit group", "使用服装组"))) service.UseGroup(current);
                FlickUi::SameLine();
                if (FlickUi::Button(Tr("Set backup outfit", "设为后备搭配"))) service.UseBackupGroup(current);
                FlickUi::SameLine();
                FlickUi::BeginDisabled(!service.store.writable);
                if (FlickUi::Button(Tr("Create subgroup", "创建子服装组")))
                {
                    quickGroup = service.CreateSubgroup(current);
                    if (!quickGroup.empty()) quickAction = QuickAction::Add;
                }
                FlickUi::EndDisabled();
                FlickUi::SameLine();
            }
            FlickUi::BeginDisabled(!service.store.writable);
            bool selectTarget = false;
            if (FlickUi::Button(Tr("Quick actions", "快捷操作"))) FlickUi::OpenPopup("Quick action menu");
            MarkManualAnchor(ManualTarget::Quick);
            if (FlickUi::BeginPopup("Quick action menu"))
            {
                if (FlickUi::MenuItem(Tr("Add to group", "添加到服装组"))) { pendingQuickAction = QuickAction::Add; selectTarget = true; }
                // 移除始终针对当前视图，模组的原生服装列表不支持直接删除成员。
                if (FlickUi::MenuItem(Tr("Remove from group", "从服装组移除"), nullptr, false, service.Group(current) && view != View::Plugin)) quickAction = QuickAction::Remove;
                if (FlickUi::MenuItem(Tr("Mark part", "标记部件分类"))) { pendingQuickAction = QuickAction::Part; selectTarget = true; }
                FlickUi::EndPopup();
            }
            // 先选操作，再确认目标；取消选择器不会改变正在进行的快捷操作。
            if (selectTarget) { quickTargetSearch.clear(); FlickUi::OpenPopup(Tr("Quick action target", "选择快捷操作目标")); }
            const auto* viewport = FlickUi::GetMainViewport();
            const float scale = FlickUi::GetFontSize() / 18.0f;
            FlickUi::SetNextWindowSize(ImVec2(std::min(560.0f * scale, viewport->WorkSize.x * 0.9f),
                std::min(420.0f * scale, viewport->WorkSize.y * 0.8f)), ImGuiCond_Appearing);
            if (FlickUi::BeginPopupModal(Tr("Quick action target", "选择快捷操作目标"), nullptr, ImGuiWindowFlags_NoSavedSettings))
            {
                FlickUi::TextUnformatted(pendingQuickAction == QuickAction::Part ? Tr("Select a part", "选择部件分类")
                    : Tr("Select a group to add to", "选择要添加到的服装组"));
                FlickUi::SetNextItemWidth(-1.0f);
                FlickUi::InputTextWithHint("##quick target search", Tr("Search / pinyin...", "搜索 / 拼音…"), &quickTargetSearch);
                FlickUi::BeginChild("Quick targets", ImVec2(0, std::max(60.0f, FlickUi::GetContentRegionAvail().y - FlickUi::GetFrameHeightWithSpacing())), ImGuiChildFlags_Borders);
                if (pendingQuickAction == QuickAction::Part)
                {
                    for (int i = 0; i < static_cast<int>(std::size(PartIDs)); ++i)
                        if ((quickTargetSearch.empty() || SearchScore(quickTargetSearch, PartName(i)) > 0)
                            && FlickUi::Selectable((PartName(i) + "##" + PartIDs[i]).c_str(), quickAction == QuickAction::Part && quickPart == i))
                        {
                            quickPart = i; quickAction = pendingQuickAction; FlickUi::CloseCurrentPopup();
                        }
                }
                else
                {
                    for (const char* collection : {"categories", "groups"})
                        for (const auto& [id, metadata] : service.store.database[collection].items())
                        {
                            const auto target = std::string(collection) + ":" + id;
                            if (!MatchesGroupSearch(id, metadata, quickTargetSearch)) continue;
                            if (FlickUi::Selectable((ObjectName(id, metadata) + "##" + target).c_str(), quickAction == pendingQuickAction && quickGroup == target))
                            {
                                quickGroup = target; quickAction = pendingQuickAction; FlickUi::CloseCurrentPopup();
                            }
                            if (FlickUi::IsItemHovered()) FlickUi::SetTooltip("%s", target.c_str());
                        }
                }
                FlickUi::EndChild();
                if (FlickUi::Button(Tr("Cancel", "取消"))) FlickUi::CloseCurrentPopup();
                FlickUi::EndPopup();
            }
            FlickUi::EndDisabled();
            if (quickAction == QuickAction::Add && !service.Group(quickGroup)) quickAction = QuickAction::None;
            if (quickAction == QuickAction::Remove && (!service.Group(current) || view == View::Plugin)) quickAction = QuickAction::None;
        }

        void DrawCatalogSettings()
        {
            auto& service = Service::Get();
            FlickUi::PushID("Catalog settings");
            FlickUi::SeparatorText(Tr("Clothing list layout", "服装列表布局"));
            bool changed = FlickUi::Checkbox(Tr("Arrange by part", "按部件分列"), &byPart);
            changed |= FlickUi::Checkbox(Tr("Automatically arrange columns", "自动调整列布局"), &automatic);
            FlickUi::TextWrapped("%s", Tr("When all configured columns are occupied, combine short categories that fit with spacing, then redistribute spare columns.",
                "设置列数全部占用后，才按可用高度合并较短分类；分类之间保留上方留白，腾出的列再分给仍需滚动的分类。"));
            FlickUi::SetNextItemWidth(160);
            changed |= FlickUi::SliderInt(Tr("Columns", "列数"), &columns, 1, 12);
            if (changed) { service.store.database["viewOptions"] = {{"byPart", byPart}, {"automatic", automatic}, {"columns", columns}}; service.Save(); }
            FlickUi::PopID();
        }

        void DrawCatalog()
        {
            auto& service = Service::Get();
            DrawGroupActions();
            FlickUi::SetNextItemWidth(-1);
            FlickUi::InputTextWithHint("##catalog search", Tr("Search name, pinyin, category, plugin, note...", "搜索名称、拼音、分类、插件、备注…"), &search);
            MarkManualAnchor(ManualTarget::Search);
            static std::vector<std::pair<std::size_t, int>> ranked;
            static std::uint64_t cachedRevision = static_cast<std::uint64_t>(-1);
            static std::string cachedSearch, cachedViewID;
            static View cachedView = View::All;
            // 大型服装目录只在筛选或元数据变更时重建排序，滚动帧仅绘制可见卡片。
            if (cachedRevision != service.revision || cachedSearch != search || cachedViewID != viewID || cachedView != view)
            {
                cachedRevision = service.revision; cachedSearch = search; cachedViewID = viewID; cachedView = view;
                ranked.clear();
                for (std::size_t i = 0; i < service.catalog.size(); ++i)
                    if (MatchesView(service.catalog[i]))
                        if (const auto score = service.Score(service.catalog[i], search); score > 0) ranked.emplace_back(i, score);
                std::stable_sort(ranked.begin(), ranked.end(), [&](const auto& a, const auto& b)
                {
                    if (a.second != b.second) return a.second > b.second;
                    return service.catalog[a.first].displayName < service.catalog[b.first].displayName;
                });
            }
            // 批量选取只属于快捷操作，不在普通浏览时修改工作搭配。
            if (quickAction != QuickAction::None)
            {
                const auto targetID = quickAction == QuickAction::Remove ? CurrentGroup() : quickGroup;
                const auto* target = service.Group(targetID);
                const auto targetName = quickAction == QuickAction::Part ? PartName(quickPart)
                    : target ? ObjectName(SplitGroupID(targetID).second, *target) : targetID;
                FlickUi::TextColored(ImVec4(1, 0.65f, 0.15f, 1), "%s: %s", quickAction == QuickAction::Add ? Tr("Adding to group", "正在快捷添加服装")
                    : quickAction == QuickAction::Part ? Tr("Marking part", "正在快捷标记部件") : Tr("Removing from group", "正在快捷移除服装"), targetName.c_str());
                FlickUi::SameLine();
                const bool finishQuickAction = FlickUi::Button(Tr("Finish quick action", "结束快捷操作"));
                FlickUi::SameLine();
                FlickUi::BeginDisabled(!service.store.writable || ranked.empty());
                int batchAction = 0;
                if (FlickUi::Button((std::string(Tr("Select all results", "选取全部结果")) + " (" + std::to_string(ranked.size()) + ")").c_str())) batchAction = 1;
                FlickUi::SameLine();
                if (FlickUi::Button((std::string(Tr("Deselect all results", "取消选取全部结果")) + " (" + std::to_string(ranked.size()) + ")").c_str())) batchAction = -1;
                FlickUi::EndDisabled();
                if (batchAction)
                {
                    std::vector<std::string> keys;
                    for (const auto& [index, score] : ranked) keys.push_back(service.catalog[index].key);
                    service.BeginBatch();
                    for (const auto& key : keys)
                    {
                        const auto* entry = service.Find(key); if (!entry) continue;
                        if (quickAction == QuickAction::Add || quickAction == QuickAction::Remove)
                            service.SetGroupItem(quickAction == QuickAction::Add ? quickGroup : CurrentGroup(), *entry, batchAction > 0);
                        else
                        {
                            auto& item = service.store.database["items"][key]; if (!item.is_object()) item = Json::object();
                            if (batchAction > 0) item["part"] = PartIDs[quickPart];
                            else if (item.value("part", "") == PartIDs[quickPart]) item.erase("part");
                        }
                    }
                    service.EndBatch(false);
                    // 本帧剩余绘制使用原始索引快照，下一帧按新 revision 重建结果。
                }
                if (finishQuickAction) quickAction = QuickAction::None;
            }
            FlickUi::TextDisabled("%zu %s", ranked.size(), Tr("items; click adds, double-click applies, right-click edits", "件；单击加入搭配，双击应用，右键编辑"));
            std::vector<std::string> references;
            if (const auto* group = service.Group(CurrentGroup()))
                for (const auto& id : group->value("references", std::vector<std::string>{}))
                    if (const auto* target = service.Group(id); target && (search.empty() || SearchMetadata(search, *target) || SearchScore(search, id))) references.push_back(id);
            const auto drawReferences = [&]()
            {
                for (const auto& id : references)
                {
                    const auto* target = service.Group(id);
                    if (target && FlickUi::Selectable((std::string(ICON_FA_FOLDER) + " " + ObjectName(SplitGroupID(id).second, *target) + "##ref-" + id).c_str())) NavigateGroup(id);
                }
            };
            if (!byPart) drawReferences();
            if (ranked.empty() && references.empty()) { FlickUi::TextWrapped("%s", Tr("No matching armor. Register plugins or adjust your filters.", "没有匹配的服装。请注册插件或调整筛选条件。")); return; }
            const float height = std::max(1.0f, FlickUi::GetContentRegionAvail().y);
            if (!byPart)
            {
                FlickUi::BeginChild("Catalog grid", ImVec2(0, height));
                const float rowHeight = CardHeight() + FlickUi::GetStyle().CellPadding.y * 2;
                const int rowCount = static_cast<int>((ranked.size() + columns - 1) / columns);
                const float rowStart = FlickUi::GetCursorPosY();
                const auto rows = FlickUi::VisibleRows(rowCount, rowHeight, rowStart,
                    FlickUi::GetScrollY(), height);
                // 直接定位跳过的行，避免 Dummy 额外引入 ItemSpacing 导致滚动边界跳动。
                FlickUi::SetCursorPosY(rowStart + rows.before);
                if (FlickUi::BeginTable("Cards", columns, ImGuiTableFlags_SizingStretchSame))
                {
                    for (int row = rows.first; row < rows.last; ++row)
                    {
                        FlickUi::TableNextRow(0, rowHeight);
                        for (int column = 0; column < columns; ++column)
                        {
                            const auto index = static_cast<std::size_t>(row * columns + column);
                            if (index >= ranked.size()) break;
                            FlickUi::TableSetColumnIndex(column);
                            DrawCard(service.catalog[ranked[index].first], FlickUi::GetContentRegionAvail().x);
                        }
                    }
                    FlickUi::EndTable();
                }
                FlickUi::SetCursorPosY(rowStart + rowCount * rowHeight);
                FlickUi::Dummy(ImVec2(0, 0));
                FlickUi::EndChild();
                return;
            }
            std::vector<PartGroup> groups;
            for (int part = 0; part < static_cast<int>(std::size(PartIDs)); ++part)
            {
                PartGroup group; group.part = part;
                for (const auto& [index, score] : ranked) if (service.Part(service.catalog[index]) == part) group.items.push_back(index);
                if (!group.items.empty()) groups.push_back(std::move(group));
            }
            const auto& style = FlickUi::GetStyle();
            // 预留表格横向滚动条、单元格及子窗口内边距，缩放字体或窗口后按实际像素重新规划。
            const float columnHeight = std::max(1.0f, height - style.ScrollbarSize - style.CellPadding.y * 2 - 2.0f);
            const float contentHeight = std::max(0.0f, columnHeight - style.WindowPadding.y * 2 - style.ChildBorderSize * 2 - 2.0f);
            const float headingHeight = FlickUi::GetTextLineHeightWithSpacing();
            const float itemHeight = CardHeight() + style.ItemSpacing.y;
            const float sectionPadding = FlickUi::GetFontSize();
            const auto layout = PlanPartColumns(groups, std::max(1, columns - (references.empty() ? 0 : 1)),
                automatic, contentHeight, headingHeight, itemHeight, sectionPadding);
            const int lanes = static_cast<int>(layout.size()) + (references.empty() ? 0 : 1);
            const float width = std::max(FlickUi::GetFontSize() * 11, (FlickUi::GetContentRegionAvail().x - 20) / columns);
            if (FlickUi::BeginTable("Part lanes", lanes, ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit, ImVec2(0, height)))
            {
                for (int i = 0; i < lanes; ++i) FlickUi::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, width);
                FlickUi::TableNextRow();
                int column = 0;
                if (!references.empty())
                {
                    FlickUi::TableSetColumnIndex(column++);
                    FlickUi::BeginChild("References scroll", ImVec2(width, columnHeight), ImGuiChildFlags_Borders);
                    FlickUi::TextUnformatted(Tr("Groups", "引用服装组"));
                    drawReferences();
                    FlickUi::EndChild();
                }
                for (const auto& plannedColumn : layout)
                {
                    FlickUi::TableSetColumnIndex(column);
                    FlickUi::PushID(column++);
                    const bool fits = plannedColumn.usedHeight <= contentHeight;
                    FlickUi::BeginChild("Part scroll", ImVec2(width, columnHeight), ImGuiChildFlags_Borders,
                        fits ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : ImGuiWindowFlags_None);
                    if (fits) FlickUi::SetScrollY(0);
                    bool firstSection = true;
                    for (const auto& section : plannedColumn.sections)
                    {
                        // 后续分类标题上方留出一行字体高度，预算与绘制使用相同的 padding。
                        if (!firstSection) FlickUi::SetCursorPosY(FlickUi::GetCursorPosY() + sectionPadding);
                        firstSection = false;
                        const auto& group = groups[section.group];
                        const int lane = section.lane;
                        FlickUi::PushID(group.part);
                        FlickUi::Text("%s (%d/%d)", PartName(group.part).c_str(), lane + 1, group.lanes);
                        const auto first = group.items.size() * lane / group.lanes;
                        const auto last = group.items.size() * (lane + 1) / group.lanes;
                        if (fits)
                        {
                            for (auto i = first; i < last; ++i)
                                DrawCard(service.catalog[group.items[i]], FlickUi::GetContentRegionAvail().x);
                        }
                        else
                        {
                            FlickUi::ListClipper clipper;
                            clipper.Begin(static_cast<int>(last - first), itemHeight);
                            while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                                DrawCard(service.catalog[group.items[first + i]], FlickUi::GetContentRegionAvail().x);
                        }
                        FlickUi::PopID();
                    }
                    FlickUi::EndChild(); FlickUi::PopID();
                }
                FlickUi::EndTable();
            }
        }

        void ViewButton(const char* label, View kind, const std::string& id = {})
        {
            if (FlickUi::Selectable(label, view == kind && viewID == id)) { view = kind; viewID = id; }
        }

        #include "wardrobe/PanelNavigation.inc"

        void DrawSidebar()
        {
            auto& service = Service::Get();
            auto& database = service.store.database;
            FlickUi::InputTextWithHint("##view search", Tr("Find groups / pinyin", "搜索服装组 / 拼音"), &sidebarSearch);
            ViewButton(Tr("All clothing", "全部服装"), View::All);
            ViewButton("★ Favorites", View::Favorites);
            ViewButton(Tr("Uncategorized", "未分类"), View::Uncategorized);
            ViewButton(Tr("Working outfit", "工作搭配"), View::Working);
            MarkManualAnchor(ManualTarget::Working);
            FlickUi::Separator();
            // 固定入口不参与滚动，下面三个分区只共用一个滚动容器。
            FlickUi::BeginChild("Scrollable group navigation", ImVec2(0, 0));
            const auto drawEntry = [&](const std::string& key, const std::string& directory, bool draggable)
            {
                const auto* metadata = service.Group(key);
                if (!metadata || !MatchesGroupSearch(SplitGroupID(key).second, *metadata, sidebarSearch)) return;
                FlickUi::PushID(key.c_str());
                const auto [collection, id] = SplitGroupID(key);
                std::string label = ObjectName(id, *metadata);
                if (collection == "pluginCategories") label += " (" + std::to_string(service.plugins.contains(id) ? service.plugins.at(id) : 0) + ")";
                ViewButton(label.c_str(), collection == "categories" ? View::Category : collection == "groups" ? View::Group : View::Plugin, id);
                if (FlickUi::IsItemHovered()) FlickUi::SetTooltip("%s", key.c_str());
                if (draggable) NavigationDrag(key, directory);
                else if (service.store.writable && FlickUi::BeginDragDropSource())
                {
                    FlickUi::SetDragDropPayload("SW_NAV", key.c_str(), key.size() + 1);
                    FlickUi::TextUnformatted(label.c_str()); FlickUi::EndDragDropSource();
                }
                NavigationMenu(key); FlickUi::PopID();
            };
            if (NavigationHeader("section:categories", Tr("Category groups", "分类服装组"), !sidebarSearch.empty()))
            {
                FlickUi::Indent(FlickUi::GetFontSize() * 0.55f);
                for (const auto& key : NavigationIDs("category")) drawEntry(key, "", true);
                FlickUi::Unindent(FlickUi::GetFontSize() * 0.55f);
            }
            FlickUi::SeparatorText(Tr("Outfit groups", "服装组"));
            const auto drawDirectory = [&](const std::string& id, const std::string& name)
            {
                auto ids = NavigationIDs("group", id);
                const bool matched = std::any_of(ids.begin(), ids.end(), [&](const auto& key)
                { const auto* metadata = service.Group(key); return metadata && MatchesGroupSearch(SplitGroupID(key).second, *metadata, sidebarSearch); });
                const bool nameMatched = !sidebarSearch.empty() && SearchScore(sidebarSearch, name) > 0;
                if (!sidebarSearch.empty() && !matched && !nameMatched) return;
                const auto key = id.empty() ? "section:unsorted" : "directory:" + id;
                const bool open = NavigationHeader(key, name + " (" + std::to_string(ids.size()) + ")", !sidebarSearch.empty());
                NavigationDrag(key, id, true);
                if (!id.empty()) NavigationMenu(key);
                if (open)
                {
                    FlickUi::Indent(FlickUi::GetFontSize() * 0.55f);
                    const auto savedSearch = sidebarSearch;
                    if (nameMatched) sidebarSearch.clear();
                    for (const auto& group : ids) drawEntry(group, id, true);
                    sidebarSearch = savedSearch;
                    FlickUi::Unindent(FlickUi::GetFontSize() * 0.55f);
                }
            };
            for (const auto& key : NavigationIDs("directory")) drawDirectory(key.substr(10), NavigationName(key));
            drawDirectory("", Tr("Unsorted", "未整理"));
            FlickUi::BeginDisabled(!service.store.writable);
            if (FlickUi::Button(Tr("New directory", "新建目录"))) { editingDirectory.clear(); directoryName.clear(); openDirectoryEditor = true; }
            if (FlickUi::Button(Tr("New outfit group", "新建服装组")))
            {
                std::string id;
                for (int i = 1;; ++i) { id = "group-" + std::to_string(i); if (!database["groups"].contains(id)) break; }
                database["groups"][id] = {{"name", id}, {"items", Json::array()}};
                service.Save(); NavigateGroup("groups:" + id); StartGroupEdit("groups:" + id);
            }
            FlickUi::EndDisabled();
            const bool pluginsOpen = NavigationHeader("section:plugins", Tr("Registered plugins", "已注册模组"), !sidebarSearch.empty());
            NavigationDrag("section:plugins", "@registered", true);
            if (pluginsOpen)
            {
                FlickUi::Indent(FlickUi::GetFontSize() * 0.55f);
                for (const auto& [id, plugin] : database["pluginCategories"].items())
                    if (plugin.value("enabled", true) && !database["navigation"]["placements"].contains("pluginCategories:" + id))
                        drawEntry("pluginCategories:" + id, "@registered", false);
                FlickUi::Unindent(FlickUi::GetFontSize() * 0.55f);
            }
            if (FlickUi::NavigationDragging() && FlickUi::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
            {
                const float y = FlickUi::GetMousePos().y;
                const float top = FlickUi::GetWindowPos().y, bottom = top + FlickUi::GetWindowSize().y;
                const float speed = FlickUi::GetFontSize() * 18 * FlickUi::GetDeltaTime();
                if (y < top + FlickUi::GetFontSize() * 1.5f) FlickUi::SetScrollY(std::max(0.0f, FlickUi::GetScrollY() - speed));
                else if (y > bottom - FlickUi::GetFontSize() * 1.5f) FlickUi::SetScrollY(FlickUi::GetScrollY() + speed);
            }
            FlickUi::EndChild();
        }

        #include "wardrobe/PanelActors.inc"

        std::string NewID(const Json& collection, std::string_view prefix);
        #include "wardrobe/PanelLayers.inc"

        std::string NewID(const Json& collection, std::string_view prefix)
        {
            for (std::uint64_t i = 1;; ++i)
            {
                auto id = std::string(prefix) + std::to_string(i);
                if (!collection.contains(id)) return id;
            }
        }

        void DrawPresets()
        {
            auto& service = Service::Get();
            auto& presets = service.store.database["presets"];
            DrawActorSelector();
            FlickUi::BeginDisabled(!service.store.writable);
            bool automaticOutfits = service.ActorProfile().value("automatic", false);
            if (FlickUi::Checkbox(Tr("Automatic outfits for this actor", "为当前角色启用自动换装"), &automaticOutfits)) service.SetAutomatic(automaticOutfits);
            if (service.MultiplePresets())
            {
                FlickUi::InputTextWithHint("##preset search", Tr("Search presets / pinyin", "搜索预设 / 拼音"), &presetSearch);
                std::string selected;
                for (const auto& [id, preset] : presets.items())
                {
                    if (preset.value("actor", "player") != service.currentActor || !MatchesGroupSearch(id, preset, presetSearch)) continue;
                    if (FlickUi::Selectable((ObjectName(id, preset) + "##" + id).c_str(), service.activePreset == id)) selected = id;
                }
                if (!selected.empty())
                {
                    service.ActivatePreset(selected);
                    presetName = ObjectName(selected, presets[selected]);
                    presetKeys = Join(presets[selected].value("search", Json::array()));
                }
            }
            const auto runtime = service.ActorProfile().value("runtime", Json::object());
            if (runtime.contains("state")) FlickUi::Text("%s: %s", StateName(runtime.value("state", "default")), (runtime.value("useWorking", false) ? std::string(Tr("Working outfit", "工作搭配")) : NavigationName(runtime.value("group", ""))).c_str());
            if (!runtime.value("error", "").empty()) FlickUi::TextWrapped("%s", runtime.value("error", "").c_str());
            if (FlickUi::Button(Tr("Resume / retry automatic", "恢复 / 重试自动换装"))) service.ResumeAutomatic();
            FlickUi::Separator();
            if (service.MultiplePresets())
            {
                EditText(Tr("Preset name", "预设名称"), presetName);
                EditText(Tr("Preset search keys", "预设搜索词（每行一个）"), presetKeys, 65);
                FlickUi::BeginDisabled(presetName.empty());
                if (FlickUi::Button(Tr("New preset", "新建预设")))
                {
                    const auto id = NewID(presets, "preset-");
                    presets[id] = {{"name", presetName}, {"search", Split(presetKeys)}, {"actor", service.currentActor}, {"group", ""},
                        {"rules", {{"default", {{"enabled", true}, {"mode", "single"}, {"groups", Json::array()}}}}}};
                    service.ActivatePreset(id); service.Save();
                }
                FlickUi::EndDisabled();
            }
            else FlickUi::TextUnformatted(Tr("Configure this actor's default outfit rules", "配置当前角色的默认换装规则"));
            if (presets.contains(service.activePreset) && presets[service.activePreset].value("actor", "player") == service.currentActor)
            {
                if (service.MultiplePresets())
                {
                    if (FlickUi::Button(Tr("Rename / update search keys", "更新名称 / 搜索词")))
                    { presets[service.activePreset]["name"] = presetName; presets[service.activePreset]["search"] = Split(presetKeys); service.Save(); }
                    FlickUi::SameLine();
                }
                if (FlickUi::Button(Tr("Apply default group", "手动应用默认服装组"))) service.SelectPreset(service.activePreset);
                FlickUi::SameLine();
                if (service.MultiplePresets() && FlickUi::Button(Tr("Delete preset", "删除预设"))) FlickUi::OpenPopup(Tr("Delete preset?", "删除预设？"));
                if (FlickUi::BeginPopupModal(Tr("Delete preset?", "删除预设？"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
                {
                    FlickUi::TextUnformatted(Tr("Delete preset? Working outfit and groups are retained.", "删除预设？工作搭配和服装组会保留。"));
                    if (FlickUi::Button(Tr("Delete", "删除")))
                    {
                        service.SetAutomatic(false); presets.erase(service.activePreset); service.activePreset.clear(); service.Save(); service.activePreset = service.ActorProfile().value("activePreset", ""); FlickUi::CloseCurrentPopup();
                    }
                    FlickUi::SameLine(); if (FlickUi::Button(Tr("Cancel", "取消"))) FlickUi::CloseCurrentPopup();
                    FlickUi::EndPopup();
                }
                if (presets.contains(service.activePreset)) {
                    FlickUi::NewLine();
                    DrawPresetRules(presets[service.activePreset]);
                }
            }
            FlickUi::EndDisabled();
        }

        void DrawManagement()
        {
            auto& service = Service::Get();
            auto& database = service.store.database;
            FlickUi::BeginDisabled(!service.store.writable);
            if (FlickUi::CollapsingHeader(Tr("Categories", "分类管理"), ImGuiTreeNodeFlags_DefaultOpen))
            {
                EditText(Tr("ID (stable)", "分类 ID（稳定标识）"), categoryID);
                EditText(Tr("Name", "名称"), categoryName);
                EditText(Tr("Search keys", "搜索词（每行一个）"), categorySearch, 65);
                if (FlickUi::Button(Tr("Create / update category", "创建 / 更新分类")) && !categoryName.empty())
                {
                    if (categoryID.empty()) categoryID = NewID(database["categories"], "category-");
                    auto& category = database["categories"][categoryID];
                    if (!category.is_object()) category = Json::object();
                    category["name"] = categoryName; category["search"] = Split(categorySearch); service.Save();
                }
                FlickUi::SameLine(); if (FlickUi::Button(Tr("New", "新建"))) { categoryID.clear(); categoryName.clear(); categorySearch.clear(); }
                if (FlickUi::Button(Tr("Delete category", "删除分类")) && database["categories"].contains(categoryID)) FlickUi::OpenPopup(Tr("Delete category?", "删除分类？"));
                if (FlickUi::BeginPopupModal(Tr("Delete category?", "删除分类？"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
                {
                    FlickUi::TextWrapped("%s", Tr("Remove this category from all items?", "删除分类并移除全部服装上的此分类标记？"));
                    if (FlickUi::Button(Tr("Delete", "删除")))
                    {
                        for (const auto& entry : service.catalog)
                        {
                            auto categories = service.store.Effective(entry.key).value("categories", std::vector<std::string>{});
                            const auto oldSize = categories.size(); std::erase(categories, categoryID);
                            if (categories.size() != oldSize)
                            {
                                auto& item = database["items"][entry.key]; if (!item.is_object()) item = Json::object();
                                item["categories"] = categories;
                            }
                        }
                        database["categories"].erase(categoryID); categoryID.clear(); categoryName.clear();
                        view = View::All; viewID.clear(); service.Save(); FlickUi::CloseCurrentPopup();
                    }
                    FlickUi::SameLine(); if (FlickUi::Button(Tr("Cancel", "取消"))) FlickUi::CloseCurrentPopup();
                    FlickUi::EndPopup();
                }
            }
            if (FlickUi::CollapsingHeader(Tr("Register plugin categories", "注册插件分类")))
            {
                FlickUi::InputTextWithHint("##plugin search", Tr("Plugin / ESP filter", "搜索插件 / ESP"), &pluginFilter);
                FlickUi::BeginChild("Plugin registry", ImVec2(0, 220), ImGuiChildFlags_Borders);
                for (const auto& [plugin, count] : service.plugins)
                {
                    if (!SearchScore(pluginFilter, plugin)) continue;
                    bool enabled = database["pluginCategories"].contains(plugin) && database["pluginCategories"][plugin].value("enabled", true);
                    FlickUi::PushID(plugin.c_str());
                    if (FlickUi::Checkbox("##registered", &enabled))
                    {
                        auto& metadata = database["pluginCategories"][plugin]; if (!metadata.is_object()) metadata = Json::object();
                        metadata["enabled"] = enabled;
                        service.Save();
                    }
                    FlickUi::SameLine();
                    if (FlickUi::Selectable((plugin + " (" + std::to_string(count) + ")").c_str(), selectedPlugin == plugin))
                    {
                        selectedPlugin = plugin;
                        const auto metadata = database["pluginCategories"].value(plugin, Json::object());
                        pluginAlias = metadata.value("alias", "");
                        // 已有别名优先；尚未命名时用插件原名作为可编辑的初始值。
                        if (pluginAlias.empty()) pluginAlias = plugin;
                        pluginKeys = Join(metadata.value("search", Json::array()));
                    }
                    FlickUi::PopID();
                }
                FlickUi::EndChild();
                FlickUi::TextUnformatted(selectedPlugin.c_str());
                EditText(Tr("Plugin alias", "插件别名"), pluginAlias);
                EditText(Tr("Plugin search keys", "插件搜索词（每行一个）"), pluginKeys, 65);
                if (FlickUi::Button(Tr("Save plugin metadata", "保存插件信息")) && !selectedPlugin.empty())
                {
                    auto& metadata = database["pluginCategories"][selectedPlugin]; if (!metadata.is_object()) metadata = Json::object();
                    metadata["alias"] = pluginAlias; metadata["search"] = Split(pluginKeys); service.Save();
                }
                const bool registered = !selectedPlugin.empty() && database["pluginCategories"].contains(selectedPlugin) &&
                    database["pluginCategories"][selectedPlugin].value("enabled", true);
                FlickUi::BeginDisabled(!registered);
                if (FlickUi::Button(Tr("Regenerate alias template", "重新生成别名包模板")))
                    service.GenerateAliasTemplate(selectedPlugin, true);
                FlickUi::EndDisabled();
                FlickUi::TextWrapped("%s", Tr("Regeneration replaces this plugin's generated template. Names use original item names; parts use current detection. Reload JSON after editing.",
                    "重新生成会覆盖该插件的生成模板。名称使用物品原名，部件使用当前识别结果；编辑文件后请重新加载 JSON。"));
            }
            if (FlickUi::CollapsingHeader(Tr("Custom part names", "自定义部件名称")))
            {
                for (int i = 15; i < 23; ++i)
                {
                    auto name = PartName(i);
                    if (EditText(PartLabel(i), name)) { database["partNames"][PartIDs[i]] = name; service.Save(); }
                }
            }
            FlickUi::EndDisabled();
            FlickUi::Separator();
            if (FlickUi::Button(Tr("Reload JSON and alias packs", "重新加载 JSON 与别名包"))) service.Reload();
            FlickUi::SameLine(); if (FlickUi::Button(Tr("Retry save", "重试保存"))) service.Save();
            FlickUi::TextWrapped("%s", Tr("User metadata overrides AliasPacks, which override plugin names. Files: Data/SKSE/Plugins/SkyWardrobe/", "优先级：用户数据 > 别名包 > 插件原名。文件目录：Data/SKSE/Plugins/SkyWardrobe/"));
            for (const auto& warning : service.store.warnings) FlickUi::TextWrapped("%s", warning.c_str());
        }
    }

    void QueueUiCommand(std::string action, std::string argument)
    {
        std::lock_guard lock(commandMutex); commands.emplace_back(std::move(action), std::move(argument));
    }

    void ProcessUiRequests()
    {
        Service::Get().PollSave();
        Service::Get().TickActors();
        std::vector<std::pair<std::string, std::string>> pending;
        { std::lock_guard lock(commandMutex); pending.swap(commands); }
        // 保留打开面板查看错误的入口，但不执行控制台排队的功能命令。
        if (!SosBackend::Get().IsInstalled()) return;
        auto& service = Service::Get();
        for (const auto& [action, argument] : pending)
        {
            if (action == "reload") service.Reload();
            else if (action == "clear") service.Apply(true);
            else if (action == "preset")
            {
                if (!service.MultiplePresets()) { service.status = "Multiple presets are disabled"; continue; }
                std::string found;
                for (const auto& [id, preset] : service.store.database["presets"].items())
                    if (preset.value("actor", "player") == service.currentActor && (Normalize(id) == Normalize(argument) || Normalize(ObjectName(id, preset)) == Normalize(argument))) { found = id; break; }
                if (found.empty()) service.status = "Preset not found: " + argument;
                else { service.SelectPreset(found); if (!service.livePreview && service.Group(service.store.database["presets"][found].value("group", ""))) service.Apply(); }
            }
        }
    }

    void DrawPanel()
    {
        manualAnchors = {};
        if (!SosBackend::Get().IsInstalled())
        {
            FlickUi::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "%s",
                Tr("Skyrim Outfit System (SOS) is not installed or loaded.", "未安装或未成功加载 Skyrim Outfit System（SOS）。"));
            FlickUi::TextWrapped("%s", Tr("All SkyWardrobe features are disabled. Install a compatible Skyrim Outfit System SE Revived and restart the game.",
                "SkyWardrobe 的全部功能已禁用。请安装与当前游戏版本兼容的 Skyrim Outfit System SE Revived，然后重启游戏。"));
            return;
        }
        auto& service = Service::Get();
        static bool settingsLoaded = false;
        if (SafeMode::Get().Active())
        {
            FlickUi::TextColored(ImVec4(1, 0.7f, 0.2f, 1), "%s", Tr("Safe mode: outfit operations paused", "安全模式：所有服装应用已暂停"));
            FlickUi::TextWrapped("%s", SafeMode::Get().Reason().c_str());
            FlickUi::TextWrapped("%s", Tr("Edit and save your wardrobe to remove faulty outfits. Preview and equipment changes are paused.",
                "请在衣柜、预设及服装组中移除问题服装并保存。预览、外观覆盖和实际装备操作均已暂停。"));
            if (playerOutfitStorage.load() == PlayerOutfitStorage::SaveLocal)
                FlickUi::TextWrapped("%s", Tr("Save the game after repairing player outfits. Loading an older save restores its old outfits.",
                    "玩家搭配使用存档局部存储：修复后请保存游戏，读取旧存档仍会恢复其中的旧搭配。"));
            if (!SafeMode::Get().Error().empty()) FlickUi::TextWrapped("%s", SafeMode::Get().Error().c_str());
            if (FlickUi::Button(Tr("Exit safe mode and resume outfits", "退出安全模式并恢复换装"))) service.ExitSafeMode();
            FlickUi::TextWrapped("%s", Tr("This deletes all SAFEMODE trigger files. INI safe_mode=1 will request safe mode again on the next launch.",
                "退出时删除全部 SAFEMODE 触发文件。若 INI 中仍为 safe_mode=1，下次启动仍会进入安全模式。"));
        }
        if (SafeMode::Get().Active()) FlickUi::Separator();
        if (!settingsLoaded)
        {
            const auto options = service.store.database.value("viewOptions", Json::object());
            if (options.is_object())
            {
                if (options.contains("byPart") && options["byPart"].is_boolean()) byPart = options["byPart"];
                if (options.contains("automatic") && options["automatic"].is_boolean()) automatic = options["automatic"];
                if (options.contains("columns") && options["columns"].is_number_integer()) columns = std::clamp(options["columns"].get<int>(), 1, 12);
            }
            settingsLoaded = true;
        }
        if (!service.store.error.empty()) FlickUi::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "%s", service.store.error.c_str());
        FlickUi::TextDisabled("%s | %s", service.status.c_str(), SosBackend::Get().Status().c_str());
        const auto shortcut = DrawQuickAccess(FlickUi::GetWindowPos(), FlickUi::GetWindowSize());
        const bool selectWardrobe = !shortcut.action.empty();
        if (shortcut.action == "category" || shortcut.action == "group")
        {
            NavigateGroup(shortcut.target);
            search.clear(); sidebarSearch.clear();
        }
        else if (shortcut.action == "preset" || shortcut.action == "applyPreset")
        {
            if (!service.MultiplePresets()) return;
            if (shortcut.action == "preset") service.ActivatePreset(shortcut.target);
            else
            {
                service.SelectPreset(shortcut.target);
                if (!service.livePreview) service.Apply();
            }
            view = View::Working; viewID.clear(); search.clear();
        }
        else if (shortcut.action == "useGroup")
        {
            service.UseGroup(shortcut.target);
            view = View::Working; viewID.clear(); search.clear();
        }
        if (FlickUi::BeginTabBar("Wardrobe pages"))
        {
            if (BeginManualTab(Tr("Wardrobe", "衣柜"), ManualTarget::Wardrobe, selectWardrobe ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
            {
                const float font = FlickUi::GetFontSize();
                const float available = FlickUi::GetContentRegionAvail().x;
                const float sidebarWidth = std::min(font * 12, available * 0.22f);
                const float workingWidth = std::min(font * 16, available * 0.27f);
                const float browseWidth = std::max(1.0f, available - sidebarWidth - workingWidth - FlickUi::GetStyle().ItemSpacing.x * 2);
                FlickUi::BeginChild("Views", ImVec2(sidebarWidth, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse); DrawSidebar(); FlickUi::EndChild();
                FlickUi::SameLine();
                FlickUi::BeginChild("Browse", ImVec2(browseWidth, 0));
                if (selectWardrobe) FlickUi::SetScrollY(0);
                DrawCatalog(); FlickUi::EndChild();
                FlickUi::SameLine(); FlickUi::BeginChild("Working", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse); DrawWorking(); FlickUi::EndChild();
                FlickUi::EndTabItem();
            }
            if (BeginManualTab(Tr("Actors", "角色管理"), ManualTarget::Actors)) { DrawActorManagement(); FlickUi::EndTabItem(); }
            if (BeginManualTab(Tr("Presets", "预设"), ManualTarget::Presets)) { DrawPresets(); FlickUi::EndTabItem(); }
            const auto metadataFlags = selectMetadata ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            selectMetadata = false;
            if (BeginManualTab(Tr("Metadata", "分类与数据"), ManualTarget::Metadata, metadataFlags)) { DrawManagement(); FlickUi::EndTabItem(); }
            if (BeginManualTab(Tr("Interface", "界面设置"), ManualTarget::Interface)) {
                if (FlickUi::Button(Tr("Open operation manual", "打开操作手册"))) { manualOpen = true; manualPage = -1; }
                FlickUi::TextWrapped("%s", Tr("Short tutorials with contents, examples, and arrows to the real controls.", "按主题查看简短教程、操作示例与实际按钮指引。"));
                FlickUi::Separator();
                bool multiple = service.MultiplePresets();
                FlickUi::BeginDisabled(!service.store.writable);
                if (FlickUi::Checkbox(Tr("Enable multiple presets (advanced)", "启用多预设（增强功能）"), &multiple)) service.SetMultiplePresets(multiple);
                FlickUi::EndDisabled();
                DrawCatalogSettings(); DrawQuickAccessSettings(); Overlay::DrawInterfaceSettings(); FlickUi::EndTabItem(); }
            FlickUi::EndTabBar();
        }
        DrawItemEditor();
        DrawGroupEditor();
        DrawNavigationDialogs();
        DrawManual();
    }
}
