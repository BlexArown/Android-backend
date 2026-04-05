#include "gui_thread.h"

#include "osm_math.h"
#include "tile_manager.h"

#include <imgui.h>
#include <implot.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <cmath>
#include <algorithm>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#endif

static void glfw_error_callback(int error, const char* description) {
    std::fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

static void draw_osm_map_window(double centerLat, double centerLon) {
    static TileManager tileManager;
    static int zoom = 13;

    ImGui::Begin("OSM Map");

    ImGui::SliderInt("Zoom", &zoom, 0, 18);
    ImGui::Text("Center lat: %.8f", centerLat);
    ImGui::Text("Center lon: %.8f", centerLon);

    bool validLat = (centerLat >= -85.05112878 && centerLat <= 85.05112878);
    bool validLon = (centerLon >= -180.0 && centerLon <= 180.0);

    if (!validLat || !validLon) {
        ImGui::Text("Нет валидной геопозиции для карты");
        ImGui::End();
        return;
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();

    float plotWidth = avail.x;
    float plotHeight = avail.y - 10.0f;

    if (plotWidth < 300.0f) {
        plotWidth = 300.0f;
    }
    if (plotHeight < 300.0f) {
        plotHeight = 300.0f;
    }

    ImVec2 plotPixelSize(plotWidth, plotHeight);

    double plotW = static_cast<double>(plotWidth);
    double plotH = static_cast<double>(plotHeight);

    double plotCenterX = plotW / 2.0;
    double plotCenterY = plotH / 2.0;

    double centerTileX = lon_to_tile_x(centerLon, zoom);
    double centerTileY = lat_to_tile_y(centerLat, zoom);

    int centerX = floor_to_int(centerTileX);
    int centerY = floor_to_int(centerTileY);

    int tilesX = std::max(1, (int)std::ceil(plotW / 256.0) + 2);
    int tilesY = std::max(1, (int)std::ceil(plotH / 256.0) + 2);

    int halfX = tilesX / 2 + 1;
    int halfY = tilesY / 2 + 1;

    if (ImPlot::BeginPlot("##OsmPlot", plotPixelSize, ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes(
            nullptr,
            nullptr,
            ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoTickMarks | ImPlotAxisFlags_NoGridLines,
            ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoTickMarks | ImPlotAxisFlags_NoGridLines
        );

        ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, plotW, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, plotH, ImPlotCond_Always);

        for (int ty = centerY - halfY; ty <= centerY + halfY; ++ty) {
            for (int tx = centerX - halfX; tx <= centerX + halfX; ++tx) {
                TileTexture* tile = tileManager.get_or_load_tile(zoom, tx, ty);
                if (!tile || !tile->ready) {
                    continue;
                }

                double leftX = plotCenterX + (tx - centerTileX) * 256.0;
		double rightX = leftX + 256.0;

		// верхняя граница тайла
		double topY = plotCenterY + (centerTileY - ty) * 256.0;
		// нижняя граница тайла
		double bottomY = topY - 256.0;

                std::string label =
                    "tile_" + std::to_string(zoom) + "_" +
                    std::to_string(tx) + "_" +
                    std::to_string(ty);

                ImPlot::PlotImage(
    		    label.c_str(),
    		    (ImTextureID)(intptr_t)tile->texture_id,
    		    ImPlotPoint(leftX, bottomY),   // нижний левый
    		    ImPlotPoint(rightX, topY)      // верхний правый
                );
            }
        }

        double px[1] = { plotCenterX };
        double py[1] = { plotCenterY };
        ImPlot::PlotScatter("Current position", px, py, 1);

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

    GLFWwindow* window = glfwCreateWindow(1200, 850, "Backend Server: Location + Telemetry + OSM", nullptr, nullptr);
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
    (void)io;

    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        double lat, lon, alt, acc;
        std::int64_t tms, lastUpd;
        std::string provider, status, raw, cells, traffic, lastRadio;

        double lastSignalPower = 0.0;
        double lastSignalQuality = 0.0;
        double lastSignalNoise = 0.0;
        double lastAsu = 0.0;

        bool hasSignalPower = false;
        bool hasSignalQuality = false;
        bool hasSignalNoise = false;
        bool hasAsu = false;

        std::vector<double> x;
        std::map<int, std::vector<double>> rsrpByPci;
        std::map<int, std::vector<double>> rssiByPci;
        std::map<int, std::vector<double>> sinrByPci;

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
        }

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
        ImGui::InputTextMultiline(
            "##raw",
            raw.data(),
            raw.size() + 1,
            ImVec2(-1, 180),
            ImGuiInputTextFlags_ReadOnly
        );

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
        ImGui::InputTextMultiline(
            "##cells",
            cells.data(),
            cells.size() + 1,
            ImVec2(-1, 220),
            ImGuiInputTextFlags_ReadOnly
        );

        ImGui::End();

        ImGui::Begin("Traffic");

        ImGui::TextWrapped("Traffic JSON block:");
        ImGui::InputTextMultiline(
            "##traffic",
            traffic.data(),
            traffic.size() + 1,
            ImVec2(-1, 180),
            ImGuiInputTextFlags_ReadOnly
        );

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

        draw_osm_map_window(lat, lon);

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