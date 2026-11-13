#include <Portal.h>
#include <Cast.h>
#include <Chooser.h>
#include <ext-image-capture-source-v1-client-protocol.h>
#include <ext-image-copy-capture-v1-client-protocol.h>
#include <ext-foreign-toplevel-list-v1-client-protocol.h>
#include <linux-dmabuf-v1-client-protocol.h>
#include <xdg-output-unstable-v1-client-protocol.h>
#include <gbm.h>
#include <xf86drm.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <cstring>

static constexpr const char *ServiceName { "org.freedesktop.impl.portal.desktop.cz" };
static constexpr const char *ObjectPath { "/org/freedesktop/portal/desktop" };
static constexpr const char *ScreenCastInterface { "org.freedesktop.impl.portal.ScreenCast" };
static constexpr const char *SessionInterface { "org.freedesktop.impl.portal.Session" };
static constexpr const char *RestoreVendor { "cuarzo" };

// ScreenCast version: v4 adds restore tokens (persist_mode, restore_data)
static constexpr UInt32 ScreenCastVersion { 4 };

// Portal responses
static constexpr UInt32 ResponseSuccess { 0 }, ResponseCancelled { 1 }, ResponseOther { 2 };

/******************************************* LOG *******************************************/

void Log(LogLevel level, const char *format, ...)
{
    static const int maxLevel { [] {
        const char *env { getenv("CZ_PORTAL_LOG_LEVEL") };
        return env ? atoi(env) : int(Warning);
    }() };

    if (int(level) > maxLevel)
        return;

    static const char *names[] { "", "Error", "Warning", "Info", "Debug" };
    fprintf(stderr, "[xdg-desktop-portal-cz] %s: ", names[level]);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}

/***************************************** PORTAL ******************************************/

Portal &Portal::Get() noexcept
{
    static Portal portal;
    return portal;
}

bool Portal::init() noexcept
{
    m_core = CZCore::GetOrMake();

    if (!m_core)
        return false;

    return initWayland() && initPipeWire() && initDBus();
}

int Portal::run() noexcept
{
    while (true)
    {
        wl_display_flush(display);

        if (m_core->dispatch(-1) < 0)
            return 1;
    }
}

Output *Portal::findOutput(const std::string &name) const noexcept
{
    for (const auto &o : outputs)
        if (o->name == name)
            return o.get();

    return nullptr;
}

Output *Portal::findOutput(wl_output *output) const noexcept
{
    for (const auto &o : outputs)
        if (o->output == output)
            return o.get();

    return nullptr;
}

gbm_device *Portal::gbmForDevice(dev_t device) noexcept
{
    if (m_gbm && m_gbmDevice == device)
        return m_gbm;

    drmDevice *drm {};

    if (drmGetDeviceFromDevId(device, 0, &drm) != 0)
        return m_gbm;

    // The render node, else the primary one
    const char *node { nullptr };

    if (drm->available_nodes & (1 << DRM_NODE_RENDER))
        node = drm->nodes[DRM_NODE_RENDER];
    else if (drm->available_nodes & (1 << DRM_NODE_PRIMARY))
        node = drm->nodes[DRM_NODE_PRIMARY];

    const int fd { node ? open(node, O_RDWR | O_CLOEXEC) : -1 };
    drmFreeDevice(&drm);

    if (fd < 0)
        return m_gbm;

    auto *gbm { gbm_create_device(fd) };

    if (!gbm)
    {
        close(fd);
        return m_gbm;
    }

    if (m_gbm)
    {
        const int oldFd { gbm_device_get_fd(m_gbm) };
        gbm_device_destroy(m_gbm);
        close(oldFd);
    }

    m_gbm = gbm;
    m_gbmDevice = device;
    return m_gbm;
}

/***************************************** WAYLAND *****************************************/

static void OutputGeometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t) {}
static void OutputMode(void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {}
static void OutputDone(void*, wl_output*) {}
static void OutputScale(void*, wl_output*, int32_t) {}
static void OutputName(void *data, wl_output*, const char *name) { static_cast<Output*>(data)->name = name; }
static void OutputDescription(void *data, wl_output*, const char *description) { static_cast<Output*>(data)->description = description; }

static const wl_output_listener OutputListener
{
    .geometry = OutputGeometry,
    .mode = OutputMode,
    .done = OutputDone,
    .scale = OutputScale,
    .name = OutputName,
    .description = OutputDescription
};

static void XdgOutputPosition(void *data, zxdg_output_v1*, int32_t x, int32_t y) { auto *o { static_cast<Output*>(data) }; o->x = x; o->y = y; }
static void XdgOutputSize(void *data, zxdg_output_v1*, int32_t w, int32_t h) { auto *o { static_cast<Output*>(data) }; o->width = w; o->height = h; }
static void XdgOutputDone(void*, zxdg_output_v1*) {}
static void XdgOutputName(void *data, zxdg_output_v1*, const char *name) { auto *o { static_cast<Output*>(data) }; if (o->name.empty()) o->name = name; }
static void XdgOutputDescription(void *data, zxdg_output_v1*, const char *description) { auto *o { static_cast<Output*>(data) }; if (o->description.empty()) o->description = description; }

static const zxdg_output_v1_listener XdgOutputListener
{
    .logical_position = XdgOutputPosition,
    .logical_size = XdgOutputSize,
    .done = XdgOutputDone,
    .name = XdgOutputName,
    .description = XdgOutputDescription
};

static void ToplevelClosed(void *data, ext_foreign_toplevel_handle_v1 *handle)
{
    auto &portal { Portal::Get() };

    // Casts of it are stopped by the compositor (its capture source is gone)
    for (const auto &session : portal.sessions)
        if (session->target.toplevel == data)
            session->target.toplevel = nullptr;

    std::erase_if(portal.toplevels, [data](const auto &t){ return t.get() == data; });
    ext_foreign_toplevel_handle_v1_destroy(handle);
}

static void ToplevelDone(void*, ext_foreign_toplevel_handle_v1*) {}
static void ToplevelTitle(void *data, ext_foreign_toplevel_handle_v1*, const char *title) { static_cast<Toplevel*>(data)->title = title; }
static void ToplevelAppId(void *data, ext_foreign_toplevel_handle_v1*, const char *appId) { static_cast<Toplevel*>(data)->appId = appId; }
static void ToplevelIdentifier(void *data, ext_foreign_toplevel_handle_v1*, const char *id) { static_cast<Toplevel*>(data)->identifier = id; }

static const ext_foreign_toplevel_handle_v1_listener ToplevelListener
{
    .closed = ToplevelClosed,
    .done = ToplevelDone,
    .title = ToplevelTitle,
    .app_id = ToplevelAppId,
    .identifier = ToplevelIdentifier
};

static void ToplevelListToplevel(void*, ext_foreign_toplevel_list_v1*, ext_foreign_toplevel_handle_v1 *handle)
{
    auto toplevel { std::make_unique<Toplevel>() };
    toplevel->handle = handle;
    ext_foreign_toplevel_handle_v1_add_listener(handle, &ToplevelListener, toplevel.get());
    Portal::Get().toplevels.emplace_back(std::move(toplevel));
}

static void ToplevelListFinished(void*, ext_foreign_toplevel_list_v1*) {}

static const ext_foreign_toplevel_list_v1_listener ToplevelListListener
{
    .toplevel = ToplevelListToplevel,
    .finished = ToplevelListFinished
};

static void AddXdgOutput(Output &o)
{
    auto &portal { Portal::Get() };

    if (!portal.xdgOutputManager || o.xdgOutput)
        return;

    o.xdgOutput = zxdg_output_manager_v1_get_xdg_output(portal.xdgOutputManager, o.output);
    zxdg_output_v1_add_listener(o.xdgOutput, &XdgOutputListener, &o);
}

static void RegistryGlobal(void*, wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    auto &portal { Portal::Get() };

    if (strcmp(interface, wl_output_interface.name) == 0)
    {
        auto o { std::make_unique<Output>() };
        o->globalName = name;
        o->output = static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, std::min<uint32_t>(version, 4)));
        wl_output_add_listener(o->output, &OutputListener, o.get());
        AddXdgOutput(*o);
        portal.outputs.emplace_back(std::move(o));
    }
    else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0)
    {
        portal.xdgOutputManager = static_cast<zxdg_output_manager_v1*>(wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, std::min<uint32_t>(version, 3)));

        for (auto &o : portal.outputs)
            AddXdgOutput(*o);
    }
    else if (strcmp(interface, wl_shm_interface.name) == 0)
        portal.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0 && version >= 3)
        portal.linuxDmabuf = static_cast<zwp_linux_dmabuf_v1*>(wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, 3));
    else if (strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name) == 0)
        portal.outputSourceManager = static_cast<ext_output_image_capture_source_manager_v1*>(wl_registry_bind(registry, name, &ext_output_image_capture_source_manager_v1_interface, 1));
    else if (strcmp(interface, ext_foreign_toplevel_image_capture_source_manager_v1_interface.name) == 0)
        portal.toplevelSourceManager = static_cast<ext_foreign_toplevel_image_capture_source_manager_v1*>(wl_registry_bind(registry, name, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1));
    else if (strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0)
        portal.copyCaptureManager = static_cast<ext_image_copy_capture_manager_v1*>(wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1));
    else if (strcmp(interface, ext_foreign_toplevel_list_v1_interface.name) == 0)
    {
        portal.toplevelList = static_cast<ext_foreign_toplevel_list_v1*>(wl_registry_bind(registry, name, &ext_foreign_toplevel_list_v1_interface, 1));
        ext_foreign_toplevel_list_v1_add_listener(portal.toplevelList, &ToplevelListListener, nullptr);
    }
}

static void RegistryGlobalRemove(void*, wl_registry*, uint32_t name)
{
    auto &portal { Portal::Get() };

    for (auto it = portal.outputs.begin(); it != portal.outputs.end(); it++)
    {
        if ((*it)->globalName != name)
            continue;

        // Casts of it are stopped by the compositor
        for (const auto &session : portal.sessions)
            if (session->target.output == it->get())
                session->target.output = nullptr;

        if ((*it)->xdgOutput)
            zxdg_output_v1_destroy((*it)->xdgOutput);

        wl_output_release((*it)->output);
        portal.outputs.erase(it);
        return;
    }
}

static const wl_registry_listener RegistryListener
{
    .global = RegistryGlobal,
    .global_remove = RegistryGlobalRemove
};

bool Portal::initWayland() noexcept
{
    display = wl_display_connect(nullptr);

    if (!display)
    {
        Log(Error, "Can't connect to the Wayland display (WAYLAND_DISPLAY=%s)", getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "");
        return false;
    }

    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &RegistryListener, nullptr);
    wl_display_roundtrip(display); // Globals
    wl_display_roundtrip(display); // Outputs, toplevels

    if (!copyCaptureManager || !outputSourceManager)
    {
        Log(Error, "The compositor doesn't support ext-image-copy-capture / ext-image-capture-source");
        return false;
    }

    m_waylandSource = CZEventSource::Make(wl_display_get_fd(display), EPOLLIN, CZOwn::Borrow, [this](int, UInt32 events, CZEventSource*) {
        if ((events & (EPOLLHUP | EPOLLERR)) || wl_display_dispatch(display) < 0)
        {
            Log(Error, "Lost the Wayland connection");
            exit(1);
        }
    });

    return true;
}

/**************************************** PIPEWIRE *****************************************/

static void OnPipeWireError(void*, uint32_t, int, int, const char *message)
{
    // The PipeWire daemon went away (e.g. restarted): exit so we get restarted and reconnect
    Log(Error, "PipeWire error: %s", message);
    exit(1);
}

static const pw_core_events PipeWireCoreEvents
{
    .version = PW_VERSION_CORE_EVENTS,
    .error = OnPipeWireError
};

bool Portal::initPipeWire() noexcept
{
    pw_init(nullptr, nullptr);
    pwLoop = pw_loop_new(nullptr);

    if (!pwLoop)
        return false;

    pwContext = pw_context_new(pwLoop, nullptr, 0);

    if (!pwContext)
        return false;

    pwCore = pw_context_connect(pwContext, nullptr, 0);

    if (!pwCore)
    {
        Log(Error, "Can't connect to PipeWire");
        return false;
    }

    pw_core_add_listener(pwCore, &pwCoreListener, &PipeWireCoreEvents, nullptr);

    pw_loop_enter(pwLoop);
    m_pipewireSource = CZEventSource::Make(pw_loop_get_fd(pwLoop), EPOLLIN, CZOwn::Borrow, [this](int, UInt32, CZEventSource*) {
        pw_loop_iterate(pwLoop, 0);
    });

    return true;
}

/****************************************** D-BUS ******************************************/

Session *Portal::findSession(const std::string &handle) const noexcept
{
    for (const auto &s : sessions)
        if (s->handle == handle)
            return s.get();

    return nullptr;
}

void Portal::destroySession(Session *session) noexcept
{
    std::erase_if(sessions, [session](const auto &s){ return s.get() == session; });
}

// Skips the a{sv} options (positioned at the array)
static int SkipOptions(sd_bus_message *msg)
{
    return sd_bus_message_skip(msg, "a{sv}");
}

static int CreateSession(sd_bus_message *msg, void*, sd_bus_error*)
{
    const char *requestHandle, *sessionHandle, *appId;
    int ret { sd_bus_message_read(msg, "oos", &requestHandle, &sessionHandle, &appId) };

    if (ret < 0)
        return ret;

    if ((ret = SkipOptions(msg)) < 0)
        return ret;

    auto &portal { Portal::Get() };

    if (!portal.findSession(sessionHandle))
    {
        auto session { std::make_unique<Session>(sessionHandle) };
        session->appId = appId;
        portal.sessions.emplace_back(std::move(session));
    }

    Log(Info, "CreateSession %s (%s)", sessionHandle, appId);
    return sd_bus_reply_method_return(msg, "ua{sv}", ResponseSuccess, 0);
}

static int SelectSources(sd_bus_message *msg, void*, sd_bus_error *error)
{
    const char *requestHandle, *sessionHandle, *appId;
    int ret { sd_bus_message_read(msg, "oos", &requestHandle, &sessionHandle, &appId) };

    if (ret < 0)
        return ret;

    auto *session { Portal::Get().findSession(sessionHandle) };

    if (!session)
        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Unknown session %s", sessionHandle);

    session->types = Monitor;
    session->cursorMode = Embedded;
    session->persistMode = 0;
    session->restoreOutput.clear();

    if ((ret = sd_bus_message_enter_container(msg, 'a', "{sv}")) < 0)
        return ret;

    while ((ret = sd_bus_message_enter_container(msg, 'e', "sv")) > 0)
    {
        const char *key;

        if ((ret = sd_bus_message_read(msg, "s", &key)) < 0)
            return ret;

        if (strcmp(key, "types") == 0)
            ret = sd_bus_message_read(msg, "v", "u", &session->types);
        else if (strcmp(key, "cursor_mode") == 0)
            ret = sd_bus_message_read(msg, "v", "u", &session->cursorMode);
        else if (strcmp(key, "persist_mode") == 0)
            ret = sd_bus_message_read(msg, "v", "u", &session->persistMode);
        else if (strcmp(key, "restore_data") == 0)
        {
            // (suv): our vendor, version 1, a{sv} { output: s }
            const char *vendor;
            UInt32 version;

            if ((ret = sd_bus_message_enter_container(msg, 'v', "(suv)")) < 0 ||
                (ret = sd_bus_message_enter_container(msg, 'r', "suv")) < 0 ||
                (ret = sd_bus_message_read(msg, "su", &vendor, &version)) < 0)
                return ret;

            if (strcmp(vendor, RestoreVendor) == 0 && version == 1)
            {
                if ((ret = sd_bus_message_enter_container(msg, 'v', "a{sv}")) < 0 ||
                    (ret = sd_bus_message_enter_container(msg, 'a', "{sv}")) < 0)
                    return ret;

                while ((ret = sd_bus_message_enter_container(msg, 'e', "sv")) > 0)
                {
                    const char *rkey;

                    if ((ret = sd_bus_message_read(msg, "s", &rkey)) < 0)
                        return ret;

                    if (strcmp(rkey, "output") == 0)
                    {
                        const char *output;

                        if ((ret = sd_bus_message_read(msg, "v", "s", &output)) < 0)
                            return ret;

                        session->restoreOutput = output;
                    }
                    else if ((ret = sd_bus_message_skip(msg, "v")) < 0)
                        return ret;

                    if ((ret = sd_bus_message_exit_container(msg)) < 0)
                        return ret;
                }

                if ((ret = sd_bus_message_exit_container(msg)) < 0 || (ret = sd_bus_message_exit_container(msg)) < 0)
                    return ret;
            }
            else if ((ret = sd_bus_message_skip(msg, "v")) < 0)
                return ret;

            if ((ret = sd_bus_message_exit_container(msg)) < 0 || (ret = sd_bus_message_exit_container(msg)) < 0)
                return ret;
        }
        else
            ret = sd_bus_message_skip(msg, "v");

        if (ret < 0)
            return ret;

        if ((ret = sd_bus_message_exit_container(msg)) < 0)
            return ret;
    }

    if (ret < 0 || (ret = sd_bus_message_exit_container(msg)) < 0)
        return ret;

    // Metadata cursors aren't supported (yet): embedded instead
    if (session->cursorMode & Metadata)
        session->cursorMode = Embedded;

    // Restored: the same screen, without asking
    session->target = {};

    if (Output *output { session->restoreOutput.empty() ? nullptr : Portal::Get().findOutput(session->restoreOutput) })
    {
        session->target.type = Monitor;
        session->target.output = output;
        session->selected = true;
    }
    else
        session->selected = ChooseTarget(session->types, session->target);

    Log(Info, "SelectSources %s: %s", sessionHandle, session->selected ? "selected" : "cancelled");
    return sd_bus_reply_method_return(msg, "ua{sv}", session->selected ? ResponseSuccess : ResponseCancelled, 0);
}

static int Start(sd_bus_message *msg, void*, sd_bus_error *error)
{
    const char *requestHandle, *sessionHandle, *appId, *parentWindow;
    int ret { sd_bus_message_read(msg, "ooss", &requestHandle, &sessionHandle, &appId, &parentWindow) };

    if (ret < 0)
        return ret;

    if ((ret = SkipOptions(msg)) < 0)
        return ret;

    auto *session { Portal::Get().findSession(sessionHandle) };

    if (!session)
        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Unknown session %s", sessionHandle);

    if (!session->selected || (!session->target.output && !session->target.toplevel))
        return sd_bus_reply_method_return(msg, "ua{sv}", ResponseCancelled, 0);

    // Replied once the stream is ready: the compositor may first ask the user (Session::replyStart())
    if (session->pendingStart)
        sd_bus_message_unref(session->pendingStart);

    session->pendingStart = sd_bus_message_ref(msg);

    if (!session->cast)
    {
        session->cast = std::make_unique<Cast>(*session);

        if (!session->cast->start())
        {
            session->cast.reset();
            session->replyStart(ResponseOther);
        }
    }
    else if (session->cast->nodeId != SPA_ID_INVALID)
        session->replyStart(ResponseSuccess);

    return 1;
}

static int GetAvailableSourceTypes(sd_bus*, const char*, const char*, const char*, sd_bus_message *reply, void*, sd_bus_error*)
{
    UInt32 types { Monitor };

    if (Portal::Get().toplevelSourceManager && Portal::Get().toplevelList)
        types |= Window;

    return sd_bus_message_append(reply, "u", types);
}

static int GetAvailableCursorModes(sd_bus*, const char*, const char*, const char*, sd_bus_message *reply, void*, sd_bus_error*)
{
    return sd_bus_message_append(reply, "u", UInt32(Hidden | Embedded));
}

static int GetVersion(sd_bus*, const char*, const char*, const char*, sd_bus_message *reply, void*, sd_bus_error*)
{
    return sd_bus_message_append(reply, "u", ScreenCastVersion);
}

static const sd_bus_vtable ScreenCastVTable[]
{
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("CreateSession", "oosa{sv}", "ua{sv}", CreateSession, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SelectSources", "oosa{sv}", "ua{sv}", SelectSources, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Start", "oossa{sv}", "ua{sv}", Start, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("AvailableSourceTypes", "u", GetAvailableSourceTypes, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("AvailableCursorModes", "u", GetAvailableCursorModes, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("version", "u", GetVersion, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END
};

bool Portal::initDBus() noexcept
{
    bus = CZBus::GetOrMakeUser();

    if (!bus)
    {
        Log(Error, "Can't connect to the session bus");
        return false;
    }

    int ret { sd_bus_add_object_vtable(bus->bus(), &m_vtableSlot, ObjectPath, ScreenCastInterface, ScreenCastVTable, nullptr) };

    if (ret < 0)
    {
        Log(Error, "Can't add the ScreenCast interface: %s", strerror(-ret));
        return false;
    }

    if ((ret = sd_bus_request_name(bus->bus(), ServiceName, 0)) < 0)
    {
        Log(Error, "Can't own %s: %s (already running?)", ServiceName, strerror(-ret));
        return false;
    }

    Log(Info, "Running as %s", ServiceName);
    return true;
}

/***************************************** SESSION *****************************************/

static int SessionClose(sd_bus_message *msg, void *data, sd_bus_error*)
{
    auto *session { static_cast<Session*>(data) };
    Log(Info, "Session %s closed by the client", session->handle.c_str());
    session->close(false);
    return sd_bus_reply_method_return(msg, "");
}

static const sd_bus_vtable SessionVTable[]
{
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Close", "", "", SessionClose, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("Closed", "", 0),
    SD_BUS_VTABLE_END
};

Session::Session(const std::string &handle) noexcept : handle(handle)
{
    sd_bus_add_object_vtable(Portal::Get().bus->bus(), &m_slot, handle.c_str(), SessionInterface, SessionVTable, this);
}

Session::~Session() noexcept
{
    cast.reset();

    if (pendingStart)
        replyStart(ResponseOther);

    sd_bus_slot_unref(m_slot);
}

void Session::close(bool emitClosed) noexcept
{
    if (emitClosed)
        sd_bus_emit_signal(Portal::Get().bus->bus(), handle.c_str(), SessionInterface, "Closed", "");

    // Destroys this
    Portal::Get().destroySession(this);
}

void Session::replyStart(UInt32 response) noexcept
{
    if (!pendingStart)
        return;

    sd_bus_message *msg { pendingStart };
    pendingStart = nullptr;
    sd_bus_message *reply {};

    if (sd_bus_message_new_method_return(msg, &reply) < 0)
    {
        sd_bus_message_unref(msg);
        return;
    }

    sd_bus_message_append(reply, "u", response);
    sd_bus_message_open_container(reply, 'a', "{sv}");

    if (response == ResponseSuccess && cast)
    {
        // streams: a(ua{sv})
        sd_bus_message_open_container(reply, 'e', "sv");
        sd_bus_message_append(reply, "s", "streams");
        sd_bus_message_open_container(reply, 'v', "a(ua{sv})");
        sd_bus_message_open_container(reply, 'a', "(ua{sv})");
        sd_bus_message_open_container(reply, 'r', "ua{sv}");
        sd_bus_message_append(reply, "u", cast->nodeId);
        sd_bus_message_open_container(reply, 'a', "{sv}");

        if (target.output)
        {
            sd_bus_message_append(reply, "{sv}", "position", "(ii)", target.output->x, target.output->y);
            sd_bus_message_append(reply, "{sv}", "size", "(ii)", target.output->width, target.output->height);
        }

        sd_bus_message_append(reply, "{sv}", "source_type", "u", UInt32(target.type));
        sd_bus_message_close_container(reply); // a{sv}
        sd_bus_message_close_container(reply); // r
        sd_bus_message_close_container(reply); // a
        sd_bus_message_close_container(reply); // v
        sd_bus_message_close_container(reply); // e

        // Restore token: the same screen next time
        if (persistMode != 0 && target.type == Monitor && target.output)
        {
            sd_bus_message_append(reply, "{sv}", "persist_mode", "u", persistMode);
            sd_bus_message_open_container(reply, 'e', "sv");
            sd_bus_message_append(reply, "s", "restore_data");
            sd_bus_message_open_container(reply, 'v', "(suv)");
            sd_bus_message_open_container(reply, 'r', "suv");
            sd_bus_message_append(reply, "su", RestoreVendor, UInt32(1));
            sd_bus_message_open_container(reply, 'v', "a{sv}");
            sd_bus_message_open_container(reply, 'a', "{sv}");
            sd_bus_message_append(reply, "{sv}", "output", "s", target.output->name.c_str());
            sd_bus_message_close_container(reply); // a{sv}
            sd_bus_message_close_container(reply); // v
            sd_bus_message_close_container(reply); // r
            sd_bus_message_close_container(reply); // v
            sd_bus_message_close_container(reply); // e
        }
    }

    sd_bus_message_close_container(reply);
    sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    sd_bus_message_unref(msg);
}
