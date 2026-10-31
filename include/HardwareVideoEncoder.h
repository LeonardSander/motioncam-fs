#pragma once

#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

namespace motioncam {
struct HardwareVideoEncoder {
    QString name;
    QString pixelFormat;
    QStringList arguments;
};

// Probe an actual encode: an encoder listed by FFmpeg may have no usable GPU,
// or may silently negotiate a lower bit depth. Keep Camera Native's full-range
// 10-bit BT.2020 contract rather than accepting that negotiation.
inline HardwareVideoEncoder selectHardwareVideoEncoder(
        const QString& ffmpeg, bool av1, const QString& chroma,
        int width, int height, double fps, int quality) {
    const QString codec = av1 ? "av1" : "hevc";
    const QStringList backends{"nvenc", "qsv", "amf", "vaapi", "videotoolbox"};
    QTemporaryDir probeDirectory;
    if (!probeDirectory.isValid() || width <= 0 || height <= 0) return {};
    for (const auto& backend : backends) {
        HardwareVideoEncoder candidate;
        candidate.name = codec + "_" + backend;
        QProcess help;
        help.setProcessChannelMode(QProcess::MergedChannels);
        help.start(ffmpeg, {"-hide_banner", "-h", "encoder=" + candidate.name});
        if (!help.waitForFinished(3000)) {
            help.kill();
            help.waitForFinished();
            continue;
        }
        const QByteArray capabilities = ' ' + help.readAll().simplified() + ' ';
        if (!capabilities.contains(("Encoder " + candidate.name + " ").toUtf8()))
            continue;
        // These are alternate packings of the requested 10-bit chroma layout.
        const QStringList formats = chroma == "444"
            ? QStringList{"yuv444p10le", "yuv444p16le"}
            : chroma == "422" ? QStringList{"yuv422p10le", "p210le", "y210le"}
                              : QStringList{"yuv420p10le", "p010le"};
        for (const auto& format : formats) {
            if (capabilities.contains((" " + format + " ").toUtf8())) {
                candidate.pixelFormat = format;
                break;
            }
        }
        // VAAPI advertises only hardware surfaces; uploads use P010.
        if (backend == "vaapi" && chroma == "420") candidate.pixelFormat = "p010le";
        if (candidate.pixelFormat.isEmpty()) continue;
        candidate.arguments = {"-c:v", candidate.name};
        const QString qp = QString::number(qBound(1, quality, 51));
        if (backend == "nvenc")
            candidate.arguments << "-preset" << "p5" << "-rc" << "vbr"
                                << "-cq" << qp << "-b:v" << "0";
        else if (backend == "qsv")
            candidate.arguments << "-preset" << "medium" << "-global_quality" << qp;
        else if (backend == "amf")
            candidate.arguments << "-rc" << "cqp" << "-qp_i" << qp << "-qp_p" << qp;
        else if (backend == "vaapi")
            candidate.arguments << "-rc_mode" << "CQP" << "-qp" << qp;
        else {
            candidate.arguments << "-allow_sw" << "0"
                                << "-q:v" << QString::number(qBound(1, 100 - quality, 100));
        }
        // LOG60 has no standardized transfer tag. Some encoders omit all
        // primaries when transfer is unspecified; set primaries explicitly in
        // the bitstream while leaving transfer unspecified.
        candidate.arguments << "-bsf:v" << (av1
            ? "av1_metadata=color_primaries=9"
            : "hevc_metadata=colour_primaries=9");
        QStringList args{"-hide_banner", "-loglevel", "error", "-y"};
        if (backend == "vaapi") args << "-init_hw_device" << "vaapi=gpu" << "-filter_hw_device" << "gpu";
        args << "-f" << "lavfi" << "-i"
             << QString("color=black:size=%1x%2:rate=%3")
                    .arg(width).arg(height).arg(fps > 0 ? fps : 24.0)
             << "-vf" << ("format=" + candidate.pixelFormat +
                              (backend == "vaapi" ? ",hwupload" : ""));
        args << candidate.arguments << "-frames:v" << "1" << "-color_range" << "pc"
             << "-colorspace" << "bt2020nc" << "-color_primaries" << "bt2020";
        const QString output = probeDirectory.filePath("probe.mov");
        args << output;
        QProcess probe;
        probe.setProcessChannelMode(QProcess::MergedChannels);
        probe.start(ffmpeg, args);
        if (!probe.waitForFinished(5000)) {
            probe.kill();
            probe.waitForFinished();
            continue;
        }
        if (probe.exitStatus() != QProcess::NormalExit || probe.exitCode()) continue;
        AVFormatContext* context = nullptr;
        const auto path = output.toUtf8();
        bool compatible = false;
        if (avformat_open_input(&context, path.constData(), nullptr, nullptr) >= 0 &&
            avformat_find_stream_info(context, nullptr) >= 0) {
            for (unsigned i = 0; i < context->nb_streams; ++i) {
                const auto* parameters = context->streams[i]->codecpar;
                if (parameters->codec_type != AVMEDIA_TYPE_VIDEO) continue;
                const auto* format = av_pix_fmt_desc_get(AVPixelFormat(parameters->format));
                compatible = format && format->nb_components == 3 &&
                    format->comp[0].depth == 10 &&
                    format->log2_chroma_w == (chroma == "444" ? 0 : 1) &&
                    format->log2_chroma_h == (chroma == "420" ? 1 : 0) &&
                    parameters->codec_id == (av1 ? AV_CODEC_ID_AV1 : AV_CODEC_ID_HEVC) &&
                    parameters->width == width && parameters->height == height &&
                    parameters->color_range == AVCOL_RANGE_JPEG &&
                    parameters->color_space == AVCOL_SPC_BT2020_NCL &&
                    parameters->color_primaries == AVCOL_PRI_BT2020;
                break;
            }
        }
        avformat_close_input(&context);
        if (compatible) return candidate;
    }
    return {};
}
} // namespace motioncam
