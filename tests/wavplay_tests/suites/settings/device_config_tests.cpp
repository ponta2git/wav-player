#include "suites/suites.h"
#include "wavplay/settings/device_config.h"
#include "wavplay/core/win32.h"
#include "framework/test_suite.h"
#include "fixtures/wav_builder.h"

#include <algorithm>
#include <chrono>
#include <array>
#include <filesystem>
#include <limits>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

using namespace wavplay_test;
using namespace std::chrono_literals;
using wavplay::settings::load_application_config;
using wavplay::settings::parse_application_config;
using wavplay::settings::save_window_config;
using wavplay::settings::WindowPlacement;
constexpr CLSID device{0x76543210, 0x89ab, 0xcdef, {0x81, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef}};
constexpr std::string_view section = "[Device.{76543210-89AB-CDEF-8123-456789ABCDEF}]\n";

std::string configuration(std::string_view values)
{
    return std::string(section) + std::string(values);
}

Bytes bytes(std::string_view text)
{
    return Bytes(text.begin(), text.end());
}

std::string file_contents(const wchar_t* path)
{
    const auto raw = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (raw == INVALID_HANDLE_VALUE) { wavplay::core::throw_last_error("read settings fixture"); }
    const wavplay::core::UniqueHandle file(raw);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) { wavplay::core::throw_last_error("measure settings fixture"); }
    expect_true("settings fixture is bounded", size.QuadPart >= 0 &&
        size.QuadPart <= static_cast<LONGLONG>(wavplay::settings::max_config_bytes));
    std::string result(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read{};
    if (!ReadFile(file.get(), result.data(), static_cast<DWORD>(result.size()), &read, nullptr)) {
        wavplay::core::throw_last_error("read settings fixture bytes");
    }
    expect("settings fixture read completes", read, result.size());
    return result;
}

void expect_no_temporary(const wchar_t* path)
{
    WIN32_FIND_DATAW found{};
    const auto handle = FindFirstFileW((std::wstring(path) + L".wavplay-*.tmp").c_str(), &found);
    if (handle != INVALID_HANDLE_VALUE) {
        FindClose(handle);
        fail("settings save left a temporary file");
    }
    expect("temporary files are absent", GetLastError(), ERROR_FILE_NOT_FOUND);
}

void test_values_and_scope()
{
    for (const auto text : {"", "; no device settings\n", "[General]\nBufferSize=not-a-number\n",
        "[Device.{00000000-0000-0000-0000-000000000000}]\nBufferSize=-1\nStartSilence=-1\n"}) {
        const auto parsed = parse_application_config(text, device).device;
        expect_true("missing and other-device settings remain unspecified", !parsed.buffer_frames && !parsed.start_silence);
    }
    for (const long value : {0L, 1L, 512L, (std::numeric_limits<long>::max)()}) {
        const auto parsed = parse_application_config(configuration("BufferSize=" + std::to_string(value)), device).device;
        expect("exact buffer value", parsed.buffer_frames.value(), value);
        expect_true("unspecified silence preserves the caller's default", !parsed.start_silence);
    }
    for (const long value : {0L, 100L, 1000L}) {
        const auto parsed = parse_application_config(configuration("StartSilence=" + std::to_string(value)), device).device;
        expect("exact silence milliseconds", parsed.start_silence.value().count(), value);
        expect_true("unspecified buffer preserves the caller's default", !parsed.buffer_frames);
    }
    const auto parsed = parse_application_config(
        "\xef\xbb\xbf# UTF-8 comment: \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\r\n"
        " [ device.{76543210-89ab-cdef-8123-456789abcdef} ] ; selected\r\n"
        " bUfFeRsIzE \t= 00512 ; frames\r\nStArTsIlEnCe=00100 # milliseconds\r\n"
        "OutputPair=3\nFutureKey=anything\nFutureBareKey\n"
        "[Device.{00000000-0000-0000-0000-000000000000}]\nBufferSize=invalid\n", device).device;
    expect("BOM, CRLF, whitespace, case and leading zeros", parsed.buffer_frames.value(), 512);
    expect("silence shares INI syntax", parsed.start_silence.value().count(), 100);
}

void test_invalid_values()
{
    for (const auto key : {"BufferSize", "StartSilence"}) {
        for (const auto value : {"", "-1", "+1", "1.0", "0x10", "1 0", "1x", "2147483648", "999999999999999999999999"}) {
            const auto source = configuration(std::string(key) + '=' + value);
            expect_throw<std::invalid_argument>("known settings refuse invalid decimal values", [&] {
                (void)parse_application_config(source, device);
            });
        }
        for (const auto suffix : {"", " value", " # comment"}) {
            const auto source = configuration(std::string(key) + suffix);
            expect_throw<std::invalid_argument>("known key requires an assignment", [&] {
                (void)parse_application_config(source, device);
            });
        }
        const auto duplicate = configuration(std::string(key) + "=0\n") + configuration(std::string(key) + "=1\n");
        expect_throw<std::invalid_argument>("zero also counts as a duplicate across repeated sections", [&] {
            (void)parse_application_config(duplicate, device);
        });
    }
    expect_throw<std::invalid_argument>("silence is capped at 1000 milliseconds", [&] {
        (void)parse_application_config(configuration("StartSilence=1001"), device);
    });
    for (const auto text : {"[Device.invalid", "[General]suffix\n"}) {
        expect_throw<std::invalid_argument>("sections must be well formed", [&] { (void)parse_application_config(text, device); });
    }
}

void test_encoding_and_bound()
{
    for (const auto invalid : {std::string("\0", 1), std::string("\xc0\xaf"), std::string("\xe3\x81"),
        std::string("\xed\xa0\x80"), std::string("\xff\xfe")}) {
        expect_throw<std::invalid_argument>("invalid UTF-8 and embedded NUL refuse the whole file", [&] {
            (void)parse_application_config(configuration("BufferSize=512\n") + invalid, device);
        });
    }
    const std::string at_limit(wavplay::settings::max_config_bytes, ' ');
    expect_true("bounded empty configuration is accepted", !parse_application_config(at_limit, device).device.buffer_frames);
    expect_throw<std::invalid_argument>("oversized configuration is refused", [&] {
        (void)parse_application_config(at_limit + ' ', device);
    });
}

// The decoder settings.ini and playlist.txt share. It neither strips a BOM nor
// refuses NUL; its callers do both.
void test_strict_utf8_decoding()
{
    using wavplay::core::utf8_to_wide;
    expect_true("empty text decodes to an empty string", utf8_to_wide("") == std::wstring());
    expect_true("ASCII, BMP and supplementary characters decode",
        utf8_to_wide("A \xe6\x97\xa5\xe6\x9c\xac \xf0\x9f\x8e\xb5") == std::wstring(L"A 日本 \U0001f3b5"));
    expect_true("a BOM decodes to U+FEFF", utf8_to_wide("\xef\xbb\xbf" "x") == std::wstring(L"\xfeff" L"x"));
    expect_true("NUL decodes", utf8_to_wide(std::string_view("a\0b", 3)) == std::wstring(L"a\0b", 3));
    for (const auto invalid : {"\xc0\xaf", "\xe3\x81", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xff\xfe", "\xef\xbb"}) {
        expect_true("invalid UTF-8 after valid text has no value", !utf8_to_wide(std::string("valid ") + invalid));
    }
}

void test_loader()
{
    TempFile file;
    const auto path = file.write(bytes(configuration("BufferSize=512\nStartSilence=0\n")));
    const auto parsed = load_application_config(path, device).device;
    expect("loader returns configured frames", parsed.buffer_frames.value(), 512);
    expect("loader returns configured silence", parsed.start_silence.value().count(), 0);
    expect("read handle released", probe_file_access(path, GENERIC_WRITE), ERROR_SUCCESS);
    file.write(bytes(configuration("BufferSize=-1")));
    expect_throw<std::invalid_argument>("invalid file is refused", [&] { (void)load_application_config(path, device); });
    expect("read handle released on parser failure", probe_file_access(path, GENERIC_WRITE), ERROR_SUCCESS);
    {
        const wavplay::core::UniqueHandle locked(CreateFileW(path, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        expect_true("test acquired an exclusive file handle", locked.get() != INVALID_HANDLE_VALUE);
        expect_throw<std::system_error>("read failure never silently selects defaults", [&] {
            (void)load_application_config(path, device);
        });
    }
    file.write_sparse(wavplay::settings::max_config_bytes + 1, {});
    expect_throw<std::invalid_argument>("loader rejects size before allocating contents", [&] {
        (void)load_application_config(path, device);
    });
    expect_true("test removes the temporary settings file", DeleteFileW(path) != FALSE);
    const auto missing = load_application_config(path, device).device;
    expect_true("missing file leaves both settings unspecified", !missing.buffer_frames && !missing.start_silence);
}

// A dedicated thread keeps these modes out of other cases. These checks cover
// restoration on successful, refused and failed transactions. Coverage of the
// actual file APIs by CriticalErrorScope is reviewed in device_config.cpp; a
// physical no-media dialog is not injected into an automated test.
void test_thread_error_mode_restored()
{
    TempFile file;
    const auto path = file.write({});
    const WindowPlacement placement{10, 20, 800, 600, false};
    const auto missing = std::wstring(path) + L".missing";
    const auto missing_parent = missing + L"\\settings.ini";
    for (const DWORD mode : {DWORD{0}, DWORD{SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX}, DWORD{SEM_FAILCRITICALERRORS}}) {
        if (!SetThreadErrorMode(mode, nullptr)) { wavplay::core::throw_last_error("SetThreadErrorMode(settings test)"); }
        file.write(bytes("[General]\nSelectedDevice=\n"));
        (void)load_application_config(path);
        expect("successful settings read restores the mode", GetThreadErrorMode(), mode);
        (void)load_application_config(missing.c_str());
        expect("absent settings restores the mode", GetThreadErrorMode(), mode);
        save_window_config(path, placement);
        expect("successful settings transaction restores the mode", GetThreadErrorMode(), mode);
        const auto original = file_contents(path);
        save_window_config(path, placement);
        expect("unchanged settings restore the mode", GetThreadErrorMode(), mode);
        {
            const auto raw = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (raw == INVALID_HANDLE_VALUE) { wavplay::core::throw_last_error("protect settings mode fixture"); }
            const wavplay::core::UniqueHandle protection(raw);
            expect_throw<std::system_error>("replacement fails while protected", [&] {
                save_window_config(path, {30, 40, 900, 700, true});
            });
            expect("replacement failure and temporary cleanup restore the mode", GetThreadErrorMode(), mode);
            expect_true("failed replacement preserves original bytes", file_contents(path) == original);
            expect_no_temporary(path);
        }
        {
            const auto raw = CreateFileW(path, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (raw == INVALID_HANDLE_VALUE) { wavplay::core::throw_last_error("lock settings mode fixture"); }
            const wavplay::core::UniqueHandle protection(raw);
            expect_throw<std::system_error>("settings read fails while locked", [&] { (void)load_application_config(path); });
            expect("failed settings read restores the mode", GetThreadErrorMode(), mode);
            expect_throw<std::system_error>("save reread fails while locked", [&] { save_window_config(path, placement); });
            expect("failed save reread restores the mode", GetThreadErrorMode(), mode);
        }
        expect_throw<std::system_error>("temporary creation fails in absent parent", [&] {
            save_window_config(missing_parent.c_str(), placement);
        });
        expect("temporary creation failure restores the mode", GetThreadErrorMode(), mode);
        file.write(bytes("[Window]\nWidth=0\n"));
        expect_throw<std::invalid_argument>("invalid settings load is refused", [&] { (void)load_application_config(path); });
        expect("settings parse failure restores the mode", GetThreadErrorMode(), mode);
        expect_throw<std::invalid_argument>("invalid settings save is refused", [&] { save_window_config(path, placement); });
        expect("save parse failure restores the mode", GetThreadErrorMode(), mode);
    }
}

void test_application_selection()
{
    for (const auto text : {"", "[General]\nSelectedDevice= ; not selected\n",
        "[General]\nSelectedDevice={00000000-0000-0000-0000-000000000000}\n"}) {
        const auto parsed = parse_application_config(text);
        expect_true("empty and null CLSIDs never choose a driver", !parsed.selected_device);
        expect_true("missing application settings retain defaults", !parsed.window && !parsed.device.output_pair);
    }
    const auto text = configuration("Name=\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e DAC\nOutputPair=3\nBufferSize=512\nStartSilence=25\n") +
        "[General]\nSelectedDevice={76543210-89ab-cdef-8123-456789abcdef} ; exact identity\n" +
        "[Device.{11111111-1111-1111-1111-111111111111}]\nOutputPair=invalid\n";
    const auto parsed = parse_application_config(text);
    expect_true("selection works independently of section order", parsed.selected_device && IsEqualGUID(*parsed.selected_device, device));
    expect("selected output pair", parsed.device.output_pair.value(), 3);
    expect("selected buffer size", parsed.device.buffer_frames.value(), 512);
    expect("selected start silence", parsed.device.start_silence.value().count(), 25);
    expect_true("UTF-8 display name is decoded", parsed.device.name == L"日本語 DAC");
    const auto override_text = "[General]\nSelectedDevice={11111111-1111-1111-1111-111111111111}\n" +
        configuration("OutputPair=5\n");
    const auto injected = parse_application_config(override_text, device);
    expect_true("test injection retains its explicit driver", injected.selected_device && IsEqualGUID(*injected.selected_device, device));
    expect("test injection uses that driver's section", injected.device.output_pair.value(), 5);
    expect_true("an unselected invalid device does not prevent loading", !parse_application_config(configuration("OutputPair=2\n")).selected_device);
    for (const auto value : {"not-a-guid", "76543210-89AB-CDEF-8123-456789ABCDEF", "{76543210-89AB-CDEF-8123-456789ABCDEG}",
        "{76543210-89AB-CDEF-8123-456789ABCDEF}suffix"}) {
        expect_throw<std::invalid_argument>("selected driver must be an exact braced CLSID", [&] {
            (void)parse_application_config(std::string("[General]\nSelectedDevice=") + value);
        });
    }
    for (const auto text_value : {"[General]\nSelectedDevice", "[General]\nSelectedDevice=\n[general]\nselecteddevice=\n"}) {
        expect_throw<std::invalid_argument>("selected driver assignment is required and unique", [&] {
            (void)parse_application_config(text_value);
        });
    }
    for (const auto value : {"0", "2", "-1", "+1", "2147483647", "2147483648", "99999999999999999999", "1.0", ""}) {
        expect_throw<std::invalid_argument>("output pair is a safe one-based odd integer", [&] {
            (void)parse_application_config(configuration(std::string("OutputPair=") + value), device);
        });
    }
    expect("largest safe odd output pair is exact", parse_application_config(configuration("OutputPair=2147483645"), device).device.output_pair.value(),
        (std::numeric_limits<long>::max)() - 2);
    for (const auto key : {"Name", "OutputPair"}) {
        expect_throw<std::invalid_argument>("new device keys also require assignment", [&] {
            (void)parse_application_config(configuration(key), device);
        });
        const auto value = std::string(key) == "Name" ? "" : "1";
        expect_throw<std::invalid_argument>("new device keys are unique even when empty", [&] {
            (void)parse_application_config(configuration(std::string(key) + '=' + value + '\n' + key + '=' + value), device);
        });
    }
}

void test_apply_to_defaults()
{
    using wavplay::asio::DeviceSettings;
    const DeviceSettings defaults{.start_silence = 11ms, .buffer_frames = 144, .output_pair = 5};
    const auto check = [](std::string_view what, const DeviceSettings& applied, long silence, long frames, long pair) {
        expect(std::string(what) + ": start silence", applied.start_silence.count(), silence);
        expect(std::string(what) + ": buffer size", applied.buffer_frames, frames);
        expect(std::string(what) + ": output pair", applied.output_pair, pair);
    };
    check("no configured value keeps every default", wavplay::settings::DeviceConfig{}.apply(defaults), 11, 144, 5);
    check("unknown keys and other devices keep every default", parse_application_config(
        "[Device.{00000000-0000-0000-0000-000000000000}]\nBufferSize=96\n" + configuration("FutureKey=1\n"), device).device.apply(defaults),
        11, 144, 5);
    check("explicit zero silence replaces only silence",
        parse_application_config(configuration("StartSilence=0\n"), device).device.apply(defaults), 0, 144, 5);
    check("explicit zero width replaces only the width",
        parse_application_config(configuration("BufferSize=0\n"), device).device.apply(defaults), 11, 0, 5);
    check("output pair replaces only the pair",
        parse_application_config(configuration("OutputPair=1\n"), device).device.apply(defaults), 11, 144, 1);
    check("all configured values replace their defaults",
        parse_application_config(configuration("StartSilence=1000\nBufferSize=512\nOutputPair=3\n"), device).device.apply(defaults), 1000, 512, 3);
    // apply() only merges. A value the parser never produces still reaches
    // DeviceSettings::validate(), which callers run on the result.
    const auto unchecked = wavplay::settings::DeviceConfig{.buffer_frames = -1}.apply(defaults);
    expect("apply leaves validation to the caller", unchecked.buffer_frames, -1);
    expect_throw<std::invalid_argument>("validate rejects the merged value", [&] { unchecked.validate(); });
}

void test_window_values()
{
    const auto parsed = parse_application_config("[WINDOW]\nY=-50\nHeight=600\nX=-1920\nWidth=800\nMaximized=1\n");
    expect_true("complete normal rectangle is restored", parsed.window.has_value());
    expect("negative monitor coordinate", parsed.window->x, -1920);
    expect("negative top coordinate", parsed.window->y, -50);
    expect("restored width", parsed.window->width, 800);
    expect("restored height", parsed.window->height, 600);
    expect_true("maximized state is independent of normal rectangle", parsed.window->maximized);
    const auto minimum = parse_application_config("[Window]\nX=-2147483648\nY=-2147483648\nWidth=1\nHeight=1\n");
    expect("full signed coordinate range is parsed", minimum.window->x, (std::numeric_limits<int>::min)());
    expect_true("maximized defaults to false", !minimum.window->maximized);
    for (const auto text_value : {"X=0\nY=0\nWidth=800", "X=0\nY=0\nWidth=0\nHeight=600",
        "X=0\nY=0\nWidth=-1\nHeight=600", "X=2147483647\nY=0\nWidth=1\nHeight=600",
        "X=0\nY=2147483647\nWidth=800\nHeight=1", "X=2147483648\nY=0\nWidth=1\nHeight=600",
        "X=0\nY=0\nWidth=800\nHeight=600\nMaximized=2", "X=0\nY=0\nWidth=800\nHeight=600\nMaximized=-1",
        "X=0\nY=0\nWidth=800\nHeight=600\n[window]\nx=1", "X", "Maximized", "Maximized=0", "Maximized=1"}) {
        expect_throw<std::invalid_argument>("invalid, partial, duplicate or overflowing window settings are refused", [&] {
            (void)parse_application_config(std::string("[Window]\n") + text_value);
        });
    }
}

void test_save_preserves_manual_configuration()
{
    const std::string original = "\xef\xbb\xbf; retained comment \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\r\n"
        "[General]\r\nSelectedDevice={76543210-89AB-CDEF-8123-456789ABCDEF}\r\nFutureSetting=\"opaque\"\r\n"
        "[Device.{76543210-89AB-CDEF-8123-456789ABCDEF}]\r\nName=USB DAC\r\nOutputPair=3\r\nBufferSize=512\r\nStartSilence=75\r\n"
        "[Device.{11111111-1111-1111-1111-111111111111}]\r\nBufferSize=anything\r\n"
        "[Window] ; geometry\r\n x \t= -1920 ; old position\r\nY = 50 # top\r\nWidth=800\r\nHeight=600\r\n"
        "KeepWindowKey=yes\r\n[Future]\r\nOpaqueText";
    TempFile file;
    const auto path = file.write(bytes(original));
    const WindowPlacement moved{-100, 120, 900, 700, true};
    save_window_config(path, moved);
    auto expected = original;
    const auto replace_value = [&](std::string_view before, std::string_view after) {
        const auto position = expected.find(before);
        expect_true("fixture value exists", position != std::string::npos);
        expected.replace(position, before.size(), after);
    };
    replace_value("x \t= -1920", "x \t= -100");
    replace_value("Y = 50", "Y = 120");
    replace_value("Width=800", "Width=900");
    replace_value("Height=600", "Height=700");
    expected += "\r\n[Window]\r\nMaximized=1\r\n";
    expect_true("only known window values change byte for byte", file_contents(path) == expected);
    const auto loaded = load_application_config(path);
    expect("saved normal position roundtrips", loaded.window->x, moved.x);
    expect("saved height roundtrips", loaded.window->height, moved.height);
    expect_true("saved maximum state roundtrips", loaded.window->maximized);
    expect("manual output pair survives", loaded.device.output_pair.value(), 3);
    expect("manual buffer survives", loaded.device.buffer_frames.value(), 512);
    expect("manual silence survives", loaded.device.start_silence.value().count(), 75);
    {
        const auto raw = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (raw == INVALID_HANDLE_VALUE) { wavplay::core::throw_last_error("protect unchanged settings fixture"); }
        const wavplay::core::UniqueHandle protection(raw); // Denies replacement, but permits the reread.
        save_window_config(path, moved);
        expect_true("unchanged geometry performs no replacement", file_contents(path) == expected);
    }
    // Simulate a hand edit completed after startup. The next save must merge it,
    // never write the stale values loaded by that application instance.
    replace_value("BufferSize=512", "BufferSize=256");
    file.write(bytes(expected));
    save_window_config(path, {0, 0, 950, 720, false});
    expect("latest hand-edited buffer survives save", load_application_config(path).device.buffer_frames.value(), 256);
    expect_no_temporary(path);
}

void test_save_missing_and_failures()
{
    TempFile file;
    const auto path = file.write({});
    expect_true("remove initial file", DeleteFileW(path) != FALSE);
    const WindowPlacement placement{10, 20, 800, 600, false};
    save_window_config(path, placement);
    const auto created = load_application_config(path);
    expect_true("new settings do not invent a selected driver", !created.selected_device);
    expect("new settings contain the window", created.window->width, 800);
    expect_true("new settings provide the manual selection key", file_contents(path).find("SelectedDevice=\n") != std::string::npos);
    const auto original = file_contents(path);
    {
        const auto raw = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (raw == INVALID_HANDLE_VALUE) { wavplay::core::throw_last_error("protect settings replacement fixture"); }
        const wavplay::core::UniqueHandle protection(raw);
        expect_throw<std::system_error>("failed atomic replacement is reported", [&] {
            save_window_config(path, {30, 40, 900, 700, true});
        });
        expect_true("failed replacement keeps original bytes", file_contents(path) == original);
        expect_no_temporary(path);
    }
    {
        const auto raw = CreateFileW(path, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (raw == INVALID_HANDLE_VALUE) { wavplay::core::throw_last_error("lock settings read fixture"); }
        const wavplay::core::UniqueHandle protection(raw);
        expect_throw<std::system_error>("unreadable settings are never overwritten", [&] { save_window_config(path, placement); });
    }
    expect_true("failed reread keeps original bytes", file_contents(path) == original);
    {
        const auto attributes = GetFileAttributesW(path);
        if (attributes == INVALID_FILE_ATTRIBUTES) { wavplay::core::throw_last_error("read settings fixture attributes"); }
        struct RestoreAttributes {
            const wchar_t* path;
            DWORD attributes;
            ~RestoreAttributes() { SetFileAttributesW(path, attributes); }
        } restore{path, attributes};
        if (!SetFileAttributesW(path, attributes | FILE_ATTRIBUTE_READONLY)) {
            wavplay::core::throw_last_error("make settings fixture readonly");
        }
        expect_throw<std::system_error>("readonly settings save is reported", [&] {
            save_window_config(path, {30, 40, 900, 700, true});
        });
        expect_true("readonly save keeps original bytes", file_contents(path) == original);
        expect_no_temporary(path);
    }
    for (const auto& invalid : {std::string("\xff"), std::string("[General]\nSelectedDevice=invalid"),
        std::string("[Window]\nWidth=0"), std::string("[Unclosed"), std::string(wavplay::settings::max_config_bytes, ' ')}) {
        file.write(bytes(invalid));
        expect_throw<std::invalid_argument>("save refuses malformed input and output beyond the size bound", [&] {
            save_window_config(path, placement);
        });
        expect_true("refused save retains every original byte", file_contents(path) == invalid);
        expect_no_temporary(path);
    }
    file.write(bytes(original));
    for (const WindowPlacement& invalid : {WindowPlacement{0, 0, 0, 600}, WindowPlacement{0, 0, 800, -1},
        WindowPlacement{(std::numeric_limits<int>::max)(), 0, 1, 600}}) {
        expect_throw<std::invalid_argument>("invalid caller geometry cannot overwrite valid settings", [&] {
            save_window_config(path, invalid);
        });
    }
    expect_true("geometry failures keep original bytes", file_contents(path) == original);
    save_window_config(path, {30, 40, 900, 700, true});
    expect("a later successful save still works", load_application_config(path).window->width, 900);
    expect_no_temporary(path);
}

// A Window value the writer replaces must be assigned and nonblank: save
// refuses an unassigned or blank one before rewriting anything. Unknown Window
// keys may be either and survive a save byte for byte.
void test_save_blank_and_unassigned_window_values()
{
    TempFile file;
    const auto path = file.write({});
    const WindowPlacement placement{10, 20, 800, 600, false};
    for (const auto& invalid : {std::string("[Window]\nX\nY=0\nWidth=800\nHeight=600\n"),
        std::string("[Window]\nX= ; blank\nY=0\nWidth=800\nHeight=600\n"),
        std::string("[Window]\nX=0\nY=0\nWidth=800\nHeight= \t\r\n")}) {
        file.write(bytes(invalid));
        expect_throw<std::invalid_argument>("save refuses an unassigned or blank Window value", [&] {
            save_window_config(path, placement);
        });
        expect_true("a refused save keeps every byte", file_contents(path) == invalid);
        expect_no_temporary(path);
    }
    file.write(bytes("[Window]\nBare ; unassigned\nBlank= \t\nX=0\nY=0\nWidth=800\nHeight=600\nEnd="));
    save_window_config(path, placement);
    expect_true("unassigned and blank unknown Window keys survive a save", file_contents(path) ==
        "[Window]\nBare ; unassigned\nBlank= \t\nX=10\nY=20\nWidth=800\nHeight=600\nEnd=\n[Window]\nMaximized=0\n");
    expect_no_temporary(path);
}

// The repository's settings.example.ini. Every build writes this executable to
// out\x64\<configuration>\, three directories below the file.
std::filesystem::path example_settings_path()
{
    std::wstring module(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (!length || length >= module.size()) { wavplay::core::throw_last_error("GetModuleFileNameW(settings example)"); }
    module.resize(length);
    return std::filesystem::path(module).parent_path().parent_path().parent_path().parent_path() / L"settings.example.ini";
}

// The example as shipped parses to no device and no window. With its commented
// sections and assignments of the parser's keys enabled, it documents exactly
// those keys, each once, with the defaults, and its comments state the
// parser's StartSilence range and Window rules.
void test_settings_example()
{
    const auto path = example_settings_path();
    if (!std::filesystem::is_regular_file(path)) {
        const auto name = path.u8string();
        fail("settings.example.ini is not at " + std::string(name.begin(), name.end()));
    }
    const auto text = file_contents(path.c_str());
    const auto shipped = parse_application_config(text);
    expect_true("the shipped example selects no device", !shipped.selected_device);
    expect_true("the shipped example sets no window", !shipped.window);
    const auto unselected = parse_application_config(text, device).device;
    expect_true("the shipped example's device values are commented",
        !unselected.buffer_frames && !unselected.start_silence && !unselected.output_pair && unselected.name.empty());

    constexpr std::array<std::string_view, 9> keys{"Name", "OutputPair", "BufferSize", "StartSilence",
        "X", "Y", "Width", "Height", "Maximized"};
    std::array<unsigned, keys.size()> documented{};
    std::string enabled;
    for (std::size_t begin = 0; begin < text.size();) {
        auto end = text.find('\n', begin);
        if (end == std::string::npos) { end = text.size(); }
        std::string_view line(text.data() + begin, end - begin);
        begin = end + 1;
        if (line.ends_with('\r')) { line.remove_suffix(1); }
        if (line == "; [Device.{ASIO-driver-CLSID}]") {
            line = section.substr(0, section.size() - 1);
        } else if (line == "; [Window]") {
            line.remove_prefix(2);
        } else if (const auto assignment = line.find('='); line.starts_with("; ") && assignment != std::string_view::npos &&
            line.find_first_of(" :", 2) > assignment) {
            const auto key = line.substr(2, assignment - 2);
            const auto known = std::ranges::find(keys, key);
            if (known == keys.end()) { fail("the example documents a key the parser does not read: " + std::string(key)); }
            ++documented[static_cast<std::size_t>(known - keys.begin())];
            line.remove_prefix(2);
        }
        enabled.append(line).push_back('\n');
    }
    for (std::size_t index = 0; index < keys.size(); ++index) {
        expect("the example documents " + std::string(keys[index]) + " once", documented[index], 1);
    }
    const auto example = parse_application_config(enabled, device);
    const wavplay::asio::DeviceSettings defaults{};
    expect("the example's OutputPair is the default", example.device.output_pair.value(), defaults.output_pair);
    expect("the example's BufferSize is the default", example.device.buffer_frames.value(), defaults.buffer_frames);
    expect_true("the example's StartSilence is the default",
        example.device.start_silence.value() == wavplay::asio::default_start_silence);
    expect_true("the example names the device", example.device.name == L"Device name");
    expect_true("the example's window parses with the default Maximized=0",
        example.window.has_value() && !example.window->maximized);

    const std::regex range(R"(StartSilence: (\d+)\.\.(\d+) milliseconds)");
    std::smatch found;
    expect_true("the example states the StartSilence range", std::regex_search(text, found, range));
    const auto minimum = std::stol(found[1].str()), maximum = std::stol(found[2].str());
    expect("the documented StartSilence minimum", minimum, 0);
    expect("the documented StartSilence maximum", maximum, wavplay::asio::max_start_silence.count());
    expect("the documented maximum is accepted", parse_application_config(
        configuration("StartSilence=" + std::to_string(maximum)), device).device.start_silence.value().count(), maximum);
    expect_throw<std::invalid_argument>("beyond the documented maximum is refused", [&] {
        (void)parse_application_config(configuration("StartSilence=" + std::to_string(maximum + 1)), device);
    });
    expect_true("an all-zero CLSID selects no device, as documented", !parse_application_config(
        "[General]\nSelectedDevice={00000000-0000-0000-0000-000000000000}\n").selected_device);
    expect_throw<std::invalid_argument>("Maximized alone fails, as documented", [] {
        (void)parse_application_config("[Window]\nMaximized=0\n");
    });
    expect_throw<std::invalid_argument>("Maximized accepts only 0 or 1, as documented", [] {
        (void)parse_application_config("[Window]\nX=100\nY=100\nWidth=900\nHeight=600\nMaximized=2\n");
    });
}

} // namespace

int wavplay_test::run_device_config_tests()
{
    TestSuite suite;
    suite.run("device configuration recognizes only selected keys and preserves exact values", test_values_and_scope);
    suite.run("device configuration rejects invalid and duplicate settings", test_invalid_values);
    suite.run("device configuration requires bounded UTF-8 text", test_encoding_and_bound);
    suite.run("strict UTF-8 decoding keeps a BOM and NUL and refuses invalid text", test_strict_utf8_decoding);
    suite.run("device configuration is read once without writes and releases every handle", test_loader);
    suite.run_timed("settings reads and saves restore the caller's thread error mode on every exit", test_thread_error_mode_restored);
    suite.run("application configuration resolves the exact manual CLSID and output pair", test_application_selection);
    suite.run("device configuration applies each configured value over the caller's defaults", test_apply_to_defaults);
    suite.run("window configuration validates signed coordinates and complete safe rectangles", test_window_values);
    suite.run("window save preserves manual values, unknown content and comments", test_save_preserves_manual_configuration);
    suite.run("window save creates missing settings and preserves originals on failure", test_save_missing_and_failures);
    suite.run("window save refuses unassigned and blank Window values and keeps unknown ones", test_save_blank_and_unassigned_window_values);
    suite.run("the settings example parses and documents the parser's keys, defaults and ranges", test_settings_example);
    return suite.result("Device configuration");
}
