// Replacement movies: assets/replacements/fmvs/<NAME>.mkv (or .mp4 / .avi / .wmv) plays instead of
// the disc's <NAME>.STR. The game itself still plays its own movie underneath - it streams the
// .STR from the disc, decodes it and waits for its end or a skip as before - and only what is seen
// and heard changes: while the game shows a movie picture (24-bit display), the host shows the
// replacement's picture instead (draw_game), and the replacement's sound takes the place of the
// movie's CD audio in the SPU (so the game's own volume still applies).
//
// A replacement starts when the game reads the first sectors of a .STR that has one, and stops
// when the game reads elsewhere on the disc, stops showing movie pictures, or stops reading.
// Decoding is Windows Media Foundation (H.264 / HEVC / AAC ... as installed; FFmpeg elsewhere) on its own thread,
// video scaled to at most 1080 lines; the picture follows the sound that has been played.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "psx.h"

#ifdef _WIN32
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#elif defined(HAVE_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

bool cdrom_read_sector_user(uint32_t lba, uint8_t* out);

namespace {

constexpr const char* kDir = "assets/replacements/fmvs";
constexpr int kMaxLines = 1080;            // decoded size: at most this tall
constexpr size_t kMaxFrames = 6;           // decoded pictures waiting
constexpr size_t kMaxAudio = 44100 * 2 * 2;  // decoded sound waiting (2 s, interleaved stereo)
constexpr uint32_t kStartWindow = 32;      // a read this near a .STR's start begins its replacement
constexpr uint64_t kStaleFrames = 90;      // no read from the movie for 1.5 s: it is over

struct Str {
    std::string name;  // upper case, no extension
    uint32_t lba, sectors;
    std::wstring file;  // its replacement ("" = none)
};
std::vector<Str> g_strs;
bool g_scanned = false;

struct Frame {
    double t;  // seconds
    std::vector<uint32_t> px;
};

// the replacement playing (main thread, except what the decoder fills under g_mx)
struct Playing {
    const Str* str = nullptr;
    uint64_t last_read = 0;   // host frame of the last sector read from its .STR
    bool picture_seen = false;
    int no_picture = 0;       // vblanks without a movie picture since one was seen
    bool has_audio = false;
    uint64_t played = 0;      // stereo samples handed to the SPU (the clock)
    uint64_t start_frame = 0; // the clock without sound
    int w = 0, h = 0;
    std::vector<uint32_t> shown;
    bool have_shown = false;
} g_play;

std::mutex g_mx;
std::condition_variable g_cv;
std::deque<Frame> g_frames;
std::deque<int16_t> g_audio;
bool g_eof = false;
std::atomic<bool> g_stop{false};
std::thread g_thread;
std::vector<int16_t> g_take;  // audio taken out of g_audio in blocks (SPU side)
size_t g_take_pos = 0;

void walk(uint32_t lba, uint32_t size, int depth) {
    uint8_t sec[2048];
    for (uint32_t s = 0; s < (size + 2047) / 2048; s++) {
        if (!cdrom_read_sector_user(lba + s, sec)) return;
        for (int o = 0; o < 2048 && sec[o];) {
            const int len = sec[o];
            uint32_t elba, esize;
            std::memcpy(&elba, sec + o + 2, 4);
            std::memcpy(&esize, sec + o + 10, 4);
            std::string name((char*)sec + o + 33, sec[o + 32]);
            const bool dir = sec[o + 25] & 2;
            if (dir && name != std::string(1, '\0') && name != std::string(1, '\1') && depth < 4) {
                walk(elba, esize, depth + 1);
            } else if (!dir) {
                if (auto semi = name.find(';'); semi != std::string::npos) name.resize(semi);
                for (char& ch : name) ch = (char)std::toupper((unsigned char)ch);
                if (name.size() > 4 && name.compare(name.size() - 4, 4, ".STR") == 0)
                    g_strs.push_back({name.substr(0, name.size() - 4), elba, (esize + 2047) / 2048, L""});
            }
            o += len;
        }
    }
}

void scan() {
    g_scanned = true;
    if (std::getenv("APOC_NO_MOVIE_REPLACE")) return;  // (tests: the disc's movies)
    uint8_t pvd[2048];
    if (!cdrom_read_sector_user(16, pvd)) return;
    uint32_t root, size;
    std::memcpy(&root, pvd + 156 + 2, 4);
    std::memcpy(&size, pvd + 156 + 10, 4);
    walk(root, size, 0);
    int found = 0;
    std::error_code ec;
    for (Str& s : g_strs) {
        for (const char* ext : {".mkv", ".mp4", ".avi", ".wmv", ".MKV", ".MP4"}) {
            const std::filesystem::path p = std::filesystem::path(kDir) / (s.name + ext);
            if (std::filesystem::exists(p, ec)) {
                s.file = std::filesystem::absolute(p, ec).wstring();
                found++;
                break;
            }
        }
    }
    log_msg(LogCat::Host, "replacement movies: %d of %zu movies on the disc", found, g_strs.size());
}

#ifdef _WIN32
template <class T> void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// The video frame rate an MKV file's header gives (its video track's DefaultDuration, 23 E3 83,
// next to its "V_..." codec id), 0 if none. Windows' MKV reader reports half of it for some
// H.264 files, and its video processor then drops every other frame.
double mkv_fps(const std::wstring& file) {
    std::vector<char> b(1 << 20);
    FILE* f = _wfopen(file.c_str(), L"rb");
    if (!f) return 0;
    b.resize(std::fread(b.data(), 1, b.size(), f));
    std::fclose(f);
    const std::string s(b.begin(), b.end());
    for (size_t v = s.find("V_"); v != std::string::npos; v = s.find("V_", v + 2)) {
        size_t best = std::string::npos;
        for (size_t d = s.find("\x23\xE3\x83", v > 256 ? v - 256 : 0); d != std::string::npos && d < v + 256;
             d = s.find("\x23\xE3\x83", d + 3))
            if (best == std::string::npos || (d > v ? d - v : v - d) < (best > v ? best - v : v - best)) best = d;
        if (best == std::string::npos || best + 4 >= s.size()) continue;
        const uint8_t len = (uint8_t)s[best + 3];  // a one-byte EBML size: 0x80 | n
        if ((len & 0xF8) != 0x80 || (len & 7) == 0 || best + 4 + (len & 7) > s.size()) continue;
        uint64_t ns = 0;
        for (int i = 0; i < (len & 7); i++) ns = ns << 8 | (uint8_t)s[best + 4 + i];
        if (ns) return 1e9 / (double)ns;
    }
    return 0;
}

// The decoder thread: reads the file in order, keeping a few pictures and some sound ahead.
void decode(std::wstring file, int* out_w, int* out_h, bool* has_audio, std::atomic<bool>* ready) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    IMFAttributes* attr = nullptr;
    IMFSourceReader* rd = nullptr;
    MFCreateAttributes(&attr, 1);
    attr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    HRESULT hr = MFCreateSourceReaderFromURL(file.c_str(), attr, &rd);
    release(attr);
    int w = 0, h = 0;
    bool audio = false;
    if (SUCCEEDED(hr)) {
        IMFMediaType* native = nullptr;
        UINT32 nw = 0, nh = 0, rn = 0, rd_ = 0;
        if (SUCCEEDED(rd->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native))) {
            MFGetAttributeSize(native, MF_MT_FRAME_SIZE, &nw, &nh);
            MFGetAttributeRatio(native, MF_MT_FRAME_RATE, &rn, &rd_);
        }
        release(native);
        if (nh > (UINT32)kMaxLines) nw = (UINT32)((uint64_t)nw * kMaxLines / nh) & ~1u, nh = kMaxLines;
        IMFMediaType* t = nullptr;
        MFCreateMediaType(&t);
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (nw && nh) MFSetAttributeSize(t, MF_MT_FRAME_SIZE, nw, nh);
        // the file's own frame rate where Windows reports less (else frames are dropped)
        const double native_fps = rd_ ? (double)rn / rd_ : 0, header_fps = mkv_fps(file);
        if (header_fps > native_fps + 0.5)
            MFSetAttributeRatio(t, MF_MT_FRAME_RATE, (UINT32)std::lround(header_fps * 1000), 1000);
        log_msg(LogCat::Host, "replacement movie: %.2f fps (Windows reports %.2f)", header_fps > native_fps ? header_fps : native_fps,
                native_fps);
        hr = rd->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, t);
        release(t);
        IMFMediaType* cur = nullptr;
        if (SUCCEEDED(hr) && SUCCEEDED(rd->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur))) {
            UINT32 cw = 0, ch = 0;
            MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &cw, &ch);
            w = (int)cw, h = (int)ch;
        }
        release(cur);
        IMFMediaType* a = nullptr;
        MFCreateMediaType(&a);
        a->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        a->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        a->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        a->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
        a->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        a->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        a->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 44100 * 4);
        audio = SUCCEEDED(rd->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, a));
        release(a);
        if (!audio) rd->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, FALSE);
    }
    if (FAILED(hr) || w <= 0 || h <= 0) {
        log_msg(LogCat::Host, "replacement movie: cannot play %ls (%08lX)", file.c_str(), (unsigned long)hr);
        release(rd);
        w = h = 0;
    }
    *out_w = w, *out_h = h, *has_audio = audio;
    ready->store(true);
    while (rd && !g_stop.load()) {
        DWORD stream = 0, flags = 0;
        LONGLONG ts = 0;
        IMFSample* s = nullptr;
        if (FAILED(rd->ReadSample(MF_SOURCE_READER_ANY_STREAM, 0, &stream, &flags, &ts, &s)) ||
            (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
            release(s);
            break;
        }
        if (!s) continue;
        IMFMediaBuffer* b = nullptr;
        BYTE* p = nullptr;
        DWORD len = 0;
        if (SUCCEEDED(s->ConvertToContiguousBuffer(&b)) && SUCCEEDED(b->Lock(&p, nullptr, &len))) {
            IMFMediaType* cur = nullptr;
            GUID major{};
            if (SUCCEEDED(rd->GetCurrentMediaType(stream, &cur))) cur->GetGUID(MF_MT_MAJOR_TYPE, &major);
            release(cur);
            std::unique_lock<std::mutex> lk(g_mx);
            if (major == MFMediaType_Video && len >= (DWORD)(w * h * 4)) {
                g_cv.wait(lk, [] { return g_frames.size() < kMaxFrames || g_stop.load(); });
                Frame f{ts / 1e7, std::vector<uint32_t>((size_t)w * h)};
                std::memcpy(f.px.data(), p, (size_t)w * h * 4);
                g_frames.push_back(std::move(f));
            } else if (major == MFMediaType_Audio) {
                g_cv.wait(lk, [] { return g_audio.size() < kMaxAudio || g_stop.load(); });
                const int16_t* q = (const int16_t*)p;
                g_audio.insert(g_audio.end(), q, q + len / 2);
            }
            b->Unlock();
        }
        release(b);
        release(s);
    }
    release(rd);
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_eof = true;
    }
    MFShutdown();
    CoUninitialize();
}
#elif defined(HAVE_FFMPEG)
AVCodecContext* open_codec(AVFormatContext* fc, int index) {
    if (index < 0) return nullptr;
    const AVCodecParameters* par = fc->streams[index]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    AVCodecContext* cc = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (cc && (avcodec_parameters_to_context(cc, par) < 0 || avcodec_open2(cc, codec, nullptr) < 0)) avcodec_free_context(&cc);
    return cc;
}

// The decoder thread (FFmpeg): reads the file in order, keeping a few pictures and some sound ahead.
void decode(std::wstring file, int* out_w, int* out_h, bool* has_audio, std::atomic<bool>* ready) {
    const std::string path = std::filesystem::path(file).string();
    AVFormatContext* fc = nullptr;
    AVCodecContext *vc = nullptr, *ac = nullptr;
    SwsContext* sws = nullptr;
    SwrContext* swr = nullptr;
    int vi = -1, ai = -1, w = 0, h = 0;
    if (avformat_open_input(&fc, path.c_str(), nullptr, nullptr) == 0 && avformat_find_stream_info(fc, nullptr) >= 0) {
        vi = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        ai = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, vi, nullptr, 0);
        vc = open_codec(fc, vi);
        ac = open_codec(fc, ai);
        if (vc && vc->width > 0 && vc->height > 0) {
            w = vc->width, h = vc->height;
            if (h > kMaxLines) w = (int)((int64_t)w * kMaxLines / h) & ~1, h = kMaxLines;
            sws = sws_getContext(vc->width, vc->height, vc->pix_fmt, w, h, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        }
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (ac && (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, 44100, &ac->ch_layout, ac->sample_fmt,
                                       ac->sample_rate, 0, nullptr) < 0 || swr_init(swr) < 0))
            swr_free(&swr);
    }
    if (!sws) {
        log_msg(LogCat::Host, "replacement movie: cannot play %s", path.c_str());
        w = h = 0;
    }
    *out_w = w, *out_h = h, *has_audio = sws && swr;
    ready->store(true);
    const AVStream* vs = vi >= 0 ? fc->streams[vi] : nullptr;
    const int64_t t0 = vs && vs->start_time != AV_NOPTS_VALUE ? vs->start_time : 0;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    auto drain = [&](AVCodecContext* cc) {
        while (!g_stop.load() && avcodec_receive_frame(cc, fr) == 0) {
            if (cc == vc) {
                const int64_t pts = fr->best_effort_timestamp == AV_NOPTS_VALUE ? t0 : fr->best_effort_timestamp;
                Frame f{(double)(pts - t0) * av_q2d(vs->time_base), std::vector<uint32_t>((size_t)w * h)};
                uint8_t* dst[1] = {(uint8_t*)f.px.data()};
                const int stride[1] = {w * 4};
                sws_scale(sws, fr->data, fr->linesize, 0, fr->height, dst, stride);
                std::unique_lock<std::mutex> lk(g_mx);
                g_cv.wait(lk, [] { return g_frames.size() < kMaxFrames || g_stop.load(); });
                g_frames.push_back(std::move(f));
            } else {
                const int max = swr_get_out_samples(swr, fr->nb_samples);
                std::vector<int16_t> buf((size_t)std::max(max, 0) * 2);
                uint8_t* out = (uint8_t*)buf.data();
                const int n = swr_convert(swr, &out, max, (const uint8_t**)fr->extended_data, fr->nb_samples);
                std::unique_lock<std::mutex> lk(g_mx);
                g_cv.wait(lk, [] { return g_audio.size() < kMaxAudio || g_stop.load(); });
                if (n > 0) g_audio.insert(g_audio.end(), buf.begin(), buf.begin() + (ptrdiff_t)n * 2);
            }
            av_frame_unref(fr);
        }
    };
    while (sws && !g_stop.load() && av_read_frame(fc, pkt) >= 0) {
        AVCodecContext* cc = pkt->stream_index == vi ? vc : pkt->stream_index == ai && swr ? ac : nullptr;
        if (cc && avcodec_send_packet(cc, pkt) >= 0) drain(cc);
        av_packet_unref(pkt);
    }
    for (AVCodecContext* cc : {vc, swr ? ac : nullptr})  // the frames still in the decoders
        if (sws && cc && avcodec_send_packet(cc, nullptr) >= 0) drain(cc);
    av_frame_free(&fr);
    av_packet_free(&pkt);
    sws_freeContext(sws);
    swr_free(&swr);
    avcodec_free_context(&vc);
    avcodec_free_context(&ac);
    avformat_close_input(&fc);
    std::lock_guard<std::mutex> lk(g_mx);
    g_eof = true;
}
#endif

void stop(const char* why) {
    if (!g_play.str) return;
    log_msg(LogCat::Host, "replacement movie %s: stopped (%s)", g_play.str->name.c_str(), why);
    g_stop.store(true);
    g_cv.notify_all();
    if (g_thread.joinable()) g_thread.join();
    g_frames.clear();
    g_audio.clear();
    g_take.clear();
    g_take_pos = 0;
    g_play = Playing{};
}

// Quitting during a movie (std::exit): the decoder must end before g_thread is destroyed, or a
// still-running std::thread's destructor aborts the program.
struct StopAtExit {
    ~StopAtExit() { stop("quit"); }
} g_stop_at_exit;

void start(const Str& s) {
#if defined(_WIN32) || defined(HAVE_FFMPEG)
    stop("another movie");
    g_stop.store(false);
    g_eof = false;
    std::atomic<bool> ready{false};
    int w = 0, h = 0;
    bool audio = false;
    g_thread = std::thread(decode, s.file, &w, &h, &audio, &ready);
    while (!ready.load()) std::this_thread::yield();
    if (!w) {
        g_thread.join();
        return;
    }
    g_play.str = &s;
    g_play.w = w, g_play.h = h;
    g_play.has_audio = audio;
    g_play.last_read = g_play.start_frame = host_frame_count();
    log_msg(LogCat::Host, "replacement movie %s: %dx%d%s", s.name.c_str(), w, h, audio ? ", with sound" : "");
#else
    (void)s;
#endif
}

}  // namespace

// cdrom.cpp: a sector was read from the disc.
void movie_cd_sector(uint32_t lba) {
    if (!g_scanned) scan();
    if (g_play.str) {
        if (lba >= g_play.str->lba && lba < g_play.str->lba + g_play.str->sectors) {
            g_play.last_read = host_frame_count();
            return;
        }
        stop("the game read elsewhere");
    }
    for (const Str& s : g_strs)
        if (!s.file.empty() && lba >= s.lba && lba < s.lba + kStartWindow) {
            start(s);
            return;
        }
}

// A replacement is playing (its sound replaces the movie's CD audio).
bool movie_replacing() { return g_play.str != nullptr; }

// spu.cpp: the next stereo sample of the replacement's sound (false: none yet / no sound).
bool movie_audio(int16_t* l, int16_t* r) {
    if (!g_play.str || !g_play.has_audio) return false;
    if (g_take_pos + 1 >= g_take.size()) {
        g_take.clear();
        g_take_pos = 0;
        std::lock_guard<std::mutex> lk(g_mx);
        const size_t n = std::min<size_t>(g_audio.size() & ~size_t(1), 2048);
        g_take.assign(g_audio.begin(), g_audio.begin() + (ptrdiff_t)n);
        g_audio.erase(g_audio.begin(), g_audio.begin() + (ptrdiff_t)n);
        if (n) g_cv.notify_all();
    }
    if (g_take_pos + 1 >= g_take.size()) {
        *l = *r = 0;
        return true;  // decoder behind (or the end): silence, the clock holds
    }
    *l = g_take[g_take_pos++];
    *r = g_take[g_take_pos++];
    g_play.played++;
    return true;
}

// host_sdl.cpp, once a frame: `movie_picture_shown` = the game shows a movie picture now.
void movie_vblank(bool movie_picture_shown) {
    if (!g_play.str) return;
    if (movie_picture_shown) {
        g_play.picture_seen = true;
        g_play.no_picture = 0;
    } else if (g_play.picture_seen && ++g_play.no_picture > 3) {
        stop("the game's movie ended");
        return;
    }
    if (host_frame_count() - g_play.last_read > kStaleFrames) stop("the game stopped reading it");
}

// host_sdl.cpp: the replacement's picture for now (null: show the game's own).
const uint32_t* movie_picture(int* w, int* h) {
    if (!g_play.str) return nullptr;
    const double t = g_play.has_audio ? (double)g_play.played / 44100.0
                                      : (double)(host_frame_count() - g_play.start_frame) / 60.0;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        bool took = false;
        while (!g_frames.empty() && (g_frames.front().t <= t + 0.008 || !g_play.have_shown)) {
            g_play.shown = std::move(g_frames.front().px);
            g_frames.pop_front();
            g_play.have_shown = took = true;
        }
        if (took) g_cv.notify_all();
    }
    if (!g_play.have_shown) return nullptr;
    *w = g_play.w, *h = g_play.h;
    return g_play.shown.data();
}
