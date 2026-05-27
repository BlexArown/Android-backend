#pragma once
#include <string>
#include <map>
#include <set>
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

    // Быстрый вызов для GUI: если тайл уже есть на диске — грузит texture,
    // если его нет — запускает скачивание в фоне и сразу возвращает nullptr.
    TileTexture* get_or_load_tile(int z, int x, int y);

    // Можно вызывать заранее для видимых тайлов, чтобы быстрее поставить их в очередь.
    void request_tile_async(int z, int x, int y);

    int active_downloads() const;
    void clear();

private:
    std::map<std::string, TileTexture> textures;
    std::set<std::string> downloading;
    mutable std::mutex mtx;

    bool ensure_tile_file_exists(int z, int x, int y, std::string& out_path);
    bool load_png_as_texture(const std::string& path, TileTexture& tex);
    std::string make_key(int z, int x, int y) const;
};
