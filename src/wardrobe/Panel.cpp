#include "wardrobe/Panel.h"
#include "wardrobe/Service.h"
#include "wardrobe/SosBackend.h"
#include "wardrobe/QuickAccess.h"
#include "ui/Overlay.h"
#include "ui/IconsFontAwesome6.h"
#include "ui/UiTabs.h"
#include <imgui.h>
#include <imgui_stdlib.h>
#include <array>
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
            ImGui::PushID(label);
            ImGui::TextWrapped("%s", label);
            ImGui::SetNextItemWidth(-1.0f);
            const bool changed = height > 0.0f
                ? ImGui::InputTextMultiline("##value", &value, ImVec2(-1.0f, height))
                : ImGui::InputText("##value", &value);
            ImGui::PopID();
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
            if (openEdit) { ImGui::OpenPopup(Tr("Item editor", "服装信息")); openEdit = false; }
            const auto* viewport = ImGui::GetMainViewport();
            const float scale = ImGui::GetFontSize() / 18.0f;
            ImGui::SetNextWindowSize(ImVec2(std::min(900.0f * scale, viewport->WorkSize.x * 0.9f),
                std::min(650.0f * scale, viewport->WorkSize.y * 0.85f)), ImGuiCond_Appearing);
            if (!ImGui::BeginPopupModal(Tr("Item editor", "服装信息"), nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
            auto& service = Service::Get();
            ImGui::TextUnformatted(editItem.c_str());
            EditText(Tr("Alias", "别名"), editAlias);
            EditText(Tr("Note", "备注"), editNote, 70);
            EditText(Tr("Search keys / pinyin (one per line)", "搜索词 / 拼音（每行一个）"), editSearch, 75);
            ImGui::Checkbox(Tr("Favorite", "收藏"), &editFavorite);
            if (ImGui::BeginCombo(Tr("Part", "部件"), PartName(editPart).c_str()))
            {
                for (int i = 0; i < static_cast<int>(std::size(PartIDs)); ++i)
                    if (ImGui::Selectable(PartName(i).c_str(), editPart == i)) editPart = i;
                ImGui::EndCombo();
            }
            ImGui::SeparatorText(Tr("Categories", "分类"));
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##item category search", Tr("Search categories / pinyin...", "搜索分类 / 拼音…"), &itemCategorySearch);
            // 两栏使用帧开始时的快照；分类修改留在编辑草稿中，保存后才写入服装数据。
            const auto selectedCategories = editCategories.get<std::vector<std::string>>();
            if (ImGui::BeginTable("Item categories", 2, ImGuiTableFlags_SizingStretchSame))
            {
                for (int column = 0; column < 2; ++column)
                {
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(column == 0 ? Tr("Available — click to add", "未分类 · 点击添加") : Tr("Assigned — click to remove", "已分类 · 点击移除"));
                    ImGui::PushID(column);
                    ImGui::BeginChild("Category list", ImVec2(0, std::max(80.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing())), ImGuiChildFlags_Borders);
                    for (const auto& [id, category] : service.store.database["categories"].items())
                    {
                        const bool selected = std::find(selectedCategories.begin(), selectedCategories.end(), id) != selectedCategories.end();
                        if (selected != (column == 1) || !MatchesGroupSearch(id, category, itemCategorySearch)) continue;
                        if (ImGui::Selectable((ObjectName(id, category) + "##" + id).c_str()))
                        {
                            auto values = editCategories.get<std::vector<std::string>>();
                            std::erase(values, id);
                            if (!selected) values.push_back(id);
                            editCategories = values;
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", id.c_str());
                    }
                    ImGui::EndChild();
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::BeginDisabled(!service.store.writable);
            if (ImGui::Button(Tr("Save", "保存")))
            {
                auto& item = service.store.database["items"][editItem];
                if (!item.is_object()) item = Json::object();
                item["alias"] = editAlias; item["note"] = editNote; item["favorite"] = editFavorite;
                item["search"] = Split(editSearch); item["categories"] = editCategories; item["part"] = PartIDs[editPart];
                if (service.Save()) ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(Tr("Reset user metadata", "恢复别名包 / 原始数据")))
            {
                service.store.database["items"].erase(editItem);
                if (service.Save()) ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button(Tr("Cancel", "取消"))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // 名字区固定占两行高度，卡片整体比单行名字时高一行；行距与高度必须共用同一组常量。
        constexpr float CardTitleLine = 1.05f;
        constexpr float CardTextHeight = 3.4f;

        float CardHeight()
        {
            return ImGui::GetFontSize() * (CardTextHeight + CardTitleLine);
        }

        // 逐字符测量 UTF-8 文本，返回能放进 width 的最长前缀末尾；至少推进一个字符，避免调用方死循环。
        const char* FitPrefix(ImFont* font, float size, const char* begin, const char* end, float width)
        {
            const char* fit = begin;
            for (const char* cursor = begin; cursor < end;)
            {
                // 跳过续字节按整字符测量，避免把多字节字符切成乱码。
                const char* next = cursor + 1;
                while (next < end && (static_cast<unsigned char>(*next) & 0xC0) == 0x80) ++next;
                if (fit != begin && font->CalcTextSizeA(size, FLT_MAX, 0.0f, begin, next).x > width) break;
                fit = cursor = next;
            }
            return fit;
        }

        // 取一行文字：优先在空白处断行，英文单词过长时退回字符级切分，返回值保证大于 begin。
        const char* WrapTitleLine(ImFont* font, float size, const char* begin, const char* end, float width)
        {
            const char* fit = FitPrefix(font, size, begin, end, width);
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
            ImGui::PushID(entry.key.c_str());
            const auto position = ImGui::GetCursorScreenPos();
            const float font = ImGui::GetFontSize(), lineHeight = font * CardTitleLine, height = CardHeight();
            const bool clicked = ImGui::InvisibleButton("card", ImVec2(std::max(30.0f, width), height));
            const bool hovered = ImGui::IsItemHovered();
            auto* draw = ImGui::GetWindowDrawList();
            const ImVec2 end(position.x + width, position.y + height);
            draw->AddRectFilled(position, end, ImGui::GetColorU32(selected ? ImGuiCol_HeaderActive : hovered ? ImGuiCol_HeaderHovered : ImGuiCol_FrameBg), 5);
            if (selected) draw->AddRect(position, end, quickAction == QuickAction::None ? ImGui::GetColorU32(ImGuiCol_CheckMark)
                : ImGui::GetColorU32(ImVec4(1.0f, 0.65f, 0.15f, 1.0f)), 5, 0, 3);
            draw->PushClipRect(ImVec2(position.x + 7, position.y + 3), ImVec2(end.x - 7, end.y - 3), true);
            // 名字占两行：第一行放不下就换到第二行，第二行仍放不下的部分不绘制，由裁剪区兜底。
            const float titleWidth = width - 16.0f;
            const auto* titleBegin = title.c_str();
            const auto* const titleEnd = titleBegin + title.size();
            const auto* lineEnd = WrapTitleLine(ImGui::GetFont(), font, titleBegin, titleEnd, titleWidth);
            draw->AddText(ImVec2(position.x + 9, position.y + 7), ImGui::GetColorU32(ImGuiCol_Text), titleBegin, lineEnd);
            if (lineEnd < titleEnd)
            {
                // 断行后可能留下前导空白，跳过它避免第二行以空格开头。
                while (lineEnd < titleEnd && (*lineEnd == ' ' || *lineEnd == '\t')) ++lineEnd;
                draw->AddText(ImVec2(position.x + 9, position.y + 7 + lineHeight), ImGui::GetColorU32(ImGuiCol_Text),
                    lineEnd, WrapTitleLine(ImGui::GetFont(), font, lineEnd, titleEnd, titleWidth));
            }
            // 小字号备注独立绘制，不缩放整个窗口；严格裁剪，避免长中文文本穿过相邻卡片。
            const float noteSize = font * Overlay::NoteFont()->LegacySize / ImGui::GetFont()->LegacySize;
            draw->AddText(Overlay::NoteFont(), noteSize, ImVec2(position.x + 9, position.y + font * 1.6f + lineHeight),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), note.c_str());
            const auto part = PartName(service.Part(entry));
            draw->AddText(ImGui::GetFont(), font * 0.7f, ImVec2(position.x + 9, position.y + font * 2.6f + lineHeight),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), part.c_str());
            draw->PopClipRect();
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
            if (clicked && quickAction == QuickAction::None && !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                if (selected) service.Remove(entry.key);
                else service.Add(entry.key);
            }
            if (quickAction == QuickAction::None && hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) service.Apply();
            if (hovered)
            {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(title.c_str());
                ImGui::TextWrapped("%s", note.c_str());
                ImGui::TextDisabled("%s", entry.originalName.c_str());
                ImGui::TextDisabled("%s / slots %08X", entry.key.c_str(), entry.slots);
                ImGui::EndTooltip();
            }
            if (ImGui::BeginPopupContextItem("Card actions"))
            {
                ImGui::BeginDisabled(!service.store.writable);
                if (ImGui::MenuItem(Tr("Favorite", "收藏"), nullptr, favorite))
                {
                    auto& item = service.store.database["items"][entry.key];
                    if (!item.is_object()) item = Json::object();
                    item["favorite"] = !favorite; service.Save();
                }
                ImGui::EndDisabled();
                if (ImGui::MenuItem(Tr("Edit metadata", "编辑别名 / 备注 / 分类 / 部件"))) StartEdit(entry);
                if (ImGui::MenuItem(Tr("Remove from working outfit", "从工作搭配移除"), nullptr, false, selected)) service.Remove(entry.key);
                ImGui::BeginDisabled(!service.store.writable);
                if (ImGui::MenuItem(Tr("Add to backup outfit", "添加到后备搭配"))) service.AddBackup(entry.key);
                ImGui::EndDisabled();
                if (ImGui::MenuItem(Tr("Apply working outfit", "应用当前工作搭配"))) service.Apply();
                ImGui::EndPopup();
            }
            ImGui::PopID();
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
            if (openGroupEditor) { ImGui::OpenPopup(Tr("Group metadata", "服装组信息")); openGroupEditor = false; }
            const auto* viewport = ImGui::GetMainViewport();
            const float scale = ImGui::GetFontSize() / 18.0f;
            ImGui::SetNextWindowSize(ImVec2(std::min(900.0f * scale, viewport->WorkSize.x * 0.9f),
                std::min(780.0f * scale, viewport->WorkSize.y * 0.85f)), ImGuiCond_Appearing);
            if (!ImGui::BeginPopupModal(Tr("Group metadata", "服装组信息"), nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
            auto& service = Service::Get();
            const auto [collection, id] = SplitGroupID(editingGroup);
            EditText(Tr("Name", "名称"), groupName);
            EditText(Tr("Alias", "别名"), groupAlias);
            EditText(Tr("Note", "备注"), groupNote, 60);
            EditText(Tr("Search keys", "搜索词（每行一个）"), groupSearch, 60);
            ImGui::BeginDisabled(!service.store.writable || !service.Group(editingGroup));
            if (ImGui::Button(Tr("Save", "保存")))
            {
                auto& metadata = service.store.database[collection][id];
                metadata["name"] = groupName; metadata["alias"] = groupAlias;
                metadata["note"] = groupNote; metadata["search"] = Split(groupSearch);
                service.Save(); ImGui::CloseCurrentPopup();
            }
            if (collection == "pluginCategories")
            {
                if (ImGui::Button(Tr("Use plugin name", "直接显示模组名"))) { groupName = id; groupAlias.clear(); }
                ImGui::SameLine();
                if (ImGui::Button(Tr("Regenerate alias template", "重新生成别名包模板"))) service.GenerateAliasTemplate(id, true);
                ImGui::SameLine();
                if (ImGui::Button(Tr("Unregister", "取消注册")))
                { service.store.database[collection][id]["enabled"] = false; service.Save(); ImGui::CloseCurrentPopup(); }
            }
            ImGui::SeparatorText(Tr("References", "引用服装组"));
            // 两个引用方向共用一套两栏布局：左栏点击建立引用，右栏点击解除引用。
            // 剩余高度两个列表各分一半，否则第一段列表会吃掉全部空间。
            const float referenceListHeight = std::max(80.0f, (ImGui::GetContentRegionAvail().y
                - (ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() * 2.0f) * 2.0f) * 0.5f);
            // 本组自己的引用列表；反向列表需要逐条查看对方是否引用本组。
            const auto refs = service.Group(editingGroup) ? service.Group(editingGroup)->value("references", std::vector<std::string>{}) : std::vector<std::string>{};
            // reverse 为真时编辑"对方引用本组"，为假时编辑"本组引用对方"。
            const auto drawGroupReferences = [&](const char* tableID, std::string& query, bool reverse)
            {
                ImGui::PushID(tableID);
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputTextWithHint("##search", Tr("Search groups / pinyin...", "搜索服装组 / 拼音…"), &query);
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
                if (ImGui::BeginTable("Group references", 2, ImGuiTableFlags_SizingStretchSame))
                {
                    for (int column = 0; column < 2; ++column)
                    {
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(column == 0
                            ? (reverse ? Tr("Not referencing this group — click to add", "未引用本组 · 点击加入")
                                       : Tr("Available — click to reference", "未引用 · 点击添加"))
                            : (reverse ? Tr("Referencing this group — click to remove", "已引用本组 · 点击移除")
                                       : Tr("Referenced — click to remove", "已引用 · 点击移除")));
                        ImGui::PushID(column);
                        ImGui::BeginChild("Reference list", ImVec2(0, referenceListHeight), ImGuiChildFlags_Borders);
                        for (const char* field : {"categories", "pluginCategories", "groups"})
                            for (const auto& [otherID, metadata] : service.store.database[field].items())
                            {
                                const auto target = std::string(field) + ":" + otherID;
                                if (target == editingGroup) continue;
                                const bool referenced = std::find(linked.begin(), linked.end(), target) != linked.end();
                                if (referenced != (column == 1) || !MatchesGroupSearch(otherID, metadata, query)) continue;
                                if (ImGui::Selectable((ObjectName(otherID, metadata) + "##" + target).c_str()))
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
                                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", target.c_str());
                            }
                        ImGui::EndChild();
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::PopID();
            };
            drawGroupReferences("Reference", referenceSearch, false);
            ImGui::SeparatorText(Tr("Add to other groups", "把自己加入对方的引用"));
            drawGroupReferences("Reverse reference", reverseReferenceSearch, true);
            ImGui::EndDisabled();
            if (ImGui::Button(Tr("Close", "关闭"))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        void DrawGroupActions()
        {
            auto& service = Service::Get();
            const auto current = CurrentGroup();
            if (service.Group(current))
            {
                if (ImGui::Button(Tr("Edit group metadata", "编辑服装组信息"))) StartGroupEdit(current);
                ImGui::SameLine(); if (ImGui::Button(Tr("Use outfit group", "使用服装组"))) service.UseGroup(current);
                ImGui::SameLine();
                if (ImGui::Button(Tr("Set backup outfit", "设为后备搭配"))) service.UseBackupGroup(current);
                ImGui::SameLine();
                ImGui::BeginDisabled(!service.store.writable);
                if (ImGui::Button(Tr("Create subgroup", "创建子服装组")))
                {
                    quickGroup = service.CreateSubgroup(current);
                    if (!quickGroup.empty()) quickAction = QuickAction::Add;
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
            }
            ImGui::BeginDisabled(!service.store.writable);
            bool selectTarget = false;
            if (ImGui::Button(Tr("Quick actions", "快捷操作"))) ImGui::OpenPopup("Quick action menu");
            if (ImGui::BeginPopup("Quick action menu"))
            {
                if (ImGui::MenuItem(Tr("Add to group", "添加到服装组"))) { pendingQuickAction = QuickAction::Add; selectTarget = true; }
                // 移除始终针对当前视图，模组的原生服装列表不支持直接删除成员。
                if (ImGui::MenuItem(Tr("Remove from group", "从服装组移除"), nullptr, false, service.Group(current) && view != View::Plugin)) quickAction = QuickAction::Remove;
                if (ImGui::MenuItem(Tr("Mark part", "标记部件分类"))) { pendingQuickAction = QuickAction::Part; selectTarget = true; }
                ImGui::EndPopup();
            }
            // 先选操作，再确认目标；取消选择器不会改变正在进行的快捷操作。
            if (selectTarget) { quickTargetSearch.clear(); ImGui::OpenPopup(Tr("Quick action target", "选择快捷操作目标")); }
            const auto* viewport = ImGui::GetMainViewport();
            const float scale = ImGui::GetFontSize() / 18.0f;
            ImGui::SetNextWindowSize(ImVec2(std::min(560.0f * scale, viewport->WorkSize.x * 0.9f),
                std::min(420.0f * scale, viewport->WorkSize.y * 0.8f)), ImGuiCond_Appearing);
            if (ImGui::BeginPopupModal(Tr("Quick action target", "选择快捷操作目标"), nullptr, ImGuiWindowFlags_NoSavedSettings))
            {
                ImGui::TextUnformatted(pendingQuickAction == QuickAction::Part ? Tr("Select a part", "选择部件分类")
                    : Tr("Select a group to add to", "选择要添加到的服装组"));
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputTextWithHint("##quick target search", Tr("Search / pinyin...", "搜索 / 拼音…"), &quickTargetSearch);
                ImGui::BeginChild("Quick targets", ImVec2(0, std::max(60.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing())), ImGuiChildFlags_Borders);
                if (pendingQuickAction == QuickAction::Part)
                {
                    for (int i = 0; i < static_cast<int>(std::size(PartIDs)); ++i)
                        if ((quickTargetSearch.empty() || SearchScore(quickTargetSearch, PartName(i)) > 0)
                            && ImGui::Selectable((PartName(i) + "##" + PartIDs[i]).c_str(), quickAction == QuickAction::Part && quickPart == i))
                        {
                            quickPart = i; quickAction = pendingQuickAction; ImGui::CloseCurrentPopup();
                        }
                }
                else
                {
                    for (const char* collection : {"categories", "groups"})
                        for (const auto& [id, metadata] : service.store.database[collection].items())
                        {
                            const auto target = std::string(collection) + ":" + id;
                            if (!MatchesGroupSearch(id, metadata, quickTargetSearch)) continue;
                            if (ImGui::Selectable((ObjectName(id, metadata) + "##" + target).c_str(), quickAction == pendingQuickAction && quickGroup == target))
                            {
                                quickGroup = target; quickAction = pendingQuickAction; ImGui::CloseCurrentPopup();
                            }
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", target.c_str());
                        }
                }
                ImGui::EndChild();
                if (ImGui::Button(Tr("Cancel", "取消"))) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::EndDisabled();
            if (quickAction == QuickAction::Add && !service.Group(quickGroup)) quickAction = QuickAction::None;
            if (quickAction == QuickAction::Remove && (!service.Group(current) || view == View::Plugin)) quickAction = QuickAction::None;
        }

        void DrawCatalogSettings()
        {
            auto& service = Service::Get();
            ImGui::PushID("Catalog settings");
            ImGui::SeparatorText(Tr("Clothing list layout", "服装列表布局"));
            bool changed = ImGui::Checkbox(Tr("Arrange by part", "按部件分列"), &byPart);
            changed |= ImGui::Checkbox(Tr("Automatically arrange columns", "自动调整列布局"), &automatic);
            ImGui::TextWrapped("%s", Tr("When all configured columns are occupied, combine short categories that fit with spacing, then redistribute spare columns.",
                "设置列数全部占用后，才按可用高度合并较短分类；分类之间保留上方留白，腾出的列再分给仍需滚动的分类。"));
            ImGui::SetNextItemWidth(160);
            changed |= ImGui::SliderInt(Tr("Columns", "列数"), &columns, 1, 12);
            if (changed) { service.store.database["viewOptions"] = {{"byPart", byPart}, {"automatic", automatic}, {"columns", columns}}; service.Save(); }
            ImGui::PopID();
        }

        void DrawCatalog()
        {
            auto& service = Service::Get();
            DrawGroupActions();
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##catalog search", Tr("Search name, pinyin, category, plugin, note...", "搜索名称、拼音、分类、插件、备注…"), &search);
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
                ImGui::TextColored(ImVec4(1, 0.65f, 0.15f, 1), "%s: %s", quickAction == QuickAction::Add ? Tr("Adding to group", "正在快捷添加服装")
                    : quickAction == QuickAction::Part ? Tr("Marking part", "正在快捷标记部件") : Tr("Removing from group", "正在快捷移除服装"), targetName.c_str());
                ImGui::SameLine();
                const bool finishQuickAction = ImGui::Button(Tr("Finish quick action", "结束快捷操作"));
                ImGui::SameLine();
                ImGui::BeginDisabled(!service.store.writable || ranked.empty());
                int batchAction = 0;
                if (ImGui::Button((std::string(Tr("Select all results", "选取全部结果")) + " (" + std::to_string(ranked.size()) + ")").c_str())) batchAction = 1;
                ImGui::SameLine();
                if (ImGui::Button((std::string(Tr("Deselect all results", "取消选取全部结果")) + " (" + std::to_string(ranked.size()) + ")").c_str())) batchAction = -1;
                ImGui::EndDisabled();
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
            ImGui::TextDisabled("%zu %s", ranked.size(), Tr("items; click adds, double-click applies, right-click edits", "件；单击加入搭配，双击应用，右键编辑"));
            std::vector<std::string> references;
            if (const auto* group = service.Group(CurrentGroup()))
                for (const auto& id : group->value("references", std::vector<std::string>{}))
                    if (const auto* target = service.Group(id); target && (search.empty() || SearchMetadata(search, *target) || SearchScore(search, id))) references.push_back(id);
            const auto drawReferences = [&]()
            {
                for (const auto& id : references)
                {
                    const auto* target = service.Group(id);
                    if (target && ImGui::Selectable((std::string(ICON_FA_FOLDER) + " " + ObjectName(SplitGroupID(id).second, *target) + "##ref-" + id).c_str())) NavigateGroup(id);
                }
            };
            if (!byPart) drawReferences();
            if (ranked.empty() && references.empty()) { ImGui::TextWrapped("%s", Tr("No matching armor. Register plugins or adjust your filters.", "没有匹配的服装。请注册插件或调整筛选条件。")); return; }
            const float height = std::max(1.0f, ImGui::GetContentRegionAvail().y);
            if (!byPart)
            {
                ImGui::BeginChild("Catalog grid", ImVec2(0, height));
                if (ImGui::BeginTable("Cards", columns, ImGuiTableFlags_SizingStretchSame))
                {
                    ImGuiListClipper clipper;
                    clipper.Begin(static_cast<int>((ranked.size() + columns - 1) / columns), CardHeight() + ImGui::GetStyle().CellPadding.y * 2);
                    while (clipper.Step())
                        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                        {
                            ImGui::TableNextRow();
                            for (int column = 0; column < columns; ++column)
                            {
                                const auto index = static_cast<std::size_t>(row * columns + column);
                                if (index >= ranked.size()) break;
                                ImGui::TableSetColumnIndex(column);
                                DrawCard(service.catalog[ranked[index].first], ImGui::GetContentRegionAvail().x);
                            }
                        }
                    ImGui::EndTable();
                }
                ImGui::EndChild();
                return;
            }
            std::vector<PartGroup> groups;
            for (int part = 0; part < static_cast<int>(std::size(PartIDs)); ++part)
            {
                PartGroup group; group.part = part;
                for (const auto& [index, score] : ranked) if (service.Part(service.catalog[index]) == part) group.items.push_back(index);
                if (!group.items.empty()) groups.push_back(std::move(group));
            }
            const auto& style = ImGui::GetStyle();
            // 预留表格横向滚动条、单元格及子窗口内边距，缩放字体或窗口后按实际像素重新规划。
            const float columnHeight = std::max(1.0f, height - style.ScrollbarSize - style.CellPadding.y * 2 - 2.0f);
            const float contentHeight = std::max(0.0f, columnHeight - style.WindowPadding.y * 2 - style.ChildBorderSize * 2 - 2.0f);
            const float headingHeight = ImGui::GetTextLineHeightWithSpacing();
            const float itemHeight = CardHeight() + style.ItemSpacing.y;
            const float sectionPadding = ImGui::GetFontSize();
            const auto layout = PlanPartColumns(groups, std::max(1, columns - (references.empty() ? 0 : 1)),
                automatic, contentHeight, headingHeight, itemHeight, sectionPadding);
            const int lanes = static_cast<int>(layout.size()) + (references.empty() ? 0 : 1);
            const float width = std::max(ImGui::GetFontSize() * 11, (ImGui::GetContentRegionAvail().x - 20) / columns);
            if (ImGui::BeginTable("Part lanes", lanes, ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit, ImVec2(0, height)))
            {
                for (int i = 0; i < lanes; ++i) ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, width);
                ImGui::TableNextRow();
                int column = 0;
                if (!references.empty())
                {
                    ImGui::TableSetColumnIndex(column++);
                    ImGui::BeginChild("References scroll", ImVec2(width, columnHeight), ImGuiChildFlags_Borders);
                    ImGui::TextUnformatted(Tr("Groups", "引用服装组"));
                    drawReferences();
                    ImGui::EndChild();
                }
                for (const auto& plannedColumn : layout)
                {
                    ImGui::TableSetColumnIndex(column);
                    ImGui::PushID(column++);
                    const bool fits = plannedColumn.usedHeight <= contentHeight;
                    ImGui::BeginChild("Part scroll", ImVec2(width, columnHeight), ImGuiChildFlags_Borders,
                        fits ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : ImGuiWindowFlags_None);
                    if (fits) ImGui::SetScrollY(0);
                    bool firstSection = true;
                    for (const auto& section : plannedColumn.sections)
                    {
                        // 后续分类标题上方留出一行字体高度，预算与绘制使用相同的 padding。
                        if (!firstSection) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + sectionPadding);
                        firstSection = false;
                        const auto& group = groups[section.group];
                        const int lane = section.lane;
                        ImGui::PushID(group.part);
                        ImGui::Text("%s (%d/%d)", PartName(group.part).c_str(), lane + 1, group.lanes);
                        const auto first = group.items.size() * lane / group.lanes;
                        const auto last = group.items.size() * (lane + 1) / group.lanes;
                        if (fits)
                        {
                            for (auto i = first; i < last; ++i)
                                DrawCard(service.catalog[group.items[i]], ImGui::GetContentRegionAvail().x);
                        }
                        else
                        {
                            ImGuiListClipper clipper;
                            clipper.Begin(static_cast<int>(last - first), itemHeight);
                            while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                                DrawCard(service.catalog[group.items[first + i]], ImGui::GetContentRegionAvail().x);
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndChild(); ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }

        void ViewButton(const char* label, View kind, const std::string& id = {})
        {
            if (ImGui::Selectable(label, view == kind && viewID == id)) { view = kind; viewID = id; }
        }

        #include "wardrobe/PanelNavigation.inc"

        void DrawSidebar()
        {
            auto& service = Service::Get();
            auto& database = service.store.database;
            ImGui::InputTextWithHint("##view search", Tr("Find groups / pinyin", "搜索服装组 / 拼音"), &sidebarSearch);
            ViewButton(Tr("All clothing", "全部服装"), View::All);
            ViewButton("★ Favorites", View::Favorites);
            ViewButton(Tr("Uncategorized", "未分类"), View::Uncategorized);
            ViewButton(Tr("Working outfit", "工作搭配"), View::Working);
            ImGui::Separator();
            // 固定入口不参与滚动，下面三个分区只共用一个滚动容器。
            ImGui::BeginChild("Scrollable group navigation", ImVec2(0, 0));
            const auto drawEntry = [&](const std::string& key, const std::string& directory, bool draggable)
            {
                const auto* metadata = service.Group(key);
                if (!metadata || !MatchesGroupSearch(SplitGroupID(key).second, *metadata, sidebarSearch)) return;
                ImGui::PushID(key.c_str());
                const auto [collection, id] = SplitGroupID(key);
                std::string label = ObjectName(id, *metadata);
                if (collection == "pluginCategories") label += " (" + std::to_string(service.plugins.contains(id) ? service.plugins.at(id) : 0) + ")";
                ViewButton(label.c_str(), collection == "categories" ? View::Category : collection == "groups" ? View::Group : View::Plugin, id);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", key.c_str());
                if (draggable) NavigationDrag(key, directory);
                else if (service.store.writable && ImGui::BeginDragDropSource())
                {
                    ImGui::SetDragDropPayload("SW_NAV", key.c_str(), key.size() + 1);
                    ImGui::TextUnformatted(label.c_str()); ImGui::EndDragDropSource();
                }
                NavigationMenu(key); ImGui::PopID();
            };
            if (NavigationHeader("section:categories", Tr("Category groups", "分类服装组"), !sidebarSearch.empty()))
            {
                ImGui::Indent(ImGui::GetFontSize() * 0.55f);
                for (const auto& key : NavigationIDs("category")) drawEntry(key, "", true);
                ImGui::Unindent(ImGui::GetFontSize() * 0.55f);
            }
            ImGui::SeparatorText(Tr("Outfit groups", "服装组"));
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
                    ImGui::Indent(ImGui::GetFontSize() * 0.55f);
                    const auto savedSearch = sidebarSearch;
                    if (nameMatched) sidebarSearch.clear();
                    for (const auto& group : ids) drawEntry(group, id, true);
                    sidebarSearch = savedSearch;
                    ImGui::Unindent(ImGui::GetFontSize() * 0.55f);
                }
            };
            for (const auto& key : NavigationIDs("directory")) drawDirectory(key.substr(10), NavigationName(key));
            drawDirectory("", Tr("Unsorted", "未整理"));
            ImGui::BeginDisabled(!service.store.writable);
            if (ImGui::Button(Tr("New directory", "新建目录"))) { editingDirectory.clear(); directoryName.clear(); openDirectoryEditor = true; }
            if (ImGui::Button(Tr("New outfit group", "新建服装组")))
            {
                std::string id;
                for (int i = 1;; ++i) { id = "group-" + std::to_string(i); if (!database["groups"].contains(id)) break; }
                database["groups"][id] = {{"name", id}, {"items", Json::array()}};
                service.Save(); NavigateGroup("groups:" + id); StartGroupEdit("groups:" + id);
            }
            ImGui::EndDisabled();
            const bool pluginsOpen = NavigationHeader("section:plugins", Tr("Registered plugins", "已注册模组"), !sidebarSearch.empty());
            NavigationDrag("section:plugins", "@registered", true);
            if (pluginsOpen)
            {
                ImGui::Indent(ImGui::GetFontSize() * 0.55f);
                for (const auto& [id, plugin] : database["pluginCategories"].items())
                    if (plugin.value("enabled", true) && !database["navigation"]["placements"].contains("pluginCategories:" + id))
                        drawEntry("pluginCategories:" + id, "@registered", false);
                ImGui::Unindent(ImGui::GetFontSize() * 0.55f);
            }
            if (ImGui::GetDragDropPayload() && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
            {
                const float y = ImGui::GetMousePos().y;
                const float top = ImGui::GetWindowPos().y, bottom = top + ImGui::GetWindowSize().y;
                const float speed = ImGui::GetFontSize() * 18 * ImGui::GetIO().DeltaTime;
                if (y < top + ImGui::GetFontSize() * 1.5f) ImGui::SetScrollY(std::max(0.0f, ImGui::GetScrollY() - speed));
                else if (y > bottom - ImGui::GetFontSize() * 1.5f) ImGui::SetScrollY(ImGui::GetScrollY() + speed);
            }
            ImGui::EndChild();
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
            ImGui::BeginDisabled(!service.store.writable);
            bool automaticOutfits = service.ActorProfile().value("automatic", false);
            if (ImGui::Checkbox(Tr("Automatic outfits for this actor", "为当前角色启用自动换装"), &automaticOutfits)) service.SetAutomatic(automaticOutfits);
            if (service.MultiplePresets())
            {
                ImGui::InputTextWithHint("##preset search", Tr("Search presets / pinyin", "搜索预设 / 拼音"), &presetSearch);
                std::string selected;
                for (const auto& [id, preset] : presets.items())
                {
                    if (preset.value("actor", "player") != service.currentActor || !MatchesGroupSearch(id, preset, presetSearch)) continue;
                    if (ImGui::Selectable((ObjectName(id, preset) + "##" + id).c_str(), service.activePreset == id)) selected = id;
                }
                if (!selected.empty())
                {
                    service.ActivatePreset(selected);
                    presetName = ObjectName(selected, presets[selected]);
                    presetKeys = Join(presets[selected].value("search", Json::array()));
                }
            }
            const auto runtime = service.ActorProfile().value("runtime", Json::object());
            if (runtime.contains("state")) ImGui::Text("%s: %s", StateName(runtime.value("state", "default")), (runtime.value("useWorking", false) ? std::string(Tr("Working outfit", "工作搭配")) : NavigationName(runtime.value("group", ""))).c_str());
            if (!runtime.value("error", "").empty()) ImGui::TextWrapped("%s", runtime.value("error", "").c_str());
            if (ImGui::Button(Tr("Resume / retry automatic", "恢复 / 重试自动换装"))) service.ResumeAutomatic();
            ImGui::Separator();
            if (service.MultiplePresets())
            {
                EditText(Tr("Preset name", "预设名称"), presetName);
                EditText(Tr("Preset search keys", "预设搜索词（每行一个）"), presetKeys, 65);
                ImGui::BeginDisabled(presetName.empty());
                if (ImGui::Button(Tr("New preset", "新建预设")))
                {
                    const auto id = NewID(presets, "preset-");
                    presets[id] = {{"name", presetName}, {"search", Split(presetKeys)}, {"actor", service.currentActor}, {"group", ""},
                        {"rules", {{"default", {{"enabled", true}, {"mode", "single"}, {"groups", Json::array()}}}}}};
                    service.ActivatePreset(id); service.Save();
                }
                ImGui::EndDisabled();
            }
            else ImGui::TextUnformatted(Tr("Configure this actor's default outfit rules", "配置当前角色的默认换装规则"));
            if (presets.contains(service.activePreset) && presets[service.activePreset].value("actor", "player") == service.currentActor)
            {
                if (service.MultiplePresets())
                {
                    if (ImGui::Button(Tr("Rename / update search keys", "更新名称 / 搜索词")))
                    { presets[service.activePreset]["name"] = presetName; presets[service.activePreset]["search"] = Split(presetKeys); service.Save(); }
                    ImGui::SameLine();
                }
                if (ImGui::Button(Tr("Apply default group", "手动应用默认服装组"))) service.SelectPreset(service.activePreset);
                ImGui::SameLine();
                if (service.MultiplePresets() && ImGui::Button(Tr("Delete preset", "删除预设"))) ImGui::OpenPopup(Tr("Delete preset?", "删除预设？"));
                if (ImGui::BeginPopupModal(Tr("Delete preset?", "删除预设？"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
                {
                    ImGui::TextUnformatted(Tr("Delete preset? Working outfit and groups are retained.", "删除预设？工作搭配和服装组会保留。"));
                    if (ImGui::Button(Tr("Delete", "删除")))
                    {
                        service.SetAutomatic(false); presets.erase(service.activePreset); service.activePreset.clear(); service.Save(); service.activePreset = service.ActorProfile().value("activePreset", ""); ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine(); if (ImGui::Button(Tr("Cancel", "取消"))) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
                if (presets.contains(service.activePreset)) {
                    ImGui::NewLine();
                    DrawPresetRules(presets[service.activePreset]);
                }
            }
            ImGui::EndDisabled();
        }

        void DrawManagement()
        {
            auto& service = Service::Get();
            auto& database = service.store.database;
            ImGui::BeginDisabled(!service.store.writable);
            if (ImGui::CollapsingHeader(Tr("Categories", "分类管理"), ImGuiTreeNodeFlags_DefaultOpen))
            {
                EditText(Tr("ID (stable)", "分类 ID（稳定标识）"), categoryID);
                EditText(Tr("Name", "名称"), categoryName);
                EditText(Tr("Search keys", "搜索词（每行一个）"), categorySearch, 65);
                if (ImGui::Button(Tr("Create / update category", "创建 / 更新分类")) && !categoryName.empty())
                {
                    if (categoryID.empty()) categoryID = NewID(database["categories"], "category-");
                    auto& category = database["categories"][categoryID];
                    if (!category.is_object()) category = Json::object();
                    category["name"] = categoryName; category["search"] = Split(categorySearch); service.Save();
                }
                ImGui::SameLine(); if (ImGui::Button(Tr("New", "新建"))) { categoryID.clear(); categoryName.clear(); categorySearch.clear(); }
                if (ImGui::Button(Tr("Delete category", "删除分类")) && database["categories"].contains(categoryID)) ImGui::OpenPopup(Tr("Delete category?", "删除分类？"));
                if (ImGui::BeginPopupModal(Tr("Delete category?", "删除分类？"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
                {
                    ImGui::TextWrapped("%s", Tr("Remove this category from all items?", "删除分类并移除全部服装上的此分类标记？"));
                    if (ImGui::Button(Tr("Delete", "删除")))
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
                        view = View::All; viewID.clear(); service.Save(); ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine(); if (ImGui::Button(Tr("Cancel", "取消"))) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
            }
            if (ImGui::CollapsingHeader(Tr("Register plugin categories", "注册插件分类")))
            {
                ImGui::InputTextWithHint("##plugin search", Tr("Plugin / ESP filter", "搜索插件 / ESP"), &pluginFilter);
                ImGui::BeginChild("Plugin registry", ImVec2(0, 220), ImGuiChildFlags_Borders);
                for (const auto& [plugin, count] : service.plugins)
                {
                    if (!SearchScore(pluginFilter, plugin)) continue;
                    bool enabled = database["pluginCategories"].contains(plugin) && database["pluginCategories"][plugin].value("enabled", true);
                    ImGui::PushID(plugin.c_str());
                    if (ImGui::Checkbox("##registered", &enabled))
                    {
                        auto& metadata = database["pluginCategories"][plugin]; if (!metadata.is_object()) metadata = Json::object();
                        metadata["enabled"] = enabled;
                        service.Save();
                    }
                    ImGui::SameLine();
                    if (ImGui::Selectable((plugin + " (" + std::to_string(count) + ")").c_str(), selectedPlugin == plugin))
                    {
                        selectedPlugin = plugin;
                        const auto metadata = database["pluginCategories"].value(plugin, Json::object());
                        pluginAlias = metadata.value("alias", "");
                        // 已有别名优先；尚未命名时用插件原名作为可编辑的初始值。
                        if (pluginAlias.empty()) pluginAlias = plugin;
                        pluginKeys = Join(metadata.value("search", Json::array()));
                    }
                    ImGui::PopID();
                }
                ImGui::EndChild();
                ImGui::TextUnformatted(selectedPlugin.c_str());
                EditText(Tr("Plugin alias", "插件别名"), pluginAlias);
                EditText(Tr("Plugin search keys", "插件搜索词（每行一个）"), pluginKeys, 65);
                if (ImGui::Button(Tr("Save plugin metadata", "保存插件信息")) && !selectedPlugin.empty())
                {
                    auto& metadata = database["pluginCategories"][selectedPlugin]; if (!metadata.is_object()) metadata = Json::object();
                    metadata["alias"] = pluginAlias; metadata["search"] = Split(pluginKeys); service.Save();
                }
                const bool registered = !selectedPlugin.empty() && database["pluginCategories"].contains(selectedPlugin) &&
                    database["pluginCategories"][selectedPlugin].value("enabled", true);
                ImGui::BeginDisabled(!registered);
                if (ImGui::Button(Tr("Regenerate alias template", "重新生成别名包模板")))
                    service.GenerateAliasTemplate(selectedPlugin, true);
                ImGui::EndDisabled();
                ImGui::TextWrapped("%s", Tr("Regeneration replaces this plugin's generated template. Names use original item names; parts use current detection. Reload JSON after editing.",
                    "重新生成会覆盖该插件的生成模板。名称使用物品原名，部件使用当前识别结果；编辑文件后请重新加载 JSON。"));
            }
            if (ImGui::CollapsingHeader(Tr("Custom part names", "自定义部件名称")))
            {
                for (int i = 15; i < 23; ++i)
                {
                    auto name = PartName(i);
                    if (EditText(PartLabel(i), name)) { database["partNames"][PartIDs[i]] = name; service.Save(); }
                }
            }
            ImGui::EndDisabled();
            ImGui::Separator();
            if (ImGui::Button(Tr("Reload JSON and alias packs", "重新加载 JSON 与别名包"))) service.Reload();
            ImGui::SameLine(); if (ImGui::Button(Tr("Retry save", "重试保存"))) service.Save();
            ImGui::TextWrapped("%s", Tr("User metadata overrides AliasPacks, which override plugin names. Files: Data/SKSE/Plugins/SkyWardrobe/", "优先级：用户数据 > 别名包 > 插件原名。文件目录：Data/SKSE/Plugins/SkyWardrobe/"));
            for (const auto& warning : service.store.warnings) ImGui::TextWrapped("%s", warning.c_str());
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
        if (!SosBackend::Get().IsInstalled())
        {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "%s",
                Tr("Skyrim Outfit System (SOS) is not installed or loaded.", "未安装或未成功加载 Skyrim Outfit System（SOS）。"));
            ImGui::TextWrapped("%s", Tr("All SkyWardrobe features are disabled. Install a compatible Skyrim Outfit System SE Revived and restart the game.",
                "SkyWardrobe 的全部功能已禁用。请安装与当前游戏版本兼容的 Skyrim Outfit System SE Revived，然后重启游戏。"));
            return;
        }
        auto& service = Service::Get();
        static bool settingsLoaded = false;
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
        if (!service.store.error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "%s", service.store.error.c_str());
        ImGui::TextDisabled("%s | %s", service.status.c_str(), SosBackend::Get().Status().c_str());
        const auto shortcut = DrawQuickAccess(ImGui::GetWindowPos(), ImGui::GetWindowSize());
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
        if (ImGui::BeginTabBar("Wardrobe pages"))
        {
            if (ImGui::BeginTabItem(Tr("Wardrobe", "衣柜"), nullptr, selectWardrobe ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
            {
                const float font = ImGui::GetFontSize();
                const float available = ImGui::GetContentRegionAvail().x;
                const float sidebarWidth = std::min(font * 12, available * 0.22f);
                const float workingWidth = std::min(font * 16, available * 0.27f);
                const float browseWidth = std::max(1.0f, available - sidebarWidth - workingWidth - ImGui::GetStyle().ItemSpacing.x * 2);
                ImGui::BeginChild("Views", ImVec2(sidebarWidth, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse); DrawSidebar(); ImGui::EndChild();
                ImGui::SameLine();
                ImGui::BeginChild("Browse", ImVec2(browseWidth, 0));
                if (selectWardrobe) ImGui::SetScrollY(0);
                DrawCatalog(); ImGui::EndChild();
                ImGui::SameLine(); ImGui::BeginChild("Working", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse); DrawWorking(); ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(Tr("Actors", "角色管理"))) { DrawActorManagement(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem(Tr("Presets", "预设"))) { DrawPresets(); ImGui::EndTabItem(); }
            const auto metadataFlags = selectMetadata ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            selectMetadata = false;
            if (ImGui::BeginTabItem(Tr("Metadata", "分类与数据"), nullptr, metadataFlags)) { DrawManagement(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem(Tr("Interface", "界面设置"))) {
                bool multiple = service.MultiplePresets();
                ImGui::BeginDisabled(!service.store.writable);
                if (ImGui::Checkbox(Tr("Enable multiple presets (advanced)", "启用多预设（增强功能）"), &multiple)) service.SetMultiplePresets(multiple);
                ImGui::EndDisabled();
                DrawCatalogSettings(); DrawQuickAccessSettings(); Overlay::DrawInterfaceSettings(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        DrawItemEditor();
        DrawGroupEditor();
        DrawNavigationDialogs();
    }
}
