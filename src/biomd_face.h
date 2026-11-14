// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// Client for biomd's face half, io.FuriOS.Biomd.Face on the system bus.
//
// biomd owns the face matcher and the enrolled template, the way it owns the
// fingerprint HAL for webos-fingerprint-adapter: it runs libfart (detection,
// MiniFASNet presentation-attack detection, MobileFaceNet match) and keeps the
// template encrypted under /var/lib/biomd. It does no capture of its own. A
// client creates an agent, registers it for enrollment or recognition, starts
// the operation and hands frames over one memfd at a time with SubmitFrame.
//
// Two things about how biomd answers shape this class:
//
//  - SubmitFrame queues the frame and returns at once. The state it reports
//    for it (RecognitionStateChanged / EnrollmentStateChanged on the agent) is
//    the result of the newest frame biomd has finished, not of the one just
//    submitted, so results run a frame or so behind and only arrive while
//    frames keep coming. Frames are paced (kSubmitIntervalMs) so the queue
//    behind the matcher does not grow.
//
//  - A finished enrollment is never reported as a state. Once the template is
//    stored biomd rejects the next SubmitFrame, because a face is enrolled now;
//    FaceEnrolled turning true is the completion.
//
// Threading: start() runs on the main loop and subscribes there, so signals
// and the name watch are dispatched on the main loop. The operations (begin,
// submit, end, removeFaceData) are blocking calls meant for the worker thread;
// they wait on state the main loop fills in. available() and enrolled() are
// safe from any thread.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include <gio/gio.h>

class BiomdFace
{
public:
    // The values biomd passes through from libfart's fart_enums.h. Kept here
    // so this daemon does not need libfart at all, not even its headers.
    enum Recognition : uint32_t
    {
        RecognitionIdle          = 0,
        RecognitionFail          = 1,
        RecognitionNoFace        = 2,
        RecognitionMultipleFaces = 3,
        RecognitionNotEnrolled   = 4,
        RecognitionRecognized    = 5,
        RecognitionNotRecognized = 6,
    };
    enum Enrollment : uint32_t
    {
        EnrollmentIdle          = 0,
        EnrollmentFail          = 1,
        EnrollmentInProgress    = 2,
        EnrollmentComplete      = 3,
        EnrollmentSaveFailed    = 4,
        EnrollmentMultipleFaces = 5,
        EnrollmentNoFace        = 6,
        EnrollmentBadLighting   = 7,
    };

    enum class Mode
    {
        Enroll,
        Recognize
    };

    // What submit() learned.
    struct Result
    {
        // biomd took the frame.
        bool accepted{false};
        // A state arrived for this submission (see the class comment: it is
        // the newest finished frame's state, not necessarily this one's).
        bool haveState{false};
        uint32_t state{0};
        // Enrollment progress, 0-100, the newest biomd reported.
        int progress{0};
        // Enroll mode only: a face is enrolled now, the enrollment is done.
        bool enrolled{false};
        // biomd went away or the agent lost its access; the operation is over.
        bool lost{false};
    };

    // Called whenever available() or enrolled() may have changed: on the main
    // loop, or on the worker when an enrollment it runs completes.
    using ChangedFn = std::function<void()>;

    // connection: the bus biomd is on. nullptr means the system bus. The
    // caller's reference is not taken over; BiomdFace takes its own.
    explicit BiomdFace(GDBusConnection *connection = nullptr);
    ~BiomdFace();

    BiomdFace(const BiomdFace &)            = delete;
    BiomdFace &operator=(const BiomdFace &) = delete;

    // Main loop. Connects, subscribes and starts watching for biomd. Returns
    // false only when there is no bus at all; biomd not running yet is fine.
    bool start(ChangedFn changed);

    // biomd is on the bus and has a working face backend (ImplementationType
    // is not 0; a biomd built without the face matcher reports 0).
    bool available() const { return available_.load(); }
    bool enrolled() const { return enrolled_.load(); }

    // Worker. Registers the agent for mode and starts the operation.
    bool begin(Mode mode, std::string &error);

    // Worker. Hands one tightly packed BGR24 frame over, then waits up to
    // waitMs for biomd's answer.
    Result submit(const uint8_t *bgr, int width, int height, int waitMs);

    // Worker. Cancels the operation and drops the agent's role. Safe to call
    // when begin() failed or was never called.
    void end();

    // Worker. Removes the enrolled face. Nothing enrolled counts as success.
    bool removeFaceData(std::string &error);

    // Wakes a submit() that is waiting, for an abort.
    void interrupt();

    static constexpr int kSubmitIntervalMs = 150;

private:
    static void onSignal(GDBusConnection *, const gchar *sender, const gchar *path,
                         const gchar *iface, const gchar *member, GVariant *params,
                         gpointer data);
    static void onAppeared(GDBusConnection *, const gchar *name, const gchar *owner,
                           gpointer data);
    static void onVanished(GDBusConnection *, const gchar *name, gpointer data);

    void refreshProperties();
    GVariant *getProperty(const char *name);
    void setEnrolled(bool value);
    void notifyChanged();

    // Returns the agent path, creating the agent first if there is none.
    bool ensureAgent(std::string &pathOut, std::string &error);
    GVariant *callManager(const char *method, GVariant *params, const GVariantType *reply,
                          std::string &error);
    GVariant *callAgent(const std::string &path, const char *method, GVariant *params,
                        const GVariantType *reply, std::string &error,
                        GUnixFDList *fds = nullptr);
    bool callAgentBool(const std::string &path, const char *method, std::string &error);
    bool registerRole(Mode mode, const std::string &path, std::string &error);
    void unregisterRole(Mode mode);

    GDBusConnection *connection_{nullptr};
    guint watchId_{0};
    guint agentSub_{0};
    guint managerSub_{0};
    ChangedFn changed_;

    std::atomic<bool> available_{false};
    std::atomic<bool> enrolled_{false};

    // Everything below is shared between the main loop (signals, the name
    // watch) and the worker, under mutex_.
    mutable std::mutex mutex_;
    std::condition_variable cond_;
    std::string owner_;     // biomd's unique name, empty while it is gone
    std::string agentPath_; // our agent, empty until created
    bool roleHeld_{false};
    Mode role_{Mode::Recognize};
    uint64_t stateSeq_{0};
    uint32_t lastState_{0};
    int lastProgress_{0};
    bool accessLost_{false};
    bool interrupted_{false};
    uint64_t ownerSeq_{0};
};
