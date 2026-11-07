#pragma once

// The launcher's window: SDL3 with an OpenGL 3.3 context, Dear ImGui through its own SDL3 and OpenGL 3 backends
// (window.cpp). Mouse and keyboard only (no gamepad: SDL's gamepad subsystem is never started).

#include <string>

namespace win
{
    // Opens the window at width x height before the DPI scale, sets ui::scale (the display's content scale, or
    // uiScale when > 0; lowered until the window fits the screen) and the ImGui context with its backends.
    bool open(const char *title, int width, int height, float uiScale, std::string &error);
    void setMinimumSize(int width, int height);
    void close();

    // The events since the last call, handed to ImGui; true when the window was asked to close.
    bool pollEvents();
    // While the window is hidden (the game runs): events only, then a pause.
    void idle(double seconds);

    void beginFrame();
    // Draws the frame and presents it; with screenshot set, the frame is also written to that PNG first.
    void endFrame(const std::string &screenshot);

    int width();  // the window's drawable size in pixels
    int height();
    int monitorWidth(); // the desktop mode of the display the window is on
    int monitorHeight();
    void *nativeHandle(); // the HWND (the Open dialog's owner)
    void hide();
    void show(); // shown again, raised and focused
}
