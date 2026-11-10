#pragma once

// The launcher's own settings (launcher.ini in the user folder) and the game's window settings (video.ini there, in
// the format of the fork's ps2xRuntime/src/lib/ps2_video_settings.cpp: the runner reads it when its window opens).

#include <string>
#include <utility>
#include <vector>

struct LauncherConfig
{
    std::string disc;          // the disc image the player picked
    std::string language;      // "en", "fr" ("" = Windows' language)
    bool motion = true;        // the background's animation
    bool buildPending = false; // a build started and did not finish (resumes)
    bool built = false;        // the last build finished
    double lastBuildSeconds = 0;  // the last finished build, all its runs (a stop and a resume: both)
    double pendingSeconds = 0;    // the runs of the unfinished build so far (stopped, failed)
    bool updateCheck = true;      // ask GitHub for a newer release at start

    bool load(const std::string &file);
    bool save(const std::string &file) const;
};

struct VideoConfig
{
    // As ps2_video::Settings; mode "windowed", "fullscreen" or "borderless". width 0: the game's default size.
    int width = 0, height = 0;
    std::string mode = "windowed";
    std::string renderScale = "1";
    int gsScale = 1;
    std::string frameRate = "game";
    bool exists = false;

    bool load(const std::string &file);
    bool save(const std::string &file) const;
};

// The sizes the game's Video tab offers on this monitor (ps2_video::resolutions): common 4:3 and 16:9 ones that fit,
// and the monitor's own.
std::vector<std::pair<int, int>> offeredResolutions(int monitorWidth, int monitorHeight);
