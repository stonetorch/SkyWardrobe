        // 此文件包含在 Panel 的匿名命名空间中，复用现有翻译和条目编辑器。
        std::string OriginalSlotName(int slot)
        {
            const char* names[] = {
                Tr("Head", "头部"), Tr("Hair", "头发"), Tr("Body", "身体"), Tr("Hands", "手部"),
                Tr("Forearms", "前臂"), Tr("Amulet", "项链"), Tr("Ring", "戒指"), Tr("Feet", "脚部"),
                Tr("Calves", "小腿"), Tr("Shield", "盾牌"), Tr("Tail", "尾部"), Tr("Long hair", "长发"), Tr("Circlet", "头饰")};
            return std::to_string(slot + 30) + " " + (slot < 13 ? names[slot] : Tr("Custom slot", "扩展位置"));
        }

        void LayerName(const std::string& name)
        {
            const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
            ImGui::Selectable((name + "##name").c_str(), false, 0, ImVec2(width, 0));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", name.c_str());
        }

        void DrawWorking()
        {
            auto& service = Service::Get();
            auto& presets = service.store.database["presets"];
            auto settings = service.LayersFor(service.currentActor);
            const auto rows = service.LayerPreview();
            const auto backupItems = settings.backup;
            std::string title = Tr("Unsaved outfit", "未命名搭配");
            if (presets.contains(service.activePreset)) title = ObjectName(service.activePreset, presets[service.activePreset]);
            if (service.Dirty()) title += " *";
            LayerName(title);
            bool preview = service.livePreview;
            if (ImGui::Checkbox(Tr("Preview", "预览"), &preview)) service.SetLivePreview(preview);
            ImGui::SameLine(); if (ImGui::Button(Tr("Apply", "应用"))) service.Apply();
            if (ImGui::GetContentRegionAvail().x > ImGui::GetFontSize() * 12) ImGui::SameLine();
            if (ImGui::Button(Tr("More", "更多"))) ImGui::OpenPopup("Layer actions");
            if (ImGui::BeginPopup("Layer actions"))
            {
                if (ImGui::MenuItem(Tr("Clear appearance", "清除外观覆盖"))) service.Apply(true);
                const bool bound = presets.contains(service.activePreset);
                const auto binding = bound ? presets[service.activePreset].value("group", "") : "";
                if (ImGui::MenuItem(Tr("Overwrite preset (working layer)", "覆盖当前预设（仅工作层）"), nullptr, false,
                    bound && service.store.writable && SplitGroupID(binding).first == "groups")) service.OverwritePreset();
                if (ImGui::MenuItem(Tr("Restore preset", "恢复预设"), nullptr, false, bound)) service.SelectPreset(service.activePreset);
                if (ImGui::MenuItem(Tr("Undo overwrite", "撤销上次覆盖"))) service.UndoOverwrite();
                if (ImGui::Button(Tr("Final appearance", "最终外观与来源"))) ImGui::OpenPopup("Layer result");
                if (ImGui::BeginPopup("Layer result"))
                {
                    ImGui::BeginChild("Result list", ImVec2(ImGui::GetFontSize() * 24, ImGui::GetFontSize() * 16));
                    for (const auto& row : rows)
                    {
                        const char* source = row.source == LayerSource::Working ? Tr("Working", "工作") :
                            row.source == LayerSource::Backup ? Tr("Backup", "后备") : Tr("Original", "原版");
                        const char* state = row.disposition == LayerDisposition::Visible ? Tr("Visible", "显示") :
                            row.disposition == LayerDisposition::Covered ? Tr("Covered", "被覆盖") :
                            row.disposition == LayerDisposition::Hidden ? Tr("Hidden", "已隐藏") :
                            row.disposition == LayerDisposition::Temporary ? Tr("Temporary", "临时实例") : Tr("Disabled", "层已关闭");
                        ImGui::TextWrapped("[%s / %s] %s", source, state, row.item.name.c_str());
                        if (!row.coveredBy.empty()) ImGui::TextWrapped("  %s", row.coveredBy.c_str());
                    }
                    ImGui::TextWrapped("%s", Tr("Calculated preview; clear appearance exits all layers. Automatic mode may display its saved result.",
                        "此处为合成预览；清除外观会退出全部覆盖。自动模式可能显示已保存的自动结果。"));
                    ImGui::EndChild(); ImGui::EndPopup();
                }
                ImGui::EndPopup();
            }
            const char* modes[] = {Tr("Off", "关闭"), Tr("Shoes only", "仅鞋子装备"), Tr("Equip all", "全部装备")};
            auto mode = static_cast<int>(service.EquipmentMode());
            ImGui::BeginDisabled(!service.store.writable);
            ImGui::SetNextItemWidth(std::max(50.0f, ImGui::GetContentRegionAvail().x));
            if (ImGui::Combo("##actual equipment", &mode, modes, 3)) service.SetEquipmentMode(static_cast<ActualEquipmentMode>(mode));
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr("Actual equipment: working and backup items may be actually equipped. Original passthrough items are not re-equipped. Changes armor effects; shoes use slot 37.",
                "实际装备：工作与后备项按模式实际穿戴，原版透出项不重新装备。会改变护甲和装备效果；鞋子按槽位37判断。"));
            if (service.ActorProfile().value("automatic", false))
            {
                if (ImGui::SmallButton(Tr("Resume automatic", "恢复自动"))) service.ResumeAutomatic();
                ImGui::SameLine(); ImGui::TextDisabled("%s", service.ActorProfile().value("manual", false) ? Tr("Paused", "手动暂缓") : Tr("Active", "已启用"));
            }

            static std::array<bool, 3> expanded{true, true, true};
            static bool compact = false;
            static int active = 0;
            const float rowHeight = ImGui::GetFrameHeightWithSpacing();
            const float available = ImGui::GetContentRegionAvail().y;
            // 阈值留出一行滞回，条目数变化不会触发布局模式切换。
            if (!compact && available < rowHeight * 12) compact = true;
            else if (compact && available > rowHeight * 13) compact = false;
            auto open = expanded;
            if (compact) for (int i = 0; i < 3; ++i) open[i] = i == active;
            int originals = 0, visible = 0, hidden = 0;
            for (const auto& row : rows) if (row.source == LayerSource::Original)
            {
                ++originals;
                visible += row.disposition == LayerDisposition::Visible;
                hidden += row.disposition == LayerDisposition::Hidden;
            }
            const int openCount = static_cast<int>(std::count(open.begin(), open.end(), true));
            const auto heights = AllocateLayerHeights(std::max(0.0f, available - rowHeight * 3 - openCount * ImGui::GetStyle().ItemSpacing.y), rowHeight,
                {static_cast<int>(service.working.size()), static_cast<int>(backupItems.size()) + 1, originals + 2}, open);
            const std::array<std::string, 3> headings{
                std::string(Tr("Working outfit", "工作搭配")) + " (" + std::to_string(service.working.size()) + ")",
                std::string(Tr("Backup outfit", "后备搭配")) + " (" + std::to_string(backupItems.size()) + ")" + (settings.backupEnabled ? "" : Tr(" Off", " 已关闭")),
                std::string(Tr("Original clothing", "原版服装显示")) + " " + std::to_string(visible) + "/" + std::to_string(originals) + " " + Tr("hidden:", "隐藏:") + std::to_string(hidden)};
            for (int layer = 0; layer < 3; ++layer)
            {
                ImGui::PushID(layer);
                const auto label = std::string(open[layer] ? "v " : "> ") + headings[layer];
                if (ImGui::Selectable(label.c_str(), open[layer], 0, ImVec2(0, ImGui::GetFrameHeight())))
                {
                    active = layer;
                    if (!compact) expanded[layer] = !expanded[layer];
                }
                if (open[layer] && heights[layer] > 0)
                {
                    ImGui::BeginChild("Layer items", ImVec2(0, heights[layer]));
                    ImGui::BeginDisabled(!service.store.writable);
                    if (layer == 1)
                    {
                        bool enabled = settings.backupEnabled;
                        if (ImGui::Checkbox(Tr("Enabled", "启用后备搭配"), &enabled)) service.SetLayerOptions(enabled, settings.originalEnabled, settings.hiddenSlots);
                    }
                    if (layer < 2)
                    {
                        const auto items = layer == 0 ? service.working : backupItems;
                        for (const auto& key : items)
                        {
                            ImGui::PushID(key.c_str());
                            if (ImGui::SmallButton("x")) { if (layer == 0) service.Remove(key); else service.RemoveBackup(key); }
                            ImGui::SameLine();
                            if (ImGui::SmallButton(layer == 0 ? "v" : "^")) service.MoveLayer(key, layer == 0);
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", layer == 0 ? Tr("Move to backup outfit", "移到后备搭配") : Tr("Move to working outfit", "移到工作搭配"));
                            ImGui::SameLine();
                            const auto* entry = service.Find(key);
                            LayerName(entry ? service.Name(*entry) : Tr("Missing: ", "缺失：") + key);
                            if (entry && ImGui::IsItemClicked(ImGuiMouseButton_Right)) StartEdit(*entry);
                            for (const auto& row : rows)
                                if (row.item.key == key && row.source == (layer == 0 ? LayerSource::Working : LayerSource::Backup) &&
                                    row.disposition != LayerDisposition::Visible && ImGui::IsItemHovered())
                                    ImGui::SetTooltip("%s\n%s", row.item.name.c_str(), row.coveredBy.empty() ? Tr("Layer disabled", "层已关闭") : row.coveredBy.c_str());
                            ImGui::PopID();
                        }
                    }
                    else
                    {
                        bool enabled = settings.originalEnabled;
                        if (ImGui::Checkbox(Tr("Allow original clothing", "允许原版服装透出"), &enabled)) service.SetLayerOptions(settings.backupEnabled, enabled, settings.hiddenSlots);
                        if (ImGui::Button(Tr("Configure all slots", "配置全部位置"))) ImGui::OpenPopup("Original slots");
                        if (ImGui::BeginPopup("Original slots"))
                        {
                            ImGui::TextWrapped("%s", Tr("A matching slot hides the entire original armor, including its other slots.", "命中任一隐藏位置会隐藏整件原版装备，包括其关联位置。"));
                            ImGui::BeginChild("Slots", ImVec2(ImGui::GetFontSize() * 22, ImGui::GetFontSize() * 14));
                            for (int slot = 0; slot < 32; ++slot)
                            {
                                const auto bit = std::uint32_t(1) << slot;
                                bool hide = (settings.hiddenSlots & bit) != 0;
                                std::string text = OriginalSlotName(slot);
                                for (const auto& row : rows) if (row.source == LayerSource::Original && (row.item.slots & bit)) text += " · " + row.item.name;
                                if (ImGui::Checkbox(text.c_str(), &hide))
                                { settings.hiddenSlots = hide ? settings.hiddenSlots | bit : settings.hiddenSlots & ~bit; service.SetLayerOptions(settings.backupEnabled, settings.originalEnabled, settings.hiddenSlots); }
                            }
                            ImGui::EndChild(); ImGui::EndPopup();
                        }
                        for (const auto& row : rows) if (row.source == LayerSource::Original)
                        {
                            ImGui::PushID(row.item.key.c_str());
                            bool show = (row.item.slots & settings.hiddenSlots) == 0;
                            if (ImGui::Checkbox("##visible", &show))
                            {
                                settings.hiddenSlots = show ? settings.hiddenSlots & ~row.item.slots : settings.hiddenSlots | row.item.slots;
                                service.SetLayerOptions(settings.backupEnabled, settings.originalEnabled, settings.hiddenSlots);
                            }
                            const char* badge = row.disposition == LayerDisposition::Visible ? Tr("[Visible] ", "[透出] ") :
                                row.disposition == LayerDisposition::Hidden ? Tr("[Hidden] ", "[隐藏] ") :
                                row.disposition == LayerDisposition::Covered ? Tr("[Covered] ", "[覆盖] ") :
                                row.disposition == LayerDisposition::Temporary ? Tr("[Temp] ", "[临时] ") : Tr("[Off] ", "[关闭] ");
                            ImGui::SameLine(); LayerName(std::string(badge) + row.item.name);
                            if (ImGui::IsItemHovered())
                            {
                                std::string slots;
                                for (int slot = 0; slot < 32; ++slot) if (row.item.slots & (std::uint32_t(1) << slot)) slots += std::to_string(slot + 30) + " ";
                                const char* state = row.disposition == LayerDisposition::Visible ? Tr("Visible", "正在透出") :
                                    row.disposition == LayerDisposition::Hidden ? Tr("Hidden", "已隐藏整件装备") :
                                    row.disposition == LayerDisposition::Temporary ? Tr("Wardrobe temporary instance", "衣柜临时实例，不参与原版透出") :
                                    row.disposition == LayerDisposition::Disabled ? Tr("Original layer off", "原版透出已关闭") : Tr("Covered", "被覆盖");
                                ImGui::SetTooltip("%s\n%s\n%s\n%s", row.item.name.c_str(), slots.c_str(), state, row.coveredBy.c_str());
                            }
                            ImGui::PopID();
                        }
                    }
                    ImGui::EndDisabled(); ImGui::EndChild();
                }
                ImGui::PopID();
            }
        }
