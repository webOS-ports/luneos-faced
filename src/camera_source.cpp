// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>

#include "camera_source.h"

#include <cerrno>
#include <glib.h>
#include <nlohmann/json.hpp>
#include <opencv2/imgproc.hpp>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using json = nlohmann::json;

namespace
{
constexpr const char *kCameraService = "luna://com.webos.service.camera2";
constexpr const char *kAppId         = "com.webos.service.faceunlock";
// LunaClient::callSync takes its timeout in MILLISECONDS, despite the header
// defaulting it to 30 - which reads like seconds. It feeds the value straight to
// LSCallSetTimeout (ms) and, when called off the main context, waits in a
// g_usleep(1000) loop for that many iterations. Passing "5" here does not mean
// five seconds, it means five milliseconds, and every call fails while the
// camera service is answering correctly.
constexpr int kCallTimeoutMs         = 5000;
// open() cold-starts the vendor camera HAL through droidcamsrc, which takes far
// longer than an ordinary call the first time. Giving up early is not harmless:
// the service still completes the open, so we leak a handle we can no longer
// close, and every later attempt fails with "Already another device opened as
// primary" until the camera service is restarted.
constexpr int kOpenTimeoutMs         = 25000;
// startCamera builds and rolls the vendor GStreamer pipeline (droidcamsrc ->
// PLAYING), which is the slowest step of the lot on a cold camera. Same
// reasoning as open(): time out early and the service still completes it,
// leaving a stream we never stopped.
constexpr int kStartTimeoutMs        = 25000;

bool ok(const json &j) { return j.value("returnValue", false); }

// A reply that arrived is not a reply that succeeded. Without this a failed
// startCamera fell through to getFd, which then reported the far less useful
// "Shmemory is not used for this session" instead of the real cause.
bool replyOk(const std::string &reply, std::string &errOut)
{
    try
    {
        json j = json::parse(reply);
        if (ok(j))
            return true;
        errOut = j.value("errorText", reply);
        if (j.contains("errorCode"))
            errOut += " (code " + std::to_string(j["errorCode"].get<int>()) + ")";
    }
    catch (const std::exception &e)
    {
        errOut = std::string("unparseable reply: ") + e.what();
    }
    return false;
}

// The sargo vendor camera HAL provider (android.hardware.camera.provider@2.4)
// never releases the image sensor when a session ends. The kernel release path
// fails ("cam_destroy_hdl: Invalid idx 255", "cam_sensor_shutdown: failed
// destroying dhdl"), so the sensor stays acquired and every later acquire is
// rejected by the driver with "Not in right state to aquire". The visible
// effect is that exactly one camera session succeeds per provider lifetime and
// every attempt after it dies in CamX with "CreateUsecaseObject failed" /
// "configureStreamsLocked: ... Function not implemented (-38)".
//
// Restarting the provider through Android init drops the stuck handle. The
// respawn itself takes ~1s but the HAL cannot serve an open until ~3s in;
// restarting with a shorter settle reproducibly fails to reach PLAYING.
// Two goes only: each one pays the provider restart and its settle, so a third
// would push a failing unlock past the point where the user has given up and
// reached for the PIN.
constexpr int kStartAttempts     = 2;
constexpr int kStartRetryPauseUs = 500000;

constexpr const char *kProviderInitService = "vendor.camera-provider-2-4";
constexpr int         kProviderSettleUs    = 3000000;

// The vendor HAL is a single global resource while a CameraSource is created per
// attempt, so what we know about the provider has to outlive the object.
gint64 g_providerRestartedAt = 0;
bool   g_providerPrewarmed   = false;

void spawnProviderRestart(bool synchronous)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        // Double fork for the asynchronous case: the grandchild is reparented to
        // init, so nobody has to come back and reap it later from a thread that
        // may well be gone by then.
        if (!synchronous && fork() != 0)
            _exit(0);

        // Same fd hygiene as the camera service's HAL child: anything we leak
        // across the exec is a descriptor the helper can never close.
        for (int fd = 3; fd < 256; ++fd)
            close(fd);
        execl("/android/system/bin/setprop", "setprop", "ctl.restart", kProviderInitService,
              static_cast<char *>(nullptr));
        _exit(127);
    }
    if (pid < 0)
    {
        g_warning("camera: fork() for provider restart failed");
        return;
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
}

// Called once a session has finished with the provider. Restarting it now, off
// the critical path, means the next unlock attempt can usually open straight
// away instead of waiting out the full settle - which was several seconds of
// the lock screen apparently doing nothing at all.
void prewarmVendorCameraProvider()
{
    spawnProviderRestart(false);
    g_providerRestartedAt = g_get_monotonic_time();
    g_providerPrewarmed   = true;
}

void restartVendorCameraProvider()
{
    gint64 waitUs = kProviderSettleUs;

    if (g_providerPrewarmed)
    {
        // The restart already happened when the last session ended; wait out
        // only whatever is left of the settle, usually nothing.
        const gint64 elapsed = g_get_monotonic_time() - g_providerRestartedAt;
        waitUs = elapsed >= kProviderSettleUs ? 0 : kProviderSettleUs - elapsed;
        g_message("camera: provider pre-warmed, %dms of settle left", static_cast<int>(waitUs / 1000));
    }
    else
    {
        spawnProviderRestart(true);
        g_providerRestartedAt = g_get_monotonic_time();
        g_message("camera: restarted %s, settling %dms", kProviderInitService,
                  kProviderSettleUs / 1000);
    }

    g_providerPrewarmed = false;
    if (waitUs > 0)
        g_usleep(waitUs);
}
} // namespace

CameraSource::CameraSource(LSHandle *serviceHandle) : client_(serviceHandle) {}

CameraSource::~CameraSource() { stop(); }

bool CameraSource::call(const std::string &uri, const std::string &payload, std::string &reply,
                        int *fd, int timeoutMs)
{
    if (!client_.callSync(uri.c_str(), payload.c_str(), &reply, timeoutMs, fd))
    {
        lastError_ = "call failed: " + uri;
        return false;
    }
    return true;
}

bool CameraSource::findFrontCamera(std::string &idOut)
{
    std::string reply;
    if (!call(std::string(kCameraService) + "/getCameraList", "{}", reply))
        return false;

    json list;
    try
    {
        list = json::parse(reply);
    }
    catch (const std::exception &e)
    {
        lastError_ = std::string("getCameraList parse: ") + e.what();
        return false;
    }

    if (!ok(list) || !list.contains("deviceList") || list["deviceList"].empty())
    {
        lastError_ = "no cameras reported";
        return false;
    }

    std::string first;
    for (const auto &entry : list["deviceList"])
    {
        // deviceList entries are ids ("camera1") on some releases and objects
        // carrying the id on others; accept both.
        std::string id = entry.is_string() ? entry.get<std::string>()
                                           : entry.value("id", entry.value("deviceId", ""));
        if (id.empty())
            continue;
        if (first.empty())
            first = id;

        std::string info;
        if (!call(std::string(kCameraService) + "/getInfo", json{{"id", id}}.dump(), info))
            continue;

        // "Droid front camera" from the droid notifier; v4l2 devices carry
        // whatever the UVC descriptor says.
        std::string lowered = info;
        for (auto &c : lowered)
            c = static_cast<char>(::tolower(c));
        if (lowered.find("front") != std::string::npos)
        {
            idOut = id;
            g_message("front camera: %s", id.c_str());
            return true;
        }
    }

    if (first.empty())
    {
        lastError_ = "no usable camera id";
        return false;
    }

    // Nothing self-identified as front-facing. On Halium the droid notifier
    // enumerates back as 0 and front as 1, so prefer the second entry.
    idOut = first;
    if (list["deviceList"].size() > 1)
    {
        const auto &second = list["deviceList"][1];
        std::string id     = second.is_string() ? second.get<std::string>()
                                                : second.value("id", second.value("deviceId", ""));
        if (!id.empty())
            idOut = id;
    }
    g_warning("no camera self-identified as front-facing, using %s", idOut.c_str());
    return true;
}

bool CameraSource::requestFd(const char *type, int &fdOut)
{
    std::string reply;
    int fd = -1;
    if (!call(std::string(kCameraService) + "/getFd",
              json{{"handle", handle_}, {"type", type}}.dump(), reply, &fd))
        return false;

    std::string err;
    if (!replyOk(reply, err))
    {
        lastError_ = std::string("getFd(") + type + ") failed: " + err;
        return false;
    }
    if (fd < 0)
    {
        lastError_ = std::string("getFd(") + type + ") returned no descriptor";
        return false;
    }
    fdOut = fd;
    return true;
}

// Every session tears the vendor HAL down and brings it back up (see
// restartVendorCameraProvider), and doing that back to back races: the camera
// service can still be unwinding the previous session while we open the next
// one. It surfaces as one of several unrelated-looking errors - "startCamera
// failed (code -1)", "getFd(buffer) returned no descriptor", even a failed
// getCameraList - and a single one of them used to fail the whole unlock and
// burn one of the user's five attempts. That is what made face unlock work only
// every other time. A retry costs one more provider restart and turns most of
// them into successes.
bool CameraSource::start(int width, int height)
{
    for (int attempt = 1; attempt <= kStartAttempts; ++attempt)
    {
        if (startOnce(width, height))
            return true;

        if (attempt < kStartAttempts)
        {
            g_message("camera: start failed (%s), retrying %d/%d", lastError_.c_str(), attempt + 1,
                      kStartAttempts);
            // Let the service finish unwinding before we restart the HAL again.
            stop();
            g_usleep(kStartRetryPauseUs);
        }
    }
    return false;
}

bool CameraSource::startOnce(int width, int height)
{
    stop();
    lastError_.clear();

    // Must happen before every open, not just the first: see
    // restartVendorCameraProvider().
    restartVendorCameraProvider();

    std::string id;
    if (!findFrontCamera(id))
        return false;

    std::string reply;
    if (!call(std::string(kCameraService) + "/open",
              json{{"id", id}, {"mode", "primary"}, {"appId", kAppId}}.dump(), reply, nullptr,
              kOpenTimeoutMs))
        return false;

    json j;
    try
    {
        j = json::parse(reply);
    }
    catch (const std::exception &e)
    {
        lastError_ = std::string("open parse: ") + e.what();
        return false;
    }
    if (!ok(j) || !j.contains("handle"))
    {
        // Most likely another client already holds the device - the camera app
        // in the foreground when the screen locked. Caller falls back to PIN.
        lastError_ = "open failed: " + reply;
        return false;
    }
    handle_ = j["handle"].get<int>();
    width_  = width;
    height_ = height;

    // The droid HAL plugin forces NV21 whatever we ask for; v4l2 devices honour
    // the request. readFrameBgr() works out which it got from the frame size.
    if (!call(std::string(kCameraService) + "/setFormat",
              json{{"handle", handle_},
                   {"params",
                    {{"width", width}, {"height", height}, {"format", "YUV"}, {"fps", 30}}}}
                  .dump(),
              reply, nullptr, kStartTimeoutMs))
    {
        stop();
        return false;
    }
    if (!replyOk(reply, lastError_))
    {
        lastError_ = "setFormat failed: " + lastError_;
        stop();
        return false;
    }

    if (!call(std::string(kCameraService) + "/startCamera",
              json{{"handle", handle_}, {"params", json::object()}}.dump(), reply, nullptr,
              kStartTimeoutMs))
    {
        stop();
        return false;
    }
    if (!replyOk(reply, lastError_))
    {
        lastError_ = "startCamera failed: " + lastError_;
        stop();
        return false;
    }

    g_message("camera: open/setFormat/startCamera ok (handle %d), fetching fds", handle_);

    if (!requestFd("buffer", bufferFd_) || !requestFd("signal", signalFd_))
    {
        stop();
        return false;
    }

    g_message("camera: got buffer fd %d, signal fd %d", bufferFd_, signalFd_);

    if (!shm_.open(bufferFd_, signalFd_))
    {
        lastError_ = "CameraSharedMemory::open failed";
        stop();
        return false;
    }
    shmOpen_ = true;

    g_message("camera streaming %dx%d (handle %d)", width, height, handle_);
    return true;
}

bool CameraSource::readFrameBgr(cv::Mat &out, int timeoutMs)
{
    if (!shmOpen_)
    {
        lastError_ = "not streaming";
        return false;
    }

    unsigned char *data = nullptr, *meta = nullptr, *extra = nullptr, *solution = nullptr;
    size_t dataSize = 0, metaSize = 0, extraSize = 0, solutionSize = 0;

    if (!shm_.read(&data, &dataSize, &meta, &metaSize, &extra, &extraSize, &solution,
                   &solutionSize, timeoutMs) ||
        !data || dataSize == 0)
    {
        lastError_ = "no frame within timeout";
        return false;
    }

    const size_t pixels = static_cast<size_t>(width_) * static_cast<size_t>(height_);

    if (dataSize >= pixels * 3 / 2 && dataSize < pixels * 2)
    {
        // NV21: full-res Y plane followed by interleaved VU at half resolution.
        cv::Mat yuv(height_ + height_ / 2, width_, CV_8UC1, data);
        cv::cvtColor(yuv, out, cv::COLOR_YUV2BGR_NV21);
        return true;
    }
    if (dataSize >= pixels * 2)
    {
        cv::Mat yuy2(height_, width_, CV_8UC2, data);
        cv::cvtColor(yuy2, out, cv::COLOR_YUV2BGR_YUY2);
        return true;
    }

    lastError_ = "unexpected frame size " + std::to_string(dataSize) + " for " +
                 std::to_string(width_) + "x" + std::to_string(height_);
    return false;
}

void CameraSource::stop()
{
    /* stop() runs on the failure path as well as the normal one, and the calls
     * it makes go through call(), which writes lastError_. Without this the
     * teardown's own error replaces the one the caller actually needs - a
     * failure in getFd would get reported as "call failed: .../close". */
    const std::string keepError = lastError_;

    if (shmOpen_)
    {
        shm_.close();
        shmOpen_ = false;
    }
    // CameraSharedMemory owns neither fd; the service dup'd them onto the reply.
    for (int *fd : {&bufferFd_, &signalFd_})
    {
        if (*fd >= 0)
        {
            ::close(*fd);
            *fd = -1;
        }
    }

    const bool usedCamera = handle_ >= 0;

    if (handle_ >= 0)
    {
        std::string reply;
        call(std::string(kCameraService) + "/stopCamera", json{{"handle", handle_}}.dump(), reply);
        call(std::string(kCameraService) + "/close", json{{"handle", handle_}}.dump(), reply);
        handle_ = -1;
    }

    // Only after a session that actually opened the camera: that is what leaves
    // the sensor stuck, and stop() also runs on paths that never got that far.
    if (usedCamera)
        prewarmVendorCameraProvider();

    if (!keepError.empty())
        lastError_ = keepError;
}
