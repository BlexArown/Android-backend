#pragma once

#include "heatmap_generator.h"

#include <mutex>
#include <string>
#include <cstdint>
#include <vector>
#include <map>
#include <set>

struct CellSample {
    bool has_rsrp = false;
    double rsrp = 0.0;

    bool has_rsrq = false;
    double rsrq = 0.0;

    bool has_rssi = false;
    double rssi = 0.0;

    bool has_sinr = false;
    double sinr = 0.0;
};

struct LocationShared {
    std::mutex mtx;

    // Последняя локация
    double latitude = 55.030204;
    double longitude = 82.920430;
    double altitude = 0.0;
    double accuracy = 0.0;

    std::int64_t time_ms = 0;
    std::string provider = "default: Novosibirsk center";

    std::string last_raw_json = "{}";
    std::string status = "demo: map starts at Novosibirsk center";
    std::int64_t last_update_unix_ms = 0;

    std::string cells_text = "no data";
    std::string traffic_text = "no data";

    // Последние значения (для краткого текстового вывода)
    std::string last_radio = "unknown";

    double last_signal_power = 0.0;
    double last_signal_quality = 0.0;
    double last_signal_noise = 0.0;
    double last_asu = 0.0;

    bool has_signal_power = false;
    bool has_signal_quality = false;
    bool has_signal_noise = false;
    bool has_asu = false;

    // История графиков
    std::vector<double> hist_x;

    // Для каждого PCI - свой массив значений
    std::map<int, std::vector<double>> hist_rsrp_by_pci;
    std::map<int, std::vector<double>> hist_rsrq_by_pci;
    std::map<int, std::vector<double>> hist_rssi_by_pci;
    std::map<int, std::vector<double>> hist_sinr_by_pci;

    // Множество PCI, которые уже встречались
    std::set<int> known_pci;

    // Heatmap точки из PostgreSQL
    std::vector<HeatPoint> heat_points;

    // Управление демо-режимом / БД / телефоном
    bool request_load_db_points = false;
    bool request_start_phone_server = false;

    bool db_points_loaded_once = false;

    bool phone_server_running = false;
    bool phone_server_error = false;

    std::string db_status = "БД не подключалась";
    std::string phone_status = "Приём телефона выключен";

    double sample_index = 0.0;

    static constexpr std::size_t MAX_HISTORY = 200;

    void push_multi_pci_history(const std::map<int, CellSample>& cellsByPci) {
        hist_x.push_back(sample_index++);

        for (const auto& [pci, _] : cellsByPci) {
            known_pci.insert(pci);

            if (hist_rsrp_by_pci.find(pci) == hist_rsrp_by_pci.end()) {
                hist_rsrp_by_pci[pci] =
                    std::vector<double>(hist_x.size() - 1, 0.0);
            }

            if (hist_rsrq_by_pci.find(pci) == hist_rsrq_by_pci.end()) {
                hist_rsrq_by_pci[pci] =
                    std::vector<double>(hist_x.size() - 1, 0.0);
            }

            if (hist_rssi_by_pci.find(pci) == hist_rssi_by_pci.end()) {
                hist_rssi_by_pci[pci] =
                    std::vector<double>(hist_x.size() - 1, 0.0);
            }

            if (hist_sinr_by_pci.find(pci) == hist_sinr_by_pci.end()) {
                hist_sinr_by_pci[pci] =
                    std::vector<double>(hist_x.size() - 1, 0.0);
            }
        }

        for (auto& [pci, values] : hist_rsrp_by_pci) {
            auto it = cellsByPci.find(pci);

            if (it != cellsByPci.end() && it->second.has_rsrp) {
                values.push_back(it->second.rsrp);
            } else {
                values.push_back(0.0);
            }
        }

        for (auto& [pci, values] : hist_rsrq_by_pci) {
            auto it = cellsByPci.find(pci);

            if (it != cellsByPci.end() && it->second.has_rsrq) {
                values.push_back(it->second.rsrq);
            } else {
                values.push_back(0.0);
            }
        }

        for (auto& [pci, values] : hist_rssi_by_pci) {
            auto it = cellsByPci.find(pci);

            if (it != cellsByPci.end() && it->second.has_rssi) {
                values.push_back(it->second.rssi);
            } else {
                values.push_back(0.0);
            }
        }

        for (auto& [pci, values] : hist_sinr_by_pci) {
            auto it = cellsByPci.find(pci);

            if (it != cellsByPci.end() && it->second.has_sinr) {
                values.push_back(it->second.sinr);
            } else {
                values.push_back(0.0);
            }
        }

        trim_history();
    }

    void trim_history() {
        if (hist_x.size() <= MAX_HISTORY) {
            return;
        }

        hist_x.erase(hist_x.begin());

        for (auto& [_, values] : hist_rsrp_by_pci) {
            if (!values.empty()) {
                values.erase(values.begin());
            }
        }

        for (auto& [_, values] : hist_rsrq_by_pci) {
            if (!values.empty()) {
                values.erase(values.begin());
            }
        }

        for (auto& [_, values] : hist_rssi_by_pci) {
            if (!values.empty()) {
                values.erase(values.begin());
            }
        }

        for (auto& [_, values] : hist_sinr_by_pci) {
            if (!values.empty()) {
                values.erase(values.begin());
            }
        }
    }
};