#include "stillwriter.h"
#include "camerasession.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QTransform>

#include <cmath>
#include <numeric>

#ifdef HAVE_EXIV2
#include <exiv2/exiv2.hpp>
#endif

namespace {

constexpr int kJpegQuality = 92;

QString readFirstLine(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    // Device tree strings end in NUL.
    return QString::fromUtf8(file.readAll()).section(QChar(0), 0, 0).trimmed();
}

// The phone's name, from the device tree on ARM boards or DMI elsewhere.
QString deviceModel()
{
    QString model = readFirstLine(QStringLiteral("/sys/firmware/devicetree/base/model"));
    if (model.isEmpty())
        model = readFirstLine(QStringLiteral("/sys/class/dmi/id/product_name"));
    return model;
}

// Opens a new file for the photo, named after the time, as <name>.part: it
// becomes the photo only once complete, so nothing reads a partial file.
QString reservePath(const QString &folder, const QDateTime &when, QFile *part)
{
    const QString stem = folder + QStringLiteral("/IMG_") +
                         when.toString(QStringLiteral("yyyyMMdd_HHmmss"));
    for (int n = 0; n < 100; ++n) {
        const QString path = n ? stem + QStringLiteral("_%1.jpg").arg(n) : stem + QStringLiteral(".jpg");
        if (QFileInfo::exists(path))
            continue;
        part->setFileName(path + QStringLiteral(".part"));
        if (part->open(QIODevice::WriteOnly | QIODevice::NewOnly))
            return path;
    }
    return {};
}

#ifdef HAVE_EXIV2
void writeExif(const QString &path, const StillInfo &info, const QDateTime &when)
{
    try {
        auto image = Exiv2::ImageFactory::open(path.toStdString());
        image->readMetadata();
        Exiv2::ExifData &exif = image->exifData();

        const QString model = deviceModel();
        if (!model.isEmpty()) {
            exif["Exif.Image.Make"] = model.section(QLatin1Char(' '), 0, 0).toStdString();
            exif["Exif.Image.Model"] = model.toStdString();
        }
        const std::string stamp =
            when.toString(QStringLiteral("yyyy:MM:dd HH:mm:ss")).toStdString();
        exif["Exif.Image.DateTime"] = stamp;
        exif["Exif.Photo.DateTimeOriginal"] = stamp;
        exif["Exif.Image.Software"] = std::string("Omarchy Camera");
        // The pixels are already upright.
        exif["Exif.Image.Orientation"] = static_cast<uint16_t>(1);
        if (info.exposureUs > 0) {
            const uint32_t divisor = std::gcd(static_cast<uint32_t>(info.exposureUs), 1000000u);
            exif["Exif.Photo.ExposureTime"] =
                Exiv2::URational(static_cast<uint32_t>(info.exposureUs) / divisor,
                                 1000000u / divisor);
        }
        if (info.analogueGain > 0.0)
            exif["Exif.Photo.ISOSpeedRatings"] =
                static_cast<uint16_t>(std::lround(info.analogueGain * 100.0));
        image->writeMetadata();
    } catch (const Exiv2::Error &) {
        // The photo is saved; missing metadata is not worth failing it.
    }
}
#endif

} // namespace

namespace StillWriter {

QString write(const QImage &frame, const StillInfo &info, const QString &folder, QString *error)
{
    if (!QDir().mkpath(folder)) {
        *error = QStringLiteral("Could not create %1").arg(folder);
        return {};
    }

    // Digital zoom keeps the centre of the frame, at the sensor's own pixels.
    QImage upright = frame;
    if (info.zoom > 1.001) {
        const int width = qRound(frame.width() / info.zoom) & ~1;
        const int height = qRound(frame.height() / info.zoom) & ~1;
        upright = frame.copy((frame.width() - width) / 2, (frame.height() - height) / 2, width, height);
    }
    const int rotation = ((info.rotation % 360) + 360) % 360;
    if (rotation)
        upright = upright.transformed(QTransform().rotate(rotation));
    upright = upright.convertToFormat(QImage::Format_RGB888);

    const QDateTime when = QDateTime::currentDateTime();
    QFile part;
    const QString path = reservePath(folder, when, &part);
    if (path.isEmpty()) {
        *error = QStringLiteral("Could not create a file in %1").arg(folder);
        return {};
    }
    QImageWriter writer(&part, "jpeg");
    writer.setQuality(kJpegQuality);
    const bool written = writer.write(upright);
    part.close();
    if (!written) {
        *error = writer.errorString();
        part.remove();
        return {};
    }

#ifdef HAVE_EXIV2
    writeExif(part.fileName(), info, when);
#endif
    if (!part.rename(path)) {
        *error = part.errorString();
        part.remove();
        return {};
    }
    return path;
}

} // namespace StillWriter
