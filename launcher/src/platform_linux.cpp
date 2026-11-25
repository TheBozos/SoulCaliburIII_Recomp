// The launcher's Linux layer (platform.h).

#include "platform.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include <type_traits>

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C"
{
    extern const unsigned char sc3_res_exo2[], sc3_res_exo2_end[];
    extern const unsigned char sc3_res_ofl[], sc3_res_ofl_end[];
}

#define SC3_INCBIN(symbol, file)                                                                                       \
    asm(".section .rodata\n.global " #symbol "\n.balign 16\n" #symbol ":\n.incbin \"" file "\"\n.global " #symbol      \
        "_end\n" #symbol "_end:\n.byte 0\n.previous\n")
SC3_INCBIN(sc3_res_exo2, SC3_EXO2_FILE);
SC3_INCBIN(sc3_res_ofl, SC3_OFL_FILE);

namespace plat
{
    namespace
    {
        std::string readLink(const char *link)
        {
            std::string buffer(PATH_MAX, '\0');
            for (;;)
            {
                const ssize_t n = readlink(link, buffer.data(), buffer.size());
                if (n < 0)
                    return {};
                if (static_cast<size_t>(n) < buffer.size())
                {
                    buffer.resize(static_cast<size_t>(n));
                    return buffer;
                }
                buffer.resize(buffer.size() * 2);
            }
        }

        const std::string g_exePath = readLink("/proc/self/exe");

        std::string localeLanguage()
        {
            for (const char *name : {"LC_ALL", "LC_MESSAGES", "LANG"})
                if (const char *v = std::getenv(name); v && *v)
                    return std::string(v).substr(0, 2);
            return "en";
        }
    }

    std::wstring widen(const std::string &utf8)
    {
        std::wstring out;
        for (size_t i = 0; i < utf8.size();)
        {
            const unsigned char c = static_cast<unsigned char>(utf8[i]);
            const int extra = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
            if (extra < 0 || i + extra >= utf8.size() + (extra == 0 ? 1 : 0))
            {
                out.push_back(0xFFFD);
                ++i;
                continue;
            }
            char32_t code = extra == 0 ? c : c & (0x3F >> extra);
            bool valid = true;
            for (int k = 1; k <= extra; ++k)
            {
                const unsigned char d = static_cast<unsigned char>(utf8[i + k]);
                valid = valid && (d >> 6) == 2;
                code = (code << 6) | (d & 0x3F);
            }
            out.push_back(valid ? static_cast<wchar_t>(code) : 0xFFFD);
            i += valid ? extra + 1 : 1;
        }
        return out;
    }

    std::string narrow(const std::wstring &wide)
    {
        std::string out;
        for (const wchar_t w : wide)
        {
            const char32_t c = static_cast<char32_t>(w);
            if (c < 0x80)
                out.push_back(static_cast<char>(c));
            else if (c < 0x800)
                out += {static_cast<char>(0xC0 | (c >> 6)), static_cast<char>(0x80 | (c & 0x3F))};
            else if (c < 0x10000)
                out += {static_cast<char>(0xE0 | (c >> 12)), static_cast<char>(0x80 | ((c >> 6) & 0x3F)),
                        static_cast<char>(0x80 | (c & 0x3F))};
            else
                out += {static_cast<char>(0xF0 | (c >> 18)), static_cast<char>(0x80 | ((c >> 12) & 0x3F)),
                        static_cast<char>(0x80 | ((c >> 6) & 0x3F)), static_cast<char>(0x80 | (c & 0x3F))};
        }
        return out;
    }

    std::filesystem::path path(const std::string &utf8) { return std::filesystem::path(utf8); }
    std::string str(const std::filesystem::path &p) { return p.string(); }

    std::string exeDirectory()
    {
        return g_exePath.empty() ? "." : std::filesystem::path(g_exePath).parent_path().string();
    }

    std::vector<std::string> arguments()
    {
        std::vector<std::string> out;
        std::ifstream in("/proc/self/cmdline", std::ios::binary);
        std::string arg;
        bool first = true;
        while (std::getline(in, arg, '\0'))
        {
            if (!first)
                out.push_back(arg);
            first = false;
        }
        return out;
    }

    std::string getEnv(const char *name)
    {
        const char *v = std::getenv(name);
        return v ? v : "";
    }

    void setEnv(const char *name, const std::string &value)
    {
        if (value.empty())
            unsetenv(name);
        else
            ::setenv(name, value.c_str(), 1);
    }

    std::string userDirectory()
    {
        std::filesystem::path dir;
        if (const std::string forced = getEnv("PS2X_USER_DIR"); !forced.empty())
            dir = forced;
        else if (const std::string data = getEnv("XDG_DATA_HOME"); !data.empty())
            dir = std::filesystem::path(data) / "soulcalibur3_recomp";
        else if (const std::string home = getEnv("HOME"); !home.empty())
            dir = std::filesystem::path(home) / ".local" / "share" / "soulcalibur3_recomp";
        else
            dir = std::filesystem::path(exeDirectory()) / "user";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return std::filesystem::absolute(dir, ec).lexically_normal().string();
    }

    std::string systemLanguage() { return localeLanguage() == "fr" ? "fr" : "en"; }

    std::string systemGameLanguage()
    {
        const std::string l = localeLanguage();
        return l == "fr" || l == "de" || l == "es" || l == "it" ? l : "en";
    }

    std::string findProgram(const std::string &name)
    {
        if (name.find('/') != std::string::npos)
            return access(name.c_str(), X_OK) == 0 ? name : std::string();
        const std::string pathVar = getEnv("PATH").empty() ? "/usr/local/bin:/usr/bin:/bin" : getEnv("PATH");
        size_t start = 0;
        for (;;)
        {
            const size_t end = pathVar.find(':', start);
            std::string dir = pathVar.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!dir.empty())
            {
                const std::string candidate = dir + "/" + name;
                struct stat st{};
                if (stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(candidate.c_str(), X_OK) == 0)
                    return candidate;
            }
            if (end == std::string::npos)
                return {};
            start = end + 1;
        }
    }

    std::string pickDiscImage(void *ownerWindow, const std::string &title, const std::string &filterName,
                              const std::string &allFilesName)
    {
        (void)title;
        struct Result
        {
            std::atomic<bool> done{false};
            std::string file, error;
        } result;
        const std::string discs = filterName + " (*.iso, *.bin, *.cue)";
        const SDL_DialogFileFilter filters[] = {{discs.c_str(), "iso;bin;cue;ISO;BIN;CUE"}, {allFilesName.c_str(), "*"}};
        SDL_ShowOpenFileDialog(
            [](void *user, const char *const *files, int)
            {
                auto *r = static_cast<Result *>(user);
                if (!files)
                    r->error = SDL_GetError();
                else if (files[0])
                    r->file = files[0];
                r->done = true;
            },
            &result, static_cast<SDL_Window *>(ownerWindow), filters, 2, nullptr, false);
        while (!result.done)
        {
            SDL_PumpEvents();
            SDL_Delay(20);
        }
        if (result.file.empty() && !result.error.empty())
            messageBox("SC3 Launcher", "No file dialog could open (" + result.error +
                                           "). Install zenity or your desktop's xdg-desktop-portal, then try again.");
        return result.file;
    }

    namespace
    {
        void spawnDetached(const std::vector<std::string> &argv)
        {
            std::vector<char *> args;
            for (const std::string &a : argv)
                args.push_back(const_cast<char *>(a.c_str()));
            args.push_back(nullptr);
            const pid_t pid = fork();
            if (pid == 0)
            {
                if (fork() == 0)
                {
                    setsid();
                    const int null = ::open("/dev/null", O_RDWR | O_CLOEXEC);
                    if (null >= 0)
                    {
                        dup2(null, 0);
                        dup2(null, 1);
                        dup2(null, 2);
                    }
                    execvp(args[0], args.data());
                }
                _exit(0);
            }
            if (pid > 0)
                waitpid(pid, nullptr, 0);
        }
    }

    void open(const std::string &target) { spawnDetached({"xdg-open", target}); }

    void showInFolder(const std::string &file)
    {
        spawnDetached({"xdg-open", std::filesystem::path(file).parent_path().string()});
    }

    const void *resource(const char *name, size_t &size)
    {
        size = 0;
        if (std::strcmp(name, "EXO2") == 0)
        {
            size = static_cast<size_t>(sc3_res_exo2_end - sc3_res_exo2);
            return sc3_res_exo2;
        }
        if (std::strcmp(name, "OFL") == 0)
        {
            size = static_cast<size_t>(sc3_res_ofl_end - sc3_res_ofl);
            return sc3_res_ofl;
        }
        return nullptr;
    }

    double now()
    {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    std::vector<std::pair<std::string, uint64_t>> listFiles(const std::string &folder, const std::string &suffix)
    {
        std::vector<std::pair<std::string, uint64_t>> out;
        std::error_code ec;
        for (const auto &entry : std::filesystem::directory_iterator(folder, ec))
        {
            const std::string name = entry.path().filename().string();
            if (!entry.is_regular_file(ec) || name.size() < suffix.size() ||
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
                continue;
            struct stat st{};
            if (stat(entry.path().c_str(), &st) != 0)
                continue;
            const uint64_t ticks = (static_cast<uint64_t>(st.st_mtim.tv_sec) + 11644473600ull) * 10000000ull +
                                   static_cast<uint64_t>(st.st_mtim.tv_nsec) / 100u;
            out.emplace_back(folder + "/" + name, ticks);
        }
        return out;
    }

    uint64_t freeBytes(const std::string &folder)
    {
        struct statvfs st{};
        if (statvfs(folder.c_str(), &st) != 0)
            return 0;
        return static_cast<uint64_t>(st.f_bavail) * st.f_frsize;
    }

    struct Process::Impl
    {
        pid_t pid = 0;
        bool group = false, reaped = false;
        int status = 0;
        int readFd = -1;
        std::thread reader;
        std::atomic<bool> stop{false};
        std::mutex mutex;
        std::deque<std::string> lines;
        std::atomic<bool> readerDone{true};

        ~Impl()
        {
            if (group && pid > 0 && !reap())
            {
                ::kill(-pid, SIGKILL);
                waitpid(pid, &status, 0);
                reaped = true;
            }
            if (reader.joinable())
            {
                for (int i = 0; i < 200 && !readerDone; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                stop = true;
                reader.join();
            }
            if (readFd >= 0)
                close(readFd);
        }

        bool reap()
        {
            if (reaped || pid <= 0)
                return reaped;
            const pid_t got = waitpid(pid, &status, WNOHANG);
            if (got == pid || (got < 0 && errno == ECHILD))
                reaped = true;
            return reaped;
        }

        void readLoop()
        {
            std::string pending;
            char buffer[4096];
            while (!stop)
            {
                pollfd p{readFd, POLLIN, 0};
                const int ready = poll(&p, 1, 100);
                if (ready < 0 && errno == EINTR)
                    continue;
                if (ready == 0)
                    continue;
                if (ready < 0)
                    break;
                const ssize_t got = read(readFd, buffer, sizeof(buffer));
                if (got < 0 && errno == EINTR)
                    continue;
                if (got <= 0)
                    break;
                pending.append(buffer, static_cast<size_t>(got));
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
        std::vector<std::string> argv{exe};
        argv.insert(argv.end(), args.begin(), args.end());
        std::vector<char *> cargv;
        for (std::string &a : argv)
            cargv.push_back(a.data());
        cargv.push_back(nullptr);

        int out[2] = {-1, -1}, report[2] = {-1, -1};
        if ((captureOutput && pipe2(out, O_CLOEXEC) != 0) || pipe2(report, O_CLOEXEC) != 0)
        {
            error = std::strerror(errno);
            for (int fd : {out[0], out[1], report[0], report[1]})
                if (fd >= 0)
                    close(fd);
            return false;
        }
        const pid_t parent = getpid();
        const pid_t pid = fork();
        if (pid < 0)
        {
            error = std::strerror(errno);
            for (int fd : {out[0], out[1], report[0], report[1]})
                if (fd >= 0)
                    close(fd);
            return false;
        }
        if (pid == 0)
        {
            setsid();
            if (captureOutput)
            {
                prctl(PR_SET_PDEATHSIG, SIGKILL);
                if (getppid() != parent)
                    _exit(127);
            }
            sigset_t none;
            sigemptyset(&none);
            sigprocmask(SIG_SETMASK, &none, nullptr);
            const int null = ::open("/dev/null", O_RDWR | O_CLOEXEC);
            if (null >= 0)
            {
                dup2(null, 0);
                if (!captureOutput)
                {
                    dup2(null, 1);
                    dup2(null, 2);
                }
            }
            if (captureOutput)
            {
                dup2(out[1], 1);
                dup2(out[1], 2);
            }
            if (!cwd.empty() && chdir(cwd.c_str()) != 0)
            {
                const int e = errno;
                (void)!write(report[1], &e, sizeof(e));
                _exit(127);
            }
            execvp(cargv[0], cargv.data());
            const int e = errno;
            (void)!write(report[1], &e, sizeof(e));
            _exit(127);
        }
        close(report[1]);
        if (captureOutput)
            close(out[1]);
        int childErrno = 0;
        ssize_t n;
        do
            n = read(report[0], &childErrno, sizeof(childErrno));
        while (n < 0 && errno == EINTR);
        close(report[0]);
        if (n == sizeof(childErrno))
        {
            waitpid(pid, nullptr, 0);
            if (captureOutput)
                close(out[0]);
            error = exe + ": " + std::strerror(childErrno);
            return false;
        }
        impl->pid = pid;
        impl->group = captureOutput;
        if (captureOutput)
        {
            impl->readFd = out[0];
            impl->readerDone = false;
            impl->reader = std::thread([self = impl.get()] { self->readLoop(); });
        }
        return true;
    }

    bool Process::running() { return impl->pid > 0 && !impl->reap(); }

    long Process::exitCode()
    {
        if (impl->pid <= 0 || !impl->reap())
            return -1;
        if (WIFEXITED(impl->status))
            return WEXITSTATUS(impl->status);
        if (WIFSIGNALED(impl->status))
            return 128 + WTERMSIG(impl->status);
        return -1;
    }

    uint32_t Process::pid() const { return static_cast<uint32_t>(impl->pid); }

    std::vector<std::string> Process::takeLines()
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        std::vector<std::string> out(impl->lines.begin(), impl->lines.end());
        impl->lines.clear();
        return out;
    }

    void Process::kill()
    {
        if (impl->pid <= 0)
            return;
        if (impl->group)
            ::kill(-impl->pid, SIGKILL);
        else if (!impl->reap())
            ::kill(impl->pid, SIGKILL);
    }

    void Process::drain(int timeoutMs)
    {
        const double end = now() + timeoutMs / 1000.0;
        while (!impl->readerDone && now() < end)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    void Process::requestClose()
    {
        if (impl->pid > 0 && !impl->reap())
            ::kill(impl->pid, SIGTERM);
    }

    namespace
    {
        using CURL = void;
        struct curl_slist;
        using curl_off_t = long long;
        constexpr int kOptUrl = 10002, kOptUserAgent = 10018, kOptHttpHeader = 10023, kOptWriteFunction = 20011,
                      kOptWriteData = 10001, kOptFollow = 52, kOptMaxRedirs = 68, kOptConnectTimeoutMs = 156,
                      kOptLowSpeedLimit = 19, kOptLowSpeedTime = 20, kOptNoSignal = 99, kOptProtocols = 181,
                      kOptRedirProtocols = 182, kOptErrorBuffer = 10010, kOptXferInfoFunction = 20219,
                      kOptXferInfoData = 10057, kOptNoProgress = 43;
        constexpr int kInfoResponseCode = 0x200000 + 2, kInfoContentLength = 0x600000 + 15;
        constexpr long kProtoHttp = 1, kProtoHttps = 2;
        constexpr int kCurlOk = 0, kErrorSize = 256;

        struct Curl
        {
            bool loaded = false;
            int (*global_init)(long) = nullptr;
            CURL *(*easy_init)() = nullptr;
            int (*easy_setopt)(CURL *, int, ...) = nullptr;
            int (*easy_perform)(CURL *) = nullptr;
            int (*easy_getinfo)(CURL *, int, ...) = nullptr;
            void (*easy_cleanup)(CURL *) = nullptr;
            const char *(*easy_strerror)(int) = nullptr;
            curl_slist *(*slist_append)(curl_slist *, const char *) = nullptr;
            void (*slist_free_all)(curl_slist *) = nullptr;

            Curl()
            {
                void *lib = nullptr;
                for (const char *name : {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl-nss.so.4", "libcurl.so"})
                    if ((lib = dlopen(name, RTLD_NOW | RTLD_LOCAL)))
                        break;
                if (!lib)
                    return;
                auto get = [&](auto &fn, const char *name)
                { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name)); return fn != nullptr; };
                loaded = get(global_init, "curl_global_init") && get(easy_init, "curl_easy_init") &&
                         get(easy_setopt, "curl_easy_setopt") && get(easy_perform, "curl_easy_perform") &&
                         get(easy_getinfo, "curl_easy_getinfo") && get(easy_cleanup, "curl_easy_cleanup") &&
                         get(easy_strerror, "curl_easy_strerror") && get(slist_append, "curl_slist_append") &&
                         get(slist_free_all, "curl_slist_free_all");
                if (loaded)
                    global_init(3);
            }
        };

        Curl &curl()
        {
            static Curl c;
            return c;
        }

        struct Transfer
        {
            CURL *handle = nullptr;
            long status = 0;
            uint64_t length = 0;
            bool sized = false, aborted = false;
            const std::function<bool(Transfer &, const char *, size_t)> *sink = nullptr;
            const std::atomic<bool> *cancel = nullptr;

            void readHeaders()
            {
                if (sized)
                    return;
                sized = true;
                curl().easy_getinfo(handle, kInfoResponseCode, &status);
                curl_off_t n = -1;
                if (curl().easy_getinfo(handle, kInfoContentLength, &n) == kCurlOk && n > 0)
                    length = static_cast<uint64_t>(n);
            }
        };

        size_t writeCallback(char *data, size_t size, size_t count, void *user)
        {
            auto *t = static_cast<Transfer *>(user);
            t->readHeaders();
            const size_t n = size * count;
            if (!(*t->sink)(*t, data, n))
            {
                t->aborted = true;
                return 0;
            }
            return n;
        }

        int progressCallback(void *user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
        {
            auto *t = static_cast<Transfer *>(user);
            if (!t->cancel || !t->cancel->load())
                return 0;
            t->aborted = true;
            return 1;
        }

        bool httpRequest(const std::string &url, const std::string &userAgent, const std::vector<std::string> &headers,
                         int receiveMs, long &status, std::string &error,
                         const std::function<bool(Transfer &, const char *, size_t)> &sink,
                         const std::atomic<bool> *cancel = nullptr)
        {
            status = 0;
            Curl &c = curl();
            if (!c.loaded)
            {
                error = "libcurl is not installed";
                return false;
            }
            if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0)
            {
                error = "bad URL";
                return false;
            }
            Transfer t;
            t.handle = c.easy_init();
            if (!t.handle)
            {
                error = "libcurl could not start";
                return false;
            }
            t.sink = &sink;
            t.cancel = cancel;
            curl_slist *list = nullptr;
            for (const std::string &h : headers)
                list = c.slist_append(list, h.c_str());
            char errorBuffer[kErrorSize] = {};
            const long https = url.rfind("https://", 0) == 0;
            c.easy_setopt(t.handle, kOptUrl, url.c_str());
            c.easy_setopt(t.handle, kOptUserAgent, userAgent.c_str());
            c.easy_setopt(t.handle, kOptHttpHeader, list);
            c.easy_setopt(t.handle, kOptFollow, 1L);
            c.easy_setopt(t.handle, kOptMaxRedirs, 10L);
            c.easy_setopt(t.handle, kOptProtocols, kProtoHttp | kProtoHttps);
            c.easy_setopt(t.handle, kOptRedirProtocols, https ? kProtoHttps : kProtoHttp | kProtoHttps);
            c.easy_setopt(t.handle, kOptConnectTimeoutMs, 5000L);
            c.easy_setopt(t.handle, kOptLowSpeedLimit, 1L);
            c.easy_setopt(t.handle, kOptLowSpeedTime, static_cast<long>((receiveMs + 999) / 1000));
            c.easy_setopt(t.handle, kOptNoSignal, 1L);
            c.easy_setopt(t.handle, kOptErrorBuffer, errorBuffer);
            c.easy_setopt(t.handle, kOptWriteFunction, &writeCallback);
            c.easy_setopt(t.handle, kOptWriteData, &t);
            if (cancel)
            {
                c.easy_setopt(t.handle, kOptXferInfoFunction, &progressCallback);
                c.easy_setopt(t.handle, kOptXferInfoData, &t);
                c.easy_setopt(t.handle, kOptNoProgress, 0L);
            }
            const int code = c.easy_perform(t.handle);
            t.readHeaders();
            status = t.status;
            c.easy_cleanup(t.handle);
            if (list)
                c.slist_free_all(list);
            if (t.aborted)
                return false;
            if (code != kCurlOk)
            {
                error = errorBuffer[0] ? errorBuffer : c.easy_strerror(code);
                return false;
            }
            return true;
        }
    }

    bool httpGet(const std::string &url, const std::string &userAgent, std::string &body, long &status,
                 std::string &error, size_t maxBytes)
    {
        body.clear();
        return httpRequest(url, userAgent, {"Accept: application/vnd.github+json", "X-GitHub-Api-Version: 2022-11-28"},
                           8000, status, error, [&](Transfer &, const char *data, size_t n)
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
        std::FILE *out = std::fopen(file.c_str(), "wb");
        if (!out)
        {
            error = std::strerror(errno);
            return false;
        }
        long status = 0;
        bool ok = httpRequest(url, userAgent, {"Accept: application/octet-stream"}, 30000, status, error,
                              [&](Transfer &t, const char *data, size_t n)
                              {
                                  total = t.length;
                                  if (t.status != 200)
                                      return false;
                                  if (cancel.load())
                                  {
                                      error = "stopped";
                                      return false;
                                  }
                                  if (std::fwrite(data, 1, n, out) != n)
                                  {
                                      error = std::strerror(errno);
                                      return false;
                                  }
                                  done += n;
                                  if (done.load() <= maxBytes)
                                      return true;
                                  error = "download too large";
                                  return false; }, &cancel);
        if (!ok && error.empty() && cancel.load())
            error = "stopped";
        if (std::fclose(out) != 0 && ok)
        {
            ok = false;
            error = std::strerror(errno);
        }
        if (ok && status != 200)
        {
            ok = false;
            error = "HTTP " + std::to_string(status);
        }
        else if (!ok && error.empty())
            error = status != 200 ? "HTTP " + std::to_string(status) : "download failed";
        if (!ok)
            std::remove(file.c_str());
        return ok;
    }

    std::string exePath() { return g_exePath; }

    void messageBox(const std::string &title, const std::string &text)
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(), text.c_str(), nullptr);
    }
}
