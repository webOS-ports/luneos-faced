// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// A recognized face's unlock token goes on the subscription of the call that
// asked to identify, and on no other: identify updates are broadcast, and any
// other subscriber (an app with faceunlock.identify, ...) must learn only
// that a face matched.

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "fake_ls2.h"
#include "identify_post.h"

using json = nlohmann::json;

namespace
{
const std::string kToken = "0123456789abcdef0123456789abcdef";

LSHandle *handle()
{
    return reinterpret_cast<LSHandle *>(0x1);
}

json only(const LSMessage *m)
{
    EXPECT_EQ(1u, m->sent.size());
    if (m->sent.size() != 1)
        return json();
    json j = json::parse(m->sent[0], nullptr, false);
    EXPECT_TRUE(j.is_object()) << m->sent[0];
    return j;
}

class IdentifyPost : public ::testing::Test
{
protected:
    void TearDown() override { fakels2::reset(); }
};
} // namespace

TEST_F(IdentifyPost, TheTokenGoesToTheCallerOnly)
{
    LSMessage *shell = fakels2::subscribe("/identify", "com.webos.surfacemanager:1.10", 7);
    LSMessage *app = fakels2::subscribe("/identify", "com.example.app:1.20", 3);
    LSMessage *other = fakels2::subscribe("/identify", "com.example.other:1.30", 7);

    identify::postMatch(handle(), shell, kToken);

    json s = only(shell);
    EXPECT_EQ(kToken, s.value("unlockToken", ""));
    EXPECT_EQ(true, s.value("recognized", false));
    EXPECT_FALSE(s.contains("passcodeRequired"));

    for (LSMessage *m : {app, other})
    {
        json o = only(m);
        const std::string sent = m->sent.empty() ? std::string() : m->sent[0];
        EXPECT_FALSE(o.contains("unlockToken")) << sent;
        EXPECT_EQ(std::string::npos, sent.find(kToken)) << sent;
        EXPECT_EQ(true, o.value("recognized", false));
        EXPECT_FALSE(o.contains("passcodeRequired"));
    }
}

TEST_F(IdentifyPost, AnotherCallOnTheSameConnectionGetsNoToken)
{
    LSMessage *caller = fakels2::subscribe("/identify", "com.webos.surfacemanager:1.10", 7);
    LSMessage *again = fakels2::subscribe("/identify", "com.webos.surfacemanager:1.10", 8);

    identify::postMatch(handle(), caller, kToken);

    EXPECT_TRUE(only(caller).contains("unlockToken"));
    EXPECT_FALSE(only(again).contains("unlockToken"));
}

TEST_F(IdentifyPost, TheCallerIsKnownByItsCallNotItsPointer)
{
    LSMessage *subscribed = fakels2::subscribe("/identify", "com.webos.surfacemanager:1.10", 7);
    LSMessage *held = fakels2::message("com.webos.surfacemanager:1.10", 7);

    identify::postMatch(handle(), held, kToken);

    EXPECT_EQ(kToken, only(subscribed).value("unlockToken", ""));
    EXPECT_TRUE(held->sent.empty());
}

TEST_F(IdentifyPost, WithoutACallerNobodyGetsTheToken)
{
    LSMessage *a = fakels2::subscribe("/identify", "com.example.a:1.1", 1);
    LSMessage *b = fakels2::subscribe("/identify", "com.example.b:1.2", 1);

    identify::postMatch(handle(), nullptr, kToken);

    EXPECT_FALSE(only(a).contains("unlockToken"));
    EXPECT_FALSE(only(b).contains("unlockToken"));
}

TEST_F(IdentifyPost, WithoutATokenEveryoneIsToldThePasscodeIsNeeded)
{
    LSMessage *caller = fakels2::subscribe("/identify", "com.webos.surfacemanager:1.10", 7);
    LSMessage *other = fakels2::subscribe("/identify", "com.example.app:1.20", 3);

    identify::postMatch(handle(), caller, std::string());

    for (LSMessage *m : {caller, other})
    {
        json o = only(m);
        EXPECT_EQ(true, o.value("passcodeRequired", false));
        EXPECT_FALSE(o.contains("unlockToken"));
    }
}

TEST_F(IdentifyPost, OtherKeysAreNotTouched)
{
    LSMessage *status = fakels2::subscribe("/getStatus", "com.webos.surfacemanager:1.10", 7);
    LSMessage *caller = fakels2::subscribe("/identify", "com.webos.surfacemanager:1.10", 8);

    identify::postMatch(handle(), caller, kToken);

    EXPECT_TRUE(status->sent.empty());
    EXPECT_EQ(1u, caller->sent.size());
}

TEST_F(IdentifyPost, BusFailuresAreSurvived)
{
    LSMessage *caller = fakels2::subscribe("/identify", "com.webos.surfacemanager:1.10", 7);

    fakels2::failAcquire(true);
    identify::postMatch(handle(), caller, kToken);
    EXPECT_TRUE(caller->sent.empty());

    fakels2::failAcquire(false);
    fakels2::failRespond(true);
    identify::postMatch(handle(), caller, kToken);
    EXPECT_TRUE(caller->sent.empty());
}

TEST_F(IdentifyPost, SameCall)
{
    LSMessage *a = fakels2::message("x:1.1", 5);
    LSMessage *a2 = fakels2::message("x:1.1", 5);
    LSMessage *b = fakels2::message("x:1.1", 6);
    LSMessage *c = fakels2::message("y:1.2", 5);

    EXPECT_TRUE(identify::sameCall(a, a));
    EXPECT_TRUE(identify::sameCall(a, a2));
    EXPECT_FALSE(identify::sameCall(a, b));
    EXPECT_FALSE(identify::sameCall(a, c));
    EXPECT_FALSE(identify::sameCall(a, nullptr));
    EXPECT_FALSE(identify::sameCall(nullptr, a));
}
