#include "tile_manager.h"
#include "osm_math.h"
#include "curl_func.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <filesystem>
#include <thread>

TileManager::TileManager() {
}

TileManager::~TileManager() {
    clear();
}

void TileManager::clear() {
    std::lock_guard<std::mutex> lg(mtx);
    for (auto& [key, tex] : textures) {
        if (tex.texture_id != 0) {
            glDeleteTextures(1, &tex.texture_id);
            tex.texture_id = 0;
        }
    }
    textures.clear();
}

std::string TileManager::make_key(int z, int x, int y) const {
    return std::to_string(z) + "/" + std::to_string(x) + "/" + std::to_string(y);
}

int TileManager::active_downloads() const {
    std::lock_guard<std::mutex> lg(mtx);
    return (int)downloading.size();
}

void TileManager::request_tile_async(int z, int x, int y) {
    namespace fs = std::filesystem;

    x = wrap_tile_x(x, z);
    y = clamp_tile_y(y, z);

    const std::string key = make_key(z, x, y);
    const std::string path = make_tile_path(z, x, y);

    if (fs::exists(path)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lg(mtx);
        if (downloading.find(key) != downloading.end()) {
            return;
        }
        downloading.insert(key);
    }

    std::thread([this, z, x, y, key, path]() {
        std::string url = make_tile_url(z, x, y);
        download_file_with_curl(url, path);

        std::lock_guard<std::mutex> lg(mtx);
        downloading.erase(key);
    }).detach();
}

bool TileManager::ensure_tile_file_exists(int z, int x, int y, std::string& out_path) {
    namespace fs = std::filesystem;

    out_path = make_tile_path(z, x, y);

    if (fs::exists(out_path)) {
        return true;
    }

    request_tile_async(z, x, y);
    return false;
}

bool TileManager::load_png_as_texture(const std::string& path, TileTexture& tex) {
    int w = 0;
    int h = 0;
    int channels = 0;

    stbi_set_flip_vertically_on_load(false);
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
    if (!data) {
        return false;
    }

    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA,
        w,
        h,
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        data
    );

    stbi_image_free(data);

    tex.texture_id = id;
    tex.width = w;
    tex.height = h;
    tex.ready = true;

    return true;
}

TileTexture* TileManager::get_or_load_tile(int z, int x, int y) {
    x = wrap_tile_x(x, z);
    y = clamp_tile_y(y, z);

    const std::string key = make_key(z, x, y);

    {
        std::lock_guard<std::mutex> lg(mtx);
        auto it = textures.find(key);
        if (it != textures.end()) {
            return &it->second;
        }
    }

    std::string path;
    if (!ensure_tile_file_exists(z, x, y, path)) {
        return nullptr;
    }

    TileTexture tex;
    if (!load_png_as_texture(path, tex)) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lg(mtx);
    auto [it, inserted] = textures.emplace(key, tex);
    if (!inserted) {
        if (tex.texture_id != 0) {
            glDeleteTextures(1, &tex.texture_id);
        }
    }
    return &it->second;
}
