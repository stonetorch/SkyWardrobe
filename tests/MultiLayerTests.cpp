#include "wardrobe/MultiLayer.h"
#include "wardrobe/LifeModel.h"
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <imgui_internal.h>

namespace
{
    void Require(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }
    void CheckImGuiLayout(float height, float scale, bool compact, const std::array<bool, 3>& expanded)
    {
        auto& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1600, 1600);
        for (int frame = 0; frame < 3; ++frame)
        {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(320 * scale, height));
            ImGui::Begin("Layers", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
            ImGui::SetWindowFontScale(scale);
            const float row = ImGui::GetFrameHeightWithSpacing();
            auto open = expanded;
            if (compact) open = {false, true, false};
            const auto count = std::count(open.begin(), open.end(), true);
            const float available = ImGui::GetContentRegionAvail().y;
            const auto heights = Wardrobe::AllocateLayerHeights(std::max(0.0f, available - row * 3 - count * ImGui::GetStyle().ItemSpacing.y),
                row, {30, 3, 10}, open);
            for (int layer = 0; layer < 3; ++layer)
            {
                ImGui::PushID(layer);
                ImGui::Selectable("Header", open[layer], 0, ImVec2(0, ImGui::GetFrameHeight()));
                Require(ImGui::GetItemRectMax().y <= ImGui::GetWindowPos().y + ImGui::GetWindowHeight() + 0.5f, "Layer header clipped");
                if (open[layer] && heights[layer] > 0)
                {
                    ImGui::BeginChild("items", ImVec2(0, heights[layer]));
                    for (int i = 0; i < (layer == 0 ? 30 : layer == 1 ? 3 : 10); ++i) ImGui::Button("Item");
                    ImGui::EndChild();
                }
                ImGui::PopID();
            }
            Require(ImGui::GetCurrentWindow()->DC.CursorMaxPos.y <= ImGui::GetCurrentWindow()->InnerRect.Max.y + 0.5f, "Parent content overflows");
            ImGui::End(); ImGui::Render();
        }
    }
}
int main()
{
    using namespace Wardrobe;
    try
    {
        LayerSettings settings{{"backup"}, true, true, 0};
        auto rows = ComposeLayers({{"dress", "dress", 4}}, {{"shoe", "shoe", 128}},
            {{"armor", "armor", 4}, {"boots", "boots", 128}, {"glove", "glove", 8}}, settings);
        Require(rows[0].disposition == LayerDisposition::Visible && rows[1].disposition == LayerDisposition::Visible, "Upper layers visible");
        Require(rows[2].disposition == LayerDisposition::Covered && rows[3].coveredBy == "shoe", "Original covered by correct layer");
        Require(rows[4].disposition == LayerDisposition::Visible, "Original passthrough");
        rows = ComposeLayers({}, {{"shoe", "shoe", 128}}, {{"boots", "boots", 128}}, settings);
        Require(rows[0].disposition == LayerDisposition::Visible, "Remove working reveals backup");
        rows = ComposeLayers({}, {}, {{"boots", "boots", 128}}, settings);
        Require(rows[0].disposition == LayerDisposition::Visible, "Remove backup reveals original");
        settings.hiddenSlots = 4;
        rows = ComposeLayers({{"dress", "dress", 4}}, {{"wig", "wig", 2}},
            {{"suit", "suit", 4 | 128}, {"temp", "temp", 8, true}}, settings);
        Require(rows[0].disposition == LayerDisposition::Visible && rows[1].disposition == LayerDisposition::Visible, "Filter applies only to originals");
        Require(rows[2].disposition == LayerDisposition::Hidden, "Any hidden slot hides whole multi-slot armor");
        Require(rows[3].disposition == LayerDisposition::Temporary, "Temporary equipment never passes through");
        settings.hiddenSlots = 0;
        rows = ComposeLayers({{"helmet", "helmet", 1 | 2}}, {{"wig", "wig", 2}}, {{"suit", "suit", 2 | 128}}, settings);
        Require(rows[1].disposition == LayerDisposition::Covered && rows[2].disposition == LayerDisposition::Covered, "Whole armor conflict, not semantic parts");
        rows = ComposeLayers({{"zero", "zero", 0}}, {{"zero", "zero", 0}}, {}, settings);
        Require(rows[1].disposition == LayerDisposition::Covered, "Zero-slot duplicates deduplicated");
        settings.backupEnabled = false; settings.originalEnabled = false;
        rows = ComposeLayers({}, {{"shoe", "shoe", 128}}, {{"boot", "boot", 128}}, settings);
        Require(rows[0].disposition == LayerDisposition::Disabled && rows[1].disposition == LayerDisposition::Disabled, "Disabled layers preserve configuration");

        LifeJson db = {{"workingItems", {"Skyrim.esm|000123"}}, {"activePreset", ""}, {"actualEquipmentMode", "off"}, {"presets", LifeJson::object()}};
        ValidateLifeData(db);
        Require(db["actors"]["player"]["backupItems"].empty() && !db["actors"]["player"]["originalEnabled"].get<bool>(), "Migration keeps previous appearance");
        db["actors"]["npc"] = NewActorProfile("npc");
        db["actors"]["player"]["backupItems"] = {"Skyrim.esm|000456"};
        db["actors"]["player"]["hiddenSlots"] = 0x80000000u;
        ValidateLifeData(db);
        Require(db["actors"]["npc"]["backupItems"].empty(), "Per actor backup isolation");
        auto roundtrip = LifeJson::parse(db.dump()); ValidateLifeData(roundtrip);
        Require(roundtrip == db, "JSON roundtrip preserves layer settings and high slot bit");
        for (const auto& invalid : {LifeJson(-1), LifeJson(0x100000000ull), LifeJson("bad"), LifeJson(1.5)})
        {
            auto broken = db; broken["actors"]["player"]["hiddenSlots"] = invalid;
            bool rejected = false; try { ValidateLifeData(broken); } catch (...) { rejected = true; }
            Require(rejected, "Invalid slot mask rejected");
        }
        int cases = 0;
        for (float scale : {0.75f, 1.0f, 1.5f, 2.0f})
            for (int budget = 0; budget <= 1000; budget += 17)
                for (int mask = 0; mask < 8; ++mask)
                    for (const auto counts : {std::array<int, 3>{0, 0, 0}, {30, 2, 8}, {2, 30, 60}})
                    {
                        const std::array<bool, 3> open{bool(mask & 1), bool(mask & 2), bool(mask & 4)};
                        const auto heights = AllocateLayerHeights(float(budget), 24 * scale, counts, open);
                        Require(heights[0] + heights[1] + heights[2] <= budget + 0.01f, "Height budget respected");
                        for (int i = 0; i < 3; ++i) Require(std::isfinite(heights[i]) && heights[i] >= 0 && (open[i] || heights[i] == 0), "Collapsed and small height invariants");
                        ++cases;
                    }
        const auto heights = AllocateLayerHeights(500, 24, {30, 2, 8}, {true, true, true});
        Require(heights[1] == 72 && heights[0] > heights[2], "Short backup gets only necessary space and working has priority");
        ImGui::CreateContext();
        auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.DeltaTime = 1.0f / 60;
        io.Fonts->AddFontDefault(); io.Fonts->Build();
        int uiCases = 0;
        for (float scale : {0.75f, 1.0f, 1.5f, 2.0f})
            for (float height : {250.0f, 400.0f, 700.0f, 1100.0f})
                for (int mask = 0; mask < 8; ++mask)
                {
                    CheckImGuiLayout(height, scale, height < 400 * scale, {bool(mask & 1), bool(mask & 2), bool(mask & 4)});
                    ++uiCases;
                }
        ImGui::DestroyContext();
        std::cout << "Multi-layer composition, migration and " << cases << " layout cases passed\n";
        std::cout << uiCases << " real ImGui layout cases passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
