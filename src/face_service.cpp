// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>

#include "face_service.h"
#include "camera_source.h"
#include "frame_orientation.h"

#include <glib.h>
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>

using json = nlohmann::json;

namespace
{
// One authentication attempt is time-boxed. Free-running detection while the
// lockscreen is up is a battery hole, and a user who is not looking at the phone
// should fail fast to the PIN path rather than hold the camera open.
constexpr int kIdentifyBudgetMs = 3000;
constexpr int kEnrollBudgetMs   = 30000;
constexpr int kFrameTimeoutMs   = 2000;
constexpr int kWidth            = 640;
constexpr int kHeight           = 480;

// How many consecutive faceless frames mean the learned sensor rotation is
// wrong rather than the user simply being out of shot. Generous on purpose: a
// wrong rotation fails every frame forever, so paying for a re-sweep now and
// then costs far less than never recovering.
constexpr int kRecalibrateAfter = 20;

struct PostJob
{
    FaceService *self;
    LSHandle *handle;
    std::string method;
    std::string payload;
    // When the worker handed this to the main loop. A recognized face that
    // takes seconds to reach the lock screen looks identical to slow
    // recognition from the outside, so measure the hop rather than guess.
    gint64 queued;
};

gboolean postOnMainLoop(gpointer data)
{
    std::unique_ptr<PostJob> job(static_cast<PostJob *>(data));
    const gint64 waited = (g_get_monotonic_time() - job->queued) / 1000;
    if (waited > 100)
        g_message("post(%s): waited %" G_GINT64_FORMAT "ms for the main loop", job->method.c_str(),
                  waited);
    LSError err;
    LSErrorInit(&err);
    if (!LSSubscriptionPost(job->handle, "/", job->method.c_str(), job->payload.c_str(), &err))
    {
        g_warning("LSSubscriptionPost(%s): %s", job->method.c_str(), err.message);
        LSErrorFree(&err);
    }
    return G_SOURCE_REMOVE;
}

std::string errorReply(const char *text)
{
    return json{{"returnValue", false}, {"errorText", text}}.dump();
}
} // namespace

const char *FaceService::stateName(State s)
{
    switch (s)
    {
    case State::Enrolling:
        return "enrolling";
    case State::Identifying:
        return "identifying";
    default:
        return "idle";
    }
}

FaceService::FaceService(LSHandle *handle, std::string modelDir, std::string stateDir)
    : handle_(handle), modelDir_(std::move(modelDir)), stateDir_(std::move(stateDir)),
      orientation_(stateDir_), store_(stateDir_), context_(g_main_context_default())
{
    orientation_.load();
}

FaceService::~FaceService()
{
    cancel_ = true;
    if (worker_.joinable())
        worker_.join();
}

bool FaceService::start()
{
    static LSMethod methods[] = {
        {"getStatus", &FaceService::cbGetStatus, LUNA_METHOD_FLAGS_NONE},
        {"enroll", &FaceService::cbEnroll, LUNA_METHOD_FLAGS_NONE},
        {"identify", &FaceService::cbIdentify, LUNA_METHOD_FLAGS_NONE},
        {"abort", &FaceService::cbAbort, LUNA_METHOD_FLAGS_NONE},
        {"clear", &FaceService::cbClear, LUNA_METHOD_FLAGS_NONE},
        {nullptr, nullptr, LUNA_METHOD_FLAGS_NONE},
    };

    LSError err;
    LSErrorInit(&err);
    if (!LSRegisterCategory(handle_, "/", methods, nullptr, nullptr, &err))
    {
        g_critical("LSRegisterCategory: %s", err.message);
        LSErrorFree(&err);
        return false;
    }
    if (!LSCategorySetData(handle_, "/", this, &err))
    {
        g_critical("LSCategorySetData: %s", err.message);
        LSErrorFree(&err);
        return false;
    }

    std::string enrollment;
    if (!store_.load(enrollment))
        g_warning("template store: %s", store_.lastError().c_str());

    if (!engine_.init(modelDir_, enrollment))
    {
        g_warning("libfart unavailable; the service will report available=false");
        return true; // still register, so callers get a clean "unavailable"
    }
    return true;
}

void FaceService::post(const std::string &method, const std::string &payload)
{
    auto *job = new PostJob{this, handle_, method, payload, g_get_monotonic_time()};
    g_main_context_invoke_full(context_, G_PRIORITY_DEFAULT, postOnMainLoop, job, nullptr);
}

void FaceService::postStatus()
{
    post("getStatus", json{{"returnValue", true},
                            {"available", engine_.ready()},
                            {"enrolled", engine_.isEnrolled()},
                            {"state", stateName(state_.load())}}
                           .dump());
}

bool FaceService::beginOperation(State want, LSMessage *msg)
{
    State expected = State::Idle;
    if (!state_.compare_exchange_strong(expected, want))
        return false;

    if (worker_.joinable())
        worker_.join();
    cancel_ = false;

    if (want == State::Enrolling)
        worker_ = std::thread([this] { runEnroll(); });
    else
        worker_ = std::thread([this] { runIdentify(); });

    postStatus();
    return true;
}

void FaceService::finish()
{
    state_ = State::Idle;
    postStatus();
}

void FaceService::runIdentify()
{
    CameraSource camera(handle_);
    if (!camera.start(kWidth, kHeight))
    {
        // Most often the camera app holds the device. Fail soft: the lockscreen
        // falls back to the passcode rather than presenting face unlock as broken.
        g_warning("identify: camera unavailable: %s", camera.lastError().c_str());
        post("identify", json{{"returnValue", false},
                               {"state", "unavailable"},
                               {"errorText", camera.lastError()}}
                              .dump());
        finish();
        return;
    }

    const gint64 deadline = g_get_monotonic_time() + kIdentifyBudgetMs * 1000;
    cv::Mat raw, bgr;
    RecognitionState last = RECOGNITION_IDLE;
    int blindFrames = 0;
    int idFrames = 0;
    const char *idDumpDir = g_getenv("FACED_DUMP_DIR");

    while (!cancel_ && g_get_monotonic_time() < deadline)
    {
        if (!camera.readFrameBgr(raw, kFrameTimeoutMs))
            break;

        // Normally already learned during enrollment; this covers a template
        // restored onto a device whose camera sits at a different angle.
        if (!orientation_.calibrated())
            orientation_.calibrate(engine_, raw);
        orientation_.apply(raw, bgr);

        last = engine_.recognize(bgr.data, bgr.cols, bgr.rows);

        // Tell the lock screen the moment frames start arriving. Until now it
        // showed "Looking for your face..." for the whole attempt, including the
        // second or so where auto-exposure is still too dark to match - which
        // reads as nothing happening even though the camera has the user in
        // frame and is working through it.
        if (idFrames == 0)
            post("identify", json{{"returnValue", true}, {"scanning", true}}.dump());

        // Same reasoning as the enroll dump: an intermittent unlock is only
        // diagnosable if we can see which frames the matcher rejected and what
        // they looked like. RECOGNITION_NOT_RECOGNIZED covers both a genuine
        // mismatch and an anti-spoof rejection, so the frame itself is the only
        // way to tell a dark or half-converged exposure from a real miss.
        // Per-frame detail is diagnostic only: a dozen lines per unlock attempt
        // would drown the journal in normal use. The summary line at the end of
        // the attempt is what stays on.
        if (idDumpDir && idFrames < 12)
        {
            const cv::Scalar m = cv::mean(bgr);
            g_message("identify: frame %d %dx%d mean=%.0f state=%d (%s)", idFrames, bgr.cols,
                      bgr.rows, (m[0] + m[1] + m[2]) / 3.0, static_cast<int>(last),
                      last == RECOGNITION_RECOGNIZED       ? "recognized"
                      : last == RECOGNITION_NO_FACE        ? "noFace"
                      : last == RECOGNITION_NOT_RECOGNIZED ? "notRecognized/spoof"
                      : last == RECOGNITION_MULTIPLE_FACES ? "multipleFaces"
                      : last == RECOGNITION_NOT_ENROLLED   ? "notEnrolled"
                                                           : "other");

            char *path = g_strdup_printf("%s/identify-%d.png", idDumpDir, idFrames);
            cv::imwrite(path, bgr);
            g_free(path);
        }
        ++idFrames;

        if (last == RECOGNITION_NO_FACE)
        {
            if (orientation_.calibrated() && ++blindFrames >= kRecalibrateAfter)
            {
                orientation_.invalidate();
                blindFrames = 0;
            }
        }
        else
        {
            blindFrames = 0;
        }
        if (last == RECOGNITION_RECOGNIZED)
        {
            const gint64 matchedAt = g_get_monotonic_time();
            post("identify", json{{"returnValue", true}, {"recognized", true}}.dump());
            camera.stop();
            g_message("identify: recognized, teardown took %" G_GINT64_FORMAT "ms",
                      (g_get_monotonic_time() - matchedAt) / 1000);
            finish();
            return;
        }
        if (last == RECOGNITION_NOT_ENROLLED)
            break;
    }

    camera.stop();

    g_message("identify: finished after %d frames, last state %d", idFrames,
              static_cast<int>(last));

    const char *reason = cancel_             ? "aborted"
                         : last == RECOGNITION_NOT_ENROLLED ? "notEnrolled"
                         : last == RECOGNITION_NO_FACE      ? "noFace"
                         : last == RECOGNITION_MULTIPLE_FACES ? "multipleFaces"
                                                              : "notRecognized";

    // NOTE: a "not recognized" here covers both a genuine mismatch and a
    // presentation-attack rejection from MiniFASNet. libfart does not
    // distinguish them, and deliberately telling an attacker which one it was
    // would be a gift, so the shell should treat both the same.
    post("identify",
         json{{"returnValue", true}, {"recognized", false}, {"reason", reason}}.dump());
    finish();
}

void FaceService::runEnroll()
{
    CameraSource camera(handle_);
    if (!camera.start(kWidth, kHeight))
    {
        g_warning("enroll: camera unavailable: %s", camera.lastError().c_str());
        post("enroll", json{{"returnValue", false}, {"errorText", camera.lastError()}}.dump());
        finish();
        return;
    }

    const gint64 deadline = g_get_monotonic_time() + kEnrollBudgetMs * 1000;
    cv::Mat raw, bgr;
    int progress = 0, lastPosted = -1;
    int blindFrames = 0;

    // Diagnostics: with FACED_DUMP_DIR set we write the first few frames exactly
    // as they are handed to libfart, plus the state it returned for each. A
    // detector that reports noFace on a frame the camera clearly filled is a
    // very different bug from one that never gets a frame at all, and the dump
    // is the only way to tell them apart from off-device.
    const char *dumpDir = g_getenv("FACED_DUMP_DIR");
    int         dumped  = 0;

    while (!cancel_ && g_get_monotonic_time() < deadline)
    {
        if (!camera.readFrameBgr(raw, kFrameTimeoutMs))
            break;

        // Enrollment is the one moment we can count on the user looking straight
        // at the camera, so it is the best chance to learn the sensor angle.
        if (!orientation_.calibrated())
            orientation_.calibrate(engine_, raw);
        orientation_.apply(raw, bgr);

        EnrollmentState st = engine_.enroll(bgr.data, bgr.cols, bgr.rows, &progress);

        // A calibrated rotation that stops yielding faces is a rotation we got
        // wrong. Throw it away and re-learn rather than failing every attempt
        // from here on.
        if (st == ENROLLMENT_NO_FACE)
        {
            if (orientation_.calibrated() && ++blindFrames >= kRecalibrateAfter)
            {
                g_message("enroll: %d frames without a face, re-learning orientation",
                          blindFrames);
                orientation_.invalidate();
                blindFrames = 0;
            }
        }
        else
        {
            blindFrames = 0;
        }

        if (dumpDir && dumped < 6)
        {
            char *path = g_strdup_printf("%s/enroll-%d.png", dumpDir, dumped);
            cv::imwrite(path, bgr);
            const cv::Scalar m = cv::mean(bgr);
            g_message("enroll: frame %d %dx%d mean(B,G,R)=(%.0f,%.0f,%.0f) state=%d -> %s", dumped,
                      bgr.cols, bgr.rows, m[0], m[1], m[2], static_cast<int>(st), path);
            g_free(path);
            ++dumped;
        }

        if (progress != lastPosted)
        {
            lastPosted = progress;
            const char *hint = st == ENROLLMENT_NO_FACE          ? "noFace"
                               : st == ENROLLMENT_MULTIPLE_FACES ? "multipleFaces"
                               : st == ENROLLMENT_BAD_LIGHTING   ? "badLighting"
                                                                 : "ok";
            post("enroll", json{{"returnValue", true},
                                 {"progress", progress},
                                 {"hint", hint},
                                 {"completed", false}}
                                .dump());
        }

        if (st == ENROLLMENT_COMPLETE)
        {
            camera.stop();
            const std::string blob = engine_.exportEnrollment();
            if (blob.empty() || !store_.save(blob))
            {
                post("enroll", json{{"returnValue", false},
                                     {"errorText", "could not persist template"}}
                                    .dump());
            }
            else
            {
                post("enroll",
                     json{{"returnValue", true}, {"progress", 100}, {"completed", true}}.dump());
            }
            finish();
            return;
        }
        if (st == ENROLLMENT_FAIL || st == ENROLLMENT_SAVE_FAILED)
            break;
    }

    camera.stop();
    post("enroll", json{{"returnValue", false},
                         {"completed", false},
                         {"errorText", cancel_ ? "aborted" : "enrollment did not complete"}}
                        .dump());
    finish();
}

bool FaceService::cbGetStatus(LSHandle *sh, LSMessage *msg, void *ctx)
{
    auto *self = static_cast<FaceService *>(ctx);
    LSError err;
    LSErrorInit(&err);

    bool subscribed = false;
    if (LSMessageIsSubscription(msg) && !LSSubscriptionProcess(sh, msg, &subscribed, &err))
    {
        g_warning("subscribe getStatus: %s", err.message);
        LSErrorFree(&err);
    }

    const std::string reply = json{{"returnValue", true},
                                   {"available", self->engine_.ready()},
                                   {"enrolled", self->engine_.isEnrolled()},
                                   {"state", stateName(self->state_.load())}}
                                  .dump();
    if (!LSMessageReply(sh, msg, reply.c_str(), &err))
        LSErrorFree(&err);
    return true;
}

bool FaceService::cbEnroll(LSHandle *sh, LSMessage *msg, void *ctx)
{
    auto *self = static_cast<FaceService *>(ctx);
    LSError err;
    LSErrorInit(&err);

    if (!self->engine_.ready())
    {
        LSMessageReply(sh, msg, errorReply("face engine unavailable").c_str(), &err);
        return true;
    }
    bool subscribed = false;
    if (LSMessageIsSubscription(msg) && !LSSubscriptionProcess(sh, msg, &subscribed, &err))
        LSErrorFree(&err);

    if (!self->beginOperation(State::Enrolling, msg))
    {
        LSMessageReply(sh, msg, errorReply("another operation is in progress").c_str(), &err);
        return true;
    }
    LSMessageReply(sh, msg, json{{"returnValue", true}, {"progress", 0}}.dump().c_str(), &err);
    return true;
}

bool FaceService::cbIdentify(LSHandle *sh, LSMessage *msg, void *ctx)
{
    auto *self = static_cast<FaceService *>(ctx);
    LSError err;
    LSErrorInit(&err);

    if (!self->engine_.ready())
    {
        LSMessageReply(sh, msg, errorReply("face engine unavailable").c_str(), &err);
        return true;
    }
    if (!self->engine_.isEnrolled())
    {
        LSMessageReply(sh, msg, errorReply("no face enrolled").c_str(), &err);
        return true;
    }
    bool subscribed = false;
    if (LSMessageIsSubscription(msg) && !LSSubscriptionProcess(sh, msg, &subscribed, &err))
        LSErrorFree(&err);

    if (!self->beginOperation(State::Identifying, msg))
    {
        LSMessageReply(sh, msg, errorReply("another operation is in progress").c_str(), &err);
        return true;
    }
    LSMessageReply(sh, msg, json{{"returnValue", true}}.dump().c_str(), &err);
    return true;
}

bool FaceService::cbAbort(LSHandle *sh, LSMessage *msg, void *ctx)
{
    auto *self = static_cast<FaceService *>(ctx);
    self->cancel_ = true;
    LSError err;
    LSErrorInit(&err);
    LSMessageReply(sh, msg, json{{"returnValue", true}}.dump().c_str(), &err);
    return true;
}

bool FaceService::cbClear(LSHandle *sh, LSMessage *msg, void *ctx)
{
    auto *self = static_cast<FaceService *>(ctx);
    LSError err;
    LSErrorInit(&err);

    if (self->state_.load() != State::Idle)
    {
        LSMessageReply(sh, msg, errorReply("an operation is in progress").c_str(), &err);
        return true;
    }

    const bool okay = self->store_.clear() && self->engine_.clearEnrollment();
    LSMessageReply(sh, msg,
                   okay ? json{{"returnValue", true}}.dump().c_str()
                        : errorReply("could not clear enrollment").c_str(),
                   &err);
    self->postStatus();
    return true;
}
