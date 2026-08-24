#include "FrameTimingDialog.h"

#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QGuiApplication>
#include <QScreen>
#include <QVBoxLayout>
#include <QFutureWatcher>
#include <QPointer>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>

namespace {

struct Sample { double seconds; double distanceMs; };

constexpr std::size_t MaxAnalyzedFrames = 20000000;
constexpr std::size_t MaxRenderedSamples = 200000;

class SampleEnvelope
{
public:
    explicit SampleEnvelope(std::size_t expected)
        : mBucketSize(std::max<std::size_t>(
              1, (expected + MaxRenderedSamples / 2 - 1) / (MaxRenderedSamples / 2)))
    {
        mSamples.reserve(std::min(expected, MaxRenderedSamples));
    }

    void add(const Sample& sample)
    {
        if (mCountInBucket == 0) {
            mMin = mMax = sample;
            mMinIndex = mMaxIndex = mTotalCount;
        } else {
            if (sample.distanceMs < mMin.distanceMs) {
                mMin = sample; mMinIndex = mTotalCount;
            }
            if (sample.distanceMs > mMax.distanceMs) {
                mMax = sample; mMaxIndex = mTotalCount;
            }
        }
        ++mCountInBucket;
        ++mTotalCount;
        if (mCountInBucket == mBucketSize) flush();
    }

    std::vector<Sample> finish()
    {
        flush();
        return std::move(mSamples);
    }

private:
    void flush()
    {
        if (mCountInBucket == 0) return;
        if (mMinIndex <= mMaxIndex) {
            mSamples.push_back(mMin);
            if (mMaxIndex != mMinIndex) mSamples.push_back(mMax);
        } else {
            mSamples.push_back(mMax);
            mSamples.push_back(mMin);
        }
        mCountInBucket = 0;
    }

    std::size_t mBucketSize;
    std::size_t mCountInBucket = 0;
    std::size_t mTotalCount = 0;
    std::size_t mMinIndex = 0;
    std::size_t mMaxIndex = 0;
    Sample mMin{};
    Sample mMax{};
    std::vector<Sample> mSamples;
};

std::vector<Sample> distanceToIdeal(const std::vector<std::int64_t>& inputPts,
                                    double ticksPerSecond, double fps,
                                    bool useCfrMapping)
{
    std::vector<Sample> samples;
    if (inputPts.size() < 2 || ticksPerSecond <= 0.0 || fps <= 0.0) return samples;

    std::vector<std::int64_t> pts = inputPts;
    if (useCfrMapping) {
        std::sort(pts.begin(), pts.end());
        pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    }
    const std::int64_t origin = pts.front();
    for (auto& value : pts) value -= origin;

    const double step = ticksPerSecond / fps;
    if (useCfrMapping) {
        const double finalOutputFrame = std::round(static_cast<double>(pts.back()) / step);
        const std::size_t expected = finalOutputFrame >= static_cast<double>(MaxAnalyzedFrames - 1)
            ? MaxAnalyzedFrames : static_cast<std::size_t>(finalOutputFrame) + 1;
        SampleEnvelope envelope(expected);
        std::size_t nextOutput = 0;
        std::size_t previousSource = 0;
        for (std::size_t source = 0;
             source < pts.size() && nextOutput < MaxAnalyzedFrames; ++source) {
            const auto outputFrame = static_cast<std::int64_t>(
                std::round(static_cast<double>(pts[source]) / step));
            if (outputFrame < static_cast<std::int64_t>(nextOutput)) continue;
            while (nextOutput < static_cast<std::size_t>(outputFrame) &&
                   nextOutput < MaxAnalyzedFrames) {
                const double ideal = static_cast<double>(nextOutput) * step;
                envelope.add({ideal / ticksPerSecond,
                              (ideal - static_cast<double>(pts[previousSource])) /
                                  ticksPerSecond * 1000.0});
                ++nextOutput;
            }
            if (nextOutput >= MaxAnalyzedFrames) break;
            const double ideal = static_cast<double>(nextOutput) * step;
            envelope.add({ideal / ticksPerSecond,
                          (ideal - static_cast<double>(pts[source])) /
                              ticksPerSecond * 1000.0});
            ++nextOutput;
            previousSource = source;
        }
        samples = envelope.finish();
    } else {
        // Passthrough keeps every source frame. Playback advances one target
        // frame per source frame, so compare corresponding frame indices
        // instead of selecting the nearest PTS (which implies drop/duplicate).
        const std::size_t count = std::min(pts.size(), MaxAnalyzedFrames);
        SampleEnvelope envelope(count);
        for (std::size_t i = 0; i < count; ++i) {
            const double playback = static_cast<double>(i) * step;
            envelope.add({playback / ticksPerSecond,
                          (playback - static_cast<double>(pts[i])) /
                              ticksPerSecond * 1000.0});
        }
        samples = envelope.finish();
    }
    return samples;
}

class TimingGraph final : public QWidget
{
public:
    explicit TimingGraph(std::vector<Sample> samples, QWidget* parent = nullptr)
        : QWidget(parent), mSamples(std::move(samples)) { setMinimumSize(720, 380); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), QColor("#20242b"));
        const QRectF plot = rect().adjusted(72, 24, -24, -54);
        if (mSamples.empty()) {
            p.setPen(Qt::lightGray);
            p.drawText(rect(), Qt::AlignCenter, tr("No frame timing data available"));
            return;
        }

        double low = -10.0, high = 10.0;
        for (const auto& sample : mSamples) {
            low = std::min(low, sample.distanceMs);
            high = std::max(high, sample.distanceMs);
        }
        const double pad = std::max(1.0, (high - low) * 0.08);
        low -= pad; high += pad;
        const double duration = std::max(0.001, mSamples.back().seconds);
        auto y = [&](double value) { return plot.bottom() - (value - low) / (high - low) * plot.height(); };

        p.setPen(QPen(QColor("#424a55"), 1));
        for (int i = 0; i <= 4; ++i) {
            const double value = low + (high - low) * i / 4.0;
            const double py = y(value);
            p.drawLine(QPointF(plot.left(), py), QPointF(plot.right(), py));
            p.setPen(QColor("#aeb7c2"));
            p.drawText(QRectF(4, py - 10, 62, 20), Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(value, 'f', 1));
            p.setPen(QPen(QColor("#424a55"), 1));
        }
        p.setPen(QPen(QColor("#8994a3"), 1));
        p.drawRect(plot);
        if (low <= 0.0 && high >= 0.0) {
            p.setPen(QPen(QColor("#d7dce2"), 1, Qt::DashLine));
            p.drawLine(QPointF(plot.left(), y(0.0)), QPointF(plot.right(), y(0.0)));
        }

        QPainterPath path;
        for (std::size_t i = 0; i < mSamples.size(); ++i) {
            const double px = plot.left() + mSamples[i].seconds / duration * plot.width();
            const QPointF point(px, y(mSamples[i].distanceMs));
            i == 0 ? path.moveTo(point) : path.lineTo(point);
        }
        p.setClipRect(plot.adjusted(-1, -1, 1, 1));
        p.setPen(QPen(QColor("#5d8cff"), 1.25));
        p.drawPath(path);
        p.setClipping(false);
        p.setPen(QColor("#aeb7c2"));
        p.drawText(QRectF(plot.left(), plot.bottom() + 10, plot.width(), 22),
                   Qt::AlignCenter, tr("Clip time (seconds)"));
        p.save();
        p.translate(18, plot.center().y()); p.rotate(-90);
        p.drawText(QRectF(-plot.height() / 2, -12, plot.height(), 24), Qt::AlignCenter,
                   tr("Distance to ideal (ms)"));
        p.restore();
    }

private:
    std::vector<Sample> mSamples;
};

} // namespace

FrameTimingDialog::FrameTimingDialog(const QString& clipName,
                                     const std::vector<std::int64_t>& pts,
                                     int timeBaseNum, int timeBaseDen,
                                     double targetFps, bool useCfrMapping,
                                     QWidget* parent)
    : QDialog(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("Frame timing — %1").arg(clipName));
    QScreen* screen = parent && parent->screen() ? parent->screen() : QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 1800, 900);
    resize(available.width() / 2, std::min(620, available.height() * 2 / 3));
    const double ticksPerSecond = timeBaseNum > 0
        ? static_cast<double>(timeBaseDen) / timeBaseNum : 0.0;
    auto* layout = new QVBoxLayout(this);
    mSummary = new QLabel(this);
    mSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(mSummary);
    updateTiming(pts, timeBaseNum, timeBaseDen, targetFps, useCfrMapping);
}

void FrameTimingDialog::updateTiming(const std::vector<std::int64_t>& pts,
                                     int timeBaseNum, int timeBaseDen,
                                     double targetFps, bool useCfrMapping)
{
    const double ticksPerSecond = timeBaseNum > 0
        ? static_cast<double>(timeBaseDen) / timeBaseNum : 0.0;
    mSummary->setText(
        tr("Target: %1 fps   •   Time base: %2/%3 s (tbn %4)   •   Frames: %5   •   %6")
            .arg(targetFps, 0, 'g', 8).arg(timeBaseNum).arg(timeBaseDen)
            .arg(ticksPerSecond, 0, 'g', 10).arg(pts.size())
            .arg(useCfrMapping ? tr("CFR nearest-frame mapping")
                               : tr("VFR frame-by-frame playback")));

    const auto generation = ++mAnalysisGeneration;
    auto* watcher = new QFutureWatcher<std::vector<Sample>>(this);
    QPointer<FrameTimingDialog> guardedThis(this);
    connect(watcher, &QFutureWatcher<std::vector<Sample>>::finished, this,
            [guardedThis, watcher, generation] {
        auto samples = watcher->result();
        watcher->deleteLater();
        if (!guardedThis || generation != guardedThis->mAnalysisGeneration) return;
        auto* graph = new TimingGraph(std::move(samples), guardedThis);
        auto* layout = qobject_cast<QVBoxLayout*>(guardedThis->layout());
        if (guardedThis->mGraph) {
            layout->replaceWidget(guardedThis->mGraph, graph);
            guardedThis->mGraph->deleteLater();
        } else {
            layout->addWidget(graph, 1);
        }
        guardedThis->mGraph = graph;
    });
    watcher->setFuture(QtConcurrent::run(
        [pts, ticksPerSecond, targetFps, useCfrMapping] {
            return distanceToIdeal(pts, ticksPerSecond, targetFps, useCfrMapping);
        }));
}
