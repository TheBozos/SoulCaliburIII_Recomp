// The launcher's window (window.h): SDL3, an OpenGL 3.3 core context, Dear ImGui's SDL3 and OpenGL 3 backends.
// The few GL calls of its own (clear, viewport, the screenshot's read-back) are loaded through SDL, so nothing links
// opengl32 directly; the screenshot is written by BozoEngine's dependency-free PNG writer (bozo/app/png.h).

#include "window.h"
#include "platform.h"
#include "ui.h"

#include "bozo/app/png.h"

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#ifdef _WIN32
#define GLAPI_CALL __stdcall // opengl32's convention (the same as the default on x64)
#else
#define GLAPI_CALL
#endif

namespace win
{
    namespace
    {
        SDL_Window *g_window = nullptr;
        SDL_GLContext g_context = nullptr;
        bool g_vsync = false;
        uint64_t g_lastFrame = 0;

        // GL 1.1 entry points (the types spelt out: no GL header needed).
        using GlClearColor = void(GLAPI_CALL *)(float, float, float, float);
        using GlClear = void(GLAPI_CALL *)(unsigned);
        using GlViewport = void(GLAPI_CALL *)(int, int, int, int);
        using GlReadPixels = void(GLAPI_CALL *)(int, int, int, int, unsigned, unsigned, void *);
        using GlPixelStorei = void(GLAPI_CALL *)(unsigned, int);
        using GlReadBuffer = void(GLAPI_CALL *)(unsigned);
        GlClearColor glClearColor_ = nullptr;
        GlClear glClear_ = nullptr;
        GlViewport glViewport_ = nullptr;
        GlReadPixels glReadPixels_ = nullptr;
        GlPixelStorei glPixelStorei_ = nullptr;
        GlReadBuffer glReadBuffer_ = nullptr;
        constexpr unsigned kColorBufferBit = 0x4000, kRgba = 0x1908, kUnsignedByte = 0x1401, kPackAlignment = 0x0D05,
                           kBack = 0x0405;

        template <typename T>
        T proc(const char *name)
        {
            return reinterpret_cast<T>(SDL_GL_GetProcAddress(name));
        }

        SDL_DisplayID display()
        {
            SDL_DisplayID d = g_window ? SDL_GetDisplayForWindow(g_window) : 0;
            return d ? d : SDL_GetPrimaryDisplay();
        }

        void screenshot(const std::string &path)
        {
            const int w = width(), h = height();
            if (w <= 0 || h <= 0 || !glReadPixels_)
                return;
            std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4), rows(pixels.size());
            if (glReadBuffer_)
                glReadBuffer_(kBack);
            glPixelStorei_(kPackAlignment, 1);
            glReadPixels_(0, 0, w, h, kRgba, kUnsignedByte, pixels.data());
            const size_t stride = static_cast<size_t>(w) * 4;
            for (int y = 0; y < h; ++y) // GL's rows are bottom-up
                std::copy_n(pixels.data() + static_cast<size_t>(h - 1 - y) * stride, stride, rows.data() + y * stride);
            bozo::app::writePng(path, rows.data(), static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        }
    }

    bool open(const char *title, int w, int h, float uiScale, std::string &error)
    {
        // No gamepad, no sound: the launcher is driven by mouse and keyboard.
        SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            error = std::string("SDL could not start: ") + SDL_GetError();
            return false;
        }
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1); // the boot towers' rounded glow, as raylib's MSAA 4x hint
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
        // The layout is made for w x h at 1x: at a larger display scale the window grows with it, the scale lowered
        // until the window fits (Windows' 150 % on a 1920 x 1080 screen: 1.32; text still half as large again).
        const SDL_DisplayID d = SDL_GetPrimaryDisplay();
        float scale = uiScale > 0 ? uiScale : SDL_GetDisplayContentScale(d);
        scale = std::clamp(scale > 0 ? scale : 1.0f, 1.0f, 3.0f);
        const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(d);
        if (mode && scale > 1.0f)
        {
            const float fit = std::min((mode->w - 40) / static_cast<float>(w), (mode->h - 80) / static_cast<float>(h));
            if (fit > 0.5f)
                scale = std::max(1.0f, std::min(scale, fit));
        }
        ui::scale = scale;
        int ww = static_cast<int>(w * scale), wh = static_cast<int>(h * scale);
        if (mode)
            ww = std::min(ww, mode->w - 40), wh = std::min(wh, mode->h - 80);
        g_window = SDL_CreateWindow(title, ww, wh, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
        if (!g_window)
        {
            // No multisampling on this GPU / driver: without it.
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
            g_window = SDL_CreateWindow(title, ww, wh, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
        }
        if (!g_window)
        {
            error = std::string("the window could not be opened: ") + SDL_GetError();
            return false;
        }
        g_context = SDL_GL_CreateContext(g_window);
        if (!g_context)
        {
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
            SDL_DestroyWindow(g_window);
            g_window = SDL_CreateWindow(title, ww, wh, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
            g_context = g_window ? SDL_GL_CreateContext(g_window) : nullptr;
        }
        if (!g_context || !SDL_GL_MakeCurrent(g_window, g_context))
        {
            error = std::string("OpenGL 3.3 is not available (update the graphics driver): ") + SDL_GetError();
            return false;
        }
        g_vsync = SDL_GL_SetSwapInterval(1);
        glClearColor_ = proc<GlClearColor>("glClearColor");
        glClear_ = proc<GlClear>("glClear");
        glViewport_ = proc<GlViewport>("glViewport");
        glReadPixels_ = proc<GlReadPixels>("glReadPixels");
        glPixelStorei_ = proc<GlPixelStorei>("glPixelStorei");
        glReadBuffer_ = proc<GlReadBuffer>("glReadBuffer");
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED_DISPLAY(d), SDL_WINDOWPOS_CENTERED_DISPLAY(d));
        SDL_ShowWindow(g_window);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        if (!ImGui_ImplSDL3_InitForOpenGL(g_window, g_context) || !ImGui_ImplOpenGL3_Init("#version 330"))
        {
            error = "ImGui's SDL3 / OpenGL 3 backends could not start";
            return false;
        }
        g_lastFrame = SDL_GetTicksNS();
        return true;
    }

    void setMinimumSize(int w, int h)
    {
        if (g_window)
            SDL_SetWindowMinimumSize(g_window, w, h);
    }

    void close()
    {
        if (ImGui::GetCurrentContext())
        {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
        }
        if (g_context)
            SDL_GL_DestroyContext(g_context);
        if (g_window)
            SDL_DestroyWindow(g_window);
        g_context = nullptr;
        g_window = nullptr;
        SDL_Quit();
    }

    bool pollEvents()
    {
        bool closeAsked = false;
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT ||
                (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(g_window)))
                closeAsked = true;
        }
        return closeAsked;
    }

    void idle(double seconds)
    {
        SDL_WaitEventTimeout(nullptr, static_cast<int32_t>(seconds * 1000));
    }

    void beginFrame()
    {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
    }

    void endFrame(const std::string &shot)
    {
        ImGui::Render();
        const int w = width(), h = height();
        glViewport_(0, 0, w, h);
        glClearColor_(1 / 255.0f, 3 / 255.0f, 10 / 255.0f, 1.0f); // global.css's --bg
        glClear_(kColorBufferBit);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (!shot.empty())
            screenshot(shot);
        SDL_GL_SwapWindow(g_window);
        // 60 frames a second at most (as raylib's SetTargetFPS(60)): vsync on most PCs, else a wait.
        const uint64_t frameNs = 1000000000ull / 60, now = SDL_GetTicksNS();
        if (now - g_lastFrame < frameNs)
            SDL_DelayPrecise(frameNs - (now - g_lastFrame));
        g_lastFrame = SDL_GetTicksNS();
    }

    int width()
    {
        int w = 0, h = 0;
        if (g_window)
            SDL_GetWindowSizeInPixels(g_window, &w, &h);
        return w;
    }

    int height()
    {
        int w = 0, h = 0;
        if (g_window)
            SDL_GetWindowSizeInPixels(g_window, &w, &h);
        return h;
    }

    int monitorWidth()
    {
        const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(display());
        return mode ? mode->w : 1920;
    }

    int monitorHeight()
    {
        const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(display());
        return mode ? mode->h : 1080;
    }

    void *nativeHandle()
    {
        if (!g_window)
            return nullptr;
        return SDL_GetPointerProperty(SDL_GetWindowProperties(g_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    }

    void hide()
    {
        if (g_window)
            SDL_HideWindow(g_window);
    }

    void show()
    {
        if (!g_window)
            return;
        SDL_ShowWindow(g_window);
        SDL_RaiseWindow(g_window);
        g_lastFrame = SDL_GetTicksNS();
    }
}
