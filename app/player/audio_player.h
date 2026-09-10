// AudioPlayer — Phase 5.0.a, single-clip audio playback.
//
// Owns one AudioDecoder + one platform audio device. The render
// callback drains the decoder's ring buffer into the device output
// buffer. Phase 5.0.a is the simplest path — no master-clock sync,
// no gap detection, no volume soft-limit. The decode thread runs
// free; the device runs free; the audio plays at its own rate as
// fast as the codec produces it.
//
// Phase 5.0.b adds the master-clock sync: the video clock owns
// time, AudioPlayer::update() polls drift and seeks the decoder
// when drift > threshold. Volume + mute land there too.

#pragma once

#include "audio_sync_servo.h"
#include "fractional_resampler.h"
#include "i_audio_source.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QtQmlIntegration>

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

namespace ufbplayer {

#if defined(Q_OS_MACOS) || defined(__APPLE__)
class CoreAudioDevice;
#elif defined(Q_OS_WIN)
class WasapiAudioDevice;
#endif

class AudioPlayer : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool hasAudio READ hasAudio NOTIFY hasAudioChanged)
    Q_PROPERTY(bool isPlaying READ isPlaying NOTIFY isPlayingChanged)
    // Linear gain in [0,1]. Soft-limited in the render callback so
    // a sum of multi-track sources (Phase 5.x) doesn't clip.
    Q_PROPERTY(float volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool  muted  READ muted  WRITE setMuted  NOTIFY mutedChanged)

public:
    explicit AudioPlayer(QObject *parent = nullptr);
    ~AudioPlayer() override;

    AudioPlayer(const AudioPlayer &) = delete;
    AudioPlayer &operator=(const AudioPlayer &) = delete;

    // Q_INVOKABLE on the transport surface: the lightbox drives AudioPlayer
    // from QML (QCView drove it from C++, so these were plain methods).
    Q_INVOKABLE bool initialize();
    Q_INVOKABLE void shutdown();

    // Opens the file's audio. `audioStreamCountHint`, when >= 0,
    // skips the dispatch probe (avformat_open_input +
    // find_stream_info just to count streams) and uses the supplied
    // value to pick AudioDecoder (≤1) vs MultiStreamAudioDecoder
    // (≥2). WindowManager passes the cached
    // MediaItem.video.audioStreamCount once metadata has loaded;
    // -1 falls back to the in-process probe (cold-load case).
    Q_INVOKABLE bool open(const QString &path, int audioStreamCountHint = -1);
    Q_INVOKABLE void close();

    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void stop();

    // Seek the audio decoder to `seconds`. Phase 5.0.a wires this
    // to VideoDecoder so scrub + play-after-scrub align audio with
    // video. Master-clock drift correction (continuous re-seek
    // during playback) is Phase 5.0.b.
    Q_INVOKABLE void seek(double seconds);

    bool   hasAudio()  const;
    bool   isPlaying() const { return m_isPlaying.load(); }
    float  volume()    const { return m_volume.load(); }
    bool   muted()     const { return m_muted.load(); }
    // Audio file duration in seconds. 0.0 when no file is open. Read
    // from AudioDecoder, which pulls from the FFmpeg container's
    // duration field at open() time. Authoritative for audio-only
    // files where VideoDecoder reports 0 (no video stream → no
    // fps × frameCount duration). Q_INVOKABLE for AudioPreview.qml,
    // which drives this player standalone (no VideoDecoder).
    Q_INVOKABLE double duration() const;

    Q_INVOKABLE void setVolume(float v);
    Q_INVOKABLE void setMuted(bool m);

    // A/V sync compensation. Positive offset DELAYS audio relative
    // to video (read sample for time T-offset when video is at T) —
    // the right direction to compensate for video pipeline + display
    // lag, which is why audio normally sounds "ahead" of the visible
    // image. Negative offset advances audio (uncommon — use case is
    // an audio-side delay we want to compensate for). WindowManager
    // owns the user-facing slider + per-OS default + QSettings
    // persistence; this is just the live knob. Re-anchors on the
    // next seek; call seek(currentVideoPos) yourself if you want
    // immediate effect.
    void setSyncOffsetMs(int ms);
    int  syncOffsetMs() const { return m_syncOffsetMs.load(); }

    // Per-clip channel routing. Pass-through to the decoder; safe to
    // call mid-playback (decoder does the SwrContext rebuild on its
    // own thread). `mode` matches ufbplayer::AudioRoutingMode (0 = Auto,
    // 1 = Downmix5_1, 2 = Stereo7_8). WindowManager calls this both
    // on initial open (with the MediaItem's stored mode) and when
    // the inspector pill emits audioRoutingModeChanged.
    void setRoutingMode(int mode);

    // Per-source-channel peak levels of the most-recently-decoded
    // frame, pre-fold. Empty when no audio is open. Driven by the
    // 30 Hz audio meter pump in WindowManager.
    QVariantList audioChannelPeaks() const;
    QStringList  audioChannelNames() const;

    // Sync tick. Caller passes the master clock's current position in
    // seconds (the video clock, or AudioPreview's own timer). Inside a
    // ~40 ms band the PI servo trims the render callback's consumption
    // ratio by up to ±0.2 % (inaudible) so drift converges to ~0 with
    // no seeks; beyond it the old re-seek tiers remain (soft with a
    // 1 s cooldown, immediate past 1 s). Call at ~30 Hz or per frame.
    Q_INVOKABLE void update(double videoPositionSeconds);

signals:
    void hasAudioChanged();
    void isPlayingChanged();
    void volumeChanged();
    void mutedChanged();

private:
    static void dataCallback(void *device, float *output,
                             uint32_t frameCount, void *userData);
    void processAudio(float *output, uint32_t frameCount);
    // Re-anchor the playout-position estimate at `srcSec` (source
    // seconds) and reset the servo. Called from seek(), and from
    // open()/close() because UFB's QML resumes without a seek after
    // a source change (QCView always seeks before play).
    void resetAnchor(double srcSec);

    bool                              m_initialized = false;
    // IAudioSource so the same player drives single-stream
    // (AudioDecoder) and multi-stream (MultiStreamAudioDecoder)
    // sources interchangeably. Concrete instance picked at open()
    // time based on the file's audio-stream count.
    std::unique_ptr<IAudioSource>     m_decoder;
#if defined(Q_OS_MACOS) || defined(__APPLE__)
    std::unique_ptr<CoreAudioDevice>  m_device;
#elif defined(Q_OS_WIN)
    std::unique_ptr<WasapiAudioDevice> m_device;
#endif
    std::atomic<bool>                 m_isPlaying{false};
    std::atomic<float>                m_volume{1.0f};
    std::atomic<bool>                 m_muted{false};

    // ---- Servo state (ported from QCView) ----
    // Audio playout position is reconstructed in SOURCE seconds:
    //   audioSrcPos = m_anchorSrcSec
    //               + srcFramesConsumed / sampleRate
    //               - device bufferLatencySeconds() × ratio
    // The anchor is owned by seek() / resetAnchor() (play() does not
    // re-anchor: consumption simply pauses with the device while
    // stopped, so the estimate stays valid across pause/play).
    // Consumption counts REAL ring frames drained by the render
    // callback — silence padding on underrun does not advance it, and
    // while a decoder seek is pending the callback outputs silence
    // without consuming, so pre-flush frames never count against a
    // fresh anchor.
    double                            m_anchorSrcSec = 0.0;   // UI thread
    std::atomic<uint64_t>             m_srcFramesConsumed{0};

    // Controller runs on the UI thread in update(); the resulting
    // ratio crosses to the render callback through this atomic.
    AudioSyncServo                    m_servo;                // UI thread
    std::atomic<float>                m_servoRatio{1.0f};

    // Render-callback-only: fractional resampler + its source scratch
    // (sized once in initialize() from the device's real buffer frame
    // count — WASAPI can request the full buffer in one callback).
    FractionalResampler               m_servoResampler;
    std::vector<float>                m_servoScratch;
    // Look-ahead carry: sourceFramesNeeded() asks for 1–2 frames more
    // than process() advances past (Catmull-Rom needs p2/p3 beyond the
    // last output sample). The ring read is destructive, so without a
    // carry those frames were dropped every callback — one skipped
    // sample per 512-frame CoreAudio callback = the stream (and the
    // consumption counter) running 0.2 % fast, which ate the servo's
    // entire ±0.2 % authority and forced a re-seek every ~10 s. Render
    // thread only; resetAnchor() invalidates it via the atomic flag.
    static constexpr std::size_t      kServoCarryMax = 8;
    std::vector<float>                m_servoCarry;
    std::size_t                       m_servoCarryFrames = 0;
    std::atomic<bool>                 m_servoCarryInvalidate{false};

    // dt source for the servo's PI terms (update cadence is
    // irregular). UI thread only.
    std::chrono::steady_clock::time_point m_lastServoUpdate{};
    bool                              m_lastServoUpdateValid = false;
    int                               m_servoLogCounter = 0;

    // A/V sync offset in milliseconds. Applied at seek() — the
    // decoder is told to fetch samples for `videoTime - offsetMs/1000`
    // and the anchor lands in that same source-seconds domain;
    // update() compares against `videoPos - offset` so both sides of
    // the drift subtraction stay in one clock domain, which makes the
    // offset take effect continuously rather than only on the next
    // seek. End result: audio plays `offsetMs` later than video
    // (positive offset = compensates for video display + pipeline lag).
    std::atomic<int>                  m_syncOffsetMs{0};
};

} // namespace ufbplayer
