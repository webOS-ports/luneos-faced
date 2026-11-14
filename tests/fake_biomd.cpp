// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>

#include "fake_biomd.h"

#include <algorithm>

#include <unistd.h>

#include <gio/gunixfdlist.h>

namespace
{
// The parts of biomd's introspection XML (include/introspect.h) the client uses.
const char kManagerXml[] =
    "<node><interface name='io.FuriOS.Biomd.Face'>"
    "<method name='RegisterEnrollmentAgent'><arg type='o' direction='in'/>"
    "<arg type='b' direction='out'/></method>"
    "<method name='UnregisterEnrollmentAgent'><arg type='b' direction='out'/></method>"
    "<method name='RegisterRecognitionAgent'><arg type='o' direction='in'/>"
    "<arg type='b' direction='out'/></method>"
    "<method name='UnregisterRecognitionAgent'><arg type='b' direction='out'/></method>"
    "<method name='CreateAgent'><arg type='o' direction='out'/></method>"
    "<method name='DestroyAgent'><arg type='o' direction='in'/>"
    "<arg type='b' direction='out'/></method>"
    "<property name='FaceEnrolled' type='b' access='read'/>"
    "<property name='ImplementationType' type='i' access='read'/>"
    "<signal name='FaceEnrolledChanged'><arg type='b'/></signal>"
    "</interface></node>";

const char kAgentXml[] =
    "<node><interface name='io.FuriOS.Biomd.Face.Agent'>"
    "<method name='StartEnrollment'><arg type='b' direction='out'/></method>"
    "<method name='StartRecognition'><arg type='b' direction='out'/></method>"
    "<method name='Cancel'><arg type='b' direction='out'/></method>"
    "<method name='RemoveFaceData'><arg type='b' direction='out'/></method>"
    "<method name='SubmitFrame'><arg type='h' direction='in'/><arg type='i' direction='in'/>"
    "<arg type='i' direction='in'/><arg type='i' direction='in'/>"
    "<arg type='u' direction='in'/><arg type='b' direction='out'/></method>"
    "<property name='HasAccess' type='b' access='read'/>"
    "<signal name='AccessChanged'><arg type='b'/></signal>"
    "<signal name='EnrollmentStateChanged'><arg type='u'/></signal>"
    "<signal name='EnrollmentProgressChanged'><arg type='i'/></signal>"
    "<signal name='RecognitionStateChanged'><arg type='u'/></signal>"
    "</interface></node>";

const char kName[]       = "io.FuriOS.Biomd";
const char kFacePath[]   = "/io/FuriOS/Biomd/Face";
const char kAgentIface[] = "io.FuriOS.Biomd.Face.Agent";

constexpr uint32_t kEnrollInProgress = 2;
constexpr uint32_t kEnrollComplete   = 3;
constexpr uint32_t kRecognitionFail  = 1;

void replyBool(GDBusMethodInvocation *inv, bool value)
{
    g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", value));
}
} // namespace

FakeBiomd::FakeBiomd(const std::string &address) : address_(address)
{
    managerInfo_ = g_dbus_node_info_new_for_xml(kManagerXml, nullptr);
    agentInfo_   = g_dbus_node_info_new_for_xml(kAgentXml, nullptr);
}

FakeBiomd::~FakeBiomd()
{
    stop();
    g_dbus_node_info_unref(managerInfo_);
    g_dbus_node_info_unref(agentInfo_);
}

void FakeBiomd::start()
{
    GError *error = nullptr;
    connection_   = g_dbus_connection_new_for_address_sync(
        address_.c_str(),
        static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
        nullptr, nullptr, &error);
    g_assert_no_error(error);

    static const GDBusInterfaceVTable vtable = {&FakeBiomd::onManagerCall,
                                                &FakeBiomd::onManagerGet, nullptr, {nullptr}};
    managerReg_ = g_dbus_connection_register_object(connection_, kFacePath,
                                                    managerInfo_->interfaces[0], &vtable, this,
                                                    nullptr, &error);
    g_assert_no_error(error);

    // Own the name synchronously, so the test knows biomd is up when start()
    // returns.
    GVariant *reply = g_dbus_connection_call_sync(
        connection_, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "RequestName", g_variant_new("(su)", kName, 0u), G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
    g_assert_no_error(error);
    g_variant_unref(reply);
}

void FakeBiomd::stop()
{
    if (!connection_)
        return;
    std::vector<Agent *> agents;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        agents.swap(agents_);
        enrollAgent_.clear();
        recogAgent_.clear();
    }
    for (Agent *agent : agents)
    {
        g_dbus_connection_unregister_object(connection_, agent->regId);
        delete agent;
    }
    g_dbus_connection_unregister_object(connection_, managerReg_);
    // Closing the connection drops the name, which is what clients watch.
    g_dbus_connection_close_sync(connection_, nullptr, nullptr);
    g_object_unref(connection_);
    connection_ = nullptr;
}

void FakeBiomd::script(std::deque<uint32_t> states)
{
    std::lock_guard<std::mutex> lock(mutex_);
    script_ = std::move(states);
}

void FakeBiomd::setFallback(uint32_t state)
{
    std::lock_guard<std::mutex> lock(mutex_);
    fallback_ = state;
}

void FakeBiomd::setEnrolled(bool enrolled)
{
    std::lock_guard<std::mutex> lock(mutex_);
    enrolled_ = enrolled;
}

void FakeBiomd::setImplementationType(int type)
{
    std::lock_guard<std::mutex> lock(mutex_);
    implementationType_ = type;
}

void FakeBiomd::blockRecognition(bool block)
{
    std::lock_guard<std::mutex> lock(mutex_);
    blockRecognition_ = block;
}

std::vector<FakeBiomd::Frame> FakeBiomd::frames()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_;
}

std::vector<std::string> FakeBiomd::calls()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return calls_;
}

int FakeBiomd::agentsCreated()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return created_;
}

FakeBiomd::Agent *FakeBiomd::agentAt(const std::string &path)
{
    for (Agent *agent : agents_)
        if (agent->path == path)
            return agent;
    return nullptr;
}

bool FakeBiomd::hasAccess(const Agent *agent) const
{
    if (agent->path == enrollAgent_)
        return true;
    return agent->path == recogAgent_ && enrollAgent_.empty() && !blockRecognition_;
}

void FakeBiomd::emitAgent(const Agent &agent, const char *member, GVariant *params)
{
    g_dbus_connection_emit_signal(connection_, nullptr, agent.path.c_str(), kAgentIface, member,
                                  params, nullptr);
}

void FakeBiomd::emitEnrolled()
{
    g_dbus_connection_emit_signal(connection_, nullptr, kFacePath, "io.FuriOS.Biomd.Face",
                                  "FaceEnrolledChanged", g_variant_new("(b)", enrolled_), nullptr);
}

void FakeBiomd::onManagerCall(GDBusConnection *, const gchar *sender, const gchar *,
                              const gchar *, const gchar *method, GVariant *params,
                              GDBusMethodInvocation *inv, gpointer data)
{
    auto *self = static_cast<FakeBiomd *>(data);
    std::lock_guard<std::mutex> lock(self->mutex_);
    self->calls_.push_back(method);

    if (g_strcmp0(method, "CreateAgent") == 0)
    {
        auto *agent  = new Agent;
        agent->path  = "/io/FuriOS/Biomd/Face/Agent" + std::to_string(self->nextAgent_++);
        agent->owner = sender;
        static const GDBusInterfaceVTable vtable = {&FakeBiomd::onAgentCall,
                                                    &FakeBiomd::onAgentGet, nullptr, {nullptr}};
        agent->regId = g_dbus_connection_register_object(
            self->connection_, agent->path.c_str(), self->agentInfo_->interfaces[0], &vtable,
            self, nullptr, nullptr);
        self->agents_.push_back(agent);
        ++self->created_;
        g_dbus_method_invocation_return_value(inv,
                                              g_variant_new("(o)", agent->path.c_str()));
        return;
    }
    if (g_strcmp0(method, "DestroyAgent") == 0)
    {
        replyBool(inv, true);
        return;
    }
    if (g_strcmp0(method, "RegisterEnrollmentAgent") == 0 ||
        g_strcmp0(method, "RegisterRecognitionAgent") == 0)
    {
        const gchar *path = nullptr;
        g_variant_get(params, "(&o)", &path);
        Agent *agent = self->agentAt(path);
        if (!agent || agent->owner != sender)
        {
            g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.Failed",
                                                       "Unknown agent path");
            return;
        }
        if (method[8] == 'E')
            self->enrollAgent_ = path;
        else
            self->recogAgent_ = path;
        replyBool(inv, true);
        return;
    }
    if (g_strcmp0(method, "UnregisterEnrollmentAgent") == 0)
    {
        self->enrollAgent_.clear();
        replyBool(inv, true);
        return;
    }
    if (g_strcmp0(method, "UnregisterRecognitionAgent") == 0)
    {
        self->recogAgent_.clear();
        replyBool(inv, true);
        return;
    }
    g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod",
                                               method);
}

GVariant *FakeBiomd::onManagerGet(GDBusConnection *, const gchar *, const gchar *, const gchar *,
                                  const gchar *prop, GError **, gpointer data)
{
    auto *self = static_cast<FakeBiomd *>(data);
    std::lock_guard<std::mutex> lock(self->mutex_);
    if (g_strcmp0(prop, "FaceEnrolled") == 0)
        return g_variant_new_boolean(self->enrolled_);
    return g_variant_new_int32(self->implementationType_);
}

GVariant *FakeBiomd::onAgentGet(GDBusConnection *, const gchar *, const gchar *path,
                                const gchar *, const gchar *, GError **, gpointer data)
{
    auto *self = static_cast<FakeBiomd *>(data);
    std::lock_guard<std::mutex> lock(self->mutex_);
    Agent *agent = self->agentAt(path);
    return g_variant_new_boolean(agent && self->hasAccess(agent));
}

void FakeBiomd::onAgentCall(GDBusConnection *, const gchar *sender, const gchar *path,
                            const gchar *, const gchar *method, GVariant *params,
                            GDBusMethodInvocation *inv, gpointer data)
{
    auto *self = static_cast<FakeBiomd *>(data);
    std::lock_guard<std::mutex> lock(self->mutex_);
    self->calls_.push_back(method);

    Agent *agent = self->agentAt(path);
    if (!agent || agent->owner != sender || !self->hasAccess(agent))
    {
        replyBool(inv, false);
        return;
    }

    if (g_strcmp0(method, "StartEnrollment") == 0)
    {
        if (self->enrolled_)
        {
            replyBool(inv, false);
            return;
        }
        agent->mode      = 1;
        self->lastState_ = kEnrollInProgress;
        self->progress_  = 0;
        replyBool(inv, true);
        return;
    }
    if (g_strcmp0(method, "StartRecognition") == 0)
    {
        if (!self->enrolled_)
        {
            replyBool(inv, false);
            return;
        }
        agent->mode      = 2;
        self->lastState_ = kRecognitionFail;
        replyBool(inv, true);
        return;
    }
    if (g_strcmp0(method, "Cancel") == 0)
    {
        agent->mode = 0;
        replyBool(inv, true);
        return;
    }
    if (g_strcmp0(method, "RemoveFaceData") == 0)
    {
        const bool was  = self->enrolled_;
        self->enrolled_ = false;
        if (was)
            self->emitEnrolled();
        replyBool(inv, was);
        return;
    }

    // SubmitFrame
    gint32 handle = -1, width = 0, height = 0, channels = 0;
    guint32 format = 0;
    g_variant_get(params, "(hiiiu)", &handle, &width, &height, &channels, &format);
    GUnixFDList *list = g_dbus_message_get_unix_fd_list(g_dbus_method_invocation_get_message(inv));
    const int fd = list ? g_unix_fd_list_get(list, handle, nullptr) : -1;
    if (fd < 0 || agent->mode == 0 || (agent->mode == 1 && self->enrolled_))
    {
        if (fd >= 0)
            close(fd);
        replyBool(inv, false);
        return;
    }

    Frame frame{width, height, channels, format, {}};
    frame.bytes.resize(static_cast<size_t>(width) * height * channels);
    const ssize_t n = pread(fd, frame.bytes.data(), frame.bytes.size(), 0);
    close(fd);
    if (n != static_cast<ssize_t>(frame.bytes.size()))
        frame.bytes.clear();
    self->frames_.push_back(std::move(frame));

    // Like biomd: answer with what the newest finished frame gave...
    replyBool(inv, true);
    if (self->silent_)
    {
    }
    else if (agent->mode == 1)
    {
        self->emitAgent(*agent, "EnrollmentProgressChanged", g_variant_new("(i)", self->progress_));
        self->emitAgent(*agent, "EnrollmentStateChanged", g_variant_new("(u)", self->lastState_));
    }
    else
    {
        self->emitAgent(*agent, "RecognitionStateChanged", g_variant_new("(u)", self->lastState_));
    }

    // ...then finish this one.
    uint32_t next = self->fallback_;
    if (!self->script_.empty())
    {
        next = self->script_.front();
        self->script_.pop_front();
    }
    self->lastState_ = next;
    if (agent->mode == 1)
    {
        if (next == kEnrollInProgress)
            self->progress_ = std::min(100, self->progress_ + 25);
        if (next == kEnrollComplete)
        {
            self->progress_ = 100;
            self->enrolled_ = true;
        }
    }
}
