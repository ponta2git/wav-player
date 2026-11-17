#include "suites/suites.h"
#include "wavplay/playlist/playlist.h"
#include "wavplay/core/win32.h"
#include "fixtures/call_gate.h"
#include "fixtures/wav_builder.h"
#include "framework/test_suite.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace wavplay_test;
using wavplay::playlist::load;
using wavplay::playlist::parse;

std::string utf8(std::wstring_view text)
{
    if (text.empty()) { return {}; }
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!count) { wavplay::core::throw_last_error("Encode playlist fixture"); }
    std::string result(static_cast<std::size_t>(count), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        result.data(), count, nullptr, nullptr)) { wavplay::core::throw_last_error("Encode playlist fixture"); }
    return result;
}

Bytes bytes(std::string_view text)
{
    return Bytes(text.begin(), text.end());
}

void test_text_and_exclusions()
{
    TempFile first, second, broken, missing;
    const std::wstring a(first.write(wave({pcm16(2, 48000), pcm16_ramp(2, 5)})));
    const std::wstring b(second.write(wave({pcm24(1, 44100), pcm24_ramp(1, 3)})));
    const std::wstring bad(broken.write(bytes("not a WAV")));
    // Keep the reserved temporary name alive, so that no other fixture in this
    // process can take it (and the missing row's derived name) during the test.
    const std::wstring absent = std::wstring(missing.write({})) + L".playlist-missing";

    const auto result = parse("\xef\xbb\xbf# comment \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\r\n\n" +
        utf8(a) + "\r\n" + utf8(b) + "\n" + utf8(a) + "\r" + utf8(bad) + "\n" + utf8(absent) +
        "\n # not a comment\n\t\nrelative.wav\nC:relative.wav\n\\root-relative.wav\n");
    expect("valid rows keep duplicate entries", result.paths.size(), 3);
    expect_true("row order and full original path are retained", result.paths == std::vector<std::wstring>{a, b, a});
    expect("invalid nonempty rows each count once", result.excluded_count, 7);
    expect("empty list is valid", parse("").paths.size(), 0);
    expect("BOM and comments alone have no exclusions", parse("\xef\xbb\xbf#comment\r\n\n\r").excluded_count, 0);
    const auto duplicate_invalid = parse(utf8(absent) + "\n" + utf8(absent));
    expect("invalid duplicates still count separately", duplicate_invalid.excluded_count, 2);
}

void test_invalid_utf8_is_atomic()
{
    TempFile valid;
    const auto prefix = utf8(valid.write(wave({pcm16(1, 48000), pcm16_ramp(1, 1)}))) + '\n';
    for (const auto invalid : {"\xc0\xaf", "\xe3\x81", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xff\xfe"}) {
        expect_throw<std::invalid_argument>("invalid UTF-8 after a valid row refuses the complete list", [&] {
            (void)parse(prefix + invalid);
        });
        expect_throw<std::invalid_argument>("even ignored comments must contain valid UTF-8", [&] {
            (void)parse(prefix + '#' + invalid);
        });
    }
    const auto embedded_nul = parse(prefix + prefix.substr(0, prefix.size() - 1) + std::string("\0suffix", 7));
    expect("NUL cannot alias an existing filename", embedded_nul.paths.size(), 1);
    expect("NUL is an excluded filename, not malformed UTF-8", embedded_nul.excluded_count, 1);
}

class FileTree {
public:
    FileTree()
    {
        TempFile name;
        root_ = L"\\\\?\\" + std::wstring(name.write({})) + L"-playlist";
        add_directory(root_);
    }
    ~FileTree()
    {
        for (const auto& file : files_) { DeleteFileW(file.c_str()); }
        for (auto directory = directories_.rbegin(); directory != directories_.rend(); ++directory) {
            RemoveDirectoryW(directory->c_str());
        }
    }
    FileTree(const FileTree&) = delete;
    FileTree& operator=(const FileTree&) = delete;

    std::wstring copy(const wchar_t* source, std::wstring_view name)
    {
        const auto path = root_ + L'\\' + std::wstring(name);
        files_.push_back(path);
        if (!CopyFileW(source, path.c_str(), TRUE)) { wavplay::core::throw_last_error("Copy playlist fixture"); }
        return path;
    }
    void deepen()
    {
        root_ += L'\\' + std::wstring(120, L'x');
        add_directory(root_);
    }
    const std::wstring& root() const noexcept { return root_; }

private:
    void add_directory(const std::wstring& path)
    {
        directories_.push_back(path);
        if (!CreateDirectoryW(path.c_str(), nullptr)) { wavplay::core::throw_last_error("Create playlist fixture directory"); }
    }
    std::wstring root_;
    std::vector<std::wstring> directories_, files_;
};

void test_unicode_long_paths_and_spaces()
{
    TempFile media;
    const auto source = media.write(wave({pcm16(1, 48000), pcm16_ramp(1, 4)}));
    FileTree tree;
    const auto unicode = tree.copy(source, L" 日本語 \U0001f3b5 #曲.wav");
    const auto trailing = tree.copy(source, L"末尾空白.wav ");
    tree.deepen();
    tree.deepen();
    const auto long_path = tree.copy(source, L"長いパス.wav");
    expect_true("fixture exceeds legacy MAX_PATH", long_path.size() > MAX_PATH);
    const auto result = parse(utf8(unicode) + '\n' + utf8(trailing) + '\n' + utf8(long_path) + '\n' +
        utf8(long_path.substr(4)) + '\n' + utf8(trailing.substr(4)));
    expect("Unicode, extended and ordinary long paths are accepted", result.excluded_count, 0);
    expect_true("all spelling including trailing spaces is preserved", result.paths ==
        std::vector<std::wstring>{unicode, trailing, long_path, long_path.substr(4), trailing.substr(4)});
    auto dotted = long_path.substr(4);
    dotted.insert(dotted.find_last_of(L'\\') + 1, L".\\..\\" + std::wstring(120, L'x') + L"\\");
    auto slashes = long_path.substr(4);
    std::ranges::replace(slashes, L'\\', L'/');
    const auto normalized = parse(utf8(dotted) + '\n' + utf8(slashes));
    expect("absolute dot components and forward separators still resolve", normalized.paths.size(), 2);
    expect_true("display paths are not normalized", normalized.paths == std::vector<std::wstring>{dotted, slashes});
    const auto different_names = parse(utf8(unicode) + " \n " + utf8(unicode) + '\n' + utf8(unicode) + '\t');
    expect("surrounding whitespace does not alias the unspaced filename", different_names.paths.size(), 0);
    expect("whitespace-bearing missing names are excluded", different_names.excluded_count, 3);
    expect("directory is not parseable media", parse(utf8(tree.root())).excluded_count, 1);
}

void test_nonlocal_and_namespace_paths()
{
    for (const auto path : {"\\\\server\\share\\track.wav", "\\\\?\\UNC\\server\\share\\track.wav",
        "//server/share/track.wav", "\\\\.\\C:\\track.wav", "\\\\?\\GLOBALROOT\\Device\\track.wav",
        "\\??\\C:\\track.wav", "file:///C:/track.wav", "https://example.invalid/track.wav",
        "\\\\?\\C:track.wav", "\\\\?\\Volume{bad-guid}\\track.wav", "1:\\track.wav"}) {
        const auto result = parse(path);
        expect("unsupported or nonlocal path is excluded", result.excluded_count, 1);
        expect("unsupported namespace never becomes a row", result.paths.size(), 0);
    }
}

void test_mapped_network_drive()
{
    // A private DOS-device alias identifies MUP as a network drive without
    // contacting a server, mapping credentials, or requiring a real share.
    constexpr auto target = L"\\Device\\Mup\\127.0.0.1\\wavplay-playlist-test";
    const auto drives = GetLogicalDrives();
    if (!drives) { wavplay::core::throw_last_error("Enumerate drive letters for playlist fixture"); }
    wchar_t letter = L'Z';
    while (letter >= L'D' && (drives & (1UL << (letter - L'A')))) { --letter; }
    expect_true("an unused drive letter is available", letter >= L'D');
    const std::array<wchar_t, 3> device{letter, L':', L'\0'};
    if (!DefineDosDeviceW(DDD_RAW_TARGET_PATH | DDD_NO_BROADCAST_SYSTEM, device.data(), target)) {
        wavplay::core::throw_last_error("Define private network-drive fixture");
    }
    struct RemoveAlias {
        const wchar_t* name;
        const wchar_t* destination;
        ~RemoveAlias()
        {
            if (name) {
                DefineDosDeviceW(DDD_RAW_TARGET_PATH | DDD_NO_BROADCAST_SYSTEM | DDD_REMOVE_DEFINITION |
                    DDD_EXACT_MATCH_ON_REMOVE, name, destination);
            }
        }
    } cleanup{device.data(), target};
    const auto root = std::wstring(device.data()) + L'\\';
    expect("fixture is a mapped network drive", GetDriveTypeW(root.c_str()), DRIVE_REMOTE);
    const auto path = root + L"track.wav";
    const auto result = parse(utf8(path) + '\n' + utf8(L"\\\\?\\" + path));
    expect("both mapped drive spellings are excluded", result.excluded_count, 2);
    expect("network-drive paths never become list rows", result.paths.size(), 0);
    if (!DefineDosDeviceW(DDD_RAW_TARGET_PATH | DDD_NO_BROADCAST_SYSTEM | DDD_REMOVE_DEFINITION |
        DDD_EXACT_MATCH_ON_REMOVE, device.data(), target)) {
        wavplay::core::throw_last_error("Remove private network-drive fixture");
    }
    cleanup.name = nullptr;
}

void test_volume_guid_path()
{
    TempFile media;
    const auto source = media.write(wave({pcm16(1, 48000), pcm16_ramp(1, 1)}));
    const auto raw = CreateFileW(source, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (raw == INVALID_HANDLE_VALUE) { wavplay::core::throw_last_error("Open GUID-path fixture"); }
    const wavplay::core::UniqueHandle file(raw);
    std::array<wchar_t, 1024> final_path{};
    const auto size = GetFinalPathNameByHandleW(file.get(), final_path.data(),
        static_cast<DWORD>(final_path.size()), VOLUME_NAME_GUID);
    if (!size) { wavplay::core::throw_last_error("Resolve GUID-path fixture"); }
    expect_true("GUID fixture path fits", size < final_path.size());
    const std::wstring path(final_path.data(), size);
    const auto result = parse(utf8(path));
    expect("local volume GUID path accepted", result.paths.size(), 1);
    expect_true("volume GUID spelling retained", result.paths.front() == path);
    auto lower = path;
    lower[4] = L'v';
    expect("volume namespace is case insensitive", parse(utf8(lower)).paths.size(), 1);
}

void test_load_errors_and_startup_snapshot()
{
    TempFile media, list;
    const std::wstring media_path(media.write(wave({pcm16(1, 48000), pcm16_ramp(1, 2)})));
    const std::wstring list_path(list.write(bytes(utf8(media_path))));
    const auto result = load(list_path.c_str());
    expect("file loading parses the content", result.paths.size(), 1);
    list.write(bytes("# changed after loading"));
    expect("loaded snapshot does not follow disk changes", result.paths.size(), 1);
    expect("another explicit load sees changed file", load(list_path.c_str()).paths.size(), 0);
    expect("WAV handles are released after snapshot validation", probe_file_access(media_path.c_str(), GENERIC_WRITE), ERROR_SUCCESS);

    list.write(bytes("\xff"));
    expect_throw<std::invalid_argument>("invalid file encoding is a load error", [&] { (void)load(list_path.c_str()); });
    const auto raw = CreateFileW(list_path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (raw == INVALID_HANDLE_VALUE) { wavplay::core::throw_last_error("Lock playlist fixture"); }
    wavplay::core::UniqueHandle lock(raw);
    expect_throw<std::system_error>("sharing denial is a load error", [&] { (void)load(list_path.c_str()); });
    lock.reset();
    const auto missing_path = list_path + L".missing-playlist";
    expect_throw<std::system_error>("missing playlist is a load error", [&] { (void)load(missing_path.c_str()); });
}

void test_all_media_validation()
{
    TempFile valid, invalid, unsupported, empty;
    const std::wstring good(valid.write(wave({extensible_pcm(2, 48000, 24, 32), pcm24_in32_ramp(2, 4)})));
    auto nonzero = pcm24_in32_ramp(2, 4);
    // The low unused byte of the last sample must still be inspected.
    nonzero.payload[nonzero.payload.size() - 4] = 1;
    const std::wstring bad(invalid.write(wave({extensible_pcm(2, 48000, 24, 32), nonzero})));
    const std::wstring unknown(unsupported.write(wave({pcm16(2, 12345), pcm16_ramp(2, 1)})));
    const std::wstring no_samples(empty.write(wave({pcm16(2, 48000), pcm16_ramp(2, 0)})));
    const auto result = parse(utf8(good) + '\n' + utf8(bad) + '\n' + utf8(unknown) + '\n' + utf8(no_samples));
    expect("valid Extensible media accepted", result.paths.size(), 1);
    expect("all data and source-format restrictions apply at loading", result.excluded_count, 3);
}

// A stop requested before the load reaches the first entry opens no entry and
// returns no list, not an empty one. The load runs on another thread, so that
// an open the gate holds would not block the check.
void test_requested_stop_opens_no_entry()
{
    TempFile media, list;
    const std::wstring media_path(media.write(wave({pcm16(1, 48000), pcm16_ramp(1, 2)})));
    const std::wstring list_path(list.write(bytes("# list\n" + utf8(media_path) + '\n' + utf8(media_path))));
    const std::atomic<bool> requested{true}, running{false};
    {
        OpenGate entry(media_path.c_str());
        auto loaded = std::async(std::launch::async, [&] { return load(list_path.c_str(), requested); });
        const auto finished = loaded.wait_for(hang_budget(std::chrono::seconds(5))) == std::future_status::ready;
        entry.open();
        expect_true("a requested stop opens no entry", !entry.reached());
        expect_true("a requested stop does not wait for an entry", finished);
        expect_true("a requested stop returns no list", !loaded.get().has_value());
    }
    expect_true("a requested stop also refuses parsed text", !parse(utf8(media_path), requested).has_value());
    const auto unstopped = load(list_path.c_str(), running);
    expect_true("an unset flag loads the whole list", unstopped && unstopped->paths.size() == 2 && unstopped->excluded_count == 0);
}

// A stop raised while the last entry is validated has no later entry to stop
// before, so the load returns the complete list. The flag is read only before
// entries: neither the LF that a CRLF leaves nor trailing blank and comment
// lines may turn that into no list.
void test_stop_during_last_entry_returns_list()
{
    TempFile media;
    const std::wstring media_path(media.write(wave({pcm16(1, 48000), pcm16_ramp(1, 2)})));
    struct Ending {
        std::string_view name, text;
    };
    for (const auto& [name, text] : {Ending{"no line end", ""}, Ending{"LF", "\n"}, Ending{"CRLF", "\r\n"},
        Ending{"CRLF, comment and blank line", "\r\n# end\r\n\r\n"}}) {
        TempFile list;
        const std::wstring list_path(list.write(bytes("# list\r\n" + utf8(media_path) + std::string(text))));
        std::atomic<bool> stop{false};
        OpenGate entry(media_path.c_str());
        auto loaded = std::async(std::launch::async, [&] { return load(list_path.c_str(), stop); });
        ReleaseOpen release{entry}; // Opens the gate before the future waits on a failure.
        entry.wait();
        stop.store(true, std::memory_order_relaxed);
        entry.open();
        const auto result = loaded.get();
        expect_true("a stop during the last entry returns the complete list: " + std::string(name),
            result && result->paths == std::vector<std::wstring>{media_path} && result->excluded_count == 0);
    }
}

// Loading, with its volume queries, media validation and a missing row, leaves
// the caller's thread error mode as it was. That those calls fail critical errors
// (an empty card reader or optical drive) while they run is not observable here;
// it rests on code review and the manual no-media check. Runs on a dedicated
// thread (run_timed).
void test_thread_error_mode_restored()
{
    TempFile media, list;
    const std::wstring media_path(media.write(wave({pcm16(1, 48000), pcm16_ramp(1, 2)})));
    const auto absent = media_path + L".playlist-missing";
    const std::wstring list_path(list.write(bytes(utf8(media_path) + '\n' + utf8(absent))));
    for (const DWORD mode : {DWORD{0}, DWORD{SEM_NOGPFAULTERRORBOX}}) {
        if (!SetThreadErrorMode(mode, nullptr)) { wavplay::core::throw_last_error("SetThreadErrorMode"); }
        const auto result = load(list_path.c_str());
        expect("valid row with a restored mode", result.paths.size(), 1);
        expect("missing row is excluded", result.excluded_count, 1);
        expect("loading restores the caller's mode", GetThreadErrorMode(), mode);
    }
}

} // namespace

int wavplay_test::run_playlist_tests()
{
    TestSuite suite;
    suite.run("UTF-8 list order, delimiters, duplicate and exclusion counts", test_text_and_exclusions);
    suite.run("Invalid UTF-8 refuses the whole playlist", test_invalid_utf8_is_atomic);
    suite.run("Unicode, long paths and literal whitespace", test_unicode_long_paths_and_spaces);
    suite.run("Nonlocal and device namespaces", test_nonlocal_and_namespace_paths);
    suite.run("Mapped network drive rejection", test_mapped_network_drive);
    suite.run("Local volume GUID paths", test_volume_guid_path);
    suite.run("Playlist file errors and immutable startup snapshot", test_load_errors_and_startup_snapshot);
    suite.run("Complete supported WAV validation", test_all_media_validation);
    suite.run_timed("A requested stop opens no entry and returns no list", test_requested_stop_opens_no_entry);
    suite.run_timed("A stop during the last entry returns the complete list", test_stop_during_last_entry_returns_list);
    suite.run_timed("Loading restores the caller's thread error mode", test_thread_error_mode_restored);
    return suite.result("Playlist");
}
