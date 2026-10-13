// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <util/crash_report.h>

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <clientversion.h>
#include <tinyformat.h>
#include <util/check.h>
#include <util/threadnames.h>
#include <util/time.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

#ifdef WIN32
#include <compat/compat.h>
#else
#include <fcntl.h>
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h>
#include <execinfo.h>
#define B3_CRASH_BACKTRACE 1
#endif
#endif

// Everything a handler touches is prepared by Install() in static storage,
// so writing a report needs no allocation, no locks and no C++ runtime.
namespace crash_report {
namespace {

constexpr int MAX_FRAMES{64};
constexpr size_t REPORT_CAPACITY{16384};
constexpr size_t STDERR_TAIL_BYTES{2048};
constexpr std::string_view ANNOUNCED_FILENAME{"announced"};

std::atomic_flag g_reporting;
std::atomic_flag g_message_claimed;
std::atomic_flag g_message_ready;
char g_message[2048];
char g_header[1024];
char g_report[REPORT_CAPACITY];
int64_t g_start_time{0};

std::mutex g_previous_mutex;
std::vector<fs::path> g_previous;

//! Appends text to a fixed buffer; output beyond its capacity is dropped.
class Writer
{
public:
    Writer(char* buffer, size_t capacity) : m_buf{buffer}, m_cap{capacity} {}

    void Str(const char* text)
    {
        while (text != nullptr && *text != '\0' && m_len + 1 < m_cap) m_buf[m_len++] = *text++;
    }
    void Char(char c)
    {
        if (m_len + 1 < m_cap) m_buf[m_len++] = c;
    }
    void Hex(uint64_t value)
    {
        char digits[16];
        int count{0};
        do {
            digits[count++] = "0123456789abcdef"[value & 0xf];
            value >>= 4;
        } while (value != 0);
        Str("0x");
        while (count > 0) Char(digits[--count]);
    }
    void Dec(int64_t value)
    {
        char digits[20];
        int count{0};
        uint64_t magnitude{value < 0 ? 0 - static_cast<uint64_t>(value) : static_cast<uint64_t>(value)};
        do {
            digits[count++] = static_cast<char>('0' + magnitude % 10);
            magnitude /= 10;
        } while (magnitude != 0);
        if (value < 0) Char('-');
        while (count > 0) Char(digits[--count]);
    }
    //! Module base name and the offset of `address` inside it.
    void Frame(int index, uint64_t address, const char* module, uint64_t base)
    {
        Str("  #");
        Dec(index);
        Char(' ');
        if (module != nullptr && base != 0 && address >= base) {
            const char* name{module};
            for (const char* p{module}; *p != '\0'; ++p) {
                if (*p == '/' || *p == '\\') name = p + 1;
            }
            Str(name);
            Char('+');
            Hex(address - base);
        } else {
            Hex(address);
        }
        Char('\n');
    }
    const char* Data() const { return m_buf; }
    size_t Size() const { return m_len; }
    void Clear() { m_len = 0; }

private:
    char* m_buf;
    size_t m_cap;
    size_t m_len{0};
};

void WriteCommon(Writer& w, const char* reason, const char* thread_name)
{
    w.Str(g_header);
    w.Str("uptime_seconds: ");
    w.Dec(static_cast<int64_t>(std::time(nullptr)) - g_start_time);
    w.Str("\nreason: ");
    w.Str(reason);
    w.Str("\nthread: ");
    w.Str(thread_name != nullptr && thread_name[0] != '\0' ? thread_name : "(unnamed)");
    w.Char('\n');
    const char* message{g_message_ready.test() ? g_message : AssertionFailureMessage()};
    if (message[0] != '\0') {
        w.Str("message: ");
        w.Str(message);
        w.Char('\n');
    }
}

#ifdef WIN32

wchar_t g_report_path[MAX_PATH * 4];
wchar_t g_stderr_path[MAX_PATH * 4];
bool g_stderr_captured{false};
HANDLE g_request_event{nullptr};
HANDLE g_done_event{nullptr};

struct Pending {
    const char* reason;
    DWORD code;
    uint64_t fault_address;
    bool access_violation;
    uint64_t access_address;
    bool access_write;
    const char* thread_name;
    CONTEXT context;
};
Pending g_pending;

const char* ExceptionName(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "datatype misalignment";
    case 0xC0000374: return "heap corruption";
    case 0xC0000409: return "stack buffer overrun or fail-fast";
    default: return "exception";
    }
}

void WriteOut(HANDLE file, const Writer& w)
{
    DWORD written{0};
    ::WriteFile(file, w.Data(), static_cast<DWORD>(w.Size()), &written, nullptr);
}

void ModuleOf(uint64_t address, char* name, size_t capacity, uint64_t& base)
{
    name[0] = '\0';
    base = 0;
    HMODULE module{nullptr};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(address)), &module)) {
        return;
    }
    wchar_t wide[MAX_PATH];
    const DWORD len{GetModuleFileNameW(module, wide, MAX_PATH)};
    // Keep only the base name, in ASCII; a path would reveal the user name.
    DWORD start{0};
    for (DWORD i{0}; i < len; ++i) {
        if (wide[i] == L'\\' || wide[i] == L'/') start = i + 1;
    }
    size_t out{0};
    for (DWORD i{start}; i < len && out + 1 < capacity; ++i) {
        name[out++] = wide[i] < 0x80 ? static_cast<char>(wide[i]) : '?';
    }
    name[out] = '\0';
    base = reinterpret_cast<uintptr_t>(module);
}

void AppendStderrTail(HANDLE file)
{
    if (!g_stderr_captured) return;
    HANDLE in{CreateFileW(g_stderr_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (in == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size;
    if (GetFileSizeEx(in, &size) && size.QuadPart > 0) {
        LARGE_INTEGER offset;
        offset.QuadPart = size.QuadPart > static_cast<LONGLONG>(STDERR_TAIL_BYTES) ? size.QuadPart - STDERR_TAIL_BYTES : 0;
        static char tail[STDERR_TAIL_BYTES];
        DWORD read{0};
        if (SetFilePointerEx(in, offset, nullptr, FILE_BEGIN) && ReadFile(in, tail, sizeof(tail), &read, nullptr) && read > 0) {
            Writer w{g_report, sizeof(g_report)};
            w.Str("stderr_tail: |\n");
            WriteOut(file, w);
            DWORD written{0};
            ::WriteFile(file, tail, read, &written, nullptr);
            ::WriteFile(file, "\n", 1, &written, nullptr);
        }
    }
    CloseHandle(in);
}

//! Runs on the handler thread, so a crash with an exhausted stack can still
//! be reported. The crashing thread waits until the report is written.
DWORD WINAPI ReportThread(LPVOID)
{
    WaitForSingleObject(g_request_event, INFINITE);
    HANDLE file{CreateFileW(g_report_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file != INVALID_HANDLE_VALUE) {
        Writer w{g_report, sizeof(g_report)};
        WriteCommon(w, g_pending.reason, g_pending.thread_name);
        if (g_pending.code != 0) {
            w.Str("exception_code: ");
            w.Hex(g_pending.code);
            w.Str(" (");
            w.Str(ExceptionName(g_pending.code));
            w.Str(")\n");
        }
        if (g_pending.access_violation) {
            w.Str(g_pending.access_write ? "access: write " : "access: read ");
            w.Hex(g_pending.access_address);
            w.Char('\n');
        }
        // Write what is known before walking a possibly damaged stack.
        WriteOut(file, w);
        w.Clear();
        w.Str("frames:\n");
#if defined(_M_X64) || defined(__x86_64__)
        CONTEXT context{g_pending.context};
        for (int i{0}; i < MAX_FRAMES && context.Rip != 0; ++i) {
            char module[MAX_PATH];
            uint64_t base;
            ModuleOf(context.Rip, module, sizeof(module), base);
            w.Frame(i, context.Rip, module, base);
            DWORD64 image_base{0};
            PRUNTIME_FUNCTION function{RtlLookupFunctionEntry(context.Rip, &image_base, nullptr)};
            if (function == nullptr) {
                // Leaf function: the return address is on top of the stack.
                DWORD64 return_address{0};
                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(context.Rsp)), &return_address,
                                       sizeof(return_address), nullptr)) {
                    break;
                }
                context.Rip = return_address;
                context.Rsp += sizeof(return_address);
            } else {
                PVOID handler_data{nullptr};
                DWORD64 establisher_frame{0};
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data,
                                 &establisher_frame, nullptr);
            }
        }
#else
        if (g_pending.fault_address != 0) {
            char module[MAX_PATH];
            uint64_t base;
            ModuleOf(g_pending.fault_address, module, sizeof(module), base);
            w.Frame(0, g_pending.fault_address, module, base);
        }
#endif
        WriteOut(file, w);
        AppendStderrTail(file);
        CloseHandle(file);
    }
    SetEvent(g_done_event);
    return 0;
}

void RequestReport()
{
    SetEvent(g_request_event);
    WaitForSingleObject(g_done_event, 15000);
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info)
{
    if (!g_reporting.test_and_set()) {
        const EXCEPTION_RECORD& record{*info->ExceptionRecord};
        g_pending.reason = "fatal exception";
        g_pending.code = record.ExceptionCode;
        g_pending.fault_address = reinterpret_cast<uintptr_t>(record.ExceptionAddress);
        g_pending.access_violation = record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2;
        if (g_pending.access_violation) {
            g_pending.access_write = record.ExceptionInformation[0] != 0;
            g_pending.access_address = record.ExceptionInformation[1];
        }
        g_pending.thread_name = util::ThreadGetInternalNameRaw();
        g_pending.context = *info->ContextRecord;
        RequestReport();
    }
    // Let Windows Error Reporting record the crash as before.
    return EXCEPTION_CONTINUE_SEARCH;
}

void AbortSignalHandler(int)
{
    if (!g_reporting.test_and_set()) {
        g_pending.reason = "abort";
        g_pending.code = 0;
        g_pending.thread_name = util::ThreadGetInternalNameRaw();
        RtlCaptureContext(&g_pending.context);
        RequestReport();
    }
    // Returning lets abort() terminate the process.
}

std::string OsDescription()
{
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    const HMODULE ntdll{GetModuleHandleW(L"ntdll.dll")};
    const auto get_version{ntdll ? reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion"))) : nullptr};
    if (get_version == nullptr || get_version(&version) != 0) return "Windows";
#if defined(_M_X64) || defined(__x86_64__)
    const char* arch{"x86_64"};
#elif defined(_M_ARM64) || defined(__aarch64__)
    const char* arch{"arm64"};
#else
    const char* arch{"x86"};
#endif
    return strprintf("Windows %u.%u.%u %s", version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber, arch);
}

bool InstallPlatform(const fs::path& report, const fs::path& directory)
{
    const std::wstring wide{report.wstring()};
    if (wide.size() + 1 > std::size(g_report_path)) return false;
    std::copy(wide.begin(), wide.end(), g_report_path);
    g_report_path[wide.size()] = L'\0';

    // A GUI process has no console, so an assertion message written to
    // stderr is lost. Keep it in a file the report can quote.
    const HANDLE err{GetStdHandle(STD_ERROR_HANDLE)};
    if (err == nullptr || err == INVALID_HANDLE_VALUE) {
        const std::wstring err_path{(directory / "stderr.log").wstring()};
        if (err_path.size() + 1 <= std::size(g_stderr_path) && _wfreopen(err_path.c_str(), L"w", stderr) != nullptr) {
            std::copy(err_path.begin(), err_path.end(), g_stderr_path);
            g_stderr_path[err_path.size()] = L'\0';
            setvbuf(stderr, nullptr, _IONBF, 0);
            // Send CRT assertion messages to stderr instead of a dialog.
            _set_error_mode(_OUT_TO_STDERR);
            g_stderr_captured = true;
        }
    }

    g_request_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_done_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_request_event == nullptr || g_done_event == nullptr) return false;
    HANDLE thread{CreateThread(nullptr, 256 * 1024, ReportThread, nullptr, 0, nullptr)};
    if (thread == nullptr) return false;
    CloseHandle(thread);

    // Reserve stack so the filter itself can run after a stack overflow.
    ULONG guarantee{64 * 1024};
    SetThreadStackGuarantee(&guarantee);
    SetUnhandledExceptionFilter(CrashFilter);
    std::signal(SIGABRT, AbortSignalHandler);
    return true;
}

#else // WIN32

char g_report_path[4096];
alignas(16) char g_alt_stack[64 * 1024];

const char* SignalName(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    default: return "signal";
    }
}

void WriteAll(int fd, const Writer& w)
{
    const char* data{w.Data()};
    size_t left{w.Size()};
    while (left > 0) {
        const ssize_t n{write(fd, data, left)};
        if (n <= 0) return;
        data += n;
        left -= static_cast<size_t>(n);
    }
}

void SignalHandler(int sig, siginfo_t* info, void*)
{
    if (!g_reporting.test_and_set()) {
        const int fd{open(g_report_path, O_WRONLY | O_CREAT | O_TRUNC, 0600)};
        if (fd >= 0) {
            Writer w{g_report, sizeof(g_report)};
            WriteCommon(w, sig == SIGABRT ? "abort" : "fatal signal", util::ThreadGetInternalNameRaw());
            w.Str("signal: ");
            w.Dec(sig);
            w.Str(" (");
            w.Str(SignalName(sig));
            w.Str(")\n");
            if (sig != SIGABRT && info != nullptr) {
                w.Str("fault_address: ");
                w.Hex(reinterpret_cast<uintptr_t>(info->si_addr));
                w.Char('\n');
            }
            // Write what is known before walking a possibly damaged stack.
            WriteAll(fd, w);
            w.Clear();
            w.Str("frames:\n");
#ifdef B3_CRASH_BACKTRACE
            void* frames[MAX_FRAMES];
            const int count{backtrace(frames, MAX_FRAMES)};
            for (int i{0}; i < count; ++i) {
                Dl_info module{};
                const bool found{dladdr(frames[i], &module) != 0};
                w.Frame(i, reinterpret_cast<uintptr_t>(frames[i]), found ? module.dli_fname : nullptr,
                        found ? reinterpret_cast<uintptr_t>(module.dli_fbase) : 0);
            }
#endif
            WriteAll(fd, w);
            close(fd);
        }
    }
    // SA_RESETHAND restored the default action: die the usual way.
    raise(sig);
}

std::string OsDescription()
{
    struct utsname name;
    if (uname(&name) != 0) return "unknown";
    return strprintf("%s %s %s", name.sysname, name.release, name.machine);
}

bool InstallPlatform(const fs::path& report, const fs::path&)
{
    const std::string narrow{fs::PathToString(report)};
    if (narrow.size() + 1 > sizeof(g_report_path)) return false;
    std::memcpy(g_report_path, narrow.c_str(), narrow.size() + 1);

#ifdef B3_CRASH_BACKTRACE
    // The first backtrace() call may load libgcc; never do that in a handler.
    void* warmup[1];
    backtrace(warmup, 1);
#endif
    stack_t alt{};
    alt.ss_sp = g_alt_stack;
    alt.ss_size = sizeof(g_alt_stack);
    sigaltstack(&alt, nullptr);

    struct sigaction action{};
    action.sa_sigaction = SignalHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    for (const int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) {
        if (sigaction(sig, &action, nullptr) != 0) return false;
    }
    return true;
}

#endif // WIN32

[[noreturn]] void TerminateHandler()
{
    std::string message{"std::terminate called without an active exception"};
    if (const std::exception_ptr current{std::current_exception()}) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception& e) {
            message = strprintf("uncaught exception: %s", e.what());
        } catch (...) {
            message = "uncaught exception of unknown type";
        }
    }
    // Keep the first message if several threads terminate at once.
    if (!g_message_claimed.test_and_set()) {
        const size_t len{std::min(message.size(), sizeof(g_message) - 1)};
        std::memcpy(g_message, message.data(), len);
        g_message[len] = '\0';
        g_message_ready.test_and_set();
    }
    message += '\n';
    std::fwrite(message.data(), 1, message.size(), stderr);
    std::abort();
}

//! Reports newer than the last announced one; records the newest as announced.
std::vector<fs::path> TakeNewReports(const fs::path& directory)
{
    const fs::path announced_path{directory / fs::u8path(std::string{ANNOUNCED_FILENAME})};
    std::string announced;
    if (FILE* in{fsbridge::fopen(announced_path, "rb")}) {
        char line[256];
        if (std::fgets(line, sizeof(line), in) != nullptr) {
            announced = line;
            while (!announced.empty() && (announced.back() == '\n' || announced.back() == '\r')) announced.pop_back();
        }
        std::fclose(in);
    }
    std::vector<fs::path> reports;
    std::error_code ec;
    for (fs::directory_iterator it{directory, ec}, end; !ec && it != end; it.increment(ec)) {
        const std::string name{fs::PathToString(it->path().filename())};
        if (name.starts_with("crash-") && name.ends_with(".txt") && name > announced) {
            reports.push_back(it->path());
        }
    }
    std::sort(reports.begin(), reports.end());
    if (!reports.empty()) {
        if (FILE* out{fsbridge::fopen(announced_path, "wb")}) {
            const std::string newest{fs::PathToString(reports.back().filename()) + "\n"};
            std::fwrite(newest.data(), 1, newest.size(), out);
            std::fclose(out);
        }
    }
    return reports;
}

int Recurse(volatile int depth)
{
    volatile char frame[4096];
    frame[depth % sizeof(frame)] = static_cast<char>(depth);
    if (depth >= 0) return Recurse(depth + 1) + frame[0];
    return frame[0];
}

} // namespace

std::optional<std::vector<fs::path>> Install(const fs::path& directory)
{
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec) return std::nullopt;

    std::vector<fs::path> previous{TakeNewReports(directory)};

    g_start_time = static_cast<int64_t>(std::time(nullptr));
    std::string stamp{FormatISO8601DateTime(g_start_time)};
    std::erase(stamp, '-');
    std::erase(stamp, ':');
#ifdef WIN32
    const unsigned long pid{GetCurrentProcessId()};
#else
    const unsigned long pid{static_cast<unsigned long>(getpid())};
#endif
    const fs::path report{directory / fs::u8path(strprintf("crash-%s-%lu.txt", stamp, pid))};

    const std::string header{strprintf(
        "B3 crash report (contains no keys, wallet data, memory or log contents)\n"
        "version: %s %s\n"
        "os: %s\n"
        "started: %s\n",
        CLIENT_NAME, FormatFullVersion(), OsDescription(), FormatISO8601DateTime(g_start_time))};
    if (header.size() + 1 > sizeof(g_header)) return std::nullopt;
    std::memcpy(g_header, header.c_str(), header.size() + 1);

    if (!InstallPlatform(report, directory)) return std::nullopt;
    std::set_terminate(TerminateHandler);

    {
        std::lock_guard lock{g_previous_mutex};
        g_previous = previous;
    }
    return previous;
}

std::vector<fs::path> PreviousReports()
{
    std::lock_guard lock{g_previous_mutex};
    return g_previous;
}


void TriggerTestCrash(std::string_view kind)
{
    if (kind == "segv") {
        volatile int* volatile null_pointer{nullptr};
        *null_pointer = 1;
    } else if (kind == "abort") {
        std::abort();
    } else if (kind == "assert") {
        volatile bool never{false};
        assert(never);
    } else if (kind == "check") {
        volatile int* volatile null_pointer{nullptr};
        (void)Assert(null_pointer);
    } else if (kind == "throw") {
        std::thread{[] {
            util::ThreadRename("crashtest");
            throw std::runtime_error{"crash report test exception"};
        }}.join();
    } else if (kind == "overflow") {
        std::thread{[] {
            util::ThreadRename("crashtest");
            Recurse(0);
        }}.join();
    }
}

} // namespace crash_report
