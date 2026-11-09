#pragma once

// The launcher's look: RecompWatcher's identity (its src/styles/global.css tokens and public/icon.svg) on Dear
// ImGui, in an SDL3 window (window.h). The PS2 BIOS's radial blue behind faint glowing "boot towers", glass cards, Exo 2.

#include "imgui.h"

#include <string>

namespace ui
{
    // global.css's tokens.
    namespace color
    {
        constexpr unsigned bg = 0x01030a, ink = 0xe8efff, inkDim = 0x9aaccf, inkFaint = 0x64759a, accent = 0xb79bff;
        constexpr unsigned cross = 0x7ea6ff, circle = 0xff6b7a, triangle = 0x4fe0a3, square = 0xff8fd8;
        constexpr unsigned good = 0x4fe0a3, wip = 0xffc35c, early = 0xff6b7a, link = 0x9ec0ff, focus = 0xb9d0ff;
    }
    ImU32 rgba(unsigned rgb, float alpha = 1.0f);
    ImVec4 vec(unsigned rgb, float alpha = 1.0f);

    extern float scale; // the monitor's DPI scale
    inline float S(float v) { return v * scale; }

    struct Fonts
    {
        ImFont *text = nullptr; // Exo 2
        ImFont *mono = nullptr; // Cascadia Mono or Consolas (logs), else Exo 2
    };
    extern Fonts fonts;
    // After win::open: Exo 2 from the exe's resources, the monospace font from Windows, the style.
    void setup();

    // The background, on ImGui's background draw list (after win::beginFrame): the radial gradient, the boot towers
    // (animated unless motion is off), a vignette and faint scanlines.
    void drawBackground(double time, bool motion);
    void releaseBackground();

    // Text helpers (sizes before the DPI scale).
    void text(const char *s, float size, unsigned rgb, float alpha = 1.0f);
    void textWrapped(const char *s, float size, unsigned rgb, float alpha = 1.0f);
    // Glowing text with a faux-bold weight and letter spacing (the wordmark, the title).
    void glowText(ImVec2 pos, const char *s, float size, unsigned rgb, float spacing, bool bold, float glow);
    ImVec2 measure(const char *s, float size, float spacing = 0.0f);

    enum class Kind
    {
        Primary, // the blue-to-violet gradient, glowing
        Normal,  // glass
        Danger,  // glass with the circle's red
        Quiet,   // text only (links, tabs)
    };
    bool button(const char *label, ImVec2 size, Kind kind = Kind::Normal, bool enabled = true, float textSize = 20.0f);
    // A pill that is selected or not (tabs, segmented choices).
    bool pill(const char *label, bool selected, float textSize = 17.0f, bool enabled = true);
    void progressBar(float fraction, ImVec2 size, double time);
    // A glass card from the cursor: begin returns its inner origin; end closes it at the content's height.
    void beginCard(float width, float padding = 28.0f);
    void endCard();
    // A status dot (Pending, Running, Done, Skipped, Failed: 0..4).
    void statusDot(ImVec2 centre, int state, double time);
    void spinner(ImVec2 centre, float radius, double time);
}
