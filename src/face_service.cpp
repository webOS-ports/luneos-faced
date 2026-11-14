// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>

#include "face_service.h"
#include "camera_source.h"
#include "frame_orientation.h"
#include "identify_post.h"

#include <string.h> // explicit_bzero

#include <chrono>
#include <thread>

#include <glib.h>
#include <glib/gstdio.h>
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
// How long to wait for biomd's answer after handing it a frame. It answers as
// part of taking the frame, so this is only a ceiling for a stalled biomd.
constexpr int kAnswerWaitMs     = 500;

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

constexpr const char *kIssueUri   = "luna://com.webos.service.auth/issueBiometricToken";
constexpr const char *kFailureUri = "luna://com.webos.service.auth/reportBiometricFailure";
constexpr const char *kConsumeUri = "luna://com.webos.service.auth/consumeUnlockToken";
constexpr const char *kNeedsPasscode = "Enrolling a face takes the passcode";

gboolean runOnMainLoop(gpointer data)
{
    std::unique_ptr<std::function<void()>> fn(static_cast<std::function<void()> *>(data));
    (*fn)();
    return G_SOURCE_REMOVE;
}

// A token, or a payload that carried one, is wiped before it is dropped
void wipe(std::string &text)
{
    if (!text.empty())
        explicit_bzero(&text[0], text.size());
    text.clear();
}

json parsePayload(LSMessage *msg)
{
    const char *payload = LSMessageGetPayload(msg);
    json j = json::parse(payload ? payload : "", nullptr, false);
    return j.is_object() ? j : json::object();
}

bool isTrue(const json &j, const char *key)
{
    const auto it = j.find(key);
    return it != j.end() && it->is_boolean() && it->get<bool>();
}

struct EnrollJob
{
    FaceService *self;
    LSHandle *handle;
    LSMessage *message;
};

// Whether biomd's answer means it found a face in the frame, which is what
// FrameOrientation learns the sensor angle from. A plain failure is not: it is
// also what biomd reports before it has finished any frame.
bool recognitionSawFace(uint32_t state)
{
    return state == BiomdFace::RecognitionMultipleFaces ||
           state == BiomdFace::RecognitionRecognized ||
           state == BiomdFace::RecognitionNotRecognized;
}

bool enrollmentSawFace(uint32_t state)
{
    return state == BiomdFace::EnrollmentInProgress || state == BiomdFace::EnrollmentComplete ||
           state == BiomdFace::EnrollmentMultipleFaces ||
           state == BiomdFace::EnrollmentBadLighting;
}

const char *recognitionName(uint32_t state)
{
    switch (state)
    {
    case BiomdFace::RecognitionRecognized:
        return "recognized";
    case BiomdFace::RecognitionNoFace:
        return "noFace";
    case BiomdFace::RecognitionNotRecognized:
        return "notRecognized/spoof";
    case BiomdFace::RecognitionMultipleFaces:
        return "multipleFaces";
    case BiomdFace::RecognitionNotEnrolled:
        return "notEnrolled";
    default:
        return "other";
    }
}

// Holds frames back to biomd's pace: it queues whatever it is given, and a
// queue that grows only makes every answer later.
class Pacer
{
public:
    void wait()
    {
        const auto now = std::chrono::steady_clock::now();
        if (last_ != std::chrono::steady_clock::time_point{} && now < last_ + kInterval)
            std::this_thread::sleep_for(last_ + kInterval - now);
        last_ = std::chrono::steady_clock::now();
    }

private:
    static constexpr std::chrono::milliseconds kInterval{BiomdFace::kSubmitIntervalMs};
    std::chrono::steady_clock::time_point last_{};
};
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

FaceService::FaceService(LSHandle *handle, std::string stateDir, GDBusConnection *biomdBus)
    : handle_(handle), stateDir_(std::move(stateDir)), biomd_(biomdBus), orientation_(stateDir_),
      context_(g_main_context_default())
{
    orientation_.load();
}

FaceService::~FaceService()
{
    cancel_ = true;
    biomd_.interrupt();
    if (worker_.joinable())
        worker_.join();
}

void FaceService::dropOldTemplate()
{
    // Before biomd held the face, this daemon kept the template here, encrypted
    // under a key next to it. biomd cannot import it, so it is of no use now,
    // and a face template is not something to leave lying around.
    bool removed = false;
    for (const char *name : {"templates.enc", ".key"})
    {
        const std::string path = stateDir_ + "/" + name;
        if (g_remove(path.c_str()) == 0)
            removed = true;
    }
    if (removed)
        g_message("removed the face template kept before biomd; the face needs enrolling again");
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

    dropOldTemplate();

    // biomd not running yet is fine: getStatus reports available=false until
    // it appears, so callers get a clean "unavailable".
    if (!biomd_.start([this] { postStatus(); }))
        g_warning("no system bus; the service will report available=false");
    return true;
}

void FaceService::onMainLoop(std::function<void()> fn)
{
    auto *job = new std::function<void()>(std::move(fn));
    g_main_context_invoke_full(context_, G_PRIORITY_DEFAULT, runOnMainLoop, job, nullptr);
}

struct TokenRequest
{
    FaceService *self;
    LSHandle *handle;
    LSMessage *call;
};

bool FaceService::cbUnlockTokenIssued(LSHandle *, LSMessage *reply, void *ctx)
{
    std::unique_ptr<TokenRequest> request(static_cast<TokenRequest *>(ctx));
    json j = parsePayload(reply);
    std::string token;
    const auto it = j.find("unlockToken");
    if (isTrue(j, "returnValue") && it != j.end() && it->is_string())
        token = it->get<std::string>();
    if (token.empty())
        g_message("identify: a face matched, but luna-authmanager wants the passcode");
    identify::postMatch(request->handle, request->call, token);
    LSMessageUnref(request->call);
    wipe(token);
    if (it != j.end() && it->is_string())
        wipe(it->get_ref<std::string &>());
    return true;
}

void FaceService::requestUnlockToken(LSMessage *call)
{
    const char *sender = LSMessageGetSenderServiceName(call);
    const std::string payload =
        json{{"factor", "face"}, {"requester", sender ? sender : ""}}.dump();
    auto *request = new TokenRequest{this, handle_, call};
    LSError err;
    LSErrorInit(&err);
    if (!LSCallOneReply(handle_, kIssueUri, payload.c_str(), cbUnlockTokenIssued, request,
                        nullptr, &err))
    {
        g_warning("identify: cannot ask luna-authmanager for an unlock token: %s", err.message);
        LSErrorFree(&err);
        identify::postMatch(handle_, call, std::string());
        LSMessageUnref(call);
        delete request;
    }
}

void FaceService::reportFailure()
{
    LSError err;
    LSErrorInit(&err);
    if (!LSCallOneReply(handle_, kFailureUri, R"({"factor":"face"})", nullptr, nullptr, nullptr,
                        &err))
    {
        g_warning("identify: cannot report a miss to luna-authmanager: %s", err.message);
        LSErrorFree(&err);
    }
}

void FaceService::post(const std::string &method, const std::string &payload)
{
    auto *job = new PostJob{this, handle_, method, payload, g_get_monotonic_time()};
    g_main_context_invoke_full(context_, G_PRIORITY_DEFAULT, postOnMainLoop, job, nullptr);
}

void FaceService::postStatus()
{
    post("getStatus", json{{"returnValue", true},
                            {"available", biomd_.available()},
                            {"enrolled", biomd_.enrolled()},
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
    {
        LSMessageRef(msg);
        worker_ = std::thread([this, msg] { runIdentify(msg); });
    }

    postStatus();
    return true;
}

void FaceService::finish()
{
    state_ = State::Idle;
    postStatus();
}

void FaceService::runIdentify(LSMessage *call)
{
    // Drops the reference to call on the main loop, unless it was handed on
    // to requestUnlockToken.
    struct CallRef
    {
        FaceService *self;
        LSMessage *call;
        ~CallRef()
        {
            if (call)
                self->onMainLoop([c = call] { LSMessageUnref(c); });
        }
        LSMessage *release()
        {
            LSMessage *c = call;
            call = nullptr;
            return c;
        }
    } ref{this, call};

    std::string error;
    if (!biomd_.begin(BiomdFace::Mode::Recognize, error))
    {
        g_warning("identify: biomd: %s", error.c_str());
        post("identify",
             json{{"returnValue", false}, {"state", "unavailable"}, {"errorText", error}}.dump());
        finish();
        return;
    }

    CameraSource camera(handle_);
    if (!camera.start(kWidth, kHeight))
    {
        // Most often the camera app holds the device. Fail soft: the lockscreen
        // falls back to the passcode rather than presenting face unlock as broken.
        g_warning("identify: camera unavailable: %s", camera.lastError().c_str());
        biomd_.end();
        post("identify", json{{"returnValue", false},
                               {"state", "unavailable"},
                               {"errorText", camera.lastError()}}
                              .dump());
        finish();
        return;
    }

    const gint64 deadline = g_get_monotonic_time() + kIdentifyBudgetMs * 1000;
    cv::Mat raw, bgr;
    uint32_t last = BiomdFace::RecognitionIdle;
    bool lost     = false;
    int idFrames  = 0;
    Pacer pacer;
    const char *idDumpDir = g_getenv("FACED_DUMP_DIR");

    while (!cancel_ && g_get_monotonic_time() < deadline)
    {
        pacer.wait();
        if (!camera.readFrameBgr(raw, kFrameTimeoutMs))
            break;

        // Normally already learned during enrollment; this covers a device
        // whose camera sits at an angle nothing has seen yet.
        orientation_.apply(raw, bgr);
        if (!bgr.isContinuous())
            bgr = bgr.clone();

        const BiomdFace::Result r = biomd_.submit(bgr.data, bgr.cols, bgr.rows, kAnswerWaitMs);
        if (r.lost)
        {
            lost = true;
            break;
        }

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
        // way to tell a dark or half-converged exposure from a real miss. The
        // state logged with a frame is biomd's newest answer, which may belong
        // to the frame before it.
        if (idDumpDir && idFrames < 12)
        {
            const cv::Scalar m = cv::mean(bgr);
            g_message("identify: frame %d %dx%d mean=%.0f state=%u (%s)", idFrames, bgr.cols,
                      bgr.rows, (m[0] + m[1] + m[2]) / 3.0, r.state, recognitionName(r.state));

            char *path = g_strdup_printf("%s/identify-%d.png", idDumpDir, idFrames);
            cv::imwrite(path, bgr);
            g_free(path);
        }
        ++idFrames;

        if (!r.haveState)
            continue;
        last = r.state;
        orientation_.observe(recognitionSawFace(last));

        if (last == BiomdFace::RecognitionRecognized)
        {
            const gint64 matchedAt = g_get_monotonic_time();
            // Posted once luna-authmanager has answered, with its token
            onMainLoop([this, c = ref.release()] { requestUnlockToken(c); });
            camera.stop();
            biomd_.end();
            g_message("identify: recognized, teardown took %" G_GINT64_FORMAT "ms",
                      (g_get_monotonic_time() - matchedAt) / 1000);
            finish();
            return;
        }
        if (last == BiomdFace::RecognitionNotEnrolled)
            break;
    }

    camera.stop();
    biomd_.end();

    g_message("identify: finished after %d frames, last state %u%s", idFrames, last,
              cancel_ ? " (aborted)" : lost ? " (biomd went away)" : "");

    if (lost && !cancel_)
    {
        post("identify", json{{"returnValue", false},
                               {"state", "unavailable"},
                               {"errorText", "biomd went away"}}
                              .dump());
        finish();
        return;
    }

    const char *reason = cancel_                                    ? "aborted"
                         : last == BiomdFace::RecognitionNotEnrolled  ? "notEnrolled"
                         : last == BiomdFace::RecognitionNoFace       ? "noFace"
                         : last == BiomdFace::RecognitionMultipleFaces ? "multipleFaces"
                                                                      : "notRecognized";

    // NOTE: a "not recognized" here covers both a genuine mismatch and a
    // presentation-attack rejection from MiniFASNet. libfart does not
    // distinguish them, and deliberately telling an attacker which one it was
    // would be a gift, so the shell should treat both the same.
    // A face that was seen and did not match counts against the budget
    // luna-authmanager shares with the fingerprint and the passcode.
    if (!cancel_ && last == BiomdFace::RecognitionNotRecognized)
        onMainLoop([this] { reportFailure(); });
    post("identify",
         json{{"returnValue", true}, {"recognized", false}, {"reason", reason}}.dump());
    finish();
}

void FaceService::runEnroll()
{
    std::string error;
    // A new enrollment replaces the old face. biomd keeps one and will not
    // start enrolling while it has it; the caller has just given the passcode.
    if (biomd_.enrolled() && !biomd_.removeFaceData(error))
    {
        g_warning("enroll: cannot remove the enrolled face: %s", error.c_str());
        post("enroll", json{{"returnValue", false}, {"errorText", error}}.dump());
        finish();
        return;
    }
    if (!biomd_.begin(BiomdFace::Mode::Enroll, error))
    {
        g_warning("enroll: biomd: %s", error.c_str());
        post("enroll", json{{"returnValue", false}, {"errorText", error}}.dump());
        finish();
        return;
    }

    CameraSource camera(handle_);
    if (!camera.start(kWidth, kHeight))
    {
        g_warning("enroll: camera unavailable: %s", camera.lastError().c_str());
        biomd_.end();
        post("enroll", json{{"returnValue", false}, {"errorText", camera.lastError()}}.dump());
        finish();
        return;
    }

    const gint64 deadline = g_get_monotonic_time() + kEnrollBudgetMs * 1000;
    cv::Mat raw, bgr;
    int lastPosted       = -1;
    const char *failure  = nullptr;
    Pacer pacer;

    // Diagnostics: with FACED_DUMP_DIR set we write the first few frames exactly
    // as they are handed to biomd, plus the state it answered with. A detector
    // that reports noFace on a frame the camera clearly filled is a very
    // different bug from one that never gets a frame at all, and the dump is
    // the only way to tell them apart from off-device.
    const char *dumpDir = g_getenv("FACED_DUMP_DIR");
    int dumped          = 0;

    while (!cancel_ && g_get_monotonic_time() < deadline)
    {
        pacer.wait();
        if (!camera.readFrameBgr(raw, kFrameTimeoutMs))
            break;

        // Enrollment is the one moment we can count on the user looking straight
        // at the camera, so it is the best chance to learn the sensor angle.
        orientation_.apply(raw, bgr);
        if (!bgr.isContinuous())
            bgr = bgr.clone();

        const BiomdFace::Result r = biomd_.submit(bgr.data, bgr.cols, bgr.rows, kAnswerWaitMs);

        if (dumpDir && dumped < 6)
        {
            char *path = g_strdup_printf("%s/enroll-%d.png", dumpDir, dumped);
            cv::imwrite(path, bgr);
            const cv::Scalar m = cv::mean(bgr);
            g_message("enroll: frame %d %dx%d mean(B,G,R)=(%.0f,%.0f,%.0f) state=%u -> %s", dumped,
                      bgr.cols, bgr.rows, m[0], m[1], m[2], r.state, path);
            g_free(path);
            ++dumped;
        }

        if (r.enrolled)
        {
            camera.stop();
            biomd_.end();
            post("enroll",
                 json{{"returnValue", true}, {"progress", 100}, {"completed", true}}.dump());
            finish();
            return;
        }
        if (r.lost)
        {
            failure = "biomd went away";
            break;
        }
        if (!r.haveState)
            continue;

        orientation_.observe(enrollmentSawFace(r.state));

        // biomd stored nothing; going on would only fail the same way.
        if (r.state == BiomdFace::EnrollmentSaveFailed)
        {
            failure = "could not persist template";
            break;
        }

        if (r.progress != lastPosted)
        {
            lastPosted = r.progress;
            const char *hint = r.state == BiomdFace::EnrollmentNoFace          ? "noFace"
                               : r.state == BiomdFace::EnrollmentMultipleFaces ? "multipleFaces"
                               : r.state == BiomdFace::EnrollmentBadLighting   ? "badLighting"
                                                                                : "ok";
            post("enroll", json{{"returnValue", true},
                                 {"progress", r.progress},
                                 {"hint", hint},
                                 {"completed", false}}
                                .dump());
        }
    }

    camera.stop();
    biomd_.end();
    post("enroll", json{{"returnValue", false},
                         {"completed", false},
                         {"errorText", cancel_    ? "aborted"
                                       : failure ? failure
                                                 : "enrollment did not complete"}}
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
                                   {"available", self->biomd_.available()},
                                   {"enrolled", self->biomd_.enrolled()},
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

    if (!self->biomd_.available())
    {
        LSMessageReply(sh, msg, errorReply("face engine unavailable").c_str(), &err);
        return true;
    }

    // "token": the unlockToken luna-authmanager's matchDevicePasscode gave
    // this caller. Spent there (purpose "enroll") before the camera starts.
    json request = parsePayload(msg);
    const auto it = request.find("token");
    const char *requester = LSMessageGetSenderServiceName(msg);
    if (it == request.end() || !it->is_string() || it->get_ref<const std::string &>().empty() ||
        !requester || !*requester)
    {
        LSMessageReply(sh, msg, errorReply(kNeedsPasscode).c_str(), &err);
        if (it != request.end() && it->is_string())
            wipe(it->get_ref<std::string &>());
        return true;
    }

    std::string payload = json{{"token", it->get_ref<const std::string &>()},
                               {"requester", requester},
                               {"purpose", "enroll"}}
                              .dump();
    wipe(it->get_ref<std::string &>());

    auto *job = new EnrollJob{self, sh, msg};
    LSMessageRef(msg);
    const bool called =
        LSCallOneReply(sh, kConsumeUri, payload.c_str(), cbEnrollTokenSpent, job, nullptr, &err);
    wipe(payload);
    if (!called)
    {
        g_warning("enroll: cannot reach luna-authmanager: %s", err.message);
        LSErrorFree(&err);
        LSMessageReply(sh, msg, errorReply(kNeedsPasscode).c_str(), nullptr);
        LSMessageUnref(msg);
        delete job;
    }
    return true;
}

bool FaceService::cbEnrollTokenSpent(LSHandle *, LSMessage *reply, void *ctx)
{
    std::unique_ptr<EnrollJob> job(static_cast<EnrollJob *>(ctx));
    const json j = parsePayload(reply);

    if (isTrue(j, "returnValue") && isTrue(j, "valid"))
        job->self->startEnroll(job->handle, job->message);
    else
        LSMessageReply(job->handle, job->message, errorReply(kNeedsPasscode).c_str(), nullptr);

    LSMessageUnref(job->message);
    return true;
}

void FaceService::startEnroll(LSHandle *sh, LSMessage *msg)
{
    LSError err;
    LSErrorInit(&err);

    bool subscribed = false;
    if (LSMessageIsSubscription(msg) && !LSSubscriptionProcess(sh, msg, &subscribed, &err))
        LSErrorFree(&err);

    if (!beginOperation(State::Enrolling, msg))
    {
        LSMessageReply(sh, msg, errorReply("another operation is in progress").c_str(), nullptr);
        return;
    }
    LSMessageReply(sh, msg, json{{"returnValue", true}, {"progress", 0}}.dump().c_str(), nullptr);
}

bool FaceService::cbIdentify(LSHandle *sh, LSMessage *msg, void *ctx)
{
    auto *self = static_cast<FaceService *>(ctx);
    LSError err;
    LSErrorInit(&err);

    if (!self->biomd_.available())
    {
        LSMessageReply(sh, msg, errorReply("face engine unavailable").c_str(), &err);
        return true;
    }
    if (!self->biomd_.enrolled())
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
    self->biomd_.interrupt();
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

    // Blocks the main loop for a few calls to biomd; clear is rare and biomd
    // answers at once.
    std::string error;
    const bool okay = self->biomd_.removeFaceData(error);
    if (!okay)
        g_warning("clear: %s", error.c_str());
    LSMessageReply(sh, msg,
                   okay ? json{{"returnValue", true}}.dump().c_str()
                        : errorReply("could not clear enrollment").c_str(),
                   &err);
    self->postStatus();
    return true;
}
