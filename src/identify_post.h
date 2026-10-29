// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// The identify update for a recognized face. Identify updates go to every
// subscriber of identify, but luna-authmanager binds the unlock token to the
// one that asked to identify, so that is the only subscription the token is
// sent on; the others learn that a face matched and nothing they could
// unlock with.

#pragma once

#include <string>

#include <luna-service2/lunaservice.h>

namespace identify
{
// Whether a and b are the same call: the same sender connection and the same
// message token (LSMessageGetUniqueToken).
bool sameCall(LSMessage *a, LSMessage *b);

// Posts {"recognized": true} to every subscriber of /identify, with
// "unlockToken" on caller's subscription only. An empty token (luna-authmanager
// wants the passcode) gives everyone "passcodeRequired": true.
void postMatch(LSHandle *handle, LSMessage *caller, const std::string &token);
} // namespace identify
