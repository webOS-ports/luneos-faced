// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// Works out which way up this device's camera delivers frames.
//
// Sensors are mounted at whatever angle the OEM found convenient, and nothing
// in the webOS camera service reports it: getInfo() gives resolutions and a
// name, no orientation. On sargo the front sensor arrives rotated 90 degrees
// clockwise, so the user appears lying on their side and libfart - which only
// finds upright faces - returns NO_FACE on frames that are otherwise perfectly
// exposed and framed. Hardcoding sargo's angle would just move the bug to the
// next device, so instead we ask the detector: rotate one frame every which
// way, keep whichever it finds a face in, and remember that on disk.

#pragma once

#include <opencv2/core.hpp>
#include <string>

class FartEngine;

class FrameOrientation
{
public:
    explicit FrameOrientation(std::string stateDir);

    // Reads back a previously learned rotation, if there is one.
    void load();

    bool calibrated() const { return calibrated_; }

    // Turns a raw camera frame into the orientation the detector expects. Safe
    // to call before calibration: it just passes the frame through.
    void apply(const cv::Mat &src, cv::Mat &dst) const;

    // Tries every rotation on one frame and keeps the one with the strongest
    // detection. Returns true when it learned an orientation, which it then
    // persists so later boots skip the sweep. A frame with no face in it at any
    // angle leaves us uncalibrated, to be retried on the next frame.
    bool calibrate(FartEngine &engine, const cv::Mat &raw);

    // Forgets the learned rotation. Used when a calibrated orientation stops
    // producing detections, which means we learned the wrong one - better to
    // pay for one more sweep than to keep failing.
    void invalidate();

    // Human-readable current rotation, for logs.
    const char *name() const;

private:
    bool save() const;

    std::string path_;
    int         rotation_{kNoRotation};
    bool        calibrated_{false};

public:
    static constexpr int kNoRotation = -1;
};
