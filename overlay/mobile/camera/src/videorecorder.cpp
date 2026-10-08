#include "videorecorder.h"

#ifndef HAVE_GSTREAMER

// Built without GStreamer: no recording.
VideoRecorder::~VideoRecorder() = default;
bool VideoRecorder::available() { return false; }
bool VideoRecorder::start(const QString &, QSize, quint32, int, QString *error)
{
    *error = QStringLiteral("This build cannot record video");
    return false;
}
void VideoRecorder::push(const uchar *, quint32, qint64) {}
QString VideoRecorder::stop(QString *) { return {}; }

#else

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/tag/tag.h>

#include <libcamera/formats.h>

#include <cstring>
#include <ctime>

#include <sys/resource.h>
#include <unistd.h>

namespace {

bool has(const char *element)
{
    GstElementFactory *factory = gst_element_factory_find(element);
    if (!factory)
        return false;
    gst_object_unref(factory);
    return true;
}

bool init()
{
    static const bool ok = gst_init_check(nullptr, nullptr, nullptr);
    return ok;
}

// GStreamer names memory order; DRM names the 32-bit word from the top.
const char *rawFormat(quint32 fourcc)
{
    using namespace libcamera;
    if (fourcc == formats::ABGR8888.fourcc())
        return "RGBA";
    if (fourcc == formats::XBGR8888.fourcc())
        return "RGBx";
    if (fourcc == formats::ARGB8888.fourcc())
        return "BGRA";
    if (fourcc == formats::XRGB8888.fourcc())
        return "BGRx";
    return nullptr;
}

// Every streaming thread of the recorder, and so the encoder threads they
// start, runs below the camera's: a frame the image processor misses is
// lost, while the encoder can catch up.
GstBusSyncReply lowerPriority(GstBus *, GstMessage *message, gpointer)
{
    GstStreamStatusType type;
    GstElement *owner;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_STREAM_STATUS) {
        gst_message_parse_stream_status(message, &type, &owner);
        if (type == GST_STREAM_STATUS_TYPE_ENTER)
            setpriority(PRIO_PROCESS, gettid(), 5);
    }
    return GST_BUS_PASS;
}

const char *orientation(int rotation)
{
    switch (((rotation % 360) + 360) % 360) {
    case 90: return "rotate-90";
    case 180: return "rotate-180";
    case 270: return "rotate-270";
    default: return "rotate-0";
    }
}

} // namespace

VideoRecorder::~VideoRecorder()
{
    if (running()) {
        QString ignored;
        stop(&ignored);
    }
}

bool VideoRecorder::available()
{
    return init() && ((has("x264enc") && has("mp4mux")) ||
                      (has("v4l2h264enc") && has("h264parse") && has("mp4mux")) ||
                      (has("theoraenc") && has("oggmux")));
}

bool VideoRecorder::start(const QString &folder, QSize size, quint32 fourcc, int rotation,
                          QString *error)
{
    const char *format = rawFormat(fourcc);
    if (!init() || !format || !size.isValid()) {
        *error = QStringLiteral("This camera format cannot be recorded");
        return false;
    }

    // Encoder and container, best first. Each stage runs in its own thread
    // behind a queue. x264 at ultrafast encodes 1080p at about 50 frames a
    // second on the Pixel 7 Pro, not counting the camera; its frame threads
    // add latency, which a recording does not mind.
    QString video, muxer, extension;
    if (has("v4l2h264enc") && has("h264parse") && has("mp4mux")) {
        video = QStringLiteral("videoconvert ! video/x-raw,format=NV12 ! queue ! v4l2h264enc ! "
                               "video/x-h264,level=(string)4.1 ! h264parse");
        muxer = QStringLiteral("mp4mux name=mux faststart=true");
        extension = QStringLiteral("mp4");
    } else if (has("x264enc") && has("mp4mux")) {
        video = QStringLiteral("videoconvert ! video/x-raw,format=I420 ! queue ! "
                               "x264enc speed-preset=ultrafast bitrate=12000 key-int-max=60 ! "
                               "video/x-h264,profile=high");
        muxer = QStringLiteral("mp4mux name=mux faststart=true");
        extension = QStringLiteral("mp4");
    } else if (has("theoraenc") && has("oggmux")) {
        // Ogg counts frames, so dropped frames are filled in at a fixed rate.
        video = QStringLiteral("videorate ! video/x-raw,framerate=30/1 ! videoconvert ! queue ! "
                               "theoraenc bitrate=8000");
        muxer = QStringLiteral("oggmux name=mux");
        extension = QStringLiteral("ogv");
    } else {
        *error = QStringLiteral("No video encoder is installed");
        return false;
    }
    // Sound through PipeWire where it can, else whatever ALSA's default is.
    QString sound;
    for (const char *source : { "pipewiresrc", "pulsesrc", "alsasrc" }) {
        if (has(source)) {
            sound = QLatin1String(source);
            break;
        }
    }
    if (!has("opusenc"))
        sound.clear();

    QDir().mkpath(folder);
    path_ = folder + QStringLiteral("/VID_") +
            QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")) +
            QLatin1Char('.') + extension;
    // Live source, stamped with capture times (see push()), so sound and
    // picture line up. At most three frames wait, then two
    // more in the queue; beyond that frames are dropped rather than stalling
    // the camera. Encoded streams may wait three seconds for each other at
    // the muxer.
    const QString waits = QStringLiteral("queue max-size-buffers=0 max-size-bytes=0 "
                                         "max-size-time=3000000000");
    const QString description =
        QStringLiteral("appsrc name=camera is-live=true format=time "
                       "max-buffers=3 leaky-type=downstream "
                       "caps=video/x-raw,format=%1,width=%2,height=%3,framerate=0/1 ! "
                       "queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 ! "
                       "%4 ! %5 ! mux. ")
            .arg(QLatin1String(format)).arg(size.width()).arg(size.height()).arg(video, waits) +
        (sound.isEmpty() ? QString()
                         : QStringLiteral("%1 ! %2 ! audioconvert ! audioresample ! "
                                          "opusenc bitrate=96000 ! %2 ! mux. ")
                               .arg(sound, waits)) +
        muxer + QStringLiteral(" ! filesink location=\"%1\"").arg(path_);
    qInfo().noquote() << "Recording:" << description;

    GError *gerror = nullptr;
    pipeline_ = gst_parse_launch(description.toUtf8().constData(), &gerror);
    if (!pipeline_ || gerror) {
        *error = QStringLiteral("The recorder could not be set up: ") +
                 QString::fromUtf8(gerror ? gerror->message : "unknown error");
        if (gerror)
            g_error_free(gerror);
        if (pipeline_)
            gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        return false;
    }
    source_ = gst_bin_get_by_name(GST_BIN(pipeline_), "camera");
    if (extension == QLatin1String("mp4")) {
        if (GstElement *mux = gst_bin_get_by_name(GST_BIN(pipeline_), "mux")) {
            gst_tag_setter_add_tags(GST_TAG_SETTER(mux), GST_TAG_MERGE_REPLACE,
                                    GST_TAG_IMAGE_ORIENTATION, orientation(rotation), nullptr);
            gst_object_unref(mux);
        }
    }
    GstBus *bus = gst_element_get_bus(pipeline_);
    gst_bus_set_sync_handler(bus, lowerPriority, nullptr, nullptr);
    gst_object_unref(bus);
    // The monotonic system clock, which libcamera stamps frames with, rather
    // than the microphone's: the sound follows it instead.
    GstClock *clock = gst_system_clock_obtain();
    gst_pipeline_use_clock(GST_PIPELINE(pipeline_), clock);
    gst_object_unref(clock);
    size_ = size;
    lastPts_ = GST_CLOCK_TIME_NONE;
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        *error = QStringLiteral("The recorder did not start");
        QString ignored;
        stop(&ignored);
        return false;
    }
    return true;
}

void VideoRecorder::push(const uchar *data, quint32 stride, qint64 capturedNs)
{
    QMutexLocker locked(&lock_);
    if (!source_)
        return;
    // Stamped with when the sensor took the frame, not when it arrived: the
    // camera hands frames over unevenly. The pipeline runs on the same
    // monotonic clock (see start()), so that is the capture time less the
    // pipeline's start. A time from another clock counts as now.
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const qint64 now = qint64(ts.tv_sec) * 1000000000 + ts.tv_nsec;
    const qint64 captured = capturedNs <= now && capturedNs > now - 500000000 ? capturedNs : now;
    const GstClockTime base = gst_element_get_base_time(pipeline_);
    GstClockTime pts = GstClockTime(captured) > base ? GstClockTime(captured) - base : 0;
    if (lastPts_ != GST_CLOCK_TIME_NONE && pts <= lastPts_)
        pts = lastPts_ + 1;
    lastPts_ = pts;
    const gsize row = size_.width() * 4;
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, row * size_.height(), nullptr);
    GstMapInfo map;
    if (!buffer || !gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        if (buffer)
            gst_buffer_unref(buffer);
        return;
    }
    if (stride == row) {
        memcpy(map.data, data, row * size_.height());
    } else {
        for (int y = 0; y < size_.height(); ++y)
            memcpy(map.data + y * row, data + y * stride, row);
    }
    gst_buffer_unmap(buffer, &map);
    GST_BUFFER_PTS(buffer) = pts;
    gst_app_src_push_buffer(GST_APP_SRC(source_), buffer);	// takes the buffer
}

QString VideoRecorder::stop(QString *error)
{
    if (!pipeline_)
        return {};
    QString path = path_;
    GstElement *source;
    {
        QMutexLocker locked(&lock_);
        source = source_;
        source_ = nullptr;
    }
    // Every source ends, the microphone too: the muxer writes the file only
    // once all its streams have.
    gst_element_send_event(pipeline_, gst_event_new_eos());
    GstBus *bus = gst_element_get_bus(pipeline_);
    GstMessage *message = gst_bus_timed_pop_filtered(
        bus, 5 * GST_SECOND, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    if (!message) {
        *error = QStringLiteral("The video did not finish writing");
        path.clear();
    } else {
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError *gerror = nullptr;
            gst_message_parse_error(message, &gerror, nullptr);
            *error = QStringLiteral("The video failed: ") +
                     QString::fromUtf8(gerror ? gerror->message : "unknown error");
            if (gerror)
                g_error_free(gerror);
            path.clear();
        }
        gst_message_unref(message);
    }
    gst_object_unref(bus);
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (path.isEmpty()) {
        qWarning().noquote() << "Recording failed:" << *error;
        QFile::remove(path_);
    }
    if (source)
        gst_object_unref(source);
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    return path;
}

#endif // HAVE_GSTREAMER
