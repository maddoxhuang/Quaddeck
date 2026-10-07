#include "HangWatchdog.hpp"

#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <vector>

namespace quaddeck {

namespace {

constexpr int kMaximumFrames = 48;

struct ThreadStack {
    DWORD id{};
    int count{};
    DWORD64 frames[kMaximumFrames]{};
};

// Runs while the thread is suspended, so it touches neither the heap nor a
// lock: the suspended thread may be holding either. A stack that cannot be
// read ends the walk instead of the process.
int unwindStack(const CONTEXT& start, DWORD64* frames, int capacity) noexcept {
#if defined(_M_X64)
    CONTEXT context = start;
    int count = 0;
    __try {
        while (count < capacity && context.Rip != 0) {
            frames[count++] = context.Rip;
            const DWORD64 previousStack = context.Rsp;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
            if (function) {
                PVOID handlerData = nullptr;
                DWORD64 establisherFrame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context,
                                 &handlerData, &establisherFrame, nullptr);
            } else {
                // A leaf function: the return address is on top of the stack.
                context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
                context.Rsp += sizeof(DWORD64);
            }
            if (context.Rsp <= previousStack) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return count;
#else
    (void)start; (void)frames; (void)capacity;
    return 0;
#endif
}

std::vector<DWORD> processThreads() {
    std::vector<DWORD> threads;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return threads;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD process = GetCurrentProcessId();
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID == process) threads.push_back(entry.th32ThreadID);
    }
    CloseHandle(snapshot);
    return threads;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(std::max(size, 0)), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                            result.data(), size, nullptr, nullptr);
    }
    return result;
}

std::string threadName(DWORD id) {
    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
    if (!thread) return {};
    PWSTR description = nullptr;
    std::string name;
    if (SUCCEEDED(GetThreadDescription(thread, &description)) && description) {
        name = narrow(description);
        LocalFree(description);
    }
    CloseHandle(thread);
    return name;
}

// Function names and lines come from dbghelp: QuadDeck's own from the PDB
// beside the executable, a DLL without one by its exports, which names the
// nearest exported function and so is only a hint. The module and offset are
// always written, and stand on their own when dbghelp has nothing.
class Symbolizer {
public:
    Symbolizer() {
        process_ = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES |
                      SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
        std::wstring modulePath(MAX_PATH, L'\0');
        const DWORD written = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
        modulePath.resize(written);
        const std::wstring directory = std::filesystem::path(modulePath).parent_path().wstring();
        ready_ = SymInitializeW(process_, directory.empty() ? nullptr : directory.c_str(), TRUE) != FALSE;
    }
    ~Symbolizer() {
        if (ready_) SymCleanup(process_);
    }
    Symbolizer(const Symbolizer&) = delete;
    Symbolizer& operator=(const Symbolizer&) = delete;

    std::string describe(DWORD64 address) const {
        std::ostringstream out;
        HMODULE module = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(address), &module) && module) {
            wchar_t path[MAX_PATH]{};
            GetModuleFileNameW(module, path, MAX_PATH);
            out << narrow(std::filesystem::path(path).stem().wstring()) << "+0x" << std::hex
                << (address - reinterpret_cast<DWORD64>(module)) << std::dec;
        } else {
            out << "0x" << std::hex << address << std::dec;
        }
        if (!ready_) return out.str();
        alignas(SYMBOL_INFOW) unsigned char buffer[sizeof(SYMBOL_INFOW) + 256 * sizeof(wchar_t)]{};
        auto* symbol = reinterpret_cast<SYMBOL_INFOW*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        if (SymFromAddrW(process_, address, &displacement, symbol)) {
            out << ' ' << narrow(symbol->Name) << "+0x" << std::hex << displacement << std::dec;
            IMAGEHLP_LINEW64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisplacement = 0;
            if (SymGetLineFromAddrW64(process_, address, &lineDisplacement, &line) && line.FileName) {
                out << ' ' << narrow(std::filesystem::path(line.FileName).filename().wstring()) << ':'
                    << line.LineNumber;
            }
        }
        return out.str();
    }

private:
    HANDLE process_{};
    bool ready_{};
};

}  // namespace

std::string describeAllThreadStacks(DWORD watchedThread) {
    const DWORD self = GetCurrentThreadId();
    std::vector<DWORD> ids = processThreads();
    ids.erase(std::remove(ids.begin(), ids.end(), self), ids.end());
    // The watched thread first: it is the one the report is about.
    std::stable_partition(ids.begin(), ids.end(), [&](DWORD id) { return id == watchedThread; });

    // Allocated before any thread is suspended.
    std::vector<ThreadStack> stacks(ids.size());
    std::size_t captured = 0;
    for (const DWORD id : ids) {
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
        if (!thread) continue;
        auto& stack = stacks[captured];
        stack.id = id;
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread, &context)) {
                stack.count = unwindStack(context, stack.frames, kMaximumFrames);
            }
            ResumeThread(thread);
            ++captured;
        }
        CloseHandle(thread);
    }
    stacks.resize(captured);

    Symbolizer symbols;
    std::ostringstream out;
    for (std::size_t index = 0; index < stacks.size(); ++index) {
        const auto& stack = stacks[index];
        out << "\nthread " << stack.id;
        if (stack.id == watchedThread) out << " (window thread)";
        if (const auto name = threadName(stack.id); !name.empty()) out << " \"" << name << '"';
        const auto same = std::find_if(stacks.begin(), stacks.begin() + static_cast<std::ptrdiff_t>(index),
                                       [&](const ThreadStack& earlier) {
            return earlier.count == stack.count && earlier.count > 0 &&
                   std::equal(earlier.frames, earlier.frames + earlier.count, stack.frames);
        });
        if (same != stacks.begin() + static_cast<std::ptrdiff_t>(index)) {
            out << ": same stack as thread " << same->id;
            continue;
        }
        if (stack.count == 0) out << ": stack not readable";
        for (int frame = 0; frame < stack.count; ++frame) {
            out << "\n  " << symbols.describe(stack.frames[frame]);
        }
    }
    return out.str();
}

void HangWatchdog::start(DWORD watchedThread, Sink sink, StallTracker::Timing timing,
                         std::chrono::milliseconds pollInterval) {
    stop();
    if (watchedThread == 0) watchedThread = GetCurrentThreadId();
    {
        std::scoped_lock lock(mutex_);
        stopping_ = false;
    }
    beat();
    worker_ = std::thread(&HangWatchdog::run, this, watchedThread, std::move(sink), StallTracker(timing),
                          pollInterval);
}

void HangWatchdog::stop() {
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void HangWatchdog::run(DWORD watchedThread, Sink sink, StallTracker tracker,
                       std::chrono::milliseconds pollInterval) {
    SetThreadDescription(GetCurrentThread(), L"QuadDeck hang watchdog");
    std::unique_lock lock(mutex_);
    while (!wake_.wait_for(lock, pollInterval, [&] { return stopping_; })) {
        lock.unlock();
        const auto action = tracker.poll(nowMs(), lastBeat_.load(std::memory_order_relaxed));
        if (action.recovered && sink) {
            std::ostringstream out;
            out << "Window thread ran again after " << std::fixed << std::setprecision(1)
                << static_cast<double>(action.recoveredAfterMs) / 1000.0 << " s";
            sink(out.str());
        }
        if (action.capture && sink) {
            std::ostringstream out;
            out << "HANG: the window thread has not run for " << std::fixed << std::setprecision(1)
                << static_cast<double>(action.stalledMs) / 1000.0 << " s; stacks of all threads, look "
                << action.captureNumber << " of 2:" << describeAllThreadStacks(watchedThread);
            sink(out.str());
        }
        lock.lock();
    }
}

}  // namespace quaddeck
