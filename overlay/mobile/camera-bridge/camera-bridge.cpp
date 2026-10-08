// Omarchy Mobile camera bridge: a phone camera as a plain webcam for apps
// that only know V4L2 webcams, such as the browser (getUserMedia: video
// calls, Snapchat). The phone's cameras are raw sensors that only libcamera,
// with its image processor, turns into pictures. (Android's external camera
// HAL wants MJPEG and calls such cameras "external", which apps that look
// for a front or back camera skip: not served yet.)
//
//   omarchy-camera-bridge DEVICE front|back
//
// DEVICE is a v4l2loopback node. The bridge sets its format (YUYV, 720x1280
// portrait, 30 fps) and writes one black frame, so apps see a webcam that is
// ready. The camera stays off until an app starts capturing: v4l2loopback
// tells the writer when the number of capturing readers changes (its
// client-usage event). Then libcamera streams the camera (the front one, or
// the back camera with the most pixels, as Omarchy Camera picks), and each
// frame is turned upright and converted. When the last reader stops, the
// camera goes off again. A camera another app holds (Omarchy Camera) leaves
// the webcam black until it is free.
//
// Frames are never stored: they go from the camera's buffers to the
// loopback device and nowhere else.

#include <libcamera/libcamera.h>

#include <linux/dma-buf.h>
#include <linux/videodev2.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace libcamera;

namespace {

// v4l2loopback.c: the number of capturing readers changed (0 or 1).
constexpr unsigned kClientUsageEvent = V4L2_EVENT_PRIVATE_START + 0x08E00000 + 1;
struct ClientUsage { __u32 count; };

constexpr unsigned kWidth = 720, kHeight = 1280;    // upright, as the phone is held
constexpr unsigned kSensorWidth = 1280, kSensorHeight = 720;
constexpr unsigned kBuffers = 4;
constexpr unsigned kFps = 30;

class Bridge {
public:
    Bridge(int out, bool front) : out_(out), front_(front), frame_(kWidth * kHeight * 2) {}

    bool setUp()
    {
        v4l2_format format = {};
        format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        format.fmt.pix.width = kWidth;
        format.fmt.pix.height = kHeight;
        format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        format.fmt.pix.field = V4L2_FIELD_NONE;
        format.fmt.pix.bytesperline = kWidth * 2;
        format.fmt.pix.sizeimage = kWidth * kHeight * 2;
        format.fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;
        if (ioctl(out_, VIDIOC_S_FMT, &format) < 0) {
            perror("VIDIOC_S_FMT");
            return false;
        }
        v4l2_streamparm parm = {};
        parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        parm.parm.output.timeperframe = { 1, kFps };
        ioctl(out_, VIDIOC_S_PARM, &parm);
        black();
        v4l2_event_subscription sub = {};
        sub.type = kClientUsageEvent;
        sub.flags = V4L2_EVENT_SUB_FL_SEND_INITIAL;
        if (ioctl(out_, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) {
            perror("VIDIOC_SUBSCRIBE_EVENT (v4l2loopback 0.13 or later)");
            return false;
        }
        return true;
    }

    // Readers started or stopped capturing.
    void readers(unsigned count)
    {
        if (count && !camera_)
            start();
        else if (!count && camera_)
            stop();
    }

    void stop()
    {
        if (!camera_)
            return;
        camera_->stop();
        camera_->requestCompleted.disconnect(this);
        requests_.clear();
        for (const auto &[fd, map] : maps_)
            munmap(map.first, map.second);
        maps_.clear();
        allocator_.reset();
        config_.reset();
        camera_->release();
        camera_.reset();
        manager_->stop();
        manager_.reset();
        black();
        fprintf(stderr, "camera off\n");
    }

private:
    void black()
    {
        for (size_t i = 0; i < frame_.size(); i += 2) {
            frame_[i] = 16;
            frame_[i + 1] = 128;
        }
        if (write(out_, frame_.data(), frame_.size()) < 0)
            perror("write");
    }

    std::shared_ptr<Camera> pick()
    {
        std::shared_ptr<Camera> best;
        long bestPixels = -1;
        for (const std::shared_ptr<Camera> &camera : manager_->cameras()) {
            const ControlList &props = camera->properties();
            const auto location = props.get(properties::Location);
            if (!location || (*location == properties::CameraLocationFront) != front_)
                continue;
            const auto size = props.get(properties::PixelArraySize);
            const long pixels = size ? long(size->width) * size->height : 0;
            if (pixels > bestPixels) {
                best = camera;
                bestPixels = pixels;
            }
        }
        return best;
    }

    void start()
    {
        manager_ = std::make_unique<CameraManager>();
        if (manager_->start()) {
            fprintf(stderr, "libcamera did not start\n");
            manager_.reset();
            return;
        }
        std::shared_ptr<Camera> camera = pick();
        if (!camera || camera->acquire()) {
            fprintf(stderr, "the %s camera is missing or in use\n", front_ ? "front" : "back");
            manager_->stop();
            manager_.reset();
            return;
        }
        camera_ = camera;
        // libcamera states the mounting rotation counter-clockwise; frames
        // are turned clockwise by the rest of a full turn.
        rotation_ = (360 - camera_->properties().get(properties::Rotation).value_or(0) % 360) % 360;
        config_ = camera_->generateConfiguration({ StreamRole::VideoRecording });
        if (!config_ || config_->empty()) {
            fail("no configuration");
            return;
        }
        StreamConfiguration &cfg = config_->at(0);
        const std::vector<PixelFormat> offered = cfg.formats().pixelformats();
        for (const PixelFormat &format : { formats::ABGR8888, formats::XBGR8888,
                                           formats::ARGB8888, formats::XRGB8888 }) {
            if (std::find(offered.begin(), offered.end(), format) != offered.end()) {
                cfg.pixelFormat = format;
                break;
            }
        }
        cfg.size = Size(kSensorWidth, kSensorHeight);
        cfg.bufferCount = kBuffers;
        if (config_->validate() == CameraConfiguration::Invalid || camera_->configure(config_.get())) {
            fail("configuration refused");
            return;
        }
        stream_ = cfg.stream();
        size_ = cfg.size;
        stride_ = cfg.stride;
        // ABGR/XBGR keep R first in memory, ARGB/XRGB keep B first.
        redFirst_ = cfg.pixelFormat == formats::ABGR8888 || cfg.pixelFormat == formats::XBGR8888;
        allocator_ = std::make_unique<FrameBufferAllocator>(camera_);
        if (allocator_->allocate(stream_) < 0) {
            fail("no buffers");
            return;
        }
        for (const std::unique_ptr<FrameBuffer> &buffer : allocator_->buffers(stream_)) {
            std::unique_ptr<Request> request = camera_->createRequest();
            if (!request || request->addBuffer(stream_, buffer.get())) {
                fail("no request");
                return;
            }
            requests_.push_back(std::move(request));
        }
        camera_->requestCompleted.connect(this, &Bridge::completed);
        ControlList initial(camera_->controls());
        if (camera_->controls().count(&controls::AfMode))
            initial.set(controls::AfMode, controls::AfModeContinuous);
        if (camera_->start(&initial)) {
            fail("did not start");
            return;
        }
        for (std::unique_ptr<Request> &request : requests_)
            camera_->queueRequest(request.get());
        fprintf(stderr, "camera on: %ux%u %s, turned %d degrees\n", size_.width, size_.height,
                cfg.pixelFormat.toString().c_str(), rotation_);
    }

    void fail(const char *what)
    {
        fprintf(stderr, "camera: %s\n", what);
        camera_->release();
        camera_.reset();
        config_.reset();
        allocator_.reset();
        requests_.clear();
        manager_->stop();
        manager_.reset();
    }

    // libcamera's thread.
    void completed(Request *request)
    {
        if (request->status() == Request::RequestCancelled)
            return;
        const FrameBuffer *buffer = request->buffers().begin()->second;
        const FrameBuffer::Plane &plane = buffer->planes()[0];
        const int fd = plane.fd.get();
        auto it = maps_.find(fd);
        if (it == maps_.end()) {
            const size_t length = plane.offset + plane.length;
            void *map = mmap(nullptr, length, PROT_READ, MAP_SHARED, fd, 0);
            if (map != MAP_FAILED)
                it = maps_.emplace(fd, std::make_pair(map, length)).first;
        }
        if (it != maps_.end()) {
            dma_buf_sync sync = { DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
            ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
            convert(static_cast<const uint8_t *>(it->second.first) + plane.offset);
            sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
            ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
            if (write(out_, frame_.data(), frame_.size()) < 0 && errno != EAGAIN)
                perror("write");
        }
        request->reuse(Request::ReuseBuffers);
        camera_->queueRequest(request);
    }

    // 32-bit RGB from the image processor to upright YUYV (BT.601, limited
    // range). The output is portrait; the sensor's frame is landscape and
    // turned by 90 or 270 degrees clockwise (other turns are turned by 90).
    void convert(const uint8_t *src)
    {
        const unsigned srcW = size_.width, srcH = size_.height;
        const int r = redFirst_ ? 0 : 2, b = redFirst_ ? 2 : 0;
        const bool cw270 = rotation_ == 270;
        for (unsigned y = 0; y < kHeight; y++) {
            uint8_t *dst = frame_.data() + size_t(y) * kWidth * 2;
            // Clockwise 90: out(x, y) = src(y, H-1-x); 270: src(W-1-y, x).
            const unsigned sx = std::min(cw270 ? srcW - 1 - y : y, srcW - 1);
            for (unsigned x = 0; x < kWidth; x += 2) {
                int Y[2], U = 0, V = 0;
                for (unsigned i = 0; i < 2; i++) {
                    const unsigned sy = std::min(cw270 ? x + i : srcH - 1 - (x + i), srcH - 1);
                    const uint8_t *p = src + size_t(sy) * stride_ + sx * 4;
                    const int R = p[r], G = p[1], B = p[b];
                    Y[i] = ((66 * R + 129 * G + 25 * B + 128) >> 8) + 16;
                    U += ((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128;
                    V += ((112 * R - 94 * G - 18 * B + 128) >> 8) + 128;
                }
                dst[0] = uint8_t(Y[0]);
                dst[1] = uint8_t(U / 2);
                dst[2] = uint8_t(Y[1]);
                dst[3] = uint8_t(V / 2);
                dst += 4;
            }
        }
    }

    const int out_;
    const bool front_;
    std::vector<uint8_t> frame_;
    std::unique_ptr<CameraManager> manager_;
    std::shared_ptr<Camera> camera_;
    std::unique_ptr<CameraConfiguration> config_;
    std::unique_ptr<FrameBufferAllocator> allocator_;
    std::vector<std::unique_ptr<Request>> requests_;
    std::map<int, std::pair<void *, size_t>> maps_;
    Stream *stream_ = nullptr;
    Size size_;
    unsigned stride_ = 0;
    int rotation_ = 0;
    bool redFirst_ = true;
};

} // namespace

int main(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[2], "front") && strcmp(argv[2], "back"))) {
        fprintf(stderr, "usage: %s DEVICE front|back\n", argv[0]);
        return 2;
    }
    const int out = open(argv[1], O_RDWR | O_CLOEXEC);
    if (out < 0) {
        perror(argv[1]);
        return 1;
    }
    // TERM and INT through a descriptor: libcamera's threads, which inherit
    // the mask, never take them, and the loop below sees them with the device.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    const int signals = signalfd(-1, &mask, SFD_CLOEXEC);
    Bridge bridge(out, !strcmp(argv[2], "front"));
    if (signals < 0 || !bridge.setUp())
        return 1;
    for (;;) {
        pollfd fds[] = { { out, POLLPRI, 0 }, { signals, POLLIN, 0 } };
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }
        if (fds[1].revents)
            break;
        // DQEVENT blocks once the queue is empty: stop at the last pending.
        v4l2_event event = {};
        do {
            if (ioctl(out, VIDIOC_DQEVENT, &event) < 0)
                break;
            if (event.type == kClientUsageEvent)
                bridge.readers(reinterpret_cast<const ClientUsage *>(&event.u)->count);
        } while (event.pending > 0);
    }
    bridge.stop();
    close(out);
    return 0;
}
