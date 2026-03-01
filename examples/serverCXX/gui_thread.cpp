#include "gui_thread.h"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>
#include <cstdio>
#include <string>

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

    GLFWwindow* window = glfwCreateWindow(1000, 650, "Backend Server: Location + Telemetry", nullptr, nullptr);
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
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("Location");

        double lat, lon, alt, acc;
        std::int64_t tms, lastUpd;
        std::string provider, status, raw;

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
        }

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
        ImGui::InputTextMultiline("##raw",
            raw.data(), raw.size() + 1,
            ImVec2(-1, 180),
            ImGuiInputTextFlags_ReadOnly);

        ImGui::End();

        ImGui::Begin("Cells");

        std::string cells;
        {
            std::lock_guard<std::mutex> lg(loc->mtx);
            cells = loc->cells_text;
        }

        ImGui::TextWrapped("Cells JSON block:");
        ImGui::InputTextMultiline("##cells",
            cells.data(), cells.size() + 1,
            ImVec2(-1, 250),
            ImGuiInputTextFlags_ReadOnly);

        ImGui::End();

        ImGui::Begin("Traffic");

        std::string traffic;
        {
            std::lock_guard<std::mutex> lg(loc->mtx);
            traffic = loc->traffic_text;
        }

        ImGui::TextWrapped("Traffic JSON block:");
        ImGui::InputTextMultiline("##traffic",
            traffic.data(), traffic.size() + 1,
            ImVec2(-1, 200),
            ImGuiInputTextFlags_ReadOnly);

        ImGui::End();

        // Render
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
}
