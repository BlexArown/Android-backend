#include "gui_thread.h"

#include <imgui.h>
#include <implot.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

static void glfw_error_callback(int error, const char* description) {
    std::fprintf(stderr, "GLFW Error %d: %s\n", error, description);
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

    GLFWwindow* window = glfwCreateWindow(1100, 800, "Backend Server: Location + Telemetry", nullptr, nullptr);
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

        std::vector<double> x, yPower, yQuality, yNoise, yAsu;

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
            yPower = loc->hist_signal_power;
            yQuality = loc->hist_signal_quality;
            yNoise = loc->hist_signal_noise;
            yAsu = loc->hist_asu;
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
        if (hasSignalPower)   ImGui::Text("Signal power   : %.2f", lastSignalPower);
        else                  ImGui::Text("Signal power   : no data");

        if (hasSignalQuality) ImGui::Text("Signal quality : %.2f", lastSignalQuality);
        else                  ImGui::Text("Signal quality : no data");

        if (hasSignalNoise)   ImGui::Text("Signal noise   : %.2f", lastSignalNoise);
        else                  ImGui::Text("Signal noise   : no data");

        if (hasAsu)           ImGui::Text("ASU            : %.2f", lastAsu);
        else                  ImGui::Text("ASU            : no data");

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
            if (ImPlot::BeginPlot("Signal Power / Quality / Noise", ImVec2(-1, 300))) {
                ImPlot::SetupAxes("Sample", "Value");
                if (!yPower.empty())
                    ImPlot::PlotLine("Power", x.data(), yPower.data(), (int)x.size());
                if (!yQuality.empty())
                    ImPlot::PlotLine("Quality", x.data(), yQuality.data(), (int)x.size());
                if (!yNoise.empty())
                    ImPlot::PlotLine("Noise/SINR", x.data(), yNoise.data(), (int)x.size());
                ImPlot::EndPlot();
            }

            if (ImPlot::BeginPlot("ASU", ImVec2(-1, 220))) {
                ImPlot::SetupAxes("Sample", "ASU");
                if (!yAsu.empty())
                    ImPlot::PlotLine("ASU", x.data(), yAsu.data(), (int)x.size());
                ImPlot::EndPlot();
            }
        } else {
            ImGui::Text("Пока нет данных для графиков.");
        }

        ImGui::End();

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