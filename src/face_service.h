// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// com.webos.service.faceunlock - the LS2 surface.
//
// Shape deliberately mirrors com.webos.service.fingerprint so the lockscreen can
// drive both the same way. Like the fingerprint adapter this is a *sensor*
// adapter, not an authentication authority: it reports "this face matched", it
// does not decide whether the device unlocks, and it owns no retry budget. That
// policy belongs to luna-authmanager, which owns one budget across passcode,
// fingerprint and face: a recognized face is answered with the unlock token
// luna-authmanager issues for whoever asked to identify (or with
// "passcodeRequired" when it will not), every miss is reported to it, and
// enrolling takes a token from the passcode.

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <luna-service2/lunaservice.h>

#include "fart_engine.h"
#include "template_store.h"
#include "frame_orientation.h"

class FaceService
{
public:
    FaceService(LSHandle *handle, std::string modelDir, std::string stateDir);
    ~FaceService();

    bool start();

private:
    enum class State
    {
        Idle,
        Enrolling,
        Identifying
    };

    static const char *stateName(State s);

    // LS2 entry points
    static bool cbGetStatus(LSHandle *sh, LSMessage *msg, void *ctx);
    static bool cbEnroll(LSHandle *sh, LSMessage *msg, void *ctx);
    static bool cbIdentify(LSHandle *sh, LSMessage *msg, void *ctx);
    static bool cbAbort(LSHandle *sh, LSMessage *msg, void *ctx);
    static bool cbClear(LSHandle *sh, LSMessage *msg, void *ctx);

    bool beginOperation(State want, LSMessage *msg);

    // Enrolling takes the passcode: the caller's token is spent through
    // luna-authmanager before the camera starts.
    static bool cbEnrollTokenSpent(LSHandle *sh, LSMessage *reply, void *ctx);
    void startEnroll(LSHandle *sh, LSMessage *msg);

    // Main loop only: the token for a recognized face, a miss reported.
    static bool cbUnlockTokenIssued(LSHandle *sh, LSMessage *reply, void *ctx);
    void requestUnlockToken(const std::string &requester);
    void reportFailure();
    void postIdentified(const std::string &token);

    // Runs fn on the main loop, where every LS2 call is made.
    void onMainLoop(std::function<void()> fn);
    void runEnroll();
    // requester: who asked to identify; luna-authmanager binds the unlock
    // token for a match to it.
    void runIdentify(const std::string &requester);
    void finish();

    // Posts to everyone subscribed to `method`, on the main loop.
    void post(const std::string &method, const std::string &payload);
    void postStatus();

    LSHandle *handle_;
    std::string modelDir_;
    std::string stateDir_;

    FartEngine engine_;
    FrameOrientation orientation_;
    TemplateStore store_;

    std::atomic<State> state_{State::Idle};
    std::atomic<bool> cancel_{false};
    std::thread worker_;
    GMainContext *context_{nullptr};
};
