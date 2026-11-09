#include "ui.h"
#include "platform.h"
#include "window.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace ui
{
    float scale = 1.0f;
    Fonts fonts;

    ImU32 rgba(unsigned rgb, float alpha)
    {
        return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255));
    }

    ImVec4 vec(unsigned rgb, float alpha)
    {
        return ImVec4(((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, alpha);
    }

    void setup()
    {
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Tab, arrows, Enter, Space, Esc as usual

        size_t size = 0;
        const void *exo = plat::resource("EXO2", size);
        if (exo && size)
        {
            ImFontConfig cfg;
            cfg.FontDataOwnedByAtlas = false; // the exe's resource, read only
            std::snprintf(cfg.Name, sizeof(cfg.Name), "Exo 2");
            fonts.text = io.Fonts->AddFontFromMemoryTTF(const_cast<void *>(exo), static_cast<int>(size), S(20), &cfg);
        }
        const std::string windir = plat::getEnv("WINDIR").empty() ? "C:\\Windows" : plat::getEnv("WINDIR");
        for (const char *name : {"CascadiaMono.ttf", "CascadiaCode.ttf", "consola.ttf"})
        {
            const std::string file = windir + "\\Fonts\\" + name;
            std::error_code ec;
            if (std::filesystem::exists(plat::path(file), ec))
            {
                fonts.mono = io.Fonts->AddFontFromFileTTF(file.c_str(), S(16));
                if (fonts.mono)
                    break;
            }
        }
        if (!fonts.text)
            fonts.text = io.Fonts->Fonts[0];
        if (!fonts.mono)
            fonts.mono = fonts.text;
        io.FontDefault = fonts.text;

        ImGuiStyle &st = ImGui::GetStyle();
        st.FontSizeBase = S(20);
        st.WindowPadding = ImVec2(0, 0);
        st.WindowBorderSize = 0;
        st.FramePadding = ImVec2(S(14), S(9));
        st.ItemSpacing = ImVec2(S(12), S(12));
        st.ItemInnerSpacing = ImVec2(S(10), S(8));
        st.FrameRounding = S(12);
        st.PopupRounding = S(12);
        st.GrabRounding = S(10);
        st.WindowRounding = S(14);
        st.ChildRounding = S(14);
        st.ScrollbarRounding = S(10);
        st.ScrollbarSize = S(12);
        st.FrameBorderSize = 1;
        st.PopupBorderSize = 1;
        st.PopupRounding = S(12);
        st.SelectableTextAlign = ImVec2(0, 0.5f);

        ImVec4 *c = st.Colors;
        const ImVec4 none(0, 0, 0, 0);
        c[ImGuiCol_Text] = vec(color::ink);
        c[ImGuiCol_TextDisabled] = vec(color::inkFaint);
        c[ImGuiCol_WindowBg] = none;
        c[ImGuiCol_ChildBg] = none;
        c[ImGuiCol_PopupBg] = ImVec4(6 / 255.0f, 11 / 255.0f, 32 / 255.0f, 0.97f);
        c[ImGuiCol_Border] = ImVec4(140 / 255.0f, 170 / 255.0f, 1.0f, 0.18f);
        c[ImGuiCol_BorderShadow] = none;
        c[ImGuiCol_FrameBg] = ImVec4(30 / 255.0f, 50 / 255.0f, 120 / 255.0f, 0.35f);
        c[ImGuiCol_FrameBgHovered] = ImVec4(50 / 255.0f, 80 / 255.0f, 170 / 255.0f, 0.45f);
        c[ImGuiCol_FrameBgActive] = ImVec4(70 / 255.0f, 100 / 255.0f, 200 / 255.0f, 0.55f);
        c[ImGuiCol_Button] = c[ImGuiCol_FrameBg];
        c[ImGuiCol_ButtonHovered] = c[ImGuiCol_FrameBgHovered];
        c[ImGuiCol_ButtonActive] = c[ImGuiCol_FrameBgActive];
        c[ImGuiCol_Header] = vec(color::accent, 0.22f);
        c[ImGuiCol_HeaderHovered] = vec(color::cross, 0.28f);
        c[ImGuiCol_HeaderActive] = vec(color::accent, 0.4f);
        c[ImGuiCol_CheckMark] = vec(color::triangle);
        c[ImGuiCol_SliderGrab] = vec(color::cross);
        c[ImGuiCol_SliderGrabActive] = vec(color::accent);
        c[ImGuiCol_ScrollbarBg] = none;
        c[ImGuiCol_ScrollbarGrab] = ImVec4(110 / 255.0f, 140 / 255.0f, 230 / 255.0f, 0.5f);
        c[ImGuiCol_ScrollbarGrabHovered] = vec(color::cross, 0.7f);
        c[ImGuiCol_ScrollbarGrabActive] = vec(color::accent, 0.8f);
        c[ImGuiCol_Separator] = c[ImGuiCol_Border];
        c[ImGuiCol_TextSelectedBg] = ImVec4(110 / 255.0f, 150 / 255.0f, 1.0f, 0.4f);
        c[ImGuiCol_NavCursor] = vec(color::focus);
        c[ImGuiCol_ModalWindowDimBg] = ImVec4(1 / 255.0f, 3 / 255.0f, 10 / 255.0f, 0.7f);
        c[ImGuiCol_PlotHistogram] = vec(color::cross);
    }

    // ---- background ----

    namespace
    {
        float smoothstep(float a, float b, float x)
        {
            const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
            return t * t * (3 - 2 * t);
        }

        // icon.svg's radialGradient (#14328f at the bottom centre, 0.55 #050b26, 1 #01030a) stretched to the window,
        // with global.css's vignette baked in: the colour at (u, v) in 0..1.
        ImU32 gradientAt(float u, float v)
        {
            auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
            const float s0[3] = {0x14, 0x32, 0x8f}, s1[3] = {0x05, 0x0b, 0x26}, s2[3] = {0x01, 0x03, 0x0a};
            const float d = std::sqrt(((u - 0.5f) / 0.62f) * ((u - 0.5f) / 0.62f) + ((v - 1.02f) / 1.05f) * ((v - 1.02f) / 1.05f));
            float rgb[3];
            for (int k = 0; k < 3; ++k)
                rgb[k] = d < 0.55f ? lerp(s0[k], s1[k], d / 0.55f) : lerp(s1[k], s2[k], std::min(1.0f, (d - 0.55f) / 0.45f));
            const float e = std::sqrt((u - 0.5f) * (u - 0.5f) * 4 + (v - 0.5f) * (v - 0.5f) * 4) / 1.414f;
            const float vignette = 1.0f - 0.65f * smoothstep(0.45f, 1.0f, e);
            return IM_COL32(static_cast<int>(rgb[0] * vignette), static_cast<int>(rgb[1] * vignette),
                            static_cast<int>(rgb[2] * vignette), 255);
        }

        // The gradient as a mesh of coloured quads (the GPU interpolates between the vertices: no texture), 48 x 30
        // cells as raylib's 384 x 240 texture was bilinearly stretched.
        void drawGradient(ImDrawList *dl, float W, float H)
        {
            constexpr int cols = 48, rows = 30;
            static std::vector<ImU32> colours;
            if (colours.empty())
                for (int y = 0; y <= rows; ++y)
                    for (int x = 0; x <= cols; ++x)
                        colours.push_back(gradientAt(static_cast<float>(x) / cols, static_cast<float>(y) / rows));
            const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
            dl->PrimReserve(cols * rows * 6, (cols + 1) * (rows + 1));
            const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
            for (int y = 0; y <= rows; ++y)
                for (int x = 0; x <= cols; ++x)
                    dl->PrimWriteVtx(ImVec2(W * x / cols, H * y / rows), uv, colours[static_cast<size_t>(y * (cols + 1) + x)]);
            for (int y = 0; y < rows; ++y)
                for (int x = 0; x < cols; ++x)
                {
                    const ImDrawIdx a = static_cast<ImDrawIdx>(base + y * (cols + 1) + x), b = static_cast<ImDrawIdx>(a + 1),
                                    c = static_cast<ImDrawIdx>(a + cols + 1), d = static_cast<ImDrawIdx>(c + 1);
                    dl->PrimWriteIdx(a), dl->PrimWriteIdx(b), dl->PrimWriteIdx(d);
                    dl->PrimWriteIdx(a), dl->PrimWriteIdx(d), dl->PrimWriteIdx(c);
                }
        }
    }

    void drawBackground(double time, bool motion)
    {
        ImDrawList *dl = ImGui::GetBackgroundDrawList();
        const float W = static_cast<float>(win::width()), H = static_cast<float>(win::height());
        drawGradient(dl, W, H);

        // The boot towers: faint glowing columns rising from the bottom, as on the PS2's boot screen.
        const double t = motion ? time : 0.0;
        const int n = 15;
        for (int i = 0; i < n; ++i)
        {
            const float seed = std::sin(i * 12.9898f) * 43758.5453f;
            const float rnd = seed - std::floor(seed);
            const float centreWeight = 1.0f - std::fabs((i + 0.5f) / n - 0.5f) * 1.2f;
            const float x = W * ((i + 0.5f) / n + (rnd - 0.5f) * 0.02f);
            const float w = W * (0.018f + 0.01f * rnd);
            const float hBase = H * (0.08f + 0.22f * centreWeight * (0.55f + 0.45f * rnd));
            const float hgt = hBase * (1.0f + 0.07f * static_cast<float>(std::sin(t * 0.35 + i * 1.7)));
            const float breathe = 0.85f + 0.15f * static_cast<float>(std::sin(t * 0.5 + i * 2.3));
            const float top = H - hgt;
            for (int g = 3; g >= 1; --g)
            {
                const float pad = S(5.0f * g * g * 0.7f);
                const ImVec2 a(x - w / 2 - pad, top - pad), b(x + w / 2 + pad, top + hgt);
                // raylib's DrawRectangleRounded roundness 0.3: 30 % of the shorter side's half.
                dl->AddRectFilled(a, b, rgba(0x4f74ff, 0.03f * breathe / g), 0.3f * 0.5f * std::min(b.x - a.x, b.y - a.y));
            }
            const float left = std::floor(x - w / 2), right = left + std::floor(w);
            dl->AddRectFilledMultiColor(ImVec2(left, std::floor(top)), ImVec2(right, std::floor(top) + std::floor(hgt)),
                                        rgba(0x6f8dff, 0.26f * breathe), rgba(0x6f8dff, 0.26f * breathe),
                                        rgba(0x14328f, 0.06f * breathe), rgba(0x14328f, 0.06f * breathe));
            dl->AddRectFilled(ImVec2(left, std::floor(top)), ImVec2(right, std::floor(top) + std::floor(S(2))),
                              rgba(0xc9dbff, 0.35f * breathe));
        }
        // Scanlines (global.css's .vignette: 1 px of white at 1.8 % every 3 px).
        for (int y = 0; y < static_cast<int>(H); y += 3)
            dl->AddRectFilled(ImVec2(0, static_cast<float>(y)), ImVec2(W, static_cast<float>(y + 1)), IM_COL32(255, 255, 255, 5));
    }

    void releaseBackground() {}

    // ---- text ----

    void text(const char *s, float size, unsigned rgb, float alpha)
    {
        ImGui::PushFont(fonts.text, S(size));
        ImGui::PushStyleColor(ImGuiCol_Text, vec(rgb, alpha));
        ImGui::TextUnformatted(s);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

    void textWrapped(const char *s, float size, unsigned rgb, float alpha)
    {
        ImGui::PushFont(fonts.text, S(size));
        ImGui::PushStyleColor(ImGuiCol_Text, vec(rgb, alpha));
        ImGui::TextWrapped("%s", s);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

    ImVec2 measure(const char *s, float size, float spacing)
    {
        ImFont *font = fonts.text;
        const ImVec2 base = font->CalcTextSizeA(S(size), FLT_MAX, 0.0f, s);
        if (spacing <= 0)
            return base;
        int glyphs = 0;
        for (const char *p = s; *p;)
        {
            unsigned c;
            p += ImTextCharFromUtf8(&c, p, nullptr);
            ++glyphs;
        }
        return ImVec2(base.x + S(spacing) * std::max(0, glyphs - 1), base.y);
    }

    namespace
    {
        void drawSpaced(ImDrawList *dl, ImVec2 pos, const char *s, float px, ImU32 colour, float spacing)
        {
            if (spacing <= 0)
            {
                dl->AddText(fonts.text, px, pos, colour, s);
                return;
            }
            float x = pos.x;
            for (const char *p = s; *p;)
            {
                const char *start = p;
                unsigned c;
                p += ImTextCharFromUtf8(&c, p, nullptr);
                dl->AddText(fonts.text, px, ImVec2(x, pos.y), colour, start, p);
                x += fonts.text->CalcTextSizeA(px, FLT_MAX, 0.0f, start, p).x + S(spacing);
            }
        }
    }

    void glowText(ImVec2 pos, const char *s, float size, unsigned rgb, float spacing, bool bold, float glow)
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const float px = S(size);
        if (glow > 0)
        {
            // global.css's --glow: 0 0 10px rgb(120 165 255 / .65), 0 0 28px rgb(80 130 255 / .35), approximated by
            // rings of faint copies.
            for (int ring = 1; ring <= 3; ++ring)
            {
                const float r = S(1.6f * ring);
                const ImU32 c = IM_COL32(110, 155, 255, static_cast<int>(255 * glow * 0.10f / ring));
                for (int k = 0; k < 8; ++k)
                {
                    const float a = k * 3.14159265f / 4;
                    drawSpaced(dl, ImVec2(pos.x + std::cos(a) * r, pos.y + std::sin(a) * r), s, px, c, spacing);
                }
            }
        }
        const ImU32 c = rgba(rgb);
        drawSpaced(dl, pos, s, px, c, spacing);
        if (bold)
        {
            drawSpaced(dl, ImVec2(pos.x + S(0.55f), pos.y), s, px, c, spacing);
            drawSpaced(dl, ImVec2(pos.x + S(0.3f), pos.y + S(0.3f)), s, px, c, spacing);
        }
    }

    // ---- widgets ----

    namespace
    {
        void glowRect(ImDrawList *dl, ImVec2 a, ImVec2 b, float rounding, unsigned rgb, float strength)
        {
            for (int g = 1; g <= 4; ++g)
            {
                const float pad = S(2.5f * g);
                dl->AddRectFilled(ImVec2(a.x - pad, a.y - pad), ImVec2(b.x + pad, b.y + pad), rgba(rgb, strength * 0.07f / g),
                                  rounding + pad);
            }
        }
    }

    bool button(const char *label, ImVec2 size, Kind kind, bool enabled, float textSize)
    {
        const char *visibleEnd = ImGui::FindRenderedTextEnd(label);
        const float px = S(textSize);
        const ImVec2 textSz = fonts.text->CalcTextSizeA(px, FLT_MAX, 0.0f, label, visibleEnd);
        if (size.x <= 0)
            size.x = textSz.x + S(kind == Kind::Quiet ? 4 : 44);
        if (size.y <= 0)
            size.y = textSz.y + S(kind == Kind::Quiet ? 4 : 22);
        if (!enabled)
            ImGui::BeginDisabled();
        const bool pressed = ImGui::InvisibleButton(label, size);
        const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive(), focused = ImGui::IsItemFocused();
        if (!enabled)
            ImGui::EndDisabled();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const float r = std::min(S(16), size.y / 2);
        const float fade = enabled ? 1.0f : 0.4f;
        const ImVec2 tp(a.x + (size.x - textSz.x) / 2, a.y + (size.y - textSz.y) / 2 - S(1));
        switch (kind)
        {
        case Kind::Primary:
        {
            if (enabled)
                glowRect(dl, a, b, r, color::cross, hovered || focused ? 1.6f : 0.8f);
            const int v0 = dl->VtxBuffer.Size;
            dl->AddRectFilled(a, b, IM_COL32_WHITE, r);
            const int v1 = dl->VtxBuffer.Size;
            const float lift = active ? 0.85f : hovered ? 1.0f : 0.92f;
            auto tone = [&](unsigned rgb)
            {
                return IM_COL32(static_cast<int>(((rgb >> 16) & 255) * lift), static_cast<int>(((rgb >> 8) & 255) * lift),
                                static_cast<int>((rgb & 255) * lift), static_cast<int>(255 * fade));
            };
            ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, v1, a, b, tone(color::cross), tone(color::accent));
            dl->AddRect(a, b, rgba(0xdbe7ff, 0.55f * fade), r, 0, S(1.2f));
            dl->AddText(fonts.text, px, tp, rgba(0x050b26, fade), label, visibleEnd);
            dl->AddText(fonts.text, px, ImVec2(tp.x + S(0.5f), tp.y), rgba(0x050b26, fade), label, visibleEnd);
            break;
        }
        case Kind::Normal:
        case Kind::Danger:
        {
            const unsigned edge = kind == Kind::Danger ? color::circle : 0x8caaff;
            if (enabled && (hovered || focused))
                glowRect(dl, a, b, r, kind == Kind::Danger ? color::circle : color::cross, 0.7f);
            dl->AddRectFilled(a, b, IM_COL32(30, 50, 120, static_cast<int>((active ? 0.6f : hovered ? 0.5f : 0.35f) * 255 * fade)), r);
            dl->AddRect(a, b, rgba(edge, (hovered || focused ? 0.55f : 0.25f) * fade), r, 0, S(1));
            dl->AddText(fonts.text, px, tp, rgba(kind == Kind::Danger ? color::circle : color::ink, fade), label, visibleEnd);
            break;
        }
        case Kind::Quiet:
        {
            const unsigned c = hovered || focused ? color::ink : color::link;
            dl->AddText(fonts.text, px, tp, rgba(c, fade), label, visibleEnd);
            if (hovered)
                dl->AddLine(ImVec2(tp.x, tp.y + textSz.y), ImVec2(tp.x + textSz.x, tp.y + textSz.y), rgba(c, 0.8f), S(1));
            break;
        }
        }
        if (pressed && enabled)
            return true;
        return false;
    }

    bool pill(const char *label, bool selected, float textSize, bool enabled)
    {
        const char *visibleEnd = ImGui::FindRenderedTextEnd(label);
        const float px = S(textSize);
        const ImVec2 textSz = fonts.text->CalcTextSizeA(px, FLT_MAX, 0.0f, label, visibleEnd);
        const ImVec2 size(textSz.x + S(30), textSz.y + S(14));
        if (!enabled)
            ImGui::BeginDisabled();
        const bool pressed = ImGui::InvisibleButton(label, size);
        const bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused();
        if (!enabled)
            ImGui::EndDisabled();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const float r = size.y / 2;
        const float fade = enabled ? 1.0f : 0.4f;
        if (selected)
        {
            glowRect(dl, a, b, r, color::accent, 0.6f);
            dl->AddRectFilled(a, b, rgba(color::accent, 0.2f * fade), r);
            dl->AddRect(a, b, rgba(color::accent, 0.65f * fade), r, 0, S(1));
        }
        else if (hovered || focused)
            dl->AddRectFilled(a, b, IM_COL32(30, 50, 120, static_cast<int>(0.45f * 255)), r);
        const ImVec2 tp(a.x + (size.x - textSz.x) / 2, a.y + (size.y - textSz.y) / 2 - S(1));
        dl->AddText(fonts.text, px, tp, rgba(selected || hovered ? color::ink : color::inkDim, fade), label, visibleEnd);
        return pressed && enabled;
    }

    void progressBar(float fraction, ImVec2 size, double time)
    {
        ImGui::Dummy(size);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const float r = size.y / 2;
        dl->AddRectFilled(a, b, IM_COL32(30, 50, 120, 90), r);
        dl->AddRect(a, b, IM_COL32(140, 170, 255, 46), r, 0, S(1));
        fraction = std::clamp(fraction, 0.0f, 1.0f);
        if (fraction > 0.0f)
        {
            const ImVec2 fb(a.x + std::max(size.y, size.x * fraction), b.y);
            glowRect(dl, a, fb, r, color::cross, 0.9f);
            const int v0 = dl->VtxBuffer.Size;
            dl->AddRectFilled(a, fb, IM_COL32_WHITE, r);
            ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, a, ImVec2(a.x + size.x, a.y),
                                                          rgba(color::cross), rgba(color::accent));
            // A soft light sweeping along the filled part while it works.
            if (fraction < 1.0f)
            {
                const float w = fb.x - a.x;
                const float x = a.x + std::fmod(static_cast<float>(time) * S(160), w + S(80)) - S(40);
                dl->PushClipRect(a, fb, true);
                dl->AddRectFilledMultiColor(ImVec2(x - S(40), a.y), ImVec2(x, b.y), IM_COL32(255, 255, 255, 0),
                                            IM_COL32(255, 255, 255, 60), IM_COL32(255, 255, 255, 60), IM_COL32(255, 255, 255, 0));
                dl->AddRectFilledMultiColor(ImVec2(x, a.y), ImVec2(x + S(40), b.y), IM_COL32(255, 255, 255, 60),
                                            IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 60));
                dl->PopClipRect();
            }
        }
    }

    namespace
    {
        ImVec2 g_cardStart;
        float g_cardWidth = 0, g_cardPad = 0;
    }

    void beginCard(float width, float padding)
    {
        g_cardStart = ImGui::GetCursorScreenPos();
        g_cardWidth = width;
        g_cardPad = S(padding);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(1);
        ImGui::SetCursorScreenPos(ImVec2(g_cardStart.x + g_cardPad, g_cardStart.y + g_cardPad));
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(g_cardStart.x + width - g_cardPad - ImGui::GetWindowPos().x);
    }

    void endCard()
    {
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        const float bottom = ImGui::GetItemRectMax().y + g_cardPad;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->ChannelsSetCurrent(0);
        const ImVec2 a = g_cardStart, b(g_cardStart.x + g_cardWidth, bottom);
        // global.css's --glass, --line and a top highlight (--glass-hi).
        dl->AddRectFilled(ImVec2(a.x, a.y + S(6)), ImVec2(b.x, b.y + S(10)), IM_COL32(0, 0, 0, 70), S(16));
        dl->AddRectFilled(a, b, IM_COL32(8, 14, 36, 205), S(14));
        dl->AddRectFilledMultiColor(ImVec2(a.x + S(14), a.y + 1), ImVec2(b.x - S(14), a.y + S(40)), IM_COL32(30, 50, 120, 60),
                                    IM_COL32(30, 50, 120, 60), IM_COL32(30, 50, 120, 0), IM_COL32(30, 50, 120, 0));
        dl->AddRect(a, b, IM_COL32(140, 170, 255, 46), S(14), 0, S(1));
        dl->ChannelsMerge();
        ImGui::SetCursorScreenPos(ImVec2(a.x, b.y));
        ImGui::Dummy(ImVec2(g_cardWidth, 0));
    }

    void statusDot(ImVec2 c, int state, double time)
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const float r = S(9);
        switch (state)
        {
        case 0: // pending
            dl->AddCircle(c, r * 0.7f, rgba(color::inkFaint, 0.8f), 0, S(1.5f));
            break;
        case 1: // running
        {
            const float pulse = 0.5f + 0.5f * static_cast<float>(std::sin(time * 4.0));
            dl->AddCircleFilled(c, r * (1.2f + 0.5f * pulse), rgba(color::cross, 0.15f));
            dl->AddCircleFilled(c, r * 0.75f, rgba(color::cross));
            break;
        }
        case 2: // done
        case 3: // done earlier
        {
            const unsigned g = color::good;
            dl->AddCircleFilled(c, r, rgba(g, state == 2 ? 0.22f : 0.12f));
            dl->AddCircle(c, r, rgba(g, state == 2 ? 0.9f : 0.5f), 0, S(1.5f));
            const ImVec2 pts[3] = {ImVec2(c.x - r * 0.45f, c.y + r * 0.02f), ImVec2(c.x - r * 0.1f, c.y + r * 0.38f),
                                   ImVec2(c.x + r * 0.5f, c.y - r * 0.38f)};
            dl->AddPolyline(pts, 3, rgba(g, state == 2 ? 1.0f : 0.6f), 0, S(2));
            break;
        }
        default: // failed
            dl->AddCircleFilled(c, r, rgba(color::early, 0.22f));
            dl->AddCircle(c, r, rgba(color::early), 0, S(1.5f));
            dl->AddLine(ImVec2(c.x - r * 0.4f, c.y - r * 0.4f), ImVec2(c.x + r * 0.4f, c.y + r * 0.4f), rgba(color::early), S(2));
            dl->AddLine(ImVec2(c.x + r * 0.4f, c.y - r * 0.4f), ImVec2(c.x - r * 0.4f, c.y + r * 0.4f), rgba(color::early), S(2));
            break;
        }
    }

    void spinner(ImVec2 c, float radius, double time)
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const float a0 = static_cast<float>(time * 5.0);
        dl->PathArcTo(c, S(radius), a0, a0 + 4.2f, 32);
        dl->PathStroke(rgba(color::cross), 0, S(3));
    }
}
