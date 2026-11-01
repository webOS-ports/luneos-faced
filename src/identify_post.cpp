// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>

#include "identify_post.h"

#include <cstring>
#include <string.h> // explicit_bzero

#include <glib.h>
#include <nlohmann/json.hpp>

namespace
{
constexpr const char *kIdentifyKey = "/identify";

void wipe(std::string &text)
{
    if (!text.empty())
        explicit_bzero(&text[0], text.size());
    text.clear();
}
} // namespace

namespace identify
{
bool sameCall(LSMessage *a, LSMessage *b)
{
    if (!a || !b)
        return false;
    if (a == b)
        return true;
    const char *ua = LSMessageGetUniqueToken(a);
    const char *ub = LSMessageGetUniqueToken(b);
    return ua && ub && std::strcmp(ua, ub) == 0;
}

void postMatch(LSHandle *handle, LSMessage *caller, const std::string &token)
{
    nlohmann::json reply{{"returnValue", true}, {"recognized", true}};
    if (token.empty())
        reply["passcodeRequired"] = true;
    std::string forOthers = reply.dump();
    std::string forCaller = forOthers;
    if (!token.empty())
    {
        reply["unlockToken"] = token;
        forCaller = reply.dump();
        wipe(reply["unlockToken"].get_ref<std::string &>());
    }

    LSError err;
    LSErrorInit(&err);
    LSSubscriptionIter *iter = nullptr;
    if (!LSSubscriptionAcquire(handle, kIdentifyKey, &iter, &err))
    {
        g_warning("identify: cannot reach the subscribers: %s", err.message);
        LSErrorFree(&err);
    }
    else
    {
        while (LSSubscriptionHasNext(iter))
        {
            LSMessage *message = LSSubscriptionNext(iter);
            const std::string &payload = sameCall(message, caller) ? forCaller : forOthers;
            if (!LSMessageRespond(message, payload.c_str(), &err))
            {
                g_warning("identify: cannot post the update: %s", err.message);
                LSErrorFree(&err);
            }
        }
        LSSubscriptionRelease(iter);
    }
    wipe(forCaller);
}
} // namespace identify
