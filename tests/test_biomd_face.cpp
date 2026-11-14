// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// src/biomd_face.cpp against tests/fake_biomd.cpp on a private bus.

#include "biomd_face.h"
#include "fake_biomd.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace
{
using Clock = std::chrono::steady_clock;

// A thread running a main context. The client gets one (faced's main loop,
// with the test body as the worker thread) and the fake biomd another, as it
// would be in its own process: the client makes blocking calls from its main
// loop, which would deadlock against a fake sharing it.
class Loop
{
public:
    Loop()
    {
        context_ = g_main_context_new();
        loop_    = g_main_loop_new(context_, FALSE);
        thread_  = std::thread([this] {
            g_main_context_push_thread_default(context_);
            g_main_loop_run(loop_);
            g_main_context_pop_thread_default(context_);
        });
    }
    ~Loop()
    {
        run([this] { g_main_loop_quit(loop_); });
        thread_.join();
        // Let whatever was still queued go before the context does.
        while (g_main_context_iteration(context_, FALSE))
        {
        }
        g_main_loop_unref(loop_);
        g_main_context_unref(context_);
    }

    // Runs fn on the loop and waits for it.
    //
    // The job goes through queue_, under our own mutex, and glib is only asked
    // to wake the loop: a hand-over through g_main_context_invoke alone goes
    // through glib's locking, which TSan cannot see in an uninstrumented glib.
    void run(std::function<void()> fn)
    {
        Job job;
        job.fn = std::move(fn);
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            queue_.push_back(&job);
        }
        g_main_context_invoke(context_, &Loop::drain, this);
        std::unique_lock<std::mutex> lock(queueMutex_);
        doneCond_.wait(lock, [&] { return job.done; });
    }

private:
    struct Job
    {
        std::function<void()> fn;
        bool done{false}; // under queueMutex_
    };

    static gboolean drain(gpointer data)
    {
        auto *self = static_cast<Loop *>(data);
        for (;;)
        {
            Job *job = nullptr;
            {
                std::lock_guard<std::mutex> lock(self->queueMutex_);
                if (self->queue_.empty())
                    break;
                job = self->queue_.front();
                self->queue_.pop_front();
            }
            job->fn();
            std::lock_guard<std::mutex> lock(self->queueMutex_);
            job->done = true;
            self->doneCond_.notify_all();
        }
        return G_SOURCE_REMOVE;
    }

    GMainContext *context_;
    GMainLoop *loop_;
    std::thread thread_;
    std::mutex queueMutex_;
    std::condition_variable doneCond_;
    std::deque<Job *> queue_;
};

bool waitFor(const std::function<bool()> &pred, int ms = 3000)
{
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < end)
    {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

std::vector<uint8_t> pattern(int width, int height, uint8_t seed)
{
    std::vector<uint8_t> bytes(static_cast<size_t>(width) * height * 3);
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<uint8_t>(i * 7 + seed);
    return bytes;
}

class BiomdFaceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        bus_ = g_test_dbus_new(G_TEST_DBUS_NONE);
        g_test_dbus_up(bus_);
        address_ = g_test_dbus_get_bus_address(bus_);
        loop_    = std::make_unique<Loop>();
        biomdLoop_ = std::make_unique<Loop>();
    }

    void TearDown() override
    {
        biomdLoop_->run([this] {
            if (fake_)
                fake_->stop();
            fake_.reset();
        });
        loop_->run([this] {
            client_.reset();
            if (connection_)
            {
                g_dbus_connection_close_sync(connection_, nullptr, nullptr);
                g_object_unref(connection_);
                connection_ = nullptr;
            }
        });
        loop_.reset();
        biomdLoop_.reset();
        g_test_dbus_down(bus_);
        g_object_unref(bus_);
    }

    void startFake(std::function<void(FakeBiomd &)> setup = nullptr)
    {
        biomdLoop_->run([&] {
            fake_ = std::make_unique<FakeBiomd>(address_);
            if (setup)
                setup(*fake_);
            fake_->start();
        });
    }

    void startClient()
    {
        loop_->run([this] {
            GError *error = nullptr;
            connection_   = g_dbus_connection_new_for_address_sync(
                address_.c_str(),
                static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                  G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
                nullptr, nullptr, &error);
            ASSERT_EQ(error, nullptr);
            client_ = std::make_unique<BiomdFace>(connection_);
            ASSERT_TRUE(client_->start([this] { ++changes_; }));
        });
    }

    // Starts the fake and the client and waits until the client has seen biomd.
    void up(std::function<void(FakeBiomd &)> setup = nullptr)
    {
        startFake(std::move(setup));
        startClient();
        ASSERT_TRUE(waitFor([this] { return changes_.load() > 0; }));
    }

    BiomdFace::Result submit(uint8_t seed = 1, int waitMs = 1000)
    {
        const auto bytes = pattern(kW, kH, seed);
        return client_->submit(bytes.data(), kW, kH, waitMs);
    }

    static constexpr int kW = 8;
    static constexpr int kH = 6;

    GTestDBus *bus_{nullptr};
    std::string address_;
    std::unique_ptr<Loop> loop_;
    std::unique_ptr<Loop> biomdLoop_;
    std::unique_ptr<FakeBiomd> fake_;
    GDBusConnection *connection_{nullptr};
    std::unique_ptr<BiomdFace> client_;
    std::atomic<int> changes_{0};
};

TEST_F(BiomdFaceTest, ReportsWhatBiomdHas)
{
    up([](FakeBiomd &f) { f.setEnrolled(true); });
    EXPECT_TRUE(client_->available());
    EXPECT_TRUE(client_->enrolled());
}

TEST_F(BiomdFaceTest, BiomdWithoutFaceBackendIsUnavailable)
{
    up([](FakeBiomd &f) { f.setImplementationType(0); });
    EXPECT_FALSE(client_->available());
}

TEST_F(BiomdFaceTest, NoBiomdNoService)
{
    startClient();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(client_->available());
    std::string error;
    EXPECT_FALSE(client_->begin(BiomdFace::Mode::Recognize, error));
    EXPECT_FALSE(error.empty());
}

TEST_F(BiomdFaceTest, FramesArriveIntact)
{
    up([](FakeBiomd &f) { f.setEnrolled(true); });
    std::string error;
    ASSERT_TRUE(client_->begin(BiomdFace::Mode::Recognize, error)) << error;
    const BiomdFace::Result r = submit(42);
    EXPECT_TRUE(r.accepted);

    const auto frames = fake_->frames();
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].width, kW);
    EXPECT_EQ(frames[0].height, kH);
    EXPECT_EQ(frames[0].channels, 3);
    EXPECT_EQ(frames[0].format, 0u); // BGR
    EXPECT_EQ(frames[0].bytes, pattern(kW, kH, 42));
}

TEST_F(BiomdFaceTest, RecognitionAnswersRunOneFrameBehind)
{
    up([](FakeBiomd &f) {
        f.setEnrolled(true);
        f.script({BiomdFace::RecognitionNoFace, BiomdFace::RecognitionRecognized});
    });
    std::string error;
    ASSERT_TRUE(client_->begin(BiomdFace::Mode::Recognize, error)) << error;

    BiomdFace::Result r = submit();
    ASSERT_TRUE(r.haveState);
    EXPECT_EQ(r.state, BiomdFace::RecognitionFail); // nothing finished yet
    r = submit();
    ASSERT_TRUE(r.haveState);
    EXPECT_EQ(r.state, BiomdFace::RecognitionNoFace);
    r = submit();
    ASSERT_TRUE(r.haveState);
    EXPECT_EQ(r.state, BiomdFace::RecognitionRecognized);
    EXPECT_FALSE(r.lost);
}

TEST_F(BiomdFaceTest, EnrollmentIsDoneWhenBiomdStopsTakingFrames)
{
    up([](FakeBiomd &f) {
        f.script({BiomdFace::EnrollmentInProgress, BiomdFace::EnrollmentComplete});
    });
    ASSERT_FALSE(client_->enrolled());
    std::string error;
    ASSERT_TRUE(client_->begin(BiomdFace::Mode::Enroll, error)) << error;

    BiomdFace::Result r = submit();
    EXPECT_TRUE(r.accepted);
    EXPECT_FALSE(r.enrolled);
    EXPECT_EQ(r.state, BiomdFace::EnrollmentInProgress);
    r = submit();
    EXPECT_FALSE(r.enrolled);
    EXPECT_EQ(r.progress, 25);

    // The template is stored now, and biomd refuses the frame instead of
    // saying so.
    r = submit();
    EXPECT_FALSE(r.accepted);
    EXPECT_TRUE(r.enrolled);
    EXPECT_TRUE(client_->enrolled());
}

TEST_F(BiomdFaceTest, EnrollingOverAnEnrolledFaceNeedsItRemovedFirst)
{
    up([](FakeBiomd &f) { f.setEnrolled(true); });
    std::string error;
    EXPECT_FALSE(client_->begin(BiomdFace::Mode::Enroll, error));
    EXPECT_TRUE(client_->removeFaceData(error)) << error;
    EXPECT_FALSE(client_->enrolled());
    EXPECT_TRUE(client_->begin(BiomdFace::Mode::Enroll, error)) << error;
}

TEST_F(BiomdFaceTest, RemovingNothingIsFine)
{
    up();
    std::string error;
    EXPECT_TRUE(client_->removeFaceData(error)) << error;
    const auto calls = fake_->calls();
    EXPECT_EQ(std::count(calls.begin(), calls.end(), "RemoveFaceData"), 0);
    // The role it took for this is given back.
    EXPECT_EQ(calls.back(), "UnregisterEnrollmentAgent");
}

TEST_F(BiomdFaceTest, NoRecognitionWhileSomeoneElseEnrolls)
{
    up([](FakeBiomd &f) {
        f.setEnrolled(true);
        f.blockRecognition(true);
    });
    std::string error;
    EXPECT_FALSE(client_->begin(BiomdFace::Mode::Recognize, error));
    EXPECT_FALSE(error.empty());
    const auto calls = fake_->calls();
    EXPECT_EQ(std::count(calls.begin(), calls.end(), "StartRecognition"), 0);
    EXPECT_EQ(calls.back(), "UnregisterRecognitionAgent");
}

TEST_F(BiomdFaceTest, EndCancelsAndGivesTheRoleBack)
{
    up([](FakeBiomd &f) { f.setEnrolled(true); });
    std::string error;
    ASSERT_TRUE(client_->begin(BiomdFace::Mode::Recognize, error)) << error;
    client_->end();
    const auto calls = fake_->calls();
    ASSERT_GE(calls.size(), 2u);
    EXPECT_EQ(calls[calls.size() - 2], "Cancel");
    EXPECT_EQ(calls.back(), "UnregisterRecognitionAgent");
    // Nothing to submit to any more.
    EXPECT_TRUE(submit().lost);
}

TEST_F(BiomdFaceTest, BiomdLeavingEndsTheOperationAndComingBackWorks)
{
    up([](FakeBiomd &f) { f.setEnrolled(true); });
    std::string error;
    ASSERT_TRUE(client_->begin(BiomdFace::Mode::Recognize, error)) << error;

    biomdLoop_->run([this] { fake_->stop(); });
    ASSERT_TRUE(waitFor([this] { return !client_->available(); }));
    EXPECT_TRUE(submit().lost);
    client_->end();

    biomdLoop_->run([this] {
        fake_ = std::make_unique<FakeBiomd>(address_);
        fake_->setEnrolled(true);
        fake_->start();
    });
    ASSERT_TRUE(waitFor([this] { return client_->available(); }));
    // The old agent died with the old biomd; a new one is made.
    ASSERT_TRUE(client_->begin(BiomdFace::Mode::Recognize, error)) << error;
    EXPECT_EQ(fake_->agentsCreated(), 1);
    EXPECT_TRUE(submit().accepted);
}

TEST_F(BiomdFaceTest, InterruptWakesAWaitingSubmit)
{
    up([](FakeBiomd &f) {
        f.setEnrolled(true);
        f.setSilent(true);
    });
    std::string error;
    ASSERT_TRUE(client_->begin(BiomdFace::Mode::Recognize, error)) << error;

    std::thread aborter([this] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        client_->interrupt();
    });
    const auto start = Clock::now();
    const BiomdFace::Result r = submit(1, 10000);
    const auto took = Clock::now() - start;
    aborter.join();
    EXPECT_TRUE(r.accepted);
    EXPECT_FALSE(r.haveState);
    EXPECT_LT(took, std::chrono::seconds(5));
}

TEST_F(BiomdFaceTest, BiomdLeavingWakesAWaitingSubmit)
{
    up([](FakeBiomd &f) {
        f.setEnrolled(true);
        f.setSilent(true);
    });
    std::string error;
    ASSERT_TRUE(client_->begin(BiomdFace::Mode::Recognize, error)) << error;

    std::thread leaver([this] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        biomdLoop_->run([this] { fake_->stop(); });
    });
    const auto start = Clock::now();
    const BiomdFace::Result r = submit(1, 10000);
    const auto took = Clock::now() - start;
    leaver.join();
    EXPECT_TRUE(r.lost);
    EXPECT_LT(took, std::chrono::seconds(5));
}

TEST_F(BiomdFaceTest, AStalledBiomdOnlyCostsTheWait)
{
    up([](FakeBiomd &f) {
        f.setEnrolled(true);
        f.setSilent(true);
    });
    std::string error;
    ASSERT_TRUE(client_->begin(BiomdFace::Mode::Recognize, error)) << error;
    const BiomdFace::Result r = submit(1, 50);
    EXPECT_TRUE(r.accepted);
    EXPECT_FALSE(r.haveState);
    EXPECT_FALSE(r.lost);
}
} // namespace
