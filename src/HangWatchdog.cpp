#include "HangWatchdog.hpp"

#include <dbghelp.h>
#include <psapi.h>
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

// A loaded image and its unwind table, listed before any thread is
// suspended. Everything here is read through ReadProcessMemory or from the
// process's own module list without the loader lock: the stalled thread may
// be the one holding it, inside a DLL's load or unload.
struct ModuleRange {
    DWORD64 base{};
    DWORD64 end{};
    const RUNTIME_FUNCTION* functions{};
    DWORD functionCount{};
    std::wstring path;
    std::string name;
};

bool readMemory(DWORD64 address, void* buffer, std::size_t size) noexcept {
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), buffer, size, &read) &&
           read == size;
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

std::vector<ModuleRange> listModules() {
    const HANDLE process = GetCurrentProcess();
    std::vector<HMODULE> handles(512);
    DWORD needed = 0;
    for (;;) {
        const auto bytes = static_cast<DWORD>(handles.size() * sizeof(HMODULE));
        if (!K32EnumProcessModules(process, handles.data(), bytes, &needed)) return {};
        if (needed <= bytes) break;
        handles.resize(needed / sizeof(HMODULE) + 32);
    }
    handles.resize(needed / sizeof(HMODULE));
    std::vector<ModuleRange> modules;
    modules.reserve(handles.size());
    for (const HMODULE handle : handles) {
        MODULEINFO information{};
        if (!K32GetModuleInformation(process, handle, &information, sizeof(information))) continue;
        ModuleRange module;
        module.base = reinterpret_cast<DWORD64>(information.lpBaseOfDll);
        module.end = module.base + information.SizeOfImage;
        std::wstring path(MAX_PATH, L'\0');
        const DWORD length = K32GetModuleFileNameExW(process, handle, path.data(), static_cast<DWORD>(path.size()));
        path.resize(length);
        module.name = narrow(std::filesystem::path(path).stem().wstring());
        module.path = std::move(path);
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 headers{};
        if (readMemory(module.base, &dos, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE &&
            readMemory(module.base + static_cast<DWORD64>(dos.e_lfanew), &headers, sizeof(headers)) &&
            headers.Signature == IMAGE_NT_SIGNATURE &&
            headers.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
            headers.OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXCEPTION) {
            const auto& table = headers.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
            if (table.VirtualAddress != 0 && table.VirtualAddress + table.Size <= information.SizeOfImage) {
                module.functions = reinterpret_cast<const RUNTIME_FUNCTION*>(module.base + table.VirtualAddress);
                module.functionCount = table.Size / sizeof(RUNTIME_FUNCTION);
            }
        }
        modules.push_back(std::move(module));
    }
    std::sort(modules.begin(), modules.end(),
              [](const ModuleRange& a, const ModuleRange& b) { return a.base < b.base; });
    return modules;
}

const ModuleRange* moduleAt(const ModuleRange* modules, std::size_t count, DWORD64 address) noexcept {
    std::size_t low = 0;
    std::size_t high = count;
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        if (modules[middle].base <= address) low = middle + 1; else high = middle;
    }
    if (low == 0) return nullptr;
    const ModuleRange* module = modules + (low - 1);
    return address < module->end ? module : nullptr;
}

// What RtlLookupFunctionEntry answers, by a binary search of the image's own
// table: that function takes ntdll's function-table locks, which a thread
// loading or unloading a DLL holds exclusively. Called with a thread
// suspended, it could wait for good on the thread it suspended.
const RUNTIME_FUNCTION* functionAt(const ModuleRange& module, DWORD64 address) noexcept {
    if (!module.functions) return nullptr;
    const auto rva = static_cast<DWORD>(address - module.base);
    std::size_t low = 0;
    std::size_t high = module.functionCount;
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        const RUNTIME_FUNCTION& entry = module.functions[middle];
        if (rva < entry.BeginAddress) {
            high = middle;
        } else if (rva >= entry.EndAddress) {
            low = middle + 1;
        } else {
            // An odd UnwindData points at the entry that holds the unwind
            // information, as ntdll's own lookup follows it.
            if (entry.UnwindData & 1U) {
                return reinterpret_cast<const RUNTIME_FUNCTION*>(module.base + (entry.UnwindData & ~1U));
            }
            return &entry;
        }
    }
    return nullptr;
}

// Runs while the thread is suspended, so it touches neither the heap nor a
// lock: the suspended thread may be holding either. It reads only the
// committed stack region the thread's own stack pointer lies in, and stops at
// code outside every listed image (generated code has no table to trust). A
// stack that cannot be read ends the walk instead of the process.
int unwindStack(const CONTEXT& start, const ModuleRange* modules, std::size_t moduleCount,
                DWORD64* frames, int capacity) noexcept {
#if defined(_M_X64)
    CONTEXT context = start;
    int count = 0;
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(context.Rsp), &region, sizeof(region)) != sizeof(region) ||
        region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        if (capacity > 0 && context.Rip != 0) frames[count++] = context.Rip;
        return count;
    }
    const DWORD64 stackEnd = reinterpret_cast<DWORD64>(region.BaseAddress) + region.RegionSize;
    __try {
        while (count < capacity && context.Rip != 0) {
            frames[count++] = context.Rip;
            if (context.Rsp < start.Rsp || context.Rsp + sizeof(DWORD64) > stackEnd) break;
            const ModuleRange* module = moduleAt(modules, moduleCount, context.Rip);
            if (!module) break;
            const DWORD64 previousStack = context.Rsp;
            const RUNTIME_FUNCTION* function = functionAt(*module, context.Rip);
            if (function) {
                // A frame register (RBP, usually) the unwind reads through
                // must lie in the same stack above the frame.
                const auto* information = reinterpret_cast<const BYTE*>(module->base + function->UnwindData);
                const unsigned frameRegister = information[3] & 0x0FU;
                if (frameRegister != 0) {
                    const DWORD64 frame = (&context.Rax)[frameRegister];
                    if (frame < context.Rsp || frame >= stackEnd) break;
                }
                PVOID handlerData = nullptr;
                DWORD64 establisherFrame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, module->base, context.Rip,
                                 const_cast<PRUNTIME_FUNCTION>(function), &context, &handlerData,
                                 &establisherFrame, nullptr);
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
    (void)start; (void)modules; (void)moduleCount; (void)frames; (void)capacity;
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

std::string moduleOffset(const std::vector<ModuleRange>& modules, DWORD64 address) {
    std::ostringstream out;
    if (const ModuleRange* module = moduleAt(modules.data(), modules.size(), address)) {
        out << module->name << "+0x" << std::hex << (address - module->base) << std::dec;
    } else {
        out << "0x" << std::hex << address << std::dec;
    }
    return out.str();
}

// Function names and lines come from dbghelp: QuadDeck's own from the PDB
// beside the executable, a DLL without one by its exports, which names the
// nearest exported function and so is only a hint. dbghelp is given the
// listed modules rather than told to look for them itself, which would walk
// the loader's list under its lock.
class Symbolizer {
public:
    explicit Symbolizer(const std::vector<ModuleRange>& modules) {
        process_ = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES |
                      SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
        std::wstring directory;
        for (const auto& module : modules) {
            if (module.base == reinterpret_cast<DWORD64>(GetModuleHandleW(nullptr))) {
                directory = std::filesystem::path(module.path).parent_path().wstring();
            }
        }
        ready_ = SymInitializeW(process_, directory.empty() ? nullptr : directory.c_str(), FALSE) != FALSE;
        if (!ready_) return;
        for (const auto& module : modules) {
            SymLoadModuleExW(process_, nullptr, module.path.c_str(), nullptr, module.base,
                             static_cast<DWORD>(module.end - module.base), nullptr, 0);
        }
    }
    ~Symbolizer() {
        if (ready_) SymCleanup(process_);
    }
    Symbolizer(const Symbolizer&) = delete;
    Symbolizer& operator=(const Symbolizer&) = delete;

    std::string describe(DWORD64 address) const {
        if (!ready_) return {};
        std::ostringstream out;
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

std::string describeStacks(const std::vector<ThreadStack>& stacks, DWORD watchedThread,
                           const std::vector<ModuleRange>& modules, const Symbolizer* symbols) {
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
            out << "\n  " << moduleOffset(modules, stack.frames[frame]);
            if (symbols) out << symbols->describe(stack.frames[frame]);
        }
    }
    return out.str();
}

}  // namespace

void reportAllThreadStacks(DWORD watchedThread, const std::string& heading, const std::string& namedHeading,
                           const std::function<void(const std::string&)>& sink) {
    const DWORD self = GetCurrentThreadId();
    std::vector<DWORD> ids = processThreads();
    ids.erase(std::remove(ids.begin(), ids.end(), self), ids.end());
    // The watched thread first: it is the one the report is about.
    std::stable_partition(ids.begin(), ids.end(), [&](DWORD id) { return id == watchedThread; });

    // Allocated before any thread is suspended.
    const std::vector<ModuleRange> modules = listModules();
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
                stack.count = unwindStack(context, modules.data(), modules.size(), stack.frames, kMaximumFrames);
            }
            ResumeThread(thread);
            ++captured;
        }
        CloseHandle(thread);
    }
    stacks.resize(captured);

    // Module offsets reach the log before dbghelp starts, which reads PDBs
    // and can be slow or wait on a lock the stalled thread holds; the names
    // follow in a second record.
    if (!sink) return;
    sink(heading + describeStacks(stacks, watchedThread, modules, nullptr));
    const Symbolizer symbols(modules);
    sink(namedHeading + describeStacks(stacks, watchedThread, modules, &symbols));
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
            std::ostringstream heading;
            heading << "HANG: the window thread has not run for " << std::fixed << std::setprecision(1)
                    << static_cast<double>(action.stalledMs) / 1000.0 << " s; stacks of all threads, look "
                    << action.captureNumber << " of 2 (names follow):";
            reportAllThreadStacks(watchedThread, heading.str(),
                                  "HANG names, look " + std::to_string(action.captureNumber) + " of 2:", sink);
        }
        lock.lock();
    }
}

}  // namespace quaddeck
