#include "wavplay/asio/driver_queries.h"

#include "wavplay/asio/asio_error.h"
#include "wavplay/asio/clock.h"
#include "wavplay/asio/device_settings.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace wavplay::asio {
namespace {

bool matches_granularity(const BufferSizeRange& size, long width) noexcept
{
    // Positive steps start at the minimum. ASIO reserves -1 for powers of two.
    return size.granularity == -1 ? std::has_single_bit(static_cast<unsigned long>(width))
        : size.granularity == 0 || (width - size.minimum) % size.granularity == 0;
}

// Null for a consistent driver range, otherwise why it contradicts itself.
const char* range_fault(const BufferSizeRange& size) noexcept
{
    if (size.minimum <= 0 || size.preferred < size.minimum || size.preferred > size.maximum) {
        return "Invalid preferred ASIO buffer size";
    }
    if (size.granularity < -1 || !matches_granularity(size, size.preferred)) {
        return "Preferred ASIO buffer size contradicts its granularity";
    }
    return nullptr;
}

void validate_buffer_size(const BufferSizeRange& size)
{
    if (const auto* fault = range_fault(size)) {
        throw std::runtime_error(fault);
    }
}

// Zero, or a width the range lists. Only for a range without a fault, so the
// step test never sees a width below a positive minimum.
bool fits(const BufferSizeRange& size, long requested) noexcept
{
    return requested == 0 ||
        (requested >= size.minimum && requested <= size.maximum && matches_granularity(size, requested));
}

// Check the entire mapping on the control thread, before either half is
// touched. The driver still owns its allocation and lifetime until disposal.
void validate_writable_range(std::uintptr_t address, std::size_t bytes)
{
    constexpr DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    while (bytes != 0) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) ||
            region.State != MEM_COMMIT || !(region.Protect & writable) || (region.Protect & PAGE_GUARD)) {
            throw std::runtime_error("ASIO output buffer is not writable");
        }
        const auto offset = address - reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        const auto covered = std::min(bytes, region.RegionSize - offset);
        address += covered;
        bytes -= covered;
    }
}

} // namespace

long output_channel_count(IASIO& driver)
{
    long inputs{}, outputs{};
    const auto result = driver.getChannels(&inputs, &outputs);
    if (result == ASE_NotPresent) {
        return 0; // No inputs or outputs.
    }
    throw_if_error(result, "ASIO getChannels failed");
    if (inputs < 0 || outputs < 0) {
        throw std::runtime_error("Invalid ASIO channel count");
    }
    return outputs;
}

BufferSizeRange query_buffer_size(IASIO& driver)
{
    BufferSizeRange size{};
    auto& [minimum, maximum, preferred, granularity] = size;
    throw_if_error(driver.getBufferSize(&minimum, &maximum, &preferred, &granularity), "ASIO getBufferSize failed");
    validate_buffer_size(size);
    return size;
}

bool accepts_buffer_frames(const BufferSizeRange& range, long requested) noexcept
{
    return !range_fault(range) && fits(range, requested);
}

std::size_t select_buffer_frames(const BufferSizeRange& range, long requested)
{
    validate_buffer_size(range);
    if (!fits(range, requested)) {
        throw std::invalid_argument(buffer_frames_refusal(range, requested));
    }
    return static_cast<std::size_t>(requested == 0 ? range.preferred : requested);
}

std::string buffer_frames_refusal(const BufferSizeRange& range, long requested, std::uint32_t sample_rate)
{
    // Integer formatting only: no std::format or floating-point text.
    std::string text;
    const auto field = [&text](const char* name, long long value) {
        text += name;
        text += std::to_string(value);
    };
    field("Unsupported ASIO buffer size: requested=", requested);
    field(", minimum=", range.minimum);
    field(", preferred=", range.preferred);
    field(", maximum=", range.maximum);
    field(", granularity=", range.granularity);
    if (sample_rate != 0) {
        field(", rate=", sample_rate);
    }
    return text;
}

ASIOError query_output_info(IASIO& driver, long channel, ASIOChannelInfo& info)
{
    info.channel = channel;
    info.isInput = ASIOFalse;
    const auto error = driver.getChannelInfo(&info);
    if (error == ASE_OK && (info.channel != channel || info.isInput != ASIOFalse)) {
        throw std::runtime_error("ASIO returned information for a different output");
    }
    return error;
}

std::optional<ASIOSampleRate> current_sample_rate(IASIO& driver)
{
    ASIOSampleRate rate{};
    const auto error = driver.getSampleRate(&rate);
    if (error == ASE_NoClock) {
        return std::nullopt;
    }
    throw_if_error(error, "ASIO getSampleRate failed");
    if (!is_valid_clock(rate)) {
        return std::nullopt;
    }
    return rate;
}

DriverCapabilities query_capabilities(IASIO& driver)
{
    DriverCapabilities caps{};
    const auto rate = current_sample_rate(driver);
    if (!rate) {
        throw std::runtime_error("ASIO getSampleRate returned no valid clock");
    }
    caps.sample_rate = *rate;
    caps.output_channels = output_channel_count(driver);
    if (caps.output_channels < 2) {
        throw std::runtime_error("ASIO outputs 1/2 are unavailable");
    }
    for (long ch = 0; ch < 2; ++ch) {
        ASIOChannelInfo info{};
        throw_if_error(query_output_info(driver, ch, info),
            ch == 0 ? "ASIO getChannelInfo failed for output 1" : "ASIO getChannelInfo failed for output 2");
        caps.output_types[ch] = info.type;
    }
    caps.buffer_size = query_buffer_size(driver);
    return caps;
}

long output_latency(IASIO& driver)
{
    long input{}, output{};
    throw_if_error(driver.getLatencies(&input, &output), "ASIO getLatencies failed");
    if (output < 0) {
        throw std::runtime_error("Invalid ASIO output latency");
    }
    return output;
}

void inquire_overload_reporting(IASIO& driver)
{
    const auto reporting = driver.future(kAsioCanReportOverload, nullptr);
    if (reporting != ASE_SUCCESS && reporting != ASE_InvalidParameter &&
        reporting != ASE_NotPresent && reporting != ASE_OK) {
        throw_if_error(reporting, "ASIO overload inquiry failed");
    }
}

void validate_output_buffers(const ASIOBufferInfo (&buffers)[2], std::size_t channel_bytes, long output_pair)
{
    validate_output_pair(output_pair);
    std::uintptr_t previous[4]{};
    std::size_t count = 0;
    for (long index = 0; index < 2; ++index) {
        const auto& channel = buffers[index];
        if (channel.isInput != ASIOFalse || channel.channelNum != output_pair - 1 + index) {
            throw std::runtime_error("ASIO changed the requested output mapping");
        }
        for (const auto* buffer : channel.buffers) {
            if (!buffer) {
                throw std::runtime_error("Missing ASIO channel buffer");
            }
            const auto address = reinterpret_cast<std::uintptr_t>(buffer);
            validate_writable_range(address, channel_bytes);
            for (std::size_t i = 0; i < count; ++i) {
                if (std::max(previous[i], address) - std::min(previous[i], address) < channel_bytes) {
                    throw std::runtime_error("Overlapping ASIO channel buffers");
                }
            }
            previous[count++] = address;
        }
    }
}

} // namespace wavplay::asio
