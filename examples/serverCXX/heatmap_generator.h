#pragma once

#include <string>
#include <vector>

struct HeatPoint {
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;

    int earfcn = -1;
    int pci = -1;

    bool has_rsrp = false;
    double rsrp = 0.0;

    bool has_rsrq = false;
    double rsrq = 0.0;

    bool has_rssi = false;
    double rssi = 0.0;
};

enum class HeatCriterion {
    RSRP = 0,
    RSRQ = 1,
    RSSI = 2,
    ALTITUDE = 3
};

std::string heat_criterion_name(HeatCriterion criterion);

std::string make_heatmap_tile_path(
    int z,
    int x,
    int y,
    HeatCriterion criterion,
    int earfcn,
    int pci
);

bool generate_heatmap_tile_png(
    int z,
    int x,
    int y,
    HeatCriterion criterion,
    int earfcn,
    int pci,
    const std::vector<HeatPoint>& points,
    const std::string& out_path,
    double radius_meters,
    double power
);
