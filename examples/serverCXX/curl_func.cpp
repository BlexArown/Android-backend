#include "curl_func.h"

#include <curl/curl.h>
#include <cstdio>
#include <filesystem>

static size_t write_file_callback(void* ptr, size_t size, size_t nmemb, void* stream) {
    FILE* f = static_cast<FILE*>(stream);
    return std::fwrite(ptr, size, nmemb, f);
}

bool download_file_with_curl(const std::string& url, const std::string& out_path) {
    namespace fs = std::filesystem;

    fs::path p(out_path);
    fs::create_directories(p.parent_path());

    FILE* f = std::fopen(out_path.c_str(), "wb");
    if (!f) {
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::fclose(f);
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "server_app_osm/1.0");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);

    curl_easy_cleanup(curl);
    std::fclose(f);

    return res == CURLE_OK;
}
