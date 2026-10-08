#pragma once

#include <QMutex>
#include <QSize>
#include <QString>

typedef struct _GstElement GstElement;

// Records the camera's processed frames (32-bit RGB, as the viewfinder gets
// them) and the microphone into a video file, through GStreamer. The best
// encoder installed wins: hardware H.264 (v4l2h264enc) or x264, both into
// MP4, else Theora into Ogg. Sound is Opus, from PipeWire where it can.
class VideoRecorder
{
public:
    ~VideoRecorder();

    // Whether any video encoder is installed.
    static bool available();

    // Opens folder/VID_<time>.<ext> for frames of size and DRM fourcc;
    // rotation (degrees clockwise to turn upright) is stored in an MP4.
    bool start(const QString &folder, QSize size, quint32 fourcc, int rotation,
               QString *error);
    // Camera thread: one frame, taken at capturedNs (CLOCK_MONOTONIC).
    // Dropped, never waited for, if the encoder is behind.
    void push(const uchar *data, quint32 stride, qint64 capturedNs);
    // Ends the file and waits for it to be written; returns its path, or an
    // empty string on failure.
    QString stop(QString *error);
    bool running() const { return pipeline_ != nullptr; }

private:
    QMutex lock_;	// push() on the camera thread against stop()
    GstElement *pipeline_ = nullptr;
    GstElement *source_ = nullptr;
    QSize size_;
    QString path_;
    quint64 lastPts_ = 0;
};
