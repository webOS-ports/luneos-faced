// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// Just enough of biomd's io.FuriOS.Biomd.Face for src/biomd_face.cpp, behaving
// the way biomd's face.c and face_tensorflow_fart.c do where it matters:
//
//  - SubmitFrame answers with the state of the newest finished frame, then
//    "finishes" the submitted one, so answers lag one frame;
//  - Start* reset that state (IN_PROGRESS for enrollment, FAIL for
//    recognition);
//  - once an enrollment completes, FaceEnrolled is true and further enrollment
//    frames are refused, with no state;
//  - a recognition agent has access only while no enrollment agent is
//    registered.
//
// Runs on whatever main context is thread-default when start() is called.

#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <gio/gio.h>

class FakeBiomd
{
public:
    explicit FakeBiomd(const std::string &address);
    ~FakeBiomd();

    // Registers the objects and takes the bus name.
    void start();
    // Drops the bus name and every agent, as a biomd that exits.
    void stop();

    // The states the next frames finish with, in order. When it runs out,
    // frames finish with fallback().
    void script(std::deque<uint32_t> states);
    void setFallback(uint32_t state);
    void setEnrolled(bool enrolled);
    void setImplementationType(int type);
    // Registers an enrollment agent for someone else, which takes recognition
    // access away from everyone.
    void blockRecognition(bool block);
    // Takes frames without answering, as a stalled biomd.
    void setSilent(bool silent)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        silent_ = silent;
    }

    // What arrived, for the tests to look at.
    struct Frame
    {
        int width, height, channels;
        uint32_t format;
        std::vector<uint8_t> bytes;
    };
    std::vector<Frame> frames();
    std::vector<std::string> calls();
    int agentsCreated();

private:
    struct Agent
    {
        std::string path;
        std::string owner;
        guint regId{0};
        int mode{0}; // 0 none, 1 enroll, 2 recognize
    };

    static void onManagerCall(GDBusConnection *, const gchar *sender, const gchar *path,
                              const gchar *iface, const gchar *method, GVariant *params,
                              GDBusMethodInvocation *inv, gpointer self);
    static GVariant *onManagerGet(GDBusConnection *, const gchar *sender, const gchar *path,
                                  const gchar *iface, const gchar *prop, GError **error,
                                  gpointer self);
    static void onAgentCall(GDBusConnection *, const gchar *sender, const gchar *path,
                            const gchar *iface, const gchar *method, GVariant *params,
                            GDBusMethodInvocation *inv, gpointer self);
    static GVariant *onAgentGet(GDBusConnection *, const gchar *sender, const gchar *path,
                                const gchar *iface, const gchar *prop, GError **error,
                                gpointer self);

    Agent *agentAt(const std::string &path);
    bool hasAccess(const Agent *agent) const;
    void emitAgent(const Agent &agent, const char *member, GVariant *params);
    void emitEnrolled();

    std::string address_;
    GDBusConnection *connection_{nullptr};
    GDBusNodeInfo *managerInfo_{nullptr};
    GDBusNodeInfo *agentInfo_{nullptr};
    guint managerReg_{0};

    std::mutex mutex_;
    std::vector<Agent *> agents_;
    unsigned nextAgent_{0};
    int created_{0};
    std::string enrollAgent_, recogAgent_;
    bool blockRecognition_{false};
    bool silent_{false};
    bool enrolled_{false};
    int implementationType_{1};
    uint32_t lastState_{0};
    int progress_{0};
    std::deque<uint32_t> script_;
    uint32_t fallback_{2};
    std::vector<Frame> frames_;
    std::vector<std::string> calls_;
};
