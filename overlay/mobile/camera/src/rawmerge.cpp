#include "rawmerge.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QtConcurrent/QtConcurrentMap>

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <utility>

namespace {

using Plane = std::vector<uint16_t>;

// Tiles for motion rejection, in 2x2 Bayer quads: 32 pixels across.
constexpr int kTile = 16;
// Alignment search, in quads at a quarter of the quad resolution and then at
// full quad resolution around the coarse result.
constexpr int kCoarseFactor = 4;
constexpr int kCoarseRange = 8;
constexpr int kFineRange = 3;
// A tile merges fully while it differs from the reference by up to 1.4 times
// the typical difference of a still tile, and not at all beyond twice that.
// Averaged over a tile, still tiles fall within about 1.2 times; anything
// that moved lands well beyond.
constexpr float kMatchNoise = 1.4f;
constexpr float kRejectNoise = 2.0f;
// Read noise floor in quad sums, so dark tiles are not judged by shot noise
// alone.
constexpr float kReadNoiseFloor = 32.0f;
// The reference's own noise comes from its flattest tiles, this percentile;
// differences between frames may show up to this much more before the
// reference's estimate caps them.
constexpr size_t kFlatShare = 10;
constexpr float kSpatialSlack = 1.5f;
// White balance: one sample every this many pixels each way...
constexpr int kBalanceStep = 16;
// ...leaving out samples near clipping or near black...
constexpr float kBalanceClip = 0.85f;
constexpr float kBalanceDark = 0.01f;
// ...weighting the rest by how close to grey they are, out to this distance
// in rg-chromaticity...
constexpr float kBalanceGrey = 0.08f;
// ...when enough of the frame is close enough to grey. That sets the tint
// only, within this factor of the image processor's.
constexpr double kBalanceMinShare = 0.05;
constexpr float kBalanceTrust = 1.3f;
// Warm light keeps some warmth: none below this blue-to-red gain ratio,
// growing in log steps to 1 at the next and up to kMostWarmth beyond, where
// red is raised and blue lowered by these shares for each step, after colour
// correction. A room lit only by a salt lamp stays orange.
constexpr float kWarmFrom = 1.3f;
constexpr float kWarmTo = 3.5f;
constexpr float kMostWarmth = 2.0f;
constexpr float kWarmRed = 0.12f;
constexpr float kWarmBlue = 0.20f;
// Very warm light (a fire, candles) is not neutralised past this blue-to-red
// gain ratio: the eye keeps firelight orange, and the large blue gain it
// would take lifted the dim blue of embers and flame edges into pink
// (October 4). Indoor bulbs (about 2.2) stay below it.
constexpr float kMaxBlueRed = 2.6f;
// Black point: the darkest share of the frame goes to black, unless that
// would take more than this much of the sensor's range.
constexpr double kBlackShare = 0.005;
constexpr float kMaxBlack = 0.005f;
// Exposure: the photo's log-average brightness goes up to a key, from the
// image processor's own exposure (its digital gain) to at most 1 + lift
// times that, more for more frames merged, as there is less noise to show.
// A bright scene keeps the image processor's exposure. The key follows the
// scene's light, its sensor average per second of exposure at unity gain:
// kKey from kDayLight up, down to kNightKey at kNightLight and below, so a
// room lit by one lamp still looks like night (a Pixel 9 Pro's photo of it
// was about half as bright as ours at kKey).
constexpr float kKey = 0.18f;
constexpr float kNightKey = 0.09f;
constexpr float kDayLight = 0.5f;
constexpr float kNightLight = 0.04f;
constexpr float kMaxLift = 3.0f;
constexpr float kLiftBase = 1.5f;
constexpr float kLiftPerFrame = 0.25f;
// Highlights: brightness more than kKneeStops above the key is compressed,
// just enough in log terms that the brightest share of the frame lands at
// kWhite, and never below kMinSlope stops out per stop in.
constexpr float kKneeStops = 1.0f;
constexpr float kKneeSharpness = 2.0f;
constexpr double kBrightShare = 0.01;
constexpr float kWhite = 0.85f;
constexpr float kMinSlope = 0.35f;
// The tone curve follows the brightness around each point: log brightness at
// an eighth of the quad resolution (16 pixels), smoothed over this many of
// those cells each way (about 130 pixels), except across edges whose log
// brightness varies by more than this (a variance)...
constexpr int kBaseStep = 8;
constexpr int kBaseRadius = 8;
constexpr float kBaseEdge = 0.3f;
constexpr float kBaseFloor = 1e-5f;
// ...and detail the curve takes past this rolls off smoothly below white.
constexpr float kClipKnee = 0.8f;
// Veiling glare: the frame's light spread over Gaussians of these widths in
// coarse cells (about 50, 190 and 640 pixels), from the bright parts only
// (a lamp, a window), sources near clipping counted this many times over
// since their true brightness is lost...
constexpr float kGlareWidths[] = { 3.0f, 12.0f, 40.0f };
constexpr float kGlareSourceFrom = 0.15f;
constexpr float kGlareSourceTo = 0.35f;
constexpr float kGlareClip = 0.8f;
constexpr float kGlareClipBoost = 4.0f;
// ...each width scaled in turn as far as the darkest content allows: this
// share of the cells may sit below the estimate, which stays under this
// fraction of it...
constexpr double kGlareFloorShare = 0.01;
constexpr float kGlareSafety = 0.8f;
// ...and never takes more than this share of a pixel.
constexpr float kGlareMost = 0.85f;

template<typename F>
void parallelRows(int rows, F &&work)
{
    std::vector<std::pair<int, int>> bands;
    for (int y = 0; y < rows; y += 32)
        bands.emplace_back(y, std::min(rows, y + 32));
    QtConcurrent::blockingMap(bands, [&work](const std::pair<int, int> &band) {
        work(band.first, band.second);
    });
}

Plane unpack(const RawFrame &frame, const RawLayout &layout)
{
    const int width = layout.size.width();
    const int height = layout.size.height();
    Plane out(size_t(width) * height);
    parallelRows(height, [&](int first, int last) {
        for (int y = first; y < last; ++y) {
            const uint8_t *row = frame.data.data() + size_t(y) * layout.stride;
            uint16_t *dst = out.data() + size_t(y) * width;
            if (layout.csi2Packed && layout.bits == 10) {
                // Four pixels in five bytes: high bits, then the low bits.
                for (int x = 0; x + 3 < width; x += 4) {
                    const uint8_t *p = row + x / 4 * 5;
                    dst[x] = uint16_t(p[0] << 2 | (p[4] & 3));
                    dst[x + 1] = uint16_t(p[1] << 2 | ((p[4] >> 2) & 3));
                    dst[x + 2] = uint16_t(p[2] << 2 | ((p[4] >> 4) & 3));
                    dst[x + 3] = uint16_t(p[3] << 2 | (p[4] >> 6));
                }
            } else if (layout.csi2Packed && layout.bits == 12) {
                // Two pixels in three bytes.
                for (int x = 0; x + 1 < width; x += 2) {
                    const uint8_t *p = row + x / 2 * 3;
                    dst[x] = uint16_t(p[0] << 4 | (p[2] & 0xf));
                    dst[x + 1] = uint16_t(p[1] << 4 | (p[2] >> 4));
                }
            } else if (layout.bits == 8) {
                for (int x = 0; x < width; ++x)
                    dst[x] = row[x];
            } else {
                const auto *src = reinterpret_cast<const uint16_t *>(row);
                for (int x = 0; x < width; ++x)
                    dst[x] = src[x];
            }
        }
    });
    return out;
}

// Sums of each 2x2 Bayer quad: a quarter-size image with every colour in it.
struct Quads {
    int width = 0;
    int height = 0;
    std::vector<float> values;
    float at(int x, int y) const
    {
        x = std::clamp(x, 0, width - 1);
        y = std::clamp(y, 0, height - 1);
        return values[size_t(y) * width + x];
    }
};

Quads quads(const Plane &raw, QSize size)
{
    Quads q;
    q.width = size.width() / 2;
    q.height = size.height() / 2;
    q.values.resize(size_t(q.width) * q.height);
    parallelRows(q.height, [&](int first, int last) {
        for (int y = first; y < last; ++y) {
            const uint16_t *top = raw.data() + size_t(2 * y) * size.width();
            const uint16_t *bottom = top + size.width();
            for (int x = 0; x < q.width; ++x)
                q.values[size_t(y) * q.width + x] =
                    float(top[2 * x] + top[2 * x + 1] + bottom[2 * x] + bottom[2 * x + 1]);
        }
    });
    return q;
}

Quads shrink(const Quads &in, int factor)
{
    Quads out;
    out.width = in.width / factor;
    out.height = in.height / factor;
    out.values.resize(size_t(out.width) * out.height);
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x) {
            float sum = 0;
            for (int j = 0; j < factor; ++j)
                for (int i = 0; i < factor; ++i)
                    sum += in.values[size_t(y * factor + j) * in.width + x * factor + i];
            out.values[size_t(y) * out.width + x] = sum / float(factor * factor);
        }
    return out;
}

// Mean absolute difference over the central 60% of the reference.
double difference(const Quads &ref, const Quads &frame, int dx, int dy, int step)
{
    const int x0 = ref.width / 5, x1 = ref.width - ref.width / 5;
    const int y0 = ref.height / 5, y1 = ref.height - ref.height / 5;
    double sum = 0;
    size_t count = 0;
    for (int y = y0; y < y1; y += step)
        for (int x = x0; x < x1; x += step) {
            sum += std::abs(frame.at(x + dx, y + dy) - ref.values[size_t(y) * ref.width + x]);
            ++count;
        }
    return count ? sum / count : 0.0;
}

// The shift, in quads, that best lays frame over ref: coarse, then fine.
std::array<int, 2> align(const Quads &ref, const Quads &refCoarse, const Quads &frame,
                         const Quads &frameCoarse)
{
    std::array<int, 2> best{ 0, 0 };
    double bestScore = difference(refCoarse, frameCoarse, 0, 0, 1);
    for (int dy = -kCoarseRange; dy <= kCoarseRange; ++dy)
        for (int dx = -kCoarseRange; dx <= kCoarseRange; ++dx) {
            const double score = difference(refCoarse, frameCoarse, dx, dy, 1);
            if (score < bestScore) {
                bestScore = score;
                best = { dx, dy };
            }
        }

    const std::array<int, 2> centre{ best[0] * kCoarseFactor, best[1] * kCoarseFactor };
    std::vector<std::array<int, 2>> candidates;
    for (int dy = -kFineRange; dy <= kFineRange; ++dy)
        for (int dx = -kFineRange; dx <= kFineRange; ++dx)
            candidates.push_back({ centre[0] + dx, centre[1] + dy });
    std::vector<double> scores(candidates.size());
    std::vector<size_t> indices(candidates.size());
    std::iota(indices.begin(), indices.end(), 0);
    QtConcurrent::blockingMap(indices, [&](size_t i) {
        scores[i] = difference(ref, frame, candidates[i][0], candidates[i][1], 2);
    });
    return candidates[std::min_element(scores.begin(), scores.end()) - scores.begin()];
}

// Noise per tile, normalised by the square root of its signal as shot noise
// grows, from the differences between neighbouring quads of the reference in
// its flattest tiles. Differences between frames measure the same thing where
// nothing moves, but motion across most of the frame would inflate them.
float spatialNoise(const Quads &ref, float black, int tilesX, int tilesY)
{
    std::vector<float> normalised;
    normalised.reserve(size_t(tilesX) * tilesY);
    for (int ty = 0; ty < tilesY; ++ty)
        for (int tx = 0; tx < tilesX; ++tx) {
            double sum = 0, delta = 0;
            int count = 0;
            for (int y = ty * kTile; y < std::min(ref.height, (ty + 1) * kTile); ++y)
                for (int x = tx * kTile; x + 1 < std::min(ref.width, (tx + 1) * kTile); ++x) {
                    const float r = ref.values[size_t(y) * ref.width + x];
                    sum += r;
                    delta += std::abs(ref.values[size_t(y) * ref.width + x + 1] - r);
                    ++count;
                }
            if (count)
                normalised.push_back(float(delta / count) /
                                     std::sqrt(std::max(0.0, sum / count - black) + kReadNoiseFloor));
        }
    if (normalised.empty())
        return 1e9f;
    const size_t pick = normalised.size() * kFlatShare / 100;
    std::nth_element(normalised.begin(), normalised.begin() + pick, normalised.end());
    return std::max(normalised[pick], 1e-3f);
}

// Weight of each tile of frame against ref, from how far it differs beyond
// the noise. Most tiles of a burst are still, so a low percentile of the
// normalised differences estimates the noise, but never above what the
// reference alone shows.
std::vector<float> tileWeights(const Quads &ref, const Quads &frame, std::array<int, 2> shift,
                               float black, float spatial, int tilesX, int tilesY,
                               double *rejected)
{
    std::vector<float> level(size_t(tilesX) * tilesY), diff(level.size());
    for (int ty = 0; ty < tilesY; ++ty)
        for (int tx = 0; tx < tilesX; ++tx) {
            double sum = 0, delta = 0;
            int count = 0;
            for (int y = ty * kTile; y < std::min(ref.height, (ty + 1) * kTile); ++y)
                for (int x = tx * kTile; x < std::min(ref.width, (tx + 1) * kTile); ++x) {
                    const float r = ref.values[size_t(y) * ref.width + x];
                    sum += r;
                    delta += std::abs(frame.at(x + shift[0], y + shift[1]) - r);
                    ++count;
                }
            const size_t i = size_t(ty) * tilesX + tx;
            level[i] = count ? float(sum / count) : 0.0f;
            diff[i] = count ? float(delta / count) : 0.0f;
        }

    std::vector<float> normalised(level.size());
    for (size_t i = 0; i < level.size(); ++i)
        normalised[i] = diff[i] / std::sqrt(std::max(0.0f, level[i] - black) + kReadNoiseFloor);
    std::vector<float> sorted = normalised;
    const size_t pick = sorted.size() * 35 / 100;
    std::nth_element(sorted.begin(), sorted.begin() + pick, sorted.end());
    const float scale = std::clamp(sorted[pick], 1e-3f, kSpatialSlack * spatial);

    std::vector<float> match(level.size());
    for (size_t i = 0; i < level.size(); ++i) {
        const float noise = scale * std::sqrt(std::max(0.0f, level[i] - black) + kReadNoiseFloor);
        match[i] = std::clamp((kRejectNoise * noise - diff[i]) /
                                  ((kRejectNoise - kMatchNoise) * noise), 0.0f, 1.0f);
    }

    // A tile next to one that moved takes the lower weight too: an edge of
    // the moving thing may cover only a little of it.
    std::vector<float> weights(level.size());
    double left = 0;
    for (int ty = 0; ty < tilesY; ++ty)
        for (int tx = 0; tx < tilesX; ++tx) {
            float w = 1.0f;
            for (int ny = std::max(0, ty - 1); ny <= std::min(tilesY - 1, ty + 1); ++ny)
                for (int nx = std::max(0, tx - 1); nx <= std::min(tilesX - 1, tx + 1); ++nx)
                    w = std::min(w, match[size_t(ny) * tilesX + nx]);
            weights[size_t(ty) * tilesX + tx] = w;
            left += 1.0 - w;
        }
    *rejected = weights.empty() ? 0.0 : left / weights.size();
    return weights;
}

// Per-quad weights, blended between tile centres so tile edges leave no seams.
std::vector<float> quadWeights(const std::vector<float> &tiles, int tilesX, int tilesY,
                               int width, int height)
{
    std::vector<float> out(size_t(width) * height);
    parallelRows(height, [&](int first, int last) {
        for (int y = first; y < last; ++y) {
            const float fy = std::clamp((y + 0.5f) / kTile - 0.5f, 0.0f, float(tilesY - 1));
            const int y0 = int(fy), y1 = std::min(y0 + 1, tilesY - 1);
            const float wy = fy - y0;
            for (int x = 0; x < width; ++x) {
                const float fx = std::clamp((x + 0.5f) / kTile - 0.5f, 0.0f, float(tilesX - 1));
                const int x0 = int(fx), x1 = std::min(x0 + 1, tilesX - 1);
                const float wx = fx - x0;
                const float top = tiles[size_t(y0) * tilesX + x0] * (1 - wx) +
                                  tiles[size_t(y0) * tilesX + x1] * wx;
                const float bottom = tiles[size_t(y1) * tilesX + x0] * (1 - wx) +
                                     tiles[size_t(y1) * tilesX + x1] * wx;
                out[size_t(y) * width + x] = top * (1 - wy) + bottom * wy;
            }
        }
    });
    return out;
}

struct Matrix3 {
    std::array<float, 9> m;
    Matrix3 operator*(const Matrix3 &o) const
    {
        Matrix3 r{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                float s = 0;
                for (int k = 0; k < 3; ++k)
                    s += m[i * 3 + k] * o.m[k * 3 + j];
                r.m[i * 3 + j] = s;
            }
        return r;
    }
};

// The software ISP's saturation, applied the same way (BT.601 YCbCr).
Matrix3 saturationMatrix(float saturation)
{
    const Matrix3 toYcc{ { 0.256788235294f, 0.504129411765f, 0.0979058823529f,
                           -0.148223529412f, -0.290992156863f, 0.439215686275f,
                           0.439215686275f, -0.367788235294f, -0.0714274509804f } };
    const Matrix3 toRgb{ { 1.16438356164f, 0.0f, 1.59602678571f,
                           1.16438356164f, -0.391762290094f, -0.812967647235f,
                           1.16438356164f, 2.01723214285f, 0.0f } };
    const Matrix3 scale{ { 1, 0, 0, 0, saturation, 0, 0, 0, saturation } };
    return toRgb * scale * toYcc;
}


// White balance for the photo, from the merged frame. The image processor
// balances the average of the whole frame to grey, so a lamp or a wall of
// leaves in view tints everything else. This balances the parts of the frame
// that are nearly grey instead, starting from the image processor's gains,
// and says how warm the light was, so the photo can keep some of that
// warmth, as the eye does.
struct Balance {
    std::array<float, 2> gains;
    float warmth = 0.0f;   // 0 in daylight to 1 under a candle-coloured lamp
    float tint = 1.0f;     // red and blue against the image processor's
};

Balance balance(const std::vector<float> &raw, const RawLayout &layout, float black,
                             float white, std::array<float, 2> gains)
{
    struct Sample {
        float r, g, b;
    };
    const int width = layout.size.width();
    const int height = layout.size.height();
    const float range = white - black;
    const int rx = layout.redX, ry = layout.redY;
    std::vector<Sample> samples;
    samples.reserve(size_t(width / kBalanceStep + 1) * (height / kBalanceStep + 1));
    for (int y = 0; y + 1 < height; y += kBalanceStep) {
        for (int x = 0; x + 1 < width; x += kBalanceStep) {
            const auto at = [&](int dx, int dy) {
                return raw[size_t(y + dy) * width + x + dx] - black;
            };
            const Sample s{ at(rx, ry), (at(1 - rx, ry) + at(rx, 1 - ry)) / 2,
                            at(1 - rx, 1 - ry) };
            // Clipped samples have lost their colour; the darkest are noise.
            if (std::max({ s.r, s.g, s.b }) > kBalanceClip * range || s.g < kBalanceDark * range)
                continue;
            samples.push_back(s);
        }
    }

    const std::array<float, 2> start = gains;
    for (int pass = 0; pass < 4; ++pass) {
        double r = 0, g = 0, b = 0, total = 0;
        for (const Sample &s : samples) {
            const float cr = s.r * gains[0], cg = s.g, cb = s.b * gains[1];
            const float sum = cr + cg + cb;
            const float dr = cr / sum - 1.0f / 3, db = cb / sum - 1.0f / 3;
            const float w = 1.0f - std::sqrt(dr * dr + db * db) / kBalanceGrey;
            if (w <= 0.0f)
                continue;
            r += w * s.r;
            g += w * s.g;
            b += w * s.b;
            total += w;
        }
        // Too little grey in view to judge: keep what the image processor chose.
        if (total < kBalanceMinShare * samples.size() || r <= 0 || b <= 0)
            break;
        gains = { float(g / r), float(g / b) };
    }

    // Only the tint, green against magenta, comes from the grey search; how
    // warm or cool the light is stays the image processor's call. Its
    // average of the whole frame judges that well enough, while the search,
    // with little grey in view as at night by one lamp, can wander along it.
    // The tint stays within kBalanceTrust of the image processor's too.
    const float tint = std::clamp(std::sqrt(gains[0] * gains[1]) / std::sqrt(start[0] * start[1]),
                                  1.0f / kBalanceTrust, kBalanceTrust);
    gains = { start[0] * tint, start[1] * tint };
    // Past kMaxBlueRed, red is raised and blue lowered by the same factor,
    // keeping their product (the green balance).
    if (gains[1] / gains[0] > kMaxBlueRed) {
        const float ease = std::sqrt(gains[1] / gains[0] / kMaxBlueRed);
        gains = { gains[0] * ease, gains[1] / ease };
    }

    const float ratio = std::log(gains[1] / gains[0]);
    const float warmth = std::clamp((ratio - std::log(kWarmFrom)) /
                                        (std::log(kWarmTo) - std::log(kWarmFrom)),
                                    0.0f, kMostWarmth);
    return { gains, warmth, tint };
}

// Linear RGB of the quad at x, y (in quads) of a raw Bayer frame.
using QuadColour = std::function<void(int x, int y, float &r, float &g, float &b)>;

// The black point: the darkest few samples, on the brightest channel, go to
// black, removing the veil lens flare and the noise floor leave.
float blackPoint(const RawLayout &layout, const QuadColour &colourAt)
{
    const int qw = layout.size.width() / 2, qh = layout.size.height() / 2;
    const int step = kBalanceStep / 2;
    std::vector<float> peaks;
    peaks.reserve(size_t(qw / step + 1) * (qh / step + 1));
    for (int y = 0; y < qh; y += step)
        for (int x = 0; x < qw; x += step) {
            float r, g, b;
            colourAt(x, y, r, g, b);
            peaks.push_back(std::max({ r, g, b }));
        }
    if (peaks.empty())
        return 0.0f;
    const size_t pick = size_t(double(peaks.size()) * kBlackShare);
    std::nth_element(peaks.begin(), peaks.begin() + pick, peaks.end());
    return std::clamp(peaks[pick], 0.0f, kMaxBlack);
}

// The photo's tone curve, on the smoothed brightness around a point, in
// stops (log2) from the key. An exposure brings the frame's log-average
// brightness to the key, within what the noise allows. Above a knee a stop
// above the key, the curve then compresses stops just enough that the
// brightest 1% of the frame lands at kWhite: a lamp in a dark room comes
// down into range while a daylight scene is left alone. Detail rides on
// top unchanged, so the lamp's surroundings keep their contrast.
struct Tone {
    float key = kKey;
    float light = 0.0f;   // the scene's, as localGain measured it
    float exposure = 1.0f;
    float slope = 1.0f;   // stops out per stop in, well above the knee

    static float softplus(float stops)
    {
        return std::log2(1.0f + std::exp2(kKneeSharpness * stops)) / kKneeSharpness;
    }

    // Output brightness for a smoothed brightness base.
    float operator()(float base) const
    {
        const float x = std::log2(std::max(base * exposure, kBaseFloor) / key);
        const float y = x - (1.0f - slope) * softplus(x - kKneeStops);
        return key * std::exp2(y);
    }
};

// Box mean over a (2 radius + 1) square window, clamped at the edges.
std::vector<float> boxMean(const std::vector<float> &in, int width, int height, int radius)
{
    std::vector<double> sums(size_t(width + 1) * (height + 1), 0.0);
    for (int y = 0; y < height; ++y) {
        double row = 0;
        for (int x = 0; x < width; ++x) {
            row += in[size_t(y) * width + x];
            sums[size_t(y + 1) * (width + 1) + x + 1] = sums[size_t(y) * (width + 1) + x + 1] + row;
        }
    }
    std::vector<float> out(in.size());
    for (int y = 0; y < height; ++y) {
        const int y0 = std::max(0, y - radius), y1 = std::min(height, y + radius + 1);
        for (int x = 0; x < width; ++x) {
            const int x0 = std::max(0, x - radius), x1 = std::min(width, x + radius + 1);
            const double sum = sums[size_t(y1) * (width + 1) + x1] - sums[size_t(y0) * (width + 1) + x1] -
                               sums[size_t(y1) * (width + 1) + x0] + sums[size_t(y0) * (width + 1) + x0];
            out[size_t(y) * width + x] = float(sum / double((x1 - x0) * (y1 - y0)));
        }
    }
    return out;
}

// The tone curve as a gain per 2x2 quad, taken at the brightness around the
// quad rather than its own, so texture keeps its contrast: a leaf by the lamp
// is brightened as much as its surroundings, veins and all, where the curve
// on each pixel's own value would flatten it into its shoulder. The
// surroundings come from a guided filter of the log brightness by itself, at
// an eighth of the quad resolution: smooth over about kBaseRadius, but
// following strong edges, so a dark statue in front of the lamp is judged
// dark without a halo around it.
std::vector<float> localGain(const RawLayout &layout, const QuadColour &colourAt,
                             float ispExposure, float sensorExposure, int frames,
                             Tone *toneOut)
{
    const int qw = layout.size.width() / 2, qh = layout.size.height() / 2;

    // Log brightness per quad, as the brightest channel so saturated colours
    // count as bright and are not pushed out of range.
    std::vector<float> logs(size_t(qw) * qh);
    parallelRows(qh, [&](int first, int last) {
        for (int y = first; y < last; ++y)
            for (int x = 0; x < qw; ++x) {
                float r, g, b;
                colourAt(x, y, r, g, b);
                logs[size_t(y) * qw + x] = std::log(std::max({ r, g, b, kBaseFloor }));
            }
    });

    const int lw = (qw + kBaseStep - 1) / kBaseStep, lh = (qh + kBaseStep - 1) / kBaseStep;
    std::vector<float> low(size_t(lw) * lh), lowSquared(low.size());
    for (int y = 0; y < lh; ++y)
        for (int x = 0; x < lw; ++x) {
            double sum = 0;
            int count = 0;
            for (int j = y * kBaseStep; j < std::min(qh, (y + 1) * kBaseStep); ++j)
                for (int i = x * kBaseStep; i < std::min(qw, (x + 1) * kBaseStep); ++i) {
                    sum += logs[size_t(j) * qw + i];
                    ++count;
                }
            const float mean = float(sum / count);
            low[size_t(y) * lw + x] = mean;
            lowSquared[size_t(y) * lw + x] = mean * mean;
        }

    const std::vector<float> mean = boxMean(low, lw, lh, kBaseRadius);
    const std::vector<float> meanSquared = boxMean(lowSquared, lw, lh, kBaseRadius);
    std::vector<float> slope(low.size()), offset(low.size());
    for (size_t i = 0; i < low.size(); ++i) {
        const float variance = std::max(0.0f, meanSquared[i] - mean[i] * mean[i]);
        slope[i] = variance / (variance + kBaseEdge);
        offset[i] = mean[i] - slope[i] * mean[i];
    }
    const std::vector<float> slopes = boxMean(slope, lw, lh, kBaseRadius);
    const std::vector<float> offsets = boxMean(offset, lw, lh, kBaseRadius);

    // Exposure from the log-average, then the compression from the
    // brightest share, both over the coarse grid.
    Tone tone;
    const double average = std::accumulate(low.begin(), low.end(), 0.0) / double(low.size());
    const float key = float(std::exp(average));
    const float most = ispExposure *
                       (1.0f + std::min(kMaxLift, kLiftBase + kLiftPerFrame * float(frames)));
    const float light = sensorExposure > 0.0f ? key / sensorExposure : kDayLight;
    const float day = std::clamp(std::log(light / kNightLight) / std::log(kDayLight / kNightLight),
                                 0.0f, 1.0f);
    tone.key = kNightKey * std::pow(kKey / kNightKey, day);
    tone.light = light;
    tone.exposure = std::clamp(tone.key / key, ispExposure, most);
    std::vector<float> sorted = low;
    const size_t pick = size_t(double(sorted.size()) * (1.0 - kBrightShare));
    std::nth_element(sorted.begin(), sorted.begin() + pick, sorted.end());
    const float bright = std::log2(std::exp(sorted[pick]) * tone.exposure / tone.key);
    const float white = std::log2(kWhite / tone.key);
    if (bright > white)
        tone.slope = std::clamp(1.0f - (bright - white) / Tone::softplus(bright - kKneeStops),
                                kMinSlope, 1.0f);
    *toneOut = tone;

    std::vector<float> gains(logs.size());
    parallelRows(qh, [&](int first, int last) {
        for (int y = first; y < last; ++y) {
            const float fy = std::clamp((y + 0.5f) / kBaseStep - 0.5f, 0.0f, float(lh - 1));
            const int y0 = int(fy), y1 = std::min(y0 + 1, lh - 1);
            const float wy = fy - y0;
            for (int x = 0; x < qw; ++x) {
                const float fx = std::clamp((x + 0.5f) / kBaseStep - 0.5f, 0.0f, float(lw - 1));
                const int x0 = int(fx), x1 = std::min(x0 + 1, lw - 1);
                const float wx = fx - x0;
                const auto blend = [&](const std::vector<float> &v) {
                    return (v[size_t(y0) * lw + x0] * (1 - wx) + v[size_t(y0) * lw + x1] * wx) * (1 - wy) +
                           (v[size_t(y1) * lw + x0] * (1 - wx) + v[size_t(y1) * lw + x1] * wx) * wy;
                };
                const float base = std::exp(blend(slopes) * logs[size_t(y) * qw + x] + blend(offsets));
                gains[size_t(y) * qw + x] = tone(base) / base;
            }
        }
    });
    return gains;
}

// Three box passes approximate a Gaussian of this width, in cells.
std::vector<float> gaussian(std::vector<float> v, int width, int height, float sigma)
{
    const int radius = std::max(1, int(std::lround(std::sqrt(sigma * sigma + 0.25f) - 0.5f)));
    for (int pass = 0; pass < 3; ++pass)
        v = boxMean(v, width, height, radius);
    return v;
}

// Veiling glare: light the lens scatters from bright things across the
// frame, a haze over everything dark near a lamp that brightening the room
// makes plain. Its shape is the frame's light spread wide; its strength is
// the most the frame allows, since glare cannot be brighter than the darkest
// thing it lies on. That keeps the estimate safe: it only takes off a haze
// the frame proves is there.
struct Glare {
    int width = 0;
    int height = 0;
    std::array<float, std::size(kGlareWidths)> shares{};
    std::vector<float> planes[3];

    // x and y in quads.
    void at(float x, float y, float *out) const
    {
        const float fx = std::clamp((x + 0.5f) / kBaseStep - 0.5f, 0.0f, float(width - 1));
        const float fy = std::clamp((y + 0.5f) / kBaseStep - 0.5f, 0.0f, float(height - 1));
        const int x0 = int(fx), x1 = std::min(x0 + 1, width - 1);
        const int y0 = int(fy), y1 = std::min(y0 + 1, height - 1);
        const float wx = fx - x0, wy = fy - y0;
        for (int c = 0; c < 3; ++c) {
            const std::vector<float> &v = planes[c];
            out[c] = (v[size_t(y0) * width + x0] * (1 - wx) + v[size_t(y0) * width + x1] * wx) * (1 - wy) +
                     (v[size_t(y1) * width + x0] * (1 - wx) + v[size_t(y1) * width + x1] * wx) * wy;
        }
    }
};

Glare glareFor(const RawLayout &layout, const QuadColour &colourAt)
{
    const int qw = layout.size.width() / 2, qh = layout.size.height() / 2;
    Glare glare;
    glare.width = (qw + kBaseStep - 1) / kBaseStep;
    glare.height = (qh + kBaseStep - 1) / kBaseStep;
    const size_t cells = size_t(glare.width) * glare.height;

    // Per cell and channel, the mean and the floor: the darkest of its four
    // 4x4 blocks of quads, averaged so noise does not set it.
    std::vector<float> mean[3], floor[3];
    for (int c = 0; c < 3; ++c) {
        mean[c].assign(cells, 0.0f);
        floor[c].assign(cells, 0.0f);
    }
    parallelRows(glare.height, [&](int first, int last) {
        for (int cy = first; cy < last; ++cy)
            for (int cx = 0; cx < glare.width; ++cx) {
                double sum[3] = { 0, 0, 0 };
                float low[3] = { 1e9f, 1e9f, 1e9f };
                int count = 0;
                constexpr int kBlock = kBaseStep / 2;
                for (int by = cy * kBaseStep; by + kBlock <= std::min(qh, (cy + 1) * kBaseStep); by += kBlock)
                    for (int bx = cx * kBaseStep; bx + kBlock <= std::min(qw, (cx + 1) * kBaseStep); bx += kBlock) {
                        float block[3] = { 0, 0, 0 };
                        for (int j = 0; j < kBlock; ++j)
                            for (int i = 0; i < kBlock; ++i) {
                                float r, g, b;
                                colourAt(bx + i, by + j, r, g, b);
                                block[0] += r / (kBlock * kBlock);
                                block[1] += g / (kBlock * kBlock);
                                block[2] += b / (kBlock * kBlock);
                            }
                        for (int c = 0; c < 3; ++c) {
                            sum[c] += block[c];
                            low[c] = std::min(low[c], block[c]);
                        }
                        ++count;
                    }
                const size_t i = size_t(cy) * glare.width + cx;
                for (int c = 0; c < 3; ++c) {
                    mean[c][i] = count ? float(sum[c] / count) : 0.0f;
                    floor[c][i] = count ? low[c] : 0.0f;
                }
            }
    });

    // Sources: bright cells, fading in from kGlareSourceFrom to
    // kGlareSourceTo, and clipped ones counted as brighter than they read.
    std::vector<float> source[3];
    for (int c = 0; c < 3; ++c)
        source[c].assign(cells, 0.0f);
    for (size_t i = 0; i < cells; ++i) {
        const float peak = std::max({ mean[0][i], mean[1][i], mean[2][i] });
        float weight = std::clamp((peak - kGlareSourceFrom) / (kGlareSourceTo - kGlareSourceFrom),
                                  0.0f, 1.0f);
        if (peak >= kGlareClip)
            weight *= kGlareClipBoost;
        for (int c = 0; c < 3; ++c)
            source[c][i] = weight * mean[c][i];
    }

    // Each width in turn, narrowest first, takes as much of the glare as the
    // darkest content still allows: the falloff follows the lens.
    for (int c = 0; c < 3; ++c)
        glare.planes[c].assign(cells, 0.0f);
    for (size_t w = 0; w < std::size(kGlareWidths); ++w) {
        std::vector<float> spread[3];
        std::vector<float> ratios;
        for (int c = 0; c < 3; ++c) {
            spread[c] = gaussian(source[c], glare.width, glare.height, kGlareWidths[w]);
            for (size_t i = 0; i < cells; ++i)
                if (spread[c][i] > 1e-5f)
                    ratios.push_back(std::max(0.0f, floor[c][i] - glare.planes[c][i]) / spread[c][i]);
        }
        if (ratios.empty())
            continue;
        const size_t pick = size_t(double(ratios.size()) * kGlareFloorShare);
        std::nth_element(ratios.begin(), ratios.begin() + pick, ratios.end());
        glare.shares[w] = kGlareSafety * ratios[pick];
        for (int c = 0; c < 3; ++c)
            for (size_t i = 0; i < cells; ++i)
                glare.planes[c][i] += glare.shares[w] * spread[c][i];
    }
    return glare;
}

// Values the tone curve pushed past white come back under it smoothly,
// through the brightest channel so the hue holds.
void softClip(float &r, float &g, float &b)
{
    const float peak = std::max({ r, g, b });
    if (peak <= kClipKnee)
        return;
    const float room = 1.0f - kClipKnee;
    const float k = (kClipKnee + room * (1.0f - std::exp(-(peak - kClipKnee) / room))) / peak;
    r *= k;
    g *= k;
    b *= k;
}

QImage render(const std::vector<float> &raw, const RawLayout &layout, const RawFrame &settings,
              int frames)
{
    const int width = layout.size.width();
    const int height = layout.size.height();
    const float black = float(settings.blackLevel16 >> (16 - layout.bits));
    const float white = float((1u << layout.bits) - 1);
    const float scale = 1.0f / (white - black);

    Matrix3 ccm{ settings.ccm };
    const Matrix3 colour = saturationMatrix(settings.saturation) * ccm;
    const Balance wb = balance(raw, layout, black, white, settings.colourGains);
    // Gains of at least 1 on every channel, so a highlight clipped in all
    // three stays white: with red below 1 under warm light, a clipped lamp
    // would turn cyan. The exposure takes the common factor back.
    const float least = std::min({ wb.gains[0], 1.0f, wb.gains[1] });
    const float gainR = wb.gains[0] / least;
    const float gainG = 1.0f / least;
    const float gainB = wb.gains[1] / least;
    // Warmth goes back in after colour correction, where warm means the same
    // on every sensor.
    const float warmR = 1.0f + kWarmRed * wb.warmth;
    const float warmB = 1.0f - kWarmBlue * wb.warmth;
    // Sensor values to linear RGB: black level, white balance saturated at
    // the sensor range, then colour correction and saturation, as the
    // software ISP does. The tone curve then sets the exposure.
    const auto toLinear = [&](float &r, float &g, float &b) {
        r = std::clamp((r - black) * scale * gainR, 0.0f, 1.0f);
        g = std::clamp((g - black) * scale * gainG, 0.0f, 1.0f);
        b = std::clamp((b - black) * scale * gainB, 0.0f, 1.0f);
        const float cr = warmR * (colour.m[0] * r + colour.m[1] * g + colour.m[2] * b);
        const float cg = colour.m[3] * r + colour.m[4] * g + colour.m[5] * b;
        const float cb = warmB * (colour.m[6] * r + colour.m[7] * g + colour.m[8] * b);
        r = std::max(cr, 0.0f);
        g = std::max(cg, 0.0f);
        b = std::max(cb, 0.0f);
    };
    const int rx = layout.redX, ry = layout.redY;
    const auto quadColour = [&](int x, int y, float &r, float &g, float &b) {
        const auto at = [&](int dx, int dy) { return raw[size_t(2 * y + dy) * width + 2 * x + dx]; };
        r = at(rx, ry);
        g = (at(1 - rx, ry) + at(rx, 1 - ry)) / 2;
        b = at(1 - rx, 1 - ry);
        toLinear(r, g, b);
    };

    QElapsedTimer timer;
    timer.start();
    // Glare off, then the black point: x and y in quads.
    const Glare glare = glareFor(layout, quadColour);
    const auto unveil = [&](float &r, float &g, float &b, float x, float y) {
        float haze[3];
        glare.at(x, y, haze);
        r -= std::min(haze[0], r * kGlareMost);
        g -= std::min(haze[1], g * kGlareMost);
        b -= std::min(haze[2], b * kGlareMost);
    };
    const QuadColour unveiled = [&](int x, int y, float &r, float &g, float &b) {
        quadColour(x, y, r, g, b);
        unveil(r, g, b, float(x), float(y));
    };
    const float blackPoint = ::blackPoint(layout, unveiled);
    const auto clear = [&](float &r, float &g, float &b, float x, float y) {
        unveil(r, g, b, x, y);
        r = std::max(0.0f, r - blackPoint);
        g = std::max(0.0f, g - blackPoint);
        b = std::max(0.0f, b - blackPoint);
    };
    const QuadColour colourAt = [&](int x, int y, float &r, float &g, float &b) {
        quadColour(x, y, r, g, b);
        clear(r, g, b, float(x), float(y));
    };
    Tone curve;
    const std::vector<float> gains =
        localGain(layout, colourAt, std::max(1.0f, float(settings.digitalGain)) * least,
                  float(settings.exposureUs) * 1e-6f * float(settings.analogueGain) / least,
                  frames, &curve);
    const int quadWidth = width / 2;
    qInfo().noquote() << QStringLiteral("White balance %1, %2 (image processor %3, %4; tint %14); warmth %5; "
                                        "glare %6; black %7; light %8, key %9; exposure %10 "
                                        "(digital gain %11); slope %12; tone map %13 ms")
                             .arg(gainR, 0, 'f', 3).arg(gainB, 0, 'f', 3)
                             .arg(settings.colourGains[0], 0, 'f', 3)
                             .arg(settings.colourGains[1], 0, 'f', 3)
                             .arg(wb.warmth, 0, 'f', 2)
                             .arg(QStringLiteral("%1/%2/%3").arg(glare.shares[0], 0, 'f', 3)
                                      .arg(glare.shares[1], 0, 'f', 3).arg(glare.shares[2], 0, 'f', 3))
                             .arg(blackPoint, 0, 'f', 4)
                             .arg(curve.light, 0, 'f', 3)
                             .arg(curve.key, 0, 'f', 3)
                             .arg(curve.exposure, 0, 'f', 2)
                             .arg(settings.digitalGain, 0, 'f', 2)
                             .arg(curve.slope, 0, 'f', 2)
                             .arg(timer.elapsed())
                             .arg(wb.tint, 0, 'f', 3);

    // Gamma, then contrast about the middle of the encoded range, so contrast
    // shapes the picture as it is seen. (The software ISP applies contrast to
    // linear values, which darkens the middle tones.) Fine steps, as the
    // shadows are stretched.
    constexpr int kLut = 16384;
    std::vector<uint8_t> tone(kLut);
    const double contrastExp =
        std::tan(std::clamp(double(settings.contrast) * M_PI_4, 0.0, M_PI_2 - 0.00001));
    for (int i = 0; i < kLut; ++i) {
        double v = double(i) / (kLut - 1);
        v = std::pow(v, 1.0 / settings.gamma);
        v = v < 0.5 ? 0.5 * std::pow(v / 0.5, contrastExp)
                    : 1.0 - 0.5 * std::pow((1.0 - v) / 0.5, contrastExp);
        tone[i] = uint8_t(std::lround(255.0 * v));
    }

    QImage image(width, height, QImage::Format_RGB888);
    parallelRows(height, [&](int first, int last) {
        auto v = [&](int x, int y) {
            x = std::clamp(x, 0, width - 1);
            y = std::clamp(y, 0, height - 1);
            return raw[size_t(y) * width + x];
        };
        for (int y = first; y < last; ++y) {
            uchar *out = image.scanLine(y);
            const bool redRow = ((y - layout.redY) & 1) == 0;
            for (int x = 0; x < width; ++x) {
                const bool redColumn = ((x - layout.redX) & 1) == 0;
                const float c = v(x, y);
                const float cross = v(x - 1, y) + v(x + 1, y) + v(x, y - 1) + v(x, y + 1);
                const float diagonal = v(x - 1, y - 1) + v(x + 1, y - 1) + v(x - 1, y + 1) +
                                       v(x + 1, y + 1);
                const float far = v(x - 2, y) + v(x + 2, y) + v(x, y - 2) + v(x, y + 2);
                const float farH = v(x - 2, y) + v(x + 2, y);
                const float farV = v(x, y - 2) + v(x, y + 2);
                const float nearH = v(x - 1, y) + v(x + 1, y);
                const float nearV = v(x, y - 1) + v(x, y + 1);

                // Malvar-He-Cutler 5x5 demosaic.
                float r, g, b;
                if (redRow == redColumn) {
                    // A red or blue site.
                    const float green = (4 * c + 2 * cross - far) / 8;
                    const float other = (6 * c + 2 * diagonal - 1.5f * far) / 8;
                    g = green;
                    if (redRow) {
                        r = c;
                        b = other;
                    } else {
                        b = c;
                        r = other;
                    }
                } else {
                    // A green site: one colour left and right, the other above
                    // and below.
                    const float along = (5 * c + 4 * nearH - farH - diagonal + 0.5f * farV) / 8;
                    const float across = (5 * c + 4 * nearV - farV - diagonal + 0.5f * farH) / 8;
                    g = c;
                    if (redRow) {
                        r = along;
                        b = across;
                    } else {
                        b = along;
                        r = across;
                    }
                }

                toLinear(r, g, b);
                clear(r, g, b, x * 0.5f, y * 0.5f);
                const float gain = gains[size_t(std::min(y / 2, height / 2 - 1)) * quadWidth +
                                         std::min(x / 2, quadWidth - 1)];
                r *= gain;
                g *= gain;
                b *= gain;
                softClip(r, g, b);
                out[3 * x] = tone[int(std::clamp(r, 0.0f, 1.0f) * (kLut - 1))];
                out[3 * x + 1] = tone[int(std::clamp(g, 0.0f, 1.0f) * (kLut - 1))];
                out[3 * x + 2] = tone[int(std::clamp(b, 0.0f, 1.0f) * (kLut - 1))];
            }
        }
    });
    return image;
}

// How sharp a frame is: the mean difference between neighbouring pixels of
// the same colour on every eighth row of the middle 60%, from the top eight
// bits. Shake and blur lower it; the frames of a burst share their exposure,
// so noise weighs the same in each.
double sharpness(const RawFrame &frame, const RawLayout &layout)
{
    const int width = layout.size.width();
    const int height = layout.size.height();
    const auto top = [&](const uint8_t *row, int x) -> int {
        if (layout.csi2Packed && layout.bits == 10)
            return row[x / 4 * 5 + x % 4];
        if (layout.csi2Packed && layout.bits == 12)
            return row[x / 2 * 3 + x % 2];
        if (layout.bits == 8)
            return row[x];
        return reinterpret_cast<const uint16_t *>(row)[x] >> (layout.bits - 8);
    };
    double sum = 0;
    size_t count = 0;
    for (int y = height / 5; y < height - height / 5; y += 8) {
        const uint8_t *row = frame.data.data() + size_t(y) * layout.stride;
        for (int x = width / 5; x + 2 < width - width / 5; ++x) {
            sum += std::abs(top(row, x + 2) - top(row, x));
            ++count;
        }
    }
    return count ? sum / double(count) : 0.0;
}

} // namespace

bool RawLayout::fromName(const QString &name, QSize size, unsigned int stride, RawLayout *layout)
{
    static const QRegularExpression pattern(QStringLiteral("^S([RGB]{4})(\\d+)(_CSI2P)?$"));
    const QRegularExpressionMatch match = pattern.match(name);
    if (!match.hasMatch())
        return false;
    const QString order = match.captured(1);
    const int red = order.indexOf(QLatin1Char('R'));
    const unsigned int bits = match.captured(2).toUInt();
    const bool packed = match.hasCaptured(3);
    if (red < 0 || !(bits == 8 || bits == 10 || bits == 12 || bits == 16) ||
        (packed && bits != 10 && bits != 12))
        return false;

    layout->size = size;
    layout->stride = stride;
    layout->bits = bits;
    layout->csi2Packed = packed;
    layout->redX = red % 2;
    layout->redY = red / 2;
    return true;
}

namespace RawMerge {

int framesForGain(double totalGain)
{
    if (totalGain <= 1.5)
        return 1;
    if (totalGain <= 3.0)
        return 2;
    if (totalGain <= 6.0)
        return 4;
    if (totalGain <= 12.0)
        return 6;
    return 8;
}

QImage process(const std::vector<RawFrame> &frames, const RawLayout &layout, MergeReport *report)
{
    QElapsedTimer timer;
    timer.start();
    if (frames.empty())
        return {};

    const int width = layout.size.width();
    const int height = layout.size.height();

    // The sharpest frame is the reference, as the tap on the shutter shakes
    // the first; the others are aligned to it and merged where they match.
    std::vector<size_t> order(frames.size());
    std::iota(order.begin(), order.end(), 0);
    if (frames.size() > 1) {
        std::vector<double> scores(frames.size());
        QtConcurrent::blockingMap(order, [&](size_t i) { scores[i] = sharpness(frames[i], layout); });
        const size_t best = size_t(std::max_element(scores.begin(), scores.end()) - scores.begin());
        std::swap(order[0], order[best]);
        report->sharpness = scores;
    }
    report->reference = int(order[0]);
    const RawFrame &first = frames[order[0]];

    const Plane reference = unpack(first, layout);
    std::vector<float> sum(reference.begin(), reference.end());
    report->frames = 1;
    report->shifts = { { 0, 0 } };
    report->rejected = { 0.0 };

    if (frames.size() > 1) {
        const Quads refQuads = quads(reference, layout.size);
        const Quads refCoarse = shrink(refQuads, kCoarseFactor);
        const int tilesX = (refQuads.width + kTile - 1) / kTile;
        const int tilesY = (refQuads.height + kTile - 1) / kTile;
        const float black = 4.0f * float(first.blackLevel16 >> (16 - layout.bits));
        const float spatial = spatialNoise(refQuads, black, tilesX, tilesY);
        std::vector<float> weightSum(size_t(refQuads.width) * refQuads.height, 1.0f);

        for (size_t f = 1; f < frames.size(); ++f) {
            const Plane raw = unpack(frames[order[f]], layout);
            const Quads q = quads(raw, layout.size);
            const std::array<int, 2> shift = align(refQuads, refCoarse, q, shrink(q, kCoarseFactor));
            double rejected = 0;
            const std::vector<float> tiles =
                tileWeights(refQuads, q, shift, black, spatial, tilesX, tilesY, &rejected);
            const std::vector<float> weights =
                quadWeights(tiles, tilesX, tilesY, refQuads.width, refQuads.height);

            // Whole quads move, so the Bayer pattern stays in step.
            const int dx = 2 * shift[0], dy = 2 * shift[1];
            parallelRows(height, [&](int first, int last) {
                for (int y = first; y < last; ++y) {
                    const int sy = y + dy;
                    if (sy < 0 || sy >= height)
                        continue;
                    const float *w = weights.data() + size_t(y / 2) * refQuads.width;
                    float *dst = sum.data() + size_t(y) * width;
                    const uint16_t *src = raw.data() + size_t(sy) * width;
                    for (int x = 0; x < width; ++x) {
                        const int sx = x + dx;
                        if (sx < 0 || sx >= width)
                            continue;
                        dst[x] += w[x / 2] * src[sx];
                    }
                }
            });
            for (int y = 0; y < refQuads.height; ++y) {
                const int sy = 2 * y + dy;
                if (sy < 0 || sy + 1 >= height)
                    continue;
                for (int x = 0; x < refQuads.width; ++x) {
                    const int sx = 2 * x + dx;
                    if (sx >= 0 && sx + 1 < width)
                        weightSum[size_t(y) * refQuads.width + x] +=
                            weights[size_t(y) * refQuads.width + x];
                }
            }

            ++report->frames;
            report->shifts.push_back({ dx, dy });
            report->rejected.push_back(rejected);
        }

        parallelRows(height, [&](int first, int last) {
            for (int y = first; y < last; ++y) {
                const float *w = weightSum.data() + size_t(y / 2) * refQuads.width;
                float *row = sum.data() + size_t(y) * width;
                for (int x = 0; x < width; ++x)
                    row[x] /= w[x / 2];
            }
        });
    }

    QImage image = render(sum, layout, first, report->frames);
    report->milliseconds = timer.elapsed();
    return image;
}

} // namespace RawMerge
