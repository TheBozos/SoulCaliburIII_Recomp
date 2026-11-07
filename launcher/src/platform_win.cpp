// The launcher's Windows layer (platform.h). The only file that includes <windows.h>.

#include "platform.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winhttp.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>

namespace plat
{
    std::wstring widen(const std::string &utf8)
    {
        if (utf8.empty())
            return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring out(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
        return out;
    }

    std::string narrow(const std::wstring &wide)
    {
        if (wide.empty())
            return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        std::string out(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
        return out;
    }

    std::filesystem::path path(const std::string &utf8) { return std::filesystem::path(widen(utf8)); }
    std::string str(const std::filesystem::path &p) { return narrow(p.wstring()); }

    std::string exeDirectory()
    {
        std::wstring buffer(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (n == 0)
                return ".";
            if (n < buffer.size())
            {
                buffer.resize(n);
                break;
            }
            buffer.resize(buffer.size() * 2);
        }
        return narrow(std::filesystem::path(buffer).parent_path().wstring());
    }

    std::vector<std::string> arguments()
    {
        std::vector<std::string> out;
        int n = 0;
        wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &n);
        for (int i = 1; argv && i < n; ++i)
            out.push_back(narrow(argv[i]));
        if (argv)
            LocalFree(argv);
        return out;
    }

    std::string getEnv(const char *name)
    {
        const std::wstring wname = widen(name);
        const DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
        if (n == 0)
            return {};
        std::wstring value(n, L'\0');
        const DWORD got = GetEnvironmentVariableW(wname.c_str(), value.data(), n);
        value.resize(got);
        return narrow(value);
    }

    void setEnv(const char *name, const std::string &value)
    {
        SetEnvironmentVariableW(widen(name).c_str(), value.empty() ? nullptr : widen(value).c_str());
    }

    std::string userDirectory()
    {
        std::filesystem::path dir;
        if (const std::string forced = getEnv("PS2X_USER_DIR"); !forced.empty())
            dir = path(forced);
        else if (const std::string appData = getEnv("APPDATA"); !appData.empty())
            dir = path(appData) / L"SoulCaliburIII_Recomp";
        else
            dir = path(exeDirectory()) / L"user";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return str(std::filesystem::absolute(dir, ec).lexically_normal());
    }

    std::string systemLanguage()
    {
        return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_FRENCH ? "fr" : "en";
    }

    std::string pickDiscImage(void *ownerWindow, const std::string &title, const std::string &filterName,
                              const std::string &allFilesName)
    {
        std::wstring filter = widen(filterName) + L" (*.iso, *.bin, *.cue)";
        filter.push_back(L'\0');
        filter += L"*.iso;*.bin;*.cue";
        filter.push_back(L'\0');
        filter += widen(allFilesName) + L" (*.*)";
        filter.push_back(L'\0');
        filter += L"*.*";
        filter.push_back(L'\0');
        std::wstring file(32768, L'\0');
        const std::wstring wtitle = widen(title);
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = static_cast<HWND>(ownerWindow);
        ofn.lpstrFilter = filter.c_str();
        ofn.lpstrFile = file.data();
        ofn.nMaxFile = static_cast<DWORD>(file.size());
        ofn.lpstrTitle = wtitle.c_str();
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_EXPLORER;
        if (!GetOpenFileNameW(&ofn))
            return {};
        file.resize(wcslen(file.c_str()));
        return narrow(file);
    }

    void open(const std::string &target)
    {
        ShellExecuteW(nullptr, L"open", widen(target).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void showInFolder(const std::string &file)
    {
        const std::wstring args = L"/select,\"" + widen(file) + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }

    const void *resource(const char *name, size_t &size)
    {
        size = 0;
        HRSRC res = FindResourceA(nullptr, name, MAKEINTRESOURCEA(10) /* RT_RCDATA */);
        if (!res)
            return nullptr;
        HGLOBAL data = LoadResource(nullptr, res);
        if (!data)
            return nullptr;
        size = SizeofResource(nullptr, res);
        return LockResource(data);
    }

    double now()
    {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    std::vector<std::pair<std::string, uint64_t>> listFiles(const std::string &folder, const std::string &suffix)
    {
        std::vector<std::pair<std::string, uint64_t>> out;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((widen(folder) + L"\\*" + widen(suffix)).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
            return out;
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            const uint64_t t = (uint64_t(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
            out.emplace_back(folder + "\\" + narrow(fd.cFileName), t);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return out;
    }

    uint64_t freeBytes(const std::string &folder)
    {
        ULARGE_INTEGER avail{};
        if (!GetDiskFreeSpaceExW(widen(folder).c_str(), &avail, nullptr, nullptr))
            return 0;
        return avail.QuadPart;
    }

    // ---- Process ----

    namespace
    {
        // One argument as CommandLineToArgvW / the MSVC and MinGW CRTs parse it back.
        std::wstring quoteArg(const std::wstring &arg)
        {
            if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
                return arg;
            std::wstring out = L"\"";
            for (size_t i = 0;; ++i)
            {
                size_t backslashes = 0;
                while (i < arg.size() && arg[i] == L'\\')
                {
                    ++i;
                    ++backslashes;
                }
                if (i == arg.size())
                {
                    out.append(backslashes * 2, L'\\');
                    break;
                }
                if (arg[i] == L'"')
                {
                    out.append(backslashes * 2 + 1, L'\\');
                    out.push_back(L'"');
                }
                else
                {
                    out.append(backslashes, L'\\');
                    out.push_back(arg[i]);
                }
            }
            out.push_back(L'"');
            return out;
        }

        std::string lastError()
        {
            const DWORD code = GetLastError();
            wchar_t *text = nullptr;
            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           nullptr, code, 0, reinterpret_cast<wchar_t *>(&text), 0, nullptr);
            std::string out = text ? narrow(text) : "error " + std::to_string(code);
            if (text)
                LocalFree(text);
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == '.'))
                out.pop_back();
            return out;
        }
    }

    struct Process::Impl
    {
        HANDLE process = nullptr;
        HANDLE job = nullptr;
        HANDLE readPipe = nullptr;
        DWORD pid = 0;
        HANDLE reader = nullptr; // a Win32 thread: CancelSynchronousIo needs its handle
        std::mutex mutex;
        std::deque<std::string> lines;
        std::atomic<bool> readerDone{true};

        ~Impl()
        {
            if (job)
                CloseHandle(job); // KILL_ON_JOB_CLOSE: whatever is still running in it ends, and the pipe closes
            if (reader)
            {
                for (int i = 0; i < 200 && !readerDone; ++i)
                    Sleep(10);
                if (!readerDone)
                    CancelSynchronousIo(reader);
                WaitForSingleObject(reader, INFINITE);
                CloseHandle(reader);
            }
            if (readPipe)
                CloseHandle(readPipe);
            if (process)
                CloseHandle(process);
        }

        void readLoop()
        {
            std::string pending;
            char buffer[4096];
            DWORD got = 0;
            while (ReadFile(readPipe, buffer, sizeof(buffer), &got, nullptr) && got > 0)
            {
                pending.append(buffer, got);
                size_t at;
                while ((at = pending.find('\n')) != std::string::npos)
                {
                    std::string line = pending.substr(0, at);
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    pending.erase(0, at + 1);
                    std::lock_guard<std::mutex> lock(mutex);
                    lines.push_back(std::move(line));
                }
            }
            if (!pending.empty())
            {
                std::lock_guard<std::mutex> lock(mutex);
                lines.push_back(pending);
            }
            readerDone = true;
        }
    };

    Process::Process() : impl(std::make_unique<Impl>()) {}
    Process::~Process() = default;

    bool Process::start(const std::string &exe, const std::vector<std::string> &args, const std::string &cwd,
                        bool captureOutput, std::string &error)
    {
        impl = std::make_unique<Impl>();
        std::wstring cmd = quoteArg(widen(exe));
        for (const std::string &a : args)
            cmd += L" " + quoteArg(widen(a));

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        HANDLE writePipe = nullptr, nul = nullptr;
        DWORD flags = CREATE_UNICODE_ENVIRONMENT;
        if (captureOutput)
        {
            SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
            if (!CreatePipe(&impl->readPipe, &writePipe, &sa, 0))
            {
                error = lastError();
                return false;
            }
            SetHandleInformation(impl->readPipe, HANDLE_FLAG_INHERIT, 0);
            nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdInput = nul;
            si.hStdOutput = writePipe;
            si.hStdError = writePipe;
            // A hidden console for the build's console programs (Python, CMake, Ninja, GCC): none flashes.
            flags |= CREATE_NO_WINDOW | CREATE_SUSPENDED;
            impl->job = CreateJobObjectW(nullptr, nullptr);
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(impl->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        }
        PROCESS_INFORMATION pi{};
        const std::wstring wcwd = widen(cwd);
        const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, captureOutput ? TRUE : FALSE, flags,
                                       nullptr, wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
        if (!ok)
            error = lastError();
        if (writePipe)
            CloseHandle(writePipe);
        if (nul)
            CloseHandle(nul);
        if (!ok)
            return false;
        impl->process = pi.hProcess;
        impl->pid = pi.dwProcessId;
        if (captureOutput)
        {
            AssignProcessToJobObject(impl->job, pi.hProcess);
            ResumeThread(pi.hThread);
            impl->readerDone = false;
            impl->reader = CreateThread(
                nullptr, 0, [](LPVOID self) -> DWORD
                { static_cast<Impl *>(self)->readLoop(); return 0; },
                impl.get(), 0, nullptr);
        }
        CloseHandle(pi.hThread);
        return true;
    }

    bool Process::running()
    {
        return impl->process && WaitForSingleObject(impl->process, 0) == WAIT_TIMEOUT;
    }

    long Process::exitCode()
    {
        DWORD code = 0;
        if (!impl->process || !GetExitCodeProcess(impl->process, &code) || code == STILL_ACTIVE)
            return -1;
        return static_cast<long>(code);
    }

    uint32_t Process::pid() const { return impl->pid; }

    std::vector<std::string> Process::takeLines()
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        std::vector<std::string> out(impl->lines.begin(), impl->lines.end());
        impl->lines.clear();
        return out;
    }

    void Process::kill()
    {
        if (impl->job)
            TerminateJobObject(impl->job, 1);
        else if (impl->process)
            TerminateProcess(impl->process, 1);
    }

    void Process::drain(int timeoutMs)
    {
        const double end = now() + timeoutMs / 1000.0;
        while (!impl->readerDone && now() < end)
            Sleep(10);
    }

    void Process::requestClose()
    {
        struct Ctx
        {
            DWORD pid;
        } ctx{impl->pid};
        EnumWindows(
            [](HWND hwnd, LPARAM lp) -> BOOL
            {
                DWORD owner = 0;
                GetWindowThreadProcessId(hwnd, &owner);
                if (owner == reinterpret_cast<Ctx *>(lp)->pid && IsWindowVisible(hwnd))
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&ctx));
    }

    namespace
    {
        struct Internet
        {
            HINTERNET h = nullptr;
            ~Internet()
            {
                if (h)
                    WinHttpCloseHandle(h);
            }
        };

        // A GET whose body goes to sink (false from it stops the read): the shared part of httpGet and httpDownload.
        // receiveMs: the longest wait for data (a large download through a slow line needs more than a version check).
        bool httpRequest(const std::string &url, const std::string &userAgent, const wchar_t *headers, int receiveMs,
                         long &status, uint64_t &contentLength, std::string &error,
                         const std::function<bool(const char *, size_t)> &sink)
        {
            status = 0;
            contentLength = 0;
            std::wstring wurl = widen(url);
            URL_COMPONENTS parts{};
            parts.dwStructSize = sizeof(parts);
            wchar_t host[256] = {}, pathBuf[2048] = {};
            parts.lpszHostName = host;
            parts.dwHostNameLength = 256;
            parts.lpszUrlPath = pathBuf;
            parts.dwUrlPathLength = 2048;
            if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts) ||
                (parts.nScheme != INTERNET_SCHEME_HTTPS && parts.nScheme != INTERNET_SCHEME_HTTP))
            {
                error = "bad URL";
                return false;
            }
            Internet session, connection, request;
            // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY (Windows 8.1+): the proxy Windows itself uses.
            session.h = WinHttpOpen(widen(userAgent).c_str(), 4 /* WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY */,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (!session.h)
                session.h = WinHttpOpen(widen(userAgent).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                        WINHTTP_NO_PROXY_BYPASS, 0);
            if (!session.h)
            {
                error = lastError();
                return false;
            }
            WinHttpSetTimeouts(session.h, 5000, 5000, 5000, receiveMs);
            connection.h = WinHttpConnect(session.h, host, parts.nPort, 0);
            if (!connection.h)
            {
                error = lastError();
                return false;
            }
            request.h = WinHttpOpenRequest(connection.h, L"GET", pathBuf, nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
            if (!request.h)
            {
                error = lastError();
                return false;
            }
            // Redirects are followed (GitHub sends a release's files from its storage), never from HTTPS to HTTP.
            if (!WinHttpSendRequest(request.h, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
                !WinHttpReceiveResponse(request.h, nullptr))
            {
                error = lastError();
                return false;
            }
            DWORD code = 0, size = sizeof(code);
            WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &code, &size, WINHTTP_NO_HEADER_INDEX);
            status = static_cast<long>(code);
            wchar_t length[32] = {};
            DWORD lengthSize = sizeof(length);
            if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length, &lengthSize,
                                    WINHTTP_NO_HEADER_INDEX))
                contentLength = std::wcstoull(length, nullptr, 10);
            std::string chunk;
            for (;;)
            {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request.h, &available))
                {
                    error = lastError();
                    return false;
                }
                if (available == 0)
                    break;
                chunk.resize(available);
                DWORD read = 0;
                if (!WinHttpReadData(request.h, chunk.data(), available, &read))
                {
                    error = lastError();
                    return false;
                }
                if (!sink(chunk.data(), read))
                    return false;
            }
            return true;
        }
    }

    bool httpGet(const std::string &url, const std::string &userAgent, std::string &body, long &status,
                 std::string &error, size_t maxBytes)
    {
        body.clear();
        uint64_t length = 0;
        return httpRequest(url, userAgent, L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n", 8000,
                           status, length, error, [&](const char *data, size_t n)
                           {
                               body.append(data, n);
                               if (body.size() <= maxBytes)
                                   return true;
                               error = "response too large";
                               return false; });
    }

    bool httpDownload(const std::string &url, const std::string &userAgent, const std::string &file, std::atomic<uint64_t> &done,
                      std::atomic<uint64_t> &total, const std::atomic<bool> &cancel, std::string &error, uint64_t maxBytes)
    {
        done = 0;
        total = 0;
        HANDLE out = CreateFileW(widen(file).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out == INVALID_HANDLE_VALUE)
        {
            error = lastError();
            return false;
        }
        long status = 0;
        uint64_t length = 0;
        bool sized = false;
        bool ok = httpRequest(url, userAgent, L"Accept: application/octet-stream\r\n", 30000, status, length, error,
                              [&](const char *data, size_t n)
                              {
                                  if (!sized)
                                  {
                                      sized = true;
                                      total = length;
                                  }
                                  if (status != 200)
                                      return false; // an error page: not written
                                  if (cancel.load())
                                  {
                                      error = "stopped";
                                      return false;
                                  }
                                  DWORD written = 0;
                                  if (!WriteFile(out, data, static_cast<DWORD>(n), &written, nullptr) || written != n)
                                  {
                                      error = lastError();
                                      return false;
                                  }
                                  done += n;
                                  if (done.load() <= maxBytes)
                                      return true;
                                  error = "download too large";
                                  return false; });
        CloseHandle(out);
        if (ok && status != 200)
        {
            ok = false;
            error = "HTTP " + std::to_string(status);
        }
        else if (!ok && error.empty())
            error = status != 200 ? "HTTP " + std::to_string(status) : "download failed";
        if (!ok)
            DeleteFileW(widen(file).c_str());
        return ok;
    }

    std::string exePath()
    {
        std::wstring buffer(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (n == 0)
                return {};
            if (n < buffer.size())
            {
                buffer.resize(n);
                return narrow(buffer);
            }
            buffer.resize(buffer.size() * 2);
        }
    }

    void messageBox(const std::string &title, const std::string &text)
    {
        MessageBoxW(nullptr, widen(text).c_str(), widen(title).c_str(), MB_OK | MB_ICONERROR);
    }
}
