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
            const float width = std::max(1.0f, FlickUi::GetContentRegionAvail().x);
            FlickUi::Selectable((name + "##name").c_str(), false, 0, ImVec2(width, 0));
            if (FlickUi::IsItemHovered()) FlickUi::SetTooltip("%s", name.c_str());
        }

        void DrawWorking()
        {
            DrawActorSelector(true);
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
            FlickUi::BeginDisabled(SafeMode::Get().Active());
            if (FlickUi::Checkbox(Tr("Preview", "预览"), &preview)) service.SetLivePreview(preview);
            MarkManualAnchor(ManualTarget::Preview);
            FlickUi::SameLine(); if (FlickUi::Button(Tr("Apply", "应用"))) service.Apply();
            FlickUi::EndDisabled();
            if (FlickUi::GetContentRegionAvail().x > FlickUi::GetFontSize() * 12) FlickUi::SameLine();
            if (FlickUi::Button(Tr("More", "更多"))) FlickUi::OpenPopup("Layer actions");
            if (FlickUi::BeginPopup("Layer actions"))
            {
                if (FlickUi::MenuItem(Tr("Clear appearance", "清除外观覆盖"))) service.Apply(true);
                const bool bound = presets.contains(service.activePreset);
                const auto binding = bound ? presets[service.activePreset].value("group", "") : "";
                if (FlickUi::MenuItem(Tr("Overwrite preset (working layer)", "覆盖当前预设（仅工作层）"), nullptr, false,
                    bound && service.store.writable && SplitGroupID(binding).first == "groups")) service.OverwritePreset();
                if (FlickUi::MenuItem(Tr("Restore preset", "恢复预设"), nullptr, false, bound)) service.SelectPreset(service.activePreset);
                if (FlickUi::MenuItem(Tr("Undo overwrite", "撤销上次覆盖"))) service.UndoOverwrite();
                if (FlickUi::Button(Tr("Final appearance", "最终外观与来源"))) FlickUi::OpenPopup("Layer result");
                if (FlickUi::BeginPopup("Layer result"))
                {
                    FlickUi::BeginChild("Result list", ImVec2(FlickUi::GetFontSize() * 24, FlickUi::GetFontSize() * 16));
                    for (const auto& row : rows)
                    {
                        const char* source = row.source == LayerSource::Working ? Tr("Working", "工作") :
                            row.source == LayerSource::Backup ? Tr("Backup", "后备") : Tr("Original", "原版");
                        const char* state = row.disposition == LayerDisposition::Visible ? Tr("Visible", "显示") :
                            row.disposition == LayerDisposition::Covered ? Tr("Covered", "被覆盖") :
                            row.disposition == LayerDisposition::Hidden ? Tr("Hidden", "已隐藏") :
                            row.disposition == LayerDisposition::Temporary ? Tr("Temporary", "临时实例") : Tr("Disabled", "层已关闭");
                        FlickUi::TextWrapped("[%s / %s] %s", source, state, row.item.name.c_str());
                        if (!row.coveredBy.empty()) FlickUi::TextWrapped("  %s", row.coveredBy.c_str());
                    }
                    FlickUi::TextWrapped("%s", Tr("Calculated preview; clear appearance exits all layers. Automatic mode may display its saved result.",
                        "此处为合成预览；清除外观会退出全部覆盖。自动模式可能显示已保存的自动结果。"));
                    FlickUi::EndChild(); FlickUi::EndPopup();
                }
                FlickUi::EndPopup();
            }
            if (FlickUi::Button(Tr("New outfit group from working outfit", "从工作搭配新建服装组"))) FlickUi::OpenPopup("Save working group");
            MarkManualAnchor(ManualTarget::CreateGroup);
            if (FlickUi::BeginPopup("Save working group"))
            {
                static std::string name;
                FlickUi::InputText(Tr("Outfit group name", "服装组名称"), &name);
                FlickUi::BeginDisabled(name.empty() || !service.store.writable);
                if (FlickUi::Button(Tr("Create", "创建")))
                {
                    auto& groups = service.store.database["groups"];
                    const auto id = NewID(groups, "group-");
                    groups[id] = {{"name", name}, {"items", service.working}};
                    // 保存成员快照，不改变当前预设、工作搭配或自动状态。
                    service.Save(); name.clear(); FlickUi::CloseCurrentPopup();
                }
                FlickUi::EndDisabled(); FlickUi::EndPopup();
            }
            const char* modes[] = {Tr("Off", "关闭"), Tr("Shoes only", "仅鞋子装备"), Tr("Equip all", "全部装备")};
            auto mode = static_cast<int>(service.EquipmentMode());
            FlickUi::BeginDisabled(!service.store.writable);
            FlickUi::SetNextItemWidth(std::max(50.0f, FlickUi::GetContentRegionAvail().x));
            if (FlickUi::Combo("##actual equipment", &mode, modes, 3)) service.SetEquipmentMode(static_cast<ActualEquipmentMode>(mode));
            MarkManualAnchor(ManualTarget::Equipment);
            FlickUi::EndDisabled();
            if (FlickUi::IsItemHovered()) FlickUi::SetTooltip("%s", Tr("Actual equipment: working and backup items may be actually equipped. Original passthrough items are not re-equipped. Changes armor effects; shoes use slot 37.",
                "实际装备：工作与后备项按模式实际穿戴，原版透出项不重新装备。会改变护甲和装备效果；鞋子按槽位37判断。"));
            if (service.ActorProfile().value("automatic", false))
            {
                if (FlickUi::SmallButton(Tr("Resume automatic", "恢复自动"))) service.ResumeAutomatic();
                FlickUi::SameLine(); FlickUi::TextDisabled("%s", service.ActorProfile().value("manual", false) ? Tr("Paused", "手动暂缓") : Tr("Active", "已启用"));
            }

            static std::array<bool, 3> expanded{true, true, true};
            static bool compact = false;
            static int active = 0;
            const float rowHeight = FlickUi::GetFrameHeightWithSpacing();
            const float available = FlickUi::GetContentRegionAvail().y;
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
            const auto heights = AllocateLayerHeights(std::max(0.0f, available - rowHeight * 3 - openCount * FlickUi::GetStyle().ItemSpacing.y), rowHeight,
                {static_cast<int>(service.working.size()), static_cast<int>(backupItems.size()) + 1, originals + 2}, open);
            const std::array<std::string, 3> headings{
                std::string(Tr("Working outfit", "工作搭配")) + " (" + std::to_string(service.working.size()) + ")",
                std::string(Tr("Backup outfit", "后备搭配")) + " (" + std::to_string(backupItems.size()) + ")" + (settings.backupEnabled ? "" : Tr(" Off", " 已关闭")),
                std::string(Tr("Original clothing", "原版服装显示")) + " " + std::to_string(visible) + "/" + std::to_string(originals) + " " + Tr("hidden:", "隐藏:") + std::to_string(hidden)};
            for (int layer = 0; layer < 3; ++layer)
            {
                FlickUi::PushID(layer);
                // 与左栏共用原生折叠箭头；展开状态仍由高度自适应布局管理。
                FlickUi::SetNextItemOpen(open[layer], ImGuiCond_Always);
                const bool headerOpen = FlickUi::CollapsingHeader((headings[layer] + "###layer header").c_str());
                if (layer == 1) MarkManualAnchor(ManualTarget::Backup);
                if (layer == 2) MarkManualAnchor(ManualTarget::Original);
                if (headerOpen != open[layer])
                {
                    active = layer;
                    if (!compact) expanded[layer] = !expanded[layer];
                }
                if (open[layer] && heights[layer] > 0)
                {
                    FlickUi::BeginChild("Layer items", ImVec2(0, heights[layer]));
                    FlickUi::BeginDisabled(!service.store.writable);
                    if (layer == 1)
                    {
                        bool enabled = settings.backupEnabled;
                        if (FlickUi::Checkbox(Tr("Enabled", "启用后备搭配"), &enabled)) service.SetLayerOptions(enabled, settings.originalEnabled, settings.hiddenSlots);
                    }
                    if (layer < 2)
                    {
                        const auto items = layer == 0 ? service.working : backupItems;
                        for (const auto& key : items)
                        {
                            FlickUi::PushID(key.c_str());
                            if (FlickUi::SmallButton("x")) { if (layer == 0) service.Remove(key); else service.RemoveBackup(key); }
                            FlickUi::SameLine();
                            if (FlickUi::SmallButton(layer == 0 ? "v" : "^")) service.MoveLayer(key, layer == 0);
                            if (FlickUi::IsItemHovered()) FlickUi::SetTooltip("%s", layer == 0 ? Tr("Move to backup outfit", "移到后备搭配") : Tr("Move to working outfit", "移到工作搭配"));
                            FlickUi::SameLine();
                            const auto* entry = service.Find(key);
                            LayerName(entry ? service.Name(*entry) : Tr("Missing: ", "缺失：") + key);
                            if (entry && FlickUi::IsItemClicked(ImGuiMouseButton_Right)) StartEdit(*entry);
                            for (const auto& row : rows)
                                if (row.item.key == key && row.source == (layer == 0 ? LayerSource::Working : LayerSource::Backup) &&
                                    row.disposition != LayerDisposition::Visible && FlickUi::IsItemHovered())
                                    FlickUi::SetTooltip("%s\n%s", row.item.name.c_str(), row.coveredBy.empty() ? Tr("Layer disabled", "层已关闭") : row.coveredBy.c_str());
                            FlickUi::PopID();
                        }
                    }
                    else
                    {
                        bool enabled = settings.originalEnabled;
                        if (FlickUi::Checkbox(Tr("Allow original clothing", "允许原版服装透出"), &enabled)) service.SetLayerOptions(settings.backupEnabled, enabled, settings.hiddenSlots);
                        if (FlickUi::Button(Tr("Configure all slots", "配置全部位置"))) FlickUi::OpenPopup("Original slots");
                        if (FlickUi::BeginPopup("Original slots"))
                        {
                            FlickUi::TextWrapped("%s", Tr("A matching slot hides the entire original armor, including its other slots.", "命中任一隐藏位置会隐藏整件原版装备，包括其关联位置。"));
                            FlickUi::BeginChild("Slots", ImVec2(FlickUi::GetFontSize() * 22, FlickUi::GetFontSize() * 14));
                            for (int slot = 0; slot < 32; ++slot)
                            {
                                const auto bit = std::uint32_t(1) << slot;
                                bool hide = (settings.hiddenSlots & bit) != 0;
                                std::string text = OriginalSlotName(slot);
                                for (const auto& row : rows) if (row.source == LayerSource::Original && (row.item.slots & bit)) text += " · " + row.item.name;
                                if (FlickUi::Checkbox(text.c_str(), &hide))
                                { settings.hiddenSlots = hide ? settings.hiddenSlots | bit : settings.hiddenSlots & ~bit; service.SetLayerOptions(settings.backupEnabled, settings.originalEnabled, settings.hiddenSlots); }
                            }
                            FlickUi::EndChild(); FlickUi::EndPopup();
                        }
                        for (const auto& row : rows) if (row.source == LayerSource::Original)
                        {
                            FlickUi::PushID(row.item.key.c_str());
                            bool show = (row.item.slots & settings.hiddenSlots) == 0;
                            if (FlickUi::Checkbox("##visible", &show))
                            {
                                settings.hiddenSlots = show ? settings.hiddenSlots & ~row.item.slots : settings.hiddenSlots | row.item.slots;
                                service.SetLayerOptions(settings.backupEnabled, settings.originalEnabled, settings.hiddenSlots);
                            }
                            const char* badge = row.disposition == LayerDisposition::Visible ? Tr("[Visible] ", "[透出] ") :
                                row.disposition == LayerDisposition::Hidden ? Tr("[Hidden] ", "[隐藏] ") :
                                row.disposition == LayerDisposition::Covered ? Tr("[Covered] ", "[覆盖] ") :
                                row.disposition == LayerDisposition::Temporary ? Tr("[Temp] ", "[临时] ") : Tr("[Off] ", "[关闭] ");
                            FlickUi::SameLine(); LayerName(std::string(badge) + row.item.name);
                            if (FlickUi::IsItemHovered())
                            {
                                std::string slots;
                                for (int slot = 0; slot < 32; ++slot) if (row.item.slots & (std::uint32_t(1) << slot)) slots += std::to_string(slot + 30) + " ";
                                const char* state = row.disposition == LayerDisposition::Visible ? Tr("Visible", "正在透出") :
                                    row.disposition == LayerDisposition::Hidden ? Tr("Hidden", "已隐藏整件装备") :
                                    row.disposition == LayerDisposition::Temporary ? Tr("Wardrobe temporary instance", "衣柜临时实例，不参与原版透出") :
                                    row.disposition == LayerDisposition::Disabled ? Tr("Original layer off", "原版透出已关闭") : Tr("Covered", "被覆盖");
                                FlickUi::SetTooltip("%s\n%s\n%s\n%s", row.item.name.c_str(), slots.c_str(), state, row.coveredBy.c_str());
                            }
                            FlickUi::PopID();
                        }
                    }
                    FlickUi::EndDisabled(); FlickUi::EndChild();
                }
                FlickUi::PopID();
            }
        }
