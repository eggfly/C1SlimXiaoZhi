#ifndef C1XZ_AUDIO_AUDIO_SERVICE_H
#define C1XZ_AUDIO_AUDIO_SERVICE_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "audio/audio_codec.h"
#include "audio/opus_codec.h"
#include "protocols/protocol.h"

// Same frame length as the ESP32 firmware; it is part of the hello message the
// server sees, so changing it changes the wire contract.
#define OPUS_FRAME_DURATION_MS 60

// Queue depths, in packets. Kept small: on a 1 GHz single core with a 685 ms
// e-paper refresh, a deep queue just turns a stall into latency.
#define MAX_DECODE_PACKETS_IN_QUEUE (1200 / OPUS_FRAME_DURATION_MS)
#define MAX_SEND_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)

struct AudioServiceCallbacks {
    std::function<void()> on_send_queue_available;
    std::function<void(const std::string&)> on_wake_word_detected;
    std::function<void(bool)> on_vad_change;
    std::function<void()> on_playback_drained;
};

// Owns the audio threads and the Opus codecs.
//
//   microphone -> [input thread] -> encode -> {send queue} -> application
//   application -> {decode queue} -> [output thread] -> decode -> speaker
//
// Two threads rather than upstream's four: this core cannot usefully run more,
// and folding the Opus work into the threads that already own the data saves a
// queue and a copy in each direction.
class AudioService {
public:
    AudioService();
    ~AudioService();

    AudioService(const AudioService&) = delete;
    AudioService& operator=(const AudioService&) = delete;

    bool Initialize(AudioCodec* codec);
    void Start();
    void Stop();

    void SetCallbacks(const AudioServiceCallbacks& callbacks);

    // Opens the capture path and starts producing encoded uplink packets.
    void EnableVoiceProcessing(bool enable);
    bool IsVoiceProcessingRunning() const { return voice_processing_.load(); }

    // Reserved for a future local wake word. Today the device wakes on a key
    // press, so this is accepted and ignored.
    void EnableWakeWordDetection(bool enable);
    bool IsWakeWordRunning() const { return wake_word_running_.load(); }

    bool IsVoiceDetected() const { return voice_detected_.load(); }

    // True once everything queued has been played out.
    bool IsIdle();
    bool IsPlaybackIdle();

    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait = false);
    std::unique_ptr<AudioStreamPacket> PopPacketFromSendQueue();

    // Drops everything queued in both directions. Used when a response is
    // aborted so the speaker stops immediately.
    void ClearQueues();

    void ResetDecoder();

    // Plays one of the compiled-in Ogg Opus prompts.
    void PlaySound(std::string_view ogg_data);

    void SetOutputVolume(int volume);
    int output_volume() const;

    // Mean absolute level of the last captured frame, as
    // 20*log10(mean|sample|). Same cheap metric the factory recorder uses; the
    // scale runs to about 90 for full scale.
    int input_level_db() const { return input_level_db_.load(); }

    // Silence threshold and hang time for the VAD that drives auto-stop.
    void SetVadThresholdDb(int db) { vad_threshold_db_ = db; }

private:
    AudioCodec* codec_ = nullptr;
    AudioServiceCallbacks callbacks_;

    std::unique_ptr<c1xz::OpusEncoderWrapper> encoder_;
    std::unique_ptr<c1xz::OpusDecoderWrapper> decoder_;
    std::mutex decoder_mutex_;

    std::thread input_thread_;
    std::thread output_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> voice_processing_{false};
    std::atomic<bool> wake_word_running_{false};
    std::atomic<bool> voice_detected_{false};
    std::atomic<int> input_level_db_{0};

    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<std::unique_ptr<AudioStreamPacket>> decode_queue_;
    std::deque<std::unique_ptr<AudioStreamPacket>> send_queue_;
    std::deque<std::vector<int16_t>> prompt_queue_;
    bool output_in_flight_ = false;
    bool playback_drained_notified_ = true;

    int vad_threshold_db_ = 45;
    int vad_silence_frames_ = 0;

    void InputLoop();
    void OutputLoop();
    void WritePcm(const std::vector<int16_t>& pcm);
    void NotifyPlaybackDrainedLocked();
};

#endif  // C1XZ_AUDIO_AUDIO_SERVICE_H
