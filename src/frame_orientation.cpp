// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>

#include "frame_orientation.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

#include <glib.h>
#include <glib/gstdio.h>
#include <opencv2/imgproc.hpp>

namespace
{
// Upright first, so a sensor that needs no correction settles on the first
// candidate. Sideways mounts are far more common than upside down ones.
const int kCandidates[] = {FrameOrientation::kNoRotation, cv::ROTATE_90_COUNTERCLOCKWISE,
                           cv::ROTATE_90_CLOCKWISE, cv::ROTATE_180};
constexpr int kCandidateCount = sizeof(kCandidates) / sizeof(kCandidates[0]);

bool validRotation(int value)
{
    return std::find(std::begin(kCandidates), std::end(kCandidates), value) !=
           std::end(kCandidates);
}
} // namespace

FrameOrientation::FrameOrientation(std::string stateDir) : path_(std::move(stateDir) + "/orientation")
{
}

void FrameOrientation::load()
{
    gchar *contents = nullptr;
    gsize len       = 0;
    if (!g_file_get_contents(path_.c_str(), &contents, &len, nullptr))
        return;

    int value = kNoRotation;
    if (std::sscanf(contents, "%d", &value) == 1 && validRotation(value))
    {
        rotation_   = value;
        calibrated_ = true;
        blind_      = 0;
        g_message("orientation: using learned rotation %s", name());
    }
    g_free(contents);
}

bool FrameOrientation::save() const
{
    gchar *text = g_strdup_printf("%d\n", rotation_);
    const gboolean ok = g_file_set_contents(path_.c_str(), text, -1, nullptr);
    g_free(text);
    if (!ok)
        g_warning("orientation: could not persist to %s", path_.c_str());
    return ok;
}

void FrameOrientation::apply(const cv::Mat &src, cv::Mat &dst) const
{
    if (rotation_ == kNoRotation)
        dst = src;
    else
        cv::rotate(src, dst, rotation_);
}

void FrameOrientation::startSweep()
{
    calibrated_ = false;
    candidate_  = 0;
    answers_    = 0;
    blind_      = 0;
    rotation_   = kCandidates[0];
}

void FrameOrientation::invalidate()
{
    startSweep();
    g_remove(path_.c_str());
}

bool FrameOrientation::observe(bool faceSeen)
{
    if (calibrated_)
    {
        if (faceSeen)
        {
            blind_ = 0;
            return false;
        }
        if (++blind_ >= kRecalibrateAfter)
        {
            g_message("orientation: %d answers without a face under %s, re-learning", blind_,
                      name());
            invalidate();
        }
        return false;
    }

    // Sweeping. The first kSettle answers may still be about frames sent out
    // under the previous candidate.
    ++answers_;
    if (answers_ > kSettle && faceSeen)
    {
        calibrated_ = true;
        blind_      = 0;
        g_message("orientation: learned rotation %s", name());
        save();
        return true;
    }
    if (answers_ >= kPerCandidate)
    {
        candidate_ = (candidate_ + 1) % kCandidateCount;
        rotation_  = kCandidates[candidate_];
        answers_   = 0;
    }
    return false;
}

const char *FrameOrientation::name() const
{
    switch (rotation_)
    {
    case cv::ROTATE_90_CLOCKWISE:
        return "90 clockwise";
    case cv::ROTATE_180:
        return "180";
    case cv::ROTATE_90_COUNTERCLOCKWISE:
        return "90 counter-clockwise";
    default:
        return "none";
    }
}
