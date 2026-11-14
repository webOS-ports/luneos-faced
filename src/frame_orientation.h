// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// Works out which way up this device's camera delivers frames.
//
// Sensors are mounted at whatever angle the OEM found convenient, and nothing
// in the webOS camera service reports it: getInfo() gives resolutions and a
// name, no orientation. On sargo the front sensor arrives rotated 90 degrees
// clockwise, so the user appears lying on their side and libfart - which only
// finds upright faces - answers NO_FACE on frames that are otherwise perfectly
// exposed and framed. Hardcoding sargo's angle would just move the bug to the
// next device, so instead the matcher's own answers decide: while no rotation
// is known, frames go out turned each way in turn, and the first rotation biomd
// finds a face in is kept, and remembered on disk.
//
// biomd answers a frame or so late (see BiomdFace), so a rotation only counts
// an answer once it has had kSettle answers to flush out the frames that went
// out under the rotation before it.

#pragma once

#include <opencv2/core.hpp>
#include <string>

class FrameOrientation
{
public:
    explicit FrameOrientation(std::string stateDir);

    // Reads back a previously learned rotation, if there is one.
    void load();

    bool calibrated() const { return calibrated_; }

    // The rotation the next frame should be turned by: the learned one, or
    // while sweeping, the candidate being tried.
    int rotation() const { return rotation_; }

    // Turns a raw camera frame by rotation().
    void apply(const cv::Mat &src, cv::Mat &dst) const;

    // One answer from the matcher. faceSeen: it found a face (anything but
    // "no face" and plain failures). While sweeping, a face seen under the
    // candidate settles it; otherwise kPerCandidate answers without one move
    // on to the next. Once settled, kRecalibrateAfter answers in a row without
    // a face mean the learned rotation was wrong, and the sweep starts again.
    // Returns true when this answer settled a rotation.
    bool observe(bool faceSeen);

    // Forgets the learned rotation and starts a sweep.
    void invalidate();

    // Human-readable current rotation, for logs.
    const char *name() const;

    static constexpr int kNoRotation = -1;
    // Answers to ignore after switching candidates, for the lag.
    static constexpr int kSettle = 1;
    // Answers to give one candidate, kSettle included. A few, because the
    // first frames after the camera starts are often too dark to find
    // anything in, whatever the rotation.
    static constexpr int kPerCandidate = 4;
    // Generous on purpose: a wrong rotation fails every frame forever, so a
    // re-sweep now and then costs far less than never recovering.
    static constexpr int kRecalibrateAfter = 20;

private:
    bool save() const;
    void startSweep();

    std::string path_;
    int rotation_{kNoRotation};
    bool calibrated_{false};
    // Sweep state: which of the candidates, and answers seen under it.
    int candidate_{0};
    int answers_{0};
    // Settled: answers in a row without a face.
    int blind_{0};
};
