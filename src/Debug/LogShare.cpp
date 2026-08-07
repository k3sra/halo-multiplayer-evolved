// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Debug/LogShare.cpp
#define MPE_LOG_CATEGORY "Debug.LogShare"

#include "Debug/LogShare.h"

#include "Core/Log.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace mpe::debugshare {
namespace {

/// Closes a WinHTTP handle on the way out of a scope.
class Handle {
public:
    explicit Handle(HINTERNET handle) noexcept : handle_(handle) {}
    ~Handle() {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
        }
    }
    Handle(const Handle&)            = delete;
    Handle& operator=(const Handle&) = delete;

    [[nodiscard]] HINTERNET get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    HINTERNET handle_;
};

std::string             g_endpoint;
std::string             g_label;
std::filesystem::path   g_log_path;
std::thread             g_worker;
std::mutex              g_mutex;
std::condition_variable g_wake;
std::string             g_pending_reason;
bool                    g_has_pending = false;
std::atomic<bool>       g_running{false};

/// The most recent tail of the log, capped.
///
/// Capped because a trace level log of a long session runs to megabytes and the useful part
/// is always the end. A cap also means one badly behaved session cannot post something
/// enormous at somebody's connection.
[[nodiscard]] std::string ReadLogTail(const std::filesystem::path& path,
                                      std::size_t                  max_bytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    const auto        size  = static_cast<std::uintmax_t>(file.tellg());
    const std::size_t wanted = size > max_bytes ? max_bytes : static_cast<std::size_t>(size);
    file.seekg(static_cast<std::streamoff>(size - wanted), std::ios::beg);

    std::string text(wanted, '\0');
    file.read(text.data(), static_cast<std::streamsize>(wanted));
    text.resize(static_cast<std::size_t>(file.gcount()));
    return text;
}

[[nodiscard]] std::wstring Widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(needed), wchar_t{});
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                          needed);
    return wide;
}

/// Posts the body to the configured endpoint. Failures are logged and dropped.
///
/// Dropped deliberately. This is a diagnostic convenience, and a collector that is down
/// must not become a second problem on top of whatever is already being investigated.
void Post(const std::string& body) {
    const std::wstring url = Widen(g_endpoint);
    if (url.empty()) {
        return;
    }

    URL_COMPONENTS parts{};
    parts.dwStructSize     = sizeof(parts);
    wchar_t host[256]      = {};
    wchar_t path[1024]     = {};
    parts.lpszHostName     = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath      = path;
    parts.dwUrlPathLength  = static_cast<DWORD>(std::size(path));
    if (WinHttpCrackUrl(url.c_str(), 0, 0, &parts) == FALSE) {
        MPE_LOG_WARN("the report address could not be parsed");
        return;
    }
    if (parts.nScheme != INTERNET_SCHEME_HTTPS) {
        MPE_LOG_WARN("the report address is not https; refusing to send a log in the clear");
        return;
    }

    const Handle session(WinHttpOpen(L"MultiplayerEvolved", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        return;
    }
    DWORD timeout = 15000;
    (void)WinHttpSetOption(session.get(), WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout,
                           sizeof(timeout));
    (void)WinHttpSetOption(session.get(), WINHTTP_OPTION_SEND_TIMEOUT, &timeout,
                           sizeof(timeout));
    (void)WinHttpSetOption(session.get(), WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout,
                           sizeof(timeout));

    const Handle connection(WinHttpConnect(session.get(), host, parts.nPort, 0));
    if (!connection) {
        return;
    }
    const Handle request(WinHttpOpenRequest(connection.get(), L"POST", path, nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE));
    if (!request) {
        return;
    }

    static constexpr wchar_t kHeaders[] = L"Content-Type: text/plain; charset=utf-8\r\n";
    if (WinHttpSendRequest(request.get(), kHeaders, static_cast<DWORD>(-1),
                           const_cast<char*>(body.data()),
                           static_cast<DWORD>(body.size()),
                           static_cast<DWORD>(body.size()), 0) == FALSE ||
        WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
        MPE_LOG_WARN("sending the log failed with Windows error {}", ::GetLastError());
        return;
    }

    DWORD status = 0;
    DWORD length = sizeof(status);
    (void)WinHttpQueryHeaders(request.get(),
                              WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr,
                              &status, &length, WINHTTP_NO_HEADER_INDEX);
    MPE_LOG_INFO("log shared ({} bytes), collector answered {}", body.size(), status);
}

void Worker() {
    // The log is cumulative, so a burst of reasons is one upload carrying all of them.
    // Waiting a few seconds after the first also means a failure and whatever it causes
    // immediately afterwards travel together.
    constexpr auto kCoalesce = std::chrono::seconds(5);

    while (g_running.load(std::memory_order_acquire)) {
        std::string reason;
        {
            std::unique_lock lock(g_mutex);
            g_wake.wait(lock, [] {
                return g_has_pending || !g_running.load(std::memory_order_acquire);
            });
            if (!g_running.load(std::memory_order_acquire)) {
                return;
            }
            lock.unlock();
            std::this_thread::sleep_for(kCoalesce);
            lock.lock();
            reason        = g_pending_reason;
            g_pending_reason.clear();
            g_has_pending = false;
        }

        const std::string tail = ReadLogTail(g_log_path, 512u * 1024u);
        if (tail.empty()) {
            continue;
        }
        std::ostringstream body;
        body << "==== MultiplayerEvolved report ====\n"
             << "from   : " << g_label << '\n'
             << "reason : " << reason << '\n'
             << "bytes  : " << tail.size() << "\n\n"
             << tail;
        Post(body.str());
    }
}

} // namespace

std::string ConfiguredEndpoint(const std::filesystem::path& data_directory) {
    std::ifstream file(data_directory / "report.url");
    if (!file) {
        return {};
    }
    std::string url;
    std::getline(file, url);
    while (!url.empty() && (url.back() == '\r' || url.back() == ' ' || url.back() == '\t')) {
        url.pop_back();
    }
    // https only. A log crossing the network in the clear is not something to arrange by
    // accident, and the check is here as well as at the send so a misconfigured install
    // reports it once at startup rather than silently every time.
    if (url.rfind("https://", 0) != 0) {
        return {};
    }
    return url;
}

bool SharingEnabled() {
    return g_running.load(std::memory_order_acquire);
}

void Start(const std::filesystem::path& data_directory, std::string label) {
    g_endpoint = ConfiguredEndpoint(data_directory);
    if (g_endpoint.empty()) {
        return;
    }
    g_log_path = data_directory / "MultiplayerEvolved.log";
    g_label    = std::move(label);
    g_running.store(true, std::memory_order_release);
    g_worker = std::thread(&Worker);

    MPE_LOG_INFO("log sharing is on for this install; reports go to the address in "
                "MultiplayerEvolved/report.url");
    Queue("startup");
}

void Queue(std::string reason) {
    if (!g_running.load(std::memory_order_acquire)) {
        return;
    }
    {
        std::lock_guard lock(g_mutex);
        if (!g_pending_reason.empty()) {
            g_pending_reason += "; ";
        }
        g_pending_reason += reason;
        g_has_pending = true;
    }
    g_wake.notify_one();
}

void Stop() {
    if (!g_running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    g_wake.notify_all();
    if (g_worker.joinable()) {
        g_worker.join();
    }
}

} // namespace mpe::debugshare
