// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>

#include "biomd_face.h"

#include <cerrno>
#include <chrono>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <gio/gunixfdlist.h>

namespace
{
constexpr const char *kBusName      = "io.FuriOS.Biomd";
constexpr const char *kFacePath     = "/io/FuriOS/Biomd/Face";
constexpr const char *kFaceIface    = "io.FuriOS.Biomd.Face";
constexpr const char *kAgentIface   = "io.FuriOS.Biomd.Face.Agent";
constexpr const char *kPropsIface   = "org.freedesktop.DBus.Properties";
constexpr int kCallTimeoutMs        = 5000;
// biomd's FaceFrameFormat: 0 is BGR.
constexpr uint32_t kFormatBgr = 0;

std::string takeError(GError *error)
{
    std::string text = error && error->message ? error->message : "unknown error";
    g_clear_error(&error);
    return text;
}

// One frame in a sealed memfd. biomd dup()s the fd and reads the frame on its
// own worker later, so every frame needs its own file: reusing one would let
// the next frame overwrite a queued one.
int frameFd(const uint8_t *data, size_t size, std::string &error)
{
    const int fd = memfd_create("face-frame", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
    {
        error = std::string("memfd_create: ") + g_strerror(errno);
        return -1;
    }
    size_t done = 0;
    while (done < size)
    {
        const ssize_t n = write(fd, data + done, size - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
        {
            error = std::string("write frame: ") + g_strerror(errno);
            close(fd);
            return -1;
        }
        done += static_cast<size_t>(n);
    }
    // The frame is final; biomd only ever reads it.
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) < 0)
        g_debug("biomd: cannot seal frame memfd: %s", g_strerror(errno));
    return fd;
}
} // namespace

BiomdFace::BiomdFace(GDBusConnection *connection)
    : connection_(connection ? G_DBUS_CONNECTION(g_object_ref(connection)) : nullptr)
{
}

BiomdFace::~BiomdFace()
{
    if (connection_)
    {
        std::string path;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            path = agentPath_;
        }
        if (!path.empty())
        {
            // Best effort: biomd drops our agents anyway when we leave the bus.
            g_dbus_connection_call(connection_, kBusName, kFacePath, kFaceIface, "DestroyAgent",
                                   g_variant_new("(o)", path.c_str()), nullptr,
                                   G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr, nullptr,
                                   nullptr);
            g_dbus_connection_flush_sync(connection_, nullptr, nullptr);
        }
        if (agentSub_)
            g_dbus_connection_signal_unsubscribe(connection_, agentSub_);
        if (managerSub_)
            g_dbus_connection_signal_unsubscribe(connection_, managerSub_);
    }
    if (watchId_)
        g_bus_unwatch_name(watchId_);
    if (connection_)
        g_object_unref(connection_);
}

bool BiomdFace::start(ChangedFn changed)
{
    changed_ = std::move(changed);

    if (!connection_)
    {
        GError *error = nullptr;
        connection_   = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
        if (!connection_)
        {
            g_warning("biomd: no system bus: %s", takeError(error).c_str());
            return false;
        }
    }

    // The agent path changes with every biomd instance, so match any path and
    // check it in onSignal.
    agentSub_ = g_dbus_connection_signal_subscribe(connection_, kBusName, kAgentIface, nullptr,
                                                   nullptr, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                                   &BiomdFace::onSignal, this, nullptr);
    managerSub_ = g_dbus_connection_signal_subscribe(connection_, kBusName, kFaceIface, nullptr,
                                                     kFacePath, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                                     &BiomdFace::onSignal, this, nullptr);
    watchId_ = g_bus_watch_name_on_connection(connection_, kBusName, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                              &BiomdFace::onAppeared, &BiomdFace::onVanished,
                                              this, nullptr);
    return true;
}

void BiomdFace::notifyChanged()
{
    if (changed_)
        changed_();
}

void BiomdFace::setEnrolled(bool value)
{
    if (enrolled_.exchange(value) != value)
        notifyChanged();
}

void BiomdFace::onAppeared(GDBusConnection *, const gchar *, const gchar *owner, gpointer data)
{
    auto *self = static_cast<BiomdFace *>(data);
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        self->owner_ = owner ? owner : "";
        // A new biomd knows nothing of an agent the old one gave us.
        self->agentPath_.clear();
        self->roleHeld_ = false;
        ++self->ownerSeq_;
    }
    g_message("biomd: on the bus as %s", owner ? owner : "?");
    self->refreshProperties();
}

void BiomdFace::onVanished(GDBusConnection *, const gchar *, gpointer data)
{
    auto *self = static_cast<BiomdFace *>(data);
    bool was = false;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        was = !self->owner_.empty();
        self->owner_.clear();
        self->agentPath_.clear();
        self->roleHeld_ = false;
        self->accessLost_ = true;
        ++self->ownerSeq_;
    }
    self->cond_.notify_all();
    if (was)
        g_message("biomd: left the bus");
    self->available_ = false;
    self->enrolled_  = false;
    self->notifyChanged();
}

GVariant *BiomdFace::getProperty(const char *name)
{
    GError *error  = nullptr;
    GVariant *reply = g_dbus_connection_call_sync(
        connection_, kBusName, kFacePath, kPropsIface, "Get",
        g_variant_new("(ss)", kFaceIface, name), G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE,
        kCallTimeoutMs, nullptr, &error);
    if (!reply)
    {
        g_warning("biomd: cannot read %s: %s", name, takeError(error).c_str());
        return nullptr;
    }
    GVariant *value = nullptr;
    g_variant_get(reply, "(v)", &value);
    g_variant_unref(reply);
    return value;
}

void BiomdFace::refreshProperties()
{
    bool hasBackend = false, hasFace = false;
    if (GVariant *type = getProperty("ImplementationType"))
    {
        if (g_variant_is_of_type(type, G_VARIANT_TYPE_INT32))
            hasBackend = g_variant_get_int32(type) != 0;
        g_variant_unref(type);
    }
    if (GVariant *value = getProperty("FaceEnrolled"))
    {
        if (g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN))
            hasFace = g_variant_get_boolean(value);
        g_variant_unref(value);
    }
    if (!hasBackend)
        g_message("biomd: no face backend (built without the face matcher?)");
    available_ = hasBackend;
    enrolled_  = hasFace;
    notifyChanged();
}

void BiomdFace::onSignal(GDBusConnection *, const gchar *sender, const gchar *path,
                         const gchar *iface, const gchar *member, GVariant *params, gpointer data)
{
    auto *self = static_cast<BiomdFace *>(data);

    if (g_strcmp0(iface, kFaceIface) == 0)
    {
        if (g_strcmp0(member, "FaceEnrolledChanged") == 0 &&
            g_variant_is_of_type(params, G_VARIANT_TYPE("(b)")))
        {
            gboolean hasFace = FALSE;
            g_variant_get(params, "(b)", &hasFace);
            self->setEnrolled(hasFace);
        }
        return;
    }

    std::unique_lock<std::mutex> lock(self->mutex_);
    // Only our own agent, and only from the biomd we created it on.
    if (self->agentPath_.empty() || g_strcmp0(path, self->agentPath_.c_str()) != 0 ||
        g_strcmp0(sender, self->owner_.c_str()) != 0)
        return;

    const bool enrolling = self->role_ == Mode::Enroll;
    if (g_variant_is_of_type(params, G_VARIANT_TYPE("(u)")) &&
        g_strcmp0(member, enrolling ? "EnrollmentStateChanged" : "RecognitionStateChanged") == 0)
    {
        guint32 state = 0;
        g_variant_get(params, "(u)", &state);
        self->lastState_ = state;
        ++self->stateSeq_;
    }
    else if (enrolling && g_strcmp0(member, "EnrollmentProgressChanged") == 0 &&
             g_variant_is_of_type(params, G_VARIANT_TYPE("(i)")))
    {
        gint32 progress = 0;
        g_variant_get(params, "(i)", &progress);
        self->lastProgress_ = progress < 0 ? 0 : progress > 100 ? 100 : progress;
    }
    else if (g_strcmp0(member, "AccessChanged") == 0 &&
             g_variant_is_of_type(params, G_VARIANT_TYPE("(b)")))
    {
        gboolean granted = FALSE;
        g_variant_get(params, "(b)", &granted);
        if (!granted && self->roleHeld_)
            self->accessLost_ = true;
    }
    else
    {
        return;
    }
    lock.unlock();
    self->cond_.notify_all();
}

GVariant *BiomdFace::callManager(const char *method, GVariant *params, const GVariantType *reply,
                                 std::string &error)
{
    GError *gerror = nullptr;
    GVariant *out  = g_dbus_connection_call_sync(connection_, kBusName, kFacePath, kFaceIface,
                                                 method, params, reply, G_DBUS_CALL_FLAGS_NONE,
                                                 kCallTimeoutMs, nullptr, &gerror);
    if (!out)
        error = std::string(method) + ": " + takeError(gerror);
    return out;
}

GVariant *BiomdFace::callAgent(const std::string &path, const char *method, GVariant *params,
                               const GVariantType *reply, std::string &error, GUnixFDList *fds)
{
    GError *gerror = nullptr;
    GVariant *out  = g_dbus_connection_call_with_unix_fd_list_sync(
        connection_, kBusName, path.c_str(), kAgentIface, method, params, reply,
        G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, fds, nullptr, nullptr, &gerror);
    if (!out)
        error = std::string(method) + ": " + takeError(gerror);
    return out;
}

bool BiomdFace::callAgentBool(const std::string &path, const char *method, std::string &error)
{
    GVariant *reply = callAgent(path, method, nullptr, G_VARIANT_TYPE("(b)"), error);
    if (!reply)
        return false;
    gboolean ok = FALSE;
    g_variant_get(reply, "(b)", &ok);
    g_variant_unref(reply);
    if (!ok)
        error = std::string(method) + " refused";
    return ok;
}

bool BiomdFace::ensureAgent(std::string &pathOut, std::string &error)
{
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (owner_.empty())
        {
            error = "biomd is not running";
            return false;
        }
        if (!agentPath_.empty())
        {
            pathOut = agentPath_;
            return true;
        }
        seq = ownerSeq_;
    }

    GVariant *reply = callManager("CreateAgent", nullptr, G_VARIANT_TYPE("(o)"), error);
    if (!reply)
        return false;
    const gchar *path = nullptr;
    g_variant_get(reply, "(&o)", &path);
    pathOut = path;
    g_variant_unref(reply);

    std::lock_guard<std::mutex> lock(mutex_);
    if (seq != ownerSeq_)
    {
        // biomd restarted under us; that agent is already gone.
        error = "biomd restarted";
        return false;
    }
    agentPath_ = pathOut;
    return true;
}

bool BiomdFace::registerRole(Mode mode, const std::string &path, std::string &error)
{
    GVariant *reply = callManager(mode == Mode::Enroll ? "RegisterEnrollmentAgent"
                                                       : "RegisterRecognitionAgent",
                                  g_variant_new("(o)", path.c_str()), G_VARIANT_TYPE("(b)"), error);
    if (!reply)
        return false;
    gboolean ok = FALSE;
    g_variant_get(reply, "(b)", &ok);
    g_variant_unref(reply);
    if (!ok)
    {
        error = "biomd refused the agent";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    roleHeld_ = true;
    role_     = mode;
    return true;
}

void BiomdFace::unregisterRole(Mode mode)
{
    std::string error;
    GVariant *reply = callManager(mode == Mode::Enroll ? "UnregisterEnrollmentAgent"
                                                       : "UnregisterRecognitionAgent",
                                  nullptr, G_VARIANT_TYPE("(b)"), error);
    if (reply)
        g_variant_unref(reply);
    else
        g_debug("biomd: %s", error.c_str());
    std::lock_guard<std::mutex> lock(mutex_);
    roleHeld_ = false;
}

bool BiomdFace::begin(Mode mode, std::string &error)
{
    std::string path;
    if (!ensureAgent(path, error))
        return false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        role_         = mode;
        lastState_    = 0;
        lastProgress_ = 0;
        accessLost_   = false;
        interrupted_  = false;
    }
    if (!registerRole(mode, path, error))
        return false;

    // A recognition agent only has access while nobody is enrolling.
    GVariant *grant = nullptr;
    {
        GError *gerror = nullptr;
        GVariant *reply = g_dbus_connection_call_sync(
            connection_, kBusName, path.c_str(), kPropsIface, "Get",
            g_variant_new("(ss)", kAgentIface, "HasAccess"), G_VARIANT_TYPE("(v)"),
            G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr, &gerror);
        if (reply)
        {
            g_variant_get(reply, "(v)", &grant);
            g_variant_unref(reply);
        }
        else
        {
            g_clear_error(&gerror);
        }
    }
    const bool hasAccess =
        grant && g_variant_is_of_type(grant, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(grant);
    if (grant)
        g_variant_unref(grant);
    if (!hasAccess)
    {
        error = "biomd is busy with another client";
        unregisterRole(mode);
        return false;
    }

    if (!callAgentBool(path, mode == Mode::Enroll ? "StartEnrollment" : "StartRecognition", error))
    {
        unregisterRole(mode);
        return false;
    }
    return true;
}

BiomdFace::Result BiomdFace::submit(const uint8_t *bgr, int width, int height, int waitMs)
{
    Result result;
    std::string path;
    uint64_t seq = 0;
    Mode mode    = Mode::Recognize;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (agentPath_.empty() || !roleHeld_ || accessLost_)
        {
            result.lost = true;
            return result;
        }
        path = agentPath_;
        seq  = stateSeq_;
        mode = role_;
    }

    if (!bgr || width <= 0 || height <= 0)
        return result;

    std::string error;
    const size_t size = static_cast<size_t>(width) * static_cast<size_t>(height) * 3;
    const int fd      = frameFd(bgr, size, error);
    if (fd < 0)
    {
        g_warning("biomd: %s", error.c_str());
        return result;
    }

    GUnixFDList *fds = g_unix_fd_list_new();
    GError *gerror   = nullptr;
    const gint handle = g_unix_fd_list_append(fds, fd, &gerror);
    close(fd); // the list holds its own duplicate
    if (handle < 0)
    {
        g_warning("biomd: cannot attach the frame: %s", takeError(gerror).c_str());
        g_object_unref(fds);
        return result;
    }

    GVariant *reply = callAgent(path, "SubmitFrame",
                                g_variant_new("(hiiiu)", handle, width, height, 3, kFormatBgr),
                                G_VARIANT_TYPE("(b)"), error, fds);
    g_object_unref(fds);
    if (reply)
    {
        gboolean accepted = FALSE;
        g_variant_get(reply, "(b)", &accepted);
        g_variant_unref(reply);
        result.accepted = accepted;
    }
    else
    {
        g_debug("biomd: %s", error.c_str());
    }

    if (mode == Mode::Enroll && !result.accepted)
    {
        // biomd stops taking enrollment frames once the template is stored.
        GVariant *value = getProperty("FaceEnrolled");
        if (value)
        {
            result.enrolled = g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN) &&
                              g_variant_get_boolean(value);
            g_variant_unref(value);
        }
        if (result.enrolled)
            setEnrolled(true);
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (result.accepted && waitMs > 0)
        cond_.wait_for(lock, std::chrono::milliseconds(waitMs), [&] {
            return stateSeq_ != seq || accessLost_ || interrupted_;
        });
    result.haveState = stateSeq_ != seq;
    result.state     = lastState_;
    result.progress  = lastProgress_;
    result.lost      = accessLost_ || agentPath_.empty();
    if (mode == Mode::Enroll && result.haveState && result.state == EnrollmentComplete)
        result.enrolled = true;
    return result;
}

void BiomdFace::interrupt()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        interrupted_ = true;
    }
    cond_.notify_all();
}

void BiomdFace::end()
{
    std::string path;
    bool held  = false;
    Mode mode  = Mode::Recognize;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path = agentPath_;
        held = roleHeld_;
        mode = role_;
    }
    if (path.empty() || !held)
        return;

    std::string error;
    if (!callAgentBool(path, "Cancel", error))
        g_debug("biomd: %s", error.c_str());
    unregisterRole(mode);
}

bool BiomdFace::removeFaceData(std::string &error)
{
    std::string path;
    if (!ensureAgent(path, error))
        return false;

    // RemoveFaceData wants an agent with access; the enrollment role always
    // has it.
    if (!registerRole(Mode::Enroll, path, error))
        return false;

    bool ok = true;
    GVariant *value = getProperty("FaceEnrolled");
    const bool hasFace =
        value && g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(value);
    if (value)
        g_variant_unref(value);
    if (hasFace)
        ok = callAgentBool(path, "RemoveFaceData", error);

    unregisterRole(Mode::Enroll);
    if (ok)
        setEnrolled(false);
    return ok;
}
