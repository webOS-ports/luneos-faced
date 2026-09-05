// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>

#include "frame_orientation.h"
#include "fart_engine.h"

#include <cstdio>
#include <glib.h>
#include <glib/gstdio.h>
#include <opencv2/imgproc.hpp>

namespace
{
// Upright first, so a sensor that needs no correction costs one detection
// rather than four.
const int kCandidates[] = {FrameOrientation::kNoRotation, cv::ROTATE_90_COUNTERCLOCKWISE,
                           cv::ROTATE_90_CLOCKWISE, cv::ROTATE_180};

// The detector will occasionally fire weakly on a sideways face - an ear and a
// jawline are not nothing - so accepting the first hit can learn the wrong
// angle. Score every candidate and take the strongest, and only trust it at all
// above this confidence.
constexpr float kMinConfidence = 0.5f;

void rotateInto(const cv::Mat &src, cv::Mat &dst, int rotation)
{
    if (rotation == FrameOrientation::kNoRotation)
        dst = src;
    else
        cv::rotate(src, dst, rotation);
}
} // namespace

FrameOrientation::FrameOrientation(std::string stateDir) : path_(std::move(stateDir) + "/orientation")
{
}

void FrameOrientation::load()
{
    gchar *contents = nullptr;
    gsize  len      = 0;
    if (!g_file_get_contents(path_.c_str(), &contents, &len, nullptr))
        return;

    int value = kNoRotation;
    if (std::sscanf(contents, "%d", &value) == 1 &&
        (value == kNoRotation || value == cv::ROTATE_90_CLOCKWISE || value == cv::ROTATE_180 ||
         value == cv::ROTATE_90_COUNTERCLOCKWISE))
    {
        rotation_    = value;
        calibrated_  = true;
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
    rotateInto(src, dst, rotation_);
}

void FrameOrientation::invalidate()
{
    calibrated_ = false;
    g_remove(path_.c_str());
}

bool FrameOrientation::calibrate(FartEngine &engine, const cv::Mat &raw)
{
    int   best      = kNoRotation;
    float bestScore = 0.0f;
    cv::Mat candidate;

    for (int rotation : kCandidates)
    {
        rotateInto(raw, candidate, rotation);
        if (!candidate.isContinuous())
            candidate = candidate.clone();

        float confidence = 0.0f;
        const int faces = engine.detect(candidate.data, candidate.cols, candidate.rows, &confidence);
        if (faces > 0 && confidence > bestScore)
        {
            bestScore = confidence;
            best      = rotation;
        }
    }

    if (bestScore < kMinConfidence)
        return false;

    rotation_   = best;
    calibrated_ = true;
    g_message("orientation: learned rotation %s (confidence %.2f)", name(), bestScore);
    save();
    return true;
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
