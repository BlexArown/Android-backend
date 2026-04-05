#pragma once
#include <string>

double lon_to_tile_x(double lon, int zoom);
double lat_to_tile_y(double lat, int zoom);

double tile_x_to_lon(double x, int zoom);
double tile_y_to_lat(double y, int zoom);

int floor_to_int(double v);

double clamp_lat(double lat);
double clamp_lon(double lon);

int tiles_per_axis(int zoom);
int wrap_tile_x(int x, int zoom);
int clamp_tile_y(int y, int zoom);

std::string make_tile_url(int z, int x, int y);
std::string make_tile_path(int z, int x, int y);
