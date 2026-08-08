// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Update/UpdateCheck.cpp
#define MPE_LOG_CATEGORY "Update"

#include "Update/UpdateCheck.h"

#include "Core/Json.h"
#include "Core/Log.h"

#include <charconv>
#include <vector>

#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace mpe::update {
namespace {

/// A WinHTTP handle that closes itself.
///
/// Four handles are opened per request and every early return has to close the ones already
/// open. Doing that by hand is how a leak gets introduced on the one error path nobody
/// tested, so ownership is expressed once here instead.
class Handle {
public:
    Handle() = default;
    explicit Handle(HINTERNET handle) noexcept : handle_(handle) {}
    ~Handle() {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
        }
    }
    Handle(const Handle&)            = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

    [[nodiscard]] HINTERNET get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    HINTERNET handle_{nullptr};
};

/// Reads a response body to the end.
[[nodiscard]] std::string ReadBody(HINTERNET request) {
    std::string body;
    for (;;) {
        DWORD available = 0;
        if (WinHttpQueryDataAvailable(request, &available) == FALSE || available == 0) {
            break;
        }
        const std::size_t offset = body.size();
        body.resize(offset + available);
        DWORD read = 0;
        if (WinHttpReadData(request, body.data() + offset, available, &read) == FALSE) {
            body.resize(offset);
            break;
        }
        body.resize(offset + read);
    }
    return body;
}

/// Splits a dotted version into its numeric components, ignoring anything that is not a
/// number so a tag like v1.2.0-beta still compares on 1.2.0.
[[nodiscard]] std::vector<int> Components(std::string_view version) {
    std::vector<int> parts;
    std::size_t      index = 0;
    while (index < version.size()) {
        while (index < version.size() && (version[index] < '0' || version[index] > '9')) {
            if (version[index] == '-' || version[index] == '+') {
                return parts; // Pre-release suffix; the numeric prefix is what matters.
            }
            ++index;
        }
        const std::size_t start = index;
        while (index < version.size() && version[index] >= '0' && version[index] <= '9') {
            ++index;
        }
        if (index == start) {
            break;
        }
        int value = 0;
        (void)std::from_chars(version.data() + start, version.data() + index, value);
        parts.push_back(value);
    }
    return parts;
}

} // namespace

bool IsNewer(std::string_view candidate, std::string_view current) {
    const std::vector<int> left  = Components(candidate);
    const std::vector<int> right = Components(current);
    for (std::size_t index = 0; index < left.size() || index < right.size(); ++index) {
        const int a = index < left.size() ? left[index] : 0;
        const int b = index < right.size() ? right[index] : 0;
        if (a != b) {
            return a > b;
        }
    }
    return false;
}

Expected<ReleaseInfo> FetchLatestRelease(std::string_view repository) {
    if (repository.empty()) {
        return Error{ErrorCode::InvalidArgument, "no repository given"};
    }

    // No proxy, not automatic proxy detection.
    //
    // AUTOMATIC_PROXY asks Windows to discover a proxy, and on a machine with none to
    // discover that means waiting for WPAD to give up. The log sender had exactly this and
    // every request on one of the two test machines failed with error 12002, a WinHTTP
    // timeout. An updater that times out looks identical to one with nothing to fetch.
    const Handle session(WinHttpOpen(L"MultiplayerEvolved", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        return Error{ErrorCode::InvalidState, "could not open an HTTP session"};
    }

    // Short, because this runs while the game is starting and a slow network must not be
    // able to hold the mod up. Failing to check is harmless; blocking is not.
    DWORD timeout = 8000;
    (void)WinHttpSetOption(session.get(), WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout,
                           sizeof(timeout));
    (void)WinHttpSetOption(session.get(), WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout,
                           sizeof(timeout));

    const Handle connection(
        WinHttpConnect(session.get(), L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection) {
        return Error{ErrorCode::InvalidState, "could not reach api.github.com"};
    }

    std::wstring path = L"/repos/";
    for (const char character : repository) {
        path.push_back(static_cast<wchar_t>(character));
    }
    path += L"/releases/latest";

    const Handle request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
                                            WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE));
    if (!request) {
        return Error{ErrorCode::InvalidState, "could not build the request"};
    }

    // GitHub rejects requests without a user agent, and serves the stable v3 schema only
    // when it is asked for by name.
    const wchar_t* headers = L"User-Agent: MultiplayerEvolved\r\n"
                             L"Accept: application/vnd.github+json\r\n";
    if (WinHttpSendRequest(request.get(), headers, static_cast<DWORD>(-1),
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) == FALSE ||
        WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
        return Error{ErrorCode::InvalidState, "the release request failed"};
    }

    DWORD status = 0;
    DWORD size   = sizeof(status);
    (void)WinHttpQueryHeaders(request.get(),
                              WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                              WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        // 404 is the ordinary answer for a repository that has published no release yet,
        // which is a fact about the repository rather than a fault.
        return Error{ErrorCode::InvalidState,
                     std::format("GitHub answered {}", static_cast<int>(status))};
    }

    const std::string body = ReadBody(request.get());
    if (body.empty()) {
        return Error{ErrorCode::InvalidState, "the release response was empty"};
    }

    const json::Value document = json::Value::parse(body, nullptr, false);
    if (!document.is_object() || !document.contains("tag_name")) {
        return Error{ErrorCode::InvalidState, "the release response was not understood"};
    }

    ReleaseInfo release;
    release.version = document.at("tag_name").get<std::string>();
    if (!release.version.empty() && (release.version[0] == 'v' || release.version[0] == 'V')) {
        release.version.erase(0, 1);
    }

    // The mod's own DLL, by exact name.
    //
    // Matching the first asset ending in .dll was wrong: a release also ships version.dll,
    // the loader proxy, and picking that one would stage the loader as though it were the
    // mod. Whichever happened to be uploaded first would decide, which is not a thing worth
    // leaving to chance when the result gets loaded as code.
    //
    // WHY THE NAME IS A CONTRACT AND NOT A DETAIL
    //
    // Releases moved to shipping a zip, and this kept asking for MultiplayerEvolved.dll.
    // Every install went on reporting an update it could never fetch, for every release,
    // and the failure was silent by design: the check succeeded, the version compared
    // newer, and there was simply nothing to download.
    //
    // Nothing on the client can repair that, and this is the part worth remembering. A
    // machine whose updater cannot download is a machine that cannot receive the fix for
    // its updater. The rescue had to be a raw DLL uploaded beside the zip, so the broken
    // clients already out there find what they were always looking for. Every release from
    // now on publishes both, and build.bat produces both, so the two cannot drift apart
    // again.
    if (document.contains("assets") && document.at("assets").is_array()) {
        const json::Value& assets = document.at("assets");
        for (std::size_t index = 0; index < assets.size(); ++index) {
            const json::Value& asset = assets[index];
            if (!asset.is_object() || !asset.contains("name")) {
                continue;
            }
            const std::string name = asset.at("name").get<std::string>();
            if (name != "MultiplayerEvolved.dll") {
                continue;
            }
            release.asset_name = name;
            if (asset.contains("browser_download_url")) {
                release.download_url = asset.at("browser_download_url").get<std::string>();
            }
            if (asset.contains("size") && asset.at("size").is_number_integer()) {
                release.asset_bytes = asset.at("size").get<long long>();
            }
            break;
        }
    }

    if (release.asset_name.empty()) {
        // Loud, because this is the shape of a release that nobody can install. The
        // previous wording said "no downloadable asset" at Info and read like a note about
        // a release that had none on purpose.
        MPE_LOG_ERROR("release {} publishes no MultiplayerEvolved.dll, so no install can "
                     "update itself to it; the release is incomplete",
                     release.version);
        return release;
    }

    MPE_LOG_INFO("newest release is {} ({}, {} bytes)", release.version, release.asset_name,
                release.asset_bytes);
    return release;
}

Result DownloadRelease(const ReleaseInfo& release, const std::wstring& game_binaries_directory,
                       const std::function<void(long long, long long)>& progress) {
    if (release.download_url.empty()) {
        return Result::Fail(ErrorCode::InvalidArgument,
                            "the release publishes no downloadable asset");
    }

    // The URL is split by the API rather than by hand, so a host or path this code did not
    // anticipate cannot be silently mis-parsed into a request against the wrong server.
    std::wstring url;
    url.reserve(release.download_url.size());
    for (const char character : release.download_url) {
        url.push_back(static_cast<wchar_t>(character));
    }

    URL_COMPONENTS parts{};
    parts.dwStructSize     = sizeof(parts);
    wchar_t host[256]      = {};
    wchar_t path[2048]     = {};
    parts.lpszHostName     = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath      = path;
    parts.dwUrlPathLength  = static_cast<DWORD>(std::size(path));
    if (WinHttpCrackUrl(url.c_str(), 0, 0, &parts) == FALSE) {
        return Result::Fail(ErrorCode::InvalidArgument, "the download URL was not understood");
    }
    if (parts.nScheme != INTERNET_SCHEME_HTTPS) {
        // Refused rather than downgraded. This file is about to be loaded as code.
        return Result::Fail(ErrorCode::InvalidArgument, "the download URL is not HTTPS");
    }

    // No proxy, not automatic proxy detection.
    //
    // AUTOMATIC_PROXY asks Windows to discover a proxy, and on a machine with none to
    // discover that means waiting for WPAD to give up. The log sender had exactly this and
    // every request on one of the two test machines failed with error 12002, a WinHTTP
    // timeout. An updater that times out looks identical to one with nothing to fetch.
    const Handle session(WinHttpOpen(L"MultiplayerEvolved", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        return Result::Fail(ErrorCode::InvalidState, "could not open an HTTP session");
    }
    const Handle connection(WinHttpConnect(session.get(), host, parts.nPort, 0));
    if (!connection) {
        return Result::Fail(ErrorCode::InvalidState, "could not reach the download host");
    }
    const Handle request(WinHttpOpenRequest(connection.get(), L"GET", path, nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE));
    if (!request) {
        return Result::Fail(ErrorCode::InvalidState, "could not build the download request");
    }
    if (WinHttpSendRequest(request.get(), L"User-Agent: MultiplayerEvolved\r\n",
                           static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) == FALSE ||
        WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
        return Result::Fail(ErrorCode::InvalidState, "the download request failed");
    }

    DWORD status = 0;
    DWORD size   = sizeof(status);
    (void)WinHttpQueryHeaders(request.get(),
                              WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                              WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        return Result::Fail(ErrorCode::InvalidState,
                            std::format("the download answered {}", static_cast<int>(status)));
    }

    // Written under a temporary name and renamed only once the whole file has arrived, so a
    // download cut halfway through cannot leave something the loader would try to run.
    const std::wstring pending  = game_binaries_directory + L"MultiplayerEvolved.dll.pending";
    const std::wstring partial  = pending + L".part";
    const HANDLE       file     = ::CreateFileW(partial.c_str(), GENERIC_WRITE, 0, nullptr,
                                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return Result::Fail(ErrorCode::InvalidState, "could not open the download for writing");
    }

    long long received = 0;
    bool      failed   = false;
    for (;;) {
        DWORD available = 0;
        if (WinHttpQueryDataAvailable(request.get(), &available) == FALSE) {
            failed = true;
            break;
        }
        if (available == 0) {
            break;
        }
        std::vector<char> chunk(available);
        DWORD             read = 0;
        if (WinHttpReadData(request.get(), chunk.data(), available, &read) == FALSE) {
            failed = true;
            break;
        }
        DWORD written = 0;
        if (::WriteFile(file, chunk.data(), read, &written, nullptr) == FALSE ||
            written != read) {
            failed = true;
            break;
        }
        received += read;
        if (progress) {
            progress(received, release.asset_bytes);
        }
    }
    ::CloseHandle(file);

    if (failed || received == 0) {
        ::DeleteFileW(partial.c_str());
        return Result::Fail(ErrorCode::InvalidState, "the download did not complete");
    }
    if (release.asset_bytes > 0 && received != release.asset_bytes) {
        ::DeleteFileW(partial.c_str());
        return Result::Fail(
            ErrorCode::InvalidState,
            std::format("the download was {} bytes, expected {}", received,
                        release.asset_bytes));
    }

    ::DeleteFileW(pending.c_str());
    if (::MoveFileW(partial.c_str(), pending.c_str()) == FALSE) {
        ::DeleteFileW(partial.c_str());
        return Result::Fail(ErrorCode::InvalidState, "the download could not be put in place");
    }

    MPE_LOG_INFO("update {} downloaded ({} bytes); it will be applied at the next start",
                release.version, received);
    return Result::Success();
}

} // namespace mpe::update
