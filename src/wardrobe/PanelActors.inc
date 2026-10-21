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
        FlickUi::TextUnformatted(Tr("Current actor", "当前操作角色"));
        FlickUi::SetNextItemWidth(-1.0f);
    }
    if (FlickUi::BeginCombo(sidebar ? "##current actor" : Tr("Current actor", "当前操作角色"), name(service.currentActor).c_str()))
    {
        if (FlickUi::Selectable(name("player").c_str(), service.currentActor == "player")) service.SelectActor("player");
        for (const auto& [key, actor] : actors.items())
            if (key != "player" && actor.value("managed", true))
            {
                if (FlickUi::Selectable((name(key) + "##" + key).c_str(), service.currentActor == key)) service.SelectActor(key);
                if (FlickUi::IsItemHovered()) FlickUi::SetTooltip("%s", key.c_str());
            }
        FlickUi::EndCombo();
    }
}

void DrawActorManagement()
{
    auto& service = Service::Get();
    DrawActorSelector();
    FlickUi::BeginDisabled(!service.store.writable);
    const auto& watched = service.actorSnapshot.watched;
    FlickUi::BeginDisabled(!watched);
    const auto label = std::string(Tr("Add watched actor: ", "添加注视角色：")) + (watched ? watched->name : Tr("No person targeted", "未识别到人物"));
    if (FlickUi::Button(label.c_str()) && watched) service.AddActor(*watched);
    FlickUi::EndDisabled();
    FlickUi::SeparatorText(Tr("Managed actors", "已管理角色"));
    const auto actors = service.store.database["actors"];
    const auto draw = [&](const std::string& key, const Json& profile)
    {
        if (!profile.value("managed", true)) return;
        FlickUi::PushID(key.c_str());
        const auto it = std::find_if(service.actorSnapshot.managed.begin(), service.actorSnapshot.managed.end(), [&](const auto& actor) { return actor.key == key; });
        const bool ready = it != service.actorSnapshot.managed.end() && it->ready;
        const auto name = key == "player" ? std::string(Tr("Player", "玩家")) : profile.value("name", key);
        if (FlickUi::Selectable((name + (ready ? Tr(" — ready", " · 可编辑") : Tr(" — unloaded", " · 未加载"))).c_str(), service.currentActor == key)) service.SelectActor(key);
        if (FlickUi::IsItemHovered()) FlickUi::SetTooltip("%s", key.c_str());
        if (key != "player" && FlickUi::SmallButton(Tr("Remove management", "移除管理"))) service.RemoveActor(key);
        FlickUi::PopID();
    };
    draw("player", actors["player"]);
    for (const auto& [key, profile] : actors.items()) if (key != "player") draw(key, profile);
    FlickUi::SeparatorText(Tr("Nearby people", "附近人物"));
    static std::string actorSearch;
    FlickUi::InputTextWithHint("##actor search", Tr("Search people / pinyin", "搜索人物 / 拼音"), &actorSearch);
    FlickUi::BeginChild("Nearby actor list", ImVec2(0, 0));
    for (const auto& actor : service.actorSnapshot.nearby)
        if (SearchScore(actorSearch, actor.name) > 0)
        {
            if (FlickUi::Selectable((actor.name + "##nearby-" + actor.key).c_str())) service.AddActor(actor);
            if (FlickUi::IsItemHovered()) FlickUi::SetTooltip("%s", actor.key.c_str());
        }
    FlickUi::EndChild(); FlickUi::EndDisabled();
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
        FlickUi::PushID(state);
        auto& rules = preset["rules"];
        if (!rules.contains(state)) rules[state] = {{"enabled", false}, {"mode", "single"}, {"groups", Json::array()}};
        auto& rule = rules[state];
        if (FlickUi::CollapsingHeader(StateName(state), ImGuiTreeNodeFlags_DefaultOpen))
        {
            bool enabled = rule.value("enabled", true);
            if (FlickUi::Checkbox(Tr("Enabled", "启用条件"), &enabled)) { rule["enabled"] = enabled; changed = true; }
            bool useWorking = rule.value("useWorking", false);
            if (FlickUi::Checkbox(Tr("Continue using working outfit", "继续采用工作搭配"), &useWorking))
            { rule["useWorking"] = useWorking; changed = true; }
            FlickUi::BeginDisabled(useWorking);
            const auto mode = rule.value("mode", "single");
            int index = mode == "random" ? 1 : mode == "sequence" ? 2 : 0;
            const char* modes[] = {Tr("Single group", "单个服装组"), Tr("Random group", "随机选择"), Tr("Sequence", "固定顺序")};
            if (FlickUi::Combo(Tr("Selection", "选择方式"), &index, modes, 3))
            {
                rule["mode"] = index == 1 ? "random" : index == 2 ? "sequence" : "single";
                if (index == 0 && rule["groups"].size() > 1) rule["groups"] = Json::array({rule["groups"].front()});
                changed = true;
            }
            bool useList = !rule.value("listGroup", "").empty();
            if (index != 0 && FlickUi::Checkbox(Tr("Use outfit group as list", "用服装组作为列表"), &useList))
            {
                rule["listGroup"] = useList ? "groups:" : ""; changed = true;
            }
            if (index == 0 && useList) { rule["listGroup"] = ""; useList = false; changed = true; }
            if (useList)
            {
                const auto source = rule.value("listGroup", "");
                if (FlickUi::BeginCombo(Tr("List outfit group", "列表服装组"), NavigationName(source).c_str()))
                {
                    static std::string listSearch;
                    FlickUi::InputTextWithHint("##list search", Tr("Search groups / pinyin", "搜索服装组 / 拼音"), &listSearch);
                    for (const auto& [id, metadata] : service.store.database["groups"].items())
                        if (MatchesGroupSearch(id, metadata, listSearch) && FlickUi::Selectable((ObjectName(id, metadata) + "##" + id).c_str(), source == "groups:" + id))
                        { rule["listGroup"] = "groups:" + id; changed = true; }
                    FlickUi::EndCombo();
                }
                FlickUi::TextWrapped("%s", Tr("Uses direct references in their saved order. Edit references in the outfit group.", "按保存顺序使用此组的直接引用；在服装组中维护引用列表。"));
                for (const auto& id : RuleCandidates(service.store.database, rule)) FlickUi::BulletText("%s", NavigationName(id).c_str());
            }
            else
            {
                auto groups = rule["groups"].get<std::vector<std::string>>();
                std::string remove, moving;
                std::size_t position{};
                for (std::size_t i = 0; i < groups.size(); ++i)
                {
                    const auto& key = groups[i]; FlickUi::PushID(key.c_str());
                    if (FlickUi::SmallButton("×")) remove = key;
                    FlickUi::SameLine();
                    FlickUi::Selectable((std::to_string(i + 1) + ". " + NavigationName(key)).c_str(), false);
                    if (FlickUi::BeginDragDropSource())
                    { FlickUi::SetDragDropPayload(state, key.c_str(), key.size() + 1); FlickUi::TextUnformatted(NavigationName(key).c_str()); FlickUi::EndDragDropSource(); }
                    if (FlickUi::BeginDragDropTarget())
                    {
                        if (const auto* payload = FlickUi::AcceptDragDropPayload(state))
                        {
                            moving = static_cast<const char*>(payload->Data);
                            auto others = groups; std::erase(others, moving);
                            const auto it = std::find(others.begin(), others.end(), key);
                            position = static_cast<std::size_t>(it - others.begin());
                        }
                        FlickUi::EndDragDropTarget();
                    }
                    if (FlickUi::BeginPopupContextItem("Candidate position"))
                    {
                        static int number = 1;
                        FlickUi::InputInt(Tr("Position", "第 N 位"), &number);
                        if (FlickUi::Button(Tr("Move", "移动"))) { moving = key; position = static_cast<std::size_t>(std::max(1, number) - 1); FlickUi::CloseCurrentPopup(); }
                        static std::string referenceSearch;
                        FlickUi::InputTextWithHint("##candidate search", Tr("Search reference group", "搜索参照组"), &referenceSearch);
                        for (const auto& other : groups) if (other != key && SearchScore(referenceSearch, NavigationName(other)) > 0)
                        {
                            FlickUi::PushID(other.c_str());
                            FlickUi::TextUnformatted(NavigationName(other).c_str());
                            if (FlickUi::SmallButton(Tr("Before", "移到之前")))
                            { auto ids = groups; std::erase(ids, key); moving = key; position = std::find(ids.begin(), ids.end(), other) - ids.begin(); FlickUi::CloseCurrentPopup(); }
                            FlickUi::SameLine();
                            if (FlickUi::SmallButton(Tr("After", "移到之后")))
                            { auto ids = groups; std::erase(ids, key); moving = key; position = std::find(ids.begin(), ids.end(), other) - ids.begin() + 1; FlickUi::CloseCurrentPopup(); }
                            FlickUi::PopID();
                        }
                        FlickUi::EndPopup();
                    }
                    FlickUi::PopID();
                }
                if (!remove.empty()) { std::erase(groups, remove); changed = true; }
                if (!moving.empty()) { MoveOrdered(groups, moving, position); changed = true; }
                if (FlickUi::Button(Tr("Add candidate group", "添加候选服装组"))) FlickUi::OpenPopup("Add candidate");
                if (FlickUi::BeginPopup("Add candidate"))
                {
                    static std::string query;
                    FlickUi::InputTextWithHint("##candidate query", Tr("Search groups / pinyin", "搜索服装组 / 拼音"), &query);
                    FlickUi::BeginChild("Candidate results", ImVec2(420, 250));
                    for (const auto* collection : {"categories", "groups", "pluginCategories"})
                        for (const auto& [id, metadata] : service.store.database[collection].items())
                        {
                            const auto key = std::string(collection) + ":" + id;
                            if (!MatchesGroupSearch(id, metadata, query) || std::find(groups.begin(), groups.end(), key) != groups.end()) continue;
                            if (collection == std::string_view("pluginCategories") && !metadata.value("enabled", true)) continue;
                            if (FlickUi::Selectable((ObjectName(id, metadata) + "##" + key).c_str()))
                            { if (index == 0) groups = {key}; else groups.push_back(key); changed = true; FlickUi::CloseCurrentPopup(); }
                        }
                    FlickUi::EndChild(); FlickUi::EndPopup();
                }
                rule["groups"] = groups;
                if (index == 0 && groups.size() > 1) FlickUi::TextDisabled("%s", Tr("Single mode uses the first usable group.", "单个模式使用第一个有效服装组。"));
            }
            FlickUi::EndDisabled();
        }
        FlickUi::PopID();
    }
    if (changed)
    {
        const auto& defaults = preset["rules"]["default"]["groups"];
        preset["group"] = defaults.empty() ? std::string{} : defaults.front().get<std::string>();
        service.ResumeAutomatic(true); service.Save();
    }
}
