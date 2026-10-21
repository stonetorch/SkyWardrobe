// 包含在面板匿名命名空间内；手册只记录本帧可见控件，不保存过期坐标。
enum class ManualTarget { Wardrobe, Actors, Presets, Metadata, Interface, Working, CreateGroup, Backup, Original, Equipment, Search, Quick, Preview, Count };
struct ManualAnchor
{
    ImVec2 min{}, max{};
    bool visible{};
};
std::array<ManualAnchor, static_cast<std::size_t>(ManualTarget::Count)> manualAnchors;
bool manualOpen{};
int manualPage = -1;

void MarkManualAnchor(ManualTarget target)
{
    if (!FlickUi::IsItemVisible()) return;
    auto& anchor = manualAnchors[static_cast<std::size_t>(target)];
    // 与当前裁剪区取交集，让部分滚出区域的按钮仍指向屏幕内的部分。
    const auto clipMin = FlickUi::GetWindowDrawList()->GetClipRectMin();
    const auto clipMax = FlickUi::GetWindowDrawList()->GetClipRectMax();
    const auto min = FlickUi::GetItemRectMin(), max = FlickUi::GetItemRectMax();
    anchor = {ImVec2(std::max(min.x, clipMin.x), std::max(min.y, clipMin.y)),
        ImVec2(std::min(max.x, clipMax.x), std::min(max.y, clipMax.y)), true};
}

bool BeginManualTab(const char* label, ManualTarget target, ImGuiTabItemFlags flags = 0)
{
    const bool active = FlickUi::BeginTabItem(label, nullptr, flags);
    MarkManualAnchor(target);
    return active;
}

struct ManualLesson
{
    const char* title;
    const char* text;
    const char* hint;
    ManualTarget target;
    ManualTarget tab;
};

const auto manualLessons = std::to_array<ManualLesson>({
    {Tr("Basic workflow", "整体操作逻辑"),
        Tr("Choose the current actor, then click clothing cards to add them to the working outfit. Click a selected card again, or its x in the working list, to remove it. Preview updates the appearance as you edit; with Preview off, press Apply when ready. Double-clicking a card also applies the working outfit.",
            "先确认当前操作角色，再单击服装卡片，将它加入工作搭配。再次单击已选卡片，或点击工作列表中的 x，即可移除。开启“预览”会随编辑更新外观；关闭时，搭配好后点击“应用”。双击卡片也会应用当前工作搭配。"),
        Tr("Open Wardrobe and find Working outfit on the left.", "进入“衣柜”，左侧“工作搭配”可只看当前已选服装。"), ManualTarget::Working, ManualTarget::Wardrobe},
    {Tr("Outfit groups", "服装组：保存与一键使用"),
        Tr("An outfit group is a reusable collection of clothing. Build a working outfit, then create a group from it and name it. Select a group on the left and press Use outfit group to generate, replace, and apply the working outfit; conflicting slots are randomly resolved. Later edits to the working outfit do not change the saved group.",
            "服装组是一份可重复使用的服装集合。先搭好工作搭配，点击“从工作搭配新建服装组”并命名；以后在左侧选中它，点击“使用服装组”，即可生成、替换工作搭配并应用，同位置冲突项会随机取舍。继续修改工作搭配不会改写已保存的服装组。"),
        Tr("Create a group in the right panel; select a saved group to reveal Use outfit group.", "右侧按钮用于保存；在左侧选中服装组后，中间上方会出现“使用服装组”。"), ManualTarget::CreateGroup, ManualTarget::Wardrobe},
    {Tr("Backup outfit", "后备搭配：补足工作搭配"),
        Tr("Backup clothing fills slots left free by the working outfit; the working layer takes priority on conflicts. Right-click a card to add it to backup, or select a group and use Set backup outfit. In the right lists, v moves working clothing to backup, ^ moves it back, and x removes it from its layer. Enable backup for it to participate.",
            "后备搭配用于补足工作搭配未占用的位置，冲突时工作层优先。右键服装卡片可“添加到后备搭配”，也可选中服装组后“设为后备搭配”。右侧列表中，v 将工作项移到后备，^ 将后备项移回工作，x 从所在层移除；“启用后备搭配”控制这一层是否参与显示。"),
        Tr("Expand Backup outfit in the right panel to manage its items.", "展开右侧“后备搭配”管理条目；空间较小时，点击层标题切换。"), ManualTarget::Backup, ManualTarget::Wardrobe},
    {Tr("Original clothing and layers", "原版服装与外观层"),
        Tr("The final appearance combines working, backup, and original clothing, in that priority order. Allow original clothing lets uncovered original equipment show through. Configure all slots controls which original slots stay hidden; hiding any slot hides the whole armor item. More > Final appearance explains the source and visibility of each item.",
            "最终外观按工作、后备、原版的优先级合成。“允许原版服装透出”让未被覆盖的原本装备继续显示；“配置全部位置”可隐藏指定原版位置，命中任一位置会隐藏整件装备。“更多 → 最终外观与来源”可以查看各项为何显示或被覆盖。"),
        Tr("Expand Original clothing on the right.", "展开右侧“原版服装显示”查看透出与隐藏设置。"), ManualTarget::Original, ManualTarget::Wardrobe},
    {Tr("Appearance and actual equipment", "外观覆盖与实际装备"),
        Tr("Applying an appearance and actually equipping armor are separate operations. The equipment selector offers Off, Shoes only, and Equip all for working and backup items. Actual equipment changes armor and equipment effects; use Shoes only when shoe equipment is needed. More > Clear appearance exits the appearance layers.",
            "应用外观与实际穿戴装备是两个概念。右侧装备模式可选“关闭”“仅鞋子装备”“全部装备”，作用于工作与后备项。实际装备会改变护甲和装备效果；需要鞋子的装备效果时可选“仅鞋子装备”。“更多 → 清除外观覆盖”可退出外观层。"),
        Tr("The equipment selector is below Create group in the right panel.", "装备模式下拉框位于右侧新建服装组按钮下方。"), ManualTarget::Equipment, ManualTarget::Wardrobe},
    {Tr("Search and browsing", "搜索、收藏与部件浏览"),
        Tr("Use the central search to find names, pinyin, categories, plugins, and notes; the left search finds navigation groups. Right-click a card to favorite it or edit its metadata. Part browsing organizes clothing by its assigned part; those labels are browsing metadata, while appearance conflicts use equipment slots.",
            "中间搜索框支持名称、拼音、分类、插件和备注，左侧搜索框用于找导航中的服装组。右键卡片可收藏，或编辑别名、备注、分类和部件。按部件浏览会依照部件标记整理服装；部件是浏览分类，外观覆盖冲突则由装备位置决定。"),
        Tr("Use the search above the clothing cards.", "在服装卡片上方输入关键词；右键卡片可以编辑搜索信息。"), ManualTarget::Search, ManualTarget::Wardrobe},
    {Tr("Quick actions", "快捷操作：批量整理"),
        Tr("Quick actions lets you add clothing to a chosen group, remove members from the current group, or mark parts. Choose the action and target first, then click cards or use the batch selection controls. While this mode is active, card clicks perform that action. Exit quick mode before returning to normal working-outfit selection.",
                "“快捷操作”可以将服装加入指定组、从当前组移除，或标记部件分类。先选操作和目标，再点击卡片或使用批量选取控件；此时点击卡片执行的是整理操作。完成后点击“结束快捷操作”，再继续普通的工作搭配选取。"),
        Tr("Quick actions is above the central search box.", "入口在中间搜索框上方的“快捷操作”。"), ManualTarget::Quick, ManualTarget::Wardrobe},
    {Tr("Group references and navigation", "服装组引用与目录整理"),
        Tr("References link related groups for navigation; referenced clothing is not merged into the current group's outfit generation. Edit group metadata to manage references; Create subgroup creates a group linked in both directions. Drag navigation entries to reorder or organize them into directories, or right-click Move to for an exact destination. Deleting a directory keeps its groups.",
            "引用用于关联和跳转服装组，被引用组的服装不会合并到当前组的搭配生成中。选中组后进入“编辑服装组信息”管理引用，“创建子服装组”会创建与当前组互相引用的新组。左侧条目可拖动排序、放入目录，也可右键“移动到…”指定位置；删除目录会保留服装组。"),
        Tr("Open Wardrobe, select a group on the left, then edit its metadata.", "进入“衣柜”，在左侧选择组，再点击“编辑服装组信息”。"), ManualTarget::Wardrobe, ManualTarget::Wardrobe},
    {Tr("Managing actors", "角色管理：为不同角色搭配"),
        Tr("Add a watched actor or select someone from the nearby list. Switch Current actor before editing: each actor has its own working outfit, backup, and outfit rules. An unloaded actor must become available in the game before its appearance can be edited normally.",
            "可添加当前注视的人物，也可从附近人物列表中选择。编辑前切换“当前操作角色”：各角色分别保留自己的工作搭配、后备搭配和换装规则。未加载的角色需要先在游戏中可用，才能正常编辑其外观。"),
        Tr("Open Actors to add or switch managed characters.", "进入“角色管理”添加或切换需要管理的人物。"), ManualTarget::Actors, ManualTarget::Actors},
    {Tr("Automatic outfits", "自动换装：状态与规则"),
        Tr("In Presets, configure the current actor's default, combat, dungeon, city, and town rules, then enable automatic outfits. A rule can keep the working outfit or choose a group, randomly or in sequence. Manual changes temporarily pause automatic execution; Resume / retry automatic returns control to the rules. Review the displayed state and result when checking a rule.",
            "在“预设”中配置当前角色的默认、战斗、地下城、城市和城镇规则，再启用自动换装。规则可以继续采用工作搭配，或单选、随机、按顺序使用服装组。手动操作会暂缓自动执行；点击“恢复 / 重试自动换装”交还给规则，并可查看当前状态与执行结果。"),
        Tr("Open Presets to configure rules and inspect automatic status.", "进入“预设”查看规则及自动执行状态。"), ManualTarget::Presets, ManualTarget::Presets},
    {Tr("Multiple presets", "多预设：可选的进阶配置"),
        Tr("The default setup already provides per-actor outfit rules. Enable multiple presets in Interface only when you need several named configurations for an actor. Each preset can have its own group binding and rules; switching presets selects that configuration. Outfit groups remain the reusable clothing collections.",
            "默认配置已能为每个角色设置换装规则。如果同一角色需要多套有名称的配置，可在“界面设置”启用多预设，再到“预设”中新建。各预设可分别绑定服装组并保存规则，切换预设就是选用那份配置；服装组仍负责保存可复用的服装集合。"),
        Tr("Enable multiple presets in Interface, then configure them in Presets.", "先到“界面设置”启用多预设，再到“预设”配置。"), ManualTarget::Interface, ManualTarget::Interface},
    {Tr("Categories and data", "分类、插件注册与别名包"),
        Tr("Metadata manages custom categories and registered plugin categories. Register a plugin to expose its clothing in navigation; aliases and search keys make it easier to find. Card metadata can override supplied alias-pack information; Reset user metadata restores the alias-pack or original values for that item.",
            "“分类与数据”用于管理自定义分类和已注册插件分类。注册插件后可从导航浏览其服装，别名和搜索词方便查找。卡片中的用户信息可覆盖别名包提供的数据；在服装信息中“恢复别名包 / 原始数据”，可移除该服装的用户覆盖。"),
        Tr("Open Metadata to manage categories and plugin registration.", "进入“分类与数据”管理分类和插件注册。"), ManualTarget::Metadata, ManualTarget::Metadata},
    {Tr("Interface and quick access", "界面与快捷入口"),
        Tr("Interface contains catalog layout, quick-access, and overlay settings. Adjust clothing columns and part browsing for your screen; configure quick-access entries for frequently used groups or presets. This manual stays open as you change tabs. Move it aside to see a highlighted target; example controls here never change your outfit.",
            "“界面设置”集中提供目录布局、快捷入口和面板设置。可按屏幕调整服装列数和部件浏览，并为常用服装组或预设配置快捷入口。切换标签页时手册会保持打开；拖开手册即可查看箭头标出的目标，手册内的示例控件不会修改搭配。"),
        Tr("Open Interface to adjust layout and quick access.", "进入“界面设置”调整布局和快捷入口。"), ManualTarget::Interface, ManualTarget::Interface}
});

void DrawManualExamples(int page)
{
    if (page > 4 && page != 6) return;
    FlickUi::SeparatorText(Tr("Examples (no effect)", "操作示例（无实际效果）"));
    // 直接复用 ImGui 的原生控件和当前主题，点击结果仅丢弃，不调用业务接口。
    FlickUi::PushID("Manual examples");
    if (page == 0)
    {
        bool preview = true;
        FlickUi::Checkbox(Tr("Preview", "预览"), &preview);
        FlickUi::SameLine(); FlickUi::Button(Tr("Apply", "应用"));
        FlickUi::SmallButton("x"); FlickUi::SameLine();
        FlickUi::Selectable(Tr("Example clothing (selected)", "示例服装（已选）"), true);
    }
    else if (page == 1)
    {
        FlickUi::Button(Tr("New outfit group from working outfit", "从工作搭配新建服装组"));
        FlickUi::Button(Tr("Use outfit group", "使用服装组"));
    }
    else if (page == 2)
    {
        FlickUi::SmallButton("x"); FlickUi::SameLine(); FlickUi::SmallButton("v");
        FlickUi::SameLine(); FlickUi::TextUnformatted(Tr("Working -> backup", "工作 → 后备"));
        FlickUi::SmallButton("x"); FlickUi::SameLine(); FlickUi::SmallButton("^");
        FlickUi::SameLine(); FlickUi::TextUnformatted(Tr("Backup -> working", "后备 → 工作"));
        FlickUi::Button(Tr("Set backup outfit", "设为后备搭配"));
    }
    else if (page == 3)
    {
        bool enabled = true;
        FlickUi::Checkbox(Tr("Allow original clothing", "允许原版服装透出"), &enabled);
        FlickUi::Button(Tr("Configure all slots", "配置全部位置"));
    }
    else if (page == 4) FlickUi::Button(Tr("More", "更多"));
    else FlickUi::Button(Tr("Quick actions", "快捷操作"));
    FlickUi::PopID();
}

void DrawManual()
{
    if (!manualOpen) return;
    const auto* viewport = FlickUi::GetMainViewport();
    const float font = FlickUi::GetFontSize();
    const ImVec2 minimum(std::min(font * 23, viewport->WorkSize.x * 0.8f),
        std::min(font * 18, viewport->WorkSize.y * 0.85f));
    FlickUi::SetNextWindowSizeConstraints(minimum, viewport->WorkSize);
    FlickUi::SetNextWindowSize(ImVec2(std::min(font * 29, viewport->WorkSize.x * 0.8f),
        std::min(font * 31, viewport->WorkSize.y * 0.85f)), ImGuiCond_Appearing);
    FlickUi::SetNextWindowPos(ImVec2(viewport->WorkPos.x + std::max(0.0f, viewport->WorkSize.x - font * 30),
        viewport->WorkPos.y + font * 3), ImGuiCond_Appearing);
    if (!FlickUi::Begin(Tr("Operation manual###SkyWardrobeManual", "操作手册###SkyWardrobeManual"), &manualOpen,
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse))
    {
        FlickUi::End(); return;
    }
    const auto windowMin = FlickUi::GetWindowPos(), size = FlickUi::GetWindowSize();
    const ImVec2 windowMax(windowMin.x + size.x, windowMin.y + size.y);
    const int count = static_cast<int>(manualLessons.size());
    const int displayedPage = manualPage;
    const float footer = FlickUi::GetFrameHeightWithSpacing() * 4;
    if (FlickUi::BeginChild("Manual content", ImVec2(0, -footer)))
    {
        static int lastPage = -2;
        if (lastPage != manualPage) { FlickUi::SetScrollY(0); lastPage = manualPage; }
        if (manualPage < 0)
        {
            FlickUi::SeparatorText(Tr("Contents", "操作手册目录"));
            FlickUi::TextWrapped("%s", Tr("Start with the basics, or select a topic. Arrows point to a visible control or the tab that leads to it.",
                "可以从基本操作开始，也可以直接选择主题。箭头会指向可见控件，或能进入该功能的标签页。"));
            for (int i = 0; i < count; ++i)
            {
                if (i == 0 || i == 5 || i == 8) FlickUi::SeparatorText(i == 0 ? Tr("Basics and concepts", "基本操作与核心概念")
                    : i == 5 ? Tr("Organizing clothing", "服装查找与整理") : Tr("Advanced features", "进阶功能与设置"));
                if (FlickUi::Selectable((std::to_string(i + 1) + ". " + manualLessons[i].title).c_str())) manualPage = i;
            }
        }
        else
        {
            const auto& lesson = manualLessons[manualPage];
            FlickUi::TextDisabled("%d / %d", manualPage + 1, count);
            FlickUi::SeparatorText(lesson.title);
            FlickUi::TextWrapped("%s", lesson.text);
            DrawManualExamples(manualPage);
            FlickUi::SeparatorText(Tr("Find the real control", "实际操作位置"));
            FlickUi::TextWrapped("%s", lesson.hint);
            FlickUi::TextWrapped("%s", Tr("If the control is hidden, follow the tab arrow first, then expand or scroll the indicated area. Drag this window aside if it covers the target.",
                "控件未显示时，先按箭头切换标签，再展开或滚动对应区域。如果手册遮住目标，可拖动本窗口让开。"));
        }
    }
    FlickUi::EndChild();
    FlickUi::Separator();
    const auto centered = [](float width) { FlickUi::SetCursorPosX(FlickUi::GetCursorPosX() + std::max(0.0f, (FlickUi::GetContentRegionAvail().x - width) * 0.5f)); };
    centered(font * 18);
    FlickUi::BeginDisabled(manualPage < 0);
    if (FlickUi::Button(Tr("Previous", "上一页"), ImVec2(font * 6, 0))) --manualPage;
    FlickUi::EndDisabled(); FlickUi::SameLine();
    if (FlickUi::Button(Tr("Contents", "目录"), ImVec2(font * 5, 0))) manualPage = -1;
    FlickUi::SameLine(); FlickUi::BeginDisabled(manualPage >= count - 1);
    if (FlickUi::Button(Tr("Next", "下一页"), ImVec2(font * 6, 0))) ++manualPage;
    FlickUi::EndDisabled();
    const std::string previous = manualPage > 0 ? std::string(Tr("Previous topic: ", "上一项：")) + manualLessons[manualPage - 1].title : Tr("Previous topic: Contents", "上一项：目录");
    const std::string next = manualPage + 1 < count ? std::string(Tr("Next topic: ", "下一项：")) + manualLessons[manualPage + 1].title : Tr("Last topic", "已是最后一项");
    centered(FlickUi::CalcTextSize(previous.c_str()).x); FlickUi::TextDisabled("%s", previous.c_str());
    centered(FlickUi::CalcTextSize(next.c_str()).x); FlickUi::TextDisabled("%s", next.c_str());
    FlickUi::End();
    if (!manualOpen || manualPage < 0 || displayedPage != manualPage || FlickUi::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return;

    const auto& lesson = manualLessons[manualPage];
    auto anchor = manualAnchors[static_cast<std::size_t>(lesson.target)];
    if (!anchor.visible) anchor = manualAnchors[static_cast<std::size_t>(lesson.tab)];
    if (!anchor.visible) return;
    const ImVec2 end((anchor.min.x + anchor.max.x) * 0.5f, (anchor.min.y + anchor.max.y) * 0.5f);
    // 目标被手册遮住时不穿过正文绘线；用户拖开窗口后立即恢复指引。
    if (end.x >= windowMin.x && end.x <= windowMax.x && end.y >= windowMin.y && end.y <= windowMax.y) return;
    const ImVec2 start(std::clamp(end.x, windowMin.x, windowMax.x), std::clamp(end.y, windowMin.y, windowMax.y));
    const float dx = end.x - start.x, dy = end.y - start.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 1) return;
    const float ux = dx / length, uy = dy / length, head = font * 0.65f;
    const auto color = FlickUi::GetColorU32(ImVec4(1.0f, 0.75f, 0.2f, 1.0f));
    auto* draw = FlickUi::GetForegroundDrawList();
    draw->AddRect(anchor.min, anchor.max, color, 4, 0, 2);
    draw->AddLine(start, end, color, 2.5f);
    draw->AddTriangleFilled(end, ImVec2(end.x - ux * head - uy * head * 0.5f, end.y - uy * head + ux * head * 0.5f),
        ImVec2(end.x - ux * head + uy * head * 0.5f, end.y - uy * head - ux * head * 0.5f), color);
}
