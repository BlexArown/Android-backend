#include "gui_thread.h"

#include "osm_math.h"
#include "tile_manager.h"
#include "heatmap_generator.h"

#include <imgui.h>
#include <implot.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include "stb_image.h"

#include <GLFW/glfw3.h>
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <set>
#include <atomic>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

struct VisibleTile {
    int tx = 0;
    int ty = 0;
    double leftX = 0.0;
    double rightX = 0.0;
    double topY = 0.0;
    double bottomY = 0.0;
};

static void glfw_error_callback(int error, const char* description) {
    std::fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

static bool load_png_texture_for_overlay(const std::string& path, TileTexture& tex) {
    int w = 0;
    int h = 0;
    int channels = 0;

    stbi_set_flip_vertically_on_load(false);
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
    if (!data) return false;

    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

    stbi_image_free(data);

    tex.texture_id = id;
    tex.width = w;
    tex.height = h;
    tex.ready = true;
    return true;
}

static TileTexture* get_overlay_texture(const std::string& path, std::map<std::string, TileTexture>& cache) {
    auto it = cache.find(path);
    if (it != cache.end()) return &it->second;
    if (!std::filesystem::exists(path)) return nullptr;

    TileTexture tex;
    if (!load_png_texture_for_overlay(path, tex)) return nullptr;

    cache[path] = tex;
    return &cache[path];
}

static void clear_overlay_cache(std::map<std::string, TileTexture>& cache) {
    for (auto& [_, tex] : cache) {
        if (tex.texture_id != 0) {
            glDeleteTextures(1, &tex.texture_id);
            tex.texture_id = 0;
        }
    }
    cache.clear();
}

static std::vector<VisibleTile> calc_visible_tiles(
    int zoom,
    double centerTileX,
    double centerTileY,
    double plotW,
    double plotH,
    double plotCenterX,
    double plotCenterY
) {
    std::vector<VisibleTile> tiles;

    // Старый вариант грузил много лишних тайлов вокруг экрана.
    // Этот вариант берет только реально видимую область + небольшой запас 1 тайл.
    int minTx = floor_to_int(centerTileX - plotCenterX / 256.0) - 1;
    int maxTx = floor_to_int(centerTileX + (plotW - plotCenterX) / 256.0) + 1;

    int minTy = floor_to_int(centerTileY - (plotH - plotCenterY) / 256.0) - 1;
    int maxTy = floor_to_int(centerTileY + plotCenterY / 256.0) + 1;

    for (int ty = minTy; ty <= maxTy; ++ty) {
        for (int tx = minTx; tx <= maxTx; ++tx) {
            VisibleTile t;
            t.tx = tx;
            t.ty = ty;
            t.leftX = plotCenterX + (tx - centerTileX) * 256.0;
            t.rightX = t.leftX + 256.0;
            t.topY = plotCenterY + (centerTileY - ty) * 256.0;
            t.bottomY = t.topY - 256.0;
            tiles.push_back(t);
        }
    }

    return tiles;
}

static int count_filtered_heat_points(
    const std::vector<HeatPoint>& points,
    HeatCriterion criterion,
    int selectedEarfcn,
    int selectedPci
) {
    int count = 0;

    for (const HeatPoint& p : points) {
        if (selectedPci != -1 && p.pci != selectedPci) continue;

        if (criterion != HeatCriterion::ALTITUDE) {
            if (selectedEarfcn != -1 && p.earfcn != selectedEarfcn) continue;
        }

        if (criterion == HeatCriterion::RSRP && p.has_rsrp) count++;
        else if (criterion == HeatCriterion::RSRQ && p.has_rsrq) count++;
        else if (criterion == HeatCriterion::RSSI && p.has_rssi) count++;
        else if (criterion == HeatCriterion::ALTITUDE) count++;
    }

    return count;
}

static bool heat_point_matches_selected_mode(
    const HeatPoint& p,
    HeatCriterion criterion,
    int selectedEarfcn,
    int selectedPci
) {
    if (selectedPci != -1 && p.pci != selectedPci) return false;

    if (criterion != HeatCriterion::ALTITUDE) {
        if (selectedEarfcn != -1 && p.earfcn != selectedEarfcn) return false;
    }

    if (criterion == HeatCriterion::RSRP) return p.has_rsrp;
    if (criterion == HeatCriterion::RSRQ) return p.has_rsrq;
    if (criterion == HeatCriterion::RSSI) return p.has_rssi;
    if (criterion == HeatCriterion::ALTITUDE) return true;
    return false;
}

static ImVec4 legend_color(double ratio, double alpha = 0.85) {
    ratio = std::clamp(ratio, 0.0, 1.0);

    if (ratio < 0.5) {
        double t = ratio / 0.5;
        return ImVec4(
            static_cast<float>(t),
            static_cast<float>(t),
            static_cast<float>(150.0 / 255.0 * (1.0 - t)),
            static_cast<float>(alpha)
        );
    }

    double t = (ratio - 0.5) / 0.5;
    return ImVec4(
        1.0f,
        static_cast<float>(1.0 - t),
        0.0f,
        static_cast<float>(alpha)
    );
}

static void draw_heatmap_legend(HeatCriterion criterion) {
    ImGui::Separator();
    ImGui::TextUnformatted("Heatmap legend");

    auto swatch = [](const char* label, const ImVec4& color) {
        ImGui::ColorButton(label, color, ImGuiColorEditFlags_NoTooltip, ImVec2(18, 18));
        ImGui::SameLine();
        ImGui::TextUnformatted(label);
    };

    if (criterion == HeatCriterion::RSRP) {
        swatch("Excellent RSRP > -80 dBm", legend_color(1.0));
        swatch("Good RSRP -80..-90 dBm", legend_color(0.75));
        swatch("Fair RSRP -90..-100 dBm", legend_color(0.50));
        swatch("Poor RSRP -100..-110 dBm", legend_color(0.25));
        swatch("No signal < -110 dBm: dark blue / almost not painted", legend_color(0.0, 0.55));
    } else if (criterion == HeatCriterion::RSRQ) {
        swatch("Better quality, about -3 dB", legend_color(1.0));
        swatch("Medium quality, about -10 dB", legend_color(0.55));
        swatch("Poor quality, about -20 dB", legend_color(0.0));
    } else if (criterion == HeatCriterion::RSSI) {
        swatch("Strong RSSI, about -50 dBm", legend_color(1.0));
        swatch("Medium RSSI, about -80 dBm", legend_color(0.50));
        swatch("Weak RSSI, about -110 dBm", legend_color(0.0));
    } else {
        swatch("Highest altitude in selected data", legend_color(1.0));
        swatch("Medium altitude", legend_color(0.50));
        swatch("Lowest altitude in selected data", legend_color(0.0));
    }
}

static HeatCriterion g_currentLegendCriterion = HeatCriterion::RSRP;

static void draw_osm_map_window(
    double currentLat,
    double currentLon,
    const std::vector<HeatPoint>& heatPoints
) {
    static TileManager tileManager;
    static std::map<std::string, TileTexture> heatTextures;

    static int zoom = 18;
    static bool showHeatmap = true;
    static bool showMeasurementPoints = true;
    static bool pointsOnlyForSelectedMode = false;
    static int criterionIndex = 0;
    static int selectedEarfcn = -1;
    static int selectedPci = -1;
    static double radiusMeters = 40.0;
    static double idwPower = 2.0;

    // Отдельный центр карты: его можно двигать мышью независимо от последней GPS-точки.
    static double mapCenterLat = 55.030204;
    static double mapCenterLon = 82.920430;

    static std::atomic_bool generating{false};
    static std::atomic_bool generationDone{false};
    static std::atomic_int generatedCount{0};
    static std::atomic_int requestedCount{0};

    if (generationDone.exchange(false)) {
        clear_overlay_cache(heatTextures);
    }

    const char* criteria[] = { "RSRP", "RSRQ", "RSSI", "Altitude" };
    HeatCriterion criterion = static_cast<HeatCriterion>(criterionIndex);
    g_currentLegendCriterion = criterion;

    std::set<int> earfcnSet;
    std::set<int> pciSet;

    for (const HeatPoint& p : heatPoints) {
        if (p.earfcn != -1) earfcnSet.insert(p.earfcn);
        if (p.pci != -1) pciSet.insert(p.pci);
    }

    std::vector<int> earfcns;
    earfcns.push_back(-1);
    for (int e : earfcnSet) earfcns.push_back(e);

    std::vector<int> pcis;
    pcis.push_back(-1);
    for (int p : pciSet) pcis.push_back(p);

    bool selectedEarfcnStillExists = false;
    for (int e : earfcns) {
        if (e == selectedEarfcn) {
            selectedEarfcnStillExists = true;
            break;
        }
    }
    if (!selectedEarfcnStillExists) selectedEarfcn = -1;

    bool selectedPciStillExists = false;
    for (int p : pcis) {
        if (p == selectedPci) {
            selectedPciStillExists = true;
            break;
        }
    }
    if (!selectedPciStillExists) selectedPci = -1;

    ImGui::Begin("OSM Map + Heatmap");

    ImGui::SliderInt("Zoom", &zoom, 0, 18);
    ImGui::Text("Map center lat: %.8f", mapCenterLat);
    ImGui::Text("Map center lon: %.8f", mapCenterLon);
    ImGui::Text("Current GPS lat: %.8f", currentLat);
    ImGui::Text("Current GPS lon: %.8f", currentLon);
    ImGui::Text("Heat points loaded: %d", (int)heatPoints.size());
    ImGui::Text("Heat points for selected mode: %d", count_filtered_heat_points(heatPoints, criterion, selectedEarfcn, selectedPci));

    bool validLat = (mapCenterLat >= -85.05112878 && mapCenterLat <= 85.05112878);
    bool validLon = (mapCenterLon >= -180.0 && mapCenterLon <= 180.0);

    if (!validLat || !validLon) {
        ImGui::Text("Нет валидной геопозиции для карты");
        ImGui::End();
        return;
    }

    if (ImGui::Button("Reset map center to Novosibirsk")) {
        mapCenterLat = 55.030204;
        mapCenterLon = 82.920430;
    }
    ImGui::SameLine();
    if (ImGui::Button("Center map on current GPS")) {
        if (currentLat >= -85.05112878 && currentLat <= 85.05112878 &&
            currentLon >= -180.0 && currentLon <= 180.0) {
            mapCenterLat = currentLat;
            mapCenterLon = currentLon;
        }
    }
    ImGui::TextDisabled("Drag map with left mouse button inside the plot.");

    ImGui::Separator();
    ImGui::Checkbox("Show heatmap", &showHeatmap);
    ImGui::SameLine();
    ImGui::Checkbox("Show loaded points", &showMeasurementPoints);
    ImGui::SameLine();
    ImGui::Checkbox("Points only selected mode", &pointsOnlyForSelectedMode);

    int oldCriterionIndex = criterionIndex;
    int oldSelectedEarfcn = selectedEarfcn;
    int oldSelectedPci = selectedPci;

    ImGui::Combo("Criterion", &criterionIndex, criteria, IM_ARRAYSIZE(criteria));
    criterion = static_cast<HeatCriterion>(criterionIndex);

    if (criterion != HeatCriterion::ALTITUDE) {
        std::string preview = selectedEarfcn == -1 ? "All EARFCN" : std::to_string(selectedEarfcn);
        if (ImGui::BeginCombo("EARFCN", preview.c_str())) {
            for (int e : earfcns) {
                std::string label = e == -1 ? "All EARFCN" : std::to_string(e);
                bool selected = (selectedEarfcn == e);
                if (ImGui::Selectable(label.c_str(), selected)) selectedEarfcn = e;
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    } else {
        selectedEarfcn = -1;
    }

    {
        std::string preview = selectedPci == -1 ? "All PCI" : std::to_string(selectedPci);
        if (ImGui::BeginCombo("PCI", preview.c_str())) {
            for (int p : pcis) {
                std::string label = p == -1 ? "All PCI" : std::to_string(p);
                bool selected = (selectedPci == p);
                if (ImGui::Selectable(label.c_str(), selected)) selectedPci = p;
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }


    if (oldCriterionIndex != criterionIndex ||
        oldSelectedEarfcn != selectedEarfcn ||
        oldSelectedPci != selectedPci) {
        clear_overlay_cache(heatTextures);
    }

    double minRadius = 10.0;
    double maxRadius = 40.0;
    double minPower = 1.0;
    double maxPower = 4.0;
    ImGui::SliderScalar("IDW radius, meters", ImGuiDataType_Double, &radiusMeters, &minRadius, &maxRadius, "%.1f");
    ImGui::SliderScalar("IDW power", ImGuiDataType_Double, &idwPower, &minPower, &maxPower, "%.1f");

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float plotWidth = avail.x;
    float plotHeight = avail.y - 5.0f;
    if (plotWidth < 300.0f) plotWidth = 300.0f;
    if (plotHeight < 300.0f) plotHeight = 300.0f;

    ImVec2 plotPixelSize(plotWidth, plotHeight);
    double plotW = static_cast<double>(plotWidth);
    double plotH = static_cast<double>(plotHeight);
    double plotCenterX = plotW / 2.0;
    double plotCenterY = plotH / 2.0;

    double centerTileX = lon_to_tile_x(mapCenterLon, zoom);
    double centerTileY = lat_to_tile_y(mapCenterLat, zoom);

    std::vector<VisibleTile> visibleTiles = calc_visible_tiles(
        zoom,
        centerTileX,
        centerTileY,
        plotW,
        plotH,
        plotCenterX,
        plotCenterY
    );

    for (const VisibleTile& t : visibleTiles) {
        tileManager.request_tile_async(zoom, t.tx, t.ty);
    }

    ImGui::Text("Visible tiles: %d", (int)visibleTiles.size());
    ImGui::Text("OSM tile downloads in background: %d", tileManager.active_downloads());

    bool canGenerate = !generating.load();

    if (canGenerate) {
        if (ImGui::Button("Generate visible heatmap tiles")) {
            std::vector<HeatPoint> pointsCopy = heatPoints;
            std::vector<VisibleTile> tilesCopy = visibleTiles;
            HeatCriterion criterionCopy = criterion;
            int earfcnCopy = selectedEarfcn;
            int pciCopy = selectedPci;
            int zoomCopy = zoom;
            double radiusCopy = radiusMeters;
            double powerCopy = idwPower;

            generating = true;
            generationDone = false;
            generatedCount = 0;
            requestedCount = (int)tilesCopy.size();

            std::thread([pointsCopy, tilesCopy, criterionCopy, earfcnCopy, pciCopy, zoomCopy, radiusCopy, powerCopy]() {
                int done = 0;
                for (const VisibleTile& t : tilesCopy) {
                    int xw = wrap_tile_x(t.tx, zoomCopy);
                    int yc = clamp_tile_y(t.ty, zoomCopy);
                    std::string outPath = make_heatmap_tile_path(zoomCopy, xw, yc, criterionCopy, earfcnCopy, pciCopy);
                    generate_heatmap_tile_png(zoomCopy, xw, yc, criterionCopy, earfcnCopy, pciCopy, pointsCopy, outPath, radiusCopy, powerCopy);
                    done++;
                    generatedCount = done;
                }
                generationDone = true;
                generating = false;
            }).detach();
        }
    } else {
        ImGui::Button("Generating heatmap...");
    }

    ImGui::SameLine();
    if (ImGui::Button("Clear heatmap texture cache")) {
        clear_overlay_cache(heatTextures);
    }

    if (generating.load()) {
        ImGui::Text("Generating heatmap: %d / %d", generatedCount.load(), requestedCount.load());
    }

    if (ImPlot::BeginPlot("##OsmPlot", plotPixelSize, ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes(
            nullptr,
            nullptr,
            ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoTickMarks | ImPlotAxisFlags_NoGridLines,
            ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoTickMarks | ImPlotAxisFlags_NoGridLines
        );

        ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, plotW, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, plotH, ImPlotCond_Always);

        if (ImPlot::IsPlotHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            ImVec2 delta = ImGui::GetIO().MouseDelta;
            if (std::abs(delta.x) > 0.0f || std::abs(delta.y) > 0.0f) {
                centerTileX -= static_cast<double>(delta.x) / 256.0;
                centerTileY -= static_cast<double>(delta.y) / 256.0;

                int n = tiles_per_axis(zoom);
                while (centerTileX < 0.0) centerTileX += n;
                while (centerTileX >= n) centerTileX -= n;
                centerTileY = std::clamp(centerTileY, 0.0, static_cast<double>(n - 1));

                mapCenterLon = tile_x_to_lon(centerTileX, zoom);
                mapCenterLat = tile_y_to_lat(centerTileY, zoom);
            }
        }

        for (const VisibleTile& t : visibleTiles) {
            TileTexture* tile = tileManager.get_or_load_tile(zoom, t.tx, t.ty);
            if (!tile || !tile->ready) continue;

            int xw = wrap_tile_x(t.tx, zoom);
            int yc = clamp_tile_y(t.ty, zoom);

            std::string label = "tile_" + std::to_string(zoom) + "_" + std::to_string(xw) + "_" + std::to_string(yc);

            ImPlot::PlotImage(
                label.c_str(),
                (ImTextureID)(intptr_t)tile->texture_id,
                ImPlotPoint(t.leftX, t.bottomY),
                ImPlotPoint(t.rightX, t.topY)
            );
        }

        if (showHeatmap) {
            for (const VisibleTile& t : visibleTiles) {
                int xw = wrap_tile_x(t.tx, zoom);
                int yc = clamp_tile_y(t.ty, zoom);

                std::string path = make_heatmap_tile_path(zoom, xw, yc, criterion, selectedEarfcn, selectedPci);
                TileTexture* heat = get_overlay_texture(path, heatTextures);
                if (!heat || !heat->ready) continue;

                std::string label = "heat_" + std::to_string(zoom) + "_" + std::to_string(xw) + "_" + std::to_string(yc);

                ImPlot::PlotImage(
                    label.c_str(),
                    (ImTextureID)(intptr_t)heat->texture_id,
                    ImPlotPoint(t.leftX, t.bottomY),
                    ImPlotPoint(t.rightX, t.topY)
                );
            }
        }

        if (showMeasurementPoints && !heatPoints.empty()) {
            std::vector<double> pointXs;
            std::vector<double> pointYs;
            pointXs.reserve(heatPoints.size());
            pointYs.reserve(heatPoints.size());

            for (const HeatPoint& p : heatPoints) {
                if (pointsOnlyForSelectedMode && !heat_point_matches_selected_mode(p, criterion, selectedEarfcn, selectedPci)) {
                    continue;
                }

                double ptTileX = lon_to_tile_x(p.longitude, zoom);
                double ptTileY = lat_to_tile_y(p.latitude, zoom);

                double sx = plotCenterX + (ptTileX - centerTileX) * 256.0;
                double sy = plotCenterY + (centerTileY - ptTileY) * 256.0;

                if (sx >= -20.0 && sx <= plotW + 20.0 && sy >= -20.0 && sy <= plotH + 20.0) {
                    pointXs.push_back(sx);
                    pointYs.push_back(sy);
                }
            }

            if (!pointXs.empty()) {
                ImPlotSpec pointSpec;
                pointSpec.Marker = ImPlotMarker_Circle;
                pointSpec.MarkerSize = 1.0f;
                ImPlot::PlotScatter("Loaded points", pointXs.data(), pointYs.data(), (int)pointXs.size(), pointSpec);
            }
        }

        double gpsTileX = lon_to_tile_x(currentLon, zoom);
        double gpsTileY = lat_to_tile_y(currentLat, zoom);
        double px[1] = { plotCenterX + (gpsTileX - centerTileX) * 256.0 };
        double py[1] = { plotCenterY + (centerTileY - gpsTileY) * 256.0 };
        ImPlotSpec gpsSpec;
        gpsSpec.Marker = ImPlotMarker_Cross;
        gpsSpec.MarkerSize = 8.0f;
        ImPlot::PlotScatter("Current position", px, py, 1, gpsSpec);

        ImPlot::EndPlot();
    }

    ImGui::End();
}

void run_gui(LocationShared* loc) {
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        std::lock_guard<std::mutex> lg(loc->mtx);
        loc->status = "gui error: glfwInit failed";
        return;
    }

    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow* window = glfwCreateWindow(1200, 850, "Backend Server: Location + Telemetry + OSM + Heatmap", nullptr, nullptr);
    if (window == nullptr) {
        glfwTerminate();
        std::lock_guard<std::mutex> lg(loc->mtx);
        loc->status = "gui error: glfwCreateWindow failed";
        return;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->AddFontFromFileTTF(
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        18.0f,
        nullptr,
        io.Fonts->GetGlyphRangesCyrillic()
    );

    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        double lat = 0.0, lon = 0.0, alt = 0.0, acc = 0.0;
        std::int64_t tms = 0, lastUpd = 0;
        std::string provider, status, raw, cells, traffic, lastRadio;
        double lastSignalPower = 0.0, lastSignalQuality = 0.0, lastSignalNoise = 0.0, lastAsu = 0.0;
        bool hasSignalPower = false, hasSignalQuality = false, hasSignalNoise = false, hasAsu = false;
        std::vector<double> x;
        std::map<int, std::vector<double>> rsrpByPci;
        std::map<int, std::vector<double>> rssiByPci;
        std::map<int, std::vector<double>> sinrByPci;
        std::vector<HeatPoint> heatPoints;
        std::string dbStatus;
        std::string phoneStatus;
        bool dbPointsLoadedOnce = false;
        bool phoneServerRunning = false;
        bool phoneServerError = false;

        {
            std::lock_guard<std::mutex> lg(loc->mtx);
            lat = loc->latitude;
            lon = loc->longitude;
            alt = loc->altitude;
            acc = loc->accuracy;
            tms = loc->time_ms;
            provider = loc->provider;
            status = loc->status;
            raw = loc->last_raw_json;
            lastUpd = loc->last_update_unix_ms;
            cells = loc->cells_text;
            traffic = loc->traffic_text;
            lastRadio = loc->last_radio;
            lastSignalPower = loc->last_signal_power;
            lastSignalQuality = loc->last_signal_quality;
            lastSignalNoise = loc->last_signal_noise;
            lastAsu = loc->last_asu;
            hasSignalPower = loc->has_signal_power;
            hasSignalQuality = loc->has_signal_quality;
            hasSignalNoise = loc->has_signal_noise;
            hasAsu = loc->has_asu;
            x = loc->hist_x;
            rsrpByPci = loc->hist_rsrp_by_pci;
            rssiByPci = loc->hist_rssi_by_pci;
            sinrByPci = loc->hist_sinr_by_pci;
            heatPoints = loc->heat_points;
            dbStatus = loc->db_status;
            phoneStatus = loc->phone_status;
            dbPointsLoadedOnce = loc->db_points_loaded_once;
            phoneServerRunning = loc->phone_server_running;
            phoneServerError = loc->phone_server_error;
        }

        ImGui::Begin("Demo controls");
        ImGui::TextWrapped("Источник точек для heatmap");
        ImGui::Separator();

        if (ImGui::Button("Load ALL heat points from PostgreSQL")) {
            std::lock_guard<std::mutex> lg(loc->mtx);
            loc->request_load_db_points = true;
            loc->db_status = "Запрошена загрузка точек из БД...";
        }
        ImGui::SameLine();
        if (dbPointsLoadedOnce) {
            ImGui::Text("loaded");
        } else {
            ImGui::Text("not loaded");
        }
        ImGui::TextWrapped("DB status: %s", dbStatus.c_str());

        if (ImGui::Button("Connect phone / start receiver")) {
            std::lock_guard<std::mutex> lg(loc->mtx);
            loc->request_start_phone_server = true;
            loc->phone_status = "Запрошен запуск приёма телефона...";
        }
        ImGui::SameLine();
        if (phoneServerRunning) {
            ImGui::Text("running");
        } else if (phoneServerError) {
            ImGui::Text("error");
        } else {
            ImGui::Text("off");
        }
        ImGui::TextWrapped("Phone status: %s", phoneStatus.c_str());

        ImGui::Text("Heat points currently loaded: %d", (int)heatPoints.size());
        ImGui::TextDisabled("Карта OSM работает даже без БД: стартовый центр — Новосибирск.");

        draw_heatmap_legend(g_currentLegendCriterion);

        ImGui::SetWindowSize(ImVec2(520, 560), ImGuiCond_Once);
        ImGui::End();

        ImGui::Begin("Location");
        ImGui::Text("Status: %s", status.c_str());
        ImGui::Separator();
        ImGui::Text("Latitude : %.8f", lat);
        ImGui::Text("Longitude: %.8f", lon);
        ImGui::Text("Altitude : %.3f", alt);
        ImGui::Text("Accuracy : %.3f", acc);
        ImGui::Text("Time(ms) : %lld", (long long)tms);
        ImGui::Text("Provider : %s", provider.c_str());
        ImGui::Text("Last update unix(ms): %lld", (long long)lastUpd);
        ImGui::Separator();
        ImGui::TextWrapped("Last raw JSON (last message):");
        ImGui::InputTextMultiline("##raw", raw.data(), raw.size() + 1, ImVec2(-1, 180), ImGuiInputTextFlags_ReadOnly);
        ImGui::End();

        ImGui::Begin("Cells");
        ImGui::Text("Last radio: %s", lastRadio.c_str());
        if (hasSignalPower) ImGui::Text("Last RSRP/Power : %.2f", lastSignalPower);
        else ImGui::Text("Last RSRP/Power : no data");
        if (hasSignalQuality) ImGui::Text("Last Quality    : %.2f", lastSignalQuality);
        else ImGui::Text("Last Quality    : no data");
        if (hasSignalNoise) ImGui::Text("Last SINR/Noise : %.2f", lastSignalNoise);
        else ImGui::Text("Last SINR/Noise : no data");
        if (hasAsu) ImGui::Text("ASU             : %.2f", lastAsu);
        else ImGui::Text("ASU             : no data");
        ImGui::Separator();
        ImGui::TextWrapped("Cells JSON block:");
        ImGui::InputTextMultiline("##cells", cells.data(), cells.size() + 1, ImVec2(-1, 220), ImGuiInputTextFlags_ReadOnly);
        ImGui::End();

        ImGui::Begin("Traffic");
        ImGui::TextWrapped("Traffic JSON block:");
        ImGui::InputTextMultiline("##traffic", traffic.data(), traffic.size() + 1, ImVec2(-1, 180), ImGuiInputTextFlags_ReadOnly);
        ImGui::End();

        ImGui::Begin("Signal graphs");
        if (!x.empty()) {
            if (ImPlot::BeginPlot("RSRP by PCI", ImVec2(-1, 250))) {
                ImPlot::SetupAxes("Sample", "RSRP");
                for (const auto& [pci, values] : rsrpByPci) {
                    if (!values.empty() && values.size() == x.size()) {
                        std::string label = "PCI " + std::to_string(pci);
                        ImPlot::PlotLine(label.c_str(), x.data(), values.data(), (int)x.size());
                    }
                }
                ImPlot::EndPlot();
            }
            if (ImPlot::BeginPlot("RSSI by PCI", ImVec2(-1, 250))) {
                ImPlot::SetupAxes("Sample", "RSSI");
                for (const auto& [pci, values] : rssiByPci) {
                    if (!values.empty() && values.size() == x.size()) {
                        std::string label = "PCI " + std::to_string(pci);
                        ImPlot::PlotLine(label.c_str(), x.data(), values.data(), (int)x.size());
                    }
                }
                ImPlot::EndPlot();
            }
            if (ImPlot::BeginPlot("SINR by PCI", ImVec2(-1, 250))) {
                ImPlot::SetupAxes("Sample", "SINR");
                for (const auto& [pci, values] : sinrByPci) {
                    if (!values.empty() && values.size() == x.size()) {
                        std::string label = "PCI " + std::to_string(pci);
                        ImPlot::PlotLine(label.c_str(), x.data(), values.data(), (int)x.size());
                    }
                }
                ImPlot::EndPlot();
            }
        } else {
            ImGui::Text("Пока нет данных для графиков.");
        }
        ImGui::End();

        draw_osm_map_window(lat, lon, heatPoints);

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImPlot::DestroyContext();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
}
