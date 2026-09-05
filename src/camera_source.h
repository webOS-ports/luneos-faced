// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// Headless frame source over com.webos.service.camera2.
//
// We deliberately go through the camera service rather than talking to
// droidcamsrc or libcamera directly: the service already abstracts the Halium
// droid HAL plugin and the v4l2 plugin behind one API, ships on every machine,
// and arbitrates access to the device. startCamera() streams into shared memory
// without rendering a preview, which is exactly what a lockscreen matcher needs.
//
// Frame acquisition uses camera-utils: LunaClient::callSync() hands back the fd
// that getFd() attaches to its reply, and CameraSharedMemory consumes the
// buffer/signal fd pair.

#pragma once

#include <string>

#include <camera/camera_shared_memory.h>
#include <camera/luna_client.h>
#include <opencv2/core.hpp>

class CameraSource
{
public:
    explicit CameraSource(LSHandle *serviceHandle);
    ~CameraSource();

    CameraSource(const CameraSource &)            = delete;
    CameraSource &operator=(const CameraSource &) = delete;

    // Opens the front camera, sets the format and starts streaming to shared
    // memory. No preview surface is created.
    bool start(int width, int height);

    // Blocks up to timeoutMs for the next frame and converts it to BGR24.
    bool readFrameBgr(cv::Mat &out, int timeoutMs = 2000);

    void stop();

    bool streaming() const { return handle_ >= 0; }
    const std::string &lastError() const { return lastError_; }

private:
    // One attempt at bringing the camera up; start() wraps this in a retry.
    bool startOnce(int width, int height);

    // Picks the front-facing camera from getCameraList()/getInfo(). The droid
    // notifier announces devices as droid:<n> and names them "Droid front
    // camera" / "Droid back camera"; on v4l2 machines we fall back to the
    // first device.
    bool findFrontCamera(std::string &idOut);
    bool requestFd(const char *type, int &fdOut);
    bool call(const std::string &uri, const std::string &payload, std::string &reply,
              int *fd = nullptr, int timeoutMs = 5000);

    LunaClient client_;
    CameraSharedMemory shm_;

    int handle_{-1};
    int bufferFd_{-1};
    int signalFd_{-1};
    int width_{0};
    int height_{0};
    bool shmOpen_{false};
    std::string lastError_;
};
