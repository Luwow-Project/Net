#pragma once

#include <string>
#include <utility>
#include <vector>

namespace Luwow::Net {
    struct HttpRequest {
        std::string url;
        std::string method = "GET";
        std::vector<std::pair<std::string, std::string>> headers;
        std::string body;
        // Seconds for each step (resolving, connecting, sending, receiving), negative waits as long as the
        // system allows
        double timeout = -1;
        // When set, a successful response's body is written to this file as it arrives instead of
        // being kept in memory, so its size shows the progress
        std::string saveTo;
    };

    struct HttpResponse {
        int status = 0;
        std::string statusMessage;
        // In the order the server sent them
        std::vector<std::pair<std::string, std::string>> headers;
        // Empty when it was saved to the request's saveTo
        std::string body;
        size_t bodyLength = 0;
    };

    // Sends the request over HTTP or HTTPS with the platform's client, 
    // following redirects and decompressing the body. Returns false with error set when there's no response.
    bool sendHttpRequest(const HttpRequest& request, HttpResponse& response, std::string& error);
} // namespace Luwow::Net
