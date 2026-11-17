#pragma once

#include "wavplay/playback/segment.h"

#include <windows.h>
#include <unknwn.h>
#include "wavplay/asio/asio_sdk.h"
#include "wavplay/asio/driver_session.h"
#include "framework/test_suite.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace wavplay_test {

// The CLSID test sessions open. RegisteredDriver refuses every other one.
inline constexpr CLSID selected{0x76543210, 0x89ab, 0xcdef, {0x81, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef}};
// Exact-block rehearsals put PCM in prefilled B. Start silence has its own tests.
inline constexpr std::chrono::milliseconds no_start_silence{};

inline bool in_sta()
{
    APTTYPE type{};
    APTTYPEQUALIFIER qualifier{};
    return CoGetApartmentType(&type, &qualifier) == S_OK && (type == APTTYPE_STA || type == APTTYPE_MAINSTA);
}

// Threading: the product calls the IASIO methods on its session thread. tick()
// may run on any thread; it and the transport state changes in start(), stop()
// and disposeBuffers() hold gate_, which models the driver serializing its
// callbacks against its own stop.
class MockAsio : public IASIO {
public:
    MockAsio() = default;
    MockAsio(const MockAsio&) = delete;
    MockAsio& operator=(const MockAsio&) = delete;
    ~MockAsio() = default; // Not virtual: the IASIO vtable prefix stays exactly the SDK's.

    // An ActivateDriver bound to this driver, for a DriverSession or controller
    // that opens it (and reopens it between different-rate tracks) on its own
    // thread. The driver must outlive every session that holds the binding.
    [[nodiscard]] wavplay::asio::ActivateDriver activation()
    {
        return [this](const CLSID& clsid) { return activated(clsid); };
    }

    static constexpr long width = 2;
    using Channel = std::vector<std::uint8_t>;
    using Block = std::array<Channel, 2>;

    enum class BufferFault { none, overlap, changed_type, changed_rate, inactive_output, swapped_channels, input_buffer };
    enum class Call { rate, can_rate, set_rate, size, channel, create, latency, start, stop, dispose, channels };

    struct BufferSize {
        long minimum, maximum, preferred, granularity;
    };
    BufferSize buffer_size{width, width, width, 0};

    struct Run {
        ASIOSampleRate rate;
        long width, stopped_at;
        std::vector<Block> submitted;
    };

    ASIOSampleRate sample_rate = 48000;
    long next_width = 3, next_latency = 4;
    ASIOSampleType next_left_type = ASIOSTInt16LSB, next_right_type = ASIOSTInt16LSB;
    ASIOError can_rate_result = ASE_OK, set_rate_result = ASE_OK;
    bool keep_old_rate{}, no_clock_after_set{};
    std::function<void(Call)> on_call; // Invoked on entry, before returning or changing device state.
    unsigned preroll_switches = 1;
    std::vector<unsigned> ticks_after_start; // Hardware clock ticks for each start attempt.
    std::vector<Run> runs;
    std::vector<Call> calls;
    std::vector<ASIOSampleRate> set_rates;

    long latency = 3;
    long outputs = 2;
    long first_output{}; // Zero-based physical left output accepted by this fixture.
    std::vector<long> queried_outputs;
    std::vector<std::array<long, 2>> created_outputs;
    ASIOError sample_rate_result = ASE_OK, buffer_size_result = ASE_OK;
    ASIOError left_info_result = ASE_OK, right_info_result = ASE_OK;
    ASIOSampleType left_type = ASIOSTInt16LSB, right_type = ASIOSTInt16LSB;
    BufferFault buffer_fault{};
    bool fail_start{}, fail_stop{}, fail_dispose{}, untouched_on_dispose{}, late_switch_untouched{};
    long fault_at = -1;
    std::optional<wavplay::playback::PlaybackResult> state_before_fault;
    long fault_selector = kAsioOverload;
    long fault_supported{}, fault_reply{}, size_change_supported{};
    unsigned size_queries{}, creates{}, starts{}, stops{}, disposals{};
    long stopped_at = -1;
    bool silent_on_stop{}, peer_untouched = true;
    bool first_a_silent{};
    std::vector<Block> submitted; // Pre-filled B, then one snapshot per callback.
    std::promise<void> started;
    decltype(ASIOCallbacks::sampleRateDidChange) rate_notification{};
    decltype(ASIOCallbacks::bufferSwitch) switch_notification{};
    decltype(ASIOCallbacks::asioMessage) message_notification{};

    // Model the driver's callback serialization and stop barrier.
    void tick()
    {
        const std::lock_guard lock(gate_);
        if (running_) {
            hardware_frame_ += active_width_;
            switch_buffer();
        }
    }

    ASIOError createBuffers(ASIOBufferInfo* infos, long count, long size, ASIOCallbacks* callbacks) override
    {
        record_call(Call::create);
        const bool in_range = buffer_size.minimum > 0 && size >= buffer_size.minimum && size <= buffer_size.maximum;
        // Keep the mock's acceptance independent of the product selector.
        const bool valid_step = in_range && (buffer_size.granularity == 0 ||
            (buffer_size.granularity == -1 && (size & (size - 1)) == 0) ||
            (buffer_size.granularity > 0 && (size - buffer_size.minimum) % buffer_size.granularity == 0));
        if (callbacks_ || count != 2 || !in_range || !valid_step || !callbacks ||
            !callbacks->bufferSwitch || !callbacks->sampleRateDidChange || !callbacks->asioMessage) {
            return ASE_InvalidParameter;
        }
        for (long ch = 0; ch < count; ++ch) {
            if (infos[ch].isInput || infos[ch].channelNum != first_output + ch) {
                return ASE_InvalidParameter;
            }
            const auto type = ch == 0 ? left_type : right_type;
            // Model SDK storage widths independently of the product decoder.
            std::size_t sample_bytes{};
            switch (type) {
            case ASIOSTInt16LSB: case ASIOSTInt16MSB:
                sample_bytes = 2; break;
            case ASIOSTInt24LSB: case ASIOSTInt24MSB:
                sample_bytes = 3; break;
            case ASIOSTInt32LSB: case ASIOSTInt32MSB:
            case ASIOSTInt32LSB16: case ASIOSTInt32MSB16:
            case ASIOSTInt32LSB18: case ASIOSTInt32MSB18:
            case ASIOSTInt32LSB20: case ASIOSTInt32MSB20:
            case ASIOSTInt32LSB24: case ASIOSTInt32MSB24:
            case ASIOSTFloat32LSB: case ASIOSTFloat32MSB:
                sample_bytes = 4; break;
            case ASIOSTFloat64LSB: case ASIOSTFloat64MSB:
                sample_bytes = 8; break;
            default:
                return ASE_InvalidParameter;
            }
            for (std::size_t index = 0; index < halves_.size(); ++index) {
                halves_[index][ch].resize(static_cast<std::size_t>(size) * sample_bytes);
                infos[ch].buffers[index] = halves_[index][ch].data();
            }
        }
        for (auto& half : halves_) {
            poison(half);
        }
        if (buffer_fault == BufferFault::overlap) {
            // Two distinct pointers, but the last byte overlaps the other half.
            infos[1].buffers[1] = &halves_[0][0].back();
        }
        if (buffer_fault == BufferFault::swapped_channels) {
            std::swap(infos[0].channelNum, infos[1].channelNum);
        } else if (buffer_fault == BufferFault::input_buffer) {
            infos[1].isInput = ASIOTrue;
        }
        callbacks_ = callbacks;
        created_outputs.push_back({first_output, first_output + 1});
        active_width_ = size;
        switch_notification = callbacks->bufferSwitch;
        rate_notification = callbacks->sampleRateDidChange;
        message_notification = callbacks->asioMessage;
        ++creates;
        return ASE_OK;
    }

    ASIOError start() override
    {
        record_call(Call::start);
        const std::lock_guard lock(gate_);
        ++starts;
        if (!callbacks_) {
            return ASE_HWMalfunction;
        }
        switches_ = 0;
        hardware_frame_ = 0;
        submitted.clear();
        size_change_supported = callbacks_->asioMessage(kAsioSelectorSupported, kAsioBufferSizeChange, nullptr, nullptr);
        running_ = true;
        first_a_silent = silent(halves_[0]);
        submitted.push_back(halves_[1]);
        for (unsigned i = 0; i < preroll_switches; ++i) {
            switch_buffer(); // Hardware time is still zero throughout pre-roll.
        }
        if (fail_start) {
            return ASE_HWMalfunction; // Hardware failed after the synchronous callback.
        }
        if (ticks_after_start.empty()) {
            started.set_value();
        } else {
            const auto ticks = ticks_after_start.at(starts - 1);
            clock_ = std::jthread([this, ticks](std::stop_token stop) {
                // A signal from inside start() cannot synchronize with its return.
                while (!stop.stop_requested() && !wavplay::playback::is_streaming()) {
                    std::this_thread::yield();
                }
                for (unsigned i = 0; !stop.stop_requested() && i < ticks; ++i) {
                    tick();
                }
            });
        }
        return ASE_OK;
    }

    ASIOError stop() override
    {
        if (clock_.joinable()) {
            clock_.request_stop();
            clock_.join();
        }
        const std::lock_guard lock(gate_);
        record_call(Call::stop);
        if (!running_) {
            return ASE_InvalidMode;
        }
        if (fail_stop) {
            return ASE_NotPresent; // Not start's code, so a message shows which call it came from.
        }
        running_ = false;
        ++stops;
        stopped_at = hardware_frame_;
        const long index = switches_ % 2;
        poison(halves_[index]);
        callbacks_->bufferSwitch(index, ASIOFalse);
        silent_on_stop = silent(halves_[index]);
        if (!ticks_after_start.empty()) {
            runs.push_back({sample_rate, active_width_, stopped_at, submitted});
        }
        return ASE_OK;
    }

    ASIOError disposeBuffers() override
    {
        record_call(Call::dispose);
        if (!callbacks_) {
            return ASE_InvalidMode;
        }
        ++disposals;
        if (running_) {
            // A stray driver thread can arrive while buffers are being retired.
            late_switch_untouched = std::async(std::launch::async,
                [this] { return detached_callbacks_preserve_output(); }).get();
            const std::lock_guard lock(gate_);
            // The implicit stop's synchronous request presents the half as the host left it.
            submit_half(false);
            running_ = false;
        }
        auto initial = halves_[0];
        poison(initial);
        untouched_on_dispose = halves_[0] == initial && halves_[1] == initial;
        if (fail_dispose) {
            return ASE_HWMalfunction;
        }
        callbacks_ = nullptr;
        return ASE_OK;
    }

    bool detached_callbacks_preserve_output()
    {
        poison(halves_[0]);
        const auto before = halves_[0];
        callbacks_->bufferSwitch(0, ASIOFalse);
        callbacks_->sampleRateDidChange(0);
        callbacks_->asioMessage(kAsioOverload, 0, nullptr, nullptr);
        return halves_[0] == before;
    }

    ASIOError getLatencies(long* in, long* out) override
    {
        record_call(Call::latency);
        if (!callbacks_) {
            return ASE_InvalidMode;
        }
        *in = 0;
        *out = latency;
        return ASE_OK;
    }

    ASIOError getChannels(long* in, long* out) override
    {
        record_call(Call::channels);
        *in = 0;
        *out = outputs;
        return outputs == 0 ? ASE_NotPresent : ASE_OK;
    }
    ASIOError getBufferSize(long* min, long* max, long* preferred, long* step) override
    {
        record_call(Call::size);
        ++size_queries;
        *min = buffer_size.minimum;
        *max = buffer_size.maximum;
        *preferred = buffer_size.preferred;
        *step = buffer_size.granularity;
        return buffer_size_result;
    }
    ASIOError getChannelInfo(ASIOChannelInfo* info) override
    {
        record_call(Call::channel);
        queried_outputs.push_back(info->channel);
        if (info->channel == first_output && left_info_result != ASE_OK) {
            return left_info_result;
        }
        if (info->channel == first_output + 1 && right_info_result != ASE_OK) {
            return right_info_result;
        }
        if (info->isInput || info->channel < 0 || info->channel >= outputs) {
            return ASE_InvalidParameter;
        }
        const bool in_pair = info->channel == first_output || info->channel == first_output + 1;
        info->isActive = callbacks_ && in_pair && !(buffer_fault == BufferFault::inactive_output && info->channel == first_output + 1)
            ? ASIOTrue : ASIOFalse;
        info->type = !in_pair ? ASIOSTDSDInt8LSB1 : info->channel == first_output ? left_type : right_type;
        if (callbacks_ && buffer_fault == BufferFault::changed_type && info->channel == first_output + 1) {
            info->type = right_type == ASIOSTInt32LSB ? ASIOSTInt24LSB : ASIOSTInt32LSB;
        }
        return ASE_OK;
    }
    ASIOError getSampleRate(ASIOSampleRate* rate) override
    {
        record_call(Call::rate);
        if (no_clock_after_set && !set_rates.empty()) {
            *rate = 0;
            return ASE_NoClock;
        }
        *rate = callbacks_ && buffer_fault == BufferFault::changed_rate ?
            (sample_rate == 44100 ? 48000 : 44100) : sample_rate;
        return sample_rate_result;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    ASIOBool init(void*) override { return ASIOTrue; }
    void getDriverName(char* name) override { *name = '\0'; }
    long getDriverVersion() override { return 0; }
    void getErrorMessage(char* error) override { *error = '\0'; }
    ASIOError canSampleRate(ASIOSampleRate rate) override
    {
        record_call(Call::can_rate);
        return rate == 44100 || rate == 48000 ? can_rate_result : ASE_NoClock;
    }
    ASIOError setSampleRate(ASIOSampleRate rate) override
    {
        record_call(Call::set_rate);
        set_rates.push_back(rate);
        if (callbacks_) {
            return ASE_InvalidMode;
        }
        if (rate != 44100 && rate != 48000) {
            return ASE_NoClock;
        }
        if (set_rate_result != ASE_OK) {
            return set_rate_result;
        }
        if (!keep_old_rate) {
            sample_rate = rate;
            buffer_size = {next_width, next_width, next_width, 0};
            latency = next_latency;
            left_type = next_left_type;
            right_type = next_right_type;
        }
        return ASE_OK;
    }
    ASIOError getClockSources(ASIOClockSource*, long*) override { return ASE_NotPresent; }
    ASIOError setClockSource(long) override { return ASE_NotPresent; }
    ASIOError getSamplePosition(ASIOSamples*, ASIOTimeStamp*) override { return ASE_NotPresent; }
    ASIOError controlPanel() override { return ASE_NotPresent; }
    ASIOError future(long, void*) override { return ASE_NotPresent; }
    ASIOError outputReady() override { return ASE_NotPresent; }

    static bool silent(const Block& block)
    {
        return std::ranges::all_of(block, [](const Channel& channel) {
            return std::ranges::all_of(channel, [](std::uint8_t byte) { return byte == 0; });
        });
    }

protected:
    // Each session open and reopen. Doubles that model registration and reference
    // counting override this.
    virtual IASIO* activated(const CLSID&) { return this; }
    // A derived double's contract checks on every recorded call, before on_call,
    // so a test that installs its own on_call cannot disable them. Runs on the
    // calling thread, possibly with gate_ held: record a flag, never throw or lock.
    virtual void check_entry(Call) {}

private:
    void record_call(Call call)
    {
        calls.push_back(call);
        check_entry(call);
        if (on_call) {
            on_call(call);
        }
    }

    static void poison(Block& block)
    {
        for (auto& channel : block) {
            std::ranges::fill(channel, 0xa5);
        }
    }

    void switch_buffer()
    {
        submit_half();
        if (hardware_frame_ == fault_at) {
            if (!state_before_fault) { // Pre-roll can inject the fault repeatedly at hardware frame zero.
                state_before_fault = wavplay::playback::notifications().result();
            }
            fault_supported = callbacks_->asioMessage(kAsioSelectorSupported, fault_selector, nullptr, nullptr);
            if (fault_selector != kAsioOverload) {
                // Also exercise an unsolicited notification the host declined.
                fault_reply = callbacks_->asioMessage(fault_selector, 0, nullptr, nullptr);
            }
            // Preserve the first fatal fault, or supersede a reinitialization request.
            callbacks_->asioMessage(kAsioOverload, 0, nullptr, nullptr);
            submit_half(); // A callback after the fault but before stop can enter.
        }
    }

    void submit_half(bool poison_first = true)
    {
        const long index = switches_++ % 2;
        const auto peer = halves_[1 - index];
        if (poison_first) {
            poison(halves_[index]); // Unwritten tail bytes must not masquerade as silence.
        }
        callbacks_->bufferSwitch(index, ASIOFalse);
        peer_untouched &= halves_[1 - index] == peer;
        submitted.push_back(halves_[index]);
    }

    std::mutex gate_;
    bool running_{};
    long switches_{};
    long hardware_frame_{};
    long active_width_{};
    ASIOCallbacks* callbacks_{};
    std::array<Block, 2> halves_{};
    std::jthread clock_;
};

// A DriverSession that opens and reopens this driver. Construct it on the thread
// that plays, which owns the session's STA. Like any session, it restores a clock
// its requests changed when it closes, and reopens the driver before a
// different-rate second track. Streams start with PCM in prefilled B by default.
class MockSession : public wavplay::asio::DriverSession {
public:
    explicit MockSession(MockAsio& driver, wavplay::asio::DeviceSettings settings = {.start_silence = no_start_silence})
        : DriverSession(selected, GetDesktopWindow(), driver.activation(), settings)
    {
    }
};

inline void expect_call_order(const MockAsio& driver, std::initializer_list<MockAsio::Call> sequence,
    std::source_location location = std::source_location::current())
{
    auto next = driver.calls.begin();
    for (const auto call : sequence) {
        next = std::find(next, driver.calls.end(), call);
        if (next == driver.calls.end()) {
            fail("ASIO call missing or out of order: " + std::to_string(static_cast<int>(call)) +
                detail::location_suffix(location));
        }
        ++next;
    }
}

inline void expect_calls(const char* field, const std::vector<MockAsio::Call>& actual, std::initializer_list<MockAsio::Call> expected,
    std::source_location location = std::source_location::current())
{
    if (!std::ranges::equal(actual, expected)) {
        std::string listed;
        for (const auto call : actual) {
            listed += std::to_string(static_cast<int>(call)) + ' ';
        }
        fail(std::string(field) + ": unexpected ASIO calls " + listed + detail::location_suffix(location));
    }
}

} // namespace wavplay_test
