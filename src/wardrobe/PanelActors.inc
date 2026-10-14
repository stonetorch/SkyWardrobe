void DrawActorSelector(bool sidebar = false)
{
    auto& service = Service::Get();
    auto& actors = service.store.database["actors"];
    const auto name = [&](const std::string& key)
    {
        return key == "player" ? std::string(Tr("Player", "玩家")) : actors[key].value("name", key);
    };
    if (sidebar)
    {
        ImGui::TextUnformatted(Tr("Current actor", "当前操作角色"));
        ImGui::SetNextItemWidth(-1.0f);
    }
    if (ImGui::BeginCombo(sidebar ? "##current actor" : Tr("Current actor", "当前操作角色"), name(service.currentActor).c_str()))
    {
        if (ImGui::Selectable(name("player").c_str(), service.currentActor == "player")) service.SelectActor("player");
        for (const auto& [key, actor] : actors.items())
            if (key != "player" && actor.value("managed", true))
            {
                if (ImGui::Selectable((name(key) + "##" + key).c_str(), service.currentActor == key)) service.SelectActor(key);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", key.c_str());
            }
        ImGui::EndCombo();
    }
}

void DrawActorManagement()
{
    auto& service = Service::Get();
    DrawActorSelector();
    ImGui::BeginDisabled(!service.store.writable);
    const auto& watched = service.actorSnapshot.watched;
    ImGui::BeginDisabled(!watched);
    const auto label = std::string(Tr("Add watched actor: ", "添加注视角色：")) + (watched ? watched->name : Tr("No person targeted", "未识别到人物"));
    if (ImGui::Button(label.c_str()) && watched) service.AddActor(*watched);
    ImGui::EndDisabled();
    ImGui::SeparatorText(Tr("Managed actors", "已管理角色"));
    const auto actors = service.store.database["actors"];
    const auto draw = [&](const std::string& key, const Json& profile)
    {
        if (!profile.value("managed", true)) return;
        ImGui::PushID(key.c_str());
        const auto it = std::find_if(service.actorSnapshot.managed.begin(), service.actorSnapshot.managed.end(), [&](const auto& actor) { return actor.key == key; });
        const bool ready = it != service.actorSnapshot.managed.end() && it->ready;
        const auto name = key == "player" ? std::string(Tr("Player", "玩家")) : profile.value("name", key);
        if (ImGui::Selectable((name + (ready ? Tr(" — ready", " · 可编辑") : Tr(" — unloaded", " · 未加载"))).c_str(), service.currentActor == key)) service.SelectActor(key);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", key.c_str());
        if (key != "player" && ImGui::SmallButton(Tr("Remove management", "移除管理"))) service.RemoveActor(key);
        ImGui::PopID();
    };
    draw("player", actors["player"]);
    for (const auto& [key, profile] : actors.items()) if (key != "player") draw(key, profile);
    ImGui::SeparatorText(Tr("Nearby people", "附近人物"));
    static std::string actorSearch;
    ImGui::InputTextWithHint("##actor search", Tr("Search people / pinyin", "搜索人物 / 拼音"), &actorSearch);
    ImGui::BeginChild("Nearby actor list", ImVec2(0, 0));
    for (const auto& actor : service.actorSnapshot.nearby)
        if (SearchScore(actorSearch, actor.name) > 0)
        {
            if (ImGui::Selectable((actor.name + "##nearby-" + actor.key).c_str())) service.AddActor(actor);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", actor.key.c_str());
        }
    ImGui::EndChild(); ImGui::EndDisabled();
}

const char* StateName(std::string_view state)
{
    if (state == "combat") return Tr("Combat", "战斗中");
    if (state == "dungeon") return Tr("Dungeon", "地下城");
    if (state == "city") return Tr("City", "城市");
    if (state == "town") return Tr("Town", "城镇");
    return Tr("Default", "默认");
}

void DrawPresetRules(Json& preset)
{
    auto& service = Service::Get();
    bool changed = false;
    for (const auto* state : OutfitStates)
    {
        ImGui::PushID(state);
        auto& rules = preset["rules"];
        if (!rules.contains(state)) rules[state] = {{"enabled", false}, {"mode", "single"}, {"groups", Json::array()}};
        auto& rule = rules[state];
        if (ImGui::CollapsingHeader(StateName(state), ImGuiTreeNodeFlags_DefaultOpen))
        {
            bool enabled = rule.value("enabled", true);
            if (ImGui::Checkbox(Tr("Enabled", "启用条件"), &enabled)) { rule["enabled"] = enabled; changed = true; }
            bool useWorking = rule.value("useWorking", false);
            if (ImGui::Checkbox(Tr("Continue using working outfit", "继续采用工作搭配"), &useWorking))
            { rule["useWorking"] = useWorking; changed = true; }
            ImGui::BeginDisabled(useWorking);
            const auto mode = rule.value("mode", "single");
            int index = mode == "random" ? 1 : mode == "sequence" ? 2 : 0;
            const char* modes[] = {Tr("Single group", "单个服装组"), Tr("Random group", "随机选择"), Tr("Sequence", "固定顺序")};
            if (ImGui::Combo(Tr("Selection", "选择方式"), &index, modes, 3))
            {
                rule["mode"] = index == 1 ? "random" : index == 2 ? "sequence" : "single";
                if (index == 0 && rule["groups"].size() > 1) rule["groups"] = Json::array({rule["groups"].front()});
                changed = true;
            }
            bool useList = !rule.value("listGroup", "").empty();
            if (index != 0 && ImGui::Checkbox(Tr("Use outfit group as list", "用服装组作为列表"), &useList))
            {
                rule["listGroup"] = useList ? "groups:" : ""; changed = true;
            }
            if (index == 0 && useList) { rule["listGroup"] = ""; useList = false; changed = true; }
            if (useList)
            {
                const auto source = rule.value("listGroup", "");
                if (ImGui::BeginCombo(Tr("List outfit group", "列表服装组"), NavigationName(source).c_str()))
                {
                    static std::string listSearch;
                    ImGui::InputTextWithHint("##list search", Tr("Search groups / pinyin", "搜索服装组 / 拼音"), &listSearch);
                    for (const auto& [id, metadata] : service.store.database["groups"].items())
                        if (MatchesGroupSearch(id, metadata, listSearch) && ImGui::Selectable((ObjectName(id, metadata) + "##" + id).c_str(), source == "groups:" + id))
                        { rule["listGroup"] = "groups:" + id; changed = true; }
                    ImGui::EndCombo();
                }
                ImGui::TextWrapped("%s", Tr("Uses direct references in their saved order. Edit references in the outfit group.", "按保存顺序使用此组的直接引用；在服装组中维护引用列表。"));
                for (const auto& id : RuleCandidates(service.store.database, rule)) ImGui::BulletText("%s", NavigationName(id).c_str());
            }
            else
            {
                auto groups = rule["groups"].get<std::vector<std::string>>();
                std::string remove, moving;
                std::size_t position{};
                for (std::size_t i = 0; i < groups.size(); ++i)
                {
                    const auto& key = groups[i]; ImGui::PushID(key.c_str());
                    if (ImGui::SmallButton("×")) remove = key;
                    ImGui::SameLine();
                    ImGui::Selectable((std::to_string(i + 1) + ". " + NavigationName(key)).c_str(), false);
                    if (ImGui::BeginDragDropSource())
                    { ImGui::SetDragDropPayload(state, key.c_str(), key.size() + 1); ImGui::TextUnformatted(NavigationName(key).c_str()); ImGui::EndDragDropSource(); }
                    if (ImGui::BeginDragDropTarget())
                    {
                        if (const auto* payload = ImGui::AcceptDragDropPayload(state))
                        {
                            moving = static_cast<const char*>(payload->Data);
                            auto others = groups; std::erase(others, moving);
                            const auto it = std::find(others.begin(), others.end(), key);
                            position = static_cast<std::size_t>(it - others.begin());
                        }
                        ImGui::EndDragDropTarget();
                    }
                    if (ImGui::BeginPopupContextItem("Candidate position"))
                    {
                        static int number = 1;
                        ImGui::InputInt(Tr("Position", "第 N 位"), &number);
                        if (ImGui::Button(Tr("Move", "移动"))) { moving = key; position = static_cast<std::size_t>(std::max(1, number) - 1); ImGui::CloseCurrentPopup(); }
                        static std::string referenceSearch;
                        ImGui::InputTextWithHint("##candidate search", Tr("Search reference group", "搜索参照组"), &referenceSearch);
                        for (const auto& other : groups) if (other != key && SearchScore(referenceSearch, NavigationName(other)) > 0)
                        {
                            ImGui::PushID(other.c_str());
                            ImGui::TextUnformatted(NavigationName(other).c_str());
                            if (ImGui::SmallButton(Tr("Before", "移到之前")))
                            { auto ids = groups; std::erase(ids, key); moving = key; position = std::find(ids.begin(), ids.end(), other) - ids.begin(); ImGui::CloseCurrentPopup(); }
                            ImGui::SameLine();
                            if (ImGui::SmallButton(Tr("After", "移到之后")))
                            { auto ids = groups; std::erase(ids, key); moving = key; position = std::find(ids.begin(), ids.end(), other) - ids.begin() + 1; ImGui::CloseCurrentPopup(); }
                            ImGui::PopID();
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
                if (!remove.empty()) { std::erase(groups, remove); changed = true; }
                if (!moving.empty()) { MoveOrdered(groups, moving, position); changed = true; }
                if (ImGui::Button(Tr("Add candidate group", "添加候选服装组"))) ImGui::OpenPopup("Add candidate");
                if (ImGui::BeginPopup("Add candidate"))
                {
                    static std::string query;
                    ImGui::InputTextWithHint("##candidate query", Tr("Search groups / pinyin", "搜索服装组 / 拼音"), &query);
                    ImGui::BeginChild("Candidate results", ImVec2(420, 250));
                    for (const auto* collection : {"categories", "groups", "pluginCategories"})
                        for (const auto& [id, metadata] : service.store.database[collection].items())
                        {
                            const auto key = std::string(collection) + ":" + id;
                            if (!MatchesGroupSearch(id, metadata, query) || std::find(groups.begin(), groups.end(), key) != groups.end()) continue;
                            if (collection == std::string_view("pluginCategories") && !metadata.value("enabled", true)) continue;
                            if (ImGui::Selectable((ObjectName(id, metadata) + "##" + key).c_str()))
                            { if (index == 0) groups = {key}; else groups.push_back(key); changed = true; ImGui::CloseCurrentPopup(); }
                        }
                    ImGui::EndChild(); ImGui::EndPopup();
                }
                rule["groups"] = groups;
                if (index == 0 && groups.size() > 1) ImGui::TextDisabled("%s", Tr("Single mode uses the first usable group.", "单个模式使用第一个有效服装组。"));
            }
            ImGui::EndDisabled();
        }
        ImGui::PopID();
    }
    if (changed)
    {
        const auto& defaults = preset["rules"]["default"]["groups"];
        preset["group"] = defaults.empty() ? std::string{} : defaults.front().get<std::string>();
        service.ResumeAutomatic(true); service.Save();
    }
}
