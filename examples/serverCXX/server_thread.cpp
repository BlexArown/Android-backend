#include "server_thread.h"

#include <zmq.hpp>
#include <fstream>
#include <iostream>
#include <chrono>

static bool extract_number(const std::string& s, const std::string& key, double& out) {
    auto pos = s.find("\"" + key + "\"");
    if (pos == std::string::npos) return false;
    pos = s.find(":", pos);
    if (pos == std::string::npos) return false;
    pos++;

    while (pos < s.size() && (s[pos] == ' ')) pos++;

    size_t end = pos;
    while (end < s.size()) {
        char c = s[end];
        if ((c >= '0' && c <= '9') || c == '-' || c == '.' || c == 'e' || c == 'E' || c == '+') end++;
        else break;
    }
    if (end == pos) return false;

    try {
        out = std::stod(s.substr(pos, end - pos));
        return true;
    } catch (...) {
        return false;
    }
}

static bool extract_int64(const std::string& s, const std::string& key, std::int64_t& out) {
    double tmp = 0.0;
    if (!extract_number(s, key, tmp)) return false;
    out = static_cast<std::int64_t>(tmp);
    return true;
}

static bool extract_string(const std::string& s, const std::string& key, std::string& out) {
    auto pos = s.find("\"" + key + "\"");
    if (pos == std::string::npos) return false;
    pos = s.find(":", pos);
    if (pos == std::string::npos) return false;
    pos++;

    while (pos < s.size() && (s[pos] == ' ')) pos++;
    if (pos >= s.size() || s[pos] != '"') return false;
    pos++;

    size_t end = s.find("\"", pos);
    if (end == std::string::npos) return false;

    out = s.substr(pos, end - pos);
    return true;
}

static bool extract_json_block(const std::string& s, const std::string& key, std::string& out) {
    out.clear();

    auto pos = s.find("\"" + key + "\"");
    if (pos == std::string::npos) return false;

    pos = s.find(":", pos);
    if (pos == std::string::npos) return false;
    pos++;

    while (pos < s.size() && s[pos] == ' ') pos++;
    if (pos >= s.size()) return false;

    char open = s[pos];
    char close = (open == '[') ? ']' : (open == '{' ? '}' : '\0');
    if (close == '\0') return false;

    int depth = 0;
    size_t i = pos;

    for (; i < s.size(); i++) {
        if (s[i] == open) depth++;
        else if (s[i] == close) {
            depth--;
            if (depth == 0) { i++; break; }
        }
    }

    if (depth != 0) return false;

    out = s.substr(pos, i - pos);
    return true;
}

static std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

static void update_location_from_block(LocationShared* loc, const std::string& locBlock) {
    double lat=0, lon=0, alt=0, acc=0;
    std::int64_t tms=0;
    std::string provider="—";

    bool hasLat = extract_number(locBlock, "latitude", lat);
    bool hasLon = extract_number(locBlock, "longitude", lon);
    bool hasAlt = extract_number(locBlock, "altitude", alt);
    bool hasAcc = extract_number(locBlock, "accuracy", acc);
    bool hasTime = extract_int64(locBlock, "time", tms);
    extract_string(locBlock, "provider", provider);

    if (hasLat) loc->latitude = lat;
    if (hasLon) loc->longitude = lon;
    if (hasAlt) loc->altitude = alt;
    if (hasAcc) loc->accuracy = acc;
    if (hasTime) loc->time_ms = tms;
    if (!provider.empty()) loc->provider = provider;
}

void run_server(LocationShared* loc) {
    try {
        zmq::context_t ctx(1);
        zmq::socket_t sock(ctx, zmq::socket_type::rep);

        const std::string endpoint = "tcp://*:5555";
        sock.bind(endpoint);

        {
            std::lock_guard<std::mutex> lg(loc->mtx);
            loc->status = "server: bound " + endpoint;
        }

        std::ofstream logFile("location_log.jsonl", std::ios::app);

        while (true) {
            zmq::message_t req;
            auto ok = sock.recv(req, zmq::recv_flags::none);
            if (!ok) continue;

            std::string jsonStr(static_cast<char*>(req.data()), req.size());

            // log all incoming json lines
            logFile << jsonStr << "\n";
            logFile.flush();

            std::string type;
            extract_string(jsonStr, "type", type);

            {
                std::lock_guard<std::mutex> lg(loc->mtx);
                loc->last_raw_json = jsonStr;
                loc->last_update_unix_ms = now_ms();
            }

            if (type == "telemetry") {
                std::string locBlock, cellsBlock, trafficBlock;
                extract_json_block(jsonStr, "location", locBlock);
                extract_json_block(jsonStr, "cells", cellsBlock);
                extract_json_block(jsonStr, "traffic", trafficBlock);

                {
                    std::lock_guard<std::mutex> lg(loc->mtx);

                    if (!locBlock.empty()) {
                        update_location_from_block(loc, locBlock);
                    }
                    if (!cellsBlock.empty()) loc->cells_text = cellsBlock;
                    else loc->cells_text = "no cells";

                    if (!trafficBlock.empty()) loc->traffic_text = trafficBlock;
                    else loc->traffic_text = "no traffic";

                    loc->status = "server: telemetry received";
                }
            } else {
                double lat=0, lon=0, alt=0, acc=0;
                std::int64_t tms=0;
                std::string provider="—";

                bool hasLat = extract_number(jsonStr, "latitude", lat);
                bool hasLon = extract_number(jsonStr, "longitude", lon);
                bool hasAlt = extract_number(jsonStr, "altitude", alt);
                bool hasAcc = extract_number(jsonStr, "accuracy", acc);
                bool hasTime = extract_int64(jsonStr, "time", tms);
                extract_string(jsonStr, "provider", provider);

                {
                    std::lock_guard<std::mutex> lg(loc->mtx);

                    if (hasLat) loc->latitude = lat;
                    if (hasLon) loc->longitude = lon;
                    if (hasAlt) loc->altitude = alt;
                    if (hasAcc) loc->accuracy = acc;
                    if (hasTime) loc->time_ms = tms;
                    if (!provider.empty()) loc->provider = provider;

                    loc->status = "server: location received";
                }
            }

            // reply (REQ/REP rule)
            sock.send(zmq::buffer("OK", 2), zmq::send_flags::none);
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lg(loc->mtx);
        loc->status = std::string("server error: ") + e.what();
    }
}
