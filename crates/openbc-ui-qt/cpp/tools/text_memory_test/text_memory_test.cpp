#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <random>
#include <string>
#include <thread>

#include <QAbstractScrollArea>
#include <QApplication>
#include <QColor>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileInfo>
#include <QImage>
#include <QLabel>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSysInfo>
#include <QTextBlock>
#include <QTextFormat>
#include <QTextLayout>
#include <QTextEdit>
#include <QThreadPool>
#include <QWidget>
#include <QSet>

#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#elif defined(Q_OS_MACOS)
#include <libproc.h>
#include <malloc/malloc.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <unistd.h>
#elif defined(Q_OS_LINUX)
#include <malloc.h>
#endif

#include "backend/backend_api.h"
#include "viewers/text/text_compare_viewer.h"

extern "C" std::uint64_t openbc_rust_allocated_bytes();
extern "C" std::uint64_t openbc_rust_peak_allocated_bytes();

namespace {

std::uint64_t privateBytes() {
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                              sizeof(counters))) {
        std::fprintf(stderr, "GetProcessMemoryInfo failed\n");
        std::exit(70);
    }
    return static_cast<std::uint64_t>(counters.PrivateUsage);
#elif defined(Q_OS_MACOS)
    rusage_info_v4 usage{};
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4,
                        reinterpret_cast<rusage_info_t*>(&usage)) != 0) {
        std::fprintf(stderr, "proc_pid_rusage failed\n");
        std::exit(70);
    }
    return usage.ri_phys_footprint;
#elif defined(Q_OS_LINUX)
    std::ifstream input("/proc/self/smaps_rollup");
    if (!input) {
        std::fprintf(stderr, "cannot read /proc/self/smaps_rollup\n");
        std::exit(70);
    }
    std::string line;
    std::uint64_t totalKiB = 0;
    bool foundPrivateMetric = false;
    while (std::getline(input, line)) {
        std::istringstream row(line);
        std::string key;
        std::uint64_t value = 0;
        std::string unit;
        if (!(row >> key >> value >> unit)) continue;
        if (key == "Private_Clean:" || key == "Private_Dirty:" ||
            key == "Private_Hugetlb:") {
            totalKiB += value;
            foundPrivateMetric = true;
        }
    }
    if (!foundPrivateMetric) {
        std::fprintf(stderr, "private-byte metrics are missing from smaps_rollup\n");
        std::exit(70);
    }
    return totalKiB * 1024;
#else
    std::fprintf(stderr, "private-byte measurement is unsupported on this platform\n");
    std::exit(70);
#endif
}

void trimProcessMemory() {
#ifdef Q_OS_WIN
    HeapCompact(GetProcessHeap(), 0);
#elif defined(Q_OS_MACOS)
    malloc_zone_pressure_relief(nullptr, 0);
#elif defined(Q_OS_LINUX)
    malloc_trim(0);
#endif
}

QString cpuModel() {
#ifdef Q_OS_WIN
    return qEnvironmentVariable("PROCESSOR_IDENTIFIER", QStringLiteral("unknown"));
#elif defined(Q_OS_MACOS)
    char name[256]{};
    std::size_t length = sizeof(name);
    if (sysctlbyname("machdep.cpu.brand_string", name, &length, nullptr, 0) != 0) {
        return QStringLiteral("unknown");
    }
    return QString::fromLocal8Bit(name);
#elif defined(Q_OS_LINUX)
    std::ifstream input("/proc/cpuinfo");
    std::string line;
    while (std::getline(input, line)) {
        constexpr char prefix[] = "model name";
        if (line.rfind(prefix, 0) == 0) {
            const auto separator = line.find(':');
            if (separator != std::string::npos) {
                return QString::fromStdString(line.substr(separator + 1)).trimmed();
            }
        }
    }
    return QStringLiteral("unknown");
#else
    return QStringLiteral("unknown");
#endif
}

void qtMessageHandler(QtMsgType type, const QMessageLogContext& context,
                      const QString& message) {
    if (message == QStringLiteral("This plugin does not support propagateSizeHints()")) return;
    const QByteArray formatted = qFormatLogMessage(type, context, message).toUtf8();
    std::fprintf(stderr, "%s\n", formatted.constData());
}

class FirstPaintProbe final : public QObject {
public:
    explicit FirstPaintProbe(QElapsedTimer& timer) : timer_(timer) {}

    qint64 firstPaintMs() const { return firstPaintMs_; }
    void watch(QWidget* widget) {
        targets_.insert(widget);
        widget->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (targets_.contains(watched) && firstPaintMs_ < 0 &&
            event->type() == QEvent::Paint) {
            firstPaintMs_ = timer_.elapsed();
        }
        return false;
    }

private:
    QElapsedTimer& timer_;
    QSet<QObject*> targets_;
    qint64 firstPaintMs_ = -1;
};

QString statusText(const QWidget& viewer) {
    for (const QLabel* label : viewer.findChildren<QLabel*>()) {
        const QString value = label->text();
        if (value.startsWith(QStringLiteral("Large file")) ||
            value.startsWith(QStringLiteral("Could not load large-file")) ||
            value.startsWith(QStringLiteral("Text comparisons support"))) {
            return value;
        }
    }
    return {};
}

std::uint64_t saturatedPixels(QWidget& viewer) {
    const QImage image = viewer.grab().toImage().convertToFormat(QImage::Format_RGB32);
    std::uint64_t count = 0;
    for (int y = 0; y < image.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QColor color(row[x]);
            if (color.hsvSaturation() > 25) ++count;
        }
    }
    return count;
}

std::uint64_t visibleSyntaxSpans(QWidget& viewer) {
    std::uint64_t count = 0;
    for (QPlainTextEdit* editor : viewer.findChildren<QPlainTextEdit*>()) {
        QTextBlock block = editor->cursorForPosition(QPoint(2, 2)).block();
        const int visibleLines = editor->viewport()->height() /
                                     std::max(1, editor->fontMetrics().height()) +
                                 2;
        for (int line = 0; line < visibleLines && block.isValid(); ++line) {
            if (block.layout()) {
                for (const QTextLayout::FormatRange& range : block.layout()->formats()) {
                    if (range.format.foreground().style() != Qt::NoBrush) ++count;
                }
            }
            block = block.next();
        }
    }
    return count;
}

std::uint64_t visibleDiffBackgrounds(QWidget& viewer) {
    std::uint64_t count = 0;
    for (QPlainTextEdit* editor : viewer.findChildren<QPlainTextEdit*>()) {
        for (const QTextEdit::ExtraSelection& selection : editor->extraSelections()) {
            const QColor background = selection.format.background().color();
            if (selection.format.property(QTextFormat::FullWidthSelection).toBool() &&
                background.isValid() && background.alpha() > 30 &&
                background != QColor(255, 255, 255, 22)) {
                ++count;
            }
        }
    }
    return count;
}

std::uint64_t countColorPixels(const QImage& image, const QColor& color) {
    std::uint64_t count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y) == color) ++count;
        }
    }
    return count;
}

int runMinimapProgressTest(QApplication& application) {
    openbc::app::DifferenceOverview minimap(nullptr, true);
    minimap.resize(28, 400);
    minimap.setPixelLineMode(false);
    minimap.show();
    minimap.beginProgressiveRows();
    application.processEvents();

    const QColor changedColor("#ff5c5c");
    const QColor unmatchedColor("#9aa0b8");
    const QColor whitespaceColor("#4ea1ff");
    const QVector<openbc::app::TextDiffLine> firstPage = {
        {QStringLiteral("before"), QStringLiteral("after"), 1, 1, true, false},
        {QString(), QStringLiteral("inserted"), 0, 2, true, false},
    };
    minimap.appendProgressiveRows(firstPage);
    application.processEvents();
    const QImage firstPageImage = minimap.grab().toImage();
    const std::uint64_t firstPageChanged =
        countColorPixels(firstPageImage, changedColor);
    const std::uint64_t firstPageUnmatched =
        countColorPixels(firstPageImage, unmatchedColor);
    if (firstPageChanged == 0 || firstPageUnmatched == 0) {
        std::fprintf(stderr,
                     "minimap first-page markers missing: matched-diff=%llu unmatched=%llu\n",
                     static_cast<unsigned long long>(firstPageChanged),
                     static_cast<unsigned long long>(firstPageUnmatched));
        return 1;
    }

    const QVector<openbc::app::TextDiffLine> secondPage = {
        {QStringLiteral("same"), QStringLiteral("same"), 2, 3, false, false},
        {QStringLiteral("  value"), QStringLiteral(" value"), 3, 4, true, true},
    };
    minimap.appendProgressiveRows(secondPage);
    application.processEvents();
    const QImage completeImage = minimap.grab().toImage();
    const std::uint64_t completeChanged =
        countColorPixels(completeImage, changedColor);
    const std::uint64_t completeUnmatched =
        countColorPixels(completeImage, unmatchedColor);
    const std::uint64_t completeWhitespace =
        countColorPixels(completeImage, whitespaceColor);
    if (completeChanged == 0 || completeUnmatched == 0 || completeWhitespace == 0) {
        std::fprintf(stderr,
                     "minimap progressive markers missing: matched-diff=%llu unmatched=%llu "
                     "whitespace-diff=%llu\n",
                     static_cast<unsigned long long>(completeChanged),
                     static_cast<unsigned long long>(completeUnmatched),
                     static_cast<unsigned long long>(completeWhitespace));
        return 1;
    }

    std::printf("minimap progressive test passed: first_page matched_diff_pixels=%llu "
                "unmatched_pixels=%llu; after_second_page whitespace_diff_pixels=%llu\n",
                static_cast<unsigned long long>(firstPageChanged),
                static_cast<unsigned long long>(firstPageUnmatched),
                static_cast<unsigned long long>(completeWhitespace));
    return 0;
}

int runLargeFileMinimapProgressTest(QApplication& application,
                                    const QString& leftPath,
                                    const QString& rightPath) {
    openbc::app::TextCompareViewer viewer(leftPath, rightPath, QString(), QString());
    viewer.resize(1280, 800);
    viewer.show();
    auto* minimapFrame = viewer.findChild<QFrame*>(QStringLiteral("textCompareMinimap"));
    if (!minimapFrame) {
        std::fprintf(stderr, "large-file viewer did not create its minimap\n");
        return 1;
    }
    auto* minimap = static_cast<openbc::app::DifferenceOverview*>(minimapFrame);

    QElapsedTimer elapsed;
    elapsed.start();
    int rowsWhenMarkersFirstAppeared = -1;
    std::uint64_t firstChangedPixels = 0;
    std::uint64_t firstUnmatchedPixels = 0;
    int rowsAfterMarkersAppeared = -1;
    while (elapsed.elapsed() < 60000) {
        application.processEvents(QEventLoop::AllEvents, 25);
        const QString status = statusText(viewer);
        if (status.startsWith(QStringLiteral("Large file loaded"))) break;

        const int rows = minimap->rowCount();
        if (rowsWhenMarkersFirstAppeared < 0 && rows > 0) {
            const QImage image = minimap->grab().toImage();
            firstChangedPixels = countColorPixels(image, QColor("#ff5c5c"));
            firstUnmatchedPixels = countColorPixels(image, QColor("#9aa0b8"));
            if (firstChangedPixels + firstUnmatchedPixels > 0) {
                rowsWhenMarkersFirstAppeared = rows;
            }
        } else if (rowsWhenMarkersFirstAppeared >= 0 &&
                   rows > rowsWhenMarkersFirstAppeared) {
            rowsAfterMarkersAppeared = rows;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const bool progressiveUpdate =
        rowsWhenMarkersFirstAppeared >= 0 &&
        rowsAfterMarkersAppeared > rowsWhenMarkersFirstAppeared;
    const int finalMappedRows = minimap->rowCount();
    viewer.releaseLargeFileResources();
    viewer.hide();
    if (!QThreadPool::globalInstance()->waitForDone(30000)) {
        std::fprintf(stderr, "Qt comparison worker pool did not drain within 30 seconds\n");
        return 1;
    }
    if (!progressiveUpdate) {
        std::fprintf(stderr,
                     "large-file minimap did not show diff markers and then add more rows "
                     "progressively within 60 seconds; mapped_rows=%d first_marker_rows=%d\n",
                     finalMappedRows, rowsWhenMarkersFirstAppeared);
        return 1;
    }
    std::printf(
        "large-file minimap progressive test passed: first_markers_at_rows=%d "
        "matched_diff_pixels=%llu unmatched_pixels=%llu later_rows=%d elapsed_ms=%lld\n",
        rowsWhenMarkersFirstAppeared,
        static_cast<unsigned long long>(firstChangedPixels),
        static_cast<unsigned long long>(firstUnmatchedPixels),
        rowsAfterMarkersAppeared,
        static_cast<long long>(elapsed.elapsed()));
    return 0;
}

struct CycleResult {
    std::uint64_t beforeOpenPrivateBytes = 0;
    qint64 firstPaintMs = -1;
    qint64 firstLoadedMs = -1;
    std::uint64_t peakPrivateBytes = 0;
    std::uint64_t steadyPrivateBytes = 0;
    std::uint64_t rustPeakBytes = 0;
    std::uint64_t colorsAt5Seconds = 0;
    std::uint64_t colorsAt30Seconds = 0;
    std::uint64_t syntaxAt5Seconds = 0;
    std::uint64_t syntaxAt30Seconds = 0;
    std::uint64_t diffAt5Seconds = 0;
    std::uint64_t diffAt30Seconds = 0;
    bool loadSucceeded = false;
};

CycleResult runCycle(const QString& leftPath, const QString& rightPath, int cycle) {
    CycleResult result;
    result.beforeOpenPrivateBytes = privateBytes();
    QElapsedTimer elapsed;
    elapsed.start();
    FirstPaintProbe paintProbe(elapsed);

    {
        openbc::app::TextCompareViewer viewer(leftPath, rightPath, QString(), QString());
        for (QPlainTextEdit* editor : viewer.findChildren<QPlainTextEdit*>()) {
            paintProbe.watch(editor->viewport());
        }
        viewer.resize(1280, 800);
        viewer.show();

        bool recordedFive = false;
        bool recordedThirty = false;
        qint64 lastProgressMs = 0;
        while (elapsed.elapsed() < 30 * 60 * 1000) {
            QApplication::processEvents(QEventLoop::AllEvents, 25);
            const std::uint64_t memory = privateBytes();
            result.peakPrivateBytes = std::max(result.peakPrivateBytes, memory);

            const QString status = statusText(viewer);
            if (elapsed.elapsed() - lastProgressMs >= 5000) {
                std::printf("progress_cycle=%d elapsed_ms=%lld private_bytes=%llu status=%s\n",
                            cycle, static_cast<long long>(elapsed.elapsed()),
                            static_cast<unsigned long long>(memory),
                            status.toUtf8().constData());
                std::fflush(stdout);
                lastProgressMs = elapsed.elapsed();
            }
            if (status.startsWith(QStringLiteral("Large file loaded"))) {
                result.loadSucceeded = true;
                result.firstLoadedMs = elapsed.elapsed();
                break;
            }
            if (status.startsWith(QStringLiteral("Could not load large-file")) ||
                status.startsWith(QStringLiteral("Text comparisons support"))) {
                std::fprintf(stderr, "cycle %d stopped: %s\n", cycle,
                             status.toUtf8().constData());
                break;
            }
            if (!recordedFive && elapsed.elapsed() >= 5000) {
                result.colorsAt5Seconds = saturatedPixels(viewer);
                result.syntaxAt5Seconds = visibleSyntaxSpans(viewer);
                result.diffAt5Seconds = visibleDiffBackgrounds(viewer);
                recordedFive = true;
            }
            if (!recordedThirty && elapsed.elapsed() >= 30000) {
                result.colorsAt30Seconds = saturatedPixels(viewer);
                result.syntaxAt30Seconds = visibleSyntaxSpans(viewer);
                result.diffAt30Seconds = visibleDiffBackgrounds(viewer);
                recordedThirty = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        result.firstPaintMs = paintProbe.firstPaintMs();
        if (result.loadSucceeded) {
            auto editors = viewer.findChildren<QPlainTextEdit*>();
            std::mt19937_64 random(static_cast<std::uint64_t>(cycle) + 1);
            for (int i = 0; i < 200; ++i) {
                for (QPlainTextEdit* editor : editors) {
                    QScrollBar* bar = editor->verticalScrollBar();
                    const int maximum = bar->maximum();
                    std::uniform_int_distribution<int> position(0, maximum);
                    bar->setValue(position(random));
                }
                QApplication::processEvents(QEventLoop::AllEvents, 10);
                result.peakPrivateBytes =
                    std::max(result.peakPrivateBytes, privateBytes());
            }
            result.steadyPrivateBytes = privateBytes();
            result.peakPrivateBytes =
                std::max(result.peakPrivateBytes, result.steadyPrivateBytes);
            result.rustPeakBytes = openbc_rust_peak_allocated_bytes();
        }

        viewer.releaseLargeFileResources();
        viewer.hide();
        if (!QThreadPool::globalInstance()->waitForDone(30000)) {
            std::fprintf(stderr, "Qt comparison worker pool did not drain within 30 seconds\n");
        }
        QApplication::processEvents(QEventLoop::AllEvents, 25);
    }

    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents(QEventLoop::AllEvents, 25);
    trimProcessMemory();
    std::printf(
        "cycle=%d first_paint_ms=%lld first_loaded_ms=%lld peak_private_bytes=%llu "
        "steady_private_bytes=%llu after_close_private_bytes=%llu rust_live_bytes=%llu "
        "rust_peak_bytes=%llu saturated_pixels_t5=%llu saturated_pixels_t30=%llu "
        "syntax_spans_t5=%llu syntax_spans_t30=%llu diff_backgrounds_t5=%llu "
        "diff_backgrounds_t30=%llu private_delta_bytes=%lld loaded=%s\n",
        cycle,
        static_cast<long long>(result.firstPaintMs),
        static_cast<long long>(result.firstLoadedMs),
        static_cast<unsigned long long>(result.peakPrivateBytes),
        static_cast<unsigned long long>(result.steadyPrivateBytes),
        static_cast<unsigned long long>(privateBytes()),
        static_cast<unsigned long long>(openbc_rust_allocated_bytes()),
        static_cast<unsigned long long>(result.rustPeakBytes),
        static_cast<unsigned long long>(result.colorsAt5Seconds),
        static_cast<unsigned long long>(result.colorsAt30Seconds),
        static_cast<unsigned long long>(result.syntaxAt5Seconds),
        static_cast<unsigned long long>(result.syntaxAt30Seconds),
        static_cast<unsigned long long>(result.diffAt5Seconds),
        static_cast<unsigned long long>(result.diffAt30Seconds),
        static_cast<long long>(privateBytes()) -
            static_cast<long long>(result.beforeOpenPrivateBytes),
        result.loadSucceeded ? "yes" : "no");
    std::fflush(stdout);
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--minimap-progress-test") {
        QApplication application(argc, argv);
        application.setQuitOnLastWindowClosed(false);
        return runMinimapProgressTest(application);
    }
    const bool largeFileMinimapTest =
        argc == 4 && std::string(argv[1]) == "--large-file-minimap-test";
    if ((!largeFileMinimapTest && (argc < 3 || argc > 4)) ||
        (largeFileMinimapTest && argc != 4)) {
        std::fprintf(stderr,
                     "usage: text_memory_test LEFT RIGHT [CYCLES]\n"
                     "       text_memory_test --minimap-progress-test\n"
                     "       text_memory_test --large-file-minimap-test LEFT RIGHT\n");
        return 64;
    }
    int cycles = 5;
    if (!largeFileMinimapTest && argc == 4) {
        char* end = nullptr;
        const long parsed = std::strtol(argv[3], &end, 10);
        if (end == argv[3] || *end != '\0' || parsed < 1 || parsed > INT_MAX) {
            std::fprintf(stderr, "CYCLES must be a positive integer no greater than %d\n", INT_MAX);
            return 64;
        }
        cycles = static_cast<int>(parsed);
    }
    const int leftArgument = largeFileMinimapTest ? 2 : 1;
    const int rightArgument = largeFileMinimapTest ? 3 : 2;
    const QString leftPath =
        QFileInfo(QString::fromLocal8Bit(argv[leftArgument])).absoluteFilePath();
    const QString rightPath =
        QFileInfo(QString::fromLocal8Bit(argv[rightArgument])).absoluteFilePath();
    if (!QFileInfo::exists(leftPath) || !QFileInfo::exists(rightPath)) {
        std::fprintf(stderr, "both input files must exist\n");
        return 66;
    }
    if (openbc_initialize_observability() != 0) {
        std::fprintf(stderr, "observability initialization failed\n");
        return 70;
    }
    qInstallMessageHandler(qtMessageHandler);
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);

    if (largeFileMinimapTest) {
        return runLargeFileMinimapProgressTest(application, leftPath, rightPath);
    }

    const std::uint64_t baseline = privateBytes();
    std::printf("machine=%s cpu=%s os=%s arch=%s private_bytes_before=%llu "
                "left_bytes=%lld right_bytes=%lld cycles=%d\n",
                QSysInfo::machineHostName().toUtf8().constData(),
                cpuModel().toUtf8().constData(),
                QSysInfo::prettyProductName().toUtf8().constData(),
                QSysInfo::currentCpuArchitecture().toUtf8().constData(),
                static_cast<unsigned long long>(baseline),
                static_cast<long long>(QFileInfo(leftPath).size()),
                static_cast<long long>(QFileInfo(rightPath).size()), cycles);
    bool allLoaded = true;
    for (int cycle = 1; cycle <= cycles; ++cycle) {
        const CycleResult result = runCycle(leftPath, rightPath, cycle);
        allLoaded = allLoaded && result.loadSucceeded;
    }
    return allLoaded ? 0 : 1;
}
