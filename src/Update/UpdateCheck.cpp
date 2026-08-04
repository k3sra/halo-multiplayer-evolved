// SPDX-License-Identifier: MIT
// ForgeEvolved: Update/UpdateCheck.cpp
#define FE_LOG_CATEGORY "Update"

#include "Update/UpdateCheck.h"

#include "Core/Json.h"
#include "Core/Log.h"

#include <charconv>
#include <vector>

#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace fe::update {
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

    const Handle session(WinHttpOpen(L"ForgeEvolved", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
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
    const wchar_t* headers = L"User-Agent: ForgeEvolved\r\n"
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

    // The first asset that is a DLL. A release also carries source archives, which are not
    // what a player needs to install.
    if (document.contains("assets") && document.at("assets").is_array()) {
        const json::Value& assets = document.at("assets");
        for (std::size_t index = 0; index < assets.size(); ++index) {
            const json::Value& asset = assets[index];
            if (!asset.is_object() || !asset.contains("name")) {
                continue;
            }
            const std::string name = asset.at("name").get<std::string>();
            if (name.size() < 4 || name.compare(name.size() - 4, 4, ".dll") != 0) {
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

    FE_LOG_INFO("newest release is {} ({})", release.version,
                release.asset_name.empty() ? "no downloadable asset" : release.asset_name);
    return release;
}

} // namespace fe::update
