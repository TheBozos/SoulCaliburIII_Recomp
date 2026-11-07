#include "build_model.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace
{
    // scripts/sc3_build.py's STEPS (the build on BozoEngine), with their usual seconds on a player's build with the kit
    // (16 threads, SSD; a first build): SDL3's configure checks and the compile are most of it.
    struct StepInfo
    {
        const char *id;
        double weight;
    };
    const StepInfo kSteps[] = {
        {"verify", 1},  {"extract", 2},   {"configure", 60}, {"tools", 7},
        {"recompile", 2}, {"packs", 32}, {"compile", 55},   {"stage", 1},
    };

    int indexOf(const std::vector<BuildStep> &steps, const std::string &id)
    {
        for (size_t i = 0; i < steps.size(); ++i)
            if (steps[i].id == id)
                return static_cast<int>(i);
        return -1;
    }
}

BuildModel::BuildModel() { reset(0.0); }

void BuildModel::reset(double now)
{
    steps.clear();
    for (const StepInfo &s : kSteps)
    {
        BuildStep step;
        step.id = s.id;
        step.weight = s.weight;
        steps.push_back(step);
    }
    errorStep.clear();
    errorMessage.clear();
    details.clear();
    finished = false;
    startedAt = now;
}

void BuildModel::feed(const std::string &line, double now)
{
    details.push_back(line);
    std::istringstream in(line);
    std::string kind, id;
    in >> kind >> id;
    std::string rest;
    std::getline(in, rest);
    if (!rest.empty() && rest.front() == ' ')
        rest.erase(0, 1);
    const int i = indexOf(steps, id);
    if (kind == "PROGRESS" && i >= 0)
    {
        for (int j = 0; j < i; ++j) // an earlier step still marked running: it is over
            if (steps[j].state == BuildStep::State::Running)
                steps[j].state = BuildStep::State::Done;
        steps[i].state = BuildStep::State::Running;
        steps[i].percent = 0;
        steps[i].startedAt = now;
    }
    else if (kind == "PERCENT" && i >= 0)
        steps[i].percent = std::clamp(std::atoi(rest.c_str()), 0, 100);
    else if (kind == "SKIP" && i >= 0)
        steps[i].state = BuildStep::State::Skipped;
    else if (kind == "DONE" && i >= 0)
    {
        steps[i].state = BuildStep::State::Done;
        steps[i].percent = 100;
        const size_t space = rest.find(' ');
        steps[i].seconds = space == std::string::npos ? 0.0 : std::atof(rest.c_str() + space + 1);
    }
    else if (kind == "ERROR")
    {
        errorStep = id;
        errorMessage = rest;
        if (i >= 0)
            steps[i].state = BuildStep::State::Failed;
    }
    else if (kind == "RESULT")
    {
        finished = true; // the game folder is the one the launcher asked for (--game)
    }
}

int BuildModel::current() const
{
    for (size_t i = 0; i < steps.size(); ++i)
        if (steps[i].state == BuildStep::State::Running)
            return static_cast<int>(i);
    return -1;
}

double BuildModel::fraction() const
{
    double total = 0, done = 0;
    for (const BuildStep &s : steps)
    {
        if (s.state == BuildStep::State::Skipped)
            continue;
        total += s.weight;
        if (s.state == BuildStep::State::Done)
            done += s.weight;
        else if (s.state == BuildStep::State::Running)
            done += s.weight * s.percent / 100.0;
    }
    if (finished)
        return 1.0;
    return total > 0 ? std::clamp(done / total, 0.0, 1.0) : 0.0;
}

double BuildModel::secondsLeft(double now) const
{
    // The expected seconds of what is left, scaled by how this PC did on the work finished so far.
    double doneWeight = 0, doneSeconds = 0, leftWeight = 0;
    for (const BuildStep &s : steps)
    {
        if (s.state == BuildStep::State::Skipped)
            continue;
        if (s.state == BuildStep::State::Done)
        {
            doneWeight += s.weight;
            doneSeconds += s.seconds;
        }
        else if (s.state == BuildStep::State::Running)
        {
            const double part = s.weight * s.percent / 100.0;
            leftWeight += s.weight - part;
            if (s.percent >= 5)
            {
                doneWeight += part;
                doneSeconds += now - s.startedAt;
            }
        }
        else
            leftWeight += s.weight;
    }
    if (now - startedAt < 5.0)
        return -1.0;
    const double ratio = doneWeight >= 20.0 ? std::clamp(doneSeconds / doneWeight, 0.25, 6.0) : 1.0;
    return leftWeight * ratio;
}
