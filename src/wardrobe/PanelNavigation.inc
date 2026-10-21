// 此文件在 Panel 的匿名命名空间内引入，复用本面板的导航和搜索状态。
std::string movingEntry, moveDirectory, moveReference, moveSearch;
int moveMode = 1, movePosition = 1;
bool openMove{};
std::string directoryName, editingDirectory;
bool openDirectoryEditor{};

std::vector<std::string> NavigationIDs(const std::string& kind, const std::string& directory = {})
{
    const auto& db = Service::Get().store.database;
    std::vector<std::string> ids;
    if (kind == "directory")
        for (const auto& [id, value] : db["directories"].items()) ids.push_back("directory:" + id);
    else if (kind == "category")
        for (const auto& [id, value] : db["categories"].items()) ids.push_back("categories:" + id);
    else
    {
        const auto& placements = db["navigation"]["placements"];
        for (const auto& [id, value] : db["groups"].items())
        {
            const auto key = "groups:" + id;
            if (placements.value(key, "") == directory || (!db["directories"].contains(placements.value(key, "")) && directory.empty())) ids.push_back(key);
        }
        for (const auto& [id, value] : db["pluginCategories"].items())
        {
            const auto key = "pluginCategories:" + id;
            if (value.value("enabled", true) && placements.contains(key) &&
                (placements.value(key, "") == directory || (!db["directories"].contains(placements.value(key, "")) && directory.empty()))) ids.push_back(key);
        }
    }
    return OrderedIDs(db["navigation"][kind == "directory" ? "directoryOrder" : kind == "category" ? "categoryOrder" : "groupOrder"], ids);
}

std::string NavigationName(const std::string& key)
{
    if (key.starts_with("directory:")) return Service::Get().store.database["directories"][key.substr(10)].value("name", key);
    const auto* group = Service::Get().Group(key);
    return group ? ObjectName(SplitGroupID(key).second, *group) : key;
}

void MoveNavigation(const std::string& key, const std::string& directory, std::size_t position)
{
    auto& service = Service::Get();
    if (!service.store.writable) return;
    auto& nav = service.store.database["navigation"];
    const auto kind = key.starts_with("directory:") ? "directory" : key.starts_with("categories:") ? "category" : "group";
    const auto field = std::string_view(kind) == "directory" ? "directoryOrder" : std::string_view(kind) == "category" ? "categoryOrder" : "groupOrder";
    if (std::string_view(kind) == "group")
    {
        if (directory == "@registered")
        {
            if (!key.starts_with("pluginCategories:")) return;
            nav["placements"].erase(key);
            auto order = nav[field].get<std::vector<std::string>>(); std::erase(order, key); nav[field] = order;
            service.Save(); return;
        }
        nav["placements"][key] = directory;
    }
    auto local = NavigationIDs(kind, directory);
    MoveOrdered(local, key, position);
    // 全局顺序同时保留其他目录条目，精确位置只对目标目录成员计算。
    auto all = nav[field].get<std::vector<std::string>>();
    for (const auto& id : local) std::erase(all, id);
    all.insert(all.end(), local.begin(), local.end()); nav[field] = all;
    service.Save();
}

bool NavigationHeader(const std::string& key, const std::string& label, bool forceOpen)
{
    auto& service = Service::Get();
    auto& collapsed = service.store.database["navigation"]["collapsed"];
    FlickUi::SetNextItemOpen(forceOpen || !collapsed.value(key, false), ImGuiCond_Always);
    const bool expanded = FlickUi::CollapsingHeader((label + "##" + key).c_str());
    if (sidebarSearch.empty() && FlickUi::IsItemClicked() && service.store.writable)
    { collapsed[key] = !expanded; service.Save(); }
    return expanded;
}

void NavigationDrag(const std::string& key, const std::string& directory, bool header = false)
{
    auto& service = Service::Get();
    if (!service.store.writable) return;
    if (!key.starts_with("section:") && !key.empty() && FlickUi::BeginDragDropSource())
    {
        FlickUi::SetDragDropPayload("SW_NAV", key.c_str(), key.size() + 1);
        FlickUi::TextUnformatted(NavigationName(key).c_str()); FlickUi::EndDragDropSource();
    }
    if (FlickUi::BeginDragDropTarget())
    {
        if (const auto* payload = FlickUi::AcceptDragDropPayload("SW_NAV", ImGuiDragDropFlags_AcceptBeforeDelivery))
        {
            const std::string source(static_cast<const char*>(payload->Data));
            const bool sourceDirectory = source.starts_with("directory:");
            const bool sourceCategory = source.starts_with("categories:");
            const bool compatible = directory == "@registered" ? source.starts_with("pluginCategories:") : sourceDirectory ? key.starts_with("directory:") : sourceCategory ? key.starts_with("categories:") : !key.starts_with("categories:");
            if (compatible && source != key)
            {
                const bool after = FlickUi::GetMousePos().y > (FlickUi::GetItemRectMin().y + FlickUi::GetItemRectMax().y) / 2;
                const float y = after ? FlickUi::GetItemRectMax().y : FlickUi::GetItemRectMin().y;
                FlickUi::GetWindowDrawList()->AddLine(ImVec2(FlickUi::GetItemRectMin().x, y), ImVec2(FlickUi::GetItemRectMax().x, y), FlickUi::GetColorU32(ImGuiCol_DragDropTarget), 2);
                if (payload->IsDelivery())
                {
                    const auto kind = sourceDirectory ? "directory" : sourceCategory ? "category" : "group";
                    auto ids = NavigationIDs(kind, directory); std::erase(ids, source);
                    const auto it = std::find(ids.begin(), ids.end(), key);
                    const auto position = header && !sourceDirectory ? ids.size() : it == ids.end() ? ids.size() : static_cast<std::size_t>(it - ids.begin()) + (after ? 1 : 0);
                    MoveNavigation(source, directory, position);
                }
                else if (header && !sourceDirectory && !directory.empty() && FlickUi::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
                {
                    static std::string hovered; static double since{};
                    if (hovered != key) { hovered = key; since = FlickUi::GetTime(); }
                    if (FlickUi::GetTime() - since > 0.65) service.store.database["navigation"]["collapsed"][key] = false;
                }
            }
        }
        FlickUi::EndDragDropTarget();
    }
}

void NavigationMenu(const std::string& key)
{
    if (!FlickUi::BeginPopupContextItem()) return;
    auto& service = Service::Get();
    FlickUi::BeginDisabled(!service.store.writable);
    if (FlickUi::MenuItem(Tr("Move to...", "移动到…")))
    {
        movingEntry = key; moveDirectory = service.store.database["navigation"]["placements"].value(key, "");
        moveReference.clear(); moveSearch.clear(); moveMode = 1; openMove = true;
    }
    if (key.starts_with("directory:"))
    {
        if (FlickUi::MenuItem(Tr("Rename directory", "重命名目录")))
        { editingDirectory = key.substr(10); directoryName = NavigationName(key); openDirectoryEditor = true; }
        if (FlickUi::MenuItem(Tr("Delete directory (keep groups)", "删除目录（保留服装组）")))
        {
            const auto id = key.substr(10);
            for (auto& placement : service.store.database["navigation"]["placements"]) if (placement == id) placement = "";
            service.store.database["directories"].erase(id); service.Save();
        }
    }
    else
    {
        if (FlickUi::MenuItem(Tr("Edit group", "编辑服装组"))) StartGroupEdit(key);
        if (key.starts_with("pluginCategories:") && FlickUi::MenuItem(Tr("Return to registered plugins", "移回已注册模组"))) MoveNavigation(key, "@registered", 0);
    }
    FlickUi::EndDisabled(); FlickUi::EndPopup();
}

void DrawNavigationDialogs()
{
    auto& service = Service::Get();
    if (openMove) { FlickUi::OpenPopup(Tr("Move navigation entry", "移动导航条目")); openMove = false; }
    if (FlickUi::BeginPopupModal(Tr("Move navigation entry", "移动导航条目"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        FlickUi::TextUnformatted(NavigationName(movingEntry).c_str());
        const auto kind = movingEntry.starts_with("directory:") ? "directory" : movingEntry.starts_with("categories:") ? "category" : "group";
        if (std::string_view(kind) == "group" && FlickUi::BeginCombo(Tr("Directory", "目录"), moveDirectory.empty() ? Tr("Unsorted", "未整理") : moveDirectory.c_str()))
        {
            if (FlickUi::Selectable(Tr("Unsorted", "未整理"), moveDirectory.empty())) moveDirectory.clear();
            for (const auto& key : NavigationIDs("directory"))
                if (FlickUi::Selectable(NavigationName(key).c_str(), moveDirectory == key.substr(10))) moveDirectory = key.substr(10);
            if (movingEntry.starts_with("pluginCategories:") && FlickUi::Selectable(Tr("Registered plugins", "已注册模组"))) moveDirectory = "@registered";
            FlickUi::EndCombo();
        }
        const char* modes[] = {Tr("Beginning", "开头"), Tr("End", "末尾"), Tr("Position", "第 N 位"), Tr("Before entry", "指定条目之前"), Tr("After entry", "指定条目之后")};
        FlickUi::Combo(Tr("Position", "位置"), &moveMode, modes, 5);
        if (moveMode == 2) FlickUi::InputInt(Tr("Number", "序号"), &movePosition);
        auto ids = NavigationIDs(kind, moveDirectory); std::erase(ids, movingEntry);
        if (moveMode >= 3)
        {
            FlickUi::InputTextWithHint("##move search", Tr("Search entries / pinyin", "搜索条目 / 拼音"), &moveSearch);
            FlickUi::BeginChild("Move candidates", ImVec2(400, 220), ImGuiChildFlags_Borders);
            for (const auto& id : ids) if (SearchScore(moveSearch, NavigationName(id)) > 0)
                if (FlickUi::Selectable((NavigationName(id) + "##" + id).c_str(), moveReference == id)) moveReference = id;
            FlickUi::EndChild();
        }
        FlickUi::BeginDisabled(moveMode >= 3 && std::find(ids.begin(), ids.end(), moveReference) == ids.end());
        if (FlickUi::Button(Tr("Move", "移动")))
        {
            const auto it = std::find(ids.begin(), ids.end(), moveReference);
            const auto position = moveMode == 0 ? 0u : moveMode == 1 ? ids.size() : moveMode == 2 ? static_cast<std::size_t>(std::max(1, movePosition) - 1) :
                static_cast<std::size_t>(it - ids.begin()) + (moveMode == 4 ? 1 : 0);
            MoveNavigation(movingEntry, moveDirectory, position); FlickUi::CloseCurrentPopup();
        }
        FlickUi::EndDisabled(); FlickUi::SameLine();
        if (FlickUi::Button(Tr("Cancel", "取消"))) FlickUi::CloseCurrentPopup();
        FlickUi::EndPopup();
    }
    if (openDirectoryEditor) { FlickUi::OpenPopup(Tr("Directory name", "目录名称")); openDirectoryEditor = false; }
    if (FlickUi::BeginPopupModal(Tr("Directory name", "目录名称"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        EditText(Tr("Name", "名称"), directoryName);
        FlickUi::BeginDisabled(directoryName.empty() || !service.store.writable);
        if (FlickUi::Button(Tr("Save", "保存")))
        {
            if (editingDirectory.empty())
                for (int i = 1;; ++i) { editingDirectory = "dir-" + std::to_string(i); if (!service.store.database["directories"].contains(editingDirectory)) break; }
            service.store.database["directories"][editingDirectory] = {{"name", directoryName}};
            service.Save(); FlickUi::CloseCurrentPopup();
        }
        FlickUi::EndDisabled(); FlickUi::SameLine();
        if (FlickUi::Button(Tr("Cancel", "取消"))) FlickUi::CloseCurrentPopup();
        FlickUi::EndPopup();
    }
}
