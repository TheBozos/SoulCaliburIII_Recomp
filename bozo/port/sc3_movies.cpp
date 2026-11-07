// Sofdec answered at mwPly's level. Movies of MOVIE.AFS (partition 1) are decoded here with FFmpeg, on the game's frames, and
// Sofdec's own player is never started: full-screen ones are the renderer's held backdrop, smaller ones (menu previews) a
// picture the 2D builders ask for by handle, their ADX soundtracks go to the mixer through the port's PCM stream. Any other
// start reports its end straight away.
#include "sc3_draw.h"

#include "bozo/audio/port_stream.h"
#include "bozo/libs/cd.h"
#include "bozo/libs/libraries.h"
#include "bozo/render/renderer.h"


#include <cstdlib>
#include <cstring>

#ifdef SC3_HAVE_FFMPEG
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

namespace sc3::movies
{
    namespace
    {
        constexpr uint32_t kMwPlyCreate = 0x003AA698u;
        constexpr uint32_t kMwPlyDestroy = 0x003AAC38u;
        constexpr uint32_t kMwPlyPause = 0x003AD8F0u;
        constexpr uint32_t kMwPlyIsPause = 0x003AD300u;
        // Set on a decoder the game never gets: its volume, an output setting, a memory area, a frame's information.
        constexpr uint32_t kMwPlyDecoderSettings[] = {0x003AEF20u, 0x003B0178u, 0x003A8A98u, 0x003B3AD0u};

        constexpr uint32_t kMwPlySlots = 0x0054D6C4u;
        constexpr uint32_t kMwPlySlotCount = 8u;
        constexpr uint32_t kMwPlySlotSize = 0x304u;
        constexpr uint32_t kMwPlyParameters = 0x08u;
        constexpr uint32_t kMwPlyParametersSize = 0x30u;
        constexpr uint32_t kMwPlyPaused = 0x40u;
        constexpr uint32_t kMwPlyLastEntry = 0x2FCu;
        constexpr uint32_t kFrameObjectSize = 0x90u;
        constexpr uint32_t kMwPlyStartAfs = 0x003ADA20u;
        constexpr uint32_t kMwPlyStartAfsLp = 0x003B1698u;
        constexpr uint32_t kMwPlyGetCurFrm = 0x003AB630u;
        constexpr uint32_t kMwPlyRelCurFrm = 0x003ABF50u;
        constexpr uint32_t kMwPlyIsNextFrmReady = 0x003AC008u;
        constexpr uint32_t kMwPlyStop = 0x003AD870u;
        constexpr uint32_t kMwPlyStarts[] = {0x003AD4B0u, 0x003AD5E8u, 0x003AD6E8u, 0x003ADA20u, 0x003ADBD0u,
                                             0x003B1328u, 0x003B1428u, 0x003B1698u, 0x003B17E0u, 0x003B2DF0u};
        constexpr uint32_t kMwPlyGetStat = 0x003AE9F8u;
        constexpr int32_t kMwPlyStatPlayEnd = 3;
        constexpr uint32_t kMoviePlayStat = 0x003AE988u;
        constexpr int32_t kMoviePlayStatFinished = 4;
        constexpr uint32_t kMovieTaskEnd = 0x00218600u;
        constexpr uint32_t kMovieStatePointer = 0x70000030u;

        uint32_t readWord(PS2Runtime *runtime, uint32_t address)
        {
            uint32_t word = 0;
            if (const uint8_t *p = runtime->memory.hostPointer(address, 4))
                std::memcpy(&word, p, 4);
            return word;
        }

        // The task's end leaves the job it runs for marked playing unless the job is released here.
        void taskEndReleasesJob(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t movieState = readWord(runtime, kMovieStatePointer);
            if (movieState && readWord(runtime, movieState + 8u) == getRegU32(ctx, 4))
                if (uint8_t *p = runtime->memory.hostPointer(movieState + 8u, 4))
                    std::memset(p, 0, 4);
            func_0x218600(rdram, ctx, runtime);
        }

#ifdef SC3_HAVE_FFMPEG
        PS2Runtime *g_runtime = nullptr;

        constexpr int32_t kMwPlyStatPlaying = 2;
        constexpr uint32_t kMovieAfsPartition = 1u;
        constexpr uint32_t kMwPlyCompoMode = 0x08u;
        constexpr uint32_t kMwPlyCompoAlpha = 3u;
        constexpr uint32_t kMwPlyWork = 0x20u;

        constexpr uint32_t kRate = 48000u;
        constexpr uint32_t kFullScreenWidth = 512u;
        constexpr uint64_t kBackdropKey = 0x5C3B4C4Bu;
        constexpr double kPictureTime = 1001.0 / 30000.0;
        constexpr double kGameFrameTime = 1001.0 / 60000.0;
        constexpr uint32_t kSector = 2048u;

        struct Picture
        {
            double time = 0.0;
            uint32_t width = 0, height = 0;
            std::vector<uint8_t> rgba;
        };

        class SfdPlayer
        {
        public:
            ~SfdPlayer() { stop(); }

            bool start(const std::string &iso, uint32_t file, bool loop)
            {
                std::string error;
                uint32_t lbn = 0, bytes = 0;
                std::vector<uint8_t> head(kSector);
                if (!m_disc.open(iso, error) || !m_disc.findFile("MOVIE.AFS", lbn, bytes) || !m_disc.read(lbn, 1, head.data()) ||
                    std::memcmp(head.data(), "AFS", 4) != 0)
                    return false;
                uint32_t files = 0, entry[2] = {};
                std::memcpy(&files, head.data() + 4, 4);
                const uint32_t at = 8u + 8u * file;
                const uint32_t sectors = (at + 8u + kSector - 1u) / kSector;
                head.resize(sectors * kSector);
                if (file >= files || !m_disc.read(lbn, sectors, head.data()))
                    return false;
                std::memcpy(entry, head.data() + at, 8);
                if (entry[1] == 0u)
                    return false;
                m_base = static_cast<uint64_t>(lbn) * kSector + entry[0];
                m_size = entry[1];
                m_file = file;
                m_loop = loop;
                m_thread = std::thread([this] { decode(); });
                return true;
            }

            void stop()
            {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_quit = true;
                    m_active = false;
                    clearBackdropLocked();
                }
                m_wake.notify_all();
                if (m_thread.joinable())
                    m_thread.join();
                report();
            }

            // At the frame ends: the clock moves on, the picture of that time is the current one.
            void update()
            {
                const double now = static_cast<double>(++m_ticks) * kGameFrameTime;
                std::unique_lock<std::mutex> lock(m_mutex);
                if (!m_active)
                    return;
                m_wake.wait(lock, [&] { return m_quit || m_decoderDone || fullLocked(); });
                m_now = now;
                advanceLocked();
                if (m_fullScreen)
                    showBackdropLocked();
                updateEndLocked();
            }

            // Mixes up to `count` frames of its soundtrack into `out`; the frames it had.
            size_t mixAudio(int32_t *out, size_t count)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_active)
                    return 0;
                const size_t available = std::min(count, m_audio.size() / 2u);
                for (size_t i = 0; i < available * 2u; ++i)
                    out[i] += m_audio[i];
                m_audio.erase(m_audio.begin(), m_audio.begin() + static_cast<std::ptrdiff_t>(available * 2u));
                updateEndLocked();
                m_wake.notify_all();
                return available;
            }

            bool active()
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                updateEndLocked();
                return m_active;
            }

            bool frameInfo(bool alphaMovie, uint32_t &width, uint32_t &height, uint32_t &number)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_active || m_fullScreen || m_current.empty())
                    return false;
                width = m_width;
                height = alphaMovie ? m_height / 2u : m_height;
                number = m_number;
                return true;
            }

            // RGBA rows, alpha 0x80 = 1.0 or a mask movie's luma from the picture's bottom half. Nothing is copied while `known` is
            // still the current picture's number.
            bool picture(bool alphaMovie, uint32_t known, std::vector<uint32_t> &rgba, uint32_t &width, uint32_t &height, uint32_t &number)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_active || m_fullScreen || m_current.empty())
                    return false;
                const uint32_t rows = alphaMovie ? m_height / 2u : m_height;
                width = m_width;
                height = rows;
                number = m_number;
                if (known == m_number)
                    return true;
                rgba.resize(static_cast<size_t>(m_width) * rows);
                for (uint32_t y = 0; y < rows; ++y)
                    for (uint32_t x = 0; x < m_width; ++x)
                    {
                        const uint8_t *src = m_current.data() + (static_cast<size_t>(y) * m_width + x) * 4u;
                        const uint8_t *mask = src + static_cast<size_t>(rows) * m_width * 4u;
                        const uint32_t luma = (mask[0] * 77u + mask[1] * 150u + mask[2] * 29u) >> 8;
                        const uint32_t alpha = alphaMovie ? (luma * 0x80u + 127u) / 255u : 0x80u;
                        rgba[static_cast<size_t>(y) * m_width + x] = src[0] | (src[1] << 8) | (src[2] << 16) | (alpha << 24);
                    }
                return true;
            }

        private:
            void report()
            {
                if (m_reported)
                    return;
                m_reported = true;
                std::fprintf(stderr, "[sc3:movies] file %u %ux%u%s%s: %u pictures, clock %.2f s (%.2f/s)\n", m_file, m_width, m_height,
                             m_fullScreen ? " full screen" : " texture", m_loop ? " loop" : "", m_number, m_now,
                             m_now > 0.0 ? m_number / m_now : 0.0);
            }

            bool fullLocked() const
            {
                const size_t audioFrames = m_audio.size() / 2u;
                return (m_frames.size() >= 8u && (!m_hasAudio || audioFrames >= kRate / 2u)) || m_frames.size() >= 48u || audioFrames >= kRate * 8u;
            }

            void advanceLocked()
            {
                bool popped = false;
                while (!m_frames.empty() && m_frames.front().time <= m_now)
                {
                    m_current.swap(m_frames.front().rgba);
                    m_width = m_frames.front().width;
                    m_height = m_frames.front().height;
                    m_frames.pop_front();
                    ++m_number;
                    popped = true;
                }
                if (popped)
                    m_wake.notify_all();
            }

            void updateEndLocked()
            {
                if (m_active && m_decoderDone && m_frames.empty() && m_audio.empty())
                {
                    m_active = false;
                    clearBackdropLocked();
                }
            }

            void showBackdropLocked()
            {
                bozo::render::Renderer &renderer = bozo::render::Renderer::instance();
                if (!renderer.enabled() || (m_backdropShown && m_backdropNumber == m_number))
                    return;
                auto picture = std::make_shared<bozo::render::Texture>();
                picture->key = kBackdropKey;
                picture->version = ++m_backdropVersion;
                picture->width = m_current.empty() ? 640u : m_width;
                picture->height = m_current.empty() ? 448u : m_height;
                picture->rgba.assign(static_cast<size_t>(picture->width) * picture->height, 0xFF000000u);
                if (!m_current.empty())
                {
                    std::memcpy(picture->rgba.data(), m_current.data(), picture->rgba.size() * 4u);
                    for (uint32_t &c : picture->rgba)
                        c |= 0xFF000000u;
                }
                bozo::render::Screen screen;
                screen.originX = 0.0f;
                screen.originY = 0.0f;
                screen.width = static_cast<float>(picture->width);
                screen.height = static_cast<float>(picture->height);
                renderer.setBackdrop(std::move(picture), screen, 0.0f, 0.0f, screen.width, screen.height, true);
                m_backdropShown = true;
                m_backdropNumber = m_number;
            }

            void clearBackdropLocked()
            {
                if (!m_backdropShown)
                    return;
                m_backdropShown = false;
                bozo::render::Renderer::instance().clearBackdrop();
            }

            static int readPacket(void *opaque, uint8_t *buffer, int size)
            {
                auto *self = static_cast<SfdPlayer *>(opaque);
                const uint64_t left = self->m_size - self->m_position;
                if (left == 0u)
                    return AVERROR_EOF;
                const uint64_t at = self->m_base + self->m_position;
                const uint32_t skip = static_cast<uint32_t>(at % kSector);
                const uint32_t wanted = static_cast<uint32_t>(std::min<uint64_t>(left, static_cast<uint64_t>(size)));
                const uint32_t sectors = (skip + wanted + kSector - 1u) / kSector;
                self->m_read.resize(static_cast<size_t>(sectors) * kSector);
                if (!self->m_disc.read(static_cast<uint32_t>(at / kSector), sectors, self->m_read.data()))
                    return AVERROR_EOF;
                std::memcpy(buffer, self->m_read.data() + skip, wanted);
                self->m_position += wanted;
                return static_cast<int>(wanted);
            }

            static int64_t seekPacket(void *opaque, int64_t offset, int whence)
            {
                auto *self = static_cast<SfdPlayer *>(opaque);
                if (whence == AVSEEK_SIZE)
                    return static_cast<int64_t>(self->m_size);
                int64_t target = offset;
                if ((whence & ~AVSEEK_FORCE) == SEEK_CUR)
                    target += static_cast<int64_t>(self->m_position);
                else if ((whence & ~AVSEEK_FORCE) == SEEK_END)
                    target += static_cast<int64_t>(self->m_size);
                if (target < 0 || static_cast<uint64_t>(target) > self->m_size)
                    return -1;
                self->m_position = static_cast<uint64_t>(target);
                return target;
            }

            bool waitForRoom()
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_wake.notify_all();
                m_wake.wait(lock, [this] { return m_quit || !fullLocked(); });
                return !m_quit;
            }

            void decodePass()
            {
                m_position = 0;
                AVFormatContext *format = avformat_alloc_context();
                auto *ioBuffer = static_cast<uint8_t *>(av_malloc(64u * 1024u));
                AVIOContext *io = avio_alloc_context(ioBuffer, 64 * 1024, 0, this, &SfdPlayer::readPacket, nullptr, &SfdPlayer::seekPacket);
                format->pb = io;
                AVCodecContext *video = nullptr, *audio = nullptr;
                SwsContext *scaler = nullptr;
                SwrContext *resampler = nullptr;
                int videoIndex = -1, audioIndex = -1;
                if (avformat_open_input(&format, nullptr, nullptr, nullptr) == 0 && avformat_find_stream_info(format, nullptr) >= 0)
                {
                    for (unsigned i = 0; i < format->nb_streams; ++i)
                    {
                        const AVCodecParameters *parameters = format->streams[i]->codecpar;
                        if (parameters->codec_type == AVMEDIA_TYPE_VIDEO && videoIndex < 0)
                            videoIndex = static_cast<int>(i);
                        else if (parameters->codec_id == AV_CODEC_ID_ADPCM_ADX && audioIndex < 0)
                            audioIndex = static_cast<int>(i);
                    }
                    const auto openDecoder = [&](int index) -> AVCodecContext *
                    {
                        if (index < 0)
                            return nullptr;
                        const AVCodecParameters *parameters = format->streams[index]->codecpar;
                        const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
                        AVCodecContext *context = codec ? avcodec_alloc_context3(codec) : nullptr;
                        if (context && (avcodec_parameters_to_context(context, parameters) < 0 || avcodec_open2(context, codec, nullptr) < 0))
                            avcodec_free_context(&context);
                        return context;
                    };
                    video = openDecoder(videoIndex);
                    audio = openDecoder(audioIndex);
                    if (audio)
                    {
                        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                        if (swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_S16, kRate, &audio->ch_layout, audio->sample_fmt,
                                                audio->sample_rate, 0, nullptr) < 0 ||
                            swr_init(resampler) < 0)
                            swr_free(&resampler);
                    }
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_hasAudio = resampler != nullptr;
                    if (video)
                        m_fullScreen = static_cast<uint32_t>(video->width) >= kFullScreenWidth;
                }
                AVPacket *packet = av_packet_alloc();
                AVFrame *frame = av_frame_alloc();
                const auto drainVideo = [&]
                {
                    while (avcodec_receive_frame(video, frame) == 0)
                    {
                        scaler = sws_getCachedContext(scaler, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), frame->width,
                                                      frame->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                        Picture out;
                        out.width = static_cast<uint32_t>(frame->width);
                        out.height = static_cast<uint32_t>(frame->height);
                        out.rgba.resize(static_cast<size_t>(out.width) * out.height * 4u);
                        uint8_t *planes[1] = {out.rgba.data()};
                        const int strides[1] = {static_cast<int>(out.width * 4u)};
                        sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, planes, strides);
                        out.time = static_cast<double>(m_decoded++) * kPictureTime;
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_frames.push_back(std::move(out));
                        m_wake.notify_all();
                    }
                };
                const auto drainAudio = [&]
                {
                    while (avcodec_receive_frame(audio, frame) == 0)
                    {
                        const int capacity = swr_get_out_samples(resampler, frame->nb_samples);
                        std::vector<int16_t> pcm(static_cast<size_t>(std::max(capacity, 0)) * 2u);
                        uint8_t *planes[1] = {reinterpret_cast<uint8_t *>(pcm.data())};
                        const int produced = swr_convert(resampler, planes, capacity, const_cast<const uint8_t **>(frame->extended_data), frame->nb_samples);
                        if (produced <= 0)
                            continue;
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_audio.insert(m_audio.end(), pcm.begin(), pcm.begin() + produced * 2);
                    }
                };
                while (video && waitForRoom() && av_read_frame(format, packet) >= 0)
                {
                    if (packet->stream_index == videoIndex && avcodec_send_packet(video, packet) >= 0)
                        drainVideo();
                    else if (resampler && packet->stream_index == audioIndex && avcodec_send_packet(audio, packet) >= 0)
                        drainAudio();
                    av_packet_unref(packet);
                }
                if (video && !m_quit)
                {
                    avcodec_send_packet(video, nullptr);
                    drainVideo();
                }
                if (!video)
                    std::fprintf(stderr, "[sc3:movies] file %u: cannot decode the SFD\n", m_file);
                av_frame_free(&frame);
                av_packet_free(&packet);
                sws_freeContext(scaler);
                swr_free(&resampler);
                avcodec_free_context(&video);
                avcodec_free_context(&audio);
                avformat_close_input(&format);
                if (io)
                {
                    av_freep(&io->buffer);
                    avio_context_free(&io);
                }
            }

            void decode()
            {
                do
                {
                    const uint64_t before = m_decoded;
                    decodePass();
                    if (m_decoded == before)
                        break;
                } while (m_loop && !m_quit);
                std::lock_guard<std::mutex> lock(m_mutex);
                m_decoderDone = true;
                m_wake.notify_all();
            }

            bozo::libs::DiscImage m_disc;
            std::vector<uint8_t> m_read;
            std::thread m_thread;
            std::mutex m_mutex;
            std::condition_variable m_wake;
            uint64_t m_base = 0, m_size = 0, m_position = 0, m_decoded = 0, m_ticks = 0, m_backdropVersion = 0;
            double m_now = 0.0;
            std::deque<Picture> m_frames;
            std::deque<int16_t> m_audio;
            std::vector<uint8_t> m_current;
            uint32_t m_width = 0, m_height = 0, m_number = 0, m_file = 0, m_backdropNumber = 0;
            bool m_loop = false, m_fullScreen = false, m_decoderDone = false, m_hasAudio = false, m_backdropShown = false, m_reported = false, m_active = true;
            std::atomic<bool> m_quit{false};
        };

        std::map<uint32_t, std::shared_ptr<SfdPlayer>> g_players;
        uint64_t g_audioTicks = 0, g_audioPushed = 0;

        std::shared_ptr<SfdPlayer> find(uint32_t handle)
        {
            const auto it = g_players.find(handle);
            return it != g_players.end() ? it->second : nullptr;
        }

        // What the soundtracks owe the stream by the clock, plus a lead that outlasts a slow frame on the host's clock and the
        // mixer's block in a deterministic run. Silence is not pushed: the stream adds nothing when it has none.
        void pushAudio(PS2Runtime *runtime, bool realTime)
        {
            ++g_audioTicks;
            const double now = static_cast<double>(g_audioTicks) * kGameFrameTime;
            const uint64_t target = static_cast<uint64_t>(now * kRate) + (realTime ? kRate / 5u : kRate / 60u);
            if (target <= g_audioPushed)
                return;
            const size_t count = static_cast<size_t>(std::min<uint64_t>(target - g_audioPushed, kRate / 2u));
            std::vector<int32_t> sum(count * 2u, 0);
            size_t frames = 0;
            for (const auto &entry : g_players)
                frames = std::max(frames, entry.second->mixAudio(sum.data(), count));
            std::vector<int16_t> pcm(frames * 2u);
            for (size_t i = 0; i < pcm.size(); ++i)
                pcm[i] = static_cast<int16_t>(std::clamp(sum[i], -32768, 32767));
            bozo::port::audioStream(runtime).push(pcm.data(), frames);
            g_audioPushed += count;
        }

        void stopPlayer(uint32_t handle)
        {
            const auto it = g_players.find(handle);
            if (it == g_players.end())
                return;
            const std::shared_ptr<SfdPlayer> player = std::move(it->second);
            g_players.erase(it);
            player->stop();
        }

        void start(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            stopPlayer(getRegU32(ctx, 4));
            ctx->pc = getRegU32(ctx, 31);
        }

        void startFromAfs(R5900Context *ctx, PS2Runtime *runtime, bool loop)
        {
            const uint32_t handle = getRegU32(ctx, 4);
            stopPlayer(handle);
            static const bool noOneShots = std::getenv("SC3_NO_MOVIES") != nullptr;
            if (getRegU32(ctx, 5) == kMovieAfsPartition && (loop || !noOneShots))
            {
                auto player = std::make_shared<SfdPlayer>();
                if (player->start(bozo::port::libraries(runtime).cd.disc.path(), getRegU32(ctx, 6), loop))
                {
                    g_players[handle] = std::move(player);
                    std::fprintf(stderr, "[sc3:movies] handle 0x%x plays file %u\n", handle, getRegU32(ctx, 6));
                }
            }
            ctx->pc = getRegU32(ctx, 31);
        }

        void startAfs(uint8_t *, R5900Context *ctx, PS2Runtime *runtime) { startFromAfs(ctx, runtime, false); }
        void startAfsLp(uint8_t *, R5900Context *ctx, PS2Runtime *runtime) { startFromAfs(ctx, runtime, true); }

        void getStat(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const auto player = find(getRegU32(ctx, 4));
            setReturnS32(ctx, player && player->active() ? kMwPlyStatPlaying : kMwPlyStatPlayEnd);
            ctx->pc = getRegU32(ctx, 31);
        }

        void playStat(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const auto player = find(getRegU32(ctx, 4));
            setReturnS32(ctx, !player ? kMoviePlayStatFinished : player->active() ? kMwPlyStatPlaying : kMwPlyStatPlayEnd);
            ctx->pc = getRegU32(ctx, 31);
        }

        // The game takes a frame object with no pixels: the pictures go to the renderer by handle (picture below), never
        // through its GIF image packets. The object's buffer is the player's unused work area.
        void getCurFrm(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t handle = getRegU32(ctx, 4);
            const auto player = find(handle);
            uint8_t *object = runtime->memory.hostPointer(getRegU32(ctx, 5), kFrameObjectSize);
            if (object)
                std::memset(object, 0, kFrameObjectSize);
            uint32_t width = 0, height = 0, number = 0;
            const uint32_t buffer = (readWord(runtime, handle + kMwPlyWork) + 63u) & ~63u;
            if (object && player && player->frameInfo(readWord(runtime, handle + kMwPlyCompoMode) == kMwPlyCompoAlpha, width, height, number))
            {
                const uint32_t fields[][2] = {{0x00u, buffer}, {0x04u, number}, {0x0Cu, width}, {0x10u, height}, {0x14u, width}, {0x18u, height}, {0x24u, number}};
                for (const auto &field : fields)
                    std::memcpy(object + field[0], &field[1], 4);
            }
            ctx->pc = getRegU32(ctx, 31);
        }

        void isNextFrmReady(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            setReturnS32(ctx, find(getRegU32(ctx, 4)) ? 1 : 0);
            ctx->pc = getRegU32(ctx, 31);
        }

        void stopCall(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            stopPlayer(getRegU32(ctx, 4));
            ctx->pc = getRegU32(ctx, 31);
        }
#else
        void stopPlayer(uint32_t) {}

        void start(uint8_t *, R5900Context *ctx, PS2Runtime *) { ctx->pc = getRegU32(ctx, 31); }

        void getCurFrm(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (uint8_t *object = runtime->memory.hostPointer(getRegU32(ctx, 5), kFrameObjectSize))
                std::memset(object, 0, kFrameObjectSize);
            ctx->pc = getRegU32(ctx, 31);
        }

        void isNextFrmReady(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            setReturnS32(ctx, 0);
            ctx->pc = getRegU32(ctx, 31);
        }

        void getStat(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            setReturnS32(ctx, kMwPlyStatPlayEnd);
            ctx->pc = getRegU32(ctx, 31);
        }

        void playStat(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            setReturnS32(ctx, kMoviePlayStatFinished);
            ctx->pc = getRegU32(ctx, 31);
        }
#endif

        void answered(uint8_t *, R5900Context *ctx, PS2Runtime *) { ctx->pc = getRegU32(ctx, 31); }

        // A handle holds what the game gave and the overrides read: no decoder is ever made behind it, which is what
        // keeps Sofdec's own create and destroy, with their timer and IPU set-up, from being reached.
        void create(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t parameters = getRegU32(ctx, 4);
            uint32_t created = 0u;
            for (uint32_t i = 0u; parameters != 0u && i < kMwPlySlotCount && created == 0u; ++i)
            {
                const uint32_t handle = kMwPlySlots + i * kMwPlySlotSize;
                uint32_t used = 0u;
                std::memcpy(&used, rdram + handle, 4);
                if (used == 1u)
                    continue;
                std::memset(rdram + handle, 0, kMwPlySlotSize);
                std::memcpy(rdram + handle + kMwPlyParameters, rdram + parameters, kMwPlyParametersSize);
                const uint32_t usedNow = 1u, noEntry = ~0u;
                std::memcpy(rdram + handle, &usedNow, 4);
                std::memcpy(rdram + handle + kMwPlyLastEntry, &noEntry, 4);
                created = handle;
            }
            setReturnU32(ctx, created);
            ctx->pc = getRegU32(ctx, 31);
        }

        void destroy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t handle = getRegU32(ctx, 4);
            stopPlayer(handle);
            if (handle != 0u)
                std::memset(rdram + handle, 0, kMwPlySlotSize);
            ctx->pc = getRegU32(ctx, 31);
        }

        void pause(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t paused = getRegU32(ctx, 5);
            std::memcpy(rdram + getRegU32(ctx, 4) + kMwPlyPaused, &paused, 4);
            ctx->pc = getRegU32(ctx, 31);
        }

        void isPause(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            uint32_t paused = 0u;
            std::memcpy(&paused, rdram + getRegU32(ctx, 4) + kMwPlyPaused, 4);
            setReturnU32(ctx, paused);
            ctx->pc = getRegU32(ctx, 31);
        }
    }

    void tick(PS2Runtime *runtime)
    {
#ifdef SC3_HAVE_FFMPEG
        if (g_players.empty())
        {
            g_audioTicks = g_audioPushed = 0;
            return;
        }
        g_runtime = runtime;
        const bool realTime = bozo::port::host(runtime).realTime();
        for (const auto &entry : g_players)
            entry.second->update();
        pushAudio(runtime, realTime);
#else
        (void)runtime;
#endif
    }

    bool picture(PS2Runtime *runtime, uint32_t handle, uint32_t known, std::vector<uint32_t> &rgba, uint32_t &width, uint32_t &height, uint32_t &number)
    {
#ifdef SC3_HAVE_FFMPEG
        const auto player = find(handle);
        return player && player->picture(readWord(runtime, handle + kMwPlyCompoMode) == kMwPlyCompoAlpha, known, rgba, width, height, number);
#else
        (void)runtime, (void)handle, (void)known, (void)rgba, (void)width, (void)height, (void)number;
        return false;
#endif
    }

    void install(Registry &runtime)
    {
        for (uint32_t address : kMwPlyStarts)
            runtime.registerFunction(address, &start);
        runtime.registerFunction(kMwPlyGetStat, &getStat);
        runtime.registerFunction(kMoviePlayStat, &playStat);
        runtime.registerFunction(kMovieTaskEnd, &taskEndReleasesJob);
        runtime.registerFunction(kMwPlyCreate, &create);
        runtime.registerFunction(kMwPlyDestroy, &destroy);
        runtime.registerFunction(kMwPlyPause, &pause);
        runtime.registerFunction(kMwPlyIsPause, &isPause);
        for (uint32_t setting : kMwPlyDecoderSettings)
            runtime.registerFunction(setting, &answered);
        runtime.registerFunction(kMwPlyGetCurFrm, &getCurFrm);
        runtime.registerFunction(kMwPlyRelCurFrm, &answered);
        runtime.registerFunction(kMwPlyIsNextFrmReady, &isNextFrmReady);
#ifdef SC3_HAVE_FFMPEG
        twoD::setMoviePictures([](uint32_t handle, uint32_t known, std::vector<uint32_t> &rgba, uint32_t &width, uint32_t &height, uint32_t &number)
                               { return g_runtime && picture(g_runtime, handle, known, rgba, width, height, number); });
        runtime.registerFunction(kMwPlyStartAfs, &startAfs);
        runtime.registerFunction(kMwPlyStartAfsLp, &startAfsLp);
        runtime.registerFunction(kMwPlyStop, &stopCall);
#else
        runtime.registerFunction(kMwPlyStartAfs, &start);
        runtime.registerFunction(kMwPlyStartAfsLp, &start);
        runtime.registerFunction(kMwPlyStop, &start);
#endif
    }
}
