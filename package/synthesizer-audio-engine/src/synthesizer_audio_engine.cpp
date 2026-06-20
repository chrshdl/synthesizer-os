/**
 * synthesizer_audio_engine.cpp  —  Self-contained ALSA audio engine (v4)
 *
 * Architecture:
 * ─────────────
 *   Python note_on()  ──→  atomic gate  ──→  C++ audio thread
 *                                                   │
 *                                            render + writei
 *                                                   │
 *                                            ALSA "default"
 *                                                   │
 *                                              PipeWire → hw
 *
 * The C++ engine owns:
 *  - Precomputed wavetables (3-osc chorus per partial, random phases)
 *  - A dedicated audio thread that calls render() → snd_pcm_writei()
 *  - Lock-free note gates (std::atomic) for zero-overhead Python calls
 *
 * PipeWire compatibility:
 *  - Opens "default" ALSA device (PipeWire plugin via /etc/asound.conf)
 *  - Uses period=512 frames to match the PipeWire quantum configured
 *    in the systemd service (PIPEWIRE_PROPS=audio.quantum=512)
 *  - pygame's mixer is a separate PipeWire client; PipeWire mixes both
 *
 * Python side is a trivial ctypes shim — no threads, no subprocess, no pipe.
 *
 * C ABI:
 *   se_engine_t* se_create(int num_partials)
 *   void         se_destroy(se_engine_t*)
 *   void         se_note_on(se_engine_t*, int freq_hz)
 *   void         se_note_off(se_engine_t*, int freq_hz)
 *   void         se_all_notes_off(se_engine_t*)
 *   void         se_update_amplitudes(se_engine_t*, const float* amps, int n)
 *   void         se_set_master_volume(se_engine_t*, float vol)
 */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <sys/time.h>

#include <alsa/asoundlib.h>
#include <pthread.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr int SAMPLE_RATE      = 44100;
static constexpr int MAX_PARTIALS     = 16;
static constexpr int WAVETABLE_FRAMES = SAMPLE_RATE; // 1 second loop

// Render chunk = PipeWire quantum to avoid rebuffering
static constexpr int CHUNK_FRAMES     = 512;

static constexpr int KEY_FREQS[] = {
    262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494, 523
};
static constexpr int NUM_KEY_FREQS =
    static_cast<int>(sizeof(KEY_FREQS) / sizeof(KEY_FREQS[0]));

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

struct se_engine_t {
    int num_partials;

    std::vector<float> wavetable;

    inline float* partial_wave(int fi, int p) {
        return wavetable.data() +
               (fi * num_partials + p) * WAVETABLE_FRAMES;
    }

    float target_amps[MAX_PARTIALS];
    float current_amps[MAX_PARTIALS];
    std::mutex amp_mutex;

    std::atomic<bool>  note_gate[NUM_KEY_FREQS];
    float              note_env[NUM_KEY_FREQS];

    std::atomic<float> master_volume{0.5f};
    std::atomic<double> latency_t0{0.0};
    int                t_idx{0};

    // Audio thread
    std::atomic<bool> running{false};
    std::thread       audio_thread;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int freq_to_idx(int freq_hz) {
    for (int i = 0; i < NUM_KEY_FREQS; ++i)
        if (KEY_FREQS[i] == freq_hz) return i;
    return -1;
}

static void precompute_wavetables(se_engine_t* e) {
    srand(0xDEADBEEF);
    auto rph = []() -> float {
        return (static_cast<float>(rand()) / RAND_MAX) * 2.0f * static_cast<float>(M_PI);
    };
    for (int fi = 0; fi < NUM_KEY_FREQS; ++fi) {
        float bf = static_cast<float>(KEY_FREQS[fi]);
        for (int p = 0; p < e->num_partials; ++p) {
            float f = bf * (p + 1), d = static_cast<float>(3 + p);
            float pc = rph(), ps = rph(), pf = rph();
            float* buf = e->partial_wave(fi, p);
            for (int n = 0; n < WAVETABLE_FRAMES; ++n) {
                float t = static_cast<float>(n) / SAMPLE_RATE;
                float wc = std::sin(2.0f * static_cast<float>(M_PI) * f * t + pc);
                float ws = std::sin(2.0f * static_cast<float>(M_PI) * (f + d) * t + ps) * 0.4f;
                float wf = std::sin(2.0f * static_cast<float>(M_PI) * (f - d) * t + pf) * 0.4f;
                buf[n] = (wc + ws + wf) / 1.8f;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Render — fills interleaved stereo float32 buffer
// ---------------------------------------------------------------------------

static void render(se_engine_t* e, float* out, int nf) {
    std::fill(out, out + nf * 2, 0.0f);

    float ta[MAX_PARTIALS], ca[MAX_PARTIALS];
    {
        std::lock_guard<std::mutex> lk(e->amp_mutex);
        std::copy(e->target_amps,  e->target_amps  + e->num_partials, ta);
        std::copy(e->current_amps, e->current_amps + e->num_partials, ca);
    }

    float as[MAX_PARTIALS];
    for (int p = 0; p < e->num_partials; ++p)
        as[p] = (ta[p] - ca[p]) / nf;

    const float env_step    = 1.0f / (SAMPLE_RATE * 0.025f);
    const float max_env_chg = env_step * nf;
    const float master      = e->master_volume.load(std::memory_order_relaxed);
    const float headroom    = master / (e->num_partials * 2.5f);
    const int   start       = e->t_idx % WAVETABLE_FRAMES;

    for (int ni = 0; ni < NUM_KEY_FREQS; ++ni) {
        bool  gate = e->note_gate[ni].load(std::memory_order_relaxed);
        float es   = e->note_env[ni];
        float et   = gate ? 1.0f : 0.0f;

        if (es == 0.0f && !gate) continue;

        float ee;
        if (es == et) { ee = et; }
        else {
            float d = et - es;
            ee = es + std::copysign(std::min(std::abs(d), max_env_chg), d);
        }
        e->note_env[ni] = ee;
        float ed = (ee - es) / nf;

        for (int p = 0; p < e->num_partials; ++p) {
            const float* w = e->partial_wave(ni, p);
            float a0 = ca[p], ad = as[p];
            for (int s = 0; s < nf; ++s) {
                int wi = (start + s) % WAVETABLE_FRAMES;
                float v = w[wi] * (es + ed * s) * (a0 + ad * s);
                out[s * 2    ] += v;
                out[s * 2 + 1] += v;
            }
        }
    }

    for (int s = 0; s < nf; ++s) {
        out[s * 2    ] = std::tanh(out[s * 2    ] * headroom);
        out[s * 2 + 1] = std::tanh(out[s * 2 + 1] * headroom);
    }

    {
        std::lock_guard<std::mutex> lk(e->amp_mutex);
        for (int p = 0; p < e->num_partials; ++p) {
            float n = ca[p] + as[p] * nf;
            e->current_amps[p] = (as[p] >= 0) ? std::min(n, ta[p]) : std::max(n, ta[p]);
        }
    }

    e->t_idx += nf;
}

// ---------------------------------------------------------------------------
// ALSA setup
// ---------------------------------------------------------------------------

static snd_pcm_t* open_alsa() {
    snd_pcm_t* pcm = nullptr;

    // Open "default" which routes through PipeWire via /etc/asound.conf
    int rc = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        fprintf(stderr, "[synthengine] ALSA open failed: %s\n", snd_strerror(rc));
        return nullptr;
    }

    snd_pcm_hw_params_t* hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm, hw, 2);

    unsigned rate = SAMPLE_RATE;
    snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, nullptr);

    // Match PipeWire quantum: period=512, buffer=2*period
    snd_pcm_uframes_t period = CHUNK_FRAMES;
    snd_pcm_uframes_t buffer = period * 2;
    snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, nullptr);
    snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);

    rc = snd_pcm_hw_params(pcm, hw);
    if (rc < 0) {
        fprintf(stderr, "[synthengine] ALSA hw_params: %s\n", snd_strerror(rc));
        snd_pcm_close(pcm);
        return nullptr;
    }

    // Sw params: start threshold = 1 period, avail_min = period
    snd_pcm_sw_params_t* sw;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(pcm, sw);
    snd_pcm_sw_params_set_start_threshold(pcm, sw, period);
    snd_pcm_sw_params_set_avail_min(pcm, sw, period);
    snd_pcm_sw_params(pcm, sw);
    snd_pcm_prepare(pcm);

    // Log actual parameters
    snd_pcm_uframes_t ap = 0, ab = 0;
    snd_pcm_hw_params_get_period_size(hw, &ap, nullptr);
    snd_pcm_hw_params_get_buffer_size(hw, &ab);
    fprintf(stderr,
            "[synthengine] ALSA: rate=%u period=%lu (%.1fms) buffer=%lu (%.1fms)\n",
            rate, ap, ap * 1000.0 / rate, ab, ab * 1000.0 / rate);

    return pcm;
}

// ---------------------------------------------------------------------------
// Audio thread
// ---------------------------------------------------------------------------

static void audio_thread_fn(se_engine_t* e) {
    // Try RT scheduling (non-fatal if denied)
    {
        sched_param sp{};
        sp.sched_priority = sched_get_priority_min(SCHED_FIFO) + 5;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0)
            fprintf(stderr, "[synthengine] RT scheduling active\n");
    }

    snd_pcm_t* pcm = open_alsa();
    if (!pcm) {
        fprintf(stderr, "[synthengine] No ALSA device — audio disabled\n");
        return;
    }

    float   fbuf[CHUNK_FRAMES * 2];
    int16_t pcm_buf[CHUNK_FRAMES * 2];

    while (e->running.load(std::memory_order_relaxed)) {
        render(e, fbuf, CHUNK_FRAMES);

        // float → int16 stereo
        for (int i = 0; i < CHUNK_FRAMES * 2; ++i)
            pcm_buf[i] = static_cast<int16_t>(
                std::clamp(fbuf[i] * 32767.0f, -32768.0f, 32767.0f));

        snd_pcm_sframes_t written = snd_pcm_writei(pcm, pcm_buf, CHUNK_FRAMES);
        if (written < 0) {
            written = snd_pcm_recover(pcm, static_cast<int>(written), 1);
            if (written < 0) {
                fprintf(stderr, "[synthengine] ALSA fatal: %s\n",
                        snd_strerror(static_cast<int>(written)));
                break;
            }
        }
        
        double t0 = e->latency_t0.load(std::memory_order_relaxed);
        if (t0 > 0.0) {
            bool signal_active = false;
            for (int i = 0; i < CHUNK_FRAMES * 2; ++i) {
                if (std::abs(fbuf[i]) > 0.003f) {
                    signal_active = true;
                    break;
                }
            }
            if (signal_active) {
                struct timeval tv;
                gettimeofday(&tv, nullptr);
                double t1 = tv.tv_sec + tv.tv_usec / 1000000.0;
                fprintf(stderr, "===========================================================\n");
                fprintf(stderr, ">>> SOFTWARE LATENCY: %.2f ms <<<\n", (t1 - t0) * 1000.0);
                fprintf(stderr, "===========================================================\n");
                e->latency_t0.store(0.0, std::memory_order_relaxed);
            }
        }
    }

    if (pcm) { snd_pcm_drain(pcm); snd_pcm_close(pcm); }
}

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------

extern "C" {

se_engine_t* se_create(int num_partials) {
    if (num_partials <= 0 || num_partials > MAX_PARTIALS) num_partials = 8;

    auto* e = new se_engine_t();
    e->num_partials = num_partials;
    e->wavetable.resize(
        static_cast<size_t>(NUM_KEY_FREQS) * num_partials * WAVETABLE_FRAMES);

    for (int p = 0; p < num_partials; ++p) {
        e->target_amps[p]  = (p == 0) ? 1.0f : 0.0f;
        e->current_amps[p] = e->target_amps[p];
    }
    for (int i = 0; i < NUM_KEY_FREQS; ++i) {
        e->note_gate[i].store(false, std::memory_order_relaxed);
        e->note_env[i] = 0.0f;
    }

    precompute_wavetables(e);

    e->running.store(true);
    e->audio_thread = std::thread(audio_thread_fn, e);

    return e;
}

void se_destroy(se_engine_t* e) {
    if (!e) return;
    e->running.store(false);
    if (e->audio_thread.joinable()) e->audio_thread.join();
    delete e;
}

void se_note_on(se_engine_t* e, int freq_hz) {
    if (!e) return;
    int i = freq_to_idx(freq_hz);
    if (i >= 0) e->note_gate[i].store(true, std::memory_order_relaxed);
}

void se_note_off(se_engine_t* e, int freq_hz) {
    if (!e) return;
    int i = freq_to_idx(freq_hz);
    if (i >= 0) e->note_gate[i].store(false, std::memory_order_relaxed);
}

void se_all_notes_off(se_engine_t* e) {
    if (!e) return;
    for (int i = 0; i < NUM_KEY_FREQS; ++i)
        e->note_gate[i].store(false, std::memory_order_relaxed);
}

void se_update_amplitudes(se_engine_t* e, const float* amps, int count) {
    if (!e || !amps) return;
    if (count > e->num_partials) count = e->num_partials;
    std::lock_guard<std::mutex> lk(e->amp_mutex);
    for (int p = 0; p < count; ++p)
        e->target_amps[p] = amps[p];
}

void se_set_master_volume(se_engine_t* e, float vol) {
    if (e) e->master_volume.store(vol, std::memory_order_relaxed);
}

void se_trigger_latency_measurement(se_engine_t* e, double t0_sec) {
    if (e) e->latency_t0.store(t0_sec, std::memory_order_relaxed);
}

} // extern "C"
