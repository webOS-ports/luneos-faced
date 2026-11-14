// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// Finds the Android init service that hosts the vendor camera provider, so it
// can be restarted through "setprop ctl.restart <service>".
//
// The name differs per vendor - sargo declares vendor.camera-provider-2-4,
// MediaTek vendors (the BraX3's among them) run the provider inside
// camerahalserver - so it is read from the container's init scripts instead of
// assumed. Matched on the binary, like camera-droid-heal.sh in gst-droid.

#pragma once

#include <string>
#include <vector>

namespace camera_provider
{
// Whether an init "service" line's binary is a camera provider host:
// android.hardware.camera.provider@*-service*, an AIDL provider, or
// MediaTek's camerahalserver.
bool isProviderBinary(const std::string &path);

// The service names, sorted and without repeats, from the *.rc files in each
// of dirs and its hw/ subdirectory. Missing directories are skipped.
std::vector<std::string> findServices(const std::vector<std::string> &dirs);
} // namespace camera_provider
