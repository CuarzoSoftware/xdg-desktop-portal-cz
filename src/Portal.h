#ifndef PORTAL_H
#define PORTAL_H

#include <CZ/Core/CZCore.h>
#include <CZ/Core/CZBus.h>
#include <CZ/Core/CZEventSource.h>
#include <wayland-client.h>
#include <pipewire/pipewire.h>
#include <systemd/sd-bus.h>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace CZ;

struct gbm_device;
struct zxdg_output_manager_v1;
struct zxdg_output_v1;
struct zwp_linux_dmabuf_v1;
struct ext_output_image_capture_source_manager_v1;
struct ext_foreign_toplevel_image_capture_source_manager_v1;
struct ext_image_copy_capture_manager_v1;
struct ext_foreign_toplevel_list_v1;
struct ext_foreign_toplevel_handle_v1;

class Cast;
class Session;

// Logging (CZ_PORTAL_LOG_LEVEL: 0 none, 1 errors, 2 warnings, 3 info, 4 debug)
enum LogLevel { Error = 1, Warning = 2, Info = 3, Debug = 4 };
void Log(LogLevel level, const char *format, ...) __attribute__((format(printf, 2, 3)));

/// A screen (wl_output) the compositor exposes.
struct Output
{
    UInt32 globalName;
    wl_output *output {};
    zxdg_output_v1 *xdgOutput {};
    std::string name, description;
    Int32 x {}, y {}, width {}, height {}; // Logical (xdg-output)
};

/// A window (ext-foreign-toplevel-list).
struct Toplevel
{
    ext_foreign_toplevel_handle_v1 *handle {};
    std::string title, appId, identifier;
};

/**
 * @brief The portal process: the Wayland connection (Louvre's capture protocols), PipeWire and the
 *        org.freedesktop.impl.portal.ScreenCast D-Bus implementation.
 *
 * Everything runs on CZCore's event loop.
 */
class Portal
{
public:
    static Portal &Get() noexcept;

    bool init() noexcept;
    int run() noexcept;

    // Wayland
    wl_display *display {};
    wl_registry *registry {};
    wl_shm *shm {};
    zwp_linux_dmabuf_v1 *linuxDmabuf {};
    zxdg_output_manager_v1 *xdgOutputManager {};
    ext_output_image_capture_source_manager_v1 *outputSourceManager {};
    ext_foreign_toplevel_image_capture_source_manager_v1 *toplevelSourceManager {};
    ext_image_copy_capture_manager_v1 *copyCaptureManager {};
    ext_foreign_toplevel_list_v1 *toplevelList {};
    std::vector<std::unique_ptr<Output>> outputs;
    std::vector<std::unique_ptr<Toplevel>> toplevels;

    Output *findOutput(const std::string &name) const noexcept;
    Output *findOutput(wl_output *output) const noexcept;

    // GBM device for dma-buf buffers (the one the compositor renders with)
    gbm_device *gbmForDevice(dev_t device) noexcept;

    // PipeWire
    pw_loop *pwLoop {};
    pw_context *pwContext {};
    pw_core *pwCore {};
    spa_hook pwCoreListener {};

    // D-Bus
    std::shared_ptr<CZBus> bus;
    std::vector<std::unique_ptr<Session>> sessions;
    Session *findSession(const std::string &handle) const noexcept;
    void destroySession(Session *session) noexcept;

private:
    Portal() noexcept = default;
    bool initWayland() noexcept;
    bool initPipeWire() noexcept;
    bool initDBus() noexcept;

    std::shared_ptr<CZCore> m_core;
    std::shared_ptr<CZEventSource> m_waylandSource, m_pipewireSource;
    sd_bus_slot *m_vtableSlot {};
    gbm_device *m_gbm {};
    dev_t m_gbmDevice {};
};

/// Source types (org.freedesktop.portal.ScreenCast)
enum SourceType : UInt32 { Monitor = 1, Window = 2 };

/// Cursor modes
enum CursorMode : UInt32 { Hidden = 1, Embedded = 2, Metadata = 4 };

/// What a session casts.
struct Target
{
    SourceType type { Monitor };
    Output *output {};     // Monitor
    Toplevel *toplevel {}; // Window
};

/**
 * @brief A screen cast session (org.freedesktop.impl.portal.Session): what was selected and the
 *        stream, once started.
 */
class Session
{
public:
    Session(const std::string &handle) noexcept;
    ~Session() noexcept;

    std::string handle;
    std::string appId;
    UInt32 types { Monitor };
    UInt32 cursorMode { Embedded };
    UInt32 persistMode { 0 };
    std::string restoreOutput; // Output name from restore_data
    bool selected { false };
    Target target;
    std::unique_ptr<Cast> cast;

    // Start's reply, sent once the stream is ready (the compositor may ask the user first)
    sd_bus_message *pendingStart {};
    void replyStart(UInt32 response) noexcept;

    // Session closed (by the client or because the cast stopped)
    void close(bool emitClosed) noexcept;

private:
    sd_bus_slot *m_slot {};
};

#endif // PORTAL_H
