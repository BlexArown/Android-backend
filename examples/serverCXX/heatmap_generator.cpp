#include "heatmap_generator.h"
#include "osm_math.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <vector>

struct Color {
    int r = 0;
    int g = 0;
    int b = 0;
};

struct AccumCell {
    double sumValue = 0.0;
    double sumWeight = 0.0;
};

static constexpr int TILE_SIZE = 256;
static constexpr double PI = 3.14159265358979323846;
static constexpr double RAD = PI / 180.0;

static Color lerp(Color a, Color b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    return {
        static_cast<int>(a.r + (b.r - a.r) * t),
        static_cast<int>(a.g + (b.g - a.g) * t),
        static_cast<int>(a.b + (b.b - a.b) * t)
    };
}

static bool point_value(const HeatPoint& p, HeatCriterion criterion, double& value) {
    switch (criterion) {
        case HeatCriterion::RSRP:
            if (!p.has_rsrp) return false;
            value = p.rsrp;
            return true;

        case HeatCriterion::RSRQ:
            if (!p.has_rsrq) return false;
            value = p.rsrq;
            return true;

        case HeatCriterion::RSSI:
            if (!p.has_rssi) return false;
            value = p.rssi;
            return true;

        case HeatCriterion::ALTITUDE:
            value = p.altitude;
            return true;
    }

    return false;
}

static bool point_allowed_by_filters(
    const HeatPoint& p,
    HeatCriterion criterion,
    int earfcn,
    int pci
) {
    // PCI фильтр применяем ко всем режимам, потому что точка heatmap приходит из конкретной соты.
    if (pci != -1 && p.pci != pci) {
        return false;
    }

    // EARFCN имеет смысл только для радио-критериев.
    if (criterion != HeatCriterion::ALTITUDE) {
        if (earfcn != -1 && p.earfcn != earfcn) {
            return false;
        }
    }

    return true;
}

static bool normalize_value(
    double value,
    HeatCriterion criterion,
    const std::vector<double>& allValues,
    double& outRatio
) {
    if (criterion == HeatCriterion::RSRP) {
        // Для демонстрации делаем диапазон чуть шире, чтобы очень слабые зоны были видны темно-синим.
        // В легенде отмечаем, что ниже -110 dBm это No Signal / Unusable.
        if (value < -120.0) return false;
        outRatio = (value - (-120.0)) / 40.0; // -120 -> blue, -80 -> red
        outRatio = std::clamp(outRatio, 0.0, 1.0);
        return true;
    }

    if (criterion == HeatCriterion::RSRQ) {
        outRatio = (value - (-20.0)) / 17.0; // -20 -> blue, -3 -> red
        outRatio = std::clamp(outRatio, 0.0, 1.0);
        return true;
    }

    if (criterion == HeatCriterion::RSSI) {
        outRatio = (value - (-110.0)) / 60.0; // -110 -> blue, -50 -> red
        outRatio = std::clamp(outRatio, 0.0, 1.0);
        return true;
    }

    if (criterion == HeatCriterion::ALTITUDE) {
        if (allValues.empty()) return false;

        auto [mnIt, mxIt] = std::minmax_element(allValues.begin(), allValues.end());
        double mn = *mnIt;
        double mx = *mxIt;

        if (std::abs(mx - mn) < 0.000001) {
            outRatio = 0.5;
        } else {
            outRatio = (value - mn) / (mx - mn);
        }

        outRatio = std::clamp(outRatio, 0.0, 1.0);
        return true;
    }

    return false;
}

static Color heat_color(double ratio) {
    // 0.00 dark blue -> 0.50 yellow -> 1.00 red
    Color blue{0, 0, 150};
    Color yellow{255, 255, 0};
    Color red{255, 0, 0};

    if (ratio < 0.5) {
        return lerp(blue, yellow, ratio / 0.5);
    }

    return lerp(yellow, red, (ratio - 0.5) / 0.5);
}

static double meters_per_pixel_at_lat(double lat, int zoom) {
    // Web Mercator approximate meters per pixel.
    return 156543.03392 * std::cos(lat * RAD) / static_cast<double>(1 << zoom);
}

static double idw_weight_from_distance(double distanceMeters, double radiusMeters, double power) {
    // Плавно гасим влияние к краю радиуса, чтобы круги не были с резкой границей.
    double t = std::clamp(distanceMeters / radiusMeters, 0.0, 1.0);
    double smooth = 1.0 - t * t * (3.0 - 2.0 * t);

    // Маленькая добавка защищает от бесконечного веса в самой точке.
    double d = std::max(distanceMeters, 1.0);

    if (std::abs(power - 2.0) < 0.0001) {
        return smooth / (d * d);
    }

    if (std::abs(power - 1.0) < 0.0001) {
        return smooth / d;
    }

    return smooth / std::pow(d, power);
}

std::string heat_criterion_name(HeatCriterion criterion) {
    switch (criterion) {
        case HeatCriterion::RSRP: return "RSRP";
        case HeatCriterion::RSRQ: return "RSRQ";
        case HeatCriterion::RSSI: return "RSSI";
        case HeatCriterion::ALTITUDE: return "Altitude";
    }

    return "Unknown";
}

std::string make_heatmap_tile_path(
    int z,
    int x,
    int y,
    HeatCriterion criterion,
    int earfcn,
    int pci
) {
    std::ostringstream ss;

    ss << "build/heatmap/" << heat_criterion_name(criterion);

    if (criterion != HeatCriterion::ALTITUDE) {
        ss << "/earfcn_" << earfcn;
    } else {
        ss << "/all_earfcn";
    }

    if (pci != -1) {
        ss << "/pci_" << pci;
    } else {
        ss << "/pci_all";
    }

    ss << "/" << z << "/" << x << "/" << y << ".png";
    return ss.str();
}

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
) {
    namespace fs = std::filesystem;

    radius_meters = std::clamp(radius_meters, 10.0, 40.0);
    if (power < 0.1) power = 2.0;

    const int w = TILE_SIZE;
    const int h = TILE_SIZE;
    const int channels = 4;

    std::vector<AccumCell> grid(w * h);
    std::vector<double> criterionValues;
    criterionValues.reserve(points.size());

    // Новый вариант: не опрашиваем каждый пиксель по всем точкам.
    // Вместо этого каждая измеренная точка "размазывается" кругом заданного радиуса.
    // Это стабильнее на zoom 14/15 и не теряет heatmap в области, где точки реально есть.
    for (const HeatPoint& p : points) {
        if (!point_allowed_by_filters(p, criterion, earfcn, pci)) {
            continue;
        }

        double metric = 0.0;
        if (!point_value(p, criterion, metric)) {
            continue;
        }

        double mpp = meters_per_pixel_at_lat(p.latitude, z);
        if (mpp <= 0.000001) {
            continue;
        }

        double radiusPx = radius_meters / mpp;
        if (radiusPx < 1.0) {
            radiusPx = 1.0;
        }

        double globalX = lon_to_tile_x(p.longitude, z) * TILE_SIZE;
        double globalY = lat_to_tile_y(p.latitude, z) * TILE_SIZE;

        double localX = globalX - static_cast<double>(x * TILE_SIZE);
        double localY = globalY - static_cast<double>(y * TILE_SIZE);

        // Если круг вообще не пересекает этот тайл — пропускаем.
        if (localX + radiusPx < 0.0 || localX - radiusPx >= w ||
            localY + radiusPx < 0.0 || localY - radiusPx >= h) {
            continue;
        }

        criterionValues.push_back(metric);

        int startX = std::max(0, static_cast<int>(std::floor(localX - radiusPx)));
        int endX   = std::min(w - 1, static_cast<int>(std::ceil(localX + radiusPx)));
        int startY = std::max(0, static_cast<int>(std::floor(localY - radiusPx)));
        int endY   = std::min(h - 1, static_cast<int>(std::ceil(localY + radiusPx)));

        double radius2 = radiusPx * radiusPx;

        for (int py = startY; py <= endY; ++py) {
            double dy = static_cast<double>(py) - localY;

            for (int px = startX; px <= endX; ++px) {
                double dx = static_cast<double>(px) - localX;
                double d2 = dx * dx + dy * dy;

                if (d2 > radius2) {
                    continue;
                }

                double distanceMeters = std::sqrt(d2) * mpp;
                if (distanceMeters > radius_meters) {
                    continue;
                }

                double weight = idw_weight_from_distance(distanceMeters, radius_meters, power);
                int idx = py * w + px;

                grid[idx].sumValue += metric * weight;
                grid[idx].sumWeight += weight;
            }
        }
    }

    if (criterionValues.empty()) {
        return false;
    }

    std::vector<unsigned char> image(w * h * channels, 0);
    bool wroteAnyPixel = false;

    for (int i = 0; i < w * h; ++i) {
        if (grid[i].sumWeight <= 0.0) {
            continue;
        }

        double interpolated = grid[i].sumValue / grid[i].sumWeight;

        double ratio = 0.0;
        if (!normalize_value(interpolated, criterion, criterionValues, ratio)) {
            continue;
        }

        Color c = heat_color(ratio);

        // Чем больше рядом точек, тем плотнее overlay.
        double density = std::clamp(grid[i].sumWeight * 150.0, 0.0, 1.0);
        unsigned char alpha = static_cast<unsigned char>(std::clamp(70.0 + density * 130.0, 70.0, 200.0));

        image[i * 4 + 0] = static_cast<unsigned char>(c.r);
        image[i * 4 + 1] = static_cast<unsigned char>(c.g);
        image[i * 4 + 2] = static_cast<unsigned char>(c.b);
        image[i * 4 + 3] = alpha;

        wroteAnyPixel = true;
    }

    if (!wroteAnyPixel) {
        return false;
    }

    fs::path p(out_path);
    fs::create_directories(p.parent_path());

    int ok = stbi_write_png(out_path.c_str(), w, h, channels, image.data(), w * channels);
    return ok != 0;
}
