#include "audio/audio_service.h"

#include <cmath>
#include <cstring>

#include "audio/ogg_demuxer.h"
#include "platform/event_loop.h"
#include "platform/log.h"

#define TAG "Audio"

namespace {

// 20*log10(mean|sample|), the metric the factory recorder uses. Returns 0 for
// digital silence rather than negative infinity.
int MeanAbsoluteDb(const int16_t* samples, size_t count) {
    if (count == 0) {
        return 0;
    }
    double sum = 0;
    for (size_t i = 0; i < count; ++i) {
        sum += std::abs(static_cast<int>(samples[i]));
    }
    double mean = sum / static_cast<double>(count);
    if (mean <= 0) {
        return 0;
    }
    return static_cast<int>(20.0 * std::log10(mean));
}

}  // namespace

AudioService::AudioService() = default;

AudioService::~AudioService() { Stop(); }

bool AudioService::Initialize(AudioCodec* codec) {
    codec_ = codec;
    if (codec_ == nullptr) {
        return false;
    }

    encoder_.reset(new c1xz::OpusEncoderWrapper());
    if (!encoder_->Configure(16000, 1, OPUS_FRAME_DURATION_MS)) {
        C1XZ_LOGE(TAG, "failed to configure the Opus encoder");
        return false;
    }

    decoder_.reset(new c1xz::OpusDecoderWrapper());
    // Decode straight to the hardware's playback rate: libopus resamples
    // internally, so a 24 kHz stream from the server needs no extra stage.
    if (!decoder_->Configure(codec_->output_sample_rate(), 1, OPUS_FRAME_DURATION_MS)) {
        C1XZ_LOGE(TAG, "failed to configure the Opus decoder");
        return false;
    }
    return true;
}

void AudioService::SetCallbacks(const AudioServiceCallbacks& callbacks) {
    callbacks_ = callbacks;
}

void AudioService::Start() {
    if (running_.exchange(true)) {
        return;
    }
    input_thread_ = std::thread([this] { InputLoop(); });
    output_thread_ = std::thread([this] { OutputLoop(); });
}

void AudioService::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    voice_processing_.store(false);
    queue_cv_.notify_all();
    if (input_thread_.joinable()) {
        input_thread_.join();
    }
    if (output_thread_.joinable()) {
        output_thread_.join();
    }
}

void AudioService::EnableVoiceProcessing(bool enable) {
    bool was = voice_processing_.exchange(enable);
    if (was == enable) {
        return;
    }
    if (enable) {
        vad_silence_frames_ = 0;
        voice_detected_.store(false);
    }
    queue_cv_.notify_all();
    C1XZ_LOGI(TAG, "voice processing %s", enable ? "on" : "off");
}

void AudioService::EnableWakeWordDetection(bool enable) {
    // No local wake word in this build: the device wakes on a key press. The
    // flag is tracked so the application's state machine behaves the same.
    wake_word_running_.store(enable);
}

void AudioService::InputLoop() {
    const int frame_samples = encoder_->frame_size();
    std::vector<int16_t> pcm(static_cast<size_t>(frame_samples));
    std::vector<uint8_t> encoded;

    while (running_.load()) {
        if (!voice_processing_.load()) {
            if (codec_->input_enabled()) {
                codec_->EnableInput(false);
            }
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(100));
            continue;
        }

        if (!codec_->input_enabled()) {
            codec_->EnableInput(true);
        }

        int read = codec_->Read(pcm.data(), frame_samples);
        if (read < 0) {
            C1XZ_LOGE(TAG, "capture failed, pausing the uplink");
            voice_processing_.store(false);
            continue;
        }
        if (read < frame_samples) {
            continue;
        }

        int level = MeanAbsoluteDb(pcm.data(), pcm.size());
        input_level_db_.store(level);

        bool speaking = level >= vad_threshold_db_;
        if (speaking) {
            vad_silence_frames_ = 0;
        } else {
            ++vad_silence_frames_;
        }
        // Roughly half a second of quiet before declaring silence, so a pause
        // between words does not end the turn.
        bool detected = speaking || vad_silence_frames_ < (500 / OPUS_FRAME_DURATION_MS);
        if (detected != voice_detected_.load()) {
            voice_detected_.store(detected);
            if (callbacks_.on_vad_change) {
                callbacks_.on_vad_change(detected);
            }
        }

        if (!encoder_->Encode(pcm.data(), &encoded)) {
            continue;
        }

        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = 16000;
        packet->frame_duration = OPUS_FRAME_DURATION_MS;
        packet->timestamp = static_cast<uint32_t>(c1xz::MonotonicMs());
        packet->payload = encoded;

        bool notify = false;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (send_queue_.size() >= MAX_SEND_PACKETS_IN_QUEUE) {
                // Drop the oldest: stale uplink audio is worse than a gap.
                send_queue_.pop_front();
            }
            send_queue_.push_back(std::move(packet));
            notify = true;
        }
        if (notify && callbacks_.on_send_queue_available) {
            callbacks_.on_send_queue_available();
        }
    }

    if (codec_ != nullptr) {
        codec_->EnableInput(false);
    }
}

void AudioService::WritePcm(const std::vector<int16_t>& pcm) {
    if (pcm.empty()) {
        return;
    }
    if (!codec_->output_enabled()) {
        codec_->EnableOutput(true);
    }
    codec_->Write(pcm.data(), static_cast<int>(pcm.size()));
}

void AudioService::NotifyPlaybackDrainedLocked() {
    if (playback_drained_notified_) {
        return;
    }
    if (!decode_queue_.empty() || !prompt_queue_.empty() || output_in_flight_) {
        return;
    }
    playback_drained_notified_ = true;
    if (callbacks_.on_playback_drained) {
        callbacks_.on_playback_drained();
    }
}

void AudioService::OutputLoop() {
    std::vector<int16_t> pcm;
    int idle_frames = 0;

    while (running_.load()) {
        std::vector<int16_t> prompt;
        std::unique_ptr<AudioStreamPacket> packet;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            if (prompt_queue_.empty() && decode_queue_.empty()) {
                NotifyPlaybackDrainedLocked();
                queue_cv_.wait_for(lock, std::chrono::milliseconds(100));
                if (prompt_queue_.empty() && decode_queue_.empty()) {
                    // Close the DAC after a while so the codec can power down.
                    if (++idle_frames > 150 && codec_ != nullptr && codec_->output_enabled()) {
                        lock.unlock();
                        codec_->EnableOutput(false);
                        lock.lock();
                        idle_frames = 0;
                    }
                    continue;
                }
            }
            idle_frames = 0;
            // Prompts jump the queue: they are short and are feedback for
            // something the user just did.
            if (!prompt_queue_.empty()) {
                prompt = std::move(prompt_queue_.front());
                prompt_queue_.pop_front();
            } else {
                packet = std::move(decode_queue_.front());
                decode_queue_.pop_front();
            }
            output_in_flight_ = true;
            playback_drained_notified_ = false;
        }

        if (!prompt.empty()) {
            WritePcm(prompt);
        } else if (packet != nullptr) {
            bool ok;
            {
                std::lock_guard<std::mutex> lock(decoder_mutex_);
                ok = decoder_->Decode(packet->payload.data(), packet->payload.size(), &pcm);
            }
            if (ok) {
                WritePcm(pcm);
            }
        }

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            output_in_flight_ = false;
            NotifyPlaybackDrainedLocked();
        }
    }

    if (codec_ != nullptr) {
        codec_->EnableOutput(false);
    }
}

bool AudioService::PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait) {
    if (packet == nullptr) {
        return false;
    }
    std::unique_lock<std::mutex> lock(queue_mutex_);
    if (decode_queue_.size() >= MAX_DECODE_PACKETS_IN_QUEUE) {
        if (!wait) {
            C1XZ_LOGW(TAG, "decode queue full, dropping a packet");
            return false;
        }
        queue_cv_.wait_for(lock, std::chrono::milliseconds(200), [this] {
            return decode_queue_.size() < MAX_DECODE_PACKETS_IN_QUEUE || !running_.load();
        });
        if (decode_queue_.size() >= MAX_DECODE_PACKETS_IN_QUEUE) {
            return false;
        }
    }
    decode_queue_.push_back(std::move(packet));
    playback_drained_notified_ = false;
    lock.unlock();
    queue_cv_.notify_all();
    return true;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (send_queue_.empty()) {
        return nullptr;
    }
    auto packet = std::move(send_queue_.front());
    send_queue_.pop_front();
    return packet;
}

void AudioService::ClearQueues() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    decode_queue_.clear();
    send_queue_.clear();
    prompt_queue_.clear();
}

bool AudioService::IsIdle() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return decode_queue_.empty() && send_queue_.empty() && prompt_queue_.empty() &&
           !output_in_flight_ && !voice_processing_.load();
}

bool AudioService::IsPlaybackIdle() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return decode_queue_.empty() && prompt_queue_.empty() && !output_in_flight_;
}

void AudioService::ResetDecoder() {
    std::lock_guard<std::mutex> lock(decoder_mutex_);
    decoder_->Reset();
}

void AudioService::PlaySound(std::string_view ogg_data) {
    if (ogg_data.empty() || codec_ == nullptr) {
        return;
    }
    c1xz::OggDemuxer::OpusHead head;
    std::vector<std::vector<uint8_t>> packets;
    if (!c1xz::OggDemuxer::ParseToPackets(ogg_data, &head, &packets) || packets.empty()) {
        C1XZ_LOGE(TAG, "could not read the prompt sound");
        return;
    }

    // Prompts are decoded on the caller's thread into the hardware rate, then
    // queued as raw PCM so they do not disturb the conversation decoder state.
    c1xz::OpusDecoderWrapper decoder;
    if (!decoder.Configure(codec_->output_sample_rate(), 1, OPUS_FRAME_DURATION_MS)) {
        return;
    }
    std::vector<int16_t> pcm;
    std::vector<int16_t> merged;
    for (const auto& packet : packets) {
        if (decoder.Decode(packet.data(), packet.size(), &pcm)) {
            merged.insert(merged.end(), pcm.begin(), pcm.end());
        }
    }
    if (merged.empty()) {
        return;
    }
    // Drop the encoder delay the OpusHead declares, otherwise every prompt
    // starts with a short burst of noise.
    size_t skip = static_cast<size_t>(head.pre_skip) * codec_->output_sample_rate() / 48000;
    if (skip < merged.size()) {
        merged.erase(merged.begin(), merged.begin() + skip);
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        prompt_queue_.push_back(std::move(merged));
        playback_drained_notified_ = false;
    }
    queue_cv_.notify_all();
}

void AudioService::SetOutputVolume(int volume) {
    if (codec_ != nullptr) {
        codec_->SetOutputVolume(volume);
    }
}

int AudioService::output_volume() const {
    return codec_ != nullptr ? codec_->output_volume() : 0;
}
