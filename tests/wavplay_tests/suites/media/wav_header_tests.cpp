#include "suites/suites.h"
#include "framework/test_suite.h"
#include "fixtures/wav_builder.h"
#include "fixtures/wav_reader_fixture.h"
#include "wavplay/audio/sample_format.h"
#include "wavplay/media/wav_reader.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>

// RIFF/WAVE and RF64 container conformance: the chunk layouts, fmt variants,
// ds64 sizes and fact counts parse_wav_header accepts, where it places the
// data, and the structures it rejects. Reads only confirm that data range;
// cursor, conversion and content validation are wav_reader cases.
namespace {

using namespace wavplay_test;

void test_stereo()
{
    const Chunk data{"data", {
        0x00, 0x80, 0xff, 0x7f,
        0x34, 0x12, 0xde, 0xbc,
        0xfe, 0xff, 0x00, 0x00,
    }};
    TempFile file;
    auto fmt = pcm16(2, 48000);
    fmt.payload.insert(fmt.payload.end(), {0xff, 0xff}); // PCM readers must ignore cbSize.
    wavplay::media::WavReader reader(file.write(wave({{"JUNK", {1, 2, 3}}, data, fmt})));
    expect("rate", reader.info().format.sample_rate, 48000);
    expect("channels", reader.info().format.channels, 2);
    expect("data offset", reader.info().data_offset, 32);
    expect("frame count", reader.info().frame_count, 3);

    std::array<std::int16_t, 2> left{}, right{};
    expect("first read", read_pcm16(reader, left, right), 2);
    expect("left[0]", left[0], -32768);
    expect("right[0]", right[0], 32767);
    expect("left[1]", left[1], 0x1234);
    expect("right[1]", right[1], -0x4322);
    expect("final read", read_pcm16(reader, left, right), 1);
    expect("left[2]", left[0], -2);
    expect("right[2]", right[0], 0);
    expect("EOF", read_pcm16(reader, left, right), 0);
}

void test_extensible_extra_format_bytes()
{
    for (const auto extension : {22U, 24U}) {
        for (const auto trailing : {0U, 1U, 3U}) {
            auto fmt = extensible_pcm(2, 48000, 24, 32, 3);
            fmt.payload[16] = static_cast<std::uint8_t>(extension);
            fmt.payload.resize(18 + extension + trailing, 0xa5);
            TempFile file;
            wavplay::media::WavReader reader(file.write(wave({fmt, {"JUNK", {1}}, pcm24_in32_ramp(2, 1, 0x123456)})));
            expect("Extensible unknown fmt suffix skipped", reader.info().frame_count, 1);
            std::array<std::byte, 4> left{}, right{};
            expect("Extensible read after fmt suffix", reader.read_planar(wavplay::audio::int32_le, left, right), 1);
            constexpr std::array<unsigned, 4> expected_left{0, 0x56, 0x34, 0x12};
            constexpr std::array<unsigned, 4> expected_right{0, 0xa9, 0xcb, 0xed};
            for (std::size_t byte = 0; byte < 4; ++byte) {
                expect("Extensible fmt suffix left", static_cast<unsigned>(left[byte]), expected_left[byte]);
                expect("Extensible fmt suffix right", static_cast<unsigned>(right[byte]), expected_right[byte]);
            }
        }
    }
}

void test_float_fact_chunks()
{
    for (const bool extensible : {false, true}) {
        const auto fmt = extensible ? extensible_float32(2, 48000) : float32(2, 48000);
        const auto data = float32_ramp(2, 3);
        auto extended_fact = fact(3);
        extended_fact.payload.push_back(0xa5); // Unknown fact suffix and odd padding.
        for (const auto& bytes : {wave({fmt, data}), wave({fact(3), fmt, data}),
            wave({data, extended_fact, fmt}), wave({fmt, data, fact(3)})}) {
            TempFile file;
            wavplay::media::WavReader reader(file.write(bytes));
            expect("optional fact or arbitrary chunk order", reader.remaining_frames(), 3);
            std::array<std::byte, 12> left{}, right{};
            expect("fact parsing restores data position", reader.read_planar(wavplay::audio::float32_le, left, right), 3);
        }
        TempFile empty;
        wavplay::media::WavReader zero(empty.write(wave({fmt, fact(0), {"data", {}}})));
        expect("zero-frame Float32 with fact", zero.remaining_frames(), 0);
    }
    TempFile pcm;
    wavplay::media::WavReader unchanged(pcm.write(wave({{"fact", {1}}, pcm16(1, 48000), {"data", {0, 0}}, fact(99)})));
    expect("PCM continues to ignore fact contents and duplicates", unchanged.remaining_frames(), 1);
}

void test_rf64_supported_formats()
{
    constexpr std::size_t frames = 19;
    for (const auto source : rf64_sources) {
        for (const auto channels : {std::uint8_t{1}, std::uint8_t{2}}) {
            for (const auto rate : wavplay::audio::supported_sample_rates) {
                TempFile file;
                const auto data = rf64_samples(source, channels, frames);
                const auto* path = file.write(rf64({{"JUNK", {1, 2, 3}}, data,
                    fact(0xffffffffU), rf64_format(source, channels, rate)}, frames));
                {
                    wavplay::media::WavReader reader(path);
                    expect("RF64 supported sample descriptor", static_cast<int>(reader.info().format.sample == source.sample), 1);
                    expect("RF64 supported rate", reader.info().format.sample_rate, rate);
                    expect("RF64 mono/stereo", reader.info().format.channels, channels);
                    expect("RF64 ds64 and unknown chunk offset", reader.info().data_offset, 68);
                    expect("RF64 frames exclude odd padding", reader.info().frame_count, frames);
                    check_rf64_read(reader, data, source, channels, true);
                    auto cursor = reader.reopen();
                    cursor.seek_frame(1);
                    check_rf64_read(cursor, rf64_samples(source, channels, 1, 1), source, channels);
                    expect("RF64 independent reopened cursor", reader.remaining_frames(), 0);
                    expect("RF64 denies writing", probe_file_access(path, GENERIC_WRITE), ERROR_SHARING_VIOLATION);
                    expect("RF64 denies deletion", probe_file_access(path, DELETE), ERROR_SHARING_VIOLATION);
                }
                expect("RF64 reader releases its protected file", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
            }
        }
    }
}

void append_rf64_chunk(Bytes& bytes, const Chunk& chunk, bool sentinel = false)
{
    bytes.append_range(chunk.id);
    bytes.append_range(le32(sentinel ? 0xffffffffU : static_cast<std::uint32_t>(chunk.payload.size())));
    bytes.append_range(chunk.payload);
    if (chunk.payload.size() % 2 != 0) { bytes.push_back(0); }
}

Bytes rf64_header(const Chunk& sizes)
{
    Bytes bytes{'R', 'F', '6', '4', 0xff, 0xff, 0xff, 0xff, 'W', 'A', 'V', 'E'};
    append_rf64_chunk(bytes, sizes);
    return bytes;
}

void test_rf64_size_precedence_and_fact()
{
    for (const bool floating : {false, true}) {
        const Rf64Source source{floating ? wavplay::audio::float32_le : wavplay::audio::int16_le, false};
        const auto fmt = rf64_format(source, 1, 48000);
        const auto data = rf64_samples(source, 1, 3);
        for (const bool outer_sentinel : {false, true}) {
            for (const bool data_sentinel : {false, true}) {
                // Non-sentinel sizes, including fact, win over unrelated ds64 values.
                auto bytes = rf64({fmt, fact(3), data}, 0xffffffffffffffffULL);
                if (!outer_sentinel) {
                    set_rf64_u32(bytes, 4, static_cast<std::uint32_t>(bytes.size() - 8));
                    set_rf64_u64(bytes, 20, 1);
                }
                if (!data_sentinel) {
                    set_rf64_u32(bytes, bytes.size() - data.payload.size() - (data.payload.size() & 1) - 4,
                        static_cast<std::uint32_t>(data.payload.size()));
                    set_rf64_u64(bytes, 28, 1);
                }
                TempFile file;
                wavplay::media::WavReader reader(file.write(bytes));
                check_rf64_read(reader, data, source, 1, true);
            }
        }
        for (const auto count : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{0xffffffffffffffffULL}}) {
            TempFile file;
            wavplay::media::WavReader no_fact(file.write(rf64({fmt, data}, count)));
            expect("unused RF64 sampleCount does not constrain data", no_fact.remaining_frames(), 3);
        }
        TempFile empty;
        wavplay::media::WavReader zero(empty.write(rf64({fmt, fact(0xffffffffU), {"data", {}}}, 0)));
        expect("RF64 empty data and sentinel fact", zero.remaining_frames(), 0);
        if (floating) {
            for (const auto count : {std::uint64_t{0}, std::uint64_t{2}, std::uint64_t{4}, std::uint64_t{0x100000003ULL}}) {
                expect_wav_rejected("RF64 sentinel fact uses the entire 64-bit sampleCount", rf64({fmt, fact(0xffffffffU), data}, count));
            }
            expect_wav_rejected("RF64 non-sentinel fact must match data", rf64({fmt, fact(2), data}, 3));
            expect_wav_rejected("RF64 short fact", rf64({{"fact", {1, 2, 3}}, fmt, data}, 3));
            expect_wav_rejected("RF64 duplicate fact", rf64({fmt, fact(0xffffffffU), data, fact(3)}, 3));
        } else {
            TempFile pcm;
            wavplay::media::WavReader ignored(pcm.write(rf64({{"fact", {1}}, fmt, data, fact(0xffffffffU)}, 0)));
            expect("RF64 PCM ignores fact and sampleCount", ignored.remaining_frames(), 3);
        }
    }
    // ds64 has no special meaning in RIFF; preserve existing unknown-chunk handling.
    TempFile riff;
    wavplay::media::WavReader unchanged(riff.write(wave({{"ds64", {1}}, pcm16(1, 48000), pcm16_ramp(1, 1), {"ds64", {2}}})));
    expect("RIFF ignores ds64 chunks", unchanged.remaining_frames(), 1);
}

void test_rf64_tables()
{
    const auto data = pcm24_ramp(1, 3, 0x7fffff);
    // Same IDs consume their own table entries in order. Other IDs need not
    // appear in the same global order; an unused entry is merely reserved data.
    auto bytes = rf64_header(ds64(0, data.payload.size(), 0,
        {{"JUNK", 1}, {"LIST", 2}, {"JUNK", 3}, {"skip", 0xffffffffffffffffULL}}));
    append_rf64_chunk(bytes, {"LIST", {1, 2}}, true);
    append_rf64_chunk(bytes, {"JUNK", {3}}, true);
    append_rf64_chunk(bytes, {"JUNK", {4, 5, 6}}, true);
    append_rf64_chunk(bytes, pcm24(1, 48000));
    append_rf64_chunk(bytes, data, true);
    set_rf64_u64(bytes, 20, bytes.size() - 8);
    TempFile file;
    wavplay::media::WavReader reader(file.write(bytes));
    check_rf64_read(reader, data, {wavplay::audio::int24_le, false}, 1, true);

    for (const auto extra : {std::size_t{1}, std::size_t{2}, std::size_t{7}}) {
        auto sizes = ds64(0, 2, 1);
        sizes.payload.resize(sizes.payload.size() + extra, 0xa5);
        auto reserved = rf64_header(sizes);
        append_rf64_chunk(reserved, pcm16(1, 48000));
        append_rf64_chunk(reserved, pcm16_ramp(1, 1), true);
        set_rf64_u64(reserved, 20, reserved.size() - 8);
        TempFile suffix;
        wavplay::media::WavReader accepted(suffix.write(reserved));
        expect("RF64 ds64 reserved suffix and odd padding", accepted.remaining_frames(), 1);
    }
    for (const auto size : {std::uint64_t{1}, std::uint64_t{0xffffffffffffffffULL}}) {
        auto unused = rf64_header(ds64(0, 2, 1, {{"JUNK", size}}));
        append_rf64_chunk(unused, {"JUNK", {1, 2, 3}}); // A normal size must not consume the table entry.
        append_rf64_chunk(unused, pcm16(1, 48000));
        append_rf64_chunk(unused, pcm16_ramp(1, 1), true);
        set_rf64_u64(unused, 20, unused.size() - 8);
        TempFile normal;
        wavplay::media::WavReader accepted(normal.write(unused));
        expect("RF64 ordinary chunk size ignores its table entry", accepted.remaining_frames(), 1);
    }

    // Place matches on both sides of the parser's 8 KiB table-read boundary.
    // fmt and fact themselves use table sizes, so subsequent payload reads must
    // restore the chunk cursor after looking elsewhere in the file.
    constexpr std::size_t entries = 685;
    auto sizes = ds64(0, 2 * 19 * 4, 19);
    set_rf64_u32(sizes.payload, 24, static_cast<std::uint32_t>(entries));
    for (std::size_t index = 0; index < entries; ++index) {
        const auto id = index == 681 || index == 682 ? "JUNK" : index == 683 ? "fmt " : index == 684 ? "fact" : "skip";
        sizes.payload.append_range(std::string_view(id));
        sizes.payload.append_range(le64(index == 681 ? 1 : index == 682 ? 3 : index == 683 ? 18 : index == 684 ? 4 : 0));
    }
    auto boundary = rf64_header(sizes);
    append_rf64_chunk(boundary, {"JUNK", {1}}, true);
    append_rf64_chunk(boundary, {"JUNK", {2, 3, 4}}, true);
    append_rf64_chunk(boundary, float32(2, 48000), true);
    const auto floating = float32_ramp(2, 19);
    append_rf64_chunk(boundary, floating, true);
    append_rf64_chunk(boundary, fact(0xffffffffU), true);
    set_rf64_u64(boundary, 20, boundary.size() - 8);
    TempFile cache;
    wavplay::media::WavReader across(cache.write(boundary));
    check_rf64_read(across, floating, {wavplay::audio::float32_le, false}, 2, true);
}

void test_rf64_malformed()
{
    const auto fmt = pcm16(1, 48000);
    const auto data = pcm16_ramp(1, 3);
    const auto valid = rf64({fmt, data}, 3);
    auto missing = wave({fmt, data});
    missing[1] = 'F'; missing[2] = '6'; missing[3] = '4';
    expect_wav_rejected("RF64 needs ds64 even with small sizes", missing);
    auto misplaced = wave({fmt, ds64(0, 6, 3), data});
    misplaced[1] = 'F'; misplaced[2] = '6'; misplaced[3] = '4';
    expect_wav_rejected("RF64 ds64 must be first", misplaced);
    expect_wav_rejected("RF64 duplicate ds64", rf64({fmt, data, ds64(0, 6, 3)}, 3));
    expect_wav_rejected("RF64 missing fmt", rf64({data}, 3));
    expect_wav_rejected("RF64 missing data", rf64({fmt}, 0));
    expect_wav_rejected("RF64 duplicate fmt", rf64({fmt, data, fmt}, 3));
    expect_wav_rejected("RF64 duplicate data", rf64({fmt, data, data}, 3));
    expect_wav_rejected("RF64 incomplete PCM frame", rf64({fmt, {"data", {1, 2, 3}}}, 1));
    expect_wav_rejected("RF64 unsupported channels", rf64({pcm16(3, 48000), pcm16_ramp(3, 1)}, 1));
    expect_wav_rejected("RF64 unsupported rate", rf64({pcm16(1, 32000), data}, 3));
    auto float64 = pcm_integer(1, 48000, 64);
    float64.payload[0] = 3;
    expect_wav_rejected("RF64 does not enable Float64 input", rf64({float64, {"data", Bytes(8, 0)}}, 1));
    auto bad_extensible = extensible_pcm(1, 48000, 24, 32);
    bad_extensible.payload[25] ^= 1;
    expect_wav_rejected("RF64 preserves the full Extensible GUID check", rf64({bad_extensible, pcm24_in32_ramp(1, 1)}, 1));
    for (const auto size : {0U, 4U, 24U, 27U, 0xffffffffU}) {
        auto bytes = valid;
        set_rf64_u32(bytes, 16, size);
        expect_wav_rejected("RF64 invalid ds64 payload length", bytes);
    }
    for (const auto count : {1U, 0x15555556U, 0xffffffffU}) {
        auto bytes = valid;
        set_rf64_u32(bytes, 44, count);
        expect_wav_rejected("RF64 table length must fit ds64 without overflow", bytes);
    }
    for (const auto offset : {std::size_t{20}, std::size_t{28}}) {
        for (const auto value : {std::uint64_t{1}, std::uint64_t{0x100000006ULL},
                std::uint64_t{0x7fffffffffffffffULL}, std::uint64_t{0xffffffffffffffffULL}}) {
            auto bytes = valid;
            set_rf64_u64(bytes, offset, value);
            expect_wav_rejected("RF64 64-bit bounds or complete frame mismatch", bytes);
        }
    }
    auto trailing = valid;
    trailing.push_back(0);
    expect_wav_rejected("RF64 undeclared trailing byte", trailing);
    auto partial = valid;
    partial.push_back('J');
    set_rf64_u64(partial, 20, partial.size() - 8);
    expect_wav_rejected("RF64 partial chunk header", partial);
    auto unpadded = rf64({pcm24(1, 48000), pcm24_ramp(1, 1)}, 1);
    unpadded.pop_back();
    set_rf64_u64(unpadded, 20, unpadded.size() - 8);
    expect_wav_rejected("RF64 odd data needs its pad", unpadded);
    for (const auto table : {std::initializer_list<Ds64Entry>{}, {{"LIST", 1}}, {{"JUNK", 1}}}) {
        auto bytes = rf64_header(ds64(0, 6, 3, table));
        append_rf64_chunk(bytes, fmt);
        append_rf64_chunk(bytes, {"JUNK", {1}}, true);
        append_rf64_chunk(bytes, {"JUNK", {2}}, true);
        append_rf64_chunk(bytes, data, true);
        set_rf64_u64(bytes, 20, bytes.size() - 8);
        expect_wav_rejected("RF64 absent, wrong-ID or exhausted size-table entry", bytes);
    }
    for (const auto size : {std::uint64_t{0xfffffffffffffffeULL}, std::uint64_t{0xffffffffffffffffULL}}) {
        auto bytes = rf64_header(ds64(0, 6, 3, {{"JUNK", size}}));
        append_rf64_chunk(bytes, fmt);
        append_rf64_chunk(bytes, {"JUNK", {1}}, true);
        append_rf64_chunk(bytes, data, true);
        set_rf64_u64(bytes, 20, bytes.size() - 8);
        expect_wav_rejected("RF64 table size and odd-pad arithmetic cannot wrap", bytes);
    }
}

void test_rf64_unused_sparse_table()
{
    // A 3 GiB ds64 table is legal reserved space. No sentinel chunk needs an
    // entry, so constructor/reopen must neither allocate nor read that table.
    constexpr std::uint32_t entries = 1U << 28;
    constexpr std::uint64_t table_bytes = std::uint64_t{entries} * 12;
    auto prefix = rf64_header(ds64(0, 2, 1));
    set_rf64_u32(prefix, 16, static_cast<std::uint32_t>(28 + table_bytes));
    set_rf64_u32(prefix, 44, entries);
    const auto suffix_offset = prefix.size() + table_bytes;
    Bytes suffix;
    append_rf64_chunk(suffix, pcm16(1, 48000));
    append_rf64_chunk(suffix, pcm16_ramp(1, 1, 0x1234), true);
    const auto logical_size = suffix_offset + suffix.size();
    set_rf64_u64(prefix, 20, logical_size - 8);
    TempFile file;
    const auto* path = file.write_sparse(logical_size, {{0, prefix}, {suffix_offset, suffix}});
    {
        const auto handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "open unused RF64 table lock");
        }
        const std::unique_ptr<void, decltype(&CloseHandle)> lock(handle, &CloseHandle);
        lock_rf64_bytes(lock.get(), prefix.size(), table_bytes);
        wavplay::media::WavReader reader(path);
        expect("RF64 reserved table changes data offset only", reader.info().data_offset, suffix_offset + 32);
        check_rf64_read(reader, pcm16_ramp(1, 1, 0x1234), {wavplay::audio::int16_le, false}, 1, true);
        lock_rf64_bytes(lock.get(), 0, prefix.size());
        auto reopened = reader.reopen();
        check_rf64_read(reopened, pcm16_ramp(1, 1, 0x1234), {wavplay::audio::int16_le, false}, 1, true);
    }
    expect("RF64 unused sparse table handles released", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
}

void test_rf64_large_unknown_and_data_offset()
{
    constexpr auto unknown_size = (std::uint64_t{1} << 32) + 1; // Requires a physical odd pad, too.
    constexpr std::size_t frames = 4099;
    for (const auto source : rf64_sources) {
        for (const auto channels : {std::uint8_t{1}, std::uint8_t{2}}) {
            const auto data = rf64_samples(source, channels, frames);
            auto prefix = rf64_header(ds64(0, data.payload.size(), frames, {{"JUNK", unknown_size}}));
            append_rf64_chunk(prefix, rf64_format(source, channels, 48000));
            prefix.insert(prefix.end(), {'J', 'U', 'N', 'K', 0xff, 0xff, 0xff, 0xff});
            const auto data_header = prefix.size() + unknown_size + 1;
            const auto data_offset = data_header + 8;
            Bytes suffix;
            append_rf64_chunk(suffix, data, true);
            append_rf64_chunk(suffix, fact(0xffffffffU));
            const auto logical_size = data_header + suffix.size();
            set_rf64_u64(prefix, 20, logical_size - 8);
            TempFile file;
            const auto* path = file.write_sparse(logical_size, {{0, prefix}, {data_header, suffix}});
            {
                wavplay::media::WavReader original(path);
                expect("RF64 data offset crosses 4 GiB", original.info().data_offset, data_offset);
                expect("RF64 large unknown does not become sample data", original.info().frame_count, frames);
                check_rf64_read(original, data, source, channels, true);
                const auto handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
                if (handle == INVALID_HANDLE_VALUE) {
                    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "open RF64 high-header lock");
                }
                const std::unique_ptr<void, decltype(&CloseHandle)> lock(handle, &CloseHandle);
                lock_rf64_bytes(lock.get(), 0, data_offset);
                lock_rf64_bytes(lock.get(), data_offset + data.payload.size() - channels * source.sample.bytes(),
                    channels * source.sample.bytes());
                auto cursor = original.reopen();
                // In 24/32 this also proves the validated payload is not scanned again.
                check_rf64_read(cursor, rf64_samples(source, channels, 1), source, channels);
                expect("RF64 high-offset reopened cursor is independent", original.remaining_frames(), 0);
            }
            expect("RF64 high-offset file released", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
            if (source.sample == wavplay::audio::int24_in32_le) {
                suffix[8 + data.payload.size() - 4] = 0x80;
                (void)file.write_sparse(logical_size, {{0, prefix}, {data_header, suffix}});
                try {
                    const wavplay::media::WavReader invalid(path);
                    throw std::logic_error("RF64 accepted dirty final unused bits beyond 4 GiB");
                } catch (const std::system_error&) {
                    throw;
                } catch (const std::runtime_error&) {
                    expect("RF64 late padding failure releases file", probe_file_access(path, GENERIC_WRITE | DELETE), ERROR_SUCCESS);
                }
            }
        }
    }
}

} // namespace

int wavplay_test::run_wav_header_tests()
{
    TestSuite suite;
    suite.run("stereo PCM16, odd chunk, data before fmt", test_stereo);
    suite.run("Extensible declared and unknown fmt suffixes stay within their chunk", test_extensible_extra_format_bytes);
    suite.run("Float32 optional fact, arbitrary order and PCM fact compatibility", test_float_fact_chunks);
    suite.run("RF64 PCM and Float32 normal/Extensible mono/stereo at all rates", test_rf64_supported_formats);
    suite.run("RF64 32-bit sizes take precedence and Float fact resolves sampleCount", test_rf64_size_precedence_and_fact);
    suite.run("RF64 ordered size-table matches and reserved ds64 bytes", test_rf64_tables);
    suite.run("RF64 missing, duplicate, malformed and overflowing metadata", test_rf64_malformed);
    suite.run("RF64 sparse unknown chunk, data offset above 4 GiB and full padding validation", test_rf64_large_unknown_and_data_offset);
    suite.run("RF64 huge unused size table requires no allocation or read", test_rf64_unused_sparse_table);

    const auto reject = [&](const char* name, const Bytes& bytes) {
        suite.run(name, [&] { expect_wav_rejected(name, bytes); });
    };
    for (const bool extensible : {false, true}) {
        const auto float_fmt = extensible ? extensible_float32(2, 48000) : float32(2, 48000);
        const auto float_data = float32_ramp(2, 1);
        for (std::size_t count = 0; count < 4; ++count) {
            reject("Float32 short fact is refused before fmt",
                wave({{"fact", Bytes(count, 0)}, float_fmt, float_data}));
            reject("Float32 short fact after data is refused",
                wave({float_fmt, float_data, {"fact", Bytes(count, 0)}}));
        }
        reject("Float32 duplicate fact", wave({fact(1), float_data, float_fmt, fact(1)}));
        for (const auto count : {0U, 2U, 0xffffffffU}) {
            reject("Float32 fact must equal complete data frames", wave({float_data, fact(count), float_fmt}));
        }
        auto unpadded = wave({float_fmt, float_data, {"fact", {1, 0, 0, 0, 0xa5}}});
        unpadded.pop_back();
        std::ranges::copy(le32(static_cast<std::uint32_t>(unpadded.size() - 8)), unpadded.begin() + 4);
        reject("Float32 fact odd payload still needs its pad", unpadded);
        auto invalid = float_fmt;
        invalid.payload[12] = 4;
        reject("Float32 block alignment", wave({invalid, float_data}));
        invalid = float_fmt;
        invalid.payload[8] ^= 1;
        reject("Float32 byte rate", wave({invalid, float_data}));
        reject("Float32 incomplete frame", wave({float_fmt, {"data", Bytes(7, 0)}}));
        for (const auto channels : {std::uint8_t{0}, std::uint8_t{3}}) {
            reject("Float32 channel count", wave({extensible ? extensible_float32(channels, 48000) : float32(channels, 48000), float_data}));
        }
        reject("Float32 unsupported rate", wave({extensible ? extensible_float32(2, 32000) : float32(2, 32000), float_data}));
    }
    for (const auto length : {0U, 15U, 17U, 19U, 40U}) {
        auto fmt_float = float32(2, 48000);
        fmt_float.payload.resize(length);
        reject("Float32 rejects invalid plain fmt lengths", wave({fmt_float, float32_ramp(2, 1)}));
    }
    for (const auto extension : {1U, 22U, 65535U}) {
        auto fmt_float = float32(2, 48000);
        fmt_float.payload[16] = static_cast<std::uint8_t>(extension);
        fmt_float.payload[17] = static_cast<std::uint8_t>(extension >> 8);
        reject("Float32 plain WAVEFORMATEX requires cbSize zero", wave({fmt_float, float32_ramp(2, 1)}));
    }
    for (const auto widths : {std::array<std::uint8_t, 2>{0, 32}, {16, 16}, {24, 24}, {24, 32}, {32, 24}, {32, 64}, {64, 64}}) {
        auto fmt_float = extensible_pcm(2, 48000, widths[0], widths[1]);
        fmt_float.payload[24] = 3;
        reject("Extensible Float32 requires 32 valid bits in a 32-bit container", wave({fmt_float, float32_ramp(2, 1)}));
    }
    for (std::size_t byte = 24; byte < 40; ++byte) {
        auto fmt_float = extensible_float32(2, 48000);
        fmt_float.payload[byte] ^= 4; // Never changes the Float GUID into the supported PCM GUID.
        reject("Extensible Float32 GUID matches all bytes", wave({fmt_float, float32_ramp(2, 1)}));
    }
    for (const auto channels : {std::uint8_t{1}, std::uint8_t{2}}) {
        for (const auto mask : {1U, 2U, channels == 1 ? 3U : 4U, 0x80000000U}) {
            reject("Extensible Float32 speaker mask", wave({extensible_float32(channels, 48000, mask), float32_ramp(channels, 1)}));
        }
    }
    auto rf64_float = wave({float32(2, 48000), float32_ramp(2, 1)});
    rf64_float[1] = 'F'; rf64_float[2] = '6'; rf64_float[3] = '4';
    reject("RF64 Float32 without a leading ds64 is rejected", rf64_float);
    const auto fmt = pcm16(2, 48000);
    const Chunk data{"data", {0, 1, 2, 3}};
    reject("missing fmt", wave({data}));
    reject("missing data", wave({fmt}));
    reject("duplicate fmt", wave({fmt, data, fmt}));
    reject("duplicate data", wave({fmt, data, data}));
    reject("partial stereo frame", wave({fmt, {"data", {0, 1}}}));
    reject("unsupported channel count", wave({pcm16(3, 48000), {"data", {0, 1, 2, 3, 4, 5}}}));
    reject("unsupported rate", wave({pcm16(2, 32000), data}));
    reject("partial PCM24 mono sample", wave({pcm24(1, 48000), {"data", {1, 2}}}));
    reject("partial PCM24 stereo frame", wave({pcm24(2, 48000), {"data", {1, 2, 3}}}));
    reject("partial PCM32 mono sample", wave({pcm32(1, 48000), {"data", {0, 0, 0}}}));
    reject("partial PCM32 stereo frame", wave({pcm32(2, 48000), {"data", {0, 0, 0, 0}}}));
    auto malformed32 = pcm32(2, 48000);
    malformed32.payload[12] = 6;
    reject("PCM32 with PCM24 block alignment", wave({malformed32, pcm32_ramp(2, 1)}));
    malformed32 = pcm32(1, 48000);
    malformed32.payload[8] ^= 1;
    reject("PCM32 inconsistent byte rate", wave({malformed32, pcm32_ramp(1, 1)}));
    malformed32 = pcm_integer(1, 48000, 64);
    malformed32.payload[0] = 3;
    reject("Float64 input remains unsupported", wave({malformed32, {"data", Bytes(8, 0)}}));
    reject("PCM64 remains unsupported", wave({pcm_integer(1, 48000, 64), {"data", Bytes(8, 0)}}));
    auto malformed24 = pcm24(2, 48000);
    malformed24.payload[12] = 4;
    reject("PCM24 with PCM16 block alignment", wave({malformed24, pcm24_ramp(2, 1)}));
    malformed24 = pcm24(1, 48000);
    malformed24.payload[8] ^= 1;
    reject("PCM24 inconsistent byte rate", wave({malformed24, pcm24_ramp(1, 1)}));
    auto bad_fmt = fmt;
    bad_fmt.payload[0] = 2;
    reject("compressed format", wave({bad_fmt, data}));
    bad_fmt = fmt;
    bad_fmt.payload[12] = 2;
    reject("contradictory block align", wave({bad_fmt, data}));
    bad_fmt = fmt;
    bad_fmt.payload[14] = 12;
    reject("invalid bits per sample", wave({bad_fmt, data}));
    bad_fmt = fmt;
    bad_fmt.payload[8] ^= 1;
    reject("contradictory byte rate", wave({bad_fmt, data}));
    bad_fmt = fmt;
    bad_fmt.payload.resize(14);
    reject("short fmt payload", wave({bad_fmt, data}));

    auto truncated = wave({fmt, data});
    truncated.pop_back();
    reject("truncated file", truncated);
    // The RIFF header read is the only one not bounded by the file size. At an
    // empty file's end the positional read fails with ERROR_HANDLE_EOF, and a
    // shorter file returns a short count: malformed input, not an I/O failure.
    reject("empty file", Bytes{});
    reject("file shorter than the RIFF header", Bytes(truncated.begin(), truncated.begin() + 11));
    auto short_riff = wave({fmt, data});
    short_riff[4] -= 2;
    reject("data extends past declared RIFF", short_riff);
    auto trailing = wave({fmt, data});
    trailing.push_back(0);
    reject("undeclared byte after complete RIFF", trailing);
    auto short_header = wave({fmt, data});
    short_header.push_back('J');
    ++short_header[4];
    reject("partial chunk header at RIFF end", short_header);
    auto missing_pad = wave({fmt, data, {"JUNK", {1}}});
    missing_pad.pop_back();
    --missing_pad[4];
    reject("missing odd chunk pad", missing_pad);
    auto huge_chunk = wave({fmt, data, {"JUNK", {}}});
    std::fill(huge_chunk.end() - 4, huge_chunk.end(), std::uint8_t{0xff});
    reject("unknown chunk declares 0xffffffff bytes", huge_chunk);
    const auto extensible_data = pcm24_in32_ramp(2, 1);
    for (const auto size : {16U, 17U, 18U, 39U}) {
        auto extensible = extensible_pcm(2, 48000, 24, 32);
        extensible.payload.resize(size);
        reject("truncated Extensible fmt payload", wave({extensible, extensible_data}));
    }
    for (const auto cb_size : {0U, 21U, 23U, 65535U}) {
        auto extensible = extensible_pcm(2, 48000, 24, 32);
        extensible.payload[16] = static_cast<std::uint8_t>(cb_size);
        extensible.payload[17] = static_cast<std::uint8_t>(cb_size >> 8);
        reject("Extensible cbSize too small or beyond fmt", wave({extensible, extensible_data}));
    }
    for (std::size_t byte = 24; byte < 40; ++byte) {
        auto extensible = extensible_pcm(2, 48000, 24, 32);
        extensible.payload[byte] ^= 0x02; // Includes IEEE_FLOAT's Data1 == 3.
        reject("Extensible GUID must match all 16 bytes", wave({extensible, extensible_data}));
    }
    for (const auto widths : {std::array<std::uint8_t, 2>{0, 32}, {8, 8}, {16, 24}, {16, 32},
        {20, 24}, {23, 24}, {24, 16}, {24, 31}, {25, 32}, {32, 24}, {33, 32}, {32, 64}}) {
        reject("unsupported Extensible valid/container bits",
            wave({extensible_pcm(2, 48000, widths[0], widths[1]), extensible_data}));
    }
    for (const auto channels : {std::uint8_t{1}, std::uint8_t{2}}) {
        for (const auto channel_mask : {1U, 2U, channels == 1 ? 3U : 4U, 0x80000000U, 0xffffffffU}) {
            reject("unsupported Extensible channel mask",
                wave({extensible_pcm(channels, 48000, 24, 32, channel_mask), pcm24_in32_ramp(channels, 1)}));
        }
        reject("partial Extensible frame", wave({extensible_pcm(channels, 48000, 24, 32),
            {"data", Bytes(channels * 4 - 1, 0)}}));
    }
    for (const auto channels : {std::uint8_t{0}, std::uint8_t{3}}) {
        reject("unsupported Extensible channel count", wave({extensible_pcm(channels, 48000, 24, 32), extensible_data}));
    }
    reject("unsupported Extensible rate", wave({extensible_pcm(2, 32000, 24, 32), extensible_data}));
    auto malformed_extensible = extensible_pcm(2, 48000, 24, 32);
    malformed_extensible.payload[12] = 6;
    reject("Extensible block alignment uses the container", wave({malformed_extensible, extensible_data}));
    malformed_extensible = extensible_pcm(2, 48000, 24, 32);
    malformed_extensible.payload[8] ^= 1;
    reject("Extensible byte rate uses the container", wave({malformed_extensible, extensible_data}));

    // Plain PCM retains its own fmt-size rule; Extensible's larger fmt is not
    // accepted merely because its first 16 bytes resemble PCMWAVEFORMAT.
    auto long_pcm = fmt;
    long_pcm.payload.resize(40);
    reject("PCM fmt longer than WAVEFORMATEX", wave({long_pcm, data}));
    auto odd_fmt = fmt;
    odd_fmt.payload.resize(17);
    reject("odd-sized PCM fmt", wave({odd_fmt, data}));

    return suite.result("RIFF/RF64 WAV header");
}
