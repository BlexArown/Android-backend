cmake_minimum_required(VERSION 3.16)

project(server_app)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenGL REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(PostgreSQL REQUIRED)

pkg_check_modules(GLFW REQUIRED glfw3)
pkg_check_modules(ZMQ REQUIRED libzmq)
pkg_check_modules(CURL REQUIRED libcurl)

include_directories(
    ${OPENGL_INCLUDE_DIRS}
    ${GLFW_INCLUDE_DIRS}
    ${ZMQ_INCLUDE_DIRS}
    ${PostgreSQL_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}

    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/imgui
    ${CMAKE_SOURCE_DIR}/imgui/backends
    ${CMAKE_SOURCE_DIR}/implot
    ${CMAKE_SOURCE_DIR}/third_party
)

link_directories(
    ${GLFW_LIBRARY_DIRS}
    ${ZMQ_LIBRARY_DIRS}
    ${CURL_LIBRARY_DIRS}
)

add_executable(server_app
    main.cpp
    server_thread.cpp
    gui_thread.cpp

    osm_math.cpp
    curl_func.cpp
    tile_manager.cpp

    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_glfw.cpp
    imgui/backends/imgui_impl_opengl3.cpp

    implot/implot.cpp
    implot/implot_items.cpp
)

target_link_libraries(server_app
    PRIVATE
    ${OPENGL_LIBRARIES}
    ${GLFW_LIBRARIES}
    ${ZMQ_LIBRARIES}
    ${CURL_LIBRARIES}
    PostgreSQL::PostgreSQL
)
