#pragma once
#include <string>
#include <map>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif
#include <GL/gl.h>

struct TileTexture {
    GLuint texture_id = 0;
    int width = 0;
    int height = 0;
    bool ready = false;
};

class TileManager {
public:
    TileManager();
    ~TileManager();

    TileTexture* get_or_load_tile(int z, int x, int y);
    void clear();

private:
    std::map<std::string, TileTexture> textures;
    std::mutex mtx;

    bool ensure_tile_file_exists(int z, int x, int y, std::string& out_path);
    bool load_png_as_texture(const std::string& path, TileTexture& tex);
    std::string make_key(int z, int x, int y) const;
};
