#pragma once

#include <curl/curl.h>

#include <memory>

namespace ldm::engine {

struct CurlEasyDeleter {
    void operator()(CURL* handle) const noexcept { curl_easy_cleanup(handle); }
};
struct CurlMultiDeleter {
    void operator()(CURLM* handle) const noexcept { curl_multi_cleanup(handle); }
};
struct CurlSlistDeleter {
    void operator()(curl_slist* list) const noexcept { curl_slist_free_all(list); }
};

using CurlEasy = std::unique_ptr<CURL, CurlEasyDeleter>;
using CurlMulti = std::unique_ptr<CURLM, CurlMultiDeleter>;
using CurlSlist = std::unique_ptr<curl_slist, CurlSlistDeleter>;

}  // namespace ldm::engine
