#include "server_thread.h"

#include <zmq.hpp>
#include <libpq-fe.h>

#include <fstream>
#include <iostream>
#include <chrono>
#include <mutex>
#include <vector>
#include <string>
#include <optional>
#include <cstdint>
#include <map>

static bool extract_number(const std::string& s, const std::string& key, double& out) {
    auto pos = s.find("\"" + key + "\"");
    if (pos == std::string::npos) return false;

    pos = s.find(":", pos);
    if (pos == std::string::npos) return false;
    pos++;

    while (pos < s.size() && s[pos] == ' ') pos++;

    size_t end = pos;
    while (end < s.size()) {
        char c = s[end];
        if ((c >= '0' && c <= '9') || c == '-' || c == '.' || c == 'e' || c == 'E' || c == '+') {
            end++;
        } else {
            break;
        }
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

static bool extract_int(const std::string& s, const std::string& key, int& out) {
    double tmp = 0.0;
    if (!extract_number(s, key, tmp)) return false;
    out = static_cast<int>(tmp);
    return true;
}

static bool extract_string(const std::string& s, const std::string& key, std::string& out) {
    auto pos = s.find("\"" + key + "\"");
    if (pos == std::string::npos) return false;

    pos = s.find(":", pos);
    if (pos == std::string::npos) return false;
    pos++;

    while (pos < s.size() && s[pos] == ' ') pos++;
    if (pos >= s.size() || s[pos] != '"') return false;
    pos++;

    std::string result;
    bool escape = false;

    for (size_t i = pos; i < s.size(); ++i) {
        char c = s[i];

        if (escape) {
            result.push_back(c);
            escape = false;
            continue;
        }

        if (c == '\\') {
            escape = true;
            continue;
        }

        if (c == '"') {
            out = result;
            return true;
        }

        result.push_back(c);
    }

    return false;
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
    bool inString = false;
    bool escape = false;
    size_t i = pos;

    for (; i < s.size(); ++i) {
        char c = s[i];

        if (escape) {
            escape = false;
            continue;
        }

        if (c == '\\') {
            escape = true;
            continue;
        }

        if (c == '"') {
            inString = !inString;
            continue;
        }

        if (inString) continue;

        if (c == open) {
            depth++;
        } else if (c == close) {
            depth--;
            if (depth == 0) {
                i++;
                break;
            }
        }
    }

    if (depth != 0) return false;

    out = s.substr(pos, i - pos);
    return true;
}

static std::vector<std::string> split_top_level_objects(const std::string& jsonArray) {
    std::vector<std::string> result;

    if (jsonArray.size() < 2 || jsonArray.front() != '[' || jsonArray.back() != ']') {
        return result;
    }

    int depth = 0;
    bool inString = false;
    bool escape = false;
    size_t objStart = std::string::npos;

    for (size_t i = 0; i < jsonArray.size(); ++i) {
        char c = jsonArray[i];

        if (escape) {
            escape = false;
            continue;
        }

        if (c == '\\') {
            escape = true;
            continue;
        }

        if (c == '"') {
            inString = !inString;
            continue;
        }

        if (inString) continue;

        if (c == '{') {
            if (depth == 0) objStart = i;
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0 && objStart != std::string::npos) {
                result.push_back(jsonArray.substr(objStart, i - objStart + 1));
                objStart = std::string::npos;
            }
        }
    }

    return result;
}

static std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

static void update_location_from_block(LocationShared* loc, const std::string& locBlock) {
    double lat = 0.0;
    double lon = 0.0;
    double alt = 0.0;
    double acc = 0.0;
    std::int64_t tms = 0;
    std::string provider = "—";

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

static bool is_valid_rsrp(double v) {
    return v > -200.0 && v < -20.0;
}

static bool is_valid_rssi(double v) {
    return v > -200.0 && v < -20.0;
}

static bool is_valid_sinr(double v) {
    return v > -50.0 && v < 100.0;
}

static std::map<int, CellSample> build_cells_by_pci(const std::string& cellsBlock) {
    std::map<int, CellSample> cellsByPci;

    if (cellsBlock.empty()) return cellsByPci;

    std::vector<std::string> cellObjects = split_top_level_objects(cellsBlock);

    for (const auto& cellJson : cellObjects) {
        int pci = -1;
        if (!extract_int(cellJson, "pci", pci)) {
            continue;
        }

        CellSample sample;
        double value = 0.0;

        if (extract_number(cellJson, "rsrp", value) && is_valid_rsrp(value)) {
            sample.has_rsrp = true;
            sample.rsrp = value;
        } else if (extract_number(cellJson, "ssRsrp", value) && is_valid_rsrp(value)) {
            sample.has_rsrp = true;
            sample.rsrp = value;
        } else if (extract_number(cellJson, "dbm", value) && is_valid_rsrp(value)) {
            sample.has_rsrp = true;
            sample.rsrp = value;
        }

        if (extract_number(cellJson, "rssi", value) && is_valid_rssi(value)) {
            sample.has_rssi = true;
            sample.rssi = value;
        } else if (extract_number(cellJson, "dbm", value) && is_valid_rssi(value)) {
            sample.has_rssi = true;
            sample.rssi = value;
        }

        if (extract_number(cellJson, "rssnr", value) && is_valid_sinr(value)) {
            sample.has_sinr = true;
            sample.sinr = value;
        } else if (extract_number(cellJson, "ssSinr", value) && is_valid_sinr(value)) {
            sample.has_sinr = true;
            sample.sinr = value;
        }

        cellsByPci[pci] = sample;
    }

    return cellsByPci;
}

static void update_last_signal_text(LocationShared* loc, const std::string& cellsBlock) {
    std::string radio = "unknown";
    extract_string(cellsBlock, "radio", radio);

    double power = 0.0;
    double quality = 0.0;
    double noise = 0.0;
    double asu = 0.0;

    bool hasPower = false;
    bool hasQuality = false;
    bool hasNoise = false;
    bool hasAsu = false;

    if (extract_number(cellsBlock, "rsrp", power) && is_valid_rsrp(power)) hasPower = true;
    if (extract_number(cellsBlock, "rsrq", quality) && quality > -100.0 && quality < 50.0) hasQuality = true;
    if (extract_number(cellsBlock, "rssnr", noise) && is_valid_sinr(noise)) hasNoise = true;
    if (extract_number(cellsBlock, "asuLevel", asu) && asu >= 0.0 && asu <= 100.0) hasAsu = true;

    if (!hasPower && extract_number(cellsBlock, "ssRsrp", power) && is_valid_rsrp(power)) hasPower = true;
    if (!hasQuality && extract_number(cellsBlock, "ssRsrq", quality) && quality > -100.0 && quality < 50.0) hasQuality = true;
    if (!hasNoise && extract_number(cellsBlock, "ssSinr", noise) && is_valid_sinr(noise)) hasNoise = true;

    if (!hasPower && extract_number(cellsBlock, "dbm", power) && is_valid_rsrp(power)) hasPower = true;
    if (!hasNoise && extract_number(cellsBlock, "rssi", noise) && is_valid_rssi(noise)) hasNoise = true;

    loc->last_radio = radio;
    loc->has_signal_power = hasPower;
    loc->has_signal_quality = hasQuality;
    loc->has_signal_noise = hasNoise;
    loc->has_asu = hasAsu;

    if (hasPower) loc->last_signal_power = power;
    if (hasQuality) loc->last_signal_quality = quality;
    if (hasNoise) loc->last_signal_noise = noise;
    if (hasAsu) loc->last_asu = asu;
}

static PGconn* connect_db() {
    const char* conninfo =
        "host=127.0.0.1 "
        "port=5432 "
        "dbname=drive_test_db "
        "user=postgres "
        "password=1234";

    return PQconnectdb(conninfo);
}

static void close_db(PGconn* conn) {
    if (conn != nullptr) {
        PQfinish(conn);
    }
}

static bool db_ok(PGconn* conn) {
    return conn != nullptr && PQstatus(conn) == CONNECTION_OK;
}

static const char* null_or_cstr(const std::optional<std::string>& v) {
    return v.has_value() ? v->c_str() : nullptr;
}

static std::optional<std::string> get_opt_string(const std::string& s, const std::string& key) {
    std::string v;
    if (extract_string(s, key, v)) return v;
    return std::nullopt;
}

static std::optional<std::string> get_opt_int64_str(const std::string& s, const std::string& key) {
    std::int64_t v = 0;
    if (extract_int64(s, key, v)) return std::to_string(v);
    return std::nullopt;
}

static std::optional<std::string> get_opt_int_str(const std::string& s, const std::string& key) {
    int v = 0;
    if (extract_int(s, key, v)) return std::to_string(v);
    return std::nullopt;
}

static std::optional<std::string> get_opt_double_str(const std::string& s, const std::string& key) {
    double v = 0.0;
    if (extract_number(s, key, v)) return std::to_string(v);
    return std::nullopt;
}

static long long insert_packet(
    PGconn* conn,
    const std::string& jsonStr,
    const std::string& locBlock,
    const std::string& trafficBlock
) {
    auto type = get_opt_string(jsonStr, "type");
    auto tsClientMs = get_opt_int64_str(jsonStr, "ts_client_ms");

    auto locationLatitude = get_opt_double_str(locBlock, "latitude");
    auto locationLongitude = get_opt_double_str(locBlock, "longitude");
    auto locationAltitude = get_opt_double_str(locBlock, "altitude");
    auto locationTime = get_opt_int64_str(locBlock, "time");
    auto locationAccuracy = get_opt_double_str(locBlock, "accuracy");
    auto locationProvider = get_opt_string(locBlock, "provider");
    auto locationStatus = get_opt_string(locBlock, "status");

    auto trafficTotalRx = get_opt_int64_str(trafficBlock, "total_rx_bytes");
    auto trafficTotalTx = get_opt_int64_str(trafficBlock, "total_tx_bytes");
    auto trafficTopAppsNote = get_opt_string(trafficBlock, "top_apps_note");

    const char* paramValues[13] = {
        null_or_cstr(type),
        null_or_cstr(tsClientMs),

        null_or_cstr(locationLatitude),
        null_or_cstr(locationLongitude),
        null_or_cstr(locationAltitude),
        null_or_cstr(locationTime),
        null_or_cstr(locationAccuracy),
        null_or_cstr(locationProvider),
        null_or_cstr(locationStatus),

        null_or_cstr(trafficTotalRx),
        null_or_cstr(trafficTotalTx),
        null_or_cstr(trafficTopAppsNote),

        jsonStr.c_str()
    };

    const char* sql =
        "INSERT INTO telemetry_packets ("
        "type, ts_client_ms, "
        "location_latitude, location_longitude, location_altitude, location_time, "
        "location_accuracy, location_provider, location_status, "
        "traffic_total_rx_bytes, traffic_total_tx_bytes, traffic_top_apps_note, "
        "raw_json"
        ") VALUES ("
        "$1, $2, "
        "$3, $4, $5, $6, "
        "$7, $8, $9, "
        "$10, $11, $12, "
        "$13"
        ") RETURNING id;";

    PGresult* res = PQexecParams(
        conn,
        sql,
        13,
        nullptr,
        paramValues,
        nullptr,
        nullptr,
        0
    );

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::cerr << "insert_packet failed: " << PQerrorMessage(conn) << std::endl;
        PQclear(res);
        return -1;
    }

    char* idText = PQgetvalue(res, 0, 0);
    long long packetId = -1;

    if (idText != nullptr) {
        try {
            packetId = std::stoll(idText);
        } catch (...) {
            packetId = -1;
        }
    }

    PQclear(res);
    return packetId;
}

static bool insert_cell(PGconn* conn, long long packetId, const std::string& cellJson) {
    std::string packetIdStr = std::to_string(packetId);

    auto radio = get_opt_string(cellJson, "radio");
    auto band = get_opt_string(cellJson, "band");

    auto ci = get_opt_int64_str(cellJson, "ci");
    auto earfcn = get_opt_int64_str(cellJson, "earfcn");
    auto pci = get_opt_int_str(cellJson, "pci");
    auto tac = get_opt_int_str(cellJson, "tac");
    auto mcc = get_opt_string(cellJson, "mcc");
    auto mnc = get_opt_string(cellJson, "mnc");
    auto asuLevel = get_opt_int_str(cellJson, "asuLevel");
    auto cqi = get_opt_int_str(cellJson, "cqi");
    auto rsrp = get_opt_int_str(cellJson, "rsrp");
    auto rsrq = get_opt_int_str(cellJson, "rsrq");
    auto rssi = get_opt_int_str(cellJson, "rssi");
    auto rssnr = get_opt_int_str(cellJson, "rssnr");
    auto timingAdvance = get_opt_int_str(cellJson, "timingAdvance");

    auto cid = get_opt_int64_str(cellJson, "cid");
    auto bsic = get_opt_int_str(cellJson, "bsic");
    auto arfcn = get_opt_int64_str(cellJson, "arfcn");
    auto lac = get_opt_int_str(cellJson, "lac");
    auto psc = get_opt_string(cellJson, "psc");
    auto dbm = get_opt_int_str(cellJson, "dbm");

    auto nci = get_opt_int64_str(cellJson, "nci");
    auto nrarfcn = get_opt_int64_str(cellJson, "nrarfcn");
    auto ssRsrp = get_opt_int_str(cellJson, "ssRsrp");
    auto ssRsrq = get_opt_int_str(cellJson, "ssRsrq");
    auto ssSinr = get_opt_int_str(cellJson, "ssSinr");

    const char* paramValues[28] = {
        packetIdStr.c_str(),

        null_or_cstr(radio),
        null_or_cstr(band),

        null_or_cstr(ci),
        null_or_cstr(earfcn),
        null_or_cstr(pci),
        null_or_cstr(tac),
        null_or_cstr(mcc),
        null_or_cstr(mnc),
        null_or_cstr(asuLevel),
        null_or_cstr(cqi),
        null_or_cstr(rsrp),
        null_or_cstr(rsrq),
        null_or_cstr(rssi),
        null_or_cstr(rssnr),
        null_or_cstr(timingAdvance),

        null_or_cstr(cid),
        null_or_cstr(bsic),
        null_or_cstr(arfcn),
        null_or_cstr(lac),
        null_or_cstr(psc),
        null_or_cstr(dbm),

        null_or_cstr(nci),
        null_or_cstr(nrarfcn),
        null_or_cstr(ssRsrp),
        null_or_cstr(ssRsrq),
        null_or_cstr(ssSinr),

        cellJson.c_str()
    };

    const char* sql =
        "INSERT INTO telemetry_cells ("
        "packet_id, "
        "radio, band, "
        "ci, earfcn, pci, tac, mcc, mnc, asu_level, cqi, rsrp, rsrq, rssi, rssnr, timing_advance, "
        "cid, bsic, arfcn, lac, psc, dbm, "
        "nci, nrarfcn, ss_rsrp, ss_rsrq, ss_sinr, "
        "raw_cell_json"
        ") VALUES ("
        "$1, "
        "$2, $3, "
        "$4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16, "
        "$17, $18, $19, $20, $21, $22, "
        "$23, $24, $25, $26, $27, "
        "$28"
        ");";

    PGresult* res = PQexecParams(
        conn,
        sql,
        28,
        nullptr,
        paramValues,
        nullptr,
        nullptr,
        0
    );

    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        std::cerr << "insert_cell failed: " << PQerrorMessage(conn) << std::endl;
    }

    PQclear(res);
    return ok;
}

void run_server(LocationShared* loc) {
    PGconn* db = nullptr;

    try {
        db = connect_db();

        if (!db_ok(db)) {
            std::lock_guard<std::mutex> lg(loc->mtx);
            loc->status = std::string("db error: ") + PQerrorMessage(db);
            close_db(db);
            return;
        }

        zmq::context_t ctx(1);
        zmq::socket_t sock(ctx, zmq::socket_type::rep);

        const std::string endpoint = "tcp://*:5555";
        sock.bind(endpoint);

        {
            std::lock_guard<std::mutex> lg(loc->mtx);
            loc->status = "server: db connected, bound " + endpoint;
        }

        std::ofstream logFile("location_log.jsonl", std::ios::app);

        while (true) {
            zmq::message_t req;
            auto ok = sock.recv(req, zmq::recv_flags::none);
            if (!ok) continue;

            std::string jsonStr(static_cast<char*>(req.data()), req.size());

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
                std::string locBlock;
                std::string cellsBlock;
                std::string trafficBlock;

                extract_json_block(jsonStr, "location", locBlock);
                extract_json_block(jsonStr, "cells", cellsBlock);
                extract_json_block(jsonStr, "traffic", trafficBlock);

                long long packetId = insert_packet(db, jsonStr, locBlock, trafficBlock);

                if (packetId > 0 && !cellsBlock.empty()) {
                    std::vector<std::string> cellObjects = split_top_level_objects(cellsBlock);
                    for (const auto& cellJson : cellObjects) {
                        insert_cell(db, packetId, cellJson);
                    }
                }

                {
                    std::lock_guard<std::mutex> lg(loc->mtx);

                    if (!locBlock.empty()) {
                        update_location_from_block(loc, locBlock);
                    }

                    if (!cellsBlock.empty()) {
                        loc->cells_text = cellsBlock;

                        update_last_signal_text(loc, cellsBlock);

                        std::map<int, CellSample> cellsByPci = build_cells_by_pci(cellsBlock);
                        if (!cellsByPci.empty()) {
                            loc->push_multi_pci_history(cellsByPci);
                        }
                    } else {
                        loc->cells_text = "no cells";
                    }

                    if (!trafficBlock.empty()) {
                        loc->traffic_text = trafficBlock;
                    } else {
                        loc->traffic_text = "no traffic";
                    }

                    if (packetId > 0) {
                        loc->status = "server: telemetry received + saved to db";
                    } else {
                        loc->status = "server: telemetry received, db insert failed";
                    }
                }
            } else {
                double lat = 0.0;
                double lon = 0.0;
                double alt = 0.0;
                double acc = 0.0;
                std::int64_t tms = 0;
                std::string provider = "—";

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

            sock.send(zmq::buffer("OK", 2), zmq::send_flags::none);
        }
    } catch (const std::exception& e) {
        {
            std::lock_guard<std::mutex> lg(loc->mtx);
            loc->status = std::string("server error: ") + e.what();
        }
        close_db(db);
    }
}