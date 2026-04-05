#include "osm_math.h"

#include <cmath>
#include <sstream>
#include <algorithm>

static const double PI = 3.14159265358979323846;
static const double RAD = PI / 180.0;
static const double DEG = 180.0 / PI;

double clamp_lat(double lat) {
    if (lat < -85.05112878) return -85.05112878;
    if (lat >  85.05112878) return  85.05112878;
    return lat;
}

double clamp_lon(double lon) {
    while (lon < -180.0) lon += 360.0;
    while (lon >  180.0) lon -= 360.0;
    return lon;
}

int tiles_per_axis(int zoom) {
    return 1 << zoom;
}

double lon_to_tile_x(double lon, int zoom) {
    lon = clamp_lon(lon);
    return (lon + 180.0) / 360.0 * tiles_per_axis(zoom);
}

double lat_to_tile_y(double lat, int zoom) {
    lat = clamp_lat(lat);
    double lat_rad = lat * RAD;
    return (1.0 - std::asinh(std::tan(lat_rad)) / PI) / 2.0 * tiles_per_axis(zoom);
}

double tile_x_to_lon(double x, int zoom) {
    return x / tiles_per_axis(zoom) * 360.0 - 180.0;
}

double tile_y_to_lat(double y, int zoom) {
    double n = PI - 2.0 * PI * y / tiles_per_axis(zoom);
    return DEG * std::atan(std::sinh(n));
}

int floor_to_int(double v) {
    return static_cast<int>(std::floor(v));
}

int wrap_tile_x(int x, int zoom) {
    int n = tiles_per_axis(zoom);
    if (n <= 0) return 0;
    x %= n;
    if (x < 0) x += n;
    return x;
}

int clamp_tile_y(int y, int zoom) {
    int n = tiles_per_axis(zoom);
    if (y < 0) return 0;
    if (y >= n) return n - 1;
    return y;
}

std::string make_tile_url(int z, int x, int y) {
    std::ostringstream ss;
    ss << "https://tile.openstreetmap.org/" << z << "/" << x << "/" << y << ".png";
    return ss.str();
}

std::string make_tile_path(int z, int x, int y) {
    std::ostringstream ss;
    ss << "build/" << z << "/" << x << "/" << y << ".png";
    return ss.str();
}
