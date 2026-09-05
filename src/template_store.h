// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// Encrypted persistence for libfart's enrollment JSON.
//
// Why this is encrypted at all: a face embedding is not a one-way hash. Template
// inversion research reconstructs a recognisable face image from an ArcFace-style
// embedding, so a leaked template file is close to leaking the user's face plus a
// working unlock credential. libfart's in-memory enrollment mode lets us keep the
// plaintext out of the filesystem entirely.
//
// TODO(luna-authmanager): the data key currently lives in a root-only file
// alongside the ciphertext, which stops a casual copy of /var but not root. Once
// luna-authmanager exists it should own key derivation - passcode-derived, and on
// Halium wrapped by gatekeeper/keymaster - and hand the key to this daemon after
// a successful credential check. That also gives "PIN required after boot" for
// free: without a passcode entry the templates cannot be decrypted at all.

#pragma once

#include <string>

class TemplateStore
{
public:
    explicit TemplateStore(std::string directory);

    // Returns true and an empty payload when nothing is enrolled yet.
    bool load(std::string &jsonOut);
    bool save(const std::string &json);
    bool clear();

    bool exists() const;
    const std::string &lastError() const { return lastError_; }

private:
    bool ensureDirectory();
    bool loadOrCreateKey(unsigned char key[32]);

    std::string dir_;
    std::string blobPath_;
    std::string keyPath_;
    std::string lastError_;
};
