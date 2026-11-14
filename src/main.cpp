// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>

#include <cstdlib>
#include <glib.h>
#include <glib-unix.h>
#include <luna-service2/lunaservice.h>

#include "face_service.h"

namespace
{
constexpr const char *kServiceName = "com.webos.service.faceunlock";
constexpr const char *kStateDir    = "/var/lib/luneos-faced";

GMainLoop *g_loop = nullptr;

gboolean onSignal(gpointer)
{
    if (g_loop)
        g_main_loop_quit(g_loop);
    return G_SOURCE_REMOVE;
}
} // namespace

int main(int, char **)
{
    LSError err;
    LSErrorInit(&err);

    g_loop = g_main_loop_new(nullptr, FALSE);

    LSHandle *handle = nullptr;
    if (!LSRegister(kServiceName, &handle, &err))
    {
        g_critical("LSRegister(%s): %s", kServiceName, err.message);
        LSErrorFree(&err);
        return EXIT_FAILURE;
    }

    if (!LSGmainAttach(handle, g_loop, &err))
    {
        g_critical("LSGmainAttach: %s", err.message);
        LSErrorFree(&err);
        return EXIT_FAILURE;
    }

    // biomd holds the template and the models; all that is ours is the
    // learned camera orientation.
    FaceService service(handle, kStateDir);
    if (!service.start())
        return EXIT_FAILURE;

    g_unix_signal_add(SIGTERM, onSignal, nullptr);
    g_unix_signal_add(SIGINT, onSignal, nullptr);

    g_message("%s ready", kServiceName);
    g_main_loop_run(g_loop);

    LSUnregister(handle, &err);
    g_main_loop_unref(g_loop);
    return EXIT_SUCCESS;
}
