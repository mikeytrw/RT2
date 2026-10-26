#include "EditorFonts.h"

#include "EditorIcons.h"
#include "RTLog.h"

#include <imgui.h>

#include <filesystem>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace rt2 {

namespace {

EditorFontSet g_Fonts;

// Pixel sizes at content scale 1.0.
constexpr float kBodySize = 15.0f;
constexpr float kMonoSize = 14.0f;
constexpr float kIconSize = 28.0f; // large standalone icons (asset tiles)

std::filesystem::path FontDirectory()
{
#ifdef _WIN32
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH)
        return std::filesystem::path(buffer).parent_path() / L"fonts";
#endif
    return std::filesystem::current_path() / "fonts";
}

const ImWchar* IconRanges()
{
    static const ImWchar ranges[] = { RT2_ICON_RANGE_MIN, RT2_ICON_RANGE_MAX, 0 };
    return ranges;
}

// One Inter weight with Lucide merged in. Icons are vertically nudged so
// their 24-unit grid centers on Inter's x-height instead of sitting high.
ImFont* AddTextFont(const std::filesystem::path& textFile,
                    const std::filesystem::path& iconFile,
                    float size, float scale)
{
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig textConfig;
    textConfig.OversampleH = 2;
    textConfig.OversampleV = 2;
    ImFont* font = io.Fonts->AddFontFromFileTTF(
        textFile.string().c_str(), size * scale, &textConfig);
    if (!font)
        return nullptr;

    ImFontConfig iconConfig;
    iconConfig.MergeMode = true;
    iconConfig.PixelSnapH = true;
    iconConfig.GlyphMinAdvanceX = size * scale;
    iconConfig.GlyphOffset = ImVec2(0.0f, 2.0f * scale);
    if (!io.Fonts->AddFontFromFileTTF(iconFile.string().c_str(), size * scale,
                                      &iconConfig, IconRanges()))
        return nullptr;
    return font;
}

void FallBack(const std::string& why)
{
    RT_LOG("[EditorFonts] %s; using ImGui's built-in font", why.c_str());
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFont* builtIn = io.Fonts->AddFontDefault();
    io.FontDefault = builtIn;
    const float scale = g_Fonts.scale;
    g_Fonts = EditorFontSet{};
    g_Fonts.body = g_Fonts.medium = g_Fonts.bold = g_Fonts.mono = g_Fonts.icons = builtIn;
    g_Fonts.scale = scale;
    g_Fonts.fallback = true;
    g_Fonts.error = why;
}

} // anonymous namespace

bool LoadEditorFonts(float contentScale)
{
    g_Fonts = EditorFontSet{};
    g_Fonts.scale = contentScale > 0.0f ? contentScale : 1.0f;
    const float scale = g_Fonts.scale;

    const std::filesystem::path dir = FontDirectory();
    const std::filesystem::path regular = dir / "Inter-Regular.ttf";
    const std::filesystem::path medium = dir / "Inter-Medium.ttf";
    const std::filesystem::path semiBold = dir / "Inter-SemiBold.ttf";
    const std::filesystem::path mono = dir / "JetBrainsMono-Regular.ttf";
    const std::filesystem::path icons = dir / "lucide.ttf";

    for (const auto* file : { &regular, &medium, &semiBold, &mono, &icons })
    {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(*file, ec))
        {
            FallBack("font file missing: " + file->string());
            return false;
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    g_Fonts.body = AddTextFont(regular, icons, kBodySize, scale);
    g_Fonts.medium = AddTextFont(medium, icons, kBodySize, scale);
    g_Fonts.bold = AddTextFont(semiBold, icons, kBodySize, scale);

    ImFontConfig monoConfig;
    monoConfig.OversampleH = 2;
    monoConfig.OversampleV = 2;
    g_Fonts.mono = io.Fonts->AddFontFromFileTTF(mono.string().c_str(), kMonoSize * scale, &monoConfig);

    ImFontConfig iconConfig;
    iconConfig.PixelSnapH = true;
    g_Fonts.icons = io.Fonts->AddFontFromFileTTF(icons.string().c_str(), kIconSize * scale,
                                                 &iconConfig, IconRanges());

    if (!g_Fonts.body || !g_Fonts.medium || !g_Fonts.bold || !g_Fonts.mono || !g_Fonts.icons)
    {
        FallBack("a font in " + dir.string() + " failed to load");
        return false;
    }

    io.FontDefault = g_Fonts.body;
    RT_LOG("[EditorFonts] loaded Inter, JetBrains Mono and Lucide from %s at scale %.2f",
           dir.string().c_str(), scale);
    return true;
}

const EditorFontSet& EditorFonts()
{
    return g_Fonts;
}

} // namespace rt2
