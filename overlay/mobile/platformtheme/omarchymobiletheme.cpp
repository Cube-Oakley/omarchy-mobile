// Omarchy Mobile's Qt platform theme: Qt's generic Unix theme (what Qt uses
// under Hyprland anyway) with phone-sized flick physics.
//
// Qt Quick's Flickable (lists, grids, every scrolling page) takes its limits
// from the platform theme. Qt's defaults are desktop ones: a flick tops out at
// 2500 px/s and slows at 1500 px/s², so a quick swipe through a long list
// crawls. Android allows 8000 dp/s (about 9300 of the phone's logical pixels)
// and stops a fling about as far as a constant 3500 px/s² would (once the
// fling starts at the finger's speed, below).
//
// The speed a flick starts at is the finger's, as Qt measures it, and Qt
// 6.11 measures touch at 70 % of it here: its filter (qeventpoint.cpp, new
// velocity * 0.7 + previous * 0.3) finds no previous value for Wayland touch
// points, so every reading is 0.7 of the latest. The theme also installs an
// event filter that measures each touch point as Android's default velocity
// tracker does (LSQ2): the slope, at the newest position, of the
// least-squares parabola through the last 100 ms. A line through the same
// positions gives their average speed, which for a flick (the finger still
// speeding up as it lifts) is well below the speed at lift-off. Flickable
// also averages the velocities of the last three moves unless the device
// measures velocity itself; the filter marks touch devices that way, so a
// fling starts at the speed the finger lifted at.
//
// QT_QPA_PLATFORMTHEME=omarchy-mobile selects it. OMARCHY_FLICK_MAX_VELOCITY
// and OMARCHY_FLICK_DECELERATION override the two values for tuning, and
// OMARCHY_TOUCH_VELOCITY=0 leaves Qt's velocities alone.

#include <QtCore/QCoreApplication>
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtGui/QEventPoint>
#include <QtGui/QPointingDevice>
#include <QtGui/QTouchEvent>
#include <QtGui/QWindow>
#include <QtGui/private/qeventpoint_p.h>
#include <QtGui/private/qinputdevice_p.h>
#include <QtGui/private/qgenericunixtheme_p.h>
#include <qpa/qplatformthemeplugin.h>

namespace {

qreal envReal(const char *name, qreal fallback)
{
    bool ok = false;
    const qreal value = qEnvironmentVariable(name).toDouble(&ok);
    return ok && value > 0 ? value : fallback;
}

// Each touch point's recent positions (global, logical pixels) and times.
class TouchVelocity : public QObject
{
public:
    using QObject::QObject;

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd:
            update(static_cast<QTouchEvent *>(event));
            break;
        case QEvent::TouchCancel:
            samples.clear();
            break;
        default:
            break;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    struct Sample { qreal time; QPointF pos; };
    // Android's VelocityTracker: 100 ms of history, at most 20 positions,
    // and a finger that rested 40 ms before lifting has stopped.
    static constexpr qreal Window = 100;
    static constexpr int MaxSamples = 20;
    static constexpr qreal Stopped = 40;
    // A point's events reach the filter again for each item Qt Quick
    // delivers them to, and every copy must get the same velocity: the key
    // keeps one history per device and point, kept after the release until
    // the id is pressed again.
    QHash<quint64, QList<Sample>> samples;
    // Devices this filter marked as measuring velocity (see above).
    QSet<const QPointingDevice *> measured;

    void update(QTouchEvent *event)
    {
        const QPointingDevice *device = event->pointingDevice();
        if (device && !measured.contains(device)) {
            if (device->capabilities().testFlag(QInputDevice::Capability::Velocity))
                return;     // the hardware measures it
            QInputDevicePrivate::get(const_cast<QPointingDevice *>(device))->setCapabilities(
                    device->capabilities() | QInputDevice::Capability::Velocity);
            measured.insert(device);
        }
        for (qsizetype i = 0; i < event->pointCount(); ++i) {
            QEventPoint &point = event->point(i);
            const quint64 key = (quint64(quintptr(device)) << 16) ^ quint64(point.id());
            if (point.state() == QEventPoint::State::Pressed) {
                if (samples.size() > 32)    // released histories of other devices and ids
                    samples.clear();
                samples[key] = { { qreal(point.timestamp()), point.globalPosition() } };
                continue;
            }
            QList<Sample> &list = samples[key];
            const qreal now = qreal(point.timestamp());
            const bool released = point.state() == QEventPoint::State::Released;
            if (released) {
                // The release repeats the last position.
                if (!list.isEmpty() && now - list.last().time > Stopped)
                    list.clear();
            } else if (list.isEmpty() || list.last().time != now || list.last().pos != point.globalPosition()) {
                if (!list.isEmpty() && list.last().time == now)
                    list.last().pos = point.globalPosition();
                else
                    list.append({ now, point.globalPosition() });
            }
            while (list.size() > MaxSamples || (list.size() > 2 && list.last().time - list.first().time > Window))
                list.removeFirst();
            QMutableEventPoint::detach(point);
            QMutableEventPoint::setVelocity(point, fit(list));
        }
    }

    // Android's LSQ2 (solveUnweightedLeastSquaresDeg2 in VelocityTracker.cpp):
    // position = a + b t + c t² by least squares, with t in seconds before
    // the newest position; b is the velocity there, in pixels per second.
    // Two positions give the line through them.
    static QVector2D fit(const QList<Sample> &list)
    {
        const qsizetype count = list.size();
        if (count < 2)
            return {};
        const qreal n = count, newest = list.last().time;
        qreal st = 0, st2 = 0, st3 = 0, st4 = 0, sx = 0, sy = 0, stx = 0, sty = 0, st2x = 0, st2y = 0;
        for (const Sample &s : list) {
            const qreal t = (s.time - newest) / 1000, t2 = t * t;
            st += t; st2 += t2; st3 += t2 * t; st4 += t2 * t2;
            sx += s.pos.x(); sy += s.pos.y();
            stx += t * s.pos.x(); sty += t * s.pos.y();
            st2x += t2 * s.pos.x(); st2y += t2 * s.pos.y();
        }
        const qreal Stt = st2 - st * st / n;
        if (Stt <= 0)
            return {};
        const qreal Stx = stx - st * sx / n, Sty = sty - st * sy / n;
        if (count >= 3) {
            const qreal Stt2 = st3 - st * st2 / n, St2t2 = st4 - st2 * st2 / n;
            const qreal St2x = st2x - st2 * sx / n, St2y = st2y - st2 * sy / n;
            const qreal d = Stt * St2t2 - Stt2 * Stt2;
            if (d > 0)
                return QVector2D(float((Stx * St2t2 - St2x * Stt2) / d),
                                 float((Sty * St2t2 - St2y * Stt2) / d));
        }
        return QVector2D(float(Stx / Stt), float(Sty / Stt));
    }
};

class MobileTheme : public QGenericUnixTheme
{
public:
    QVariant themeHint(ThemeHint hint) const override
    {
        switch (hint) {
        case FlickMaximumVelocity:
            return envReal("OMARCHY_FLICK_MAX_VELOCITY", 9000);
        case FlickDeceleration:
            return envReal("OMARCHY_FLICK_DECELERATION", 3500);
        default:
            return QGenericUnixTheme::themeHint(hint);
        }
    }
};

} // namespace

class MobileThemePlugin : public QPlatformThemePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QPlatformThemeFactoryInterface_iid FILE "omarchymobiletheme.json")

public:
    QPlatformTheme *create(const QString &key, const QStringList &) override
    {
        if (key.compare(QLatin1String("omarchy-mobile"), Qt::CaseInsensitive) != 0)
            return nullptr;
        // The application is still being made: the filter goes on once its
        // event loop runs.
        if (qEnvironmentVariable("OMARCHY_TOUCH_VELOCITY") != QLatin1String("0")) {
            if (QCoreApplication::instance())
                QTimer::singleShot(0, QCoreApplication::instance(), [] {
                    QCoreApplication::instance()->installEventFilter(new TouchVelocity(QCoreApplication::instance()));
                });
        }
        return new MobileTheme;
    }
};

#include "omarchymobiletheme.moc"
