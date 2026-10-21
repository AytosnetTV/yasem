#include "player.h"

#include <QFile>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QUrl>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#ifdef slots
#undef slots
#endif
#include <codec.h>
}

#include <alsa/asoundlib.h>

namespace {

enum {
    STB_EVENT_EOF = 1,
    STB_EVENT_GOT_INFO = 2,
    STB_EVENT_PLAY_START = 4,
    STB_EVENT_PLAY_ERROR = 5,
    STB_EVENT_GOT_VIDEO_INFO = 7
};

void logLine(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

void writeSysfs(const char *path, const QByteArray &value)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Unbuffered))
        return;
    file.write(value);
}

void setPortalVisible(bool visible)
{
    writeSysfs("/sys/class/graphics/fb0/blank", visible ? "0" : "1");
    if (visible) {
        writeSysfs("/sys/class/video/video_zorder", "126");
        writeSysfs("/sys/class/graphics/fb0/order", "2");
    }
}

bool startsAnnexB(const uint8_t *data, int size)
{
    if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1)
        return true;
    if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1)
        return true;
    return false;
}

void appendStartCode(std::vector<uint8_t> &out)
{
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    out.push_back(1);
}

std::vector<uint8_t> avccToAnnexB(const uint8_t *data, int size)
{
    std::vector<uint8_t> out;
    if (!data || size < 7 || data[0] != 1)
        return out;
    int nalLen = (data[4] & 0x03) + 1;
    int offset = 5;
    int spsCount = data[offset] & 0x1f;
    offset++;
    for (int i = 0; i < spsCount && offset + 2 <= size; ++i) {
        int len = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        if (len < 0 || offset + len > size)
            break;
        appendStartCode(out);
        out.insert(out.end(), data + offset, data + offset + len);
        offset += len;
    }
    if (offset >= size)
        return out;
    int ppsCount = data[offset];
    offset++;
    for (int i = 0; i < ppsCount && offset + 2 <= size; ++i) {
        int len = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        if (len < 0 || offset + len > size)
            break;
        appendStartCode(out);
        out.insert(out.end(), data + offset, data + offset + len);
        offset += len;
    }
    Q_UNUSED(nalLen);
    return out;
}

std::vector<uint8_t> lengthPrefixedToAnnexB(const uint8_t *data, int size, int nalLen)
{
    std::vector<uint8_t> out;
    if (nalLen < 1 || nalLen > 4)
        nalLen = 4;
    int offset = 0;
    while (offset + nalLen <= size) {
        int len = 0;
        for (int i = 0; i < nalLen; ++i)
            len = (len << 8) | data[offset + i];
        offset += nalLen;
        if (len <= 0 || offset + len > size)
            break;
        appendStartCode(out);
        out.insert(out.end(), data + offset, data + offset + len);
        offset += len;
    }
    return out;
}

std::vector<uint8_t> hvccToAnnexB(const uint8_t *data, int size)
{
    std::vector<uint8_t> out;
    if (!data || size < 23 || data[0] != 1)
        return out;
    int offset = 22;
    int arrays = data[offset++];
    for (int a = 0; a < arrays && offset + 3 <= size; ++a) {
        offset++;
        int count = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        for (int n = 0; n < count && offset + 2 <= size; ++n) {
            int len = (data[offset] << 8) | data[offset + 1];
            offset += 2;
            if (len <= 0 || offset + len > size)
                return out;
            appendStartCode(out);
            out.insert(out.end(), data + offset, data + offset + len);
            offset += len;
        }
    }
    return out;
}

bool isArchiveUrl(const QString &url)
{
    const QString u = url.toLower();
    if (u.contains(QLatin1String("/timeshift/"))
        || u.contains(QLatin1String("/tvarchive/"))
        || u.contains(QLatin1String("/tv_archive/"))
        || u.contains(QLatin1String("archivetype"))
        || u.contains(QLatin1String("/stalker_portal/storage/"))
        || u.contains(QLatin1String("download.php")))
        return true;
    // Stalker archive segments: *.ts?play_token=...
    if (u.contains(QLatin1String(".ts"))
        && (u.contains(QLatin1String("play_token")) || u.contains(QLatin1String("utc="))
            || u.contains(QLatin1String("lutc="))))
        return true;
    return false;
}

QString normalizeUrl(const QString &playStr, QString *protoOut)
{
    QString text = playStr.trimmed();
    QString proto = "auto";
    int space = text.indexOf(' ');
    if (space > 0) {
        QString head = text.left(space).toLower();
        const char *known[] = {
            "auto", "rtp", "rtsp", "rtsp_ac3", "rtpac3", "mpegts", "mpegps", "file",
            "mp4", "mp4_mpa", "ffmpeg", "ffrt", "ffrt2", "ffrt3", "ffrt4", "fm", nullptr
        };
        for (int i = 0; known[i]; ++i) {
            if (head == known[i]) {
                proto = head;
                text = text.mid(space + 1).trimmed();
                break;
            }
        }
    }
    int position = text.indexOf(" position:");
    if (position > 0)
        text = text.left(position).trimmed();
    if (text.startsWith("udp://@"))
        text.replace(0, 7, "udp://");
    if (text.startsWith("rtp://@"))
        text.replace(0, 7, "rtp://");
    if (protoOut)
        *protoOut = proto;
    return text;
}

int videoFormatFor(AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_H264: return VFORMAT_H264;
    case AV_CODEC_ID_HEVC: return VFORMAT_HEVC;
    case AV_CODEC_ID_MPEG2VIDEO:
    case AV_CODEC_ID_MPEG1VIDEO: return VFORMAT_MPEG12;
    case AV_CODEC_ID_MPEG4: return VFORMAT_MPEG4;
    case AV_CODEC_ID_VP9: return VFORMAT_VP9;
    case AV_CODEC_ID_MJPEG: return VFORMAT_MJPEG;
    default: return VFORMAT_UNKNOWN;
    }
}

int videoDecFormatFor(AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_H264: return VIDEO_DEC_FORMAT_H264;
    case AV_CODEC_ID_HEVC: return VIDEO_DEC_FORMAT_HEVC;
    case AV_CODEC_ID_MPEG4: return VIDEO_DEC_FORMAT_MPEG4_5;
    case AV_CODEC_ID_VP9: return VIDEO_DEC_FORMAT_VP9;
    case AV_CODEC_ID_MJPEG: return VIDEO_DEC_FORMAT_MJPEG;
    default: return VIDEO_DEC_FORMAT_UNKNOW;
    }
}

bool writeAll(codec_para_t *codec, const uint8_t *data, int size, const std::atomic<bool> &running,
    const std::function<void()> &onWait = {})
{
    int offset = 0;
    while (offset < size && running.load()) {
        int wrote = codec_write(codec, (void *)(data + offset), size - offset);
        if (wrote > 0) {
            offset += wrote;
            continue;
        }
        if (onWait)
            onWait();
        QThread::usleep(2000);
    }
    return offset == size;
}

enum { EXTERNAL_PTS = 1, SYNC_OUTSIDE = 2 };

bool annexBHasKeyframe(const uint8_t *data, int size, AVCodecID id)
{
    if (!data || size < 5)
        return false;
    for (int i = 0; i + 4 < size; ++i) {
        int sc = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            sc = 3;
        else if (i + 5 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
            sc = 4;
        if (!sc)
            continue;
        const int nal = data[i + sc];
        if (id == AV_CODEC_ID_H264) {
            const int t = nal & 0x1f;
            if (t == 5 || t == 7) // IDR or SPS (SPS usually precedes IDR)
                return true;
        } else if (id == AV_CODEC_ID_HEVC) {
            const int t = (nal >> 1) & 0x3f;
            if (t == 19 || t == 20 || t == 32) // IDR_W, IDR_N, VPS
                return true;
        }
        i += sc;
    }
    return false;
}

int checkinPtsUs64(codec_para_t *video, uint64_t ptsUs)
{
    if (!video || video->handle < 0)
        return -1;
    if (ptsUs == 0)
        ptsUs = 1;
    struct am_ioctl_parm parm;
    memset(&parm, 0, sizeof(parm));
    parm.cmd = AMSTREAM_SET_TSTAMP_US64;
    parm.data_64 = ptsUs;
    if (ioctl(video->handle, AMSTREAM_IOC_SET, &parm) == 0)
        return 0;
    unsigned long pts90 = (unsigned long)((ptsUs * 90ULL) / 1000ULL);
    if (pts90 == 0)
        pts90 = 1;
    return codec_checkin_pts(video, pts90);
}

struct PlayShare {
    std::atomic<bool> *running = nullptr;
    std::atomic<bool> *pause = nullptr;
    std::atomic<int> *volume = nullptr;
    std::atomic<bool> *mute = nullptr;
    std::atomic<int> *audioClockMs = nullptr;
    std::atomic<bool> *audioReady = nullptr;
    codec_para_t *video = nullptr;
    const std::vector<uint8_t> *header = nullptr;
    snd_pcm_t *pcm = nullptr;
    AVCodecContext *audioDec = nullptr;
    SwrContext *resampler = nullptr;
    AVFrame *audioFrame = nullptr;
    bool haveAudio = false;
    int video10 = -1;
    int displayFrameMs = 40;

    std::mutex videoMu;
    std::condition_variable videoCv;
    std::deque<std::vector<uint8_t>> videoQ;
    std::deque<int> videoPts;
    size_t videoBytes = 0;
    std::atomic<bool> videoDone{false};
    std::atomic<bool> sendHeader{false};
    std::atomic<bool> resetVideo{false};

    std::mutex audioMu;
    std::condition_variable audioCv;
    std::deque<std::vector<uint8_t>> audioQ;
    std::atomic<bool> audioDone{false};
    std::atomic<bool> resetAudio{false};
    bool skipPtsCheckin = false;
    bool audioMasterDisplay = false;
    int prefillPackets = 8;
    size_t videoBytesLimit = 768 * 1024;

    std::mutex codecMu;
};

void runVideo(PlayShare *s)
{
    int paused = 0;
    int frames = 0;
    while (s->running->load() || !s->videoDone.load()) {
        if (s->pause->load()) {
            if (!paused) {
                std::lock_guard<std::mutex> lock(s->codecMu);
                codec_pause(s->video);
                paused = 1;
            }
            QThread::msleep(20);
            continue;
        }
        if (paused) {
            std::lock_guard<std::mutex> lock(s->codecMu);
            codec_resume(s->video);
            paused = 0;
        }
        if (s->resetVideo.exchange(false)) {
            std::lock_guard<std::mutex> lock(s->codecMu);
            codec_reset(s->video);
            s->sendHeader.store(true);
        }

        std::vector<uint8_t> chunk;
        int ptsMs = 0;
        {
            std::unique_lock<std::mutex> lock(s->videoMu);
            s->videoCv.wait_for(lock, std::chrono::milliseconds(30), [&] {
                return !s->running->load() || s->videoDone.load() || !s->videoQ.empty();
            });
            if (s->videoQ.empty()) {
                if (!s->running->load() || s->videoDone.load())
                    break;
                continue;
            }
            chunk = std::move(s->videoQ.front());
            s->videoQ.pop_front();
            ptsMs = s->videoPts.front();
            s->videoPts.pop_front();
            if (s->videoBytes >= chunk.size())
                s->videoBytes -= chunk.size();
            else
                s->videoBytes = 0;
        }
        s->videoCv.notify_all();

        std::lock_guard<std::mutex> lock(s->codecMu);
        if (s->sendHeader.exchange(false) && s->header && !s->header->empty())
            writeAll(s->video, s->header->data(), (int)s->header->size(), *s->running);
        if (!s->skipPtsCheckin && ptsMs >= 0) {
            uint64_t ptsUs = (uint64_t)ptsMs * 1000ULL;
            int rc = checkinPtsUs64(s->video, ptsUs);
            if (frames < 3)
                logLine("checkin us %llu rc %d", (unsigned long long)ptsUs, rc);
        }
        if (!chunk.empty())
            writeAll(s->video, chunk.data(), (int)chunk.size(), *s->running);
        frames++;
        if (frames == s->prefillPackets) {
            codec_resume(s->video);
            logLine("codec resume after prefill %d", s->prefillPackets);
        }
    }
}

void runDisplay(PlayShare *s)
{
    std::chrono::steady_clock::time_point playStart;
    bool started = false;
    int released = 0;
    const int frameMs = s->displayFrameMs > 0 ? s->displayFrameMs : 40;
    const bool audioMaster = s->audioMasterDisplay && s->haveAudio && s->audioClockMs;
    while (s->running->load() || !s->videoDone.load()) {
        if (s->video10 < 0) {
            QThread::msleep(20);
            continue;
        }
        if (s->pause->load()) {
            QThread::msleep(20);
            continue;
        }

        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(s->video10, VIDIOC_DQBUF, &buf) < 0) {
            if (errno != EAGAIN)
                QThread::msleep(5);
            else
                QThread::msleep(2);
            if (!s->running->load() && s->videoDone.load())
                break;
            continue;
        }

        if (!started) {
            started = true;
            playStart = std::chrono::steady_clock::now();
            writeSysfs("/sys/class/video/disable_video", "0");
            writeSysfs("/sys/module/amvideo/parameters/omx_run", "N");
            logLine("first display frame pace %d ms%s", frameMs,
                audioMaster ? " audio-master" : "");
        }

        int targetMs = released * frameMs;
        // Archive: pace from ALSA clock so video neither slow-mos nor races the wall clock.
        if (audioMaster && s->audioReady && s->audioReady->load()) {
            while (s->running->load() && !s->pause->load()) {
                const int audioMs = s->audioClockMs->load();
                // Far behind audio — show immediately (and skip below if still behind).
                if (audioMs + 15 >= targetMs)
                    break;
                QThread::msleep(2);
            }
            // Drop late frames so picture catches sound.
            while (s->running->load() && !s->pause->load()) {
                const int audioMs = s->audioClockMs->load();
                if (audioMs <= targetMs + 100)
                    break;
                if (ioctl(s->video10, VIDIOC_QBUF, &buf) < 0)
                    break;
                released++;
                targetMs = released * frameMs;
                memset(&buf, 0, sizeof(buf));
                buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                if (ioctl(s->video10, VIDIOC_DQBUF, &buf) < 0)
                    break;
            }
        } else {
            while (s->running->load() && !s->pause->load()) {
                int elapsed = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - playStart).count();
                if (elapsed + 5 >= targetMs)
                    break;
                QThread::msleep(2);
            }
        }

        if (ioctl(s->video10, VIDIOC_QBUF, &buf) < 0)
            logLine("VIDIOC_QBUF failed %d", errno);
        released++;
        if (released == 1 || released == 50 || (released % 100) == 0)
            logLine("display frames %d", released);
    }
}

void runAudio(PlayShare *s)
{
    int paused = 0;
    int64_t played = 0;
    std::vector<int16_t> pcmBuffer;
    while (s->running->load() || !s->audioDone.load()) {
        if (s->resetAudio.exchange(false)) {
            if (s->audioDec)
                avcodec_flush_buffers(s->audioDec);
            if (s->pcm) {
                snd_pcm_drop(s->pcm);
                snd_pcm_prepare(s->pcm);
            }
            played = 0;
            s->audioClockMs->store(0);
            s->audioReady->store(false);
        }
        if (s->pause->load()) {
            if (!paused && s->pcm)
                snd_pcm_pause(s->pcm, 1);
            paused = 1;
            QThread::msleep(20);
            continue;
        }
        if (paused) {
            if (s->pcm)
                snd_pcm_pause(s->pcm, 0);
            paused = 0;
        }

        std::vector<uint8_t> chunk;
        {
            std::unique_lock<std::mutex> lock(s->audioMu);
            s->audioCv.wait_for(lock, std::chrono::milliseconds(30), [&] {
                return !s->running->load() || s->audioDone.load() || !s->audioQ.empty();
            });
            if (s->audioQ.empty()) {
                if (!s->running->load() || s->audioDone.load())
                    break;
                continue;
            }
            chunk = std::move(s->audioQ.front());
            s->audioQ.pop_front();
        }
        s->audioCv.notify_all();
        if (chunk.empty() || !s->audioDec || !s->pcm || !s->resampler || !s->audioFrame)
            continue;

        AVPacket packet;
        av_init_packet(&packet);
        packet.data = chunk.data();
        packet.size = (int)chunk.size();
        if (avcodec_send_packet(s->audioDec, &packet) != 0)
            continue;
        while (s->running->load() && avcodec_receive_frame(s->audioDec, s->audioFrame) == 0) {
            int outSamples = (int)av_rescale_rnd(
                swr_get_delay(s->resampler, s->audioFrame->sample_rate) + s->audioFrame->nb_samples,
                48000, s->audioFrame->sample_rate, AV_ROUND_UP);
            pcmBuffer.resize((size_t)outSamples * 2);
            uint8_t *outData = (uint8_t *)pcmBuffer.data();
            int got = swr_convert(s->resampler, &outData, outSamples,
                (const uint8_t **)s->audioFrame->data, s->audioFrame->nb_samples);
            if (got <= 0)
                continue;
            int vol = s->mute->load() ? 0 : s->volume->load();
            if (vol < 100) {
                for (int i = 0; i < got * 2; ++i)
                    pcmBuffer[(size_t)i] = (int16_t)(pcmBuffer[(size_t)i] * vol / 100);
            }
            int16_t *ptr = pcmBuffer.data();
            int left = got;
            while (left > 0 && s->running->load()) {
                snd_pcm_sframes_t wrote = snd_pcm_writei(s->pcm, ptr, left);
                if (wrote < 0) {
                    snd_pcm_prepare(s->pcm);
                    break;
                }
                ptr += wrote * 2;
                left -= (int)wrote;
                played += wrote;
                s->audioClockMs->store((int)(played * 1000 / 48000));
                s->audioReady->store(true);
            }
        }
    }
}

} // namespace

struct HwPlayer::Engine {
    HwPlayer *owner = nullptr;
    QThread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> pauseRequested{false};
    std::atomic<bool> playing{false};
    std::atomic<int> volumeValue{100};
    std::atomic<bool> muteValue{false};
    std::atomic<int> seekTarget{-1};
    std::atomic<int> position{0};
    std::atomic<int> duration{0};
    QString url;
    int viewX = 937;
    int viewY = 139;
    int viewW = 853;
    int viewH = 480;

    void notify(int code)
    {
        QMetaObject::invokeMethod(owner, "relay", Qt::QueuedConnection, Q_ARG(int, code));
    }

    void applyViewport()
    {
        int x = viewX;
        int y = viewY;
        int w = viewW > 0 ? viewW : 1920;
        int h = viewH > 0 ? viewH : 1080;
        int x2 = x + w - 1;
        int y2 = y + h - 1;
        if (x2 < x)
            x2 = x;
        if (y2 < y)
            y2 = y;
        writeSysfs("/sys/class/video/axis", QByteArray::number(x) + " " + QByteArray::number(y) + " "
            + QByteArray::number(x2) + " " + QByteArray::number(y2));
        codec_utils_set_video_position(x, y, w, h, 0);
    }

    bool openAlsa(snd_pcm_t **pcm)
    {
        const char *devices[] = {"default", "sysdefault", "plughw:0,0", nullptr};
        for (int i = 0; devices[i]; ++i) {
            if (snd_pcm_open(pcm, devices[i], SND_PCM_STREAM_PLAYBACK, 0) < 0)
                continue;
            if (snd_pcm_set_params(*pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                    2, 48000, 1, 200000) < 0) {
                snd_pcm_close(*pcm);
                *pcm = nullptr;
                continue;
            }
            logLine("alsa open %s", devices[i]);
            return true;
        }
        return false;
    }

    void loop()
    {
        QString proto;
        const QString playUrl = normalizeUrl(url, &proto);
        QByteArray urlBytes = playUrl.toUtf8();
        const bool archive = isArchiveUrl(playUrl);
        logLine("play %s%s", urlBytes.constData(), archive ? " [archive]" : "");

        AVFormatContext *fmt = avformat_alloc_context();
        fmt->interrupt_callback.opaque = this;
        fmt->interrupt_callback.callback = [](void *opaque) -> int {
            return static_cast<Engine *>(opaque)->running.load() ? 0 : 1;
        };

        AVDictionary *options = nullptr;
        av_dict_set(&options, "rw_timeout", "8000000", 0);
        av_dict_set(&options, "stimeout", "8000000", 0);
        av_dict_set(&options, "reconnect", "1", 0);
        av_dict_set(&options, "reconnect_streamed", "1", 0);
        // Archive .ts often starts mid-GOP / odd PTS — probe deeper and synthesize PTS.
        if (archive) {
            av_dict_set(&options, "probesize", "5000000", 0);
            av_dict_set(&options, "analyzeduration", "5000000", 0);
            av_dict_set(&options, "fflags", "genpts+discardcorrupt", 0);
        } else {
            av_dict_set(&options, "probesize", "1000000", 0);
            av_dict_set(&options, "analyzeduration", "1500000", 0);
        }
        av_dict_set(&options, "user_agent", "YASEM", 0);

        if (avformat_open_input(&fmt, urlBytes.constData(), nullptr, &options) < 0) {
            av_dict_free(&options);
            logLine("open failed");
            notify(STB_EVENT_PLAY_ERROR);
            if (fmt)
                avformat_close_input(&fmt);
            playing.store(false);
            return;
        }
        av_dict_free(&options);
        if (avformat_find_stream_info(fmt, nullptr) < 0) {
            logLine("no stream info");
            notify(STB_EVENT_PLAY_ERROR);
            avformat_close_input(&fmt);
            playing.store(false);
            return;
        }

        int videoIndex = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        int audioIndex = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, videoIndex, nullptr, 0);
        if (videoIndex < 0) {
            logLine("no video stream");
            notify(STB_EVENT_PLAY_ERROR);
            avformat_close_input(&fmt);
            playing.store(false);
            return;
        }

        AVStream *videoStream = fmt->streams[videoIndex];
        AVCodecParameters *videoPar = videoStream->codecpar;
        AVCodecID videoId = videoPar->codec_id;
        int vformat = videoFormatFor(videoId);
        if (vformat == VFORMAT_UNKNOWN) {
            logLine("unsupported video codec %d", videoId);
            notify(STB_EVENT_PLAY_ERROR);
            avformat_close_input(&fmt);
            playing.store(false);
            return;
        }

        double fps = av_q2d(videoStream->avg_frame_rate);
        if (fps < 1.0)
            fps = av_q2d(videoStream->r_frame_rate);
        if (fps < 1.0)
            fps = 25.0;
        int fpsInt = (int)(fps + 0.5);
        if (fpsInt < 1 || fpsInt > 120)
            fpsInt = 25;
        unsigned int rate = (unsigned int)(96000.0 / fpsInt + 0.5);
        int frameMs = 1000 / fpsInt;
        if (frameMs < 1)
            frameMs = 40;

        bool interlaced = false;
        switch (videoPar->field_order) {
        case AV_FIELD_TT:
        case AV_FIELD_BB:
        case AV_FIELD_TB:
        case AV_FIELD_BT:
            interlaced = true;
            break;
        default:
            break;
        }
        if (videoId == AV_CODEC_ID_H264 && videoPar->height > 0 && videoPar->height <= 576
            && videoPar->width > 0 && videoPar->width <= 720)
            interlaced = true;
        // DI on SD usually outputs ~50 pictures/s → 20ms pace. Archive used to judder from
        // EXTERNAL_PTS (disabled below), not from this pace — 40ms made archive slow-mo vs audio.
        int displayFrameMs = interlaced ? (frameMs > 1 ? frameMs / 2 : 20) : frameMs;
        if (displayFrameMs < 8)
            displayFrameMs = 8;

        std::vector<uint8_t> header;
        int nalLen = 4;
        bool lengthPrefixed = false;
        if (videoPar->extradata && videoPar->extradata_size > 0) {
            if (videoId == AV_CODEC_ID_H264 && videoPar->extradata[0] == 1) {
                lengthPrefixed = true;
                nalLen = (videoPar->extradata[4] & 0x03) + 1;
                header = avccToAnnexB(videoPar->extradata, videoPar->extradata_size);
            } else if (videoId == AV_CODEC_ID_HEVC && videoPar->extradata[0] == 1) {
                header = hvccToAnnexB(videoPar->extradata, videoPar->extradata_size);
                lengthPrefixed = !header.empty();
                nalLen = 4;
            } else if (startsAnnexB(videoPar->extradata, videoPar->extradata_size)) {
                header.assign(videoPar->extradata, videoPar->extradata + videoPar->extradata_size);
            }
        }

        int video10 = open("/dev/video10", O_RDONLY | O_NONBLOCK);
        if (video10 < 0) {
            logLine("open /dev/video10 failed %d", errno);
            notify(STB_EVENT_PLAY_ERROR);
            avformat_close_input(&fmt);
            playing.store(false);
            return;
        }

        codec_para_t video;
        memset(&video, 0, sizeof(video));
        video.handle = -1;
        video.cntl_handle = -1;
        video.sub_handle = -1;
        video.audio_utils_handle = -1;
        video.stream_type = STREAM_TYPE_ES_VIDEO;
        video.has_video = 1;
        video.has_audio = 0;
        video.noblock = 0;
        video.video_type = vformat;
        video.am_sysinfo.format = videoDecFormatFor(videoId);
        video.am_sysinfo.width = videoPar->width;
        video.am_sysinfo.height = videoPar->height;
        video.am_sysinfo.rate = rate;
        // Same EXTERNAL_PTS path as live. Archive stutter came from broken amvideo delay
        // (ready_frame_delayed ~900s) when PTS checkin was disabled — not from DI/pace.
        video.am_sysinfo.param = (void *)(uintptr_t)EXTERNAL_PTS;
        video.dec_mode = STREAM_TYPE_SINGLE;
        writeSysfs("/sys/module/amvdec_h264/parameters/dec_control", "4");
        writeSysfs("/sys/module/amvdec_h264/parameters/fixed_frame_rate_flag", "1");
        writeSysfs("/sys/module/amvdec_h265/parameters/force_fps", QByteArray::number(0x100 | fpsInt));
        writeSysfs("/sys/module/di/parameters/di_debug_flag", "0");
        writeSysfs("/sys/class/ppmgr/bypass", "0");
        writeSysfs("/sys/class/vfm/map", "rm default\n");
        if (interlaced) {
            writeSysfs("/sys/module/di/parameters/bypass_all", "0");
            writeSysfs("/sys/class/vfm/map", "add default decoder ppmgr amlvideo deinterlace amvideo\n");
        } else {
            writeSysfs("/sys/module/di/parameters/bypass_all", "1");
            writeSysfs("/sys/class/vfm/map", "add default decoder ppmgr amlvideo amvideo\n");
        }
        if (codec_init(&video) != 0) {
            writeSysfs("/sys/module/amvdec_h265/parameters/force_fps", "0");
            writeSysfs("/sys/module/amvdec_h264/parameters/fixed_frame_rate_flag", "0");
            logLine("codec_init video failed");
            close(video10);
            notify(STB_EVENT_PLAY_ERROR);
            avformat_close_input(&fmt);
            playing.store(false);
            return;
        }
        codec_pause(&video);
        codec_set_cntl_mode(&video, 0);
        writeSysfs("/sys/class/tsync/enable", "0");
        writeSysfs("/sys/module/amvideo/parameters/omx_run", "N");
        writeSysfs("/sys/class/video/freerun_mode", "1");
        codec_set_freerun_mode(&video, 1);
        writeSysfs("/sys/class/video/blackout_policy", "0");
        writeSysfs("/sys/class/video/disable_video", "0");
        writeSysfs("/sys/class/video/show_first_frame_nosync", "1");
        writeSysfs("/sys/class/video/slowsync_repeat_enable", "0");
        writeSysfs("/sys/class/video/trickmode_duration", "0");
        applyViewport();
        setPortalVisible(true);
        {
            QFile map("/sys/class/vfm/map");
            QByteArray line;
            if (map.open(QIODevice::ReadOnly))
                line = map.readLine().trimmed();
            logLine("video %dx%d fps %d rate %u %s pace %dms codec %d%s %s",
                videoPar->width, videoPar->height, fpsInt, rate,
                interlaced ? "interlace" : "progressive", displayFrameMs, (int)videoId,
                archive ? " archive" : "",
                line.constData());
        }

        AVCodecContext *audioDec = nullptr;
        SwrContext *resampler = nullptr;
        snd_pcm_t *pcm = nullptr;
        AVFrame *audioFrame = nullptr;
        if (audioIndex >= 0) {
            AVCodecParameters *audioPar = fmt->streams[audioIndex]->codecpar;
            AVCodec *decoder = avcodec_find_decoder(audioPar->codec_id);
            if (decoder) {
                audioDec = avcodec_alloc_context3(decoder);
                avcodec_parameters_to_context(audioDec, audioPar);
                if (avcodec_open2(audioDec, decoder, nullptr) == 0 && openAlsa(&pcm)) {
                    int64_t inLayout = audioPar->channel_layout
                        ? audioPar->channel_layout
                        : av_get_default_channel_layout(audioPar->channels ? audioPar->channels : 2);
                    resampler = swr_alloc_set_opts(nullptr, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, 48000,
                        inLayout, audioDec->sample_fmt, audioDec->sample_rate ? audioDec->sample_rate : 48000,
                        0, nullptr);
                    if (!resampler || swr_init(resampler) < 0) {
                        logLine("resampler failed");
                        if (resampler)
                            swr_free(&resampler);
                        snd_pcm_close(pcm);
                        pcm = nullptr;
                        avcodec_free_context(&audioDec);
                    } else {
                        audioFrame = av_frame_alloc();
                        logLine("audio %d Hz %d ch", audioPar->sample_rate, audioPar->channels);
                    }
                } else if (audioDec) {
                    avcodec_free_context(&audioDec);
                }
            }
        }

        if (fmt->duration > 0 && fmt->duration != (int64_t)AV_NOPTS_VALUE)
            duration.store((int)(fmt->duration / (AV_TIME_BASE / 1000)));
        else
            duration.store(0);

        std::atomic<int> audioClockMs{0};
        std::atomic<bool> audioReady{false};
        PlayShare share;
        share.running = &running;
        share.pause = &pauseRequested;
        share.volume = &volumeValue;
        share.mute = &muteValue;
        share.audioClockMs = &audioClockMs;
        share.audioReady = &audioReady;
        share.video = &video;
        share.header = &header;
        share.pcm = pcm;
        share.audioDec = audioDec;
        share.resampler = resampler;
        share.audioFrame = audioFrame;
        share.haveAudio = pcm != nullptr;
        share.video10 = video10;
        share.displayFrameMs = displayFrameMs;
        share.skipPtsCheckin = false;
        share.audioMasterDisplay = false;
        share.prefillPackets = archive ? 24 : 8;
        share.videoBytesLimit = archive ? (size_t)(2 * 1024 * 1024) : (size_t)(768 * 1024);
        share.sendHeader.store(!header.empty());

        std::thread videoThread(runVideo, &share);
        std::thread displayThread(runDisplay, &share);
        std::thread audioThread;
        bool audioThreadOn = false;
        if (share.haveAudio) {
            audioThread = std::thread(runAudio, &share);
            audioThreadOn = true;
        }

        playing.store(true);
        notify(STB_EVENT_PLAY_START);
        notify(STB_EVENT_GOT_INFO);
        notify(STB_EVENT_GOT_VIDEO_INFO);

        int64_t originMs = -1;
        int lastMs = -1;
        int videoPackets = 0;
        bool waitingKey = archive;
        std::vector<uint8_t> converted;
        AVPacket packet;
        memset(&packet, 0, sizeof(packet));
        av_init_packet(&packet);

        while (running.load()) {
            int seekMs = seekTarget.exchange(-1);
            if (seekMs >= 0) {
                share.resetVideo.store(true);
                share.resetAudio.store(true);
                {
                    std::lock_guard<std::mutex> lock(share.videoMu);
                    share.videoQ.clear();
                    share.videoPts.clear();
                    share.videoBytes = 0;
                }
                {
                    std::lock_guard<std::mutex> lock(share.audioMu);
                    share.audioQ.clear();
                }
                share.videoCv.notify_all();
                share.audioCv.notify_all();
                int64_t target = (int64_t)seekMs * (AV_TIME_BASE / 1000);
                av_seek_frame(fmt, -1, target, AVSEEK_FLAG_BACKWARD);
                originMs = -1;
                lastMs = -1;
                videoPackets = 0;
                waitingKey = archive;
                audioClockMs.store(0);
                audioReady.store(false);
            }

            int read = av_read_frame(fmt, &packet);
            if (read < 0) {
                if (!running.load())
                    break;
                if (read == AVERROR_EOF) {
                    notify(STB_EVENT_EOF);
                    break;
                }
                QThread::msleep(10);
                continue;
            }

            if (packet.stream_index == videoIndex) {
                const uint8_t *data = packet.data;
                int size = packet.size;
                if (lengthPrefixed && !startsAnnexB(data, size)) {
                    converted = lengthPrefixedToAnnexB(data, size, nalLen);
                    data = converted.data();
                    size = (int)converted.size();
                }
                if (waitingKey) {
                    if (!annexBHasKeyframe(data, size, videoId) && !(packet.flags & AV_PKT_FLAG_KEY)) {
                        av_packet_unref(&packet);
                        continue;
                    }
                    waitingKey = false;
                    logLine("archive keyframe start");
                }
                // Archive container PTS are often broadcast-absolute/jumpy — feed monotonic
                // EXTERNAL_PTS like a clean live clock so amvideo does not report ~900s delay.
                int ptsMs;
                if (archive) {
                    ptsMs = videoPackets * frameMs;
                } else {
                    ptsMs = lastMs >= 0 ? lastMs + frameMs : 0;
                    if (packet.pts != AV_NOPTS_VALUE) {
                        int64_t raw = av_rescale_q(packet.pts, videoStream->time_base, AVRational{1, 1000});
                        if (originMs < 0)
                            originMs = raw;
                        int rel = (int)(raw - originMs);
                        if (rel < 0)
                            rel = 0;
                        if (lastMs >= 0 && rel < lastMs)
                            rel = lastMs;
                        if (lastMs >= 0 && rel > lastMs + 1000)
                            rel = lastMs + frameMs;
                        ptsMs = rel;
                    }
                }
                position.store(ptsMs);
                lastMs = ptsMs;
                if (size > 0) {
                    std::vector<uint8_t> copy(data, data + size);
                    std::unique_lock<std::mutex> lock(share.videoMu);
                    while (running.load() && share.videoBytes >= share.videoBytesLimit)
                        share.videoCv.wait_for(lock, std::chrono::milliseconds(50));
                    if (running.load()) {
                        share.videoBytes += copy.size();
                        share.videoPts.push_back(ptsMs);
                        share.videoQ.push_back(std::move(copy));
                        lock.unlock();
                        share.videoCv.notify_all();
                    }
                }
                videoPackets++;
                if (videoPackets == 1 || videoPackets == 50)
                    logLine("video packets %d", videoPackets);
            } else if (packet.stream_index == audioIndex && share.haveAudio && packet.size > 0) {
                if (waitingKey) {
                    av_packet_unref(&packet);
                    continue;
                }
                std::vector<uint8_t> copy(packet.data, packet.data + packet.size);
                std::unique_lock<std::mutex> lock(share.audioMu);
                while (running.load() && share.audioQ.size() >= 48)
                    share.audioCv.wait_for(lock, std::chrono::milliseconds(50));
                if (running.load()) {
                    share.audioQ.push_back(std::move(copy));
                    lock.unlock();
                    share.audioCv.notify_all();
                }
            }
            av_packet_unref(&packet);
        }

        share.videoDone.store(true);
        share.audioDone.store(true);
        share.videoCv.notify_all();
        share.audioCv.notify_all();
        videoThread.join();
        displayThread.join();
        if (audioThreadOn)
            audioThread.join();

        playing.store(false);
        writeSysfs("/sys/module/amvdec_h265/parameters/force_fps", "0");
        writeSysfs("/sys/module/amvdec_h264/parameters/fixed_frame_rate_flag", "0");
        writeSysfs("/sys/class/video/disable_video", "1");
        writeSysfs("/sys/class/tsync/enable", "1");
        setPortalVisible(true);
        codec_close(&video);
        close(video10);
        if (pcm)
            snd_pcm_close(pcm);
        if (resampler)
            swr_free(&resampler);
        if (audioFrame)
            av_frame_free(&audioFrame);
        if (audioDec)
            avcodec_free_context(&audioDec);
        avformat_close_input(&fmt);
        logLine("play stopped");
    }
};


HwPlayer::HwPlayer(QObject *parent)
    : QObject(parent)
    , engine(new Engine)
{
    engine->owner = this;
    avformat_network_init();
    writeSysfs("/sys/class/video/disable_video", "1");
    setPortalVisible(true);
}

HwPlayer::~HwPlayer()
{
    commandStop();
    delete engine;
    avformat_network_deinit();
}

void HwPlayer::relay(int code)
{
    emit playbackEvent(code);
}

bool HwPlayer::commandPlay(const QString &playStr, const QString &proxyParams)
{
    Q_UNUSED(proxyParams);
    commandStop();
    QString proto;
    QString url = normalizeUrl(playStr, &proto);
    if (url.isEmpty()) {
        emit playbackEvent(STB_EVENT_PLAY_ERROR);
        return false;
    }
    engine->url = url;
    engine->running.store(true);
    engine->pauseRequested.store(false);
    engine->position.store(0);
    engine->duration.store(0);
    engine->seekTarget.store(-1);
    QObject::disconnect(&engine->thread, &QThread::started, nullptr, nullptr);
    QObject::connect(&engine->thread, &QThread::started, this, [this]() {
        engine->loop();
        engine->thread.quit();
    }, Qt::DirectConnection);
    engine->thread.start();
    return true;
}

void HwPlayer::commandStop()
{
    if (!engine->thread.isRunning() && !engine->running.load())
        return;
    engine->running.store(false);
    engine->thread.wait(8000);
    engine->playing.store(false);
    writeSysfs("/sys/class/video/disable_video", "1");
    setPortalVisible(true);
}

void HwPlayer::commandPause()
{
    engine->pauseRequested.store(true);
}

void HwPlayer::commandContinue()
{
    engine->pauseRequested.store(false);
}

bool HwPlayer::isPlaying() const
{
    return engine->playing.load();
}

int HwPlayer::positionMs() const
{
    return engine->position.load();
}

int HwPlayer::positionSeconds() const
{
    return positionMs() / 1000;
}

int HwPlayer::durationMs() const
{
    return engine->duration.load();
}

int HwPlayer::durationSeconds() const
{
    return durationMs() / 1000;
}

int HwPlayer::positionPercent() const
{
    int total = durationMs();
    if (total <= 0)
        return 0;
    return positionMs() * 100 / total;
}

void HwPlayer::seekSeconds(int seconds)
{
    if (seconds < 0)
        seconds = 0;
    engine->seekTarget.store(seconds * 1000);
}

void HwPlayer::setVolume(int value)
{
    if (value < 0)
        value = 0;
    if (value > 100)
        value = 100;
    engine->volumeValue.store(value);
}

int HwPlayer::volume() const
{
    return engine->volumeValue.load();
}

void HwPlayer::setMuted(bool muted)
{
    engine->muteValue.store(muted);
}

bool HwPlayer::muted() const
{
    return engine->muteValue.load();
}

void HwPlayer::setViewport(int width, int height, int x, int y)
{
    engine->viewX = x;
    engine->viewY = y;
    engine->viewW = width;
    engine->viewH = height;
    engine->applyViewport();
}

void HwPlayer::viewport(int *width, int *height, int *x, int *y) const
{
    if (width)
        *width = engine->viewW;
    if (height)
        *height = engine->viewH;
    if (x)
        *x = engine->viewX;
    if (y)
        *y = engine->viewY;
}

extern "C" int amvideo_utils_set_virtual_position(int x, int y, int w, int h, int)
{
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    char line[80];
    snprintf(line, sizeof(line), "%d %d %d %d", x, y, x + w - 1, y + h - 1);
    int fd = open("/sys/class/video/axis", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t wrote = write(fd, line, strlen(line));
        (void)wrote;
        close(fd);
    }
    return 0;
}

#define YASEM_AUDIO_STUB(name) extern "C" int name() { return 0; }
YASEM_AUDIO_STUB(audio_channel_left_mono)
YASEM_AUDIO_STUB(audio_channel_lrmix_flag_set)
YASEM_AUDIO_STUB(audio_channel_right_mono)
YASEM_AUDIO_STUB(audio_channel_stereo)
YASEM_AUDIO_STUB(audio_channels_swap)
YASEM_AUDIO_STUB(audio_dec_ready)
YASEM_AUDIO_STUB(audio_decode_automute)
YASEM_AUDIO_STUB(audio_decode_basic_init)
YASEM_AUDIO_STUB(audio_decode_get_lrvolume)
YASEM_AUDIO_STUB(audio_decode_get_pre_gain)
YASEM_AUDIO_STUB(audio_decode_get_pre_mute)
YASEM_AUDIO_STUB(audio_decode_get_volume)
YASEM_AUDIO_STUB(audio_decode_init)
YASEM_AUDIO_STUB(audio_decode_pause)
YASEM_AUDIO_STUB(audio_decode_release)
YASEM_AUDIO_STUB(audio_decode_resume)
YASEM_AUDIO_STUB(audio_decode_set_lrvolume)
YASEM_AUDIO_STUB(audio_decode_set_mute)
YASEM_AUDIO_STUB(audio_decode_set_pre_gain)
YASEM_AUDIO_STUB(audio_decode_set_pre_mute)
YASEM_AUDIO_STUB(audio_decode_set_volume)
YASEM_AUDIO_STUB(audio_decode_stop)
YASEM_AUDIO_STUB(audio_decoder_get_enable_status)
YASEM_AUDIO_STUB(audio_decoder_set_trackrate)
YASEM_AUDIO_STUB(audio_decpara_get)
YASEM_AUDIO_STUB(audio_get_decoded_nb_frames)
YASEM_AUDIO_STUB(audio_get_decoded_pcm_delay)
YASEM_AUDIO_STUB(audio_get_pcm_level)
YASEM_AUDIO_STUB(audio_get_pts)
YASEM_AUDIO_STUB(audio_get_soundtrack)
YASEM_AUDIO_STUB(audio_output_muted)
YASEM_AUDIO_STUB(audio_set_av_sync_threshold)
YASEM_AUDIO_STUB(audio_set_skip_bytes)
YASEM_AUDIO_STUB(get_audio_decoder)
YASEM_AUDIO_STUB(get_decoder_status)
#undef YASEM_AUDIO_STUB
