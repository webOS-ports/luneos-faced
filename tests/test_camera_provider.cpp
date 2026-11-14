// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>

#include "camera_provider.h"

#include <string>

#include <glib.h>
#include <glib/gstdio.h>
#include <gtest/gtest.h>

namespace
{
class ProviderDir : public ::testing::Test
{
protected:
    void SetUp() override
    {
        gchar *dir = g_dir_make_tmp("faced-provider-XXXXXX", nullptr);
        ASSERT_NE(dir, nullptr);
        root_ = dir;
        g_free(dir);
    }
    void TearDown() override { removeTree(root_); }

    void write(const std::string &rel, const std::string &text)
    {
        const std::string path = root_ + "/" + rel;
        gchar *parent          = g_path_get_dirname(path.c_str());
        g_mkdir_with_parents(parent, 0700);
        g_free(parent);
        ASSERT_TRUE(g_file_set_contents(path.c_str(), text.c_str(), -1, nullptr));
    }

    static void removeTree(const std::string &path)
    {
        if (GDir *d = g_dir_open(path.c_str(), 0, nullptr))
        {
            while (const gchar *e = g_dir_read_name(d))
                removeTree(path + "/" + e);
            g_dir_close(d);
            g_rmdir(path.c_str());
        }
        else
        {
            g_remove(path.c_str());
        }
    }

    std::string root_;
};
} // namespace

TEST(CameraProviderBinary, MatchesHidlAidlAndMediatek)
{
    EXPECT_TRUE(camera_provider::isProviderBinary(
        "/vendor/bin/hw/android.hardware.camera.provider@2.4-service_64"));
    EXPECT_TRUE(camera_provider::isProviderBinary(
        "/vendor/bin/hw/android.hardware.camera.provider-V1-external-service"));
    EXPECT_TRUE(camera_provider::isProviderBinary("/vendor/bin/hw/camerahalserver"));
    EXPECT_TRUE(camera_provider::isProviderBinary("camerahalserver"));
}

TEST(CameraProviderBinary, RejectsOthers)
{
    EXPECT_FALSE(camera_provider::isProviderBinary("/system/bin/cameraserver"));
    EXPECT_FALSE(camera_provider::isProviderBinary("/vendor/bin/hw/camerahalserver_debug"));
    EXPECT_FALSE(camera_provider::isProviderBinary("/vendor/bin/notcamerahalserver"));
    EXPECT_FALSE(camera_provider::isProviderBinary(""));
}

// The BraX3's vendor and sargo's, as their rc files declare them.
TEST_F(ProviderDir, FindsServicesByBinary)
{
    write("vendor/camerahalserver.rc", "service camerahalserver /vendor/bin/hw/camerahalserver\n"
                                       "    class main\n"
                                       "    user cameraserver\n");
    write("vendor/hw/android.hardware.camera.provider@2.4-service_64.rc",
          "service vendor.camera-provider-2-4 "
          "/vendor/bin/hw/android.hardware.camera.provider@2.4-service_64\n"
          "    interface android.hardware.camera.provider@2.4::ICameraProvider legacy/0\n");
    write("vendor/cameraserver.rc", "service cameraserver /system/bin/cameraserver\n");
    write("vendor/notes.txt", "service ignored /vendor/bin/hw/camerahalserver\n");

    const auto found = camera_provider::findServices({root_ + "/vendor"});
    ASSERT_EQ(found.size(), 2u);
    EXPECT_EQ(found[0], "camerahalserver");
    EXPECT_EQ(found[1], "vendor.camera-provider-2-4");
}

TEST_F(ProviderDir, IgnoresIndentedAndCommentedLines)
{
    write("vendor/a.rc", "on boot\n"
                         "    service fake /vendor/bin/hw/camerahalserver\n"
                         "# service old /vendor/bin/hw/camerahalserver\n"
                         "service\n"
                         "service lonely\n");
    EXPECT_TRUE(camera_provider::findServices({root_ + "/vendor"}).empty());
}

TEST_F(ProviderDir, MergesDirectoriesAndDropsRepeats)
{
    write("vendor/a.rc", "service camerahalserver /vendor/bin/hw/camerahalserver\n");
    write("odm/b.rc", "service camerahalserver /vendor/bin/hw/camerahalserver\n");
    const auto found =
        camera_provider::findServices({root_ + "/vendor", root_ + "/odm", root_ + "/missing"});
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0], "camerahalserver");
}

TEST_F(ProviderDir, NothingWhenTreeIsMissing)
{
    EXPECT_TRUE(camera_provider::findServices({root_ + "/absent"}).empty());
}
