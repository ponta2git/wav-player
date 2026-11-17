#pragma once

#include "doubles/mock_asio.h"
#include "fixtures/sample_oracle.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

namespace wavplay_test {

// Unlike MockAsio::submitted, this queue models samples that reach the DAC.
// stop discards pending samples, so a test that advances only to a prefetched
// or submitted position cannot pass a comparison of heard. Its clock, transport
// state and per-start output (dac_*) replace MockAsio's tick() and runs, which
// stay unused.
class DacQueueAsio final : public MockAsio {
public:
    enum class Position { advancing, zero, unsupported };
    using Frame = std::array<std::array<std::uint8_t, 8>, 2>;
    static constexpr long block = 48;

    explicit DacQueueAsio(Position position, bool packed24 = false)
        : DacQueueAsio(position, static_cast<ASIOSampleType>(packed24 ? ASIOSTInt24LSB : ASIOSTInt16LSB)) {}

    DacQueueAsio(Position position, ASIOSampleType type)
        : position_(position), bytes_(output_sample_bytes(type))
    {
        buffer_size = {block, block, block, 0};
        latency = 73; // Neither a block boundary nor the prefilled B's lead.
        left_type = right_type = type;
    }

    ASIOError createBuffers(ASIOBufferInfo* infos, long count, long size, ASIOCallbacks* callbacks) override
    {
        const auto result = MockAsio::createBuffers(infos, count, size, callbacks);
        if (result == ASE_OK) {
            for (std::size_t channel = 0; channel < outputs_.size(); ++channel) {
                outputs_[channel] = infos[channel];
            }
            host_callbacks_ = callbacks;
        }
        return result;
    }

    ASIOError start() override
    {
        const std::lock_guard lock(dac_gate_);
        ++starts;
        dac_running_ = true;
        dac_switches_ = 0;
        dac_frame_ = 0;
        // Even a driver reporting less than one block still consumes the
        // prefilled B before subsequent blocks. Only excess latency delays it.
        initial_delay_ = std::max(0L, latency - block);
        source_submitted_ = 0;
        submitted_frames_ = 0;
        pending_.clear();
        dac_runs.emplace_back();
        enqueue(1); // B is already filled, before the first A callback.
        if (before_preroll) {
            before_preroll();
        }
        for (unsigned i = 0; i < preroll_switches; ++i) {
            switch_next(); // All these submissions occur at hardware frame zero.
        }
        started.set_value();
        return fail_start ? ASE_HWMalfunction : ASE_OK;
    }

    ASIOError stop() override
    {
        const std::lock_guard lock(dac_gate_);
        ++stops;
        if (before_stop) {
            before_stop();
        }
        if (fault_on_stop) {
            host_callbacks_->asioMessage(fault_on_stop, 0, nullptr, nullptr);
        }
        if (fail_stop) {
            return ASE_HWMalfunction;
        }
        dac_running_ = false;
        discard_pending();
        host_callbacks_->bufferSwitch(dac_switches_++ % 2, ASIOFalse);
        return ASE_OK;
    }

    ASIOError disposeBuffers() override
    {
        {
            const std::lock_guard lock(dac_gate_);
            if (fault_on_dispose) {
                host_callbacks_->asioMessage(fault_on_dispose, 0, nullptr, nullptr);
            }
            dac_running_ = false; // Also quiesce the failed-stop path.
            discard_pending();
        }
        const auto result = MockAsio::disposeBuffers();
        host_callbacks_ = nullptr;
        return result;
    }

    ASIOError getSamplePosition(ASIOSamples* position, ASIOTimeStamp* stamp) override
    {
        const std::lock_guard lock(dac_gate_);
        ++position_queries;
        *stamp = {};
        position->hi = 0;
        position->lo = position_ == Position::advancing ? static_cast<unsigned long>(dac_frame_) : 0;
        return position_ == Position::unsupported ? ASE_NotPresent : ASE_OK;
    }

    void advance_dac_block()
    {
        const std::lock_guard lock(dac_gate_);
        if (!dac_running_) {
            return;
        }
        for (long i = 0; i < block; ++i) {
            ++dac_frame_;
            if (initial_delay_) {
                --initial_delay_;
            } else if (!pending_.empty()) {
                const auto frame = pending_.front();
                pending_.pop_front();
                dac_runs.back().push_back(frame);
                if (!silent_frame(frame)) {
                    heard.push_back(frame);
                }
            } else {
                ++starved;
            }
        }
        switch_next();
    }

    std::uint64_t source_submitted()
    {
        const std::lock_guard lock(dac_gate_);
        return source_submitted_;
    }

    std::uint64_t submitted_frames()
    {
        const std::lock_guard lock(dac_gate_);
        return submitted_frames_;
    }

    void fault(long selector)
    {
        const std::lock_guard lock(dac_gate_);
        host_callbacks_->asioMessage(selector, 0, nullptr, nullptr);
    }

    static bool silent_frame(const Frame& frame)
    {
        return frame == Frame{};
    }

    std::size_t sample_bytes() const noexcept { return bytes_; }
    std::vector<Frame> heard;
    std::vector<std::vector<Frame>> dac_runs;
    unsigned position_queries{}, discarded_pcm{}, starved{};
    long fault_on_stop{}, fault_on_dispose{};
    std::function<void()> before_preroll;
    std::function<void()> before_stop;

private:
    void enqueue(long half)
    {
        for (long slot = 0; slot < block; ++slot) {
            Frame frame{};
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const auto* data = static_cast<const std::uint8_t*>(outputs_[channel].buffers[half]);
                for (std::size_t byte = 0; byte < bytes_; ++byte) {
                    frame[channel][byte] = data[static_cast<std::size_t>(slot) * bytes_ + byte];
                }
            }
            pending_.push_back(frame);
            ++submitted_frames_;
            if (!silent_frame(frame)) {
                ++source_submitted_;
            }
        }
    }

    void switch_next()
    {
        const auto half = dac_switches_++ % 2;
        host_callbacks_->bufferSwitch(half, ASIOFalse);
        enqueue(half);
    }

    void discard_pending()
    {
        for (const auto& frame : pending_) {
            if (!silent_frame(frame)) {
                ++discarded_pcm;
            }
        }
        pending_.clear();
    }

    const Position position_;
    const std::size_t bytes_;
    std::mutex dac_gate_;
    std::array<ASIOBufferInfo, 2> outputs_{};
    ASIOCallbacks* host_callbacks_{};
    std::deque<Frame> pending_;
    bool dac_running_{};
    long dac_switches_{}, initial_delay_{};
    std::uint64_t dac_frame_{}, source_submitted_{}, submitted_frames_{};
};

} // namespace wavplay_test
