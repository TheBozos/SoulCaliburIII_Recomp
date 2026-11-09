#pragma once

// What scripts/sc3_build.py says on stdout, as the launcher shows it: one line per event (PROGRESS, PERCENT, SKIP, DONE,
// ERROR, RESULT, INFO), the rest kept as details.

#include <string>
#include <vector>

struct BuildStep
{
    enum class State
    {
        Pending,
        Running,
        Done,
        Skipped,
        Failed
    };
    std::string id;
    State state = State::Pending;
    int percent = 0;
    double seconds = 0.0;   // DONE's
    double startedAt = 0.0; // the launcher's clock
    double weight = 1.0;    // its usual share of the time (for the estimate)
};

class BuildModel
{
public:
    BuildModel();
    void reset(double now);
    void feed(const std::string &line, double now);

    std::vector<BuildStep> steps;
    std::string errorStep, errorMessage;
    std::vector<std::string> details; // every line, for "Copy details"
    bool finished = false;            // RESULT seen
    double startedAt = 0.0;

    int current() const; // the running step's index, -1 if none
    // 0..1 of the expected work, and seconds left (-1: too early to say).
    double fraction() const;
    double secondsLeft(double now) const;
};
