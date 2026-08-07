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
#include <format>
#include <functional>
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
/// Asked for each report rather than taken once.
///
/// Sharing starts before Steam is up, so a label captured at that moment says PLAYER (0) on
/// every machine, which is exactly the one thing a label has to do: tell them apart. Asking
/// at send time means the first report after sign in carries the real identity.
std::function<std::string()> g_describe;

std::atomic<bool>       g_running{false};

/// The last label that named a real machine, kept so it never has to be asked for twice.
std::mutex  g_label_mutex;
std::string g_known_label;

/// Who this machine is, asked for at most until the answer is worth keeping.
///
/// Asking every time is what made the label useful, because sharing starts before Steam is
/// signed in and a label taken then is the same placeholder everywhere. Asking every time is
/// also what made the game crash on the way out: the supplier reads the Steam persona name
/// and id, the final report is sent during teardown, and by then the game has released
/// Steam. Reading through a released interface faulted inside steam_api64, on every exit,
/// whether the player quit from the menu or killed the window.
///
/// Caching the first real answer satisfies both. Steam is asked until it says something
/// worth keeping, and never again, so the report sent while the process is coming down
/// touches nothing that might already be gone.
[[nodiscard]] std::string Label() {
    {
        std::lock_guard lock(g_label_mutex);
        if (!g_known_label.empty()) {
            return g_known_label;
        }
    }
    if (!g_describe) {
        return "unidentified";
    }
    if (!g_running.load(std::memory_order_acquire)) {
        // Shutting down. Whatever the supplier reads may already have been released, and
        // an unnamed report is worth immeasurably more than a crash on exit.
        return "unidentified";
    }

    std::string described = g_describe();
    // "PLAYER (0)" is the shape of an answer given before Steam signed in. Anything with a
    // real id in it is worth keeping; anything else is worth asking about again.
    if (described.find("(0)") == std::string::npos && !described.empty()) {
        std::lock_guard lock(g_label_mutex);
        g_known_label = described;
    }
    return described;
}
std::filesystem::path   g_log_path;
std::thread             g_worker;
std::mutex              g_mutex;
std::condition_variable g_wake;
std::string             g_pending_reason;
bool                    g_has_pending = false;


/// How far into the log has already been sent.
std::uintmax_t g_sent_offset = 0;
/// Which report this is, so a gap in the sequence is visible rather than assumed away.
std::uint32_t g_sequence = 0;

/// Everything written since the last report.
///
/// Incremental rather than a tail, and this is what makes complete coverage affordable.
/// Sending the last N kilobytes each time re-sends most of what the previous report already
/// carried, so the choice is between sending often and sending everything. A delta is both:
/// each report carries exactly what is new, the reports read in order are the whole log with
/// nothing missing, and a long session costs no more than a short one per minute.
///
/// out_gap reports bytes skipped because a single delta was larger than the cap, so a reader
/// is told about a hole rather than shown a join that looks continuous and is not.
[[nodiscard]] std::string ReadNewLogBytes(const std::filesystem::path& path,
                                          std::size_t max_bytes, std::uintmax_t& out_gap) {
    out_gap = 0;

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    const auto size = static_cast<std::uintmax_t>(file.tellg());

    // A smaller file than last time means the log was rotated under us, which happens when
    // the game restarts. Start again from the beginning rather than reading past the end.
    if (size < g_sent_offset) {
        g_sent_offset = 0;
    }
    if (size == g_sent_offset) {
        return {};
    }

    std::uintmax_t from      = g_sent_offset;
    const std::uintmax_t available = size - from;
    if (available > max_bytes) {
        // Keep the newest. The end of a delta is where the thing being investigated is.
        out_gap = available - max_bytes;
        from    = size - max_bytes;
    }

    file.seekg(static_cast<std::streamoff>(from), std::ios::beg);
    const auto wanted = static_cast<std::size_t>(size - from);
    std::string text(wanted, '\0');
    file.read(text.data(), static_cast<std::streamsize>(wanted));
    text.resize(static_cast<std::size_t>(file.gcount()));

    g_sent_offset = size;
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

    // No proxy, not automatic proxy detection.
    //
    // AUTOMATIC_PROXY asks Windows to discover a proxy, which on a machine with no proxy to
    // discover means waiting for WPAD to give up. On one of the two machines this was tested
    // on, every single report failed with error 12002, a WinHTTP timeout, and none of that
    // machine's logs ever arrived: the half of the picture that was missing was the half
    // that had the interesting fault in it.
    //
    // Nothing here needs a proxy. A collector address is a plain https URL on the public
    // internet, and a player who genuinely is behind a proxy can lose the reports rather
    // than have the mod spend fifteen seconds a time finding that out.
    const Handle session(WinHttpOpen(L"MultiplayerEvolved", WINHTTP_ACCESS_TYPE_NO_PROXY,
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
    // Waiting a few seconds after the first reason means a failure and everything it causes
    // immediately afterwards travel in one report, which is how they want to be read.
    constexpr auto kCoalesce = std::chrono::seconds(6);

    // Nothing is sent unprompted forever, but a session that is running normally still has
    // to be recorded: the interesting question is often what a machine was doing in the
    // minute before somebody noticed anything, and nothing about that minute is a failure.
    constexpr auto kHeartbeat = std::chrono::seconds(45);

    // Each report carries what is new, so this is a cap on a burst rather than on the log.
    constexpr std::size_t kMaxDeltaBytes = 700u * 1024u;

    while (g_running.load(std::memory_order_acquire)) {
        std::string reason;
        {
            std::unique_lock lock(g_mutex);
            g_wake.wait_for(lock, kHeartbeat, [] {
                return g_has_pending || !g_running.load(std::memory_order_acquire);
            });
            if (!g_running.load(std::memory_order_acquire)) {
                return;
            }
            const bool prompted = g_has_pending;
            lock.unlock();
            if (prompted) {
                std::this_thread::sleep_for(kCoalesce);
            }
            lock.lock();
            reason = g_pending_reason.empty() ? std::string("heartbeat") : g_pending_reason;
            g_pending_reason.clear();
            g_has_pending = false;
        }

        std::uintmax_t    gap = 0;
        const std::string delta = ReadNewLogBytes(g_log_path, kMaxDeltaBytes, gap);
        if (delta.empty()) {
            continue; // Nothing has happened since the last report.
        }

        ++g_sequence;
        std::ostringstream body;
        body << "==== MultiplayerEvolved report ====\n"
             << "from     : " << Label() << '\n'
             << "sequence : " << g_sequence << '\n'
             << "reason   : " << reason << '\n'
             << "bytes    : " << delta.size() << '\n'
             << "upto     : " << g_sent_offset << '\n';
        if (gap != 0) {
            body << "MISSING  : " << gap
                 << " bytes were skipped between this report and the last\n";
        }
        body << '\n' << delta;
        Post(body.str());
    }
}

/// Every record at Warn or worse asks for a report.
///
/// Warn rather than Error, because the mod says a great deal at Warn that is the first
/// sign of a fault and not itself one: a send that failed, a widget that could not be
/// found, a refusal that was handled. Those are the lines that explain the error that
/// arrives ten seconds later, and a report triggered only by the error has already had to
/// wait for it.
void OnRecord(log::Level level, std::string_view category, std::string_view message) {
    // This sender's own complaints do not ask it to send.
    //
    // It warns when a report fails, which would queue a report, which would fail, which
    // would warn. A collector that is down would become a loop rather than a dropped
    // report, on somebody's machine, in the middle of the session it was meant to be
    // quietly recording.
    if (category == MPE_LOG_CATEGORY) {
        return;
    }

    // Truncated in the reason line, because the whole record is in the log this is about
    // to send anyway. It is here to make the report identifiable at a glance.
    std::string summary(message);
    if (summary.size() > 120) {
        summary.resize(120);
        summary += "...";
    }
    Queue(std::format("{} in {}: {}", log::ToString(level), category, summary));
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

void Start(const std::filesystem::path& data_directory, std::function<std::string()> describe) {
    g_endpoint = ConfiguredEndpoint(data_directory);
    if (g_endpoint.empty()) {
        return;
    }
    g_log_path = data_directory / "MultiplayerEvolved.log";
    g_describe = std::move(describe);
    g_running.store(true, std::memory_order_release);
    g_worker = std::thread(&Worker);

    // Everything at Warn or worse asks for a report as it happens, so a failure and the
    // lines that explain it arrive together rather than whenever a timer next comes round.
    log::SetRecordHook(&OnRecord, log::Level::Warn);

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
    // Cleared first. The worker is about to be joined, and a record written during that
    // join would otherwise queue work for a thread that is leaving.
    log::SetRecordHook(nullptr, log::Level::Error);

    g_wake.notify_all();
    if (g_worker.joinable()) {
        g_worker.join();
    }

    // One last delta, sent on this thread, carrying whatever the worker did not live to
    // send. The end of a session is the part most worth having and it is the part a
    // background sender is least likely to get out in time.
    std::uintmax_t    gap   = 0;
    const std::string delta = ReadNewLogBytes(g_log_path, 700u * 1024u, gap);
    if (!delta.empty()) {
        ++g_sequence;
        std::ostringstream body;
        body << "==== MultiplayerEvolved report ====\n"
             << "from     : " << Label() << '\n'
             << "sequence : " << g_sequence << " (final)\n"
             << "reason   : shutdown\n"
             << "bytes    : " << delta.size() << "\n\n"
             << delta;
        Post(body.str());
    }
}

} // namespace mpe::debugshare
