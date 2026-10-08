#include "camerasession.h"
#include "stillwriter.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>
#include <QtConcurrent/QtConcurrentRun>

#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

using namespace libcamera;

namespace {

// A still skips at least this many frames while the lens settles after the
// stream restart...
constexpr int kStillMinFrames = 3;
// ...then waits for exposure, gain and white balance to repeat on this many,
// since an image processor may start them over on a new configuration...
constexpr int kStillStableFrames = 2;
// ...but never waits for more than this many.
constexpr int kStillMaxFrames = 15;
// Enough buffers that the viewfinder can hold three while the camera fills
// the rest.
constexpr unsigned int kPreviewBuffers = 6;
constexpr unsigned int kStillBuffers = 3;
// Video is 16:9 1080p from the processed stream.
constexpr QSize kVideoSize{1920, 1080};

QString focusName(int state)
{
    switch (state) {
    case controls::AfStateScanning:
        return QStringLiteral("scanning");
    case controls::AfStateFocused:
        return QStringLiteral("focused");
    case controls::AfStateFailed:
        return QStringLiteral("failed");
    default:
        return QStringLiteral("idle");
    }
}

QImage::Format imageFormat(quint32 fourcc)
{
    // DRM names list components from the most significant bit, so the
    // little-endian byte order is reversed: ABGR8888 is R, G, B, A in memory.
    if (fourcc == formats::ABGR8888.fourcc())
        return QImage::Format_RGBA8888;
    if (fourcc == formats::XBGR8888.fourcc())
        return QImage::Format_RGBX8888;
    if (fourcc == formats::ARGB8888.fourcc())
        return QImage::Format_ARGB32;
    if (fourcc == formats::XRGB8888.fourcc())
        return QImage::Format_RGB32;
    return QImage::Format_Invalid;
}

QImage copyFrame(const FrameBuffer *buffer, QSize size, unsigned int stride, quint32 fourcc)
{
    const QImage::Format format = imageFormat(fourcc);
    if (format == QImage::Format_Invalid || buffer->planes().empty())
        return {};

    const FrameBuffer::Plane &plane = buffer->planes()[0];
    const int fd = plane.fd.get();
    const size_t length = plane.offset + plane.length;
    void *map = mmap(nullptr, length, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED)
        return {};

    dma_buf_sync sync = { DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    const QImage view(static_cast<const uchar *>(map) + plane.offset, size.width(),
                      size.height(), stride, format);
    QImage copy = view.copy();
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    munmap(map, length);
    return copy;
}

bool supports(const std::shared_ptr<Camera> &camera, const ControlId &id)
{
    return camera->controls().find(&id) != camera->controls().end();
}

// Back, front, then external or unknown.
int side(const std::shared_ptr<Camera> &camera)
{
    const auto location = camera->properties().get(properties::Location);
    if (!location)
        return 2;
    switch (*location) {
    case properties::CameraLocationBack:
        return 0;
    case properties::CameraLocationFront:
        return 1;
    default:
        return 2;
    }
}

qint64 pixels(const std::shared_ptr<Camera> &camera)
{
    const auto size = camera->properties().get(properties::PixelArraySize);
    return size ? qint64(size->width) * size->height : 0;
}

QString modelName(const std::shared_ptr<Camera> &camera)
{
    const auto model = camera->properties().get(properties::Model);
    return model ? QString::fromUtf8(model->data(), qsizetype(model->size()))
                 : QString::fromStdString(camera->id());
}

} // namespace

CameraSession::CameraSession(QObject *parent)
    : QObject(parent),
      frames_([this](const CameraFrame &frame) {
          // Any thread: the worker gives the buffer back to the camera.
          CameraWorker *worker = worker_;
          QMetaObject::invokeMethod(worker, [worker, frame] { worker->requeue(frame); },
                                    Qt::QueuedConnection);
      })
{
    photoFolder_ = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) +
                   QStringLiteral("/Camera");

    worker_ = new CameraWorker(&frames_);
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &CameraWorker::camerasFound, this, [this](const QVariantList &cameras) {
        cameras_ = cameras;
        Q_EMIT camerasChanged();
    });
    connect(worker_, &CameraWorker::opened, this,
            [this](int index, const QString &name, int rotation, bool canFocus) {
                opened_ = true;
                cameraIndex_ = index;
                cameraName_ = name;
                rotation_ = rotation;
                canFocus_ = canFocus;
                Q_EMIT cameraChanged();
            });
    connect(worker_, &CameraWorker::failed, this,
            [this](const QString &message) { setState(QStringLiteral("error"), message); });
    connect(worker_, &CameraWorker::streamingChanged, this, [this](const QString &mode) {
        if (mode == QLatin1String("preview"))
            setState(QStringLiteral("preview"));
        else if (mode == QLatin1String("still"))
            setState(QStringLiteral("capturing"));
        else if (state_ != QLatin1String("error"))
            setState(QStringLiteral("off"));
    });
    connect(worker_, &CameraWorker::frameReady, this, &CameraSession::frameReady);
    connect(worker_, &CameraWorker::focusStateChanged, this, [this](const QString &state) {
        focusState_ = state;
        Q_EMIT focusChanged();
    });
    connect(worker_, &CameraWorker::stillCaptured, this,
            [this](const QImage &image, int rotation, qint64 exposureUs, double gain,
                   const QString &model) {
                writeStill(image, StillInfo{ rotation, exposureUs, gain, model, captureZoom_ });
            });
    connect(worker_, &CameraWorker::stillFailed, this, [this](const QString &message) {
        pending_ = std::max(0, pending_ - 1);
        Q_EMIT pendingChanged();
        Q_EMIT photoFailed(message);
    });
    connect(worker_, &CameraWorker::recordingStopped, this,
            [this](const QString &path, const QString &error) {
                recording_ = false;
                Q_EMIT recordingChanged();
                if (path.isEmpty()) {
                    Q_EMIT videoFailed(error);
                    return;
                }
                lastVideo_ = path;
                Q_EMIT lastVideoChanged();
                Q_EMIT videoSaved(path);
            });
    worker_->burstDone = [this](std::shared_ptr<std::vector<RawFrame>> frames, RawLayout layout,
                                int rotation, QString model) {
        QMetaObject::invokeMethod(this, [this, frames, layout, rotation, model] {
            mergeBurst(frames, layout, rotation, model);
        }, Qt::QueuedConnection);
    };

    canRecord_ = VideoRecorder::available();

    thread_.setObjectName(QStringLiteral("camera"));
    thread_.start();
}

CameraSession::~CameraSession()
{
    CameraWorker *worker = worker_;
    QMetaObject::invokeMethod(worker, [worker] { worker->close(); },
                              Qt::BlockingQueuedConnection);
    thread_.quit();
    thread_.wait();
}

void CameraSession::setActive(bool active)
{
    if (active == active_)
        return;
    active_ = active;
    Q_EMIT activeChanged();

    CameraWorker *worker = worker_;
    if (active) {
        setState(QStringLiteral("starting"));
        const QSize size = previewSize_;
        const int index = cameraIndex_;
        QMetaObject::invokeMethod(worker, [worker, size, index] {
            worker->open(index);
            worker->startPreview(size);
        });
    } else {
        QMetaObject::invokeMethod(worker, [worker] { worker->stopPreview(); });
    }
}

void CameraSession::setCameraIndex(int index)
{
    // Not while a photo is being taken or a video recorded on the camera in use.
    if (index == cameraIndex_ || index < 0 || index >= cameras_.size() ||
        state_ == QLatin1String("capturing") || recording_)
        return;
    cameraIndex_ = index;
    Q_EMIT cameraChanged();
    if (!active_)
        return;

    setState(QStringLiteral("starting"));
    focusPoint_ = { -1, -1 };
    focusState_ = QStringLiteral("idle");
    Q_EMIT focusChanged();
    CameraWorker *worker = worker_;
    const QSize size = previewSize_;
    QMetaObject::invokeMethod(worker, [worker, size, index] {
        worker->open(index);
        worker->startPreview(size);
    });
}

void CameraSession::setPreviewSize(const QSize &size)
{
    if (size == previewSize_ || !size.isValid())
        return;
    previewSize_ = size;
    Q_EMIT previewSizeChanged();
}

void CameraSession::setPhotoFolder(const QString &folder)
{
    if (folder == photoFolder_)
        return;
    photoFolder_ = folder;
    Q_EMIT photoFolderChanged();
}

void CameraSession::focusAt(qreal x, qreal y)
{
    if (!canFocus_)
        return;
    x = std::clamp(x, 0.0, 1.0);
    y = std::clamp(y, 0.0, 1.0);

    // The preview shows the frame turned clockwise by rotation_; undo that
    // to find the point in the frame.
    QPointF frame;
    switch (((rotation_ % 360) + 360) % 360) {
    case 90:
        frame = { y, 1.0 - x };
        break;
    case 180:
        frame = { 1.0 - x, 1.0 - y };
        break;
    case 270:
        frame = { 1.0 - y, x };
        break;
    default:
        frame = { x, y };
        break;
    }

    focusPoint_ = { x, y };
    Q_EMIT focusChanged();
    CameraWorker *worker = worker_;
    QMetaObject::invokeMethod(worker, [worker, frame] { worker->setFocusWindow(frame, true); });
}

void CameraSession::resetFocus()
{
    focusPoint_ = { -1, -1 };
    Q_EMIT focusChanged();
    CameraWorker *worker = worker_;
    QMetaObject::invokeMethod(worker, [worker] { worker->setFocusWindow({}, false); });
}

void CameraSession::setDigitalZoom(qreal zoom)
{
    zoom = std::clamp(zoom, 1.0, 8.0);
    if (qFuzzyCompare(zoom, digitalZoom_))
        return;
    digitalZoom_ = zoom;
    Q_EMIT digitalZoomChanged();
}

void CameraSession::capture()
{
    captureZoom_ = digitalZoom_;
    // At most two photos in flight: each merge holds several raw frames.
    if (state_ != QLatin1String("preview") || pending_ >= 2 || video_)
        return;
    ++pending_;
    Q_EMIT pendingChanged();
    CameraWorker *worker = worker_;
    QMetaObject::invokeMethod(worker, [worker] { worker->capture(); });
}

void CameraSession::setVideo(bool video)
{
    if (video == video_ || recording_ || (video && !canRecord_))
        return;
    video_ = video;
    Q_EMIT videoChanged();
    CameraWorker *worker = worker_;
    QMetaObject::invokeMethod(worker, [worker, video] { worker->setVideo(video); });
}

void CameraSession::startRecording()
{
    if (!video_ || recording_ || state_ != QLatin1String("preview"))
        return;
    recording_ = true;
    Q_EMIT recordingChanged();
    CameraWorker *worker = worker_;
    const QString folder = photoFolder_;
    QMetaObject::invokeMethod(worker, [worker, folder] { worker->startRecording(folder); });
}

void CameraSession::stopRecording()
{
    if (!recording_)
        return;
    CameraWorker *worker = worker_;
    QMetaObject::invokeMethod(worker, [worker] { worker->stopRecording(); });
}

void CameraSession::setState(const QString &state, const QString &error)
{
    if (state == state_ && error == error_)
        return;
    state_ = state;
    error_ = error;
    Q_EMIT stateChanged();
}

void CameraSession::writeStill(QImage image, StillInfo info)
{
    const QString folder = photoFolder_;
    QtConcurrent::run([image = std::move(image), info, folder]() {
        QString error;
        const QString path = StillWriter::write(image, info, folder, &error);
        return path.isEmpty() ? QStringLiteral("!") + error : path;
    }).then(this, [this](const QString &result) { finishPhoto(result); });
}

void CameraSession::mergeBurst(std::shared_ptr<std::vector<RawFrame>> frames, RawLayout layout,
                               int rotation, QString model)
{
    const QString folder = photoFolder_;
    const double zoom = captureZoom_;
    QtConcurrent::run([frames, layout, rotation, model, folder, zoom]() {
        MergeReport report;
        const QImage image = RawMerge::process(*frames, layout, &report);
        QStringList shifts;
        for (size_t i = 0; i < report.shifts.size(); ++i)
            shifts << QStringLiteral("%1,%2 (%3% left out)")
                          .arg(report.shifts[i][0]).arg(report.shifts[i][1])
                          .arg(qRound(report.rejected[i] * 100));
        QStringList sharpness;
        for (double value : report.sharpness)
            sharpness << QString::number(value, 'f', 2);
        qInfo().noquote() << "Merged" << report.frames << "frames in" << report.milliseconds
                          << "ms on frame" << report.reference << "(sharpness"
                          << sharpness.join(QStringLiteral(" ")) + QStringLiteral("); shifts")
                          << shifts.join(QStringLiteral("; "));
        if (image.isNull())
            return QStringLiteral("!The photo could not be processed");
        const RawFrame &first = frames->front();
        const StillInfo info{ rotation, first.exposureUs,
                              first.analogueGain * first.digitalGain, model, zoom };
        QString error;
        const QString path = StillWriter::write(image, info, folder, &error);
        return path.isEmpty() ? QStringLiteral("!") + error : path;
    }).then(this, [this](const QString &result) { finishPhoto(result); });
}

void CameraSession::finishPhoto(const QString &result)
{
    pending_ = std::max(0, pending_ - 1);
    Q_EMIT pendingChanged();
    if (result.startsWith(QLatin1Char('!'))) {
        Q_EMIT photoFailed(result.mid(1));
        return;
    }
    lastPhoto_ = result;
    Q_EMIT lastPhotoChanged();
    Q_EMIT photoSaved(result);
}

CameraWorker::CameraWorker(FrameQueue *frames)
    : frames_(frames)
{
}

CameraWorker::~CameraWorker()
{
    close();
}

void CameraWorker::open(int index)
{
    if (!manager_) {
        manager_ = std::make_unique<CameraManager>();
        if (manager_->start()) {
            manager_.reset();
            Q_EMIT failed(QStringLiteral("The camera system did not start"));
            return;
        }
        cameras_ = manager_->cameras();
        if (cameras_.empty()) {
            // Look again next time: the camera drivers may still be loading.
            manager_.reset();
            Q_EMIT failed(QStringLiteral("No camera found"));
            return;
        }
        std::stable_sort(cameras_.begin(), cameras_.end(), [](const auto &a, const auto &b) {
            return side(a) != side(b) ? side(a) < side(b) : pixels(a) > pixels(b);
        });
        static const char *const sides[] = { "back", "front", "external" };
        QVariantList list;
        for (const auto &camera : cameras_)
            list.append(QVariantMap{ { QStringLiteral("model"), modelName(camera) },
                                     { QStringLiteral("location"),
                                       QString::fromLatin1(sides[side(camera)]) } });
        Q_EMIT camerasFound(list);
    }

    index = std::clamp(index, 0, int(cameras_.size()) - 1);
    const std::shared_ptr<Camera> chosen = cameras_[index];
    if (camera_ == chosen)
        return;
    release();
    if (chosen->acquire()) {
        Q_EMIT failed(QStringLiteral("Another app is using the camera"));
        return;
    }

    camera_ = chosen;
    camera_->requestCompleted.connect(this, &CameraWorker::onRequestCompleted);

    const ControlList &props = camera_->properties();
    // libcamera states the mounting rotation counter-clockwise; the preview
    // and photos turn the frame clockwise by the rest of a full turn.
    rotation_ = (360 - props.get(properties::Rotation).value_or(0) % 360) % 360;
    model_ = modelName(camera_);
    activeArea_ = {};
    if (const auto areas = props.get(properties::PixelArrayActiveAreas); areas && !areas->empty())
        activeArea_ = (*areas)[0];
    else if (const auto size = props.get(properties::PixelArraySize))
        activeArea_ = Rectangle(*size);
    canFocus_ = supports(camera_, controls::AfMode);
    pending_ = ControlList(camera_->controls());
    pendingSet_ = false;
    focusWindow_ = false;
    lensPosition_ = -1.0f;

    Q_EMIT opened(index, model_, rotation_, canFocus_);
}

void CameraWorker::release()
{
    if (!camera_)
        return;
    stopStream();
    mode_ = Off;
    camera_->requestCompleted.disconnect(this);
    camera_->release();
    camera_.reset();
}

void CameraWorker::close()
{
    if (camera_)
        stopPreview();
    release();
    cameras_.clear();
    manager_.reset();
}

bool CameraWorker::configure(StreamRole role, QSize size, unsigned int buffers, bool withRaw)
{
    rawStream_ = nullptr;
    config_ = withRaw ? camera_->generateConfiguration({ role, StreamRole::Raw })
                      : camera_->generateConfiguration({ role });
    if (!config_ || config_->empty() || (withRaw && config_->size() < 2))
        return false;

    StreamConfiguration &cfg = config_->at(0);
    // 32-bit RGB, which the viewfinder can hand to the GPU as it is.
    const std::vector<PixelFormat> offered = cfg.formats().pixelformats();
    for (const PixelFormat &format : { formats::ABGR8888, formats::XBGR8888,
                                       formats::ARGB8888, formats::XRGB8888 }) {
        if (std::find(offered.begin(), offered.end(), format) != offered.end()) {
            cfg.pixelFormat = format;
            break;
        }
    }
    if (size.isValid())
        cfg.size = Size(size.width(), size.height());
    cfg.bufferCount = buffers;

    if (withRaw)
        config_->at(1).bufferCount = buffers;

    if (config_->validate() == CameraConfiguration::Invalid ||
        camera_->configure(config_.get()))
        return false;

    stream_ = cfg.stream();
    streamSize_ = QSize(cfg.size.width, cfg.size.height);
    stride_ = cfg.stride;
    fourcc_ = cfg.pixelFormat.fourcc();
    if (withRaw) {
        const StreamConfiguration &raw = config_->at(1);
        if (!RawLayout::fromName(QString::fromStdString(raw.pixelFormat.toString()),
                                 QSize(raw.size.width, raw.size.height), raw.stride,
                                 &rawLayout_))
            return false;
        rawStream_ = raw.stream();
    }

    allocator_ = std::make_unique<FrameBufferAllocator>(camera_);
    if (allocator_->allocate(stream_) < 0 || (rawStream_ && allocator_->allocate(rawStream_) < 0))
        return false;

    requests_.clear();
    const auto &frames = allocator_->buffers(stream_);
    for (size_t i = 0; i < frames.size(); ++i) {
        std::unique_ptr<Request> request = camera_->createRequest();
        if (!request || request->addBuffer(stream_, frames[i].get()))
            return false;
        if (rawStream_) {
            const auto &raws = allocator_->buffers(rawStream_);
            if (i >= raws.size() || request->addBuffer(rawStream_, raws[i].get()))
                return false;
        }
        requests_.push_back(std::move(request));
    }
    return true;
}

bool CameraWorker::startStream(const ControlList &initial)
{
    ++generation_;
    stillFrames_ = 0;
    stableFrames_ = 0;
    lastExposure_ = -1;
    lastGain_ = -1.0;
    lastColour_ = { -1.0f, -1.0f };
    stillTaken_ = false;
    focusState_ = -1;

    if (camera_->start(initial.empty() ? nullptr : &initial))
        return false;
    for (const std::unique_ptr<Request> &request : requests_)
        queue(request.get());
    return true;
}

void CameraWorker::stopStream()
{
    if (!camera_ || !stream_)
        return;
    // Leaving the stream, for any reason, ends a video.
    if (recordingFrames_.exchange(false)) {
        QString error;
        const QString path = recorder_.stop(&error);
        Q_EMIT recordingStopped(path, error);
    }
    // A photo cut short, by the app going to the background or a switch of
    // camera, still has to leave the count of photos in flight.
    const bool interrupted = burst_ || (mode_ == Still && !stillTaken_);
    camera_->stop();
    unmapFrames();
    ++generation_;
    burstWanted_ = 0;
    burst_.reset();
    frames_->clear();
    requests_.clear();
    allocator_.reset();
    config_.reset();
    stream_ = nullptr;
    rawStream_ = nullptr;
    if (interrupted)
        Q_EMIT stillFailed(QStringLiteral("The photo was interrupted"));
}

void CameraWorker::startPreview(QSize size)
{
    if (!camera_ || mode_ == Preview)
        return;
    previewSize_ = size;
    // Video: the viewfinder shows the recording's own 16:9 frames, so it
    // frames what is recorded. Photos: with the raw stream where the
    // pipeline offers it; otherwise stills reconfigure to full resolution.
    const bool ready = video_
        ? configure(StreamRole::VideoRecording, kVideoSize, kPreviewBuffers, false)
        : configure(StreamRole::Viewfinder, size, kPreviewBuffers, true) ||
              configure(StreamRole::Viewfinder, size, kPreviewBuffers, false);
    if (!ready) {
        Q_EMIT failed(QStringLiteral("The camera could not be set up"));
        return;
    }
    mode_ = Preview;
    // As for stills, the first request repeats the start controls: after a
    // still in manual focus, continuous focus must reach the image processor.
    const ControlList initial = focusControls();
    pending_ = initial;
    pendingSet_ = true;
    if (!startStream(initial)) {
        mode_ = Off;
        Q_EMIT failed(QStringLiteral("The camera did not start"));
        return;
    }
    Q_EMIT streamingChanged(QStringLiteral("preview"));
}

void CameraWorker::stopPreview()
{
    stopStream();
    mode_ = Off;
    Q_EMIT streamingChanged(QStringLiteral("off"));
}

void CameraWorker::setVideo(bool video)
{
    if (video == video_)
        return;
    video_ = video;
    if (mode_ != Preview)
        return;
    stopStream();
    mode_ = Off;
    startPreview(previewSize_);
}

void CameraWorker::startRecording(const QString &folder)
{
    QString error;
    if (!video_ || mode_ != Preview || recordingFrames_)
        error = QStringLiteral("The camera is busy");
    else if (recorder_.start(folder, streamSize_, fourcc_, rotation_, &error))
        recordingFrames_ = true;
    if (!recordingFrames_)
        Q_EMIT recordingStopped({}, error);
}

void CameraWorker::stopRecording()
{
    if (!recordingFrames_.exchange(false))
        return;
    QString error;
    const QString path = recorder_.stop(&error);
    Q_EMIT recordingStopped(path, error);
}

void CameraWorker::recordFrame(Request *request, const FrameBuffer *buffer)
{
    // libcamera's thread. Buffers are mapped once each and kept until the
    // stream stops.
    const FrameBuffer::Plane &plane = buffer->planes()[0];
    const int fd = plane.fd.get();
    auto it = maps_.find(fd);
    if (it == maps_.end()) {
        const size_t length = plane.offset + plane.length;
        void *map = mmap(nullptr, length, PROT_READ, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED)
            return;
        it = maps_.emplace(fd, std::make_pair(map, length)).first;
    }
    dma_buf_sync sync = { DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    const qint64 captured = request->metadata().get(controls::SensorTimestamp)
                                .value_or(qint64(buffer->metadata().timestamp));
    recorder_.push(static_cast<const uchar *>(it->second.first) + plane.offset, stride_, captured);
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

void CameraWorker::unmapFrames()
{
    for (const auto &[fd, map] : maps_)
        munmap(map.first, map.second);
    maps_.clear();
}

ControlList CameraWorker::focusControls() const
{
    ControlList controls(camera_->controls());
    if (!canFocus_)
        return controls;

    controls.set(controls::AfMode, controls::AfModeContinuous);
    if (!supports(camera_, controls::AfMetering))
        return controls;

    if (focusWindow_ && supports(camera_, controls::AfWindows)) {
        // A window a fifth of the frame across, centred on the tap.
        const Rectangle &area = activeArea_;
        const int width = area.width / 5;
        const int height = area.height / 5;
        const int x = std::clamp(area.x + int(focusPoint_.x() * area.width) - width / 2,
                                 area.x, area.x + int(area.width) - width);
        const int y = std::clamp(area.y + int(focusPoint_.y() * area.height) - height / 2,
                                 area.y, area.y + int(area.height) - height);
        controls.set(controls::AfMetering, controls::AfMeteringWindows);
        controls.set(controls::AfWindows, { Rectangle(x, y, width, height) });
    } else {
        controls.set(controls::AfMetering, controls::AfMeteringAuto);
    }
    return controls;
}

void CameraWorker::setFocusWindow(QPointF framePoint, bool enabled)
{
    focusWindow_ = enabled;
    focusPoint_ = framePoint;
    if (!canFocus_ || mode_ != Preview)
        return;
    pending_ = focusControls();
    pendingSet_ = true;
}

void CameraWorker::queue(Request *request)
{
    if (pendingSet_) {
        request->controls().merge(pending_);
        pending_.clear();
        pendingSet_ = false;
    }
    camera_->queueRequest(request);
}

void CameraWorker::requeue(const CameraFrame &frame)
{
    if (frame.generation != generation_ || mode_ != Preview || !camera_)
        return;
    frame.request->reuse(Request::ReuseBuffers);
    queue(frame.request);
}

void CameraWorker::capture()
{
    if (mode_ != Preview || video_) {
        Q_EMIT stillFailed(QStringLiteral("The camera is not ready"));
        return;
    }
    if (rawStream_ && rawRenderable_ && !burst_) {
        captureBurst();
        return;
    }

    // Keep the focus the preview found: the still is a new configuration,
    // and continuous autofocus would otherwise scan again.
    const float lens = lensPosition_;
    stopStream();
    mode_ = Still;
    Q_EMIT streamingChanged(QStringLiteral("still"));

    if (!configure(StreamRole::StillCapture, QSize(), kStillBuffers, false)) {
        mode_ = Off;
        Q_EMIT stillFailed(QStringLiteral("The camera could not take a photo"));
        startPreview(previewSize_);
        return;
    }
    // Start controls do not reach every pipeline's image processor, so the
    // first request carries the focus as well.
    ControlList initial(camera_->controls());
    if (canFocus_ && lens >= 0.0f && supports(camera_, controls::LensPosition)) {
        initial.set(controls::AfMode, controls::AfModeManual);
        initial.set(controls::LensPosition, lens);
        pending_ = initial;
        pendingSet_ = true;
    }
    if (!startStream(initial)) {
        mode_ = Off;
        Q_EMIT stillFailed(QStringLiteral("The camera could not take a photo"));
        startPreview(previewSize_);
    }
}

void CameraWorker::captureBurst()
{
    // The next frames, as many as the light needs: one in good light, up to
    // eight in the dark. The preview keeps running throughout.
    const int count = RawMerge::framesForGain(totalGain_);
    auto frames = std::make_shared<std::vector<RawFrame>>();
    frames->reserve(count);
    burst_ = frames;
    burstWanted_ = count;
    Q_EMIT burstStarted();
}

void CameraWorker::keepBurstFrame(Request *request, const FrameBuffer *raw)
{
    // libcamera's thread.
    std::shared_ptr<std::vector<RawFrame>> frames = burst_;
    if (!frames || raw->planes().empty())
        return;

    const FrameBuffer::Plane &plane = raw->planes()[0];
    const size_t bytes = size_t(rawLayout_.stride) * rawLayout_.size.height();
    const size_t length = plane.offset + plane.length;
    void *map = mmap(nullptr, length, PROT_READ, MAP_SHARED, plane.fd.get(), 0);
    if (map == MAP_FAILED)
        return;

    RawFrame frame;
    frame.data.resize(bytes);
    dma_buf_sync sync = { DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
    ioctl(plane.fd.get(), DMA_BUF_IOCTL_SYNC, &sync);
    std::memcpy(frame.data.data(), static_cast<const uint8_t *>(map) + plane.offset,
                std::min<size_t>(bytes, plane.length));
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    ioctl(plane.fd.get(), DMA_BUF_IOCTL_SYNC, &sync);
    munmap(map, length);

    const ControlList &metadata = request->metadata();
    frame.exposureUs = metadata.get(controls::ExposureTime).value_or(0);
    frame.analogueGain = metadata.get(controls::AnalogueGain).value_or(1.0f);
    frame.digitalGain = metadata.get(controls::DigitalGain).value_or(1.0f);
    if (const auto gains = metadata.get(controls::ColourGains))
        frame.colourGains = { (*gains)[0], (*gains)[1] };
    if (const auto ccm = metadata.get(controls::ColourCorrectionMatrix))
        std::copy(ccm->begin(), ccm->end(), frame.ccm.begin());
    frame.saturation = metadata.get(controls::Saturation).value_or(1.0f);
    frame.contrast = metadata.get(controls::Contrast).value_or(1.0f);
    frame.gamma = metadata.get(controls::Gamma).value_or(2.2f);
    if (const auto black = metadata.get(controls::SensorBlackLevels))
        frame.blackLevel16 = (*black)[0];
    if (frames->empty())
        dumpBurstFrame(request, frame);
    frames->push_back(std::move(frame));

    if (--burstWanted_ == 0) {
        burst_.reset();
        if (burstDone)
            burstDone(frames, rawLayout_, rotation_, model_);
    }
}

void CameraWorker::dumpBurstFrame(Request *request, const RawFrame &frame)
{
    // For tuning: OMARCHY_CAMERA_DUMP=<folder> keeps the first raw frame of
    // each burst, its settings, and the image processor's own rendering of it.
    const QString folder = qEnvironmentVariable("OMARCHY_CAMERA_DUMP");
    if (folder.isEmpty() || !QDir().mkpath(folder))
        return;
    QFile raw(folder + QStringLiteral("/raw.bin"));
    if (raw.open(QIODevice::WriteOnly))
        raw.write(reinterpret_cast<const char *>(frame.data.data()), qint64(frame.data.size()));
    QFile info(folder + QStringLiteral("/raw.txt"));
    if (info.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&info);
        out << "size " << rawLayout_.size.width() << ' ' << rawLayout_.size.height() << '\n'
            << "stride " << rawLayout_.stride << '\n'
            << "bits " << rawLayout_.bits << '\n'
            << "packed " << int(rawLayout_.csi2Packed) << '\n'
            << "red " << rawLayout_.redX << ' ' << rawLayout_.redY << '\n'
            << "exposure " << frame.exposureUs << '\n'
            << "analogue " << frame.analogueGain << '\n'
            << "digital " << frame.digitalGain << '\n'
            << "gains " << frame.colourGains[0] << ' ' << frame.colourGains[1] << '\n'
            << "ccm";
        for (float v : frame.ccm)
            out << ' ' << v;
        out << '\n'
            << "saturation " << frame.saturation << '\n'
            << "contrast " << frame.contrast << '\n'
            << "gamma " << frame.gamma << '\n'
            << "black " << frame.blackLevel16 << '\n';
    }
    if (const FrameBuffer *buffer = request->findBuffer(stream_))
        copyFrame(buffer, streamSize_, stride_, fourcc_).save(folder + QStringLiteral("/isp.png"));
}

void CameraWorker::onRequestCompleted(Request *request)
{
    // libcamera's thread.
    if (request->status() == Request::RequestCancelled)
        return;

    const ControlList &metadata = request->metadata();
    if (const auto state = metadata.get(controls::AfState)) {
        if (focusState_.exchange(*state) != *state && mode_ == Preview)
            Q_EMIT focusStateChanged(focusName(*state));
    }
    if (const auto lens = metadata.get(controls::LensPosition); lens && mode_ == Preview)
        lensPosition_ = *lens;

    if (const auto gain = metadata.get(controls::AnalogueGain))
        totalGain_ = *gain * metadata.get(controls::DigitalGain).value_or(1.0f);
    rawRenderable_ = metadata.contains(controls::ColourGains.id()) &&
                     metadata.contains(controls::ColourCorrectionMatrix.id());

    const FrameBuffer *buffer = stream_ ? request->findBuffer(stream_) : nullptr;
    if (!buffer)
        return;
    if (mode_ == Preview && rawStream_ && burstWanted_ > 0) {
        if (const FrameBuffer *raw = request->findBuffer(rawStream_))
            keepBurstFrame(request, raw);
    }

    if (mode_ == Still) {
        handleStill(request, buffer);
        return;
    }
    if (mode_ != Preview || buffer->planes().empty())
        return;
    if (recordingFrames_)
        recordFrame(request, buffer);

    const FrameBuffer::Plane &plane = buffer->planes()[0];
    CameraFrame frame;
    frame.request = request;
    frame.generation = generation_;
    frame.fd = plane.fd.get();
    frame.offset = plane.offset;
    frame.length = plane.length;
    frame.size = streamSize_;
    frame.stride = stride_;
    frame.fourcc = fourcc_;
    frame.sequence = buffer->metadata().sequence;
    frames_->publish(frame);
    Q_EMIT frameReady();
}

void CameraWorker::handleStill(Request *request, const FrameBuffer *buffer)
{
    // libcamera's thread.
    if (stillTaken_)
        return;

    const ControlList &metadata = request->metadata();
    const qint64 exposure = metadata.get(controls::ExposureTime).value_or(-1);
    const double gain = metadata.get(controls::AnalogueGain).value_or(-1.0f);
    std::array<float, 2> colour{ -1.0f, -1.0f };
    if (const auto gains = metadata.get(controls::ColourGains))
        colour = { (*gains)[0], (*gains)[1] };
    const auto near = [](float a, float b) { return std::abs(a - b) <= 0.01f * std::max(a, b); };

    ++stillFrames_;
    const bool same = exposure == lastExposure_ && gain == lastGain_ &&
                      near(colour[0], lastColour_[0]) && near(colour[1], lastColour_[1]);
    stableFrames_ = same ? stableFrames_ + 1 : 0;
    lastExposure_ = exposure;
    lastGain_ = gain;
    lastColour_ = colour;

    const bool settled = stillFrames_ > kStillMinFrames && stableFrames_ >= kStillStableFrames;
    if (!settled && stillFrames_ < kStillMaxFrames) {
        request->reuse(Request::ReuseBuffers);
        camera_->queueRequest(request);
        return;
    }

    stillTaken_ = true;
    const QImage image = copyFrame(buffer, streamSize_, stride_, fourcc_);
    QMetaObject::invokeMethod(this, [this, image, exposure, gain] {
        finishStill(image, exposure, gain);
    }, Qt::QueuedConnection);
}

void CameraWorker::finishStill(const QImage &image, qint64 exposureUs, double gain)
{
    stopStream();
    mode_ = Off;
    if (image.isNull())
        Q_EMIT failed(QStringLiteral("The photo could not be read from the camera"));
    else
        Q_EMIT stillCaptured(image, rotation_, exposureUs, gain, model_);
    startPreview(previewSize_);
}
