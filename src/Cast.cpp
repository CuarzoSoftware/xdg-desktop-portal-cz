#include <Cast.h>
#include <ext-image-capture-source-v1-client-protocol.h>
#include <ext-image-copy-capture-v1-client-protocol.h>
#include <linux-dmabuf-v1-client-protocol.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/format-utils.h>
#include <spa/pod/dynamic.h>
#include <spa/utils/result.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>

// Based on xdg-desktop-portal-wlr's screen casting (MIT), for Louvre's capture protocols

static constexpr UInt32 PipeWireBuffers { 3 }, PipeWireBuffersMin { 2 }, PipeWireBuffersMax { 16 };
static constexpr UInt32 PipeWireAlign { 16 };
static constexpr UInt32 DamageRegions { 16 };

/***************************************** FORMATS *****************************************/

static spa_video_format SpaFromDrm(UInt32 format) noexcept
{
    switch (format)
    {
    case DRM_FORMAT_ARGB8888: return SPA_VIDEO_FORMAT_BGRA;
    case DRM_FORMAT_XRGB8888: return SPA_VIDEO_FORMAT_BGRx;
    case DRM_FORMAT_RGBA8888: return SPA_VIDEO_FORMAT_ABGR;
    case DRM_FORMAT_RGBX8888: return SPA_VIDEO_FORMAT_xBGR;
    case DRM_FORMAT_ABGR8888: return SPA_VIDEO_FORMAT_RGBA;
    case DRM_FORMAT_XBGR8888: return SPA_VIDEO_FORMAT_RGBx;
    case DRM_FORMAT_BGRA8888: return SPA_VIDEO_FORMAT_ARGB;
    case DRM_FORMAT_BGRX8888: return SPA_VIDEO_FORMAT_xRGB;
    default:                  return SPA_VIDEO_FORMAT_UNKNOWN;
    }
}

static UInt32 DrmFromSpa(spa_video_format format) noexcept
{
    switch (format)
    {
    case SPA_VIDEO_FORMAT_BGRA: return DRM_FORMAT_ARGB8888;
    case SPA_VIDEO_FORMAT_BGRx: return DRM_FORMAT_XRGB8888;
    case SPA_VIDEO_FORMAT_ABGR: return DRM_FORMAT_RGBA8888;
    case SPA_VIDEO_FORMAT_xBGR: return DRM_FORMAT_RGBX8888;
    case SPA_VIDEO_FORMAT_RGBA: return DRM_FORMAT_ABGR8888;
    case SPA_VIDEO_FORMAT_RGBx: return DRM_FORMAT_XBGR8888;
    case SPA_VIDEO_FORMAT_ARGB: return DRM_FORMAT_BGRA8888;
    case SPA_VIDEO_FORMAT_xRGB: return DRM_FORMAT_BGRX8888;
    default:                    return DRM_FORMAT_INVALID;
    }
}

// The same format without alpha (consumers often only take those)
static spa_video_format StripAlpha(spa_video_format format) noexcept
{
    switch (format)
    {
    case SPA_VIDEO_FORMAT_BGRA: return SPA_VIDEO_FORMAT_BGRx;
    case SPA_VIDEO_FORMAT_ABGR: return SPA_VIDEO_FORMAT_xBGR;
    case SPA_VIDEO_FORMAT_RGBA: return SPA_VIDEO_FORMAT_RGBx;
    case SPA_VIDEO_FORMAT_ARGB: return SPA_VIDEO_FORMAT_xRGB;
    default:                    return SPA_VIDEO_FORMAT_UNKNOWN;
    }
}

static UInt32 WlShmFromDrm(UInt32 format) noexcept
{
    switch (format)
    {
    case DRM_FORMAT_ARGB8888: return WL_SHM_FORMAT_ARGB8888;
    case DRM_FORMAT_XRGB8888: return WL_SHM_FORMAT_XRGB8888;
    default:                  return format;
    }
}

static UInt32 DrmFromWlShm(UInt32 format) noexcept
{
    switch (format)
    {
    case WL_SHM_FORMAT_ARGB8888: return DRM_FORMAT_ARGB8888;
    case WL_SHM_FORMAT_XRGB8888: return DRM_FORMAT_XRGB8888;
    default:                     return format;
    }
}

// Enumerates a video format (with its modifiers for dma-bufs)
static const spa_pod *BuildFormat(spa_pod_builder &b, spa_video_format format, UInt32 width, UInt32 height,
                                  const std::vector<UInt64> &modifiers, const UInt64 *fixedModifier = nullptr) noexcept
{
    spa_pod_frame f[2];
    const spa_video_format noAlpha { StripAlpha(format) };

    spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(&b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
    spa_pod_builder_add(&b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);

    // Modifiers are tied to their exact format: no alpha-less alternative then
    if (!modifiers.empty() || fixedModifier || noAlpha == SPA_VIDEO_FORMAT_UNKNOWN)
        spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);
    else
        spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(3, format, format, noAlpha), 0);

    if (fixedModifier)
    {
        spa_pod_builder_prop(&b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
        spa_pod_builder_long(&b, *fixedModifier);
    }
    else if (!modifiers.empty())
    {
        spa_pod_builder_prop(&b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
        spa_pod_builder_push_choice(&b, &f[1], SPA_CHOICE_Enum, 0);
        spa_pod_builder_long(&b, modifiers[0]); // Default

        for (const UInt64 modifier : modifiers)
            spa_pod_builder_long(&b, modifier);

        spa_pod_builder_pop(&b, &f[1]);
    }

    const spa_rectangle size { width, height };
    spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size), 0);

    // Variable framerate (frames come when something changes)
    const spa_fraction variable { 0, 1 };
    spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&variable), 0);

    const spa_fraction maxDefault { 60, 1 }, maxMin { 1, 1 }, maxMax { 360, 1 };
    spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(&maxDefault, &maxMin, &maxMax), 0);

    return static_cast<const spa_pod*>(spa_pod_builder_pop(&b, &f[0]));
}

/***************************************** BUFFERS *****************************************/

CastBuffer::~CastBuffer() noexcept
{
    if (buffer)
        wl_buffer_destroy(buffer);

    for (int i = 0; i < 4; i++)
        if (fd[i] >= 0)
            close(fd[i]);
}

CastBuffer *Cast::createBuffer() noexcept
{
    auto &portal { Portal::Get() };
    auto buffer { std::make_unique<CastBuffer>() };
    buffer->width = current.width;
    buffer->height = current.height;
    buffer->format = DrmFromSpa(m_format.format);
    buffer->dmabuf = m_dmabuf;

    // New buffers have no content: everything is stale
    buffer->damage.push_back({ 0, 0, Int32(buffer->width), Int32(buffer->height) });

    if (buffer->format == DRM_FORMAT_INVALID)
        return nullptr;

    if (!m_dmabuf)
    {
        const UInt32 stride { 4 * buffer->width };
        buffer->planes = 1;
        buffer->stride[0] = stride;
        buffer->size[0] = stride * buffer->height;
        buffer->fd[0] = memfd_create("xdg-desktop-portal-cz", MFD_CLOEXEC | MFD_ALLOW_SEALING);

        if (buffer->fd[0] < 0 || ftruncate(buffer->fd[0], buffer->size[0]) < 0 || !portal.shm)
            return nullptr;

        auto *pool { wl_shm_create_pool(portal.shm, buffer->fd[0], buffer->size[0]) };
        buffer->buffer = wl_shm_pool_create_buffer(pool, 0, buffer->width, buffer->height, stride, WlShmFromDrm(buffer->format));
        wl_shm_pool_destroy(pool);
        return buffer->buffer ? buffer.release() : nullptr;
    }

    auto *gbm { portal.gbmForDevice(current.device) };

    if (!gbm || !portal.linuxDmabuf)
        return nullptr;

    gbm_bo *bo {};

    if (m_format.modifier != DRM_FORMAT_MOD_INVALID)
        bo = gbm_bo_create_with_modifiers2(gbm, buffer->width, buffer->height, buffer->format, &m_format.modifier, 1, GBM_BO_USE_RENDERING);
    else
        bo = gbm_bo_create(gbm, buffer->width, buffer->height, buffer->format, GBM_BO_USE_RENDERING);

    if (!bo && m_format.modifier == DRM_FORMAT_MOD_LINEAR)
        bo = gbm_bo_create(gbm, buffer->width, buffer->height, buffer->format, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);

    if (!bo)
        return nullptr;

    buffer->planes = std::min(gbm_bo_get_plane_count(bo), 4);
    buffer->modifier = gbm_bo_get_modifier(bo);
    auto *params { zwp_linux_dmabuf_v1_create_params(portal.linuxDmabuf) };

    for (int plane = 0; plane < buffer->planes; plane++)
    {
        buffer->stride[plane] = gbm_bo_get_stride_for_plane(bo, plane);
        buffer->offset[plane] = gbm_bo_get_offset(bo, plane);
        buffer->fd[plane] = gbm_bo_get_fd_for_plane(bo, plane);

        if (buffer->fd[plane] < 0)
        {
            zwp_linux_buffer_params_v1_destroy(params);
            gbm_bo_destroy(bo);
            return nullptr;
        }

        zwp_linux_buffer_params_v1_add(params, buffer->fd[plane], plane, buffer->offset[plane], buffer->stride[plane],
                                       buffer->modifier >> 32, buffer->modifier & 0xffffffff);
    }

    buffer->buffer = zwp_linux_buffer_params_v1_create_immed(params, buffer->width, buffer->height, buffer->format, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    gbm_bo_destroy(bo);
    return buffer->buffer ? buffer.release() : nullptr;
}

/************************************* CAPTURE SESSION *************************************/

static void SessionBufferSize(void *data, ext_image_copy_capture_session_v1*, uint32_t width, uint32_t height)
{
    auto *cast { static_cast<Cast*>(data) };
    cast->pending.width = width;
    cast->pending.height = height;
}

static void SessionShmFormat(void *data, ext_image_copy_capture_session_v1*, uint32_t format)
{
    auto *cast { static_cast<Cast*>(data) };
    const UInt32 fourcc { DrmFromWlShm(format) };

    // 32 bpp formats only (stride = 4 * width)
    if (SpaFromDrm(fourcc) != SPA_VIDEO_FORMAT_UNKNOWN &&
        std::find(cast->pending.shmFormats.begin(), cast->pending.shmFormats.end(), fourcc) == cast->pending.shmFormats.end())
        cast->pending.shmFormats.push_back(fourcc);
}

static void SessionDmabufDevice(void *data, ext_image_copy_capture_session_v1*, wl_array *device)
{
    auto *cast { static_cast<Cast*>(data) };

    if (device->size == sizeof(dev_t))
    {
        memcpy(&cast->pending.device, device->data, sizeof(dev_t));
        cast->pending.hasDevice = true;
    }
}

static void SessionDmabufFormat(void *data, ext_image_copy_capture_session_v1*, uint32_t format, wl_array *modifiers)
{
    auto *cast { static_cast<Cast*>(data) };

    if (SpaFromDrm(format) == SPA_VIDEO_FORMAT_UNKNOWN)
        return;

    const auto *mods { static_cast<const UInt64*>(modifiers->data) };

    for (size_t i = 0; i < modifiers->size / sizeof(UInt64); i++)
        cast->pending.dmabufFormats.emplace_back(format, mods[i]);
}

static void SessionDone(void *data, ext_image_copy_capture_session_v1*)
{
    static_cast<Cast*>(data)->constraintsDone();
}

static void SessionStopped(void *data, ext_image_copy_capture_session_v1*)
{
    static_cast<Cast*>(data)->sessionStopped();
}

static const ext_image_copy_capture_session_v1_listener CaptureSessionListener
{
    .buffer_size = SessionBufferSize,
    .shm_format = SessionShmFormat,
    .dmabuf_device = SessionDmabufDevice,
    .dmabuf_format = SessionDmabufFormat,
    .done = SessionDone,
    .stopped = SessionStopped
};

static void FrameTransform(void *data, ext_image_copy_capture_frame_v1*, uint32_t transform)
{
    static_cast<Cast*>(data)->frameTransform = transform;
}

static void FrameDamage(void *data, ext_image_copy_capture_frame_v1*, int32_t x, int32_t y, int32_t width, int32_t height)
{
    static_cast<Cast*>(data)->frameDamage({ x, y, width, height });
}

static void FramePresentationTime(void *data, ext_image_copy_capture_frame_v1*, uint32_t secHi, uint32_t secLo, uint32_t nsec)
{
    auto *cast { static_cast<Cast*>(data) };
    cast->frameSec = (UInt64(secHi) << 32) | secLo;
    cast->frameNsec = nsec;
}

static void FrameReady(void *data, ext_image_copy_capture_frame_v1*)
{
    static_cast<Cast*>(data)->frameReady();
}

static void FrameFailed(void *data, ext_image_copy_capture_frame_v1*, uint32_t reason)
{
    static_cast<Cast*>(data)->frameFailed(reason);
}

static const ext_image_copy_capture_frame_v1_listener FrameListener
{
    .transform = FrameTransform,
    .damage = FrameDamage,
    .presentation_time = FramePresentationTime,
    .ready = FrameReady,
    .failed = FrameFailed
};

/****************************************** CAST *******************************************/

Cast::Cast(Session &session) noexcept : session(session)
{
    m_retry = std::make_unique<CZTimer>([this](CZTimer*){
        if (m_stream)
            pw_stream_trigger_process(m_stream);
    });
}

Cast::~Cast() noexcept
{
    m_retry.reset();

    if (m_frame)
        ext_image_copy_capture_frame_v1_destroy(m_frame);

    if (m_stream)
    {
        pw_stream_flush(m_stream, false);
        pw_stream_disconnect(m_stream);
        pw_stream_destroy(m_stream); // Removes the buffers (onRemoveBuffer)
    }

    if (m_captureSession)
        ext_image_copy_capture_session_v1_destroy(m_captureSession);

    if (m_source)
        ext_image_capture_source_v1_destroy(m_source);

    wl_display_flush(Portal::Get().display);
}

bool Cast::start() noexcept
{
    auto &portal { Portal::Get() };
    const Target &target { session.target };

    if (target.type == Monitor && target.output && portal.outputSourceManager)
        m_source = ext_output_image_capture_source_manager_v1_create_source(portal.outputSourceManager, target.output->output);
    else if (target.type == Window && target.toplevel && portal.toplevelSourceManager)
        m_source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(portal.toplevelSourceManager, target.toplevel->handle);

    if (!m_source)
        return false;

    const bool cursor { (session.cursorMode & Embedded) != 0 };
    m_captureSession = ext_image_copy_capture_manager_v1_create_session(portal.copyCaptureManager, m_source,
        cursor ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : 0);
    ext_image_copy_capture_session_v1_add_listener(m_captureSession, &CaptureSessionListener, this);
    wl_display_flush(portal.display);

    // The stream is created once the compositor accepts the session and sends its constraints
    Log(Info, "Capture session requested (%s)", target.type == Monitor ? target.output->name.c_str() : target.toplevel->title.c_str());
    return true;
}

void Cast::constraintsDone() noexcept
{
    Constraints next { std::move(pending) };
    pending = {};

    if (hasConstraints && next == current)
        return;

    current = std::move(next);
    Log(Debug, "Constraints: %ux%u, %zu shm, %zu dma-buf formats", current.width, current.height, current.shmFormats.size(), current.dmabufFormats.size());

    if (!hasConstraints)
    {
        // Accepted: start streaming
        hasConstraints = true;
        createStream();
    }
    else
        updateParams(); // Renegotiate (e.g. resized)
}

void Cast::sessionStopped() noexcept
{
    Log(Info, "Capture session stopped by the compositor");
    stopped = true;

    // Denied before starting: cancelled
    if (session.pendingStart)
        session.replyStart(1);

    stopAndClose();
}

void Cast::stopAndClose() noexcept
{
    // Not from within the callbacks of this cast: the session (and this) is destroyed
    const std::string handle { session.handle };

    CZTimer::OneShot(0, [handle](CZTimer*){
        if (auto *s { Portal::Get().findSession(handle) })
            s->close(true);
    });
}

void Cast::frameDamage(const Damage &damage) noexcept
{
    frameDamages.push_back(damage);
}

void Cast::frameReady() noexcept
{
    if (m_frame)
    {
        ext_image_copy_capture_frame_v1_destroy(m_frame);
        m_frame = nullptr;
    }

    if (!m_pwBuffer)
        return;

    // Queued with its damage (the other buffers get it, see queueBuffer()): it's up to date
    auto *filled { static_cast<CastBuffer*>(m_pwBuffer->user_data) };
    queueBuffer(false);

    if (filled)
        filled->damage.clear();

    frameDamages.clear();

    // Next frame (the compositor sends it once something changes)
    if (m_stream && m_streaming)
        pw_stream_trigger_process(m_stream);
}

void Cast::frameFailed(UInt32 reason) noexcept
{
    if (m_frame)
    {
        ext_image_copy_capture_frame_v1_destroy(m_frame);
        m_frame = nullptr;
    }

    frameDamages.clear();

    switch (reason)
    {
    case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS:
        // New constraints follow (renegotiated in constraintsDone())
        Log(Info, "Frame failed: buffer constraints changed");
        queueBuffer(true);
        break;
    case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED:
        queueBuffer(true);
        break; // `stopped` follows
    default:
        Log(Error, "Frame failed: unknown reason");
        queueBuffer(true);
        stopAndClose();
        break;
    }
}

void Cast::captureFrame() noexcept
{
    auto *buffer { static_cast<CastBuffer*>(m_pwBuffer->user_data) };

    if (!buffer || !m_captureSession || stopped)
    {
        queueBuffer(true);
        return;
    }

    m_frame = ext_image_copy_capture_session_v1_create_frame(m_captureSession);
    ext_image_copy_capture_frame_v1_add_listener(m_frame, &FrameListener, this);
    ext_image_copy_capture_frame_v1_attach_buffer(m_frame, buffer->buffer);

    // What this buffer misses (the compositor copies at least that, plus what changed)
    for (const Damage &d : buffer->damage)
        if (d.width > 0 && d.height > 0)
            ext_image_copy_capture_frame_v1_damage_buffer(m_frame, d.x, d.y, d.width, d.height);

    ext_image_copy_capture_frame_v1_capture(m_frame);
    wl_display_flush(Portal::Get().display);
}

void Cast::queueBuffer(bool corrupted) noexcept
{
    if (!m_pwBuffer || !m_stream)
        return;

    pw_buffer *pwBuffer { m_pwBuffer };
    m_pwBuffer = nullptr;
    spa_buffer *buffer { pwBuffer->buffer };
    auto *castBuffer { static_cast<CastBuffer*>(pwBuffer->user_data) };

    // The other buffers miss what changed in this frame
    if (!corrupted && castBuffer)
    {
        for (auto *b : m_buffers)
            if (b != castBuffer)
                b->damage.insert(b->damage.end(), frameDamages.begin(), frameDamages.end());
    }

    if (auto *header { static_cast<spa_meta_header*>(spa_buffer_find_meta_data(buffer, SPA_META_Header, sizeof(spa_meta_header))) })
    {
        header->pts = Int64(frameSec * SPA_NSEC_PER_SEC + frameNsec);
        header->flags = corrupted ? SPA_META_HEADER_FLAG_CORRUPTED : 0;
        header->seq = m_seq++;
        header->dts_offset = 0;
    }

    if (auto *transform { static_cast<spa_meta_videotransform*>(spa_buffer_find_meta_data(buffer, SPA_META_VideoTransform, sizeof(spa_meta_videotransform))) })
        transform->transform = frameTransform;

    // Damage: up to the metadata's room, the rest merged into the last region
    if (auto *meta { spa_buffer_find_meta(buffer, SPA_META_VideoDamage) })
    {
        auto *region { static_cast<spa_meta_region*>(spa_meta_first(meta)) };
        const UInt32 count { UInt32(meta->size / sizeof(spa_meta_region)) };
        UInt32 i { 0 };

        for (const Damage &d : frameDamages)
        {
            if (i < count)
            {
                region[i].region = SPA_REGION(d.x, d.y, UInt32(d.width), UInt32(d.height));
                i++;
                continue;
            }

            // Merge into the last one
            spa_region &last { region[count - 1].region };
            const Int32 x1 { std::min(last.position.x, d.x) }, y1 { std::min(last.position.y, d.y) };
            const Int32 x2 { std::max(last.position.x + Int32(last.size.width), d.x + d.width) };
            const Int32 y2 { std::max(last.position.y + Int32(last.size.height), d.y + d.height) };
            last = SPA_REGION(x1, y1, UInt32(x2 - x1), UInt32(y2 - y1));
        }

        // Ends with an empty region
        if (i < count)
            region[i].region = SPA_REGION(0, 0, 0, 0);
    }

    for (UInt32 plane = 0; plane < buffer->n_datas; plane++)
        buffer->datas[plane].chunk->flags = corrupted ? SPA_CHUNK_FLAG_CORRUPTED : SPA_CHUNK_FLAG_NONE;

    pw_stream_queue_buffer(m_stream, pwBuffer);
}

/***************************************** STREAM ******************************************/

static const pw_stream_events StreamEvents
{
    .version = PW_VERSION_STREAM_EVENTS,
    .destroy = nullptr,
    .state_changed = &Cast::onStateChanged,
    .control_info = nullptr,
    .io_changed = nullptr,
    .param_changed = &Cast::onParamChanged,
    .add_buffer = &Cast::onAddBuffer,
    .remove_buffer = &Cast::onRemoveBuffer,
    .process = &Cast::onProcess,
    .drained = nullptr,
    .command = nullptr,
    .trigger_done = nullptr
};

void Cast::buildFormats(spa_pod_builder &b, std::vector<const spa_pod*> &params) noexcept
{
    // dma-bufs (zero copy) first, then shm
    if (!m_avoidDmabuf && current.hasDevice && Portal::Get().linuxDmabuf)
    {
        std::vector<UInt32> formats;

        for (const auto &[format, modifier] : current.dmabufFormats)
            if (std::find(formats.begin(), formats.end(), format) == formats.end())
                formats.push_back(format);

        for (const UInt32 format : formats)
        {
            std::vector<UInt64> modifiers;

            for (const auto &[f, modifier] : current.dmabufFormats)
                if (f == format)
                    modifiers.push_back(modifier);

            params.push_back(BuildFormat(b, SpaFromDrm(format), current.width, current.height, modifiers));
        }
    }

    for (const UInt32 format : current.shmFormats)
        params.push_back(BuildFormat(b, SpaFromDrm(format), current.width, current.height, {}));
}

void Cast::createStream() noexcept
{
    auto &portal { Portal::Get() };

    m_stream = pw_stream_new(portal.pwCore, "xdg-desktop-portal-cz",
        pw_properties_new(PW_KEY_MEDIA_CLASS, "Video/Source", nullptr));

    if (!m_stream)
    {
        Log(Error, "Can't create the PipeWire stream");
        stopAndClose();
        return;
    }

    pw_stream_add_listener(m_stream, &m_streamListener, &StreamEvents, this);

    UInt8 storage[4096];
    spa_pod_dynamic_builder builder;
    spa_pod_dynamic_builder_init(&builder, storage, sizeof(storage), 4096);
    std::vector<const spa_pod*> params;
    buildFormats(builder.b, params);

    pw_stream_connect(m_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_ALLOC_BUFFERS | PW_STREAM_FLAG_DRIVER),
        params.data(), params.size());

    spa_pod_dynamic_builder_clean(&builder);
}

void Cast::updateParams() noexcept
{
    if (!m_stream)
        return;

    UInt8 storage[4096];
    spa_pod_dynamic_builder builder;
    spa_pod_dynamic_builder_init(&builder, storage, sizeof(storage), 4096);
    std::vector<const spa_pod*> params;
    buildFormats(builder.b, params);
    pw_stream_update_params(m_stream, params.data(), params.size());
    spa_pod_dynamic_builder_clean(&builder);
}

void Cast::onStateChanged(void *data, pw_stream_state old, pw_stream_state state, const char *error)
{
    auto *cast { static_cast<Cast*>(data) };
    cast->nodeId = pw_stream_get_node_id(cast->m_stream);
    Log(Info, "Stream %s (node %u)%s%s", pw_stream_state_as_string(state), cast->nodeId, error ? ": " : "", error ? error : "");

    // Ready to be consumed: Start's reply
    if (cast->nodeId != SPA_ID_INVALID && cast->session.pendingStart && state != PW_STREAM_STATE_ERROR)
        cast->session.replyStart(0);

    switch (state)
    {
    case PW_STREAM_STATE_STREAMING:
        cast->m_streaming = true;
        pw_stream_trigger_process(cast->m_stream);
        break;
    case PW_STREAM_STATE_PAUSED:
        if (old == PW_STREAM_STATE_STREAMING && cast->m_pwBuffer)
        {
            if (cast->m_frame)
            {
                ext_image_copy_capture_frame_v1_destroy(cast->m_frame);
                cast->m_frame = nullptr;
            }

            cast->queueBuffer(true);
        }
        cast->m_streaming = false;
        cast->m_retry->stop();
        break;
    case PW_STREAM_STATE_ERROR:
        if (cast->session.pendingStart)
            cast->session.replyStart(2);
        cast->m_streaming = false;
        cast->stopAndClose();
        break;
    default:
        cast->m_streaming = false;
        cast->m_retry->stop();
        break;
    }
}

void Cast::onParamChanged(void *data, UInt32 id, const spa_pod *param)
{
    auto *cast { static_cast<Cast*>(data) };

    if (!param || id != SPA_PARAM_Format)
        return;

    spa_format_video_raw_parse(param, &cast->m_format);

    UInt8 storage[4096];
    spa_pod_dynamic_builder builder;
    spa_pod_dynamic_builder_init(&builder, storage, sizeof(storage), 4096);
    std::vector<const spa_pod*> params;
    UInt32 blocks { 1 }, dataType;

    if (const spa_pod_prop *modifierProp { spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier) })
    {
        cast->m_dmabuf = true;
        dataType = 1 << SPA_DATA_DmaBuf;
        const UInt32 fourcc { DrmFromSpa(cast->m_format.format) };
        auto *gbm { Portal::Get().gbmForDevice(cast->current.device) };

        // Not fixated yet: pick a modifier we can allocate, then fixate it
        if (modifierProp->flags & SPA_POD_PROP_FLAG_DONT_FIXATE)
        {
            const spa_pod *choice { &modifierProp->value };
            const UInt32 count { SPA_POD_CHOICE_N_VALUES(choice) };
            const auto *values { static_cast<const UInt64*>(SPA_POD_CHOICE_VALUES(choice)) };
            std::vector<UInt64> modifiers;

            for (UInt32 i = 1; i < count; i++) // [0] is the default
                modifiers.push_back(values[i]);

            gbm_bo *bo { gbm && !modifiers.empty() ?
                gbm_bo_create_with_modifiers2(gbm, cast->current.width, cast->current.height, fourcc, modifiers.data(), modifiers.size(), GBM_BO_USE_RENDERING) : nullptr };

            if (!bo && gbm)
            {
                // Implicit modifiers
                for (const UInt64 modifier : modifiers)
                    if (modifier == DRM_FORMAT_MOD_INVALID || modifier == DRM_FORMAT_MOD_LINEAR)
                        if ((bo = gbm_bo_create(gbm, cast->current.width, cast->current.height, fourcc,
                                                GBM_BO_USE_RENDERING | (modifier == DRM_FORMAT_MOD_LINEAR ? GBM_BO_USE_LINEAR : 0))))
                            break;
            }

            if (!bo)
            {
                // Can't allocate dma-bufs here: shm
                Log(Warning, "Can't allocate dma-bufs, using shared memory");
                cast->m_avoidDmabuf = true;
                cast->buildFormats(builder.b, params);
            }
            else
            {
                const UInt64 modifier { gbm_bo_get_modifier(bo) };
                gbm_bo_destroy(bo);
                params.push_back(BuildFormat(builder.b, cast->m_format.format, cast->current.width, cast->current.height, {}, &modifier));
                cast->buildFormats(builder.b, params);
            }

            pw_stream_update_params(cast->m_stream, params.data(), params.size());
            spa_pod_dynamic_builder_clean(&builder);
            return;
        }

        if (cast->m_format.modifier != DRM_FORMAT_MOD_INVALID && gbm)
            blocks = gbm_device_get_format_modifier_plane_count(gbm, fourcc, cast->m_format.modifier);
    }
    else
    {
        cast->m_dmabuf = false;
        dataType = 1 << SPA_DATA_MemFd;
    }

    Log(Info, "Negotiated %ux%u, %s", cast->m_format.size.width, cast->m_format.size.height, cast->m_dmabuf ? "dma-buf" : "shared memory");

    // Buffers
    {
        spa_pod_frame f;
        spa_pod_builder_push_object(&builder.b, &f, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
        spa_pod_builder_add(&builder.b, SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(PipeWireBuffers, PipeWireBuffersMin, PipeWireBuffersMax), 0);
        spa_pod_builder_add(&builder.b, SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(blocks), 0);
        spa_pod_builder_add(&builder.b, SPA_PARAM_BUFFERS_align, SPA_POD_Int(PipeWireAlign), 0);
        spa_pod_builder_add(&builder.b, SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(dataType), 0);
        params.push_back(static_cast<const spa_pod*>(spa_pod_builder_pop(&builder.b, &f)));
    }

    // Metadata: header (timestamps), transform and damage
    params.push_back(static_cast<const spa_pod*>(spa_pod_builder_add_object(&builder.b,
        SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
        SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_header)))));

    params.push_back(static_cast<const spa_pod*>(spa_pod_builder_add_object(&builder.b,
        SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoTransform),
        SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_videotransform)))));

    params.push_back(static_cast<const spa_pod*>(spa_pod_builder_add_object(&builder.b,
        SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage),
        SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int(
            sizeof(spa_meta_region) * DamageRegions,
            sizeof(spa_meta_region) * 1,
            sizeof(spa_meta_region) * DamageRegions))));

    pw_stream_update_params(cast->m_stream, params.data(), params.size());
    spa_pod_dynamic_builder_clean(&builder);
}

void Cast::onAddBuffer(void *data, pw_buffer *buffer)
{
    auto *cast { static_cast<Cast*>(data) };
    spa_data *d { buffer->buffer->datas };
    CastBuffer *castBuffer { cast->createBuffer() };

    if (!castBuffer || buffer->buffer->n_datas != UInt32(castBuffer->planes))
    {
        Log(Error, "Can't create a buffer");
        delete castBuffer;
        cast->stopAndClose();
        return;
    }

    buffer->user_data = castBuffer;
    cast->m_buffers.push_back(castBuffer);

    for (UInt32 plane = 0; plane < buffer->buffer->n_datas; plane++)
    {
        d[plane].type = castBuffer->dmabuf ? SPA_DATA_DmaBuf : SPA_DATA_MemFd;
        d[plane].flags = SPA_DATA_FLAG_READABLE | (castBuffer->dmabuf ? 0 : SPA_DATA_FLAG_MAPPABLE);
        d[plane].fd = castBuffer->fd[plane];
        d[plane].mapoffset = 0;
        d[plane].maxsize = castBuffer->dmabuf ? castBuffer->stride[plane] * castBuffer->height : castBuffer->size[plane];
        d[plane].data = nullptr;
        d[plane].chunk->offset = castBuffer->offset[plane];
        d[plane].chunk->stride = castBuffer->stride[plane];
        d[plane].chunk->size = d[plane].maxsize;
    }
}

void Cast::onRemoveBuffer(void *data, pw_buffer *buffer)
{
    auto *cast { static_cast<Cast*>(data) };
    auto *castBuffer { static_cast<CastBuffer*>(buffer->user_data) };

    if (cast->m_pwBuffer == buffer)
    {
        if (cast->m_frame)
        {
            ext_image_copy_capture_frame_v1_destroy(cast->m_frame);
            cast->m_frame = nullptr;
        }

        cast->m_pwBuffer = nullptr;
    }

    // The fds are closed with the CastBuffer
    for (UInt32 plane = 0; plane < buffer->buffer->n_datas; plane++)
        buffer->buffer->datas[plane].fd = -1;

    std::erase(cast->m_buffers, castBuffer);
    delete castBuffer;
    buffer->user_data = nullptr;
}

void Cast::onProcess(void *data)
{
    auto *cast { static_cast<Cast*>(data) };

    if (!cast->m_streaming || cast->m_pwBuffer)
        return;

    cast->m_pwBuffer = pw_stream_dequeue_buffer(cast->m_stream);

    // All the buffers are in use (by consumers): try again shortly
    if (!cast->m_pwBuffer)
    {
        if (!cast->m_retry->running())
            cast->m_retry->start(16);

        return;
    }

    cast->m_retry->stop();
    cast->captureFrame();
}
