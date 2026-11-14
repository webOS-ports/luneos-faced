// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>

#include "camera_provider.h"

#include <algorithm>
#include <sstream>

#include <glib.h>

namespace camera_provider
{
bool isProviderBinary(const std::string &path)
{
    if (path.find("camera.provider") != std::string::npos)
        return true;
    const auto slash = path.rfind('/');
    return path.substr(slash == std::string::npos ? 0 : slash + 1) == "camerahalserver";
}

namespace
{
void scanFile(const std::string &file, std::vector<std::string> &out)
{
    gchar *contents = nullptr;
    if (!g_file_get_contents(file.c_str(), &contents, nullptr, nullptr))
        return;

    std::istringstream in(contents);
    g_free(contents);

    std::string line;
    while (std::getline(in, line))
    {
        std::istringstream words(line);
        std::string keyword, name, binary;
        // "service <name> <binary> [args...]" at the start of a line, as init
        // reads it; indented lines are a section's commands or options.
        if (!(words >> keyword >> name >> binary) || line.compare(0, 8, "service ") != 0)
            continue;
        if (isProviderBinary(binary))
            out.push_back(name);
    }
}

void scanDir(const std::string &dir, std::vector<std::string> &out)
{
    GDir *d = g_dir_open(dir.c_str(), 0, nullptr);
    if (!d)
        return;
    for (const gchar *entry = g_dir_read_name(d); entry; entry = g_dir_read_name(d))
    {
        if (g_str_has_suffix(entry, ".rc"))
            scanFile(dir + "/" + entry, out);
    }
    g_dir_close(d);
}
} // namespace

std::vector<std::string> findServices(const std::vector<std::string> &dirs)
{
    std::vector<std::string> found;
    for (const auto &dir : dirs)
    {
        scanDir(dir, found);
        scanDir(dir + "/hw", found);
    }
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    return found;
}
} // namespace camera_provider
