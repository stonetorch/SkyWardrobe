#include "wardrobe/Panel.h"
#include "wardrobe/Service.h"
#include "wardrobe/SosBackend.h"
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
        enum class View { All, Favorites, Uncategorized, Working, Category, Plugin };
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
            const auto name = metadata.value("name", metadata.value("alias", id));
            return name.empty() ? id : name;
        }

        void StartEdit(const ClothingEntry& entry)
        {
            auto& service = Service::Get();
            const auto metadata = entry.metadata;
            editItem = entry.key;
            editAlias = metadata.value("alias", "");
            editNote = metadata.value("note", "");
            editSearch = Join(metadata.value("search", Json::array()));
            if (metadata.contains("pinyin")) editSearch += (editSearch.empty() ? "" : "\n") + metadata["pinyin"].get<std::string>();
            editCategories = metadata.value("categories", Json::array());
            editPart = service.Part(entry);
            editFavorite = metadata.value("favorite", false);
            openEdit = true;
        }

        void DrawItemEditor()
        {
            if (openEdit) { ImGui::OpenPopup(Tr("Item editor", "服装信息")); openEdit = false; }
            ImGui::SetNextWindowSize(ImVec2(550, 570), ImGuiCond_FirstUseEver);
            if (!ImGui::BeginPopupModal(Tr("Item editor", "服装信息"), nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
            auto& service = Service::Get();
            ImGui::TextUnformatted(editItem.c_str());
            ImGui::InputText(Tr("Alias", "别名"), &editAlias);
            ImGui::InputTextMultiline(Tr("Note", "备注"), &editNote, ImVec2(-1, 70));
            ImGui::InputTextMultiline(Tr("Search keys / pinyin (one per line)", "搜索词 / 拼音（每行一个）"), &editSearch, ImVec2(-1, 75));
            ImGui::Checkbox(Tr("Favorite", "收藏"), &editFavorite);
            if (ImGui::BeginCombo(Tr("Part", "部件"), PartName(editPart).c_str()))
            {
                for (int i = 0; i < static_cast<int>(std::size(PartIDs)); ++i)
                    if (ImGui::Selectable(PartName(i).c_str(), editPart == i)) editPart = i;
                ImGui::EndCombo();
            }
            ImGui::SeparatorText(Tr("Categories", "分类"));
            ImGui::BeginChild("Item categories", ImVec2(0, 110), ImGuiChildFlags_Borders);
            for (const auto& [id, category] : service.store.database["categories"].items())
            {
                bool selected = std::find(editCategories.begin(), editCategories.end(), Json(id)) != editCategories.end();
                if (ImGui::Checkbox((ObjectName(id, category) + "##" + id).c_str(), &selected))
                {
                    auto values = editCategories.get<std::vector<std::string>>();
                    if (selected) values.push_back(id); else std::erase(values, id);
                    editCategories = values;
                }
            }
            ImGui::EndChild();
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

        void DrawCard(const ClothingEntry& entry, float width)
        {
            auto& service = Service::Get();
            const auto metadata = entry.metadata;
            const bool favorite = metadata.value("favorite", false), selected = service.InWorking(entry.key);
            const auto title = std::string(favorite ? "★ " : "") + service.Name(entry);
            const auto note = metadata.value("note", "");
            ImGui::PushID(entry.key.c_str());
            const auto position = ImGui::GetCursorScreenPos();
            const float font = ImGui::GetFontSize(), height = font * 3.4f;
            const bool clicked = ImGui::InvisibleButton("card", ImVec2(std::max(30.0f, width), height));
            const bool hovered = ImGui::IsItemHovered();
            auto* draw = ImGui::GetWindowDrawList();
            const ImVec2 end(position.x + width, position.y + height);
            draw->AddRectFilled(position, end, ImGui::GetColorU32(selected ? ImGuiCol_HeaderActive : hovered ? ImGuiCol_HeaderHovered : ImGuiCol_FrameBg), 5);
            if (selected) draw->AddRect(position, end, ImGui::GetColorU32(ImGuiCol_CheckMark), 5);
            draw->PushClipRect(ImVec2(position.x + 7, position.y + 3), ImVec2(end.x - 7, end.y - 3), true);
            draw->AddText(ImVec2(position.x + 9, position.y + 7), ImGui::GetColorU32(ImGuiCol_Text), title.c_str());
            // 小字号备注独立绘制，不缩放整个窗口；严格裁剪，避免长中文文本穿过相邻卡片。
            const float noteSize = font * Overlay::NoteFont()->LegacySize / ImGui::GetFont()->LegacySize;
            draw->AddText(Overlay::NoteFont(), noteSize, ImVec2(position.x + 9, position.y + font * 1.6f),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), note.c_str());
            const auto part = PartName(service.Part(entry));
            draw->AddText(ImGui::GetFont(), font * 0.7f, ImVec2(position.x + 9, position.y + font * 2.6f),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), part.c_str());
            draw->PopClipRect();
            if (clicked) service.Add(entry.key);
            if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) service.Apply();
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
            case View::Category:
            {
                const auto categories = metadata.value("categories", std::vector<std::string>{});
                return std::find(categories.begin(), categories.end(), viewID) != categories.end();
            }
            case View::Uncategorized: return metadata.value("categories", std::vector<std::string>{}).empty();
            default: return true;
            }
        }

        void DrawCatalog()
        {
            auto& service = Service::Get();
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##catalog search", Tr("Search name, pinyin, category, plugin, note...", "搜索名称、拼音、分类、插件、备注…"), &search);
            bool changed = ImGui::Checkbox(Tr("Arrange by part", "按部件分列"), &byPart);
            ImGui::SameLine(); changed |= ImGui::Checkbox(Tr("Distribute extra columns", "自动分配空余列"), &automatic);
            ImGui::SameLine(); ImGui::SetNextItemWidth(110);
            changed |= ImGui::SliderInt(Tr("Columns", "列数"), &columns, 1, 12);
            if (changed) { service.store.database["viewOptions"] = {{"byPart", byPart}, {"automatic", automatic}, {"columns", columns}}; service.Save(); }
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
            ImGui::TextDisabled("%zu %s", ranked.size(), Tr("items; click adds, double-click applies, right-click edits", "件；单击加入搭配，双击应用，右键编辑"));
            if (ranked.empty()) { ImGui::TextWrapped("%s", Tr("No matching armor. Register plugins or adjust your filters.", "没有匹配的服装。请注册插件或调整筛选条件。")); return; }
            const float height = std::max(80.0f, ImGui::GetContentRegionAvail().y);
            if (!byPart)
            {
                ImGui::BeginChild("Catalog grid", ImVec2(0, height));
                if (ImGui::BeginTable("Cards", columns, ImGuiTableFlags_SizingStretchSame))
                {
                    ImGuiListClipper clipper;
                    clipper.Begin(static_cast<int>((ranked.size() + columns - 1) / columns), ImGui::GetFontSize() * 3.4f + ImGui::GetStyle().CellPadding.y * 2);
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
            AllocateLanes(groups, columns, automatic);
            const int lanes = std::accumulate(groups.begin(), groups.end(), 0, [](int sum, const auto& group) { return sum + group.lanes; });
            const float width = std::max(ImGui::GetFontSize() * 11, (ImGui::GetContentRegionAvail().x - 20) / columns);
            if (ImGui::BeginTable("Part lanes", lanes, ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit, ImVec2(0, height)))
            {
                for (int i = 0; i < lanes; ++i) ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, width);
                ImGui::TableNextRow();
                int column = 0;
                for (const auto& group : groups)
                    for (int lane = 0; lane < group.lanes; ++lane)
                    {
                        ImGui::TableSetColumnIndex(column);
                        ImGui::PushID(column++);
                        ImGui::Text("%s (%d/%d)", PartName(group.part).c_str(), lane + 1, group.lanes);
                        ImGui::BeginChild("Part scroll", ImVec2(width, height - ImGui::GetFontSize() * 2), ImGuiChildFlags_Borders);
                        const auto first = group.items.size() * lane / group.lanes;
                        const auto last = group.items.size() * (lane + 1) / group.lanes;
                        ImGuiListClipper clipper;
                        clipper.Begin(static_cast<int>(last - first), ImGui::GetFontSize() * 3.4f + ImGui::GetStyle().ItemSpacing.y);
                        while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                            DrawCard(service.catalog[group.items[first + i]], ImGui::GetContentRegionAvail().x);
                        ImGui::EndChild(); ImGui::PopID();
                    }
                ImGui::EndTable();
            }
        }

        void ViewButton(const char* label, View kind, const std::string& id = {})
        {
            if (ImGui::Selectable(label, view == kind && viewID == id)) { view = kind; viewID = id; }
        }

        void DrawSidebar()
        {
            auto& service = Service::Get();
            auto& database = service.store.database;
            ImGui::InputTextWithHint("##view search", Tr("Find categories / pinyin", "搜索分类 / 拼音"), &sidebarSearch);
            ViewButton(Tr("All clothing", "全部服装"), View::All);
            ViewButton("★ Favorites", View::Favorites);
            ViewButton(Tr("Uncategorized", "未分类"), View::Uncategorized);
            ViewButton(Tr("Working outfit", "工作搭配"), View::Working);
            ImGui::SeparatorText(Tr("Categories", "分类"));
            for (const auto& [id, category] : database["categories"].items())
            {
                if (!sidebarSearch.empty() && !SearchScore(sidebarSearch, id) && !SearchMetadata(sidebarSearch, category)) continue;
                ImGui::PushID(id.c_str());
                ViewButton(ObjectName(id, category).c_str(), View::Category, id);
                if (ImGui::BeginPopupContextItem("Category menu"))
                {
                    if (ImGui::MenuItem(Tr("Edit category", "编辑分类")))
                    { categoryID = id; categoryName = ObjectName(id, category); categorySearch = Join(category.value("search", Json::array())); selectMetadata = true; }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            ImGui::SeparatorText(Tr("Registered plugins", "已注册插件"));
            for (const auto& [id, plugin] : database["pluginCategories"].items())
            {
                if (!plugin.value("enabled", true)) continue;
                if (!sidebarSearch.empty() && !SearchScore(sidebarSearch, id) && !SearchMetadata(sidebarSearch, plugin)) continue;
                ImGui::PushID(id.c_str());
                const auto count = service.plugins.contains(id) ? service.plugins.at(id) : 0;
                ViewButton((ObjectName(id, plugin) + " (" + std::to_string(count) + ")").c_str(), View::Plugin, id);
                ImGui::PopID();
            }
        }

        void DrawWorking()
        {
            auto& service = Service::Get();
            auto& presets = service.store.database["presets"];
            std::string title = Tr("Unsaved outfit", "未命名搭配");
            if (presets.contains(service.activePreset)) title = ObjectName(service.activePreset, presets[service.activePreset]);
            if (service.Dirty()) title += " *";
            ImGui::TextWrapped("%s", title.c_str());
            ImGui::Checkbox(Tr("Live preview", "实时预览"), &service.livePreview);
            if (ImGui::Button(Tr("Apply", "应用外观"))) service.Apply();
            ImGui::SameLine(); if (ImGui::Button(Tr("Clear appearance", "清除外观覆盖"))) service.Apply(true);
            ImGui::BeginDisabled(service.activePreset.empty() || !presets.contains(service.activePreset) || !service.store.writable);
            if (ImGui::Button(Tr("Overwrite current preset", "一键覆盖当前预设"))) service.OverwritePreset();
            if (ImGui::Button(Tr("Restore preset", "恢复预设"))) service.SelectPreset(service.activePreset);
            ImGui::EndDisabled();
            if (ImGui::Button(Tr("Undo overwrite", "撤销上次覆盖"))) service.UndoOverwrite();
            ImGui::SeparatorText(Tr("Working items", "工作搭配部件"));
            ImGui::BeginChild("Working items", ImVec2(0, 0));
            const auto items = service.working;
            for (const auto& key : items)
            {
                ImGui::PushID(key.c_str());
                if (ImGui::SmallButton("×")) service.Remove(key);
                ImGui::SameLine();
                const auto* entry = service.Find(key);
                const auto name = entry ? service.Name(*entry) : Tr("Missing: ", "缺失：") + key;
                ImGui::TextWrapped("%s", name.c_str());
                if (entry && ImGui::IsItemClicked(ImGuiMouseButton_Right)) StartEdit(*entry);
                ImGui::PopID();
            }
            ImGui::EndChild();
        }

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
            ImGui::InputTextWithHint("##preset search", Tr("Search presets / pinyin", "搜索预设 / 拼音"), &presetSearch);
            std::string selection;
            for (const auto& [id, preset] : presets.items())
            {
                if (!presetSearch.empty() && !SearchMetadata(presetSearch, preset) && !SearchScore(presetSearch, id)) continue;
                if (ImGui::Selectable((ObjectName(id, preset) + "##" + id).c_str(), service.activePreset == id)) selection = id;
            }
            if (!selection.empty())
            {
                service.SelectPreset(selection);
                presetName = ObjectName(selection, presets[selection]);
                presetKeys = Join(presets[selection].value("search", Json::array()));
            }
            ImGui::BeginDisabled(!presets.contains(service.activePreset));
            if (ImGui::Button(Tr("Apply selected preset", "应用选中预设")))
            {
                service.SelectPreset(service.activePreset);
                if (!service.livePreview) service.Apply();
            }
            ImGui::EndDisabled();
            ImGui::Separator();
            ImGui::InputText(Tr("Preset name", "预设名称"), &presetName);
            ImGui::InputTextMultiline(Tr("Preset search keys", "预设搜索词（每行一个）"), &presetKeys, ImVec2(-1, 65));
            ImGui::BeginDisabled(!service.store.writable || presetName.empty());
            if (ImGui::Button(Tr("Save working outfit as new", "将工作搭配保存为新预设")))
            {
                const auto id = NewID(presets, "preset-");
                presets[id] = {{"name", presetName}, {"search", Split(presetKeys)}, {"items", service.working}};
                service.activePreset = id; service.Save();
            }
            ImGui::BeginDisabled(!presets.contains(service.activePreset));
            if (ImGui::Button(Tr("Rename / update search keys", "更新当前预设名称 / 搜索词")))
            {
                presets[service.activePreset]["name"] = presetName;
                presets[service.activePreset]["search"] = Split(presetKeys); service.Save();
            }
            if (ImGui::Button(Tr("Delete current preset", "删除当前预设"))) ImGui::OpenPopup(Tr("Delete preset?", "删除预设？"));
            ImGui::EndDisabled(); ImGui::EndDisabled();
            if (ImGui::BeginPopupModal(Tr("Delete preset?", "删除预设？"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextWrapped("%s", Tr("Delete the saved preset? The working outfit is retained.", "删除已保存预设？当前工作搭配会保留。"));
                if (ImGui::Button(Tr("Delete", "删除")))
                { presets.erase(service.activePreset); service.activePreset.clear(); service.Save(); ImGui::CloseCurrentPopup(); }
                ImGui::SameLine(); if (ImGui::Button(Tr("Cancel", "取消"))) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }

        void DrawManagement()
        {
            auto& service = Service::Get();
            auto& database = service.store.database;
            ImGui::BeginDisabled(!service.store.writable);
            if (ImGui::CollapsingHeader(Tr("Categories", "分类管理"), ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::InputText(Tr("ID (stable)", "分类 ID（稳定标识）"), &categoryID);
                ImGui::InputText(Tr("Name", "名称"), &categoryName);
                ImGui::InputTextMultiline(Tr("Search keys", "搜索词（每行一个）"), &categorySearch, ImVec2(-1, 65));
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
                        metadata["enabled"] = enabled; service.Save();
                    }
                    ImGui::SameLine();
                    if (ImGui::Selectable((plugin + " (" + std::to_string(count) + ")").c_str(), selectedPlugin == plugin))
                    {
                        selectedPlugin = plugin;
                        const auto metadata = database["pluginCategories"].value(plugin, Json::object());
                        pluginAlias = metadata.value("alias", ""); pluginKeys = Join(metadata.value("search", Json::array()));
                    }
                    ImGui::PopID();
                }
                ImGui::EndChild();
                ImGui::TextUnformatted(selectedPlugin.c_str());
                ImGui::InputText(Tr("Plugin alias", "插件别名"), &pluginAlias);
                ImGui::InputTextMultiline(Tr("Plugin search keys", "插件搜索词（每行一个）"), &pluginKeys, ImVec2(-1, 65));
                if (ImGui::Button(Tr("Save plugin metadata", "保存插件信息")) && !selectedPlugin.empty())
                {
                    auto& metadata = database["pluginCategories"][selectedPlugin]; if (!metadata.is_object()) metadata = Json::object();
                    metadata["alias"] = pluginAlias; metadata["search"] = Split(pluginKeys); service.Save();
                }
            }
            if (ImGui::CollapsingHeader(Tr("Custom part names", "自定义部件名称")))
            {
                for (int i = 15; i < 23; ++i)
                {
                    auto name = PartName(i);
                    if (ImGui::InputText(PartLabel(i), &name)) { database["partNames"][PartIDs[i]] = name; service.Save(); }
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
        std::vector<std::pair<std::string, std::string>> pending;
        { std::lock_guard lock(commandMutex); pending.swap(commands); }
        auto& service = Service::Get();
        for (const auto& [action, argument] : pending)
        {
            if (action == "reload") service.Reload();
            else if (action == "clear") service.Apply(true);
            else if (action == "preset")
            {
                std::string found;
                for (const auto& [id, preset] : service.store.database["presets"].items())
                    if (Normalize(id) == Normalize(argument) || Normalize(ObjectName(id, preset)) == Normalize(argument)) { found = id; break; }
                if (found.empty()) service.status = "Preset not found: " + argument;
                else { service.SelectPreset(found); service.Apply(); }
            }
        }
    }

    void DrawPanel()
    {
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
        if (ImGui::BeginTabBar("Wardrobe pages"))
        {
            if (ImGui::BeginTabItem(Tr("Wardrobe", "衣柜")))
            {
                const float font = ImGui::GetFontSize();
                const float available = ImGui::GetContentRegionAvail().x;
                const float sidebarWidth = std::min(font * 12, available * 0.22f);
                const float workingWidth = std::min(font * 16, available * 0.27f);
                const float browseWidth = std::max(1.0f, available - sidebarWidth - workingWidth - ImGui::GetStyle().ItemSpacing.x * 2);
                ImGui::BeginChild("Views", ImVec2(sidebarWidth, 0), ImGuiChildFlags_Borders); DrawSidebar(); ImGui::EndChild();
                ImGui::SameLine();
                ImGui::BeginChild("Browse", ImVec2(browseWidth, 0)); DrawCatalog(); ImGui::EndChild();
                ImGui::SameLine(); ImGui::BeginChild("Working", ImVec2(0, 0), ImGuiChildFlags_Borders); DrawWorking(); ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(Tr("Presets", "预设"))) { DrawPresets(); ImGui::EndTabItem(); }
            const auto metadataFlags = selectMetadata ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            selectMetadata = false;
            if (ImGui::BeginTabItem(Tr("Metadata", "分类与数据"), nullptr, metadataFlags)) { DrawManagement(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem(Tr("Interface", "界面设置"))) { Overlay::DrawInterfaceSettings(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        DrawItemEditor();
    }
}
