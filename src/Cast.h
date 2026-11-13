#ifndef CAST_H
#define CAST_H

#include <Portal.h>
#include <spa/param/video/format-utils.h>
#include <CZ/Core/CZTimer.h>
#include <vector>

struct ext_image_copy_capture_session_v1;
struct ext_image_copy_capture_frame_v1;
struct ext_image_capture_source_v1;

/// A rect of damage in buffer pixels.
struct Damage { Int32 x, y, width, height; };

/// A PipeWire buffer and the wl_buffer the compositor copies frames into.
struct CastBuffer
{
    bool dmabuf {};
    UInt32 width {}, height {}, format {};
    UInt64 modifier {};
    int planes {};
    int fd[4] { -1, -1, -1, -1 };
    UInt32 size[4] {}, stride[4] {}, offset[4] {};
    wl_buffer *buffer {};
    std::vector<Damage> damage; // Stale since this buffer was last filled
    ~CastBuffer() noexcept;
};

/// Buffer constraints of the capture session.
struct Constraints
{
    UInt32 width {}, height {};
    std::vector<UInt32> shmFormats;                       // DRM fourcc
    std::vector<std::pair<UInt32, UInt64>> dmabufFormats; // DRM fourcc, modifier
    dev_t device {};
    bool hasDevice {};
    bool operator==(const Constraints &o) const noexcept
    {
        return width == o.width && height == o.height && shmFormats == o.shmFormats &&
               dmabufFormats == o.dmabufFormats && device == o.device && hasDevice == o.hasDevice;
    }
};

/**
 * @brief A screen cast: an ext-image-copy-capture session feeding a PipeWire video source.
 *
 * PipeWire asks for frames (process), each is captured into one of the stream's buffers and queued
 * with its damage and timestamp. The compositor sends a frame only once something changed (the
 * first one right away), which paces the stream.
 */
class Cast
{
public:
    Cast(Session &session) noexcept;
    ~Cast() noexcept;

    bool start() noexcept;

    /// The PipeWire node id (SPA_ID_INVALID until the stream is ready).
    UInt32 nodeId { SPA_ID_INVALID };

    /// The session was stopped by the compositor (denied, source destroyed).
    bool stopped { false };

    Session &session;

    // Capture session events
    void constraintsDone() noexcept;
    void sessionStopped() noexcept;
    Constraints pending, current;
    bool hasConstraints { false };

    // Frame events
    void frameDamage(const Damage &damage) noexcept;
    void frameReady() noexcept;
    void frameFailed(UInt32 reason) noexcept;
    UInt64 frameSec {}; UInt32 frameNsec {};
    UInt32 frameTransform {};
    std::vector<Damage> frameDamages;

    // PipeWire stream events
    static void onStateChanged(void *data, pw_stream_state old, pw_stream_state state, const char *error);
    static void onParamChanged(void *data, UInt32 id, const spa_pod *param);
    static void onAddBuffer(void *data, pw_buffer *buffer);
    static void onRemoveBuffer(void *data, pw_buffer *buffer);
    static void onProcess(void *data);

private:
    void createStream() noexcept;
    void updateParams() noexcept;
    void buildFormats(spa_pod_builder &b, std::vector<const spa_pod*> &params) noexcept;
    void captureFrame() noexcept;
    void queueBuffer(bool corrupted) noexcept;
    CastBuffer *createBuffer() noexcept;
    void stopAndClose() noexcept;

    pw_stream *m_stream {};
    spa_hook m_streamListener {};
    spa_video_info_raw m_format {};
    bool m_dmabuf { false }, m_avoidDmabuf { false }, m_streaming { false };
    UInt32 m_seq {};

    // Capture
    ext_image_capture_source_v1 *m_source {};
    ext_image_copy_capture_session_v1 *m_captureSession {};
    ext_image_copy_capture_frame_v1 *m_frame {};
    pw_buffer *m_pwBuffer {}; // Being filled
    std::vector<CastBuffer*> m_buffers; // Every buffer of the stream (owned through pw_buffer::user_data)
    std::unique_ptr<CZTimer> m_retry;
};

#endif // CAST_H
