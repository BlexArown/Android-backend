#pragma once
#include <mutex>
#include <string>
#include <cstdint>
#include <vector>

struct LocationShared {
    std::mutex mtx;

    // Последняя локация
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

    // Последние значения по сигналу
    std::string last_radio = "unknown";
    double last_signal_power = 0.0;   // rsrp
    double last_signal_quality = 0.0; // rsrq
    double last_signal_noise = 0.0;   // rssnr
    double last_asu = 0.0;

    bool has_signal_power = false;
    bool has_signal_quality = false;
    bool has_signal_noise = false;
    bool has_asu = false;

    // История для графиков
    std::vector<double> hist_x;
    std::vector<double> hist_signal_power;
    std::vector<double> hist_signal_quality;
    std::vector<double> hist_signal_noise;
    std::vector<double> hist_asu;

    double sample_index = 0.0;

    static constexpr std::size_t MAX_HISTORY = 200;

    void push_history(
        bool power_ok, double power,
        bool quality_ok, double quality,
        bool noise_ok, double noise,
        bool asu_ok, double asu
    ) {
        hist_x.push_back(sample_index++);
        hist_signal_power.push_back(power_ok ? power : 0.0);
        hist_signal_quality.push_back(quality_ok ? quality : 0.0);
        hist_signal_noise.push_back(noise_ok ? noise : 0.0);
        hist_asu.push_back(asu_ok ? asu : 0.0);

        if (hist_x.size() > MAX_HISTORY) {
            hist_x.erase(hist_x.begin());
            hist_signal_power.erase(hist_signal_power.begin());
            hist_signal_quality.erase(hist_signal_quality.begin());
            hist_signal_noise.erase(hist_signal_noise.begin());
            hist_asu.erase(hist_asu.begin());
        }
    }
};