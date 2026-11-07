#include "config.h"
#include "platform.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>

namespace
{
    std::string trim(const std::string &text)
    {
        const size_t first = text.find_first_not_of(" \t\r");
        if (first == std::string::npos)
            return {};
        return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
    }

    // name = value lines; '#' starts a comment, [sections] are ignored (as the runner reads video.ini).
    bool readIni(const std::string &file, std::map<std::string, std::string> &out, bool hashComments)
    {
        std::ifstream in(plat::path(file));
        if (!in)
            return false;
        std::string line;
        while (std::getline(in, line))
        {
            if (hashComments)
                line = line.substr(0, line.find('#'));
            line = trim(line);
            const size_t equal = line.find('=');
            if (line.empty() || line.front() == '[' || line.front() == '#' || line.front() == ';' || equal == std::string::npos)
                continue;
            out[trim(line.substr(0, equal))] = trim(line.substr(equal + 1));
        }
        return true;
    }

    bool writeAtomically(const std::string &file, const std::string &text)
    {
        const std::string tmp = file + ".tmp";
        {
            std::ofstream out(plat::path(tmp), std::ios::binary | std::ios::trunc);
            if (!out)
                return false;
            out << text;
            if (!out)
                return false;
        }
        std::error_code ec;
        std::filesystem::rename(plat::path(tmp), plat::path(file), ec);
        return !ec;
    }
}

bool LauncherConfig::load(const std::string &file)
{
    std::map<std::string, std::string> v;
    if (!readIni(file, v, false))
        return false;
    disc = v["disc"];
    language = v["language"];
    motion = v.count("motion") ? v["motion"] != "0" : true;
    buildPending = v["build_pending"] == "1";
    built = v["built"] == "1";
    lastBuildSeconds = std::atof(v["last_build_seconds"].c_str());
    pendingSeconds = std::atof(v["pending_build_seconds"].c_str());
    updateCheck = v.count("update_check") ? v["update_check"] != "0" : true;
    return true;
}

bool LauncherConfig::save(const std::string &file) const
{
    char seconds[32];
    std::snprintf(seconds, sizeof(seconds), "%.0f", lastBuildSeconds);
    std::string text = "# SC3 Launcher's settings (the launcher writes this file).\n[launcher]\n";
    text += "disc = " + disc + "\n";
    text += "language = " + language + "\n";
    text += std::string("motion = ") + (motion ? "1" : "0") + "\n";
    text += std::string("build_pending = ") + (buildPending ? "1" : "0") + "\n";
    text += std::string("built = ") + (built ? "1" : "0") + "\n";
    text += std::string("last_build_seconds = ") + seconds + "\n";
    std::snprintf(seconds, sizeof(seconds), "%.0f", pendingSeconds);
    text += std::string("pending_build_seconds = ") + seconds + "\n";
    text += std::string("update_check = ") + (updateCheck ? "1" : "0") + "\n";
    return writeAtomically(file, text);
}

bool VideoConfig::load(const std::string &file)
{
    std::map<std::string, std::string> v;
    exists = readIni(file, v, true);
    if (!exists)
        return false;
    int w = 0, h = 0;
    if (std::sscanf(v["size"].c_str(), "%dx%d", &w, &h) == 2 && w >= 160 && h >= 120)
    {
        width = w;
        height = h;
    }
    if (v["mode"] == "windowed" || v["mode"] == "fullscreen" || v["mode"] == "borderless")
        mode = v["mode"];
    if (!v["render_scale"].empty())
        renderScale = v["render_scale"];
    if (!v["gs_scale"].empty())
        gsScale = std::clamp(std::atoi(v["gs_scale"].c_str()), 1, 4);
    return true;
}

bool VideoConfig::save(const std::string &file) const
{
    // The same text ps2_video::save() writes.
    std::string text =
        "# The window (the Video tab of the settings window, F2, writes this file; PS2X_WINDOW_SIZE, PS2X_WINDOW_MODE,\n"
        "# PS2X_RENDER_SCALE and PS2X_GS_SCALE override it). size: the window's, or the full-screen video mode's.\n"
        "# mode: windowed, fullscreen (exclusive) or borderless (the monitor's size). render_scale: the 3D drawn at\n"
        "# that many times the window's size each way (0.25 to 4). gs_scale: the GS renderer's internal scale (1 to 4,\n"
        "# read at launch).\n"
        "\n[video]\n";
    text += "size = " + std::to_string(width) + "x" + std::to_string(height) + "\n";
    text += "mode = " + mode + "\n";
    text += "render_scale = " + renderScale + "\n";
    text += "gs_scale = " + std::to_string(gsScale) + "\n";
    return writeAtomically(file, text);
}

std::vector<std::pair<int, int>> offeredResolutions(int monitorWidth, int monitorHeight)
{
    static const std::pair<int, int> kCommon[] = {
        {640, 480}, {800, 600}, {1024, 768}, {1280, 960}, {1440, 1080}, {1600, 1200}, {1920, 1440}, {2880, 2160}, // 4:3
        {1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160},           // 16:9
    };
    std::vector<std::pair<int, int>> sizes;
    for (const auto &size : kCommon)
        if (monitorWidth <= 0 || (size.first <= monitorWidth && size.second <= monitorHeight))
            sizes.push_back(size);
    if (monitorWidth > 0)
        sizes.emplace_back(monitorWidth, monitorHeight);
    std::sort(sizes.begin(), sizes.end());
    sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
    return sizes;
}
