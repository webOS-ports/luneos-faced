// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// src/frame_orientation.cpp: learning the sensor angle from the matcher's
// answers.

#include "frame_orientation.h"

#include <string>

#include <glib.h>
#include <glib/gstdio.h>
#include <gtest/gtest.h>
#include <opencv2/core.hpp>

namespace
{
class FrameOrientationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        gchar *dir = g_dir_make_tmp("faced-orientation-XXXXXX", nullptr);
        ASSERT_NE(dir, nullptr);
        dir_ = dir;
        g_free(dir);
        file_ = dir_ + "/orientation";
    }
    void TearDown() override
    {
        g_remove(file_.c_str());
        g_rmdir(dir_.c_str());
    }

    bool fileExists() const { return g_file_test(file_.c_str(), G_FILE_TEST_EXISTS); }
    void writeFile(const char *text) { ASSERT_TRUE(g_file_set_contents(file_.c_str(), text, -1, nullptr)); }

    std::string dir_, file_;
};

constexpr int K = FrameOrientation::kPerCandidate;

TEST_F(FrameOrientationTest, StartsUprightAndUncalibrated)
{
    FrameOrientation o(dir_);
    o.load();
    EXPECT_FALSE(o.calibrated());
    EXPECT_EQ(o.rotation(), FrameOrientation::kNoRotation);
}

TEST_F(FrameOrientationTest, TheFirstAnswerUnderACandidateIsNotTrusted)
{
    FrameOrientation o(dir_);
    // Could be about a frame sent under the candidate before.
    EXPECT_FALSE(o.observe(true));
    EXPECT_FALSE(o.calibrated());
    EXPECT_TRUE(o.observe(true));
    EXPECT_TRUE(o.calibrated());
    EXPECT_EQ(o.rotation(), FrameOrientation::kNoRotation);
    EXPECT_TRUE(fileExists());
}

TEST_F(FrameOrientationTest, SweepsEveryRotationAndWrapsAround)
{
    FrameOrientation o(dir_);
    const int expected[] = {cv::ROTATE_90_COUNTERCLOCKWISE, cv::ROTATE_90_CLOCKWISE,
                            cv::ROTATE_180, FrameOrientation::kNoRotation};
    for (int want : expected)
    {
        for (int i = 0; i < K; ++i)
            EXPECT_FALSE(o.observe(false));
        EXPECT_EQ(o.rotation(), want);
    }
    EXPECT_FALSE(o.calibrated());
    EXPECT_FALSE(fileExists());
}

TEST_F(FrameOrientationTest, LearnsTheRotationAFaceWasSeenUnder)
{
    FrameOrientation o(dir_);
    for (int i = 0; i < 2 * K; ++i)
        o.observe(false);
    ASSERT_EQ(o.rotation(), cv::ROTATE_90_CLOCKWISE);
    o.observe(false); // settling
    EXPECT_TRUE(o.observe(true));
    EXPECT_TRUE(o.calibrated());

    FrameOrientation again(dir_);
    again.load();
    EXPECT_TRUE(again.calibrated());
    EXPECT_EQ(again.rotation(), cv::ROTATE_90_CLOCKWISE);
}

TEST_F(FrameOrientationTest, AWrongRotationIsUnlearned)
{
    writeFile("0\n"); // cv::ROTATE_90_CLOCKWISE
    FrameOrientation o(dir_);
    o.load();
    ASSERT_TRUE(o.calibrated());

    for (int i = 0; i < FrameOrientation::kRecalibrateAfter - 1; ++i)
        o.observe(false);
    // A face resets the count.
    o.observe(true);
    for (int i = 0; i < FrameOrientation::kRecalibrateAfter - 1; ++i)
        o.observe(false);
    EXPECT_TRUE(o.calibrated());

    o.observe(false);
    EXPECT_FALSE(o.calibrated());
    EXPECT_EQ(o.rotation(), FrameOrientation::kNoRotation);
    EXPECT_FALSE(fileExists());
}

TEST_F(FrameOrientationTest, IgnoresAFileItDidNotWrite)
{
    writeFile("7\n");
    FrameOrientation o(dir_);
    o.load();
    EXPECT_FALSE(o.calibrated());
    writeFile("sideways");
    o.load();
    EXPECT_FALSE(o.calibrated());
}

TEST_F(FrameOrientationTest, ApplyTurnsTheFrame)
{
    writeFile("2\n"); // cv::ROTATE_90_COUNTERCLOCKWISE
    FrameOrientation o(dir_);
    o.load();
    cv::Mat src(2, 4, CV_8UC3, cv::Scalar(1, 2, 3)), dst;
    src.at<cv::Vec3b>(0, 3) = cv::Vec3b(9, 9, 9);
    o.apply(src, dst);
    ASSERT_EQ(dst.rows, 4);
    ASSERT_EQ(dst.cols, 2);
    // Top-right ends up top-left when turned counter-clockwise.
    EXPECT_EQ(dst.at<cv::Vec3b>(0, 0), cv::Vec3b(9, 9, 9));
}
} // namespace
