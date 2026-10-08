#pragma once

#include "framequeue.h"
#include "rawmerge.h"
#include "videorecorder.h"

#include <QImage>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QThread>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include <libcamera/libcamera.h>

// What the photo writer needs to know about a still.
struct StillInfo {
    int rotation = 0;           // degrees clockwise to turn the frame upright
    qint64 exposureUs = 0;
    double analogueGain = 0.0;
    QString model;              // sensor model, from libcamera
    double zoom = 1.0;          // digital zoom: the photo is the centre 1/zoom of the frame
};

class CameraWorker;

// The camera as QML sees it: a preview for a Viewfinder, tap to focus, and
// full-resolution stills. It uses libcamera's public API only, so it works
// on any phone libcamera supports; board specifics live below libcamera.
class CameraSession : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    // The cameras, back ones first and the largest sensor first on each
    // side, so the main camera leads: [{model, location}], and the one in use.
    Q_PROPERTY(QVariantList cameras READ cameras NOTIFY camerasChanged)
    Q_PROPERTY(int cameraIndex READ cameraIndex WRITE setCameraIndex NOTIFY cameraChanged)
    Q_PROPERTY(QString cameraName READ cameraName NOTIFY cameraChanged)
    Q_PROPERTY(int rotation READ rotation NOTIFY cameraChanged)
    Q_PROPERTY(bool canFocus READ canFocus NOTIFY cameraChanged)
    Q_PROPERTY(QSize previewSize READ previewSize WRITE setPreviewSize NOTIFY previewSizeChanged)
    Q_PROPERTY(QString focusState READ focusState NOTIFY focusChanged)
    Q_PROPERTY(QPointF focusPoint READ focusPoint NOTIFY focusChanged)
    Q_PROPERTY(QString photoFolder READ photoFolder WRITE setPhotoFolder NOTIFY photoFolderChanged)
    Q_PROPERTY(QString lastPhoto READ lastPhoto NOTIFY lastPhotoChanged)
    // Photos taken but still being merged and saved.
    Q_PROPERTY(int pending READ pending NOTIFY pendingChanged)
    // Video: whether an encoder is installed, whether the camera is set up
    // for video (16:9, no photos) or photos, whether it is recording, and
    // the newest video saved.
    Q_PROPERTY(bool canRecord READ canRecord CONSTANT)
    Q_PROPERTY(bool video READ video WRITE setVideo NOTIFY videoChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    Q_PROPERTY(QString lastVideo READ lastVideo NOTIFY lastVideoChanged)
    // Digital zoom on top of the lens (1 = none): the app scales the preview,
    // and photos keep the centre 1/zoom of the frame.
    Q_PROPERTY(qreal digitalZoom READ digitalZoom WRITE setDigitalZoom NOTIFY digitalZoomChanged)

public:
    explicit CameraSession(QObject *parent = nullptr);
    ~CameraSession() override;

    bool active() const { return active_; }
    void setActive(bool active);
    QString state() const { return state_; }
    QString error() const { return error_; }
    QVariantList cameras() const { return cameras_; }
    int cameraIndex() const { return cameraIndex_; }
    void setCameraIndex(int index);
    QString cameraName() const { return cameraName_; }
    int rotation() const { return rotation_; }
    bool canFocus() const { return canFocus_; }
    QSize previewSize() const { return previewSize_; }
    void setPreviewSize(const QSize &size);
    QString focusState() const { return focusState_; }
    QPointF focusPoint() const { return focusPoint_; }
    QString photoFolder() const { return photoFolder_; }
    void setPhotoFolder(const QString &folder);
    QString lastPhoto() const { return lastPhoto_; }
    int pending() const { return pending_; }
    bool canRecord() const { return canRecord_; }
    bool video() const { return video_; }
    void setVideo(bool video);
    bool recording() const { return recording_; }
    QString lastVideo() const { return lastVideo_; }
    qreal digitalZoom() const { return digitalZoom_; }
    void setDigitalZoom(qreal zoom);

    FrameQueue *frames() { return &frames_; }

    // x and y are 0..1 across the upright preview, as the user sees it.
    Q_INVOKABLE void focusAt(qreal x, qreal y);
    Q_INVOKABLE void resetFocus();
    Q_INVOKABLE void capture();
    // In video mode: 1080p video with sound into the photo folder, until
    // stopRecording().
    Q_INVOKABLE void startRecording();
    Q_INVOKABLE void stopRecording();

Q_SIGNALS:
    void activeChanged();
    void stateChanged();
    void camerasChanged();
    void cameraChanged();
    void previewSizeChanged();
    void focusChanged();
    void photoFolderChanged();
    void lastPhotoChanged();
    void pendingChanged();
    void frameReady();
    void photoSaved(const QString &path);
    void photoFailed(const QString &reason);
    void videoChanged();
    void recordingChanged();
    void lastVideoChanged();
    void videoSaved(const QString &path);
    void videoFailed(const QString &reason);
    void digitalZoomChanged();

private:
    void setState(const QString &state, const QString &error = {});
    void writeStill(QImage image, StillInfo info);
    void mergeBurst(std::shared_ptr<std::vector<RawFrame>> frames, RawLayout layout,
                    int rotation, QString model);
    void finishPhoto(const QString &result);

    FrameQueue frames_;
    qreal digitalZoom_ = 1.0;
    // The digital zoom when the photo being saved was taken.
    qreal captureZoom_ = 1.0;
    QThread thread_;
    CameraWorker *worker_ = nullptr;
    bool active_ = false;
    bool opened_ = false;
    QString state_ = QStringLiteral("off");
    QString error_;
    QVariantList cameras_;
    int cameraIndex_ = 0;
    QString cameraName_;
    int rotation_ = 0;
    bool canFocus_ = false;
    QSize previewSize_{1440, 1080};
    QString focusState_ = QStringLiteral("idle");
    QPointF focusPoint_{-1, -1};
    QString photoFolder_;
    QString lastPhoto_;
    int pending_ = 0;
    bool canRecord_ = false;
    bool video_ = false;
    bool recording_ = false;
    QString lastVideo_;
};

// Owns libcamera. Every libcamera call except queueRequest() happens on this
// object's thread; completed requests arrive on libcamera's own thread.
class CameraWorker : public QObject
{
    Q_OBJECT

public:
    explicit CameraWorker(FrameQueue *frames);
    ~CameraWorker() override;

    // Opens cameras()[index] of the ordered list, releasing another one.
    void open(int index);
    void close();
    void startPreview(QSize size);
    void stopPreview();
    void setFocusWindow(QPointF framePoint, bool enabled);
    void capture();
    void requeue(const CameraFrame &frame);
    void setVideo(bool video);
    void startRecording(const QString &folder);
    void stopRecording();

    // Called on libcamera's thread with a finished burst.
    std::function<void(std::shared_ptr<std::vector<RawFrame>>, RawLayout, int, QString)> burstDone;

Q_SIGNALS:
    void camerasFound(const QVariantList &cameras);
    void opened(int index, const QString &name, int rotation, bool canFocus);
    void failed(const QString &message);
    void streamingChanged(const QString &mode);
    void frameReady();
    void focusStateChanged(const QString &state);
    void stillCaptured(const QImage &image, int rotation, qint64 exposureUs,
                       double gain, const QString &model);
    void burstStarted();
    void stillFailed(const QString &message);
    // A video ended: path empty on failure, with the reason in error.
    void recordingStopped(const QString &path, const QString &error);

private:
    enum Mode { Off, Preview, Still };

    void release();
    bool configure(libcamera::StreamRole role, QSize size, unsigned int buffers, bool withRaw);
    bool startStream(const libcamera::ControlList &initial);
    void stopStream();
    void queue(libcamera::Request *request);
    void onRequestCompleted(libcamera::Request *request);
    void handleStill(libcamera::Request *request, const libcamera::FrameBuffer *buffer);
    void finishStill(const QImage &image, qint64 exposureUs, double gain);
    libcamera::ControlList focusControls() const;
    void captureBurst();
    void keepBurstFrame(libcamera::Request *request, const libcamera::FrameBuffer *raw);
    void dumpBurstFrame(libcamera::Request *request, const RawFrame &frame);
    void recordFrame(libcamera::Request *request, const libcamera::FrameBuffer *buffer);
    void unmapFrames();

    FrameQueue *frames_;
    std::unique_ptr<libcamera::CameraManager> manager_;
    std::vector<std::shared_ptr<libcamera::Camera>> cameras_;
    std::shared_ptr<libcamera::Camera> camera_;
    std::unique_ptr<libcamera::CameraConfiguration> config_;
    std::unique_ptr<libcamera::FrameBufferAllocator> allocator_;
    std::vector<std::unique_ptr<libcamera::Request>> requests_;
    libcamera::Stream *stream_ = nullptr;
    // The sensor's raw frames, alongside the preview where the pipeline
    // offers them, for merged stills without reconfiguring.
    libcamera::Stream *rawStream_ = nullptr;
    RawLayout rawLayout_;
    libcamera::ControlList pending_;
    bool pendingSet_ = false;

    std::atomic<quint64> generation_{0};
    std::atomic<int> mode_{Off};
    std::atomic<bool> stillTaken_{false};
    std::atomic<float> lensPosition_{-1.0f};
    std::atomic<int> focusState_{-1};
    std::atomic<double> totalGain_{1.0};
    // Whether frames carry the white balance and colour matrix the merge
    // needs to render raw frames the way the image processor does.
    std::atomic<bool> rawRenderable_{false};
    std::atomic<int> burstWanted_{0};
    // Burst frames, filled on libcamera's thread while burstWanted_ counts down.
    std::shared_ptr<std::vector<RawFrame>> burst_;

    // Still capture, touched only from libcamera's thread while streaming.
    int stillFrames_ = 0;
    qint64 lastExposure_ = -1;
    double lastGain_ = -1.0;
    std::array<float, 2> lastColour_{ -1.0f, -1.0f };
    int stableFrames_ = 0;

    QSize previewSize_;
    QSize streamSize_;
    unsigned int stride_ = 0;
    quint32 fourcc_ = 0;
    libcamera::Rectangle activeArea_;
    int rotation_ = 0;
    bool canFocus_ = false;
    QString model_;
    bool focusWindow_ = false;
    QPointF focusPoint_;
    // Video: the preview is set up for recording, and frames go to the
    // recorder from libcamera's thread while recordingFrames_ is set.
    bool video_ = false;
    VideoRecorder recorder_;
    std::atomic<bool> recordingFrames_{false};
    // Frame buffers mapped for the recorder, by dma-buf fd: address, length.
    std::unordered_map<int, std::pair<void *, size_t>> maps_;
};
