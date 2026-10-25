#include "Http.h"

#if defined(_WIN32)

#include <windows.h>
#include <winhttp.h>

#include <cmath>

namespace Luwow::Net {
    static std::wstring widen(const std::string& text) {
        if (text.empty()) return std::wstring();
        int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring wide(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
        return wide;
    }

    static std::string narrow(const std::wstring& text) {
        if (text.empty()) return std::string();
        int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), length, nullptr, nullptr);
        return utf8;
    }

    // WinHTTP's errors have their messages in winhttp.dll, the rest are the system's
    static std::string lastError(const char* step) {
        DWORD code = GetLastError();
        char* message = nullptr;
        DWORD length = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            GetModuleHandleA("winhttp.dll"), code, 0, reinterpret_cast<char*>(&message), 0, nullptr);
        std::string text = length ? std::string(message, length) : "error " + std::to_string(code);
        if (message) LocalFree(message);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == '.')) text.pop_back();
        return std::string(step) + ": " + text;
    }

    // Closes a WinHTTP handle when it goes out of scope
    struct InternetHandle {
        HINTERNET handle = nullptr;

        explicit InternetHandle(HINTERNET handle) : handle(handle) {}
        ~InternetHandle() { if (handle) WinHttpCloseHandle(handle); }
        InternetHandle(const InternetHandle&) = delete;
        InternetHandle& operator=(const InternetHandle&) = delete;
    };

    static std::wstring queryText(HINTERNET request, DWORD info) {
        DWORD size = 0;
        WinHttpQueryHeaders(request, info, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
        if (size == 0) return std::wstring();
        std::wstring text(size / sizeof(wchar_t), L'\0');
        if (!WinHttpQueryHeaders(request, info, WINHTTP_HEADER_NAME_BY_INDEX, text.data(), &size, WINHTTP_NO_HEADER_INDEX)) return std::wstring();
        text.resize(size / sizeof(wchar_t));
        return text;
    }

    // The raw headers start with the status line, then a "Name: value" line each
    static void parseHeaders(const std::string& raw, HttpResponse& response) {
        size_t start = raw.find("\r\n");
        while (start != std::string::npos) {
            start += 2;
            size_t end = raw.find("\r\n", start);
            std::string line = raw.substr(start, end == std::string::npos ? std::string::npos : end - start);
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                size_t valueStart = line.find_first_not_of(' ', colon + 1);
                response.headers.emplace_back(line.substr(0, colon), valueStart == std::string::npos ? "" : line.substr(valueStart));
            }
            start = end;
        }
    }

    bool sendHttpRequest(const HttpRequest& request, HttpResponse& response, std::string& error) {
        std::wstring url = widen(request.url);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwSchemeLength = static_cast<DWORD>(-1);
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
            error = "the URL isn't valid";
            return false;
        }
        if (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) {
            error = "only http and https URLs are supported";
            return false;
        }
        std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        if (path.empty()) path = L"/";

        // The automatic proxy needs Windows 8.1, older versions use the system's default
        InternetHandle session(WinHttpOpen(L"Luwow", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session.handle) session.handle = WinHttpOpen(L"Luwow", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session.handle) {
            error = lastError("could not start WinHTTP");
            return false;
        }
        if (request.timeout >= 0) {
            int milliseconds = static_cast<int>(std::lround(request.timeout * 1000));
            WinHttpSetTimeouts(session.handle, milliseconds, milliseconds, milliseconds, milliseconds);
        }
        // Gzip and deflate bodies are decompressed, on versions that can
        DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
        WinHttpSetOption(session.handle, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));

        InternetHandle connection(WinHttpConnect(session.handle, host.c_str(), parts.nPort, 0));
        if (!connection.handle) {
            error = lastError("could not connect");
            return false;
        }

        DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        std::wstring method = widen(request.method);
        InternetHandle handle(WinHttpOpenRequest(connection.handle, method.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
        if (!handle.handle) {
            error = lastError("could not open the request");
            return false;
        }

        std::wstring headers;
        for (const auto& [name, value] : request.headers) headers += widen(name + ": " + value + "\r\n");
        void* body = request.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(request.body.data());
        DWORD bodyLength = static_cast<DWORD>(request.body.size());
        if (!WinHttpSendRequest(handle.handle, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                headers.empty() ? 0 : static_cast<DWORD>(-1), body, bodyLength, bodyLength, 0)) {
            error = lastError("could not send the request");
            return false;
        }
        if (!WinHttpReceiveResponse(handle.handle, nullptr)) {
            error = lastError("no response");
            return false;
        }

        DWORD status = 0;
        DWORD size = sizeof(status);
        WinHttpQueryHeaders(handle.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
            &status, &size, WINHTTP_NO_HEADER_INDEX);
        response.status = static_cast<int>(status);
        response.statusMessage = narrow(queryText(handle.handle, WINHTTP_QUERY_STATUS_TEXT));
        parseHeaders(narrow(queryText(handle.handle, WINHTTP_QUERY_RAW_HEADERS_CRLF)), response);

        // A body being saved goes through a chunk, straight to the file
        HANDLE file = INVALID_HANDLE_VALUE;
        if (!request.saveTo.empty() && response.status >= 200 && response.status < 300) {
            file = CreateFileW(widen(request.saveTo).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                error = lastError("could not create the file to save to");
                return false;
            }
        }
        auto fail = [&](const char* step) {
            error = lastError(step);
            if (file != INVALID_HANDLE_VALUE) {
                CloseHandle(file);
                DeleteFileW(widen(request.saveTo).c_str());
            }
            return false;
        };
        std::string chunk;

        while (true) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(handle.handle, &available)) return fail("could not read the response");
            if (available == 0) break;
            std::string& into = file != INVALID_HANDLE_VALUE ? chunk : response.body;
            size_t offset = file != INVALID_HANDLE_VALUE ? 0 : into.size();
            into.resize(offset + available);
            DWORD read = 0;
            if (!WinHttpReadData(handle.handle, into.data() + offset, available, &read)) return fail("could not read the response");
            into.resize(offset + read);
            response.bodyLength += read;
            if (file != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                if (!WriteFile(file, chunk.data(), read, &written, nullptr) || written != read) return fail("could not write the file to save to");
            }
        }
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        return true;
    }
} // namespace Luwow::Net

#elif !defined(__APPLE__)

// Use `libcurl4-openssl-dev` (or your distribution's libcurl development package)

#include <curl/curl.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>

namespace Luwow::Net {
    // What libcurl's callbacks fill in while the request runs
    struct Transfer {
        CURL* curl = nullptr;
        const HttpRequest* request = nullptr;
        HttpResponse* response = nullptr;
        FILE* file = nullptr;
        bool fileFailed = false;
    };

    static bool startsWithNoCase(const std::string& text, const char* prefix) {
        for (size_t i = 0; prefix[i]; ++i) {
            if (i >= text.size() || std::tolower(static_cast<unsigned char>(text[i])) != prefix[i]) return false;
        }
        return true;
    }

    // Called with each header line.
    static size_t onHeader(char* data, size_t size, size_t count, void* user) {
        Transfer* transfer = static_cast<Transfer*>(user);
        size_t length = size * count;
        std::string line(data, length);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();

        HttpResponse& response = *transfer->response;
        if (startsWithNoCase(line, "http/")) {
            response.headers.clear();
            size_t code = line.find(' ');
            size_t message = code == std::string::npos ? std::string::npos : line.find(' ', code + 1);
            response.statusMessage = message == std::string::npos ? "" : line.substr(message + 1);
            return length;
        }
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            size_t valueStart = line.find_first_not_of(" \t", colon + 1);
            response.headers.emplace_back(line.substr(0, colon), valueStart == std::string::npos ? "" : line.substr(valueStart));
        }
        return length;
    }

    // Called with the body as it arrives.
    static size_t onBody(char* data, size_t size, size_t count, void* user) {
        Transfer* transfer = static_cast<Transfer*>(user);
        size_t length = size * count;
        HttpResponse& response = *transfer->response;
        response.bodyLength += length;

        long status = 0;
        curl_easy_getinfo(transfer->curl, CURLINFO_RESPONSE_CODE, &status);
        bool saving = !transfer->request->saveTo.empty() && status >= 200 && status < 300;
        if (!saving) {
            response.body.append(data, length);
            return length;
        }
        if (!transfer->file) {
            transfer->file = std::fopen(transfer->request->saveTo.c_str(), "wb");
            if (!transfer->file) {
                transfer->fileFailed = true;
                return 0; // Stops the transfer
            }
        }
        if (std::fwrite(data, 1, length, transfer->file) != length) {
            transfer->fileFailed = true;
            return 0;
        }
        return length;
    }

    // libcurl's global setup isn't thread-safe in every version, so it's done once
    static void initializeCurl() {
        static std::once_flag once;
        std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    }

    // libcurl, as Linux distributions ship it, is the system's HTTP client there
    bool sendHttpRequest(const HttpRequest& request, HttpResponse& response, std::string& error) {
        if (!startsWithNoCase(request.url, "http://") && !startsWithNoCase(request.url, "https://")) {
            error = "only http and https URLs are supported";
            return false;
        }

        initializeCurl();
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
        if (!curl) {
            error = "could not start libcurl";
            return false;
        }
        CURL* handle = curl.get();

        Transfer transfer;
        transfer.curl = handle;
        transfer.request = &request;
        transfer.response = &response;
        char message[CURL_ERROR_SIZE] = {};

        curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
        curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, message);
        curl_easy_setopt(handle, CURLOPT_USERAGENT, "Luwow");
        curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L); // Requests run off the main thread
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 10L);
        // Redirects can't lead to other protocols
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
        curl_easy_setopt(handle, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
        curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
        // Every encoding libcurl can decompress is accepted, and decompressed
        curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, onHeader);
        curl_easy_setopt(handle, CURLOPT_HEADERDATA, &transfer);
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, onBody);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &transfer);

        if (request.timeout >= 0) {
            long milliseconds = std::lround(request.timeout * 1000);
            curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, milliseconds > 0 ? milliseconds : 1L);
            curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
            curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, static_cast<long>(std::ceil(request.timeout > 1 ? request.timeout : 1)));
        }

        // POST is libcurl's method for a body, HEAD has none, and the rest are named as they are
        const std::string& method = request.method;
        if (method == "HEAD") {
            curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
        } else if (method != "GET" || !request.body.empty()) {
            if (method != "POST") curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, method.c_str());
        }
        if (!request.body.empty()) {
            curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.data());
            curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
        }

        // The request's headers as given.
        curl_slist* headers = nullptr;
        bool hasContentType = false;
        for (const auto& [name, value] : request.headers) {
            std::string line = name + ": " + value;
            headers = curl_slist_append(headers, line.c_str());
            std::string lower = name;
            for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lower == "content-type") hasContentType = true;
        }
        if (!request.body.empty() && !hasContentType) headers = curl_slist_append(headers, "Content-Type:");
        headers = curl_slist_append(headers, "Expect:");
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);

        CURLcode result = curl_easy_perform(handle);
        curl_slist_free_all(headers);

        long status = 0;
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
        response.status = static_cast<int>(status);
        bool saving = !request.saveTo.empty() && status >= 200 && status < 300;
        if (transfer.file) std::fclose(transfer.file);

        if (transfer.fileFailed) {
            std::remove(request.saveTo.c_str());
            error = "could not write the file to save to";
            return false;
        }
        if (result != CURLE_OK) {
            if (transfer.file) std::remove(request.saveTo.c_str());
            error = message[0] ? message : curl_easy_strerror(result);
            return false;
        }

        if (saving && !transfer.file) {
            FILE* file = std::fopen(request.saveTo.c_str(), "wb");
            if (!file) {
                error = "could not create the file to save to";
                return false;
            }
            std::fclose(file);
        }
        return true;
    }
} // namespace Luwow::Net

#endif
