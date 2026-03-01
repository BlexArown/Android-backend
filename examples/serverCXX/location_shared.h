#pragma once
#include <mutex>
#include <string>
#include <cstdint>

struct LocationShared {
    std::mutex mtx;

    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    double accuracy = 0.0;

    std::int64_t time_ms = 0;
    std::string provider = "—";

    std::string last_raw_json = "{}";
    std::string status = "waiting...";
    std::int64_t last_update_unix_ms = 0;

    std::string cells_text = "no data";
    std::string traffic_text = "no data";
};
