// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>

#include "template_store.h"

#include <cerrno>
#include <fcntl.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <glib.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace
{
constexpr size_t kKeyLen = 32; // AES-256
constexpr size_t kIvLen  = 12; // GCM nonce
constexpr size_t kTagLen = 16;

bool readFile(const std::string &path, std::vector<unsigned char> &out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool writeFilePrivate(const std::string &path, const unsigned char *data, size_t len)
{
    const std::string tmp = path + ".tmp";
    int fd                = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return false;
    bool wrote = ::write(fd, data, len) == static_cast<ssize_t>(len);
    if (wrote)
        wrote = ::fsync(fd) == 0;
    ::close(fd);
    if (!wrote)
    {
        ::unlink(tmp.c_str());
        return false;
    }
    return ::rename(tmp.c_str(), path.c_str()) == 0;
}
} // namespace

TemplateStore::TemplateStore(std::string directory)
    : dir_(std::move(directory)), blobPath_(dir_ + "/templates.enc"), keyPath_(dir_ + "/.key")
{
}

bool TemplateStore::ensureDirectory()
{
    if (::mkdir(dir_.c_str(), 0700) != 0 && errno != EEXIST)
    {
        lastError_ = "mkdir " + dir_ + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

bool TemplateStore::exists() const { return ::access(blobPath_.c_str(), R_OK) == 0; }

bool TemplateStore::loadOrCreateKey(unsigned char key[kKeyLen])
{
    std::vector<unsigned char> existing;
    if (readFile(keyPath_, existing) && existing.size() == kKeyLen)
    {
        std::memcpy(key, existing.data(), kKeyLen);
        return true;
    }

    if (RAND_bytes(key, kKeyLen) != 1)
    {
        lastError_ = "RAND_bytes failed";
        return false;
    }
    if (!ensureDirectory() || !writeFilePrivate(keyPath_, key, kKeyLen))
    {
        lastError_ = "cannot persist data key";
        return false;
    }
    g_message("generated a new template data key");
    return true;
}

bool TemplateStore::save(const std::string &json)
{
    if (!ensureDirectory())
        return false;

    unsigned char key[kKeyLen];
    if (!loadOrCreateKey(key))
        return false;

    unsigned char iv[kIvLen];
    if (RAND_bytes(iv, kIvLen) != 1)
    {
        lastError_ = "RAND_bytes failed";
        return false;
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
    {
        lastError_ = "EVP_CIPHER_CTX_new failed";
        return false;
    }

    std::vector<unsigned char> out(kIvLen + json.size() + kTagLen);
    std::memcpy(out.data(), iv, kIvLen);

    bool okay = false;
    int len   = 0, total = 0;
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key, iv) == 1 &&
        EVP_EncryptUpdate(ctx, out.data() + kIvLen, &len,
                          reinterpret_cast<const unsigned char *>(json.data()),
                          static_cast<int>(json.size())) == 1)
    {
        total = len;
        if (EVP_EncryptFinal_ex(ctx, out.data() + kIvLen + total, &len) == 1)
        {
            total += len;
            okay = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagLen,
                                       out.data() + kIvLen + total) == 1;
        }
    }
    EVP_CIPHER_CTX_free(ctx);
    OPENSSL_cleanse(key, kKeyLen);

    if (!okay)
    {
        lastError_ = "AES-GCM encrypt failed";
        return false;
    }

    if (!writeFilePrivate(blobPath_, out.data(), kIvLen + total + kTagLen))
    {
        lastError_ = "cannot write " + blobPath_;
        return false;
    }
    return true;
}

bool TemplateStore::load(std::string &jsonOut)
{
    jsonOut.clear();

    std::vector<unsigned char> blob;
    if (!readFile(blobPath_, blob))
        return true; // nothing enrolled yet, not an error

    if (blob.size() < kIvLen + kTagLen)
    {
        lastError_ = "template blob truncated";
        return false;
    }

    unsigned char key[kKeyLen];
    if (!loadOrCreateKey(key))
        return false;

    const size_t cipherLen = blob.size() - kIvLen - kTagLen;
    EVP_CIPHER_CTX *ctx    = EVP_CIPHER_CTX_new();
    if (!ctx)
    {
        lastError_ = "EVP_CIPHER_CTX_new failed";
        return false;
    }

    std::vector<unsigned char> plain(cipherLen + 1, 0);
    bool okay = false;
    int len   = 0, total = 0;
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key, blob.data()) == 1 &&
        EVP_DecryptUpdate(ctx, plain.data(), &len, blob.data() + kIvLen,
                          static_cast<int>(cipherLen)) == 1)
    {
        total = len;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagLen,
                                blob.data() + kIvLen + cipherLen) == 1 &&
            EVP_DecryptFinal_ex(ctx, plain.data() + total, &len) == 1)
        {
            total += len;
            okay = true;
        }
    }
    EVP_CIPHER_CTX_free(ctx);
    OPENSSL_cleanse(key, kKeyLen);

    if (!okay)
    {
        // Wrong key or tampering. Refuse rather than silently re-enrolling.
        lastError_ = "template authentication failed";
        return false;
    }

    jsonOut.assign(reinterpret_cast<char *>(plain.data()), static_cast<size_t>(total));
    return true;
}

bool TemplateStore::clear()
{
    if (::unlink(blobPath_.c_str()) != 0 && errno != ENOENT)
    {
        lastError_ = std::string("unlink: ") + std::strerror(errno);
        return false;
    }
    return true;
}
