#include <iostream>
#include <algorithm>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstring>
#include <sstream>
#include <fstream>
#include <cctype>   // for std::toupper
#include <cmath>    // for sin, cos, atan2, sqrt
#include <iomanip>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
std::string airlinesOrderedByIataToJson(int limit, int offset);
std::string airportsOrderedByIataToJson(int limit, int offset);

struct Route {
    std::string airline;      // airline code (IATA or ICAO)
    int airlineId;            // OpenFlights airline ID
    std::string srcAirport;   // source airport code (IATA or ICAO)
    int srcAirportId;         // source airport ID
    std::string dstAirport;   // destination airport code
    int dstAirportId;         // destination airport ID
    bool codeshare;           // true if "Y"
    int stops;                // number of stops
    std::string equipment;    // space-separated plane types
};

// ---------- Helpers for parsing ----------
bool getCoordinatesForRoute(const Route &r,
                            double &srcLat, double &srcLon,
                            double &dstLat, double &dstLon);

double haversineMiles(double lat1, double lon1, double lat2, double lon2);

std::string jsonEscape(const std::string &s);
bool isNullField(const std::string &s) {
    return s.empty() || s == "\\N";
}

int parseIntOr(const std::string &s, int fallback) {
    if (isNullField(s)) return fallback;
    try {
        return std::stoi(s);
    } catch (...) {
        return fallback;
    }
}

double parseDoubleOr(const std::string &s, double fallback) {
    if (isNullField(s)) return fallback;
    try {
        return std::stod(s);
    } catch (...) {
        return fallback;
    }
}

// Simple CSV parser that respects double quotes
std::vector<std::string> parseCsvLine(const std::string &line) {
    std::vector<std::string> result;
    std::string current;
    bool inQuotes = false;
    
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') {
            inQuotes = !inQuotes;
        } else if (c == ',' && !inQuotes) {
            result.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    result.push_back(current); // last field
    return result;
}

// URL-decode a query value (handles %XX and + for spaces)
std::string urlDecode(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < s.size()) {
            auto hexToInt = [](char h) -> int {
                if (h >= '0' && h <= '9') return h - '0';
                if (h >= 'a' && h <= 'f') return 10 + (h - 'a');
                if (h >= 'A' && h <= 'F') return 10 + (h - 'A');
                return 0;
            };
            int hi = hexToInt(s[i + 1]);
            int lo = hexToInt(s[i + 2]);
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// Parse "key1=value1&key2=value2" into a map
std::unordered_map<std::string, std::string>
parseQueryString(const std::string &qs) {
    std::unordered_map<std::string, std::string> params;
    std::stringstream ss(qs);
    std::string pair;
    while (std::getline(ss, pair, '&')) {
        if (pair.empty()) continue;
        auto eqPos = pair.find('=');
        std::string key, value;
        if (eqPos == std::string::npos) {
            key = urlDecode(pair);
            value = "";
        } else {
            key = urlDecode(pair.substr(0, eqPos));
            value = urlDecode(pair.substr(eqPos + 1));
        }
        params[key] = value;
    }
    return params;
}

// Parse a non-negative integer query parameter, with a default.
// If the key is missing or the value is invalid/negative, return defaultVal.
int getIntQueryParam(const std::unordered_map<std::string, std::string> &params,
                     const std::string &key,
                     int defaultVal) {
    auto it = params.find(key);
    if (it == params.end() || it->second.empty()) {
        return defaultVal;
    }
    try {
        int value = std::stoi(it->second);
        if (value < 0) return defaultVal;
        return value;
    } catch (...) {
        return defaultVal;
    }
}

// ---------- Data structures ----------

struct Airport {
    int id;
    std::string name;
    std::string city;
    std::string country;
    std::string iata;   // 3-letter IATA code (may be empty / \N)
    std::string icao;   // 4-letter ICAO code (may be empty / \N)
    double latitude;
    double longitude;
    int altitude;       // feet
    double timezone;    // hours offset from UTC
    std::string dst;    // "E", "A", "S", "O", "Z", "N", "U"
    std::string tzdbTimezone;
    std::string type;   // "airport", "station", "port", "unknown"
    std::string source; // "OurAirports", "Legacy", "User"
};

struct Airline {
    int id;
    std::string name;
    std::string alias;
    std::string iata;     // 2-letter code (may be empty / \N)
    std::string icao;     // 3-letter code (may be empty / \N)
    std::string callsign;
    std::string country;
    bool active;          // true if "Y", false if "N" or unknown
};



// ---------- Global collections ----------

std::vector<Airport> g_airports;
std::unordered_map<std::string, const Airport*> g_airportsByIata;

std::vector<Airline> g_airlines;
std::unordered_map<std::string, const Airline*> g_airlinesByIata;

std::vector<Route> g_routes;

// 1. Map: Airport IATA/ICAO code -> List of routes originating at that airport
std::unordered_map<std::string, std::vector<const Route*>> g_routesBySrcAirport;

// 2. Map: Airport IATA/ICAA code -> List of routes terminating at that airport
std::unordered_map<std::string, std::vector<const Route*>> g_routesByDstAirport;

// 3. Map: OpenFlights Airline ID -> List of all routes flown by that airline
std::unordered_map<int, std::vector<const Route*>> g_routesByAirlineId;
// ---------- Loaders ----------

void loadAirports(const std::string &filename) {
    std::ifstream file(filename);
    if (!file) {
        std::cerr << "Error: could not open " << filename << "\n";
        return;
    }
    
    std::string line;
    int lineCount = 0;
    while (std::getline(file, line)) {
        ++lineCount;
        if (line.empty()) continue;
        
        auto fields = parseCsvLine(line);
        if (fields.size() < 14) {
            std::cerr << "Warning: airports line " << lineCount
            << " has too few fields (" << fields.size() << ")\n";
            continue;
        }
        
        Airport a;
        a.id            = parseIntOr(fields[0], -1);
        a.name          = fields[1];
        a.city          = fields[2];
        a.country       = fields[3];
        a.iata          = isNullField(fields[4]) ? "" : fields[4];
        a.icao          = isNullField(fields[5]) ? "" : fields[5];
        a.latitude      = parseDoubleOr(fields[6], 0.0);
        a.longitude     = parseDoubleOr(fields[7], 0.0);
        a.altitude      = parseIntOr(fields[8], 0);
        a.timezone      = parseDoubleOr(fields[9], 0.0);
        a.dst           = isNullField(fields[10]) ? "" : fields[10];
        a.tzdbTimezone  = isNullField(fields[11]) ? "" : fields[11];
        a.type          = (fields.size() > 12 && !isNullField(fields[12])) ? fields[12] : "";
        a.source        = (fields.size() > 13 && !isNullField(fields[13])) ? fields[13] : "";
        
        g_airports.push_back(a);
    }
    
    // Build IATA index AFTER vector is complete (pointers stay valid)
    for (const auto &a : g_airports) {
        if (!a.iata.empty()) {
            g_airportsByIata[a.iata] = &a;
        }
    }
    
    std::cout << "Loaded " << g_airports.size()
    << " airports from " << filename << "\n";
}

void loadAirlines(const std::string &filename) {
    std::ifstream file(filename);
    if (!file) {
        std::cerr << "Error: could not open " << filename << "\n";
        return;
    }
    
    std::string line;
    int lineCount = 0;
    while (std::getline(file, line)) {
        ++lineCount;
        if (line.empty()) continue;
        
        auto fields = parseCsvLine(line);
        if (fields.size() < 8) {
            std::cerr << "Warning: airlines line " << lineCount
            << " has too few fields (" << fields.size() << ")\n";
            continue;
        }
        
        Airline al;
        al.id        = parseIntOr(fields[0], -1);
        al.name      = fields[1];
        al.alias     = isNullField(fields[2]) ? "" : fields[2];
        al.iata      = isNullField(fields[3]) ? "" : fields[3];
        al.icao      = isNullField(fields[4]) ? "" : fields[4];
        al.callsign  = isNullField(fields[5]) ? "" : fields[5];
        al.country   = isNullField(fields[6]) ? "" : fields[6];
        al.active    = (!fields[7].empty() && fields[7][0] == 'Y');
        
        g_airlines.push_back(al);
    }
    
    for (const auto &al : g_airlines) {
        if (!al.iata.empty()) {
            g_airlinesByIata[al.iata] = &al;
        }
    }
    
    std::cout << "Loaded " << g_airlines.size()
    << " airlines from " << filename << "\n";
}

void loadRoutes(const std::string &filename) {
    std::ifstream file(filename);
    if (!file) {
        std::cerr << "Error: could not open " << filename << "\n";
        return;
    }
    
    std::string line;
    int lineCount = 0;
    while (std::getline(file, line)) {
        ++lineCount;
        if (line.empty()) continue;
        
        auto fields = parseCsvLine(line);
        if (fields.size() < 9) {
            std::cerr << "Warning: routes line " << lineCount
            << " has too few fields (" << fields.size() << ")\n";
            continue;
        }
        
        Route r;
        r.airline       = isNullField(fields[0]) ? "" : fields[0];
        r.airlineId     = parseIntOr(fields[1], -1);
        r.srcAirport    = isNullField(fields[2]) ? "" : fields[2];
        r.srcAirportId  = parseIntOr(fields[3], -1);
        r.dstAirport    = isNullField(fields[4]) ? "" : fields[4];
        r.dstAirportId  = parseIntOr(fields[5], -1);
        r.codeshare     = (!isNullField(fields[6]) && !fields[6].empty()
                           && fields[6][0] == 'Y');
        r.stops         = parseIntOr(fields[7], 0);
        r.equipment = isNullField(fields[8]) ? "" : fields[8];
        
        // strip any embedded CR/LF to keep JSON valid
        r.equipment.erase(
                          std::remove_if(r.equipment.begin(), r.equipment.end(),
                                         [](unsigned char ch) { return ch == '\r' || ch == '\n'; }),
                          r.equipment.end()
                          );
        g_routes.push_back(r);
    }
    
    std::cout << "Loaded " << g_routes.size()
    << " routes from " << filename << "\n";
    // ⚠️ DOUBLE-CHECK: Clear indices before populating them (prevents duplicates)
    g_routesBySrcAirport.clear();
    g_routesByDstAirport.clear();
    g_routesByAirlineId.clear();
    
    // --- NEW: Populate Indices ---
    for (const auto &r : g_routes) {
        // 1. Index by Source Airport Code (for departing flights)
        if (!r.srcAirport.empty() && !isNullField(r.srcAirport)) {
            g_routesBySrcAirport[r.srcAirport].push_back(&r);
        }
        
        // 2. Index by Destination Airport Code (for arriving flights)
        if (!r.dstAirport.empty() && !isNullField(r.dstAirport)) {
            g_routesByDstAirport[r.dstAirport].push_back(&r);
        }
        
        // 3. Index by Airline ID (Route::airlineId matches Airline::id)
        if (r.airlineId != -1) {
            g_routesByAirlineId[r.airlineId].push_back(&r);
        }
    }
    std::cout << "Built route indices for "
    << g_routesByAirlineId.size() << " airlines, "
    << g_routesBySrcAirport.size() << " source airports, and "
    << g_routesByDstAirport.size() << " destination airports.\n";
}


// ---------- HTTP helpers ----------

std::string buildHttpResponse(const std::string &body,
                              const std::string &contentType = "text/html; charset=UTF-8",
                              const std::string &statusLine = "HTTP/1.1 200 OK\r\n") {
    std::string response =
    statusLine +
    "Content-Type: " + contentType + "\r\n" +
    "Content-Length: " + std::to_string(body.size()) + "\r\n" +
    "Connection: close\r\n"
    "\r\n" +
    body;
    return response;
}

// Very simple JSON (no escaping of embedded quotes)
std::string airportToJson(const Airport &a) {
    std::ostringstream oss;
    oss << "{";
    oss << "\"id\":" << a.id << ",";
    oss << "\"name\":\""         << a.name         << "\",";
    oss << "\"city\":\""         << a.city         << "\",";
    oss << "\"country\":\""      << a.country      << "\",";
    oss << "\"iata\":\""         << a.iata         << "\",";
    oss << "\"icao\":\""         << a.icao         << "\",";
    oss << "\"latitude\":"       << a.latitude     << ",";
    oss << "\"longitude\":"      << a.longitude    << ",";
    oss << "\"altitude\":"       << a.altitude     << ",";
    oss << "\"timezone\":"       << a.timezone     << ",";
    oss << "\"dst\":\""          << a.dst          << "\",";
    oss << "\"tzdbTimezone\":\"" << a.tzdbTimezone << "\",";
    oss << "\"type\":\""         << a.type         << "\",";
    oss << "\"source\":\""       << a.source       << "\"";
    oss << "}";
    return oss.str();
}
// Return a small JSON array of the first N airports (or fewer if not enough data).
std::string airportsSampleToJson(std::size_t count) {
    std::ostringstream oss;
    oss << "[";
    bool first = true;
    
    std::size_t maxCount = std::min(count, g_airports.size());
    for (std::size_t i = 0; i < maxCount; ++i) {
        if (!first) oss << ",";
        first = false;
        oss << airportToJson(g_airports[i]);
    }
    
    oss << "]";
    return oss.str();
}
std::string airlineToJson(const Airline &al) {
    std::ostringstream oss;
    oss << "{";
    oss << "\"id\":"        << al.id       << ",";
    oss << "\"name\":\""    << al.name     << "\",";
    oss << "\"alias\":\""   << al.alias    << "\",";
    oss << "\"iata\":\""    << al.iata     << "\",";
    oss << "\"icao\":\""    << al.icao     << "\",";
    oss << "\"callsign\":\""<< al.callsign << "\",";
    oss << "\"country\":\""<< al.country  << "\",";
    oss << "\"active\":"    << (al.active ? "true" : "false");
    oss << "}";
    return oss.str();
}
// Return airlines with non-empty IATA codes, sorted by IATA, with pagination.
std::string airlinesOrderedByIataToJson(int limit, int offset) {
    std::vector<const Airline*> sorted;
    sorted.reserve(g_airlines.size());
    
    // Collect airlines that have a non-empty IATA code
    for (const auto &al : g_airlines) {
        if (!al.iata.empty()) {
            sorted.push_back(&al);
        }
    }
    
    // Sort by IATA ascending
    std::sort(sorted.begin(), sorted.end(),
              [](const Airline* a, const Airline* b) {
        return a->iata < b->iata;
    });
    
    // Apply pagination
    std::size_t start = static_cast<std::size_t>(offset);
    if (start > sorted.size()) start = sorted.size();
    
    std::size_t end;
    if (limit <= 0) {
        // No limit (shouldn’t normally happen with our parser, but be safe)
        end = sorted.size();
    } else {
        end = start + static_cast<std::size_t>(limit);
        if (end > sorted.size()) end = sorted.size();
    }
    
    std::ostringstream oss;
    oss << "[";
    bool first = true;
    for (std::size_t i = start; i < end; ++i) {
        if (!first) oss << ",";
        first = false;
        oss << airlineToJson(*sorted[i]);
    }
    oss << "]";
    return oss.str();
}

// Return airports with non-empty IATA codes, sorted by IATA, with pagination.
std::string airportsOrderedByIataToJson(int limit, int offset) {
    std::vector<const Airport*> sorted;
    sorted.reserve(g_airports.size());
    
    // Collect airports that have a non-empty IATA code
    for (const auto &a : g_airports) {
        if (!a.iata.empty()) {
            sorted.push_back(&a);
        }
    }
    
    // Sort by IATA ascending
    std::sort(sorted.begin(), sorted.end(),
              [](const Airport* a, const Airport* b) {
        return a->iata < b->iata;
    });
    
    // Apply pagination
    std::size_t start = static_cast<std::size_t>(offset);
    if (start > sorted.size()) start = sorted.size();
    
    std::size_t end;
    if (limit <= 0) {
        end = sorted.size();
    } else {
        end = start + static_cast<std::size_t>(limit);
        if (end > sorted.size()) end = sorted.size();
    }
    
    std::ostringstream oss;
    oss << "[";
    bool first = true;
    for (std::size_t i = start; i < end; ++i) {
        if (!first) oss << ",";
        first = false;
        oss << airportToJson(*sorted[i]);
    }
    oss << "]";
    return oss.str();
}
// Find an airline by its OpenFlights numeric ID (linear scan is fine here).
const Airline* findAirlineById(int id) {
    if (id < 0) return nullptr;
    for (const auto &al : g_airlines) {
        if (al.id == id) return &al;
    }
    return nullptr;
}

// Find an airline by code: prefer IATA, but also check ICAO when needed.
const Airline* findAirlineByCode(const std::string &code) {
    if (code.empty()) return nullptr;
    
    // First try IATA (we already have g_airlinesByIata)
    auto it = g_airlinesByIata.find(code);
    if (it != g_airlinesByIata.end()) {
        return it->second;
    }
    
    // If it's 3 chars, it might be an ICAO code
    if (code.size() == 3) {
        for (const auto &al : g_airlines) {
            if (al.icao == code) {
                return &al;
            }
        }
    }
    
    return nullptr;
}
// Report: for a given airline, count how many routes touch each airport
// (both src and dst), then sort airports by that count descending and
// return a JSON object:
//
// {
//   "airline": { ...full airline JSON... },
//   "airports": [
//     {
//       "airport": { ...full airport JSON... } OR { "iata": "XXX" },
//       "routes": <count>
//     },
//     ...
//   ]
// }


// Report: for a given airline, count how many routes touch each airport
// (both src and dst), then sort airports by that count descending and
// return a JSON object. This version uses the g_routesByAirlineId index for O(1) lookup.
std::string airlineRoutesReportToJson(const Airline &al) {
    
    // 1. Get the relevant routes using the index (O(1) lookup)
    std::vector<const Route*> routesToProcess;
    
    if (al.id != -1) {
        auto it = g_routesByAirlineId.find(al.id);
        if (it != g_routesByAirlineId.end()) {
            routesToProcess = it->second;
        }
    } else {
        // Fallback: If airline has no ID, we must skip. The linear scan fallback
        // is too slow and unnecessary for the primary use case.
        // We could also attempt a slow linear scan using IATA/ICAO code here,
        // but prefer index use for performance.
    }
    
    if (routesToProcess.empty()) {
        std::ostringstream oss;
        oss << "{ \"airline\":" << airlineToJson(al) << ", \"airports\":[] }";
        return oss.str();
    }
    
    // 2. Count how many times each airport code appears in the identified routes
    std::unordered_map<std::string, int> counts;
    
    for (const auto *rPtr : routesToProcess) {
        const auto &r = *rPtr;
        
        // Count both source and destination airports for this airline
        if (!r.srcAirport.empty() && !isNullField(r.srcAirport)) {
            counts[r.srcAirport] += 1;
        }
        if (!r.dstAirport.empty() && !isNullField(r.dstAirport)) {
            counts[r.dstAirport] += 1;
        }
    }
    
    // 3. Move counts into a vector and attach Airport* if we can resolve the code
    struct AirportCount {
        std::string code;
        int count;
        const Airport *airport; // may be nullptr if we can't resolve
    };
    
    std::vector<AirportCount> list;
    list.reserve(counts.size());
    
    for (const auto &kv : counts) {
        const std::string &code = kv.first;
        int count = kv.second;
        
        const Airport *ap = nullptr;
        auto itAp = g_airportsByIata.find(code);
        if (itAp != g_airportsByIata.end()) {
            ap = itAp->second;
        }
        
        list.push_back(AirportCount{code, count, ap});
    }
    
    // 4. Sort by route count descending, then by code ascending
    std::sort(list.begin(), list.end(),
              [](const AirportCount &a, const AirportCount &b) {
        if (a.count != b.count) return a.count > b.count;
        return a.code < b.code;
    });
    
    // 5. Build JSON object
    std::ostringstream oss;
    oss << "{";
    
    // Airline section
    oss << "\"airline\":" << airlineToJson(al) << ",";
    
    // Airports array
    oss << "\"airports\":[";
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (i > 0) oss << ",";
        const auto &ac = list[i];
        
        oss << "{";
        
        // "airport": full Airport JSON if we know it, otherwise minimal object
        oss << "\"airport\":";
        if (ac.airport) {
            oss << airportToJson(*ac.airport);
        } else {
            // We couldn't resolve by IATA—return minimal info
            oss << "{"
            << "\"iata\":\"" << ac.code << "\""
            << "}";
        }
        
        // "routes": count
        oss << ",\"routes\":" << ac.count;
        
        oss << "}";
    }
    oss << "]";
    
    oss << "}";
    return oss.str();
}


// Builds a GeoJSON FeatureCollection containing LineString features
// for the given list of routes, ensuring all string properties are escaped.
std::string routesToGeoJson(const std::vector<const Route*> &routes) {
    std::ostringstream oss;
    oss << "{";
    oss << "\"type\":\"FeatureCollection\",";
    oss << "\"features\":[";
    
    bool firstFeature = true;
    for (const auto *rPtr : routes) {
        const Route &r = *rPtr;
        double srcLat, srcLon, dstLat, dstLon;
        
        // Skip routes where we can't resolve coordinates
        if (!getCoordinatesForRoute(r, srcLat, srcLon, dstLat, dstLon)) {
            continue;
        }
        
        // Resolve airline (needed for feature properties)
        const Airline *al = nullptr;
        if (r.airlineId != -1) {
            al = findAirlineById(r.airlineId);
        }
        
        if (!firstFeature) oss << ",";
        firstFeature = false;
        
        // Start Feature object
        oss << "{";
        oss << "\"type\":\"Feature\",";
        
        // Geometry: LineString
        oss << "\"geometry\":{";
        oss << "\"type\":\"LineString\",";
        // GeoJSON standard: coordinates are [longitude, latitude]
        oss << "\"coordinates\":[";
        oss << "[" << srcLon << "," << srcLat << "],";
        oss << "[" << dstLon << "," << dstLat << "]";
        oss << "]"; // end coordinates
        oss << "},"; // end geometry
        
        // Properties: Metadata about the route
        oss << "\"properties\":{";
        
        // ⚠️ TIGHTEN UP: JSON escape all string properties for safety
        oss << "\"airline_code\":\""
        << jsonEscape(al ? al->iata : r.airline) << "\",";
        oss << "\"airline_name\":\""
        << jsonEscape(al ? al->name : std::string("Unknown")) << "\",";
        oss << "\"src_iata\":\""
        << jsonEscape(r.srcAirport) << "\",";
        oss << "\"dst_iata\":\""
        << jsonEscape(r.dstAirport) << "\",";
        
        oss << "\"codeshare\":" << (r.codeshare ? "true" : "false") << ",";
        oss << "\"stops\":" << r.stops << "";
        
        // Distance calculation and formatting
        double distance = haversineMiles(srcLat, srcLon, dstLat, dstLon);
        // Note: std::fixed and std::setprecision are included via <iomanip>
        oss << ",\"distance_mi\":" << std::fixed << std::setprecision(1) << distance;
        
        oss << "}"; // end properties
        
        oss << "}"; // end Feature object
    }
    
    oss << "]"; // end features array
    oss << "}"; // end FeatureCollection
    
    return oss.str();
}

// Report: for a given airport, count how many routes each airline operates
// to/from this airport, then sort airlines by that count descending and
// return a JSON object:
//
// {
//   "airport": { ...full airport JSON... },
//   "airlines": [
//     {
//       "airline": { ...full airline JSON... } OR { "code": "XX" },
//       "routes": <count>
//     },
//     ...
//   ]
// }
// Report: for a given airport, count how many routes each airline operates
// to/from this airport. This version uses the source/destination indices for O(1) lookup.
std::string airportAirlinesReportToJson(const Airport &ap) {
    // 1. Identify all routes originating from or terminating at this airport (O(1) lookup)
    
    std::vector<const Route*> routesToProcess;
    
    // Check Source Index (Departing routes)
    auto itSrc = g_routesBySrcAirport.find(ap.iata);
    if (itSrc != g_routesBySrcAirport.end()) {
        routesToProcess.insert(routesToProcess.end(), itSrc->second.begin(), itSrc->second.end());
    }
    
    // Check Destination Index (Arriving routes)
    auto itDst = g_routesByDstAirport.find(ap.iata);
    if (itDst != g_routesByDstAirport.end()) {
        routesToProcess.insert(routesToProcess.end(), itDst->second.begin(), itDst->second.end());
    }
    
    // 2. Count routes per airline.
    std::unordered_map<int, int> countsById;        // airlineId -> count
    std::unordered_map<std::string, int> tempCodeCounts; // code -> count (for routes with airlineId == -1)
    
    for (const auto *rPtr : routesToProcess) {
        const auto &r = *rPtr;
        
        // The check r.srcAirport == ap.iata is implicit because we used the indices.
        // We now just need to tally the airlines.
        
        if (r.airlineId != -1) {
            countsById[r.airlineId] += 1;
        } else if (!r.airline.empty() && !isNullField(r.airline)) {
            // Count by code when we have no numeric ID
            tempCodeCounts[r.airline] += 1;
        }
    }
    
    // 3. Try to fold code-based counts into ID-based counts when possible.
    std::unordered_map<std::string, int> unresolvedCodes; // codes we still couldn't resolve by ID
    
    for (const auto &kv : tempCodeCounts) {
        const std::string &code = kv.first;
        int count = kv.second;
        
        // We reuse findAirlineByCode (which may still be a slow linear scan on ICAO, but that's fine
        // since tempCodeCounts is small—only containing routes missing an ID).
        const Airline *al = findAirlineByCode(code);
        if (al && al->id != -1) {
            countsById[al->id] += count;
        } else {
            unresolvedCodes[code] += count;
        }
    }
    
    // 4. Build a list of airlines + counts.
    struct AirlineCount {
        const Airline *airline;   // may be nullptr if unknown
        std::string codeOrId;     // for display when airline == nullptr
        int count;
    };
    
    std::vector<AirlineCount> list;
    list.reserve(countsById.size() + unresolvedCodes.size());
    
    // From airline IDs:
    for (const auto &kv : countsById) {
        int airlineId = kv.first;
        int count = kv.second;
        
        const Airline *al = findAirlineById(airlineId);
        std::string codeOrId;
        if (al) {
            // Prefer IATA, then ICAO, then numeric id as string
            if (!al->iata.empty())      codeOrId = al->iata;
            else if (!al->icao.empty()) codeOrId = al->icao;
            else                        codeOrId = std::to_string(airlineId);
        } else {
            codeOrId = std::to_string(airlineId);
        }
        
        list.push_back(AirlineCount{al, codeOrId, count});
    }
    
    // From unresolved codes:
    for (const auto &kv : unresolvedCodes) {
        const std::string &code = kv.first;
        int count = kv.second;
        
        const Airline *al = findAirlineByCode(code); // might still be null
        list.push_back(AirlineCount{al, code, count});
    }
    
    // 5. Sort by route count descending, then by airline name or codeOrId.
    std::sort(list.begin(), list.end(),
              [](const AirlineCount &a, const AirlineCount &b) {
        if (a.count != b.count) return a.count > b.count;
        
        // Tie-breaker: airline name if available, otherwise code/id
        std::string nameA = a.airline ? a.airline->name : a.codeOrId;
        std::string nameB = b.airline ? b.airline->name : b.codeOrId;
        return nameA < nameB;
    });
    
    // 6. Build JSON.
    std::ostringstream oss;
    oss << "{";
    
    // Airport section
    oss << "\"airport\":" << airportToJson(ap) << ",";
    
    // Airlines array
    oss << "\"airlines\":[";
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (i > 0) oss << ",";
        const auto &ac = list[i];
        
        oss << "{";
        
        // "airline": full Airline JSON if known, otherwise minimal object
        oss << "\"airline\":";
        if (ac.airline) {
            oss << airlineToJson(*ac.airline);
        } else {
            oss << "{"
            << "\"code\":\"" << ac.codeOrId << "\""
            << "}";
        }
        
        // "routes": count
        oss << ",\"routes\":" << ac.count;
        
        oss << "}";
    }
    oss << "]";
    
    oss << "}";
    return oss.str();
}
// Build JSON for all direct flights between two airports.
// routes: list of Route* that match src -> dst with stops == 0
std::string directFlightsToJson(const Airport &srcAp,
                                const Airport &dstAp,
                                const std::vector<const Route*> &routes)
{
    std::ostringstream oss;
    oss << "{";
    
    // Source and destination airport objects
    oss << "\"source\":" << airportToJson(srcAp) << ",";
    oss << "\"destination\":" << airportToJson(dstAp) << ",";
    
    // Direct flights array
    oss << "\"direct_flights\":[";
    for (std::size_t i = 0; i < routes.size(); ++i) {
        if (i > 0) oss << ",";
        const Route *r = routes[i];
        
        // Resolve airline for this route
        const Airline *al = nullptr;
        if (r->airlineId != -1) {
            al = findAirlineById(r->airlineId);
        }
        if (!al && !r->airline.empty() && !isNullField(r->airline)) {
            al = findAirlineByCode(r->airline);
        }
        
        oss << "{";
        
        // Airline info (full if known, minimal if not)
        oss << "\"airline\":";
        if (al) {
            oss << airlineToJson(*al);
        } else {
            oss << "{"
            << "\"code\":\"" << r->airline << "\""
            << "}";
        }
        
        // Basic route details
        oss << ",\"codeshare\":" << (r->codeshare ? "true" : "false");
        oss << ",\"stops\":" << r->stops; // should be 0 by design
        oss << ",\"equipment\":\"" << r->equipment << "\"";
        
        oss << "}";
    }
    oss << "]";
    
    oss << "}";
    return oss.str();
}

// Lookup helper for airports by IATA code.
const Airport* findAirportByIata(const std::string &code) {
    auto it = g_airportsByIata.find(code);
    if (it == g_airportsByIata.end()) return nullptr;
    return it->second;
}

// ---------- Update helpers ----------

// Rebuild airport IATA index after inserts / deletes / modifications.
void rebuildAirportIndex() {
    g_airportsByIata.clear();
    for (const auto &a : g_airports) {
        if (!a.iata.empty()) {
            g_airportsByIata[a.iata] = &a;
        }
    }
}

// Rebuild airline IATA index after inserts / deletes / modifications.
void rebuildAirlineIndex() {
    g_airlinesByIata.clear();
    for (const auto &al : g_airlines) {
        if (!al.iata.empty()) {
            g_airlinesByIata[al.iata] = &al;
        }
    }
}

// Rebuild all route indices after any change to g_routes.
void rebuildRouteIndices() {
    g_routesBySrcAirport.clear();
    g_routesByDstAirport.clear();
    g_routesByAirlineId.clear();

    for (const auto &r : g_routes) {
        if (!r.srcAirport.empty() && !isNullField(r.srcAirport)) {
            g_routesBySrcAirport[r.srcAirport].push_back(&r);
        }
        if (!r.dstAirport.empty() && !isNullField(r.dstAirport)) {
            g_routesByDstAirport[r.dstAirport].push_back(&r);
        }
        if (r.airlineId != -1) {
            g_routesByAirlineId[r.airlineId].push_back(&r);
        }
    }
}

// Small helpers to find mutable indices by id.
int findAirportIndexById(int id) {
    for (std::size_t i = 0; i < g_airports.size(); ++i) {
        if (g_airports[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

int findAirlineIndexById(int id) {
    for (std::size_t i = 0; i < g_airlines.size(); ++i) {
        if (g_airlines[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

// Find route by (airline code, src, dst).
// We keep this simple: use airline code (IATA/ICAO) + src/dst codes.
int findRouteIndexByTriple(const std::string &airlineCode,
                           const std::string &src,
                           const std::string &dst) {
    for (std::size_t i = 0; i < g_routes.size(); ++i) {
        const Route &r = g_routes[i];
        if (!airlineCode.empty() &&
            !isNullField(r.airline) &&
            r.airline == airlineCode &&
            r.srcAirport == src &&
            r.dstAirport == dst) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::string routeToJson(const Route &r) {
    std::ostringstream oss;
    oss << "{";
    oss << "\"airline\":\""     << r.airline      << "\",";
    oss << "\"airlineId\":"     << r.airlineId    << ",";
    oss << "\"srcAirport\":\""  << r.srcAirport   << "\",";
    oss << "\"srcAirportId\":"  << r.srcAirportId << ",";
    oss << "\"dstAirport\":\""  << r.dstAirport   << "\",";
    oss << "\"dstAirportId\":"  << r.dstAirportId << ",";
    oss << "\"codeshare\":"     << (r.codeshare ? "true" : "false") << ",";
    oss << "\"stops\":"         << r.stops        << ",";
    oss << "\"equipment\":\""   << r.equipment    << "\"";
    oss << "}";
    return oss.str();
}

std::string buildUpdateResponse(const std::string &entityType,
                                const std::string &operation,
                                bool success,
                                const std::string &beforeJson,
                                const std::string &afterJson,
                                const std::string &errorMsg) {
    std::ostringstream oss;
    oss << "{";
    oss << "\"success\":" << (success ? "true" : "false") << ",";
    oss << "\"entityType\":\"" << entityType << "\",";
    oss << "\"operation\":\""  << operation  << "\",";
    oss << "\"before\":" << (beforeJson.empty() ? "null" : beforeJson) << ",";
    oss << "\"after\":"  << (afterJson.empty()  ? "null" : afterJson)  << ",";
    if (!errorMsg.empty()) {
        oss << "\"error\":\"" << errorMsg << "\"";
    } else {
        oss << "\"error\":null";
    }
    oss << "}";
    return oss.str();
}

// Normalize a code (IATA / airline, airport) to upper-case.
std::string upperCopy(const std::string &s) {
    std::string out = s;
    for (char &c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}
// Helper function to get the coordinates (lon, lat) of a route's endpoints.
// Returns true on success, false if either airport code is unresolved.
bool getCoordinatesForRoute(const Route &r,
                            double &srcLat, double &srcLon,
                            double &dstLat, double &dstLon) {
    
    const Airport *srcAp = findAirportByIata(r.srcAirport);
    const Airport *dstAp = findAirportByIata(r.dstAirport);
    
    if (!srcAp || !dstAp) {
        return false;
    }
    
    srcLat = srcAp->latitude;
    srcLon = srcAp->longitude;
    dstLat = dstAp->latitude;
    dstLon = dstAp->longitude;
    
    return true;
}

// Haversine distance in miles between two lat/lon points (degrees).
double haversineMiles(double lat1, double lon1, double lat2, double lon2) {
    const double kEarthRadiusMiles = 3958.8; // standard Earth radius in miles
    const double degToRad = 3.14159265358979323846 / 180.0;
    
    double rlat1 = lat1 * degToRad;
    double rlat2 = lat2 * degToRad;
    double dlat  = (lat2 - lat1) * degToRad;
    double dlon  = (lon2 - lon1) * degToRad;
    
    double sinHalfDlat = std::sin(dlat * 0.5);
    double sinHalfDlon = std::sin(dlon * 0.5);
    
    double a = sinHalfDlat * sinHalfDlat +
    std::cos(rlat1) * std::cos(rlat2) *
    sinHalfDlon * sinHalfDlon;
    
    double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
    return kEarthRadiusMiles * c;
}

// One-hop route info container used for JSON output.
struct OneHopRoute {
    const Route   *firstLeg;   // S -> X
    const Route   *secondLeg;  // X -> D
    const Airport *viaAirport; // X
    double         totalMiles;
    bool           sameAirline;  // NEW: true if both legs are operated by same airline
    bool           hasCodeshare; // NEW: true if either leg is a codeshare
};

// Build JSON for one-hop routes between two airports.
// Build JSON for one-hop routes with pagination metadata.
// Build JSON for one-hop routes with pagination metadata.
std::string oneHopRoutesToJson(const Airport &srcAp,
                               const Airport &dstAp,
                               const std::vector<OneHopRoute> &pairs,
                               std::size_t totalCount,
                               int limit,
                               int offset)
{
    std::ostringstream oss;
    
    // Helper to serialize a single leg (route + airline info).
    auto legToJson = [&](const Route &r) {
        const Airline *al = nullptr;
        
        if (r.airlineId != -1) {
            al = findAirlineById(r.airlineId);
        }
        if (!al && !r.airline.empty() && !isNullField(r.airline)) {
            al = findAirlineByCode(r.airline);
        }
        
        oss << "{";
        oss << "\"airline\":";
        if (al) {
            oss << airlineToJson(*al);
        } else {
            // Minimal object if airline cannot be resolved
            oss << "{"
            << "\"code\":\"" << r.airline << "\""
            << "}";
        }
        oss << ",\"codeshare\":" << (r.codeshare ? "true" : "false");
        oss << ",\"stops\":" << r.stops;  // should be 0 for our one-hop legs
        oss << ",\"equipment\":\"" << r.equipment << "\"";
        oss << "}";
    };
    
    oss << "{";
    
    // Top-level metadata
    oss << "\"source\":" << airportToJson(srcAp) << ",";
    oss << "\"destination\":" << airportToJson(dstAp) << ",";
    oss << "\"total_routes\":" << totalCount << ",";
    oss << "\"limit\":" << limit << ",";
    oss << "\"offset\":" << offset << ",";
    
    // One-hop routes
    oss << "\"one_hop_routes\":[";
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        if (i > 0) oss << ",";
        const OneHopRoute &hop = pairs[i];
        
        oss << "{";
        // via airport
        oss << "\"via\":" << airportToJson(*hop.viaAirport) << ",";
        
        // first leg
        oss << "\"first_leg\":";
        legToJson(*hop.firstLeg);
        oss << ",";
        
        // second leg
        oss << "\"second_leg\":";
        legToJson(*hop.secondLeg);
        oss << ",";
        
        // total distance + NEW fields
        oss << "\"total_distance_miles\":" << hop.totalMiles << ",";
        oss << "\"same_airline\":" << (hop.sameAirline ? "true" : "false") << ",";
        oss << "\"has_codeshare\":" << (hop.hasCodeshare ? "true" : "false");
        
        oss << "}";
    }
    oss << "]";
    
    oss << "}";
    return oss.str();
}

// --- Small JSON escape helper just for strings in search results ---
std::string jsonEscape(const std::string &s) {
    std::ostringstream oss;
    for (unsigned char c : s) {
        switch (c) {
            case '\"': oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\b': oss << "\\b";  break;
            case '\f': oss << "\\f";  break;
            case '\n': oss << "\\n";  break;
            case '\r': oss << "\\r";  break;
            case '\t': oss << "\\t";  break;
            default:
                if (c < 0x20) {
                    oss << "\\u"
                    << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(c);
                } else {
                    oss << c;
                }
        }
    }
    return oss.str();
}

// --- Airports autocomplete search: by IATA, city, or name ---
// --- Airports autocomplete search: by IATA, city, or name ---
// Now with ranking so good matches (SAN, SFO, SJC) come first.
// --- Airports autocomplete search: by IATA, city, or name, with rankings ---
//  - Prefer large hubs (type == "large_airport")
//  - Prefer US airports a bit (you can tweak or remove this if you like)
//  - Prefer city/name that STARTS with the query
//  - Prefer exact/starts-with IATA matches

std::string airportsSearchToJson(const std::string &query, int limit) {
    std::string q = query;
    // trim leading/trailing spaces
    auto isSpace = [](unsigned char ch){ return std::isspace(ch); };
    while (!q.empty() && isSpace((unsigned char)q.front())) q.erase(q.begin());
    while (!q.empty() && isSpace((unsigned char)q.back()))  q.pop_back();
    
    if (q.empty()) {
        return "[]";
    }
    
    // lowercase copy of query
    std::transform(q.begin(), q.end(), q.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    
    struct Candidate {
        const Airport* ap;
        int score;
        std::string display;
    };
    
    std::vector<Candidate> candidates;
    candidates.reserve(64);
    
    for (const auto &ap : g_airports) {
        if (ap.iata.empty()) continue; // only real IATA airports
        
        // lowercase fields for comparison
        std::string iata = ap.iata;
        std::string city = ap.city;
        std::string name = ap.name;
        std::string country = ap.country;
        std::string type = ap.type;
        
        auto lowerInPlace = [](std::string &s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        };
        lowerInPlace(iata);
        lowerInPlace(city);
        lowerInPlace(name);
        lowerInPlace(country);
        lowerInPlace(type);
        
        int score = 0;
        
        // ---- Matching on IATA code ----
        if (iata == q) {
            // Exact code match: user probably typed the code
            score += 200;
        } else if (iata.size() >= q.size() && iata.compare(0, q.size(), q) == 0) {
            // IATA starts with query
            score += 120;
        } else if (iata.find(q) != std::string::npos) {
            // IATA contains query somewhere
            score += 60;
        }
        
        // ---- Matching on city name ----
        if (!city.empty()) {
            if (city == q) {
                score += 180;
            } else if (city.size() >= q.size() && city.compare(0, q.size(), q) == 0) {
                // City starts with query: big boost for "San Diego", "San Francisco", etc.
                score += 140;
            } else if (city.find(q) != std::string::npos) {
                score += 70;
            }
        }
        
        // ---- Matching on airport name (usually "San Diego International Airport") ----
        if (!name.empty()) {
            if (name.size() >= q.size() && name.compare(0, q.size(), q) == 0) {
                score += 60;
            } else if (name.find(q) != std::string::npos) {
                score += 40;
            }
        }
        
        // If there was no textual match at all, skip
        if (score == 0) {
            continue;
        }
        
        // ---- Hub / size weighting ----
        // These use the OurAirports "type" field.
        if (type == "large_airport") {
            score += 100;   // big hub bonus
        } else if (type == "medium_airport") {
            score += 40;
        } else if (type == "small_airport") {
            score += 5;
        } else {
            // heliport / closed / etc. get nothing extra
        }
        
        // ---- Country bias (optional, tweakable) ----
        // Slight preference for US airports since that's your main use case.
        if (country == "united states" || country == "united states of america") {
            score += 20;
        }
        
        // Build display string: "City, Country (IATA)"
        std::string display = ap.city;
        if (!ap.country.empty()) {
            if (!display.empty()) display += ", ";
            display += ap.country;
        }
        if (!ap.iata.empty()) {
            if (!display.empty()) display += " (";
            else display += "(";
            display += ap.iata;
            display += ")";
        }
        
        candidates.push_back(Candidate{&ap, score, display});
    }
    
    // Nothing matched
    if (candidates.empty()) {
        return "[]";
    }
    
    // Sort by score DESC, then by display ASC as a tie-breaker
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b) {
        if (a.score != b.score) return a.score > b.score;
        return a.display < b.display;
    });
    
    // Apply limit
    if (limit <= 0) limit = 10;
    if ((int)candidates.size() > limit) {
        candidates.resize(limit);
    }
    
    // Emit JSON
    std::ostringstream oss;
    oss << "[";
    bool firstOut = true;
    for (const auto &cand : candidates) {
        const Airport &ap = *cand.ap;
        if (!firstOut) oss << ",";
        firstOut = false;
        
        oss << "{"
        << "\"code\":\""    << ap.iata << "\","
        << "\"name\":\""    << jsonEscape(ap.name) << "\","
        << "\"city\":\""    << jsonEscape(ap.city) << "\","
        << "\"country\":\"" << jsonEscape(ap.country) << "\","
        << "\"display\":\"" << jsonEscape(cand.display) << "\""
        << "}";
    }
    oss << "]";
    return oss.str();
}
std::string airlinesSearchToJson(const std::string &query, int limit) {
    std::string q = query;

    auto isSpace = [](unsigned char ch){ return std::isspace(ch); };
    while (!q.empty() && isSpace((unsigned char)q.front())) q.erase(q.begin());
    while (!q.empty() && isSpace((unsigned char)q.back()))  q.pop_back();

    if (q.empty()) {
        return "[]";
    }

    std::transform(q.begin(), q.end(), q.begin(),
                   [](unsigned char ch){ return static_cast<char>(std::tolower(ch)); });

    struct Candidate {
        const Airline* al;
        int score;
        std::string display;
    };

    std::vector<Candidate> candidates;
    candidates.reserve(64);

    for (const auto &al : g_airlines) {
        if (al.iata.empty() && al.icao.empty() && al.name.empty()) continue;

        std::string iata    = al.iata;
        std::string icao    = al.icao;
        std::string name    = al.name;
        std::string country = al.country;

        auto lowerInPlace = [](std::string &s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char ch){ return static_cast<char>(std::tolower(ch)); });
        };
        lowerInPlace(iata);
        lowerInPlace(icao);
        lowerInPlace(name);
        lowerInPlace(country);

        int score = 0;

        // IATA / ICAO exact / prefix / contains
        if (!iata.empty()) {
            if (iata == q)                       score += 200;
            else if (iata.rfind(q, 0) == 0)      score += 140;
            else if (iata.find(q) != std::string::npos) score += 70;
        }
        if (!icao.empty()) {
            if (icao == q)                       score += 150;
            else if (icao.rfind(q, 0) == 0)      score += 90;
            else if (icao.find(q) != std::string::npos) score += 40;
        }

        // Name & country
        if (!name.empty()) {
            if (name.rfind(q, 0) == 0)           score += 120;
            else if (name.find(q) != std::string::npos) score += 60;
        }
        if (!country.empty()) {
            if (country.rfind(q, 0) == 0)        score += 40;
            else if (country.find(q) != std::string::npos) score += 20;
        }

        if (score == 0) continue;

        std::string code = !al.iata.empty() ? al.iata : al.icao;
        std::string display = al.name;
        if (!code.empty()) {
            if (!display.empty()) display += " (" + code + ")";
            else display = code;
        }

        candidates.push_back(Candidate{&al, score, display});
    }

    if (candidates.empty()) {
        return "[]";
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b) {
                  if (a.score != b.score) return a.score > b.score;
                  return a.display < b.display;
              });

    if (limit <= 0) limit = 10;
    if ((int)candidates.size() > limit) {
        candidates.resize(limit);
    }

    std::ostringstream oss;
    oss << "[";
    bool firstOut = true;
    for (const auto &cand : candidates) {
        const Airline &al = *cand.al;
        std::string code = !al.iata.empty() ? al.iata : al.icao;
        if (!firstOut) oss << ",";
        firstOut = false;
        oss << "{"
            << "\"code\":\""    << jsonEscape(code)      << "\","
            << "\"name\":\""    << jsonEscape(al.name)   << "\","
            << "\"country\":\"" << jsonEscape(al.country)<< "\","
            << "\"display\":\"" << jsonEscape(cand.display) << "\""
            << "}";
    }
    oss << "]";
    return oss.str();
}
// ---------- Main server ----------

int main() {
    std::cerr << "=== starting OpenFlights server main() ===\n";
    // Load all OpenFlights data at startup
    loadAirports("airports.dat");
    loadAirlines("airlines.dat");
    loadRoutes("routes.dat");
    
    std::cout << "Summary: "
    << g_airports.size() << " airports, "
    << g_airlines.size() << " airlines, "
    << g_routes.size()   << " routes loaded.\n";
    
    // Create TCP socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        std::cerr << "Error: cannot create socket\n";
        return 1;
    }
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8080);
    
    if (bind(server_fd, (sockaddr *) &addr, sizeof(addr)) < 0) {
        std::cerr << "Error: bind failed (is port 8080 already in use?)\n";
        close(server_fd);
        return 1;
    }
    
    if (listen(server_fd, 10) < 0) {
        std::cerr << "Error: listen failed\n";
        close(server_fd);
        return 1;
    }
    
    std::cout << "Server listening on http://localhost:8080\n";
    
    while (true) {
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (sockaddr *) &client_addr, &client_len);
        if (client_fd < 0) {
            std::cerr << "Error: accept failed\n";
            continue;
        }
        
        char buffer[4096];
        ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
        if (bytes_read <= 0) {
            close(client_fd);
            continue;
        }
        buffer[bytes_read] = '\0';
        
        std::string request(buffer);
        std::cout << "Received request:\n" << request << "\n";
        
        // Parse the first line: "GET /path?query HTTP/1.1"
        std::string requestLine = request.substr(0, request.find("\r\n"));
        std::string method, path, version;
        std::istringstream iss(requestLine);
        iss >> method >> path >> version;
        
        std::cout << "Method: " << method << ", Raw Path: " << path << "\n";
        
        // Split path into pathOnly and queryString
        std::string pathOnly = path;
        std::string queryString;
        auto qpos = path.find('?');
        if (qpos != std::string::npos) {
            pathOnly = path.substr(0, qpos);
            queryString = path.substr(qpos + 1);
        }
        
        std::cout << "Path only: " << pathOnly
        << ", Query: " << queryString << "\n";
        
        std::string response;
        
        if (pathOnly == "/" || pathOnly == "/index.html") {
            std::string body = R"HTML(
        <!DOCTYPE html>
                <html lang="en">
                <head>
                    <meta charset="UTF-8" />
                    <title>OpenFlights Route Finder</title>
                    <meta name="viewport" content="width=device-width, initial-scale=1" />
                     <!-- NEW: Mapbox CSS + JS -->
                        <link href="https://api.mapbox.com/mapbox-gl-js/v3.4.0/mapbox-gl.css" rel="stylesheet" />
                        <script src="https://api.mapbox.com/mapbox-gl-js/v3.4.0/mapbox-gl.js"></script>

                    <style>
                        body {
                            font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
                            margin: 0;
                            padding: 1.5rem;
                            background: #f5f5f5;
                            color: #222;
                        }
                        .container {
                            max-width: 880px;
                            margin: 0 auto;
                            background: #ffffff;
                            border-radius: 12px;
                            padding: 1.5rem 1.75rem 1.75rem;
                            box-shadow: 0 2px 8px rgba(0, 0, 0, 0.08);
                        }
                .header-bar {
                    display: flex;
                    justify-content: space-between;
                    align-items: flex-start;
                    gap: 1rem;
                    margin-bottom: 0.75rem;
                }
                
                .header-left h1 {
                    margin: 0;
                    font-size: 1.7rem;
                }
                
                .subtitle {
                    font-size: 0.85rem;
                    color: #6b7280;
                    margin-top: 0.15rem;
                }
                
                .header-right {
                    display: flex;
                    align-items: center;
                    justify-content: flex-end;
                }

                /* NEW: pill-style nav links (Airline/Airport/Update/About) */
                .header-nav-pill {
                    font-size: 0.8rem;
                    text-decoration: none;
                    padding: 0.35rem 0.9rem;
                    border-radius: 999px;
                    border: 1px solid #e5e7eb;
                    background: #f9fafb;
                    color: #111827;
                    display: inline-flex;
                    flex-direction: column;
                    align-items: center;
                    justify-content: center;
                    font-weight: 600;     /* bold like the Student tab */
                    line-height: 1.1;
                    text-align: center;
                    white-space: normal;  /* allow wrapping to two lines */
                }

                .header-nav-pill:hover {
                    background: #f3f4f6;
                }
                
                .top-tabs {
                    display: inline-flex;
                    background: #e5e7eb;
                    border-radius: 999px;
                    padding: 0.1rem;
                }
                .top-tab {
                    border: none;
                    background: transparent;
                    cursor: pointer;
                    font-size: 0.8rem;
                    font-weight: 600;
                    padding: 0.25rem 0.8rem;
                    border-radius: 999px;
                    color: #4b5563;
                    white-space: nowrap;
                }
                
                .top-tab.active {
                    background: #111827;
                    color: #f9fafb;
                }
                
                .id-panel {
                    margin-bottom: 0.75rem;
                    display: none; /* default hidden; JS turns it on */
                }
                
                .id-panel-status {
                    font-size: 0.8rem;
                    color: #6b7280;
                    margin-bottom: 0.25rem;
                }
                
                .id-panel-body {
                    display: inline-block;
                    padding: 0.6rem 0.8rem;
                    border-radius: 0.5rem;
                    background: #f9fafb;
                    border: 1px solid #e5e7eb;
                    font-size: 0.85rem;
                    color: #111827;
                }
                        h1 {
                            margin-top: 0;
                            font-size: 1.7rem;
                        }
                        form {
                            display: grid;
                            gap: 0.75rem;
                            margin-bottom: 1rem;
                        }
                        .field-group {
                            display: flex;
                            flex-wrap: wrap;
                            gap: 0.75rem;
                        }
                        .field {
                            display: flex;
                            flex-direction: column;
                            min-width: 120px;
                            flex: 1 1 0;
                        }
                        label {
                            font-size: 0.8rem;
                            font-weight: 600;
                            margin-bottom: 0.25rem;
                            text-transform: uppercase;
                            letter-spacing: 0.06em;
                        }
                        input[type="text"],
                        input[type="number"] {
                            padding: 0.4rem 0.5rem;
                            border-radius: 6px;
                            border: 1px solid #d1d5db;
                            font-size: 0.95rem;
                        }
                        input[type="text"]::placeholder {
                            text-transform: none;
                        }
                        .checkbox-row {
                            display: flex;
                            align-items: center;
                            gap: 0.4rem;
                            margin-top: 0.35rem;
                        }
                        .button-row {
                            display: flex;
                            flex-wrap: wrap;
                            gap: 0.5rem;
                            margin-top: 0.35rem;
                        }
                        .btn {
                            padding: 0.45rem 0.9rem;
                            border-radius: 999px;
                            border: none;
                            font-size: 0.95rem;
                            font-weight: 600;
                            cursor: pointer;
                            display: inline-flex;
                            align-items: center;
                            gap: 0.25rem;
                        }
                        .btn-primary {
                            background: #2563eb;
                            color: #ffffff;
                        }
                        .btn-primary:hover {
                            background: #1d4ed8;
                        }
                        .btn-secondary {
                            background: #059669;
                            color: #ffffff;
                        }
                        .btn-secondary:hover {
                            background: #047857;
                        }
                        #error {
                            margin-top: 0.25rem;
                            color: #b91c1c;
                            font-size: 0.9rem;
                        }
                        .hint {
                            font-size: 0.85rem;
                            color: #6b7280;
                            margin-top: 0.25rem;
                        }
                
                        /* Results layout (cards) */
                        h3 {
                            margin-top: 1.25rem;
                            margin-bottom: 0.5rem;
                            font-size: 1.05rem;
                        }
                        .results-list {
                            margin-top: 0.5rem;
                            display: flex;
                            flex-direction: column;
                            gap: 0.75rem;
                        }
                        .route-card {
                            background: #ffffff;
                            border-radius: 12px;
                            padding: 0.75rem 1rem;
                            box-shadow: 0 1px 4px rgba(15, 23, 42, 0.08);
                            font-size: 0.93rem;
                        }
                        .route-main-line {
                            display: flex;
                            align-items: center;
                            gap: 0.35rem;
                            font-weight: 600;
                            margin-bottom: 0.25rem;
                        }
                        .route-city {
                            font-size: 0.96rem;
                        }
                        .route-arrow {
                            font-size: 0.85rem;
                            color: #6b7280;
                        }
                        .route-tag {
                            font-size: 0.72rem;
                            font-weight: 600;
                            padding: 0.1rem 0.55rem;
                            border-radius: 999px;
                            margin-left: 0.2rem;
                            white-space: nowrap;
                        }
                        .route-tag.nonstop {
                            background: #e6f6ea;
                            color: #137333;
                        }
                        .route-tag.onehop {
                            background: #e8f0fe;
                            color: #174ea6;
                        }
                        .route-tag.codeshare {
                            background: #fef7e0;
                            color: #915b00;
                        }
                        .route-tag.same-airline {
                            background: #e0f2fe;
                            color: #075985;
                        }
                        .route-tag.mixed-airlines {
                            background: #fef2f2;
                            color: #b91c1c;
                        }
                        .route-sub-line {
                            font-size: 0.83rem;
                            color: #4b5563;
                            display: flex;
                            flex-wrap: wrap;
                            gap: 0.4rem;
                        }
                        .route-distance {
                            margin-left: auto;
                            font-weight: 500;
                            font-size: 0.85rem;
                            color: #111827;
                        }
                .view-mode-tabs {
                  display: inline-flex;
                  gap: 0.25rem;
                  margin-top: 0.25rem;
                  margin-bottom: 0.4rem;
                  font-size: 0.8rem;
                }

                .view-tab {
                    border: none;
                    cursor: pointer;
                    background: #e5e7eb;
                    color: #111827;
                    padding: 0.15rem 0.7rem;
                    border-radius: 999px;
                    font-weight: 500;
                }
                
                .view-tab:hover {
                    background: #d1d5db;
                }
                
                .view-tab.active {
                    background: #111827;
                    color: #f9fafb;
                }
                
                /* Autocomplete dropdown */
                .autocomplete-container {
                  position: relative;
                }
                
                .autocomplete-list {
                  position: absolute;
                  top: 100%;
                  left: 0;
                  right: 0;
                  z-index: 20;
                  max-height: 14rem;
                  overflow-y: auto;
                  margin-top: 0.25rem;
                  background: #ffffff;
                  border-radius: 0.5rem;
                  border: 1px solid #e5e7eb;
                  box-shadow: 0 10px 25px rgba(15,23,42,0.15);
                }
                
                .autocomplete-item {
                  padding: 0.4rem 0.6rem;
                  font-size: 0.9rem;
                  cursor: pointer;
                }
                
                .autocomplete-item:hover,
                .autocomplete-item.active {
                  background: #f3f4f6;
                }
                
                .autocomplete-empty {
                  padding: 0.4rem 0.6rem;
                  font-size: 0.85rem;
                  color: #6b7280;
                }
                
                /* Shaded airline pill (same as airline-ui) */
                .airline-pill {
                  display: inline-flex;
                  align-items: center;
                  background: #f1f5f9;  /* light gray */
                  color: #1e293b;       /* dark slate */
                  padding: 0.15rem 0.6rem 0.15rem 0.25rem;
                  border-radius: 999px;
                  font-size: 0.78rem;
                  font-weight: 500;
                  margin-right: 0.4rem;
                  white-space: nowrap;
                  line-height: 1;
                }
                .airline-pill img {
                    height: 1.25rem;
                    width: 1.25rem;
                    border-radius: 50%;
                    object-fit: contain;
                    margin-right: 0.35rem;
                    background: #ffffff;
                    border: 1px solid #e2e8f0;
                    flex-shrink: 0;
                }
                        .no-results {
                            font-size: 0.9rem;
                            color: #6b7280;
                        }
                
                
                
                .autocomplete-container {
                                    position: relative;
                                }
                                .autocomplete-list {
                                    position: absolute;
                                    top: 100%;
                                    left: 0;
                                    right: 0;
                                    z-index: 20;
                                    max-height: 14rem;
                                    overflow-y: auto;
                                    margin-top: 0.25rem;
                                    background: #ffffff;
                                    border-radius: 0.5rem;
                                    border: 1px solid #e5e7eb;
                                    box-shadow: 0 10px 25px rgba(15,23,42,0.15);
                                }
                                .autocomplete-item {
                                    padding: 0.4rem 0.6rem;
                                    font-size: 0.9rem;
                                    cursor: pointer;
                                }
                                .autocomplete-item:hover,
                                .autocomplete-item.active {
                                    background: #f3f4f6;
                                }
                                .autocomplete-empty {
                                    padding: 0.4rem 0.6rem;
                                    font-size: 0.85rem;
                                    color: #6b7280;
                                }
                
                
                /* Bar that holds filter pills */
                .airline-filter-bar {
                    display: flex;
                    flex-wrap: wrap;
                    gap: 0.4rem;
                    margin-bottom: 0.5rem;
                }
                .layover-filter-bar {
                  display: flex;
                  flex-wrap: wrap;
                  gap: 0.4rem;
                  margin-bottom: 0.75rem;
                  align-items: center;
                }

                .layover-pill {
                  border-radius: 999px;
                  padding: 0.15rem 0.55rem;
                  border: 1px solid #ddd;
                  font-size: 0.78rem;
                  background: #f8f8f8;
                  cursor: pointer;
                }

                .layover-pill.active {
                  background: #2563eb;
                  border-color: #2563eb;
                  color: #fff;
                }

                .layover-pill .pill-count {
                  font-size: 0.68rem;
                  opacity: 0.8;
                  margin-left: 0.25rem;
                }
                /* Small clickable filter pills (AA, UA, DL, etc.) */
                .filter-pill {
                    border: none;
                    cursor: pointer;
                    background: #e5e7eb;
                    color: #111827;
                    font-size: 0.78rem;
                    font-weight: 500;
                    border-radius: 999px;
                    padding: 0.15rem 0.7rem;
                    display: inline-flex;
                    align-items: center;
                    gap: 0.3rem;
                    white-space: nowrap;
                }
                
                .filter-pill:hover {
                    background: #d1d5db;
                }
                
                .filter-pill.active {
                    background: #111827;
                    color: #f9fafb;
                }
                /* “Clear filters” pill-style button */
                .clear-filters-btn {
                  border-radius: 999px;
                  border: 1px solid #d1d5db;
                  background: #f9fafb;
                  color: #374151;
                  font-size: 0.78rem;
                  font-weight: 500;
                  padding: 0.2rem 0.9rem;
                  cursor: pointer;
                  white-space: nowrap;
                  box-shadow: 0 1px 2px rgba(15, 23, 42, 0.05);
                }

                .clear-filters-btn:hover {
                  background: #e5e7eb;
                }
                /* Scrollable code / JSON box (matches the JSON style you liked) */
                .scroll-pre {
                  background: #020617;          /* same dark background */
                  color: #e5e7eb;
                  padding: 0.75rem;
                  border-radius: 0.5rem;
                  font-size: 0.8rem;
                  font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas,
                               "Liberation Mono", "Courier New", monospace;
                  max-height: 24rem;
                  overflow: auto;
                  white-space: pre;
                }
                
                /* Code panel container (for the Get Code section) */
                .code-panel {
                  margin-top: 0.75rem;
                  border-radius: 0.75rem;
                  background: #0b1120;
                  border: 1px solid #1e293b;
                  padding: 0.75rem;
                }
                
                .code-panel .panel-status {
                  font-size: 0.8rem;
                  color: #94a3b8;
                  margin-bottom: 0.5rem;
                }
                #routeMap {
                  height: 320px;
                  margin-top: 1rem;
                  margin-bottom: 1rem;
                  border-radius: 8px;
                }
                /* Add space between the map (with its attribution text) and the chips below */
                #routeMap {
                  margin-bottom: 1.5rem;   /* tweak 1.0–2.0rem to taste */
                }
                #homeRouteMap {
                                    height: 400px;
                                    margin-top: 1rem;
                                    margin-bottom: 1rem;
                                    border-radius: 8px;
                                }
                                /* NEW: Added hover effect for clickable cards */
                                .route-card.clickable:hover {
                                    box-shadow: 0 4px 12px rgba(15, 23, 42, 0.15);
                                    cursor: pointer;
                                    transform: translateY(-1px);
                                    transition: all 0.2s ease;
                                }
                    .route-card.selected {
                        border: 2px solid #2563eb;   /* same blue as the map highlight */
                        background: #eff6ff;         /* light blue */
                    }
                    </style>
                </head>
                <body>
                <div class="container">
                    <div class="header-bar">
                      <div class="header-left">
                        <h1>OpenFlights Route Finder</h1>
                        <p class="subtitle">
                          Example: SRC = <code>SFO</code>, DST = <code>JFK</code>.
                        </p>
                      </div>
                      <div class="header-right">
          <div class="top-tabs">
            <button type="button" class="top-tab active" id="studentTab">
              Student
            </button>
            <button type="button" class="top-tab" id="codeTab">
              Code
            </button>
          </div>

          <a href="/airline-ui"
             class="header-nav-pill"
             style="margin-left:0.5rem;">
            Airline<br>Routes
          </a>

          <a href="/airport-ui"
             class="header-nav-pill"
             style="margin-left:0.5rem;">
            Airport<br>Airlines
          </a>

          <a href="/update-ui"
             class="header-nav-pill"
             style="margin-left:0.5rem;">
            Update<br>Data
          </a>

          <a href="/about-ui"
             class="header-nav-pill"
             style="margin-left:0.5rem;">
            About this<br>Project
          </a>
        </div>
                    </div>
                
                    <!-- This panel appears under the tabs when "Student" is selected -->
                    <div id="idPanel" class="id-panel" style="display:none;">
                      <div id="idPanelStatus" class="id-panel-status"></div>
                      <div id="idPanelBody" class="id-panel-body"></div>
                    </div>
                    
                <!-- CODE PANEL (hidden by default, shows /code contents in scrollable box) -->
                
                    <div id="codePanel" class="code-panel" style="display:none">
                      <div id="codePanelStatus" class="panel-status"></div>
                      <pre id="codePanelBody" class="scroll-pre"></pre>
                    </div>
                    <form id="route-form">
                       <div class="field-group">
                                           <div class="field autocomplete-container">
                                               <label for="src">Source airport (IATA)</label>
                                               <input id="src" name="src" type="text" maxlength="40" placeholder="e.g. SFO or San Diego"
                                                      autocomplete="off" />
                                               <div id="src-suggestions" class="autocomplete-list" style="display:none;"></div>
                                           </div>
                                           <div class="field autocomplete-container">
                                               <label for="dst">Destination airport (IATA)</label>
                                               <input id="dst" name="dst" type="text" maxlength="40" placeholder="e.g. JFK or San Jose"
                                                      autocomplete="off" />
                                               <div id="dst-suggestions" class="autocomplete-list" style="display:none;"></div>
                                           </div>
                                       </div>
                
                        <div class="field-group">
                            <div class="field" style="max-width: 120px;">
                                <label for="limit">Limit</label>
                                <input id="limit" name="limit" type="number" min="1" value="20" />
                            </div>
                            <div class="field" style="max-width: 260px;">
                                <label>Options</label>
                                <div class="checkbox-row">
                                    <input id="recommended" name="recommended" type="checkbox" checked />
                                    <label for="recommended" style="margin:0;font-weight:400;text-transform:none;letter-spacing:0;">
                                        Recommended only (one-hop)
                                    </label>
                                </div>
                            </div>
                        </div>
                
                        <div class="button-row">
                            <button type="submit" class="btn btn-primary">
                                Search One-Hop Routes
                            </button>
                            <button type="button" id="direct-button" class="btn btn-secondary">
                                Search Non-Stop Routes
                            </button>
                        </div>
                
                        <div id="error"></div>
                        <div class="hint">
                            Example: SRC = <code>SFO</code>, DST = <code>JFK</code>.
                        </div>
                        
                    </form>
                
                    <h3>Results</h3>
                
                    <div id="homeRouteMap"></div>

                    <div style="display:flex;align-items:center;justify-content:space-between;margin-top:0.25rem;margin-bottom:0.4rem;">
                        <div class="view-mode-tabs" id="viewModeTabs">
                            <button class="view-tab active" data-mode="cards">Cards</button>
                            <button class="view-tab" data-mode="json">JSON</button>
                        </div>

                        <!-- NEW: Clear Filters button on the right -->
                        <button type="button" id="clearFiltersBtn" class="clear-filters-btn">
                            Clear filters
                        </button>
                    </div>

                  <div class="filters-row">
                    <div class="filters-left">
                      <div id="layoverFilters" class="layover-filter-bar"></div>
                      <!-- Airline filter pills live here -->
                      <div id="airlineFilters" class="airline-filter-bar"></div>
                    </div>
                  </div>
                
                    <div id="resultsList" class="results-list">
                        <p class="no-results">(no results yet)</p>
                    </div>
                
                <script>
                (function () {
                    const studentTab = document.getElementById("studentTab");
                    const codeTab    = document.getElementById("codeTab");
                
                    const idPanel        = document.getElementById("idPanel");
                    const idPanelStatus  = document.getElementById("idPanelStatus");
                    const idPanelBody    = document.getElementById("idPanelBody");
                
                    const codePanel       = document.getElementById("codePanel");
                    const codePanelStatus = document.getElementById("codePanelStatus");
                    const codePanelBody   = document.getElementById("codePanelBody");

                    let idLoaded    = false;
                    let studentOpen = false;
                    let codeLoaded  = false;
                    let codeOpen    = false;
                
                mapboxgl.accessToken = 'pk.eyJ1IjoiY2hyaXN0aW5lcnlhbjkzIiwiYSI6ImNtaWJnZDhiMDAxN2sya29sNXZvNHExMXkifQ.6K3wdjmIhFRQD3bre4f_DA';

                            const INITIAL_CENTER = [-98.5, 39.8]; // Center of the US (approx)
                            const INITIAL_ZOOM = 2; // World view
                            
                            // Map instance (container ID changed to homeRouteMap)
                            const homeMap = new mapboxgl.Map({
                                container: 'homeRouteMap',
                                style: 'mapbox://styles/mapbox/light-v11',
                                center: INITIAL_CENTER,
                                zoom: INITIAL_ZOOM
                            });
                            
                            let homeMapIsLoaded = false; 
                            let activeRouteId = null; // Stores the ID of the currently selected route
                            
                            homeMap.on('load', () => {
                                            homeMapIsLoaded = true;
                                            // Initialize the two layers we'll use: a base layer and a highlight layer.
                                            homeMap.addSource('route-display-source', {
                                                'type': 'geojson',
                                                'data': { type: 'FeatureCollection', features: [] }
                                            });

                                            // 1. Base Layer (Thin, low opacity)
                                            homeMap.addLayer({
                                                'id': 'routes-base-layer',
                                                'type': 'line',
                                                'source': 'route-display-source',
                                                'layout': { 'line-join': 'round', 'line-cap': 'round' },
                                                'paint': {
                                                    'line-color': '#9ca3af', // Gray
                                                    'line-width': 1.0,
                                                    'line-opacity': 0.6
                                                }
                                            });

                                            // 2. Highlight Layer (Thick, bright color, filtered to one route)
                                            homeMap.addLayer({
                                                'id': 'routes-highlight-layer',
                                                'type': 'line',
                                                'source': 'route-display-source',
                                                // CORRECTED FILTER SYNTAX: Filter on the 'id' property in features.properties
                                                'filter': ['==', ['get', 'id'], ''], // Start filtered (show nothing)
                                                'layout': { 'line-join': 'round', 'line-cap': 'round' },
                                                'paint': {
                                                    'line-color': '#2563eb', // Bright Blue
                                                    'line-width': 3.0,
                                                    'line-opacity': 1.0
                                                }
                                            });
                                        // 🔥 ADD THIS BLOCK ↓↓↓↓↓↓↓↓↓↓↓↓
                                            homeMap.on('click', 'routes-base-layer', (e) => {
                                                if (!e.features.length) return;

                                                const routeId = e.features[0].properties.id;

                                                highlightRoute(routeId);

                                                // Filter results to just this card
                                                focusedRouteId = routeId;
                                                applyRouteFocusToCards();
                                            });

                                            homeMap.on('mouseenter', 'routes-base-layer', () => {
                                                homeMap.getCanvas().style.cursor = 'pointer';
                                            });

                                            homeMap.on('mouseleave', 'routes-base-layer', () => {
                                                homeMap.getCanvas().style.cursor = '';
                                            });
                                            // 🔥 END INSERTED BLOCK
                                        });
                            // --- End Mapbox Setup ---
                /** Removes all data and highlights from the home map (safer implementation). */
                            function clearHomeMap() {
                                activeRouteId = null;
                                if (!homeMapIsLoaded) return;

                                const src = homeMap.getSource('route-display-source');
                                if (src) {
                                    // Set source data to empty
                                    src.setData({ type: 'FeatureCollection', features: [] });
                                }

                                // Set highlight filter to empty if the layer exists
                                if (homeMap.getLayer('routes-highlight-layer')) {
                                    homeMap.setFilter('routes-highlight-layer', ['==', ['get', 'id'], '']);
                                }
                                
                                homeMap.flyTo({ center: INITIAL_CENTER, zoom: INITIAL_ZOOM });
                            }

                            /** Updates the Mapbox highlight filter to show one route. */
                            function highlightRoute(routeId) {
                                activeRouteId = routeId;
                                if (!homeMapIsLoaded) return;
                                
                                // CORRECTED FILTER SYNTAX: Use ['get', 'id'] to filter on feature property
                                homeMap.setFilter('routes-highlight-layer', ['==', ['get', 'id'], routeId]);
                            }
                            

                            /**
                             * Renders routes on the map using the filtered array to ensure index alignment.
                             * @param {string} mode - 'direct' or 'onehop'
                             * @param {object} data - The full JSON response (for shared properties like source/destination)
                             * @param {Array} filteredRoutes - The routes array that is actually displayed as cards
                             */
                            function renderRoutesOnHomeMap(mode, data, filteredRoutes) {
                                clearHomeMap();
                                if (!homeMapIsLoaded) return;
                                
                                const features = [];
                                let bounds = new mapboxgl.LngLatBounds();
                                let hasBounds = false;

                                filteredRoutes.forEach((route, index) => {
                                    // Data check: Assumes data.source and data.destination are available.
                                    const src = data.source;
                                    const dst = data.destination;
                                    
                                    if (!src || !dst || src.longitude === undefined || dst.longitude === undefined) {
                                        return;
                                    }
                                    
                                    const id = mode + '-' + index;
                                    let coordinates;
                                    
                                    if (mode === 'direct') {
                                        // Direct flight: Source to Destination
                                        coordinates = [[src.longitude, src.latitude], [dst.longitude, dst.latitude]];
                                    } else { 
                                        // One-hop flight: Source to Via to Destination
                                        // Assumes 'route' (a hop object) contains a 'via' airport object
                                        const via = route.viaAirport || route.via; // Use route.via for consistency
                                        
                                        if (!via || via.longitude === undefined) {
                                             return; // Skip if via airport data is missing
                                        }
                                        
                                        coordinates = [
                                            [src.longitude, src.latitude],
                                            [via.longitude, via.latitude],
                                            [dst.longitude, dst.latitude]
                                        ];
                                    }

                                    coordinates.forEach(c => { bounds.extend(c); hasBounds = true; });

                                    features.push({
                                        'type': 'Feature',
                                        'geometry': {
                                            'type': 'LineString',
                                            'coordinates': coordinates
                                        },
                                        'properties': {
                                            'id': id,
                                            'route_id': id, // Redundant but harmless, using 'id' for the filter
                                        }
                                    });
                                });
                                
                                // Update the Mapbox source with all the features
                                homeMap.getSource('route-display-source').setData({
                                    type: 'FeatureCollection',
                                    features: features
                                });

                                // Fit map to bounds if we have routes
                                if (hasBounds) {
                                    homeMap.fitBounds(bounds, {
                                        padding: 80, 
                                        maxZoom: 6,
                                        duration: 1000
                                    });
                                }
                            }
                    function setActiveTopTab(which) {
                      // which: "student", "code", or null
                      if (which === "student") {
                        studentTab.classList.add("active");
                      } else {
                        studentTab.classList.remove("active");
                      }
                
                      if (which === "code") {
                        codeTab.classList.add("active");
                      } else {
                        codeTab.classList.remove("active");
                      }
                    }
                
                    // STUDENT tab: toggle open/closed
                    studentTab.addEventListener("click", () => {
                      if (studentOpen) {
                        // close student panel
                        studentOpen = false;
                        idPanel.style.display = "none";
                        setActiveTopTab(null);
                        return;
                      }
                
                      // open student panel, close code panel
                      studentOpen = true;
                      codeOpen    = false;
                      codePanel.style.display = "none";
                
                      idPanel.style.display = "block";
                      setActiveTopTab("student");
                
                      if (!idLoaded) {
                        idPanelStatus.textContent = "Loading…";
                        idPanelBody.innerHTML = "";
                        fetch("/id")
                          .then(resp => {
                            if (!resp.ok) throw new Error("HTTP " + resp.status);
                            return resp.json();
                          })
                          .then(data => {
                            idLoaded = true;
                            idPanelStatus.textContent = "";
                            idPanelBody.innerHTML =
                              "<strong>Name:</strong> " + (data.name || "?") + "<br/>" +
                              "<strong>Student ID:</strong> " + (data.student_id || "?");
                          })
                          .catch(err => {
                            console.error(err);
                            idPanelStatus.textContent = "Could not load student info.";
                          });
                      }
                    });
                
                    // CODE tab: toggle open/closed, show /code in scrollable <pre>
                    codeTab.addEventListener("click", () => {
                      if (codeOpen) {
                        // close code panel
                        codeOpen = false;
                        codePanel.style.display = "none";
                        setActiveTopTab(null);
                        return;
                      }
                
                      // open code panel, close student panel
                      codeOpen    = true;
                      studentOpen = false;
                      idPanel.style.display = "none";
                
                      codePanel.style.display = "block";
                      setActiveTopTab("code");
                
                      if (!codeLoaded) {
                        codePanelStatus.textContent = "Loading code…";
                        codePanelBody.textContent = "";
                
                        fetch("/code")
                          .then(resp => {
                            if (!resp.ok) throw new Error("HTTP " + resp.status);
                            return resp.text();
                          })
                          .then(text => {
                            codeLoaded = true;
                            codePanelStatus.textContent = "";
                            codePanelBody.textContent = text;  // goes into the <pre>
                          })
                          .catch(err => {
                            console.error(err);
                            codePanelStatus.textContent = "Could not load code.";
                          });
                      }
                    });
                    const form = document.getElementById("route-form");
                    const srcInput = document.getElementById("src");
                    const dstInput = document.getElementById("dst");
                    const srcSuggestions = document.getElementById("src-suggestions");
                    const dstSuggestions = document.getElementById("dst-suggestions");
                    const limitInput = document.getElementById("limit");
                    const recommendedInput = document.getElementById("recommended");
                    const directButton = document.getElementById("direct-button");
                    const errorEl = document.getElementById("error");
                    const resultsList = document.getElementById("resultsList");
                    const airlineFilters = document.getElementById("airlineFilters");
                    const viewModeTabs = document.getElementById("viewModeTabs");
                    const viewTabButtons = viewModeTabs.querySelectorAll(".view-tab");
                    const layoverFilters = document.getElementById("layoverFilters");
                    const clearFiltersBtn = document.getElementById("clearFiltersBtn");
                    
                // "cards" or "json"
                    let viewMode = "cards";
                
                    // --- Aircraft code → human-readable name mapping (common types) ---
                    const AIRCRAFT_NAMES = {
                        // Airbus
                        "318": "Airbus A318",
                        "319": "Airbus A319",
                        "320": "Airbus A320",
                        "321": "Airbus A321",
                        "332": "Airbus A330-200",
                        "333": "Airbus A330-300",
                        "388": "Airbus A380-800",
                
                        // Boeing narrow-body
                        "732": "Boeing 737-200",
                        "733": "Boeing 737-300",
                        "734": "Boeing 737-400",
                        "735": "Boeing 737-500",
                        "736": "Boeing 737-600",
                        "737": "Boeing 737-700",
                        "738": "Boeing 737-800",
                        "739": "Boeing 737-900",
                        "73G": "Boeing 737-700",
                        "73W": "Boeing 737-700 (winglets)",
                
                        // Boeing wide-body (some common ones)
                        "744": "Boeing 747-400",
                        "752": "Boeing 757-200",
                        "753": "Boeing 757-300",
                        "762": "Boeing 767-200",
                        "763": "Boeing 767-300",
                        "764": "Boeing 767-400",
                        "772": "Boeing 777-200",
                        "773": "Boeing 777-300",
                        "77W": "Boeing 777-300ER",
                        "788": "Boeing 787-8",
                        "789": "Boeing 787-9",
                        "78X": "Boeing 787-10",
                
                        // Regional jets / turboprops (a small sample)
                        "CR2": "Bombardier CRJ200",
                        "CR7": "Bombardier CRJ700",
                        "CR9": "Bombardier CRJ900",
                        "E70": "Embraer 170",
                        "E75": "Embraer 175",
                        "E90": "Embraer 190",
                        "DH3": "De Havilland Canada DHC-8-300",
                        "DH4": "De Havilland Canada DHC-8-400"
                        // anything not listed will just show its raw code
                    };
                
                    // Turn "738 320 319 739 73G" → "Boeing 737-800, Airbus A320, Airbus A319, Boeing 737-900, Boeing 737-700"
                    function formatEquipment(equipmentStr) {
                        if (!equipmentStr) return "";
                        const seen = new Set();
                        const labels = [];
                
                        equipmentStr.split(/\s+/).forEach(code => {
                            if (!code) return;
                            if (seen.has(code)) return;      // dedupe
                            seen.add(code);
                            const label = AIRCRAFT_NAMES[code] || code;  // fallback to raw code if unknown
                            labels.push(label);
                        });
                
                        return labels.join(", ");
                    }
                    // Remember last results so we can re-render when filter changes
                    let lastMode = null;          // "direct" | "onehop" | null
                    let lastDirectData = null;    // JSON from /direct
                    let lastOneHopData = null;    // JSON from /onehop
                    let activeAirlineFilter = null; // e.g. "AA" or null for "all"
                    let activeLayoverFilter = null;  // NEW: currently selected via airport, e.g. "DEN"
                    let focusedRouteId = null;    // e.g. "onehop-3" or "direct-0"
                
                    /** Normalize airline code from an airline object */
                    function getAirlineCodeFromObj(airlineObj) {
                        if (!airlineObj) return "";
                        const code = airlineObj.iata || airlineObj.icao || airlineObj.code || "";
                        return code ? code.toUpperCase() : "";
                    }
                
                    function setError(msg) {
                        errorEl.textContent = msg || "";
                    }
                function applyRouteFocusToCards() {
                    const cards = document.querySelectorAll('.route-card');
                    if (!cards.length) return;

                    cards.forEach(card => {
                        const isFocused = focusedRouteId && card.dataset.routeId === focusedRouteId;
                        if (!focusedRouteId || isFocused) {
                            card.style.display = '';   // show
                        } else {
                            card.style.display = 'none'; // hide others
                        }
                        card.classList.toggle('selected', isFocused);
                    });
                }

                function clearRouteFocus() {
                    focusedRouteId = null;
                    const cards = document.querySelectorAll('.route-card');
                    cards.forEach(card => {
                        card.style.display = '';
                        card.classList.remove('selected');
                    });
                }
                /** From a one-hop route object, return the via airport (if any). */
                function getViaAirportFromHop(hop) {
                    if (!hop) return null;

                    // Your JSON likely uses hop.via, but be defensive:
                    if (hop.via) return hop.via;
                    if (hop.via_airport) return hop.via_airport;
                    if (hop.viaAirport) return hop.viaAirport;

                    return null;
                }
                
                    function clearResults() {
                        resultsList.innerHTML = "";
                    }
                
                    function extractIataFromInput(inputEl) {
                                    const raw = (inputEl.value || "").trim().toUpperCase();
                                    if (!raw) return "";
                
                                    // If it ends in "(XXX)" grab XXX
                                    const m = raw.match(/\(([A-Z0-9]{3})\)\s*$/);
                                    if (m) return m[1];
                
                                    // If the whole thing is a 3-letter code, use it
                                    if (/^[A-Z0-9]{3}$/.test(raw)) return raw;
                
                                    // Fallback: just return whatever (so old behavior still sort-of works)
                                    return raw;
                                }
                
                                function getSrcDstOrError() {
                                    const src = extractIataFromInput(srcInput);
                                    const dst = extractIataFromInput(dstInput);
                                    if (!src || !dst) {
                                        setError("Please enter both source and destination IATA codes.");
                                        return null;
                                    }
                                    return { src, dst };
                                }
                //helper func
                function updateViewModeTabs() {
                    viewTabButtons.forEach(btn => {
                        const mode = btn.getAttribute("data-mode");
                        if (mode === viewMode) {
                            btn.classList.add("active");
                        } else {
                            btn.classList.remove("active");
                        }
                    });
                }
                // --- Airline logo helpers ---
                
                    function getAirlineLogoUrl(code) {
                        if (!code || code === "\\N") {
                            // Simple placeholder for unknown codes
                            return "https://placehold.co/40x40/f1f5f9/94a3b8?text=?";
                        }
                        // Kiwi has a nice simple pattern for airline logos by IATA code
                        return "https://images.kiwi.com/airlines/64/" + code.toUpperCase() + ".png";
                    }
                
                    function createAirlinePill(airlineObj) {
                        const name  = airlineObj.name || airlineObj.code || "Unknown airline";
                        const code  = airlineObj.iata || airlineObj.icao || airlineObj.code || "";
                        const label = code ? (name + " (" + code + ")") : name;
                
                        const pill = document.createElement("span");
                        pill.className = "airline-pill";
                
                        const logo = document.createElement("img");
                        logo.src = getAirlineLogoUrl(airlineObj.iata || airlineObj.icao || "");
                        logo.alt = label + " logo";
                        logo.onerror = function () {
                            // Hide logo if the image 404s
                            this.style.display = "none";
                        };
                
                        pill.appendChild(logo);
                        pill.appendChild(document.createTextNode(label));
                        return pill;
                    }
                
                function collectAirlineCodesFromDirect(data) {
                    const codes = new Set();
                    (data.direct_flights || []).forEach(f => {
                        codes.add(getAirlineCodeFromObj(f.airline || {}));
                    });
                    codes.delete(""); // remove empty
                    return codes;
                }
                
                function collectAirlineCodesFromOneHop(data) {
                    const codes = new Set();
                    (data.one_hop_routes || []).forEach(hop => {
                        const first = hop.first_leg || {};
                        const second = hop.second_leg || {};
                        codes.add(getAirlineCodeFromObj(first.airline || {}));
                        codes.add(getAirlineCodeFromObj(second.airline || {}));
                    });
                    codes.delete("");
                    return codes;
                }
                function collectLayoverCodesFromOneHop(data) {
                    const codes = new Set();
                    (data.one_hop_routes || []).forEach(hop => {
                        const via = getViaAirportFromHop(hop);
                        if (!via) return;
                        const code = (via.iata || via.icao || "").toUpperCase();
                        if (code) codes.add(code);
                    });
                    codes.delete("");
                    return codes;
                }

                function renderLayoverFilterBar(data, subset) {
                  layoverFilters.innerHTML = "";

                  let codes;

                  if (Array.isArray(subset)) {
                    // Build layover set from just the subset of hops
                    const s = new Set();
                    subset.forEach(hop => {
                      const via = getViaAirportFromHop(hop);
                      if (!via) return;
                      const code = (via.iata || via.icao || "").toUpperCase();
                      if (code) s.add(code);
                    });
                    s.delete("");
                    codes = s;
                  } else {
                    // Fallback: all hops
                    codes = collectLayoverCodesFromOneHop(data);
                  }

                  if (!codes || !codes.size) {
                    activeLayoverFilter = null;
                    return;
                  }

                  // If the current layover filter no longer exists in this subset, clear it
                  if (activeLayoverFilter && !codes.has(activeLayoverFilter)) {
                    activeLayoverFilter = null;
                  }

                  // “All layovers” pill
                  const allBtn = document.createElement("button");
                  allBtn.textContent = "All layovers";
                  allBtn.className = "filter-pill" + (activeLayoverFilter ? "" : " active");
                  allBtn.onclick = function () {
                    activeLayoverFilter = null;
                    renderLastResults();
                  };
                  layoverFilters.appendChild(allBtn);

                  // One pill per layover code
                  Array.from(codes).sort().forEach(code => {
                    const btn = document.createElement("button");
                    btn.textContent = code;
                    btn.className =
                      "filter-pill" + (activeLayoverFilter === code ? " active" : "");
                    btn.onclick = function () {
                      activeLayoverFilter =
                        activeLayoverFilter === code ? null : code;
                      renderLastResults();
                    };
                    layoverFilters.appendChild(btn);
                  });
                }
                
                function renderAirlineFilterBar(mode, data, subset) {
                    airlineFilters.innerHTML = "";

                    let codes;

                    if (mode === "direct") {
                        // subset is an optional array of direct flights
                        if (Array.isArray(subset)) {
                            const s = new Set();
                            subset.forEach(f => {
                                s.add(getAirlineCodeFromObj((f && f.airline) || {}));
                            });
                            s.delete("");
                            codes = s;
                        } else {
                            codes = collectAirlineCodesFromDirect(data);
                        }
                    } else {
                        // "onehop"
                        if (Array.isArray(subset)) {
                            const s = new Set();
                            subset.forEach(hop => {
                                const first  = hop.first_leg  || {};
                                const second = hop.second_leg || {};
                                s.add(getAirlineCodeFromObj(first.airline  || {}));
                                s.add(getAirlineCodeFromObj(second.airline || {}));
                            });
                            s.delete("");
                            codes = s;
                        } else {
                            codes = collectAirlineCodesFromOneHop(data);
                        }
                    }

                    if (!codes || !codes.size) {
                        activeAirlineFilter = null;
                        return; // nothing to filter on
                    }
                    // If current airline filter is not present in this subset, clear it
                        if (activeAirlineFilter && !codes.has(activeAirlineFilter)) {
                            activeAirlineFilter = null;
                        }

                    // “All airlines” pill
                    const allBtn = document.createElement("button");
                    allBtn.textContent = "All airlines";
                    allBtn.className = "filter-pill" + (activeAirlineFilter ? "" : " active");
                    allBtn.onclick = function () {
                        activeAirlineFilter = null;
                        renderLastResults();
                    };
                    airlineFilters.appendChild(allBtn);

                    // One pill per airline code
                    Array.from(codes).sort().forEach(code => {
                        const btn = document.createElement("button");
                        btn.textContent = code;
                        btn.className = "filter-pill" + (activeAirlineFilter === code ? " active" : "");
                        btn.onclick = function () {
                            activeAirlineFilter = (activeAirlineFilter === code ? null : code);
                            renderLastResults();
                        };
                        airlineFilters.appendChild(btn);
                    });
                }
                
                function renderLastResults() {
                    if (lastMode === "direct" && lastDirectData) {
                        renderNonstopResults(lastDirectData); // will read activeAirlineFilter
                    } else if (lastMode === "onehop" && lastOneHopData) {
                        renderOneHopResults(lastOneHopData);  // same
                    }
                }
                
                    // ----- Render helpers -----
                
                    function renderNonstopResults(data) {
                        lastMode = "direct";
                        lastDirectData = data;

                        clearResults();
                        // 🔵 Non-stop mode: hide any leftover layover filters
                            activeLayoverFilter = null;
                            layoverFilters.innerHTML = "";


                        // Build airline chips from *all* direct flights
                        renderAirlineFilterBar("direct", data);

                        const src = data.source;
                        const dst = data.destination;
                        const allFlights = data.direct_flights || [];

                        // Apply airline filter (if set)
                        const flights = allFlights.filter(f => {
                            if (!activeAirlineFilter) return true;
                            const code = getAirlineCodeFromObj(f.airline || {});
                            return code === activeAirlineFilter;
                        });

                        if (!flights.length) {
                            const msg = activeAirlineFilter
                                ? "No non-stop routes for " + activeAirlineFilter +
                                  " on this city pair. Try another airline or 'All airlines'."
                                : "No non-stop routes found between " +
                                  (src.iata || "???") + " and " + (dst.iata || "???") + ".";
                            resultsList.innerHTML = '<p class="no-results">' + msg + '</p>';
                            clearHomeMap();
                            return;
                        }

                        // Draw just the filtered flights on the map
                        renderRoutesOnHomeMap("direct", data, flights);

                        flights.forEach(function (flight, index) {
                            const airlineObj     = flight.airline || {};
                            const codeshare      = !!flight.codeshare;
                            const equipmentLabel = formatEquipment(flight.equipment || "");
                            const equipment      = equipmentLabel ? ("Aircraft: " + equipmentLabel) : "";

                            const airlinePill = createAirlinePill(airlineObj);

                            const card = document.createElement("div");
                            card.className = "route-card clickable";

                            // For non-stop routes, ids are "direct-0", "direct-1", ...
                            const id = "direct-" + index;
                            card.dataset.routeId = id;

                            card.addEventListener("click", () => {
                                highlightRoute(id);
                                if (focusedRouteId === id) {
                                    // Clicking again clears focus
                                    clearRouteFocus();
                                } else {
                                    focusedRouteId = id;
                                    applyRouteFocusToCards();
                                }
                            });

                            card.innerHTML =
                                '<div class="route-main-line">' +
                                  '<span class="route-city">' + src.city + ' (' + src.iata + ')</span>' +
                                  '<span class="route-arrow">→</span>' +
                                  '<span class="route-city">' + dst.city + ' (' + dst.iata + ')</span>' +
                                  '<span class="route-tag nonstop">Nonstop</span>' +
                                '</div>' +
                                '<div class="route-sub-line">' +
                                  '<span class="__airline-pill-slot"></span>' +
                                  (codeshare ? '<span class="route-tag codeshare">Codeshare</span>' : '') +
                                  (equipment ? '<span>' + equipment + '</span>' : '') +
                                '</div>';

                            resultsList.appendChild(card);
                            card.querySelector(".__airline-pill-slot").replaceWith(airlinePill);
                        });

                        // Re-apply focus styling/visibility after rebuilding cards
                        applyRouteFocusToCards();
                    }
                
                function renderOneHopResults(data) {
                    lastMode      = "onehop";
                    lastOneHopData = data;

                    clearResults();

                    const src     = data.source || {};
                    const dst     = data.destination || {};
                    const allHops = data.one_hop_routes || [];

                    // Start from all hops
                    let hops = allHops;

                    // 1) Airline filter
                    if (activeAirlineFilter) {
                        hops = hops.filter(hop => {
                            const first  = hop.first_leg  || {};
                            const second = hop.second_leg || {};
                            const c1 = getAirlineCodeFromObj(first.airline  || {});
                            const c2 = getAirlineCodeFromObj(second.airline || {});
                            return c1 === activeAirlineFilter || c2 === activeAirlineFilter;
                        });
                    }

                    // 2) Layover filter
                    if (activeLayoverFilter) {
                        hops = hops.filter(hop => {
                            const via = getViaAirportFromHop(hop);
                            const viaCode = (via && (via.iata || via.icao)) || "";
                            return viaCode === activeLayoverFilter;
                        });
                    }

                    // 3) Rebuild chip bars from the *final* subset
                    renderLayoverFilterBar(data, hops);
                    renderAirlineFilterBar("onehop", data, hops);

                    // 4) Map + "no results" handling
                    renderRoutesOnHomeMap("onehop", data, hops);

                    if (!hops.length) {
                        const baseMsg = "No one-hop routes found between " +
                          (src.iata || "???") + " and " + (dst.iata || "???") + ".";
                        resultsList.innerHTML =
                          '<p class="no-results">' + baseMsg + '</p>';
                        clearHomeMap();
                        return;
                    }

                    // 5) Build cards
                    hops.forEach(function (hop, index) {
                        const via    = getViaAirportFromHop(hop);
                        const first  = hop.first_leg  || {};
                        const second = hop.second_leg || {};

                        const sameAirline  = !!hop.same_airline;
                        const hasCodeshare = !!hop.has_codeshare;
                        const distance     = hop.total_distance_miles.toFixed(0);

                        const firstAirlinePill  = createAirlinePill(first.airline  || {});
                        const secondAirlinePill = createAirlinePill(second.airline || {});

                        const firstEquipLabel  = formatEquipment(first.equipment  || "");
                        const secondEquipLabel = formatEquipment(second.equipment || "");

                        let badgesHtml = "";
                        if (sameAirline) {
                            badgesHtml += '<span class="route-tag same-airline">Same airline both legs</span>';
                        } else {
                            badgesHtml += '<span class="route-tag mixed-airlines">Mixed airlines</span>';
                        }
                        if (hasCodeshare) {
                            badgesHtml += '<span class="route-tag codeshare">Codeshare on one or more legs</span>';
                        }

                        const card = document.createElement("div");
                        card.className = "route-card clickable";

                        // For one-hops, ids are "onehop-0", "onehop-1", ...
                        const id = "onehop-" + index;
                        card.dataset.routeId = id;

                        card.addEventListener("click", () => {
                            highlightRoute(id);
                            if (focusedRouteId === id) {
                                clearRouteFocus();
                            } else {
                                focusedRouteId = id;
                                applyRouteFocusToCards();
                            }
                        });

                        card.innerHTML =
                          '<div class="route-main-line">' +
                            '<span class="route-city">' + src.city + ' (' + src.iata + ')</span>' +
                            '<span class="route-arrow">→</span>' +
                            '<span class="route-city">' + dst.city + ' (' + dst.iata + ')</span>' +
                            '<span class="route-tag onehop">1 stop via ' +
                              (via ? (via.city + ' (' + (via.iata || via.icao || '') + ')') : '—') +
                            '</span>' +
                            '<span class="route-distance">' + distance + ' mi</span>' +
                          '</div>' +
                          '<div class="route-sub-line">' +
                            badgesHtml +
                          '</div>' +
                          '<div class="route-sub-line" style="gap: 1rem;">' +
                            '<span style="display:flex;align-items:center;gap:0.25rem;">' +
                              '1st leg: <span class="__first-pill-slot"></span>' +
                              (firstEquipLabel ? ' · <span>Aircraft: ' + firstEquipLabel + '</span>' : '') +
                            '</span>' +
                            '<span style="display:flex;align-items:center;gap:0.25rem;">' +
                              '2nd leg: <span class="__second-pill-slot"></span>' +
                              (secondEquipLabel ? ' · <span>Aircraft: ' + secondEquipLabel + '</span>' : '') +
                            '</span>' +
                          '</div>';

                        resultsList.appendChild(card);
                        card.querySelector(".__first-pill-slot").replaceWith(firstAirlinePill);
                        card.querySelector(".__second-pill-slot").replaceWith(secondAirlinePill);
                    });

                    applyRouteFocusToCards();
                }
                
                function renderForCurrentMode() {
                    if (viewMode === "json") {
                        renderJsonView();
                    } else {
                        // Default: cards
                        renderLastResults();
                    }
                }
                
                viewTabButtons.forEach(btn => {
                    btn.addEventListener("click", () => {
                        const mode = btn.getAttribute("data-mode");
                        if (mode === viewMode) return;
                        viewMode = mode;
                        updateViewModeTabs();
                        renderForCurrentMode();
                    });
                });
                
                function renderJsonView() {
                    clearResults();
                
                    if (!lastMode) {
                        resultsList.innerHTML =
                            '<p class="no-results">(run a search to see JSON)</p>';
                        return;
                    }
                
                    const data = (lastMode === "direct") ? lastDirectData : lastOneHopData;
                    if (!data) {
                        resultsList.innerHTML =
                            '<p class="no-results">(no data available yet)</p>';
                        return;
                    }
                
                    // Show raw JSON, pretty-printed; basic escaping of "<"
                    const jsonStr = JSON.stringify(data, null, 2).replace(/</g, "&lt;");
                
                    // We can keep the airline filter bar, or clear it; your choice.
                    // For now, clear filters so the JSON area has more room:
                    airlineFilters.innerHTML = "";
                
                    resultsList.innerHTML =
                        '<pre style="background:#020617;color:#e5e7eb;' +
                        'padding:0.75rem;border-radius:0.5rem;font-size:0.8rem;' +
                        'overflow:auto;max-height:24rem;">' +
                        jsonStr +
                        '</pre>';
                }
                
                 // --- Autocomplete helpers ---
                
                            let activeList = null;
                            let activeIndex = -1;
                
                            async function fetchAirportSuggestions(query) {
                                if (!query || query.length < 2) return [];
                                try {
                                    const resp = await fetch("/airports/search?q=" + encodeURIComponent(query) + "&limit=8");
                                    if (!resp.ok) return [];
                                    return await resp.json();
                                } catch (e) {
                                    console.error("autocomplete fetch error", e);
                                    return [];
                                }
                            }
                
                            function renderSuggestions(airports, inputEl, listEl) {
                                listEl.innerHTML = "";
                                activeList = listEl;
                                activeIndex = -1;
                
                                if (!airports.length) {
                                    listEl.innerHTML = '<div class="autocomplete-empty">No matching airports</div>';
                                    listEl.style.display = "block";
                                    return;
                                }
                
                                airports.forEach((ap, idx) => {
                                    const div = document.createElement("div");
                                    div.className = "autocomplete-item";
                                    const display = ap.display || (ap.city + ", " + ap.country + " (" + ap.code + ")");
                                    div.textContent = display;
                                    div.dataset.code = ap.code;
                
                                    div.addEventListener("click", () => {
                                        inputEl.value = display;
                                        listEl.style.display = "none";
                                        listEl.innerHTML = "";
                                    });
                
                                    listEl.appendChild(div);
                                });
                
                                listEl.style.display = "block";
                            }
                
                            async function handleAutocompleteInput(inputEl, listEl) {
                                const query = (inputEl.value || "").trim();
                                if (query.length < 2) {
                                    listEl.style.display = "none";
                                    listEl.innerHTML = "";
                                    return;
                                }
                                const airports = await fetchAirportSuggestions(query);
                                // Only render if the text hasn't changed while fetching
                                if ((inputEl.value || "").trim() === query) {
                                    renderSuggestions(airports, inputEl, listEl);
                                }
                            }
                
                            function handleAutocompleteKeydown(e, inputEl, listEl) {
                // If the list is hidden, let the event bubble normally.
                if (listEl.style.display === "none") return;

                const items = listEl.querySelectorAll(".autocomplete-item");
                if (!items.length) return;

                if (e.key === "ArrowDown" || e.key === "ArrowUp") {
                  e.preventDefault();
                  if (activeList !== listEl) {
                    activeList = listEl;
                    activeIndex = -1;
                  }
                  const dir = e.key === "ArrowDown" ? 1 : -1;
                  activeIndex = (activeIndex + dir + items.length) % items.length;
                  items.forEach(el => el.classList.remove("active"));
                  items[activeIndex].classList.add("active");
                  items[activeIndex].scrollIntoView({ block: "nearest" });
                } else if (e.key === "Enter" || e.key === "Tab") {
                  // Accept the current selection (or first item if none selected yet)
                  if (activeIndex < 0) {
                    activeIndex = 0;
                  }
                  const chosen = items[activeIndex];
                  if (chosen) {
                    // Don’t submit the form on this Enter – we’re just choosing a suggestion.
                    if (e.key === "Enter") {
                      e.preventDefault();
                    }
                    chosen.click();  // sets input value and calls loadAirportDetails/airline
                  }
                } else if (e.key === "Escape") {
                  listEl.style.display = "none";
                }
              }
                            // Autocomplete wiring
                            srcInput.addEventListener("input", () => {
                                handleAutocompleteInput(srcInput, srcSuggestions);
                            });
                            dstInput.addEventListener("input", () => {
                                handleAutocompleteInput(dstInput, dstSuggestions);
                            });
                
                            srcInput.addEventListener("keydown", (e) => {
                                handleAutocompleteKeydown(e, srcInput, srcSuggestions);
                            });
                            dstInput.addEventListener("keydown", (e) => {
                                handleAutocompleteKeydown(e, dstInput, dstSuggestions);
                            });
                
                            // Hide suggestions when clicking outside
                            document.addEventListener("click", (e) => {
                                if (!e.target.closest(".autocomplete-container")) {
                                    srcSuggestions.style.display = "none";
                                    dstSuggestions.style.display = "none";
                                }
                            });
                            // Clear Filters button: reset airline + layover filters and re-render
                            clearFiltersBtn.addEventListener("click", () => {
                                activeAirlineFilter = null;
                                activeLayoverFilter = null;
                                clearRouteFocus(); 
                                renderLastResults();
                            });
                    // ----- Network calls -----
                
                    form.addEventListener("submit", function (event) {
                        
                        event.preventDefault();
                        setError("");
                        
                        // 🔄 Reset airline filter whenever a new search is run
                          activeAirlineFilter = null;
                          airlineFilters.innerHTML = ""; // optional, it'll be repopulated by render
                
                        activeLayoverFilter = null;
                        layoverFilters.innerHTML = "";
                        clearRouteFocus();
                        
                        errorEl.textContent = "";
                          resultsList.innerHTML = '<p class="no-results">Loading...</p>';

                
                        const pair = getSrcDstOrError();
                        if (!pair) return;
                        const src = pair.src;
                        const dst = pair.dst;
                
                        let limit = parseInt((limitInput.value || "").trim(), 10);
                        if (isNaN(limit) || limit <= 0) {
                            limit = 20;
                            limitInput.value = "20";
                        }
                        const recommended = recommendedInput.checked;
                
                        const params = new URLSearchParams({
                            src: src,
                            dst: dst,
                            limit: String(limit),
                            recommended: recommended ? "true" : "false"
                        });
                
                        clearResults();
                        clearHomeMap(); // Clear map and previous highlight

                        resultsList.innerHTML = '<p class="no-results">Loading one-hop routes…</p>';
                
                        fetch("/onehop?" + params.toString())
                            .then(function (response) {
                                if (!response.ok) {
                                    return response.text().then(function (text) {
                                        throw new Error("Server returned " + response.status + ": " + text);
                                    });
                                }
                                return response.json();
                            })
                            .then(function (data) {
                                if (data && typeof data === "object" && data.error) {
                                    setError("Server error: " + data.error);
                                    clearResults();
                                    return;
                                }
                                 // New search → default to cards view
                                viewMode = "cards";
                                updateViewModeTabs();
                                renderOneHopResults(data);
                            })
                            .catch(function (err) {
                                console.error(err);
                                setError("Request failed. Please try again.");
                                clearResults();
                            });
                    });
                
                    directButton.addEventListener("click", function () {
                        setError("");
                        const pair = getSrcDstOrError();
                        if (!pair) return;
                
                        const params = new URLSearchParams({
                            src: pair.src,
                            dst: pair.dst
                        });
                
                        // 🔵 Non-stop search: reset layover filters immediately
                            activeLayoverFilter = null;
                            layoverFilters.innerHTML = "";
                
                        clearResults();
                        clearHomeMap(); // Clear map and previous highlight
                        resultsList.innerHTML = '<p class="no-results">Loading non-stop routes…</p>';
                
                        fetch("/direct?" + params.toString())
                            .then(function (response) {
                                if (!response.ok) {
                                    return response.text().then(function (text) {
                                        throw new Error("Server returned " + response.status + ": " + text);
                                    });
                                }
                                return response.json();
                            })
                            .then(function (data) {
                                if (data && typeof data === "object" && data.error) {
                                    setError("Server error: " + data.error);
                                    clearResults();
                                    return;
                                }
                                viewMode = "cards";
                                updateViewModeTabs();
                
                                renderNonstopResults(data);
                            })
                            .catch(function (err) {
                                console.error(err);
                                setError("Request failed. Please try again.");
                                clearResults();
                            });
                    });
                
                })();
                </script>
                </body>
                </html>
        )HTML";
            
            response = buildHttpResponse(body, "text/html; charset=UTF-8");
        }
        else if (pathOnly == "/json") {
            std::string body = R"({"status":"ok","message":"Hello from CIS 22C JSON endpoint"})";
            response = buildHttpResponse(body, "application/json; charset=UTF-8");
        }
        else if (pathOnly == "/airports/sample") {
            std::string body = airportsSampleToJson(10);
            response = buildHttpResponse(body, "application/json; charset=UTF-8");
        }
        else if (pathOnly == "/airline") {
            // your existing /airline?code= handler:
            auto params = parseQueryString(queryString);
            auto it = params.find("code");
            if (it == params.end() || it->second.empty()) {
                std::string body = R"({"error":"Missing 'code' query parameter"})";
                response = buildHttpResponse(body,
                                             "application/json; charset=UTF-8",
                                             "HTTP/1.1 400 Bad Request\r\n");
            } else {
                std::string code = it->second;
                // Normalize to uppercase (IATA codes are uppercase in data)
                for (char &c : code) c = static_cast<char>(std::toupper((unsigned char)c));
                
                auto ait = g_airlinesByIata.find(code);
                if (ait == g_airlinesByIata.end()) {
                    std::ostringstream body;
                    body << R"({"error":"Airline with code ')" << code << R"(' not found"})";
                    response = buildHttpResponse(body.str(),
                                                 "application/json; charset=UTF-8",
                                                 "HTTP/1.1 404 Not Found\r\n");
                } else {
                    std::string body = airlineToJson(*(ait->second));
                    response = buildHttpResponse(body, "application/json; charset=UTF-8");
                }
            }
        }
        else if (pathOnly == "/airline-ui") {
            std::string body = R"HTML(
        <!DOCTYPE html>
        <html lang="en">
        <head>
          <meta charset="UTF-8" />
          <title>Airline Routes Explorer</title>
          <meta name="viewport" content="width=device-width, initial-scale=1" />
          <style>
            /* NEW: Map container style */
                #routeMap {
                  height: 400px; /* Define the height for the map */
                  margin-top: 1rem;
                  border-radius: 8px;
                }
            body {
              font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
              margin: 0;
              padding: 1.5rem;
              background: #f5f5f5;
              color: #222;
            }
            .container {
              max-width: 880px;
              margin: 0 auto;
              background: #ffffff;
              border-radius: 12px;
              padding: 1.5rem 1.75rem 1.75rem;
              box-shadow: 0 2px 8px rgba(0, 0, 0, 0.08);
            }
            .header-bar {
              display: flex;
              justify-content: space-between;
              align-items: center;
              gap: 1rem;
              margin-bottom: 0.75rem;
            }
            .header-left h1 {
              margin: 0;
              font-size: 1.7rem;
            }
            .subtitle {
              font-size: 0.85rem;
              color: #6b7280;
              margin-top: 0.15rem;
            }
            .header-right a {
              font-size: 0.8rem;
              text-decoration: none;
              padding: 0.35rem 0.65rem;
              border-radius: 999px;
              border: 1px solid #e5e7eb;
              color: #374151;
              background: #f9fafb;
            }
            .header-right a:hover {
              background: #e5e7eb;
            }
            form {
              display: grid;
              gap: 0.75rem;
              margin-bottom: 1rem;
            }
            .field-group {
              display: flex;
              flex-wrap: wrap;
              gap: 0.75rem;
            }
            .field {
              display: flex;
              flex-direction: column;
              min-width: 140px;
              flex: 1 1 0;
            }
            label {
              font-size: 0.8rem;
              font-weight: 600;
              margin-bottom: 0.25rem;
              text-transform: uppercase;
              letter-spacing: 0.06em;
            }

            /* NEW: keep long label + ⓘ on one line */
            .field label {
              display: inline-flex;
              align-items: center;
              gap: 0.35rem;
            }

            /* Slightly smaller text inside that label so it fits nicely */
            .field .label-text {
              font-size: 0.76rem;
            }

            /* Style the info icon itself */
            .info-icon {
              font-size: 0.9rem;
              cursor: help;
              line-height: 1;
              user-select: none;
            }

            input[type="text"],
            input[type="number"] {
              padding: 0.4rem 0.5rem;
              border-radius: 6px;
              border: 1px solid #d1d5db;
              font-size: 0.95rem;
            }
            .btn {
              padding: 0.45rem 0.9rem;
              border-radius: 999px;
              border: none;
              font-size: 0.95rem;
              font-weight: 600;
              cursor: pointer;
              display: inline-flex;
              align-items: center;
              gap: 0.25rem;
            }
            .btn-primary {
              background: #2563eb;
              color: #ffffff;
            }
            .btn-primary:hover {
              background: #1d4ed8;
            }
            #error {
              margin-top: 0.25rem;
              color: #b91c1c;
              font-size: 0.9rem;
            }
            .hint {
              font-size: 0.85rem;
              color: #6b7280;
              margin-top: 0.25rem;
            }
            h3 {
              margin-top: 1.25rem;
              margin-bottom: 0.5rem;
              font-size: 1.05rem;
            }
            .results-list {
              margin-top: 0.5rem;
              display: flex;
              flex-direction: column;
              gap: 0.75rem;
            }

                       .route-card {
                         background: #ffffff;
                         border-radius: 12px;
                         padding: 0.75rem 1rem;
                         box-shadow: 0 1px 4px rgba(15, 23, 42, 0.08);
                         font-size: 0.93rem;
                       }
                       .route-main-line {
                         display: flex;
                         align-items: center;
                         gap: 0.35rem;
                         font-weight: 600;
                         margin-bottom: 0.25rem;
                       }
                       .route-city {
                         font-size: 0.96rem;
                       }
                       .route-tag {
                         font-size: 0.72rem;
                         font-weight: 600;
                         padding: 0.1rem 0.55rem;
                         border-radius: 999px;
                         margin-left: 0.2rem;
                         white-space: nowrap;
                         background: #e8f0fe;
                         color: #174ea6;
                       }
                       /* “ceased operations” chip – same palette vibe as codeshare */
                       .route-tag.ceased {
                         background: #fef7e0;
                         color: #915b00;
                       }
                       .route-sub-line {
                         font-size: 0.83rem;
                         color: #4b5563;
                         display: flex;
                         flex-direction: column;
                         gap: 0.25rem;
                       }
                       /* NEW: dedicated line just for the ceased-ops chip */
                       .ceased-line {
                         margin-top: 0.05rem;
                       }
                       /* NEW: row of colored meta chips (country, IATA, ICAO, totals) */
                       .meta-chips-row {
                         display: flex;
                         flex-wrap: wrap;
                         gap: 0.35rem;
                       }
                       .meta-chip {
                         font-size: 0.75rem;
                         font-weight: 500;
                         padding: 0.12rem 0.6rem;
                         border-radius: 999px;
                         white-space: nowrap;
                       }
                       .meta-chip.country-chip {
                         background: #e0f2fe;
                         color: #075985;
                       }
                       .meta-chip.iata-chip {
                         background: #ecfdf5;
                         color: #166534;
                       }
                       .meta-chip.icao-chip {
                         background: #fef3c7;
                         color: #92400e;
                       }
                       .meta-chip.totals-chip {
                         background: #e5e7eb;
                         color: #374151;
                       }
                       /* right-side “Active = …” stack */
                       .airline-summary-right {
                         margin-left: auto;
                         display: flex;
                         flex-direction: column;
                         align-items: flex-end;
                         gap: 0.25rem;
                       }
                       .status-pill {
                         display: inline-flex;
                         align-items: center;
                         padding: 0.15rem 0.7rem;
                         border-radius: 999px;
                         font-size: 0.78rem;
                         font-weight: 500;
                         white-space: nowrap;
                         margin-top: 0.15rem;
                       }
                       .status-pill.active {
                         background: #111827;
                         color: #f9fafb;
                       }
                       .status-pill.inactive {
                         background: #e5e7eb;
                         color: #4b5563;
                       }
        /* --- Airport cards (clickable) --- */
        .airport-card {
          cursor: pointer;
          transition: box-shadow 0.18s ease, transform 0.18s ease, border-color 0.18s ease;
        }

        .airport-card:hover {
          box-shadow: 0 4px 10px rgba(15, 23, 42, 0.12);
          transform: translateY(-1px);
          border-color: #93c5fd;
        }

        .airport-card.selected {
          border-color: #2563eb;
          box-shadow: 0 0 0 1px rgba(37, 99, 235, 0.65);
        }

        /* Main line + subline inside airport cards */
        .airport-main-line {
          display: flex;
          align-items: center;
          justify-content: space-between;
          gap: 0.75rem;
        }

        .airport-name {
          font-weight: 600;
          font-size: 0.95rem;
        }

        .airport-subline {
          font-size: 0.82rem;
          color: #4b5563;
          margin-top: 0.1rem;
        }
        /* Expanded arrivals / departures section inside an airport card */
        .airport-flows {
          margin-top: 0.6rem;
          padding-top: 0.65rem;
          border-top: 1px solid #e5e7eb;
        }

        .flow-section {
          margin-bottom: 0.75rem;
        }

        .flow-section-title {
          font-size: 0.78rem;
          font-weight: 600;
          letter-spacing: 0.06em;
          text-transform: uppercase;
          color: #6b7280;
          margin-bottom: 0.35rem;
        }

        /* Airport details – table-like grid */
        .airport-flow-section {
          margin-top: 0.75rem;
        }

        .airport-flow-title {
          font-size: 0.78rem;
          font-weight: 600;
          text-transform: uppercase;
          letter-spacing: 0.06em;
          color: #6b7280;
          margin-bottom: 0.35rem;
        }

        /* One grid that automatically creates as many columns/rows as needed */
        .flows-grid {
          display: grid;
          grid-template-columns: repeat(auto-fill, minmax(160px, 1fr));
          column-gap: 1.75rem;
          row-gap: 0.2rem;
          font-size: 0.86rem;
        }

        /* Each destination cell */
        .flow-item {
          white-space: nowrap;
          overflow: hidden;
          text-overflow: ellipsis;
        }
        /* Expanded details inside an airport card */
                .airport-details {
                  display: none;              /* hidden until card is clicked */
                  margin-top: 0.55rem;
                  padding-top: 0.45rem;
                  border-top: 1px solid #e5e7eb;
                  font-size: 0.8rem;
                  color: #4b5563;
                }

                .airport-flow-section {
                  margin-bottom: 0.4rem;
                }

                .airport-flow-title {
                  font-weight: 600;
                  font-size: 0.78rem;
                  text-transform: uppercase;
                  letter-spacing: 0.04em;
                  margin-bottom: 0.2rem;
                  color: #6b7280;
                }

              
                .airport-flow-airport {
                  font-weight: 500;
                }

              
                .airport-details-empty {
                  font-style: italic;
                  color: #9ca3af;
                }

        /* --- Direction (Arrivals/Departures) tabs --- */
        .direction-tabs {
          display: inline-flex;
          gap: 0.25rem;
          margin-top: 0.35rem;
          margin-bottom: 0.45rem;
          font-size: 0.78rem;
        }

        .direction-tab {
          border: none;
          cursor: pointer;
          background: #e5e7eb;
          color: #111827;
          padding: 0.15rem 0.7rem;
          border-radius: 999px;
          font-weight: 500;
        }

        .direction-tab.active {
          background: #1d4ed8;
          color: #f9fafb;
        }
            .no-results {
              font-size: 0.9rem;
              color: #6b7280;
            }
            .view-mode-tabs {
              display: inline-flex;
              gap: 0.25rem;
              margin-top: 0.25rem;
              margin-bottom: 0.4rem;
              font-size: 0.8rem;
            }
            .view-tab {
              border: none;
              cursor: pointer;
              background: #e5e7eb;
              color: #111827;
              padding: 0.15rem 0.7rem;
              border-radius: 999px;
              font-weight: 500;
            }
        .clear-filters-btn {
            background: #e5e7eb;
            color: #374151;
            border: none;
            border-radius: 999px;
            padding: 0.25rem 0.8rem;
            font-size: 0.78rem;
            font-weight: 500;
            cursor: pointer;
            white-space: nowrap;
        }

        .clear-filters-btn:hover {
            background: #d1d5db;
        }
            .view-tab.active {
              background: #111827;
              color: #f9fafb;
            }
            /* Airline pill with logo */
            .airline-pill {
              display: inline-flex;
              align-items: center;
              background: #f1f5f9;  /* light gray */
              color: #1e293b;       /* dark slate */
              padding: 0.15rem 0.6rem 0.15rem 0.25rem;
              border-radius: 999px;
              font-size: 0.78rem;
              font-weight: 500;
              margin-right: 0.4rem;
              white-space: nowrap;
              line-height: 1;
            }
        
            .airline-pill img {
              height: 1.25rem;
              width: 1.25rem;
              border-radius: 50%;
              object-fit: contain;
              margin-right: 0.35rem;
              background: #ffffff;
              border: 1px solid #e2e8f0;
              flex-shrink: 0;
            }
          </style>
        </head>
        <body>
          <div class="container">
            <div class="header-bar">
              <div class="header-left">
                <h1>Airline Routes Explorer</h1>
                <p class="subtitle">
                  Enter an airline code (e.g. <code>AA</code>, <code>DL</code>, <code>UA</code>)
                  to see all airports it serves.
                </p>
              </div>
              <div class="header-right">
                <a href="/">← Back to Route Finder</a>
              </div>
            </div>
        
            <form id="airline-form">
             <div class="field-group">
               <!-- WIDER airline field -->
               <div class="field autocomplete-container"
                    style="max-width: 340px; flex: 2 1 0;">
                 <label for="airline">
                   <span class="label-text">Airline (IATA code or name)</span>
                   <span class="info-icon"
                         title="Type an IATA code like &quot;AA&quot; or part of an airline name like &quot;American&quot; to search.">
                     ⓘ
                   </span>
                 </label>
                 <input id="airline" name="airline" type="text" maxlength="40"
                        placeholder="e.g. AA, DL, UA or 'American'" autocomplete="off" />
                 <div id="airline-suggestions" class="autocomplete-list" style="display:none;"></div>
               </div>

               <!-- NARROWER max-airports field -->
               <div class="field"
                    style="max-width: 120px; flex: 0 0 120px;">
                 <label for="limit">Max airports</label>
                 <input id="limit" name="limit" type="number" min="1" value="50" />
               </div>
             </div>

             <button type="submit" class="btn btn-primary">
               Search Airline Routes
             </button>
             <div id="error"></div>
             <div class="hint">
               Uses the <code>/airline/routes?code=XX</code> API from the server.
             </div>

            </form>
        
            <h3>Results</h3>
            <div id="routeMap"></div>

            <div class="view-mode-tabs" id="viewModeTabs">
              <button class="view-tab active" data-mode="cards">Cards</button>
              <button class="view-tab" data-mode="json">JSON</button>
            </div>

            <!-- NEW: Arrivals / Departures filter for map -->
            <div class="direction-tabs" id="directionTabs">
              <button class="direction-tab active" data-direction="all">All routes</button>
              <button class="direction-tab" data-direction="arrivals">Arrivals</button>
              <button class="direction-tab" data-direction="departures">Departures</button>
            </div>

            <div id="resultsList" class="results-list">
              <p class="no-results">(no results yet)</p>
            </div>
          </div>
        <script src="https://api.mapbox.com/mapbox-gl-js/v3.4.0/mapbox-gl.js"></script>
          <script>
            (function () {
              const form = document.getElementById("airline-form");
              const airlineInput = document.getElementById("airline");
              const limitInput = document.getElementById("limit");
              const errorEl = document.getElementById("error");
              const resultsList = document.getElementById("resultsList");
        
                // --- Mapbox Setup ---
                            // IMPORTANT: Replace this with your actual public access token.
                            mapboxgl.accessToken = 'pk.eyJ1IjoiY2hyaXN0aW5lcnlhbjkzIiwiYSI6ImNtaWJnZDhiMDAxN2sya29sNXZvNHExMXkifQ.6K3wdjmIhFRQD3bre4f_DA';

                            const INITIAL_CENTER = [-98.5, 39.8]; // Center of the US (approx)
                            const INITIAL_ZOOM = 2; // World view

                            const map = new mapboxgl.Map({
                                container: 'routeMap',
                                style: 'mapbox://styles/mapbox/light-v11', // Light theme style
                                center: INITIAL_CENTER,
                                zoom: INITIAL_ZOOM
                            });
                            
                            // Map state to be used later
                            let routeMapIsLoaded = false; 
                            // Keep the full airline GeoJSON so we can filter it later
                            let lastGeoJsonData = null;

                            // Current airport filter + direction filter for the map
                            let selectedAirportCode = null;      // e.g. "DFW"
                            let directionFilter = "all";         // "all" | "arrivals" | "departures"


                            map.on('load', () => {
                                routeMapIsLoaded = true;
                                // We'll add the map source and layer here once data is ready.
                            });
                            // --- End Mapbox Setup ---
              const viewModeTabs = document.getElementById("viewModeTabs");
              const viewTabButtons = viewModeTabs.querySelectorAll(".view-tab");
        
              let viewMode = "cards";      // "cards" | "json"
              let lastData = null;         // last JSON from /airline/routes
              let lastTotalAirports = 0;   // <-- total airports before client-side limiting
              const directionTabs = document.getElementById("directionTabs");
              const directionButtons = directionTabs.querySelectorAll(".direction-tab");
            
              // --- Direction filter: All / Arrivals / Departures ---
              // Uses the existing `directionFilter` and `directionButtons`

              function updateDirectionTabs() {
                directionButtons.forEach(btn => {
                  const dir = btn.getAttribute("data-direction");   // NOTE: matches your HTML
                  if (dir === directionFilter) btn.classList.add("active");
                  else btn.classList.remove("active");
                });
              }

              // Initial state: "all" is already marked active in the HTML, but this keeps it in sync
              updateDirectionTabs();

              directionButtons.forEach(btn => {
                btn.addEventListener("click", () => {
                  const dir = btn.getAttribute("data-direction");
                  if (!dir || dir === directionFilter) return;

                  directionFilter = dir;
                  updateDirectionTabs();

                  // Re-apply the filter to whatever airport (if any) is currently selected
                  applyAirportFilterForMap(selectedAirportCode);
                });
              });
              const airlineSuggestions = document.getElementById("airline-suggestions");
    
            // Cache of all airlines from /airlines
            let allAirlines = null;
            let activeSuggestionIndex = -1;
            let selectedAirline = null; 
        
            // Load all airlines once (IATA non-empty, sorted by IATA)
            function loadAllAirlinesOnce() {
              if (allAirlines) return Promise.resolve(allAirlines);
        
              return fetch("/airlines?limit=5000&offset=0")
                .then(resp => {
                  if (!resp.ok) throw new Error("Failed to load airline list");
                  return resp.json();
                })
                .then(data => {
                  // data is an array of airline objects
                  allAirlines = data.filter(a => a && (a.iata || a.icao));
                  return allAirlines;
                })
                .catch(err => {
                  console.error("Error loading airlines:", err);
                  allAirlines = [];
                  return allAirlines;
                });
            }
        
            // Build a display label like "American Airlines (AA)"
            function airlineDisplayLabel(a) {
              const code = a.iata || a.icao || "";
              const name = a.name || code || "Unknown airline";
              return code ? (name + " (" + code + ")") : name;
            }
        
            function renderAirlineSuggestions(list) {
              airlineSuggestions.innerHTML = "";
              activeSuggestionIndex = -1;
              selectedAirline = null;   // ✅ user is typing again, clear previous selection

        
              if (!list.length) {
                airlineSuggestions.innerHTML =
                  '<div class="autocomplete-empty">No matching airlines</div>';
                airlineSuggestions.style.display = "block";
                return;
              }
        
              list.forEach((a, idx) => {
                const div = document.createElement("div");
                div.className = "autocomplete-item";
                const label = airlineDisplayLabel(a);
                div.textContent = label;
                div.dataset.code = a.iata || a.icao || "";
                div.dataset.label = label;
        
                div.addEventListener("click", () => {
                  airlineInput.value = label;
                  airlineSuggestions.style.display = "none";
                  airlineSuggestions.innerHTML = "";
                  selectedAirline = a;   // ✅ remember exactly which airline they chose
                });
        
                airlineSuggestions.appendChild(div);
              });
        
              airlineSuggestions.style.display = "block";
            }
        
            function filterAirlines(query) {
              if (!allAirlines || !allAirlines.length) return [];
        
              const q = query.trim().toLowerCase();
              if (!q) return [];
        
              // Simple scoring: prioritize IATA prefix, then name matches
              const scored = allAirlines.map(a => {
                const code = (a.iata || a.icao || "").toLowerCase();
                const name = (a.name || "").toLowerCase();
                const country = (a.country || "").toLowerCase();
        
                let score = 0;
        
                if (code === q) score += 200;
                else if (code.startsWith(q)) score += 140;
                else if (code.includes(q)) score += 60;
        
                if (name === q) score += 180;
                else if (name.startsWith(q)) score += 140;
                else if (name.includes(q)) score += 80;
        
                if (country.includes(q)) score += 10;
        
                return { airline: a, score };
              }).filter(x => x.score > 0);
        
              scored.sort((a, b) => {
                if (a.score !== b.score) return b.score - a.score;
                const la = airlineDisplayLabel(a.airline);
                const lb = airlineDisplayLabel(b.airline);
                return la.localeCompare(lb);
              });
        
              return scored.slice(0, 8).map(x => x.airline);
            }
        
            function handleAirlineAutocompleteInput() {
              const query = airlineInput.value || "";
              if (query.trim().length < 2) {
                airlineSuggestions.style.display = "none";
                airlineSuggestions.innerHTML = "";
                return;
              }
        
              loadAllAirlinesOnce().then(() => {
                const list = filterAirlines(query);
                renderAirlineSuggestions(list);
              });
            }
        
            function handleAirlineAutocompleteKeydown(e) {
              if (airlineSuggestions.style.display === "none") return;
        
              const items = airlineSuggestions.querySelectorAll(".autocomplete-item");
              if (!items.length) return;
        
              if (e.key === "ArrowDown" || e.key === "ArrowUp") {
                e.preventDefault();
                const dir = (e.key === "ArrowDown") ? 1 : -1;
                activeSuggestionIndex =
                  (activeSuggestionIndex + dir + items.length) % items.length;
        
                items.forEach(el => el.classList.remove("active"));
                items[activeSuggestionIndex].classList.add("active");
                items[activeSuggestionIndex].scrollIntoView({ block: "nearest" });
              } else if (e.key === "Enter") {
                if (activeSuggestionIndex >= 0) {
                  e.preventDefault();
                  items[activeSuggestionIndex].click();
                }
              } else if (e.key === "Escape") {
                airlineSuggestions.style.display = "none";
              }
            }
            
            // --- Airline logo helpers ---
            function getAirlineLogoUrl(code) {
              if (!code || code === "\\N") {
                // simple placeholder if unknown
                return "https://placehold.co/40x40/f1f5f9/94a3b8?text=?";
              }
              // Kiwi pattern for airline logos by IATA code
              return "https://images.kiwi.com/airlines/64/" + code.toUpperCase() + ".png";
            }
        
            function createAirlinePill(airlineObj) {
              const code = airlineObj.iata || airlineObj.icao || airlineObj.code || "";
              const name = airlineObj.name || (code ? code : "Unknown airline");
              const label = code ? (name + " (" + code + ")") : name;
        
              const pill = document.createElement("span");
              pill.className = "airline-pill";
        
              const logo = document.createElement("img");
              logo.src = getAirlineLogoUrl(code);
              logo.alt = label + " logo";
              logo.onerror = function () {
                // hide the broken image if logo is missing
                this.style.display = "none";
              };
        
              pill.appendChild(logo);
              pill.appendChild(document.createTextNode(label));
              return pill;
            }
        
        
              function setError(msg) {
                errorEl.textContent = msg || "";
              }
        
              function clearResults() {
                resultsList.innerHTML = "";
              }
         // Optional: known “ceased operations” years for some airlines.
                    // If an airline isn't in this table, we just show a generic message.
                    function getAirlineCeasedYear(airline) {
                      const byIata = {
                        // United Airways (Bangladesh) – ceased 2016
                        "4H": "2016",
                      };
                      const code = (airline.iata || airline.icao || airline.code || "").toUpperCase();
                      return byIata[code] || null;
                    }
        // Helper: Remove Existing Layer - NOW HARDENED
            function clearMapRoutes() {
                if (!routeMapIsLoaded) return;
                
                // 🔸 HARDENING: Check if layer and source exist before removal.
                if (map.getLayer('routes-line')) {
                    map.removeLayer('routes-line');
                }
                if (map.getSource('airline-routes')) {
                    map.removeSource('airline-routes');
                }
            }
        function renderMapRoutes(geoJsonData) {
                        clearMapRoutes(); // Always clear previous routes first
                        
                        if (!routeMapIsLoaded || !geoJsonData.features || geoJsonData.features.length === 0) {
                            // Map is not ready or no routes to draw
                            return; 
                        }

                        // 1. Add the GeoJSON data as a new source
                        map.addSource('airline-routes', {
                            'type': 'geojson',
                            'data': geoJsonData
                        });

                        // 2. Add a layer to display the lines
                        map.addLayer({
                            'id': 'routes-line',
                            'type': 'line',
                            'source': 'airline-routes',
                            'layout': {
                                'line-join': 'round',
                                'line-cap': 'round'
                            },
                            'paint': {
                                'line-color': '#2563eb', // Primary blue color
                                'line-width': 1.5,
                                'line-opacity': 0.7
                            }
                        });

                        // 3. Zoom to fit the new routes (optional but highly recommended)
                        const coordinates = geoJsonData.features.flatMap(f => f.geometry.coordinates);
                        if (coordinates.length > 0) {
                            const bounds = new mapboxgl.LngLatBounds();
                            coordinates.forEach(coord => {
                                // GeoJSON is [lon, lat], Mapbox expects LngLat
                                bounds.extend(coord);
                            });
                            map.fitBounds(bounds, {
                                padding: 20, // Padding around the lines
                                maxZoom: 6,  // Don't zoom in too close
                                duration: 1000 // Smooth animation
                            });
                        } else {
                            // If no routes, reset to world view
                            map.flyTo({ center: INITIAL_CENTER, zoom: INITIAL_ZOOM });
                        }
                    }
        
              function updateViewTabs() {
                viewTabButtons.forEach(btn => {
                  const mode = btn.getAttribute("data-mode");
                  if (mode === viewMode) btn.classList.add("active");
                  else btn.classList.remove("active");
                });
              }
        
              viewTabButtons.forEach(btn => {
                btn.addEventListener("click", () => {
                  const mode = btn.getAttribute("data-mode");
                  if (mode === viewMode) return;
                  viewMode = mode;
                  updateViewTabs();
                  renderForCurrentMode();
                });
              });
        
        function applyAirportFilterForMap(airportCode) {
          selectedAirportCode = airportCode || null;

          if (!routeMapIsLoaded || !lastGeoJsonData || !lastGeoJsonData.features) {
            return;
          }

          // If no airport is selected, show all routes
          if (!selectedAirportCode) {
            renderMapRoutes(lastGeoJsonData);
            return;
          }

          const dir = directionFilter; // "all" | "arrivals" | "departures"

          const filteredFeatures = lastGeoJsonData.features.filter(f => {
            const props = f.properties || {};
            const src = props.src_iata;
            const dst = props.dst_iata;

            if (dir === "arrivals") {
              return dst === selectedAirportCode;
            } else if (dir === "departures") {
              return src === selectedAirportCode;
            } else {
              // all
              return src === selectedAirportCode || dst === selectedAirportCode;
            }
          });

          const filteredGeoJson = {
            type: "FeatureCollection",
            features: filteredFeatures
          };

          renderMapRoutes(filteredGeoJson);
        }

        
          // Build a quick lookup: airport code -> metadata (city, country etc.)
                let airportMetaByCode = {};

                function rebuildAirportMetaIndex(airportsArray) {
                  airportMetaByCode = {};
                  (airportsArray || []).forEach(entry => {
                    const ap = entry.airport || {};
                    const code = ap.iata || ap.icao;
                    if (!code) return;
                    airportMetaByCode[code] = ap;
                  });
                }

                // Given an airport code, compute arrivals and departures frequencies
                function computeAirportFlows(airportCode, maxRows = Infinity) {
                  const result = {
                    arrivals: [],    // [{ other: "SFO", count: N }, ...]
                    departures: []   // [{ other: "SFO", count: N }, ...]
                  };

                  if (
                    !airportCode ||
                    !lastGeoJsonData ||
                    !Array.isArray(lastGeoJsonData.features)
                  ) {
                    return result;
                  }

                  const arrivalsMap = new Map();
                  const departuresMap = new Map();

                  lastGeoJsonData.features.forEach(f => {
                    const props = f.properties || {};
                    const src = props.src_iata;
                    const dst = props.dst_iata;
                    if (!src || !dst) return;

                    // one row per route in the underlying dataset
                    if (src === airportCode) {
                      departuresMap.set(dst, (departuresMap.get(dst) || 0) + 1);
                    }
                    if (dst === airportCode) {
                      arrivalsMap.set(src, (arrivalsMap.get(src) || 0) + 1);
                    }
                  });
        function buildAirportFlowsDetail(airportCode, flows) {
          const wrapper = document.createElement("div");
          wrapper.className = "airport-flows";

          function makeSection(title, list) {
            if (!list || !list.length) return;

            const section = document.createElement("div");
            section.className = "flow-section";

            const heading = document.createElement("div");
            heading.className = "flow-section-title";
            heading.textContent = title;
            section.appendChild(heading);

            const row = document.createElement("div");
            row.className = "flows-row";

            // 👉 12 codes per column
            const columns = chunkArray(list, 12);
            columns.forEach(colItems => {
              const col = document.createElement("div");
              col.className = "flows-col";

              colItems.forEach(item => {
                const div = document.createElement("div");
                div.className = "flow-item";
                // no "1 route" text, just the code
                div.textContent = item.code || "???";
                col.appendChild(div);
              });

              row.appendChild(col);
            });

            section.appendChild(row);
            wrapper.appendChild(section);
          }

          makeSection("ARRIVALS (TO " + airportCode + ")", flows.arrivals);
          makeSection("DEPARTURES (FROM " + airportCode + ")", flows.departures);

          return wrapper;
        }

                  function mapToSortedArray(map) {
                    return Array.from(map.entries())
                      .sort((a, b) => b[1] - a[1])       // most frequent first
                      .slice(0, maxRows)                 // cap to avoid huge lists
                      .map(([other, count]) => ({ other, count }));
                  }

                  result.arrivals = mapToSortedArray(arrivalsMap);
                  result.departures = mapToSortedArray(departuresMap);
                  return result;
                }

               // Render arrivals/departures into the details element of a card using a flexible grid
               function populateAirportDetails(airportCode, detailsEl) {
                 if (!detailsEl) return;

                 const { arrivals, departures } = computeAirportFlows(airportCode);

                 if (!arrivals.length && !departures.length) {
                   detailsEl.innerHTML =
                     '<div class="airport-details-empty">No detailed routes in the dataset for this airport.</div>';
                   return;
                 }

                 // Same helper as before: City (CODE) if we know the city
                 function formatAirportLabel(code) {
                   const meta = airportMetaByCode[code];
                   if (!meta) return code;
                   const city = meta.city || "";
                   if (city) return city + " (" + code + ")";
                   return code;
                 }

                 const parts = [];

                 function renderSection(title, list) {
                   if (!list || !list.length) return;

                   parts.push('<div class="airport-flow-section">');
                   parts.push('<div class="airport-flow-title">' + title + '</div>');
                   parts.push('<div class="flows-grid">');

                   list.forEach(({ other }) => {
                     parts.push(
                       '<div class="flow-item">' +
                         formatAirportLabel(other) +
                       '</div>'
                     );
                   });

                   parts.push('</div>');  // .flows-grid
                   parts.push('</div>');  // .airport-flow-section
                 }

                 renderSection('Arrivals (to ' + airportCode + ')', arrivals);
                 renderSection('Departures (from ' + airportCode + ')', departures);

                 detailsEl.innerHTML = parts.join('');
               }

        
                                         function renderCards(data) {
                                           clearResults();

                                           if (!data || !data.airline) {
                                             resultsList.innerHTML =
                                               '<p class="no-results">No results to display yet.</p>';
                                             return;
                                           }

                                           const airline = data.airline;
                                           const airports = data.airports || [];
                                        // Rebuild airport metadata index so flows can show city/country
                                        rebuildAirportMetaIndex(airports);

                                           // Total airports (before limiting) from last fetch
                                           const totalInDataset =
                                             (typeof lastTotalAirports === "number" && lastTotalAirports > 0)
                                               ? lastTotalAirports
                                               : airports.length;

                                           // Sum of routes across all airports
                                           const totalRoutes = airports.reduce((sum, entry) => {
                                             const c = entry && typeof entry.routes === "number" ? entry.routes : 0;
                                             return sum + c;
                                           }, 0);

                                           // --- Airline summary card at top ---
                                           const summary = document.createElement("div");
                                           summary.className = "route-card";

                                           const code = airline.iata || airline.icao || "";

                                           summary.innerHTML =
                                             '<div class="route-main-line">' +
                                               '<span class="__airline-pill-slot"></span>' +
                                               '<div class="airline-summary-right">' +
                                                 '<span class="status-pill __status-pill-slot">Active = True</span>' +
                                               '</div>' +
                                             '</div>' +
                                             // NEW: separate line just for the ceased-operations chip
                                             '<div class="route-sub-line">' +
                                               '<div class="ceased-line"></div>' +
                                               '<div class="meta-chips-row">' +
                                                 (airline.country
                                                   ? '<span class="meta-chip country-chip">Country: ' + airline.country + '</span>'
                                                   : '') +
                                                 (airline.iata
                                                   ? '<span class="meta-chip iata-chip">IATA: ' + airline.iata + '</span>'
                                                   : '') +
                                                 (airline.icao
                                                   ? '<span class="meta-chip icao-chip">ICAO: ' + airline.icao + '</span>'
                                                   : '') +
                                                 '<span class="meta-chip totals-chip">Airports: ' + totalInDataset + '</span>' +
                                                 '<span class="meta-chip totals-chip">Routes: ' + totalRoutes + '</span>' +
                                               '</div>' +
                                             '</div>';

                                           // Insert shaded logo pill
                                           const pill = createAirlinePill({
                                             name: airline.name,
                                             iata: airline.iata,
                                             icao: airline.icao,
                                             code: code
                                           });
                                           summary.querySelector(".__airline-pill-slot").replaceWith(pill);

                                           // Wire up Active = True/False status chip
                                           const statusEl = summary.querySelector(".__status-pill-slot");
                                           if (statusEl) {
                                             const isActive = !!airline.active;
                                             const statusText = isActive ? "Active = True" : "Active = False";
                                             statusEl.textContent = statusText;
                                             statusEl.className = "status-pill " + (isActive ? "active" : "inactive");

                                             // If inactive, show a “ceased operations” chip on its own line
                                             if (!isActive) {
                                               const ceasedYear = getAirlineCeasedYear(airline);
                                               const ceasedMsg = ceasedYear
                                                 ? ("This airline ceased operations in " + ceasedYear)
                                                 : "This airline has ceased operations";

                                               const ceasedTag = document.createElement("span");
                                               ceasedTag.className = "route-tag ceased";
                                               ceasedTag.textContent = ceasedMsg;

                                               const ceasedLine = summary.querySelector(".ceased-line");
                                               if (ceasedLine) {
                                                 ceasedLine.appendChild(ceasedTag);
                                               }
                                             }
                                           }

                                           resultsList.appendChild(summary);

                                           // If no airports, show the friendly message and stop
                                           if (!airports.length) {
                                             const msg = document.createElement("p");
                                             msg.className = "no-results";
                                             msg.textContent = "This airline has no routes in the dataset.";
                                             resultsList.appendChild(msg);
                                             return;
                                           }

                                           airports.forEach(function (entry) {
                                             const ap = entry.airport || {};
                                             const count = entry.routes || 0;

                                             const city = ap.city || "";
                                             const country = ap.country || "";
                                             const iata = ap.iata || ap.icao || "???";

                                             const card = document.createElement("div");
                                             card.className = "route-card airport-card";
                                             card.dataset.airportCode = iata;

                                             card.innerHTML =
                                                '<div class="airport-main-line">' +
                                                '<div>' +
                                                '<div class="airport-name">' +
                                                  (city ? city : "Unknown city") +
                                                  ' (' + iata + ')' +
                                                '</div>' +
                                                (country
                                                  ? '<div class="airport-subline">' + country + '</div>'
                                                  : '') +
                                              '</div>' +
                                              '<span class="route-tag">' + count + ' route' +
                                                (count === 1 ? '' : 's') + '</span>' +
                                            '</div>' +
                                            '<div class="airport-details"></div>';

                                          card.addEventListener("click", () => {
                                            const alreadySelected = card.classList.contains("selected");

                                            // Collapse any other selected/expanded airport
                                            document
                                              .querySelectorAll(".airport-card.selected")
                                              .forEach(el => {
                                                el.classList.remove("selected");
                                                const d = el.querySelector(".airport-details");
                                                if (d) {
                                                  d.innerHTML = "";
                                                  d.style.display = "none";
                                                }
                                              });

                                            if (alreadySelected) {
                                              // Turn this one off + reset map to all routes
                                              card.classList.remove("selected");
                                              selectedAirportCode = null;
                                              applyAirportFilterForMap(null);   // will call renderMapRoutes(lastGeoJsonData)
                                              return;
                                            }

                                            // Select this card
                                            card.classList.add("selected");
                                            selectedAirportCode = iata;
                                            applyAirportFilterForMap(iata);

                                            // Ensure we have a details container
                                            let detailsEl = card.querySelector(".airport-details");
                                            if (!detailsEl) {
                                              detailsEl = document.createElement("div");
                                              detailsEl.className = "airport-details";
                                              card.appendChild(detailsEl);
                                            }

                                            // Fill it with the nice multi-column flows layout
                                            populateAirportDetails(iata, detailsEl);
                                            detailsEl.style.display = "block";
                                          });

                                          resultsList.appendChild(card);

                                           });

                                         }


        
              function renderJson() {
                clearResults();
        
                if (!lastData) {
                  resultsList.innerHTML =
                    '<p class="no-results">(run a search to see JSON)</p>';
                  return;
                }
        
                const jsonStr = JSON.stringify(lastData, null, 2).replace(/</g, "&lt;");
        
                resultsList.innerHTML =
                  '<pre style="background:#020617;color:#e5e7eb;' +
                  'padding:0.75rem;border-radius:0.5rem;font-size:0.8rem;' +
                  'overflow:auto;max-height:24rem;">' +
                  jsonStr +
                  '</pre>';
              }
        
              function renderForCurrentMode() {
                if (viewMode === "json") renderJson();
                else renderCards(lastData);
              }
            // Wire autocomplete to the input
            airlineInput.addEventListener("input", handleAirlineAutocompleteInput);
            airlineInput.addEventListener("keydown", handleAirlineAutocompleteKeydown);
        
            // Hide suggestions when clicking outside
            document.addEventListener("click", (e) => {
              if (!e.target.closest(".autocomplete-container")) {
                airlineSuggestions.style.display = "none";
              }
            });
        
              form.addEventListener("submit", function (e) {
                e.preventDefault();
                setError("");
        
                let raw = (airlineInput.value || "").trim();
                if (!raw) {
                  setError("Please enter an airline (e.g. AA, DL, UA or 'American').");
                  return;
                }

                // ✅ Prefer the ICAO of the airline actually chosen from the dropdown
                let code = null;
                if (selectedAirline) {
                  const labelFromSelected = airlineDisplayLabel(selectedAirline);
                  if (raw === labelFromSelected) {
                    // If the airline has an ICAO (3-letter, unique), use that first
                    if (selectedAirline.icao && selectedAirline.icao.length === 3) {
                      code = selectedAirline.icao.toUpperCase();
                    } else if (selectedAirline.iata) {
                      code = selectedAirline.iata.toUpperCase();
                    }
                  }
                }

                // Fallback: parse from text if we didn't get a code from selectedAirline
                if (!code) {
                  // If the user picked "American Airlines (AA)", extract AA from parentheses
                  const m = raw.match(/\(([A-Z0-9]{2,3})\)\s*$/i);
                  if (m) {
                    code = m[1].toUpperCase();
                  } else {
                    const parts = raw.toUpperCase().split(/\s+/);
                    code = parts[0];
                  }
                }
        
                let limit = parseInt((limitInput.value || "").trim(), 10);
                if (isNaN(limit) || limit <= 0) {
                  limit = 50;
                  limitInput.value = "50";
                }
        
                clearResults();
                                clearMapRoutes(); // NEW: Clear the map on new search
                                
                                resultsList.innerHTML = '<p class="no-results">Loading airline routes…</p>';

                                // 1. Fetch JSON for CARD VIEW (existing endpoint)
                                const cardDataPromise = fetch("/airline/routes?code=" + encodeURIComponent(code))
                                    .then(resp => {
                                        if (!resp.ok) {
                                            return resp.text().then(text => { throw new Error("Card data failed: " + resp.status + ": " + text); });
                                        }
                                        return resp.json();
                                    });

                                // 2. Fetch GeoJSON for MAP VIEW (new endpoint)
                                const geoJsonPromise = fetch("/api/routes/geojson?airline=" + encodeURIComponent(code))
                                    .then(resp => {
                                        if (!resp.ok) {
                                            // If GeoJSON fails, it's not a fatal error, but we log it.
                                            console.warn("GeoJSON fetch failed:", resp.status);
                                            return { features: [] }; // return empty GeoJSON
                                        }
                                        return resp.json();
                                    });

                                // 3. Process both in parallel
                               Promise.all([cardDataPromise, geoJsonPromise])
                                 .then(([cardData, geoJsonData]) => {
                                   if (cardData && typeof cardData === "object" && cardData.error) {
                                     setError("Server error: " + cardData.error);
                                     clearResults();
                                     return;
                                   }

                                   // A. Remember the full GeoJSON so we can filter by airport later
                                   lastGeoJsonData =
                                     geoJsonData && typeof geoJsonData === "object"
                                       ? geoJsonData
                                       : { type: "FeatureCollection", features: [] };

                                   // Reset any previous airport selection
                                   selectedAirportCode = null;

                                   // B. Draw all routes initially
                                   renderMapRoutes(lastGeoJsonData);

                                   // C. Render the cards (with our updated summary + airport cards)
                                   const airportsArr = Array.isArray(cardData.airports) ? cardData.airports : [];
                                   lastTotalAirports = airportsArr.length;

                                   let limitedAirports = airportsArr;
                                   if (airportsArr.length > limit) {
                                     limitedAirports = airportsArr.slice(0, limit);
                                   }

                                   lastData = Object.assign({}, cardData, { airports: limitedAirports });

                                   viewMode = "cards";
                                   updateViewTabs();
                                   renderCards(lastData);
                                 })
                                 .catch(err => {
                                   console.error(err);
                                   setError("Request failed. Please check the console.");
                                   clearResults();
                                 });
                                    
                            });
            })();
          </script>
        </body>
        </html>
        )HTML";
            
            
            
            response = buildHttpResponse(body, "text/html; charset=UTF-8");
        }
        
        else if (pathOnly == "/about-ui" || pathOnly == "/about-ui/") {
            std::string body = R"HTML(
                <!DOCTYPE html>
                <html lang="en">
                <head>
                  <meta charset="UTF-8" />
                  <title>About This OpenFlights Project</title>
                  <meta name="viewport" content="width=device-width, initial-scale=1" />
                  <style>
                    body {
                      font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
                      margin: 0;
                      padding: 1.5rem;
                      background: #f5f5f5;
                      color: #222;
                    }
                    .container {
                      max-width: 880px;
                      margin: 0 auto;
                      background: #ffffff;
                      border-radius: 12px;
                      padding: 1.5rem 1.75rem 1.75rem;
                      box-shadow: 0 2px 8px rgba(0, 0, 0, 0.08);
                    }
                    .header-bar {
                      display: flex;
                      justify-content: space-between;
                      align-items: center;
                      gap: 1rem;
                      margin-bottom: 0.75rem;
                    }
                    .header-left h1 {
                      margin: 0;
                      font-size: 1.7rem;
                    }
                    .subtitle {
                      font-size: 0.85rem;
                      color: #6b7280;
                      margin-top: 0.15rem;
                    }
                    .header-right {
                      display: flex;
                      flex-wrap: wrap;
                      gap: 0.4rem;
                      justify-content: flex-end;
                    }
                    .header-right a {
                      font-size: 0.8rem;
                      text-decoration: none;
                      padding: 0.35rem 0.65rem;
                      border-radius: 999px;
                      border: 1px solid #e5e7eb;
                      color: #374151;
                      background: #f9fafb;
                      white-space: nowrap;
                    }
                    .header-right a:hover {
                      background: #e5e7eb;
                    }

                    /* Cards & layout, matching other pages */
                    .cards-grid {
                      display: flex;
                      flex-direction: column;
                      gap: 0.75rem;
                      margin-top: 0.75rem;
                    }
                    .card {
                      background: #ffffff;
                      border-radius: 12px;
                      padding: 0.9rem 1rem;
                      box-shadow: 0 1px 4px rgba(15, 23, 42, 0.08);
                      border: 1px solid #e5e7eb;
                      font-size: 0.93rem;
                    }
                    .card-header {
                      display: flex;
                      justify-content: space-between;
                      align-items: center;
                      margin-bottom: 0.35rem;
                    }
                    .card-title {
                      font-size: 0.95rem;
                      font-weight: 600;
                      color: #111827;
                    }
                    .card-body {
                      font-size: 0.85rem;
                      color: #374151;
                    }
                    .section-label {
                      font-size: 0.8rem;
                      font-weight: 600;
                      text-transform: uppercase;
                      letter-spacing: 0.06em;
                      color: #9ca3af;
                      margin-bottom: 0.35rem;
                    }
                    .card-body p {
                      margin: 0 0 0.45rem 0;
                    }
                    .card-body ul {
                      margin: 0.15rem 0 0.45rem 1.1rem;
                      padding: 0;
                      font-size: 0.85rem;
                    }
                    .card-body li {
                      margin-bottom: 0.15rem;
                    }

                    .pill {
                      font-size: 0.72rem;
                      font-weight: 600;
                      padding: 0.1rem 0.55rem;
                      border-radius: 999px;
                      white-space: nowrap;
                      background: #e8f0fe;
                      color: #174ea6;
                    }

                    .btn {
                      padding: 0.45rem 0.9rem;
                      border-radius: 999px;
                      border: none;
                      font-size: 0.95rem;
                      font-weight: 600;
                      cursor: pointer;
                      display: inline-flex;
                      align-items: center;
                      gap: 0.25rem;
                    }
                    .btn-primary {
                      background: #2563eb;
                      color: #ffffff;
                    }
                    .btn-primary:hover {
                      background: #1d4ed8;
                    }

                    .meta-row {
                      font-size: 0.8rem;
                      color: #6b7280;
                      margin-top: 0.25rem;
                    }
                  </style>
                </head>
                <body>
                  <div class="container">
                    <div class="header-bar">
                      <div class="header-left">
                        <h1>About This OpenFlights Project</h1>
                        <p class="subtitle">
                          Capstone-style C++ project: custom HTTP server, OpenFlights data, and interactive route explorers.
                        </p>
                      </div>
                      <div class="header-right">
                        <a href="/">← Back to Route Finder</a>
                        <a href="/airline-ui">Airline Routes</a>
                        <a href="/airport-ui">Airport Airlines</a>
                        <a href="/update-ui">Update Data</a>
                      </div>
                    </div>

                    <div class="cards-grid">

                      <!-- Project Summary -->
                      <div class="card">
                        <div class="card-header">
                          <span class="card-title">Project Summary</span>
                          <span class="pill">OpenFlights Explorer</span>
                        </div>
                        <div class="card-body">
                          <p>
                            This project is an OpenFlights-based airline and airport route explorer built on top of a
                            hand-rolled C++ HTTP server. It exposes both JSON APIs and interactive HTML pages that let you
                            explore the global route network from multiple angles.
                          </p>
                          <p class="section-label">Data sources</p>
                          <ul>
                            <li><code>airports.dat</code> &mdash; airport metadata (codes, names, cities, countries, coordinates).</li>
                            <li><code>airlines.dat</code> &mdash; airline metadata (IATA/ICAO codes, names, countries, active flag).</li>
                            <li><code>routes.dat</code> &mdash; individual routes linking airlines with source and destination airports.</li>
                          </ul>
                          <p class="section-label">High-level features</p>
                          <ul>
                            <li><strong>Airport-centric route discovery</strong> via <code>/airport-ui</code> &mdash; start from an airport and see who flies there.</li>
                            <li><strong>Airline-centric route discovery</strong> via <code>/airline-ui</code> &mdash; start from an airline and see all the airports it serves.</li>
                            <li><strong>Route Finder home page</strong> (<code>/</code>) for source/destination route search with rich filtering.</li>
                            <li><strong>Mapbox-powered route visualization</strong> so matching routes are drawn directly on an interactive map.</li>
                            <li><strong>Cards / JSON toggle mode</strong> that lets you switch between human-readable cards and raw JSON.</li>
                            <li><strong>Clickable cards with detailed expansions</strong> for airports, airlines, and routes.</li>
                            <li><strong>Fast route filtering</strong> using in-memory indexes instead of scanning the entire dataset on each request.</li>
                          </ul>
                        </div>
                      </div>

                      <!-- Technical Features -->
                      <div class="card">
                        <div class="card-header">
                          <span class="card-title">Technical Architecture & Features</span>
                          <span class="pill">C++17 • HTTP • GeoJSON</span>
                        </div>
                        <div class="card-body">
                          <p>
                            The backend is implemented as a custom C++17 HTTP server (no external web frameworks). It listens on a TCP
                            socket, parses incoming HTTP requests, routes them by path, and returns either JSON or fully rendered HTML.
                          </p>
                          <p class="section-label">Backend internals</p>
                          <ul>
                            <li><strong>Custom HTTP server:</strong> request line parsing, header parsing, and simple routing using <code>pathOnly</code> checks.</li>
                            <li><strong>In-memory indexes</strong> over OpenFlights data, including structures such as:
                              <ul>
                                <li><code>g_routesByAirlineId</code> &mdash; all routes for a given airline.</li>
                                <li><code>g_routesBySrcAirport</code> &mdash; routes grouped by source airport.</li>
                                <li><code>g_routesByDstAirport</code> &mdash; routes grouped by destination airport.</li>
                              </ul>
                            </li>
                            <li><strong>Efficient lookups:</strong> user requests are answered by querying these maps instead of re-reading the files.</li>
                            <li><strong>GeoJSON generation:</strong> endpoints produce GeoJSON FeatureCollections for routes, which the frontend uses to draw lines on the map.</li>
                          </ul>

                          <p class="section-label">Frontend behavior</p>
                          <ul>
                            <li><strong>Vanilla JavaScript</strong> for all client-side behavior (no frontend frameworks).</li>
                            <li><strong>Mapbox GL JS integration</strong> to render route lines, highlight selected routes, and synchronize card interactions with the map.</li>
                            <li><strong>Card-based UI</strong> built with reusable styles such as <code>.card</code>, <code>.card-header</code>, and <code>.card-body</code>.</li>
                            <li><strong>JSON mode</strong> that reuses the same data but renders it inside a scrollable, syntax-like mono-spaced block.</li>
                          </ul>
                          <p class="meta-row">
                            The goal is to keep the codebase small, readable, and focused on data structures, algorithms, and HTTP fundamentals.
                          </p>
                        </div>
                      </div>

                      <!-- Collaboration with AI -->
                      <div class="card">
                        <div class="card-header">
                          <span class="card-title">Collaboration with AI (Pair Programming)</span>
                          <span class="pill">Human + ChatGPT</span>
                        </div>
                        <div class="card-body">
                          <p>
                            This project was developed with AI (ChatGPT) acting as a pair programmer and design collaborator. The AI
                            assisted with brainstorming, drafting code, and refining UI layouts, while the human developer wrote,
                            integrated, and tested the final implementation in C++ and JavaScript.
                          </p>
                          <p class="section-label">How AI helped</p>
                          <ul>
                            <li><strong>UI layout & cards:</strong> iterating on the overall look and feel, including headers, cards, chips, and JSON panels.</li>
                            <li><strong>Mapbox integration:</strong> wiring Mapbox GL JS into the pages, defining GeoJSON layers, and syncing card clicks with map highlights.</li>
                            <li><strong>Interactive features:</strong> designing expandable cards, “Cards vs JSON” toggle modes, autocomplete fields, and status chips.</li>
                            <li><strong>Server endpoints & helpers:</strong> suggesting helper APIs (e.g., GeoJSON endpoints) and data structures that fit the existing C++ design.</li>
                            <li><strong>Debugging & refactoring:</strong> helping reason about edge cases, clarify error handling, and improve prompt wording when iterating on features.</li>
                          </ul>
                          <p class="section-label">Human responsibilities</p>
                          <ul>
                            <li>Owning the repository, build, and toolchain setup (Xcode, Docker, Fly.io, etc.).</li>
                            <li>Deciding on the overall architecture and data structures for indexes and GeoJSON generation.</li>
                            <li>Reviewing AI-generated suggestions, integrating them into the actual codebase, and adapting them to course requirements.</li>
                            <li>Testing the server with real OpenFlights data and adjusting features based on performance and usability.</li>
                          </ul>
                          <p class="meta-row">
                            The result is a capstone-style project that blends traditional data structures and systems programming
                            with modern AI-assisted development and frontend mapping tools.
                          </p>
                        </div>
                      </div>

                      <!-- How to explore the UI -->
                      <div class="card">
                        <div class="card-header">
                          <span class="card-title">How to Explore This Interface</span>
                        </div>
                        <div class="card-body">
                          <ul>
                            <li><strong>Route Finder (<code>/</code>)</strong> &mdash; search for routes between two airports, filter, and visualize paths on the map.</li>
                            <li><strong>Airline Routes Explorer (<code>/airline-ui</code>)</strong> &mdash; start from an airline and explore all of its airports and routes.</li>
                            <li><strong>Airport Airlines Explorer (<code>/airport-ui</code>)</strong> &mdash; start from an airport and see which airlines operate there.</li>
                            <li><strong>Update UI (<code>/update-ui</code>)</strong> &mdash; experiment with inserting, modifying, or removing airports, airlines, and routes.</li>
                            <li><strong>About UI (<code>/about-ui</code>)</strong> &mdash; this page, documenting the project, technical choices, and collaboration with AI.</li>
                          </ul>
                        </div>
                      </div>

                    </div>
                  </div>
                </body>
                </html>
            )HTML";

            response = buildHttpResponse(body, "text/html; charset=UTF-8");
        }
        else if (pathOnly == "/update-ui" || pathOnly == "/update-ui/") {
            std::string body = R"HTML(
        <!DOCTYPE html>
        <html lang="en">
        <head>
          <meta charset="UTF-8" />
          <title>Update OpenFlights Data</title>
          <meta name="viewport" content="width=device-width, initial-scale=1" />
          <style>
            body {
              font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
              margin: 0;
              padding: 1.5rem;
              background: #f5f5f5;
              color: #222;
            }
            .container {
              max-width: 880px;
              margin: 0 auto;
              background: #ffffff;
              border-radius: 12px;
              padding: 1.5rem 1.75rem 1.75rem;
              box-shadow: 0 2px 8px rgba(0, 0, 0, 0.08);
            }
            .header-bar {
              display: flex;
              justify-content: space-between;
              align-items: center;
              gap: 1rem;
              margin-bottom: 0.75rem;
            }
            .header-left h1 {
              margin: 0;
              font-size: 1.7rem;
            }
            .subtitle {
              font-size: 0.85rem;
              color: #6b7280;
              margin-top: 0.15rem;
            }
            .header-right a {
              font-size: 0.8rem;
              text-decoration: none;
              padding: 0.35rem 0.65rem;
              border-radius: 999px;
              border: 1px solid #e5e7eb;
              color: #374151;
              background: #f9fafb;
            }
            .header-right a:hover {
              background: #e5e7eb;
            }

            form {
              display: grid;
              gap: 0.75rem;
              margin-bottom: 1.25rem;
            }
            .field-group {
              display: flex;
              flex-wrap: wrap;
              gap: 0.75rem;
            }
            .field {
              display: flex;
              flex-direction: column;
              min-width: 140px;
              flex: 1 1 0;
            }
            .field label {
              font-size: 0.8rem;
              font-weight: 600;
              margin-bottom: 0.15rem;
              color: #374151;
            }
            .field input,
            .field select,
            .field textarea {
              border-radius: 8px;
              border: 1px solid #d1d5db;
              padding: 0.45rem 0.55rem;
              font-size: 0.9rem;
            }

            .btn {
              border-radius: 999px;
              padding: 0.45rem 1.1rem;
              font-size: 0.9rem;
              border: none;
              cursor: pointer;
              font-weight: 600;
            }
            .btn-primary {
              background: #2563eb;
              color: #f9fafb;
            }
            .btn-primary:hover {
              background: #1d4ed8;
            }

            .badge {
              display: inline-flex;
              align-items: center;
              padding: 0.2rem 0.6rem;
              border-radius: 999px;
              font-size: 0.75rem;
              font-weight: 600;
              background: #e5e7eb;
              color: #374151;
            }
            .badge-entity {
              background: #eef2ff;
              color: #312e81;
            }
            .badge-op {
              background: #eff6ff;
              color: #1d4ed8;
              margin-left: 0.35rem;
            }

            .hint {
              font-size: 0.8rem;
              color: #6b7280;
            }

            #error {
              margin-top: 0.25rem;
              font-size: 0.85rem;
              color: #b91c1c;
            }

            .results-list {
              display: grid;
              gap: 0.75rem;
            }
            .card {
              border-radius: 10px;
              border: 1px solid #e5e7eb;
              padding: 0.9rem 1rem;
              background: #f9fafb;
            }
            .card-header {
              display: flex;
              justify-content: space-between;
              align-items: center;
              margin-bottom: 0.35rem;
            }
            .card-title {
              font-size: 0.95rem;
              font-weight: 600;
              color: #111827;
            }
            .card-body {
              font-size: 0.85rem;
              color: #374151;
            }
            .card-body dl {
              display: grid;
              grid-template-columns: minmax(0, 120px) minmax(0, 1fr);
              row-gap: 0.15rem;
              column-gap: 0.75rem;
              margin: 0;
            }
            .card-body dt {
              font-weight: 600;
              color: #6b7280;
            }
            .card-body dd {
              margin: 0;
              word-break: break-word;
            }

            .card-success {
              border-color: #bbf7d0;
              background: #ecfdf3;
            }
            .card-error {
              border-color: #fecaca;
              background: #fef2f2;
              color: #b91c1c;
            }
            .muted {
              color: #6b7280;
            }

            .section-label {
              font-size: 0.8rem;
              font-weight: 600;
              text-transform: uppercase;
              letter-spacing: 0.06em;
              color: #9ca3af;
              margin-bottom: 0.35rem;
            }

            .checkbox-row {
              display: flex;
              align-items: center;
              gap: 0.35rem;
              margin-top: 0.15rem;
            }
            .checkbox-row label {
              margin: 0;
              font-size: 0.8rem;
              font-weight: 500;
            }

            /* Autocomplete */
            .autocomplete-container {
              position: relative;
            }
            .autocomplete-list {
              position: absolute;
              top: 100%;
              left: 0;
              right: 0;
              z-index: 20;
              max-height: 14rem;
              overflow-y: auto;
              margin-top: 0.25rem;
              background: #ffffff;
              border-radius: 0.5rem;
              border: 1px solid #e5e7eb;
              box-shadow: 0 10px 25px rgba(15,23,42,0.15);
              display: none;
            }
            .autocomplete-item {
              padding: 0.4rem 0.6rem;
              font-size: 0.9rem;
              cursor: pointer;
            }
            .autocomplete-item:hover,
            .autocomplete-item.active {
              background: #f3f4f6;
            }
            .autocomplete-empty {
              padding: 0.4rem 0.6rem;
              font-size: 0.85rem;
              color: #6b7280;
            }
          </style>
        </head>
        <body>
          <div class="container">
            <div class="header-bar">
              <div class="header-left">
                <h1>Update OpenFlights Data</h1>
                <p class="subtitle">
                  Insert, modify, or remove Airports, Airlines, and Routes. Changes are in-memory only
                  and immediately affect the /airport-ui, /airline-ui, and JSON APIs.
                </p>
              </div>
              <div class="header-right">
                <a href="/">← Back to Route Finder</a>
              </div>
            </div>

            <form id="update-form">
              <div class="field-group">
                <div class="field" style="max-width: 220px;">
                  <label for="entity">Entity Type</label>
                  <select id="entity" name="entity">
                    <option value="airport">Airport</option>
                    <option value="airline">Airline</option>
                    <option value="route">Route</option>
                  </select>
                </div>
                <div class="field" style="max-width: 220px;">
                  <label for="operation">Operation</label>
                  <select id="operation" name="operation">
                    <option value="insert">Insert</option>
                    <option value="modify">Modify</option>
                    <option value="remove">Remove</option>
                  </select>
                </div>
              </div>

              <div class="section-label">Fields</div>
              <div id="dynamicFields"></div>

              <button type="submit" class="btn btn-primary">
                Apply Update
              </button>
              <div id="error"></div>
              <div class="hint">
                This calls the <code>/update</code> JSON API:
                <code>/update?entity=airport&amp;operation=modify&amp;…</code>
              </div>
            </form>

            <div class="section-label">Result</div>
            <div id="updateResults" class="results-list">
              <div class="card muted">
                <div class="card-header">
                  <span class="card-title">No updates yet</span>
                </div>
                <div class="card-body">
                  Submit an update to see the <strong>Before</strong> and <strong>After</strong> cards here.
                </div>
              </div>
            </div>
          </div>

          <script>
            (function () {
              const form         = document.getElementById("update-form");
              const entitySelect = document.getElementById("entity");
              const opSelect     = document.getElementById("operation");
              const fieldsRoot   = document.getElementById("dynamicFields");
              const errorEl      = document.getElementById("error");
              const resultsRoot  = document.getElementById("updateResults");

              let activeList = null;
              let activeIndex = -1;

              // Safety check: if these are null, nothing else will work
              if (!form || !entitySelect || !opSelect || !fieldsRoot || !resultsRoot) {
                console.error("Update UI: one or more required elements not found");
                return;
              }

              function renderFields() {
                const entity = entitySelect.value;
                console.log("renderFields called for entity:", entity); // DEBUG
                let html = "";

                if (entity === "airport") {
                  html += `
                    <div class="field-group">
                      <div class="field autocomplete-container">
                        <label>Search Airport (name or IATA)</label>
                        <input id="searchAirportInput" type="text"
                               placeholder="Start typing an airport…" autocomplete="off" />
                        <div id="searchAirportList" class="autocomplete-list"></div>
                        <div class="hint">
                          Select an airport to auto-fill ID and details (handy for modify/remove).
                        </div>
                      </div>
                    </div>
                    <div class="field-group">
                      <div class="field" style="max-width:150px;">
                        <label>Airport ID (required for modify/remove)</label>
                        <input name="id" type="number" placeholder="e.g. 3364" />
                      </div>
                      <div class="field" style="max-width:150px;">
                        <label>IATA code</label>
                        <input name="iata" maxlength="3" placeholder="e.g. SJC" />
                      </div>
                      <div class="field">
                        <label>Name</label>
                        <input name="name" placeholder="e.g. San Jose International" />
                      </div>
                    </div>
                    <div class="field-group">
                      <div class="field">
                        <label>City</label>
                        <input name="city" placeholder="e.g. San Jose" />
                      </div>
                      <div class="field">
                        <label>Country</label>
                        <input name="country" placeholder="e.g. United States" />
                      </div>
                      <div class="field">
                        <label>ICAO</label>
                        <input name="icao" maxlength="4" placeholder="e.g. KSJC" />
                      </div>
                    </div>
                    <div class="field-group">
                      <div class="field">
                        <label>Latitude</label>
                        <input name="latitude" type="text" placeholder="e.g. 37.3626" />
                      </div>
                      <div class="field">
                        <label>Longitude</label>
                        <input name="longitude" type="text" placeholder="-121.9290" />
                      </div>
                      <div class="field">
                        <label>Altitude (ft)</label>
                        <input name="altitude" type="number" />
                      </div>
                    </div>
                    <div class="field-group">
                      <div class="field">
                        <label>Timezone offset</label>
                        <input name="timezone" type="text" placeholder="-8" />
                      </div>
                      <div class="field">
                        <label>DST</label>
                        <input name="dst" maxlength="2" placeholder="e.g. U" />
                      </div>
                      <div class="field">
                        <label>TZDB timezone</label>
                        <input name="tzdbTimezone" placeholder="e.g. America/Los_Angeles" />
                      </div>
                    </div>
                    <div class="field-group">
                      <div class="field">
                        <label>Type</label>
                        <input name="type" placeholder="airport / station / port" />
                      </div>
                      <div class="field">
                        <label>Source</label>
                        <input name="source" placeholder="e.g. OurAirports" />
                      </div>
                    </div>
                  `;
                } else if (entity === "airline") {
                  html += `
                    <div class="field-group">
                      <div class="field autocomplete-container">
                        <label>Search Airline (name or code)</label>
                        <input id="searchAirlineInput" type="text"
                               placeholder="Start typing an airline…" autocomplete="off" />
                        <div id="searchAirlineList" class="autocomplete-list"></div>
                        <div class="hint">
                          Select an airline to auto-fill ID and details (handy for modify/remove).
                        </div>
                      </div>
                    </div>
                    <div class="field-group">
                      <div class="field" style="max-width:150px;">
                        <label>Airline ID (required for modify/remove)</label>
                        <input name="id" type="number" placeholder="e.g. 5209" />
                      </div>
                      <div class="field" style="max-width:150px;">
                        <label>IATA code</label>
                        <input name="iata" maxlength="3" placeholder="e.g. AA" />
                      </div>
                      <div class="field">
                        <label>Name</label>
                        <input name="name" placeholder="e.g. American Airlines" />
                      </div>
                    </div>
                    <div class="field-group">
                      <div class="field">
                        <label>Alias</label>
                        <input name="alias" />
                      </div>
                      <div class="field">
                        <label>ICAO</label>
                        <input name="icao" maxlength="4" placeholder="e.g. AAL" />
                      </div>
                      <div class="field">
                        <label>Callsign</label>
                        <input name="callsign" placeholder="e.g. AMERICAN" />
                      </div>
                    </div>
                    <div class="field-group">
                      <div class="field">
                        <label>Country</label>
                        <input name="country" placeholder="e.g. United States" />
                      </div>
                      <div class="field">
                        <label>Active</label>
                        <select name="active">
                          <option value="">(leave unchanged)</option>
                          <option value="true">Active (Y)</option>
                          <option value="false">Inactive (N)</option>
                        </select>
                      </div>
                    </div>
                  `;
                } else if (entity === "route") {
                  html += `
                    <div class="field-group">
                      <div class="field" style="max-width:150px;">
                        <label>Airline code (IATA/ICAO)</label>
                        <input name="airline" maxlength="3" placeholder="e.g. UA" />
                      </div>
                      <div class="field" style="max-width:150px%;">
                        <label>Source airport (IATA)</label>
                        <input name="src" maxlength="3" placeholder="e.g. SFO" />
                      </div>
                      <div class="field" style="max-width:150px;">
                        <label>Destination airport (IATA)</label>
                        <input name="dst" maxlength="3" placeholder="e.g. JFK" />
                      </div>
                    </div>
                    <div class="field-group">
                      <div class="field">
                        <label>Stops</label>
                        <input name="stops" type="number" placeholder="0" />
                      </div>
                      <div class="field">
                        <label>Equipment</label>
                        <input name="equipment" placeholder="e.g. 738 739" />
                      </div>
                      <div class="field">
                        <label>Codeshare</label>
                        <div class="checkbox-row">
                          <input id="codeshareChk" name="codeshare" type="checkbox" />
                          <label for="codeshareChk">Codeshare (Y)</label>
                        </div>
                      </div>
                    </div>
                    <div class="hint">
                      For <strong>modify</strong> and <strong>remove</strong>, the combination
                      (airline, src, dst) identifies the route. Modify currently lets you
                      update stops, equipment, and codeshare.
                    </div>
                  `;
                }

                fieldsRoot.innerHTML = html;
                setupEntitySpecificBehaviour();
              }

              function clearResults() {
                resultsRoot.innerHTML = `
                  <div class="card muted">
                    <div class="card-header">
                      <span class="card-title">No updates yet</span>
                    </div>
                    <div class="card-body">
                      Submit an update to see the <strong>Before</strong> and <strong>After</strong> cards here.
                    </div>
                  </div>
                `;
              }

              function renderEntitySummary(entity, obj) {
                if (!obj) return "<p class=\"muted\">(no data)</p>";

                function row(label, value) {
                  if (value === undefined || value === null || value === "") return "";
                  return `<dt>${label}</dt><dd>${String(value)}</dd>`;
                }

                if (entity === "airport") {
                  return `
                    <dl>
                      ${row("ID", obj.id)}
                      ${row("IATA", obj.iata)}
                      ${row("ICAO", obj.icao)}
                      ${row("Name", obj.name)}
                      ${row("City", obj.city)}
                      ${row("Country", obj.country)}
                      ${row("Latitude", obj.latitude)}
                      ${row("Longitude", obj.longitude)}
                      ${row("Altitude", obj.altitude)}
                      ${row("Timezone", obj.timezone)}
                      ${row("DST", obj.dst)}
                      ${row("TZDB", obj.tzdbTimezone)}
                      ${row("Type", obj.type)}
                      ${row("Source", obj.source)}
                    </dl>
                  `;
                } else if (entity === "airline") {
                  return `
                    <dl>
                      ${row("ID", obj.id)}
                      ${row("IATA", obj.iata)}
                      ${row("ICAO", obj.icao)}
                      ${row("Name", obj.name)}
                      ${row("Alias", obj.alias)}
                      ${row("Callsign", obj.callsign)}
                      ${row("Country", obj.country)}
                      ${row("Active", obj.active)}
                    </dl>
                  `;
                } else if (entity === "route") {
                  return `
                    <dl>
                      ${row("Airline", obj.airline)}
                      ${row("Airline ID", obj.airlineId)}
                      ${row("Source", obj.srcAirport)}
                      ${row("Destination", obj.dstAirport)}
                      ${row("Src ID", obj.srcAirportId)}
                      ${row("Dst ID", obj.dstAirportId)}
                      ${row("Stops", obj.stops)}
                      ${row("Codeshare", obj.codeshare)}
                      ${row("Equipment", obj.equipment)}
                    </dl>
                  `;
                }
                return `<pre>${JSON.stringify(obj, null, 2)}</pre>`;
              }

              function renderResultCards(data) {
                const entity = data.entityType || entitySelect.value;
                const op     = data.operation   || opSelect.value;

                const before = data.before || null;
                const after  = data.after  || null;

                let html = "";

                if (before) {
                  html += `
                    <div class="card">
                      <div class="card-header">
                        <span class="card-title">Before Update</span>
                        <span>
                          <span class="badge badge-entity">${entity}</span>
                          <span class="badge badge-op">${op}</span>
                        </span>
                      </div>
                      <div class="card-body">
                        ${renderEntitySummary(entity, before)}
                      </div>
                    </div>
                  `;
                } else if (op === "insert") {
                  html += `
                    <div class="card muted">
                      <div class="card-header">
                        <span class="card-title">Before Update</span>
                      </div>
                      <div class="card-body">
                        No previous entity (insert).
                      </div>
                    </div>
                  `;
                }

                if (op === "remove") {
                  html += `
                    <div class="card card-success">
                      <div class="card-header">
                        <span class="card-title">After Update</span>
                      </div>
                      <div class="card-body">
                        Entity successfully removed.
                      </div>
                    </div>
                  `;
                } else if (after) {
                  html += `
                    <div class="card card-success">
                      <div class="card-header">
                        <span class="card-title">After Update</span>
                      </div>
                      <div class="card-body">
                        ${renderEntitySummary(entity, after)}
                      </div>
                    </div>
                  `;
                }

                resultsRoot.innerHTML = html;
              }

              function setError(msg) {
                errorEl.textContent = msg || "";
              }

              // ---------- Autocomplete helpers ----------

              async function fetchSuggestions(kind, query) {
                if (!query || query.length < 2) return [];
                const base = kind === "airport" ? "/airports/search" : "/airlines/search";
                try {
                  const resp = await fetch(base + "?q=" + encodeURIComponent(query) + "&limit=8");
                  if (!resp.ok) return [];
                  return await resp.json();
                } catch (e) {
                  console.error("autocomplete fetch error", e);
                  return [];
                }
              }

              function renderSuggestions(kind, items, inputEl, listEl) {
                listEl.innerHTML = "";
                activeList = listEl;
                activeIndex = -1;

                if (!items.length) {
                  listEl.innerHTML = '<div class="autocomplete-empty">No matches</div>';
                  listEl.style.display = "block";
                  return;
                }

                items.forEach((obj) => {
                  const div = document.createElement("div");
                  div.className = "autocomplete-item";
                  const display = obj.display || obj.name || obj.code || "";
                  div.textContent = display;
                  div.dataset.code = obj.code || "";

                  div.addEventListener("click", () => {
                    inputEl.value = display;
                    listEl.style.display = "none";
                    listEl.innerHTML = "";
                    const code = div.dataset.code;
                    if (code) {
                      if (kind === "airport") {
                        loadAirportDetails(code);
                      } else if (kind === "airline") {
                        loadAirlineDetails(code);
                      }
                    }
                  });

                  listEl.appendChild(div);
                });

                listEl.style.display = "block";
              }

              async function handleAutocompleteInput(kind, inputEl, listEl) {
                const query = (inputEl.value || "").trim();
                if (query.length < 2) {
                  listEl.style.display = "none";
                  listEl.innerHTML = "";
                  return;
                }
                const items = await fetchSuggestions(kind, query);
                if ((inputEl.value || "").trim() === query) {
                  renderSuggestions(kind, items, inputEl, listEl);
                }
              }

              function handleAutocompleteKeydown(e, inputEl, listEl) {
                if (listEl.style.display === "none") return;

                const items = listEl.querySelectorAll(".autocomplete-item");
                if (!items.length) return;

                if (e.key === "ArrowDown" || e.key === "ArrowUp") {
                  e.preventDefault();
                  if (activeList !== listEl) {
                    activeList = listEl;
                    activeIndex = -1;
                  }
                  const dir = e.key === "ArrowDown" ? 1 : -1;
                  activeIndex = (activeIndex + dir + items.length) % items.length;
                  items.forEach(el => el.classList.remove("active"));
                  items[activeIndex].classList.add("active");
                  items[activeIndex].scrollIntoView({ block: "nearest" });
                } else if (e.key === "Enter") {
                  if (activeIndex >= 0) {
                    e.preventDefault();
                    items[activeIndex].click();
                  }
                } else if (e.key === "Escape") {
                  listEl.style.display = "none";
                }
              }

              async function loadAirportDetails(code) {
                try {
                  const resp = await fetch("/airport?code=" + encodeURIComponent(code));
                  if (!resp.ok) {
                    setError("Airport " + code + " not found");
                    return;
                  }
                  const data = await resp.json();
                  populateAirportForm(data);
                  setError("");
                } catch (e) {
                  console.error(e);
                  setError("Failed to load airport details");
                }
              }

              async function loadAirlineDetails(code) {
                try {
                  const resp = await fetch("/airline?code=" + encodeURIComponent(code));
                  if (!resp.ok) {
                    setError("Airline " + code + " not found");
                    return;
                  }
                  const data = await resp.json();
                  populateAirlineForm(data);
                  setError("");
                } catch (e) {
                  console.error(e);
                  setError("Failed to load airline details");
                }
              }

              function populateAirportForm(ap) {
                const f = form.elements;
                if (!f) return;

                if (f.id)            f.id.value            = (ap.id != null ? ap.id : "");
                if (f.iata)          f.iata.value          = ap.iata || "";
                if (f.name)          f.name.value          = ap.name || "";
                if (f.city)          f.city.value          = ap.city || "";
                if (f.country)       f.country.value       = ap.country || "";
                if (f.icao)          f.icao.value          = ap.icao || "";
                if (f.latitude)      f.latitude.value      = (ap.latitude != null ? ap.latitude : "");
                if (f.longitude)     f.longitude.value     = (ap.longitude != null ? ap.longitude : "");
                if (f.altitude)      f.altitude.value      = (ap.altitude != null ? ap.altitude : "");
                if (f.timezone)      f.timezone.value      = (ap.timezone != null ? ap.timezone : "");
                if (f.dst)           f.dst.value           = ap.dst || "";
                if (f.tzdbTimezone)  f.tzdbTimezone.value  = ap.tzdbTimezone || "";
                if (f.type)          f.type.value          = ap.type || "";
                if (f.source)        f.source.value        = ap.source || "";

                if (opSelect.value === "insert" && f.id) {
                  f.id.value = "";
                }
              }

              function populateAirlineForm(al) {
                const f = form.elements;
                if (!f) return;

                if (f.id)       f.id.value       = (al.id != null ? al.id : "");
                if (f.iata)     f.iata.value     = al.iata || "";
                if (f.name)     f.name.value     = al.name || "";
                if (f.alias)    f.alias.value    = al.alias || "";
                if (f.icao)     f.icao.value     = al.icao || "";
                if (f.callsign) f.callsign.value = al.callsign || "";
                if (f.country)  f.country.value  = al.country || "";
                if (f.active)   f.active.value   = (al.active ? "true" : "false");

                if (opSelect.value === "insert" && f.id) {
                  f.id.value = "";
                }
              }

              function setupEntitySpecificBehaviour() {
                const entity = entitySelect.value;

                if (entity === "airport") {
                  const input = document.getElementById("searchAirportInput");
                  const list  = document.getElementById("searchAirportList");
                  if (input && list) {
                    input.addEventListener("input", () => {
                      handleAutocompleteInput("airport", input, list);
                    });
                    input.addEventListener("keydown", (e) => {
                      handleAutocompleteKeydown(e, input, list);
                    });
                  }
                } else if (entity === "airline") {
                  const input = document.getElementById("searchAirlineInput");
                  const list  = document.getElementById("searchAirlineList");
                  if (input && list) {
                    input.addEventListener("input", () => {
                      handleAutocompleteInput("airline", input, list);
                    });
                    input.addEventListener("keydown", (e) => {
                      handleAutocompleteKeydown(e, input, list);
                    });
                  }
                }
              }

              // Hide autocomplete lists when clicking outside
              document.addEventListener("click", (e) => {
                if (!e.target.closest(".autocomplete-container")) {
                  document.querySelectorAll(".autocomplete-list").forEach((el) => {
                    el.style.display = "none";
                  });
                }
              });

              // ---------- Submit handler ----------
              form.addEventListener("submit", function (e) {
                e.preventDefault();
                setError("");

                const entity = entitySelect.value;
                const op     = opSelect.value;

                const params = new URLSearchParams();
                params.set("entity", entity);
                params.set("operation", op);

                const inputs = fieldsRoot.querySelectorAll("input, select, textarea");
                inputs.forEach(function (el) {
                  const name = el.name;
                  if (!name) return;

                  if (el.type === "checkbox") {
                    if (el.checked) {
                      params.set(name, "true");
                    }
                    return;
                  }

                  const value = (el.value || "").trim();
                  if (value !== "") {
                    params.set(name, value);
                  }
                });

                resultsRoot.innerHTML = `
                  <div class="card muted">
                    <div class="card-header">
                      <span class="card-title">Applying update…</span>
                    </div>
                    <div class="card-body">
                      Please wait while the server validates and applies your change.
                    </div>
                  </div>
                `;

                fetch("/update?" + params.toString())
                  .then(function (resp) {
                    if (!resp.ok) {
                      return resp.text().then(function (text) {
                        throw new Error("Server returned " + resp.status + ": " + text);
                      });
                    }
                    return resp.json();
                  })
                  .then(function (data) {
                    if (!data || typeof data !== "object") {
                      throw new Error("Malformed response from /update");
                    }
                    if (!data.success) {
                      setError(data.error || "Update failed.");
                      resultsRoot.innerHTML = `
                        <div class="card card-error">
                          <div class="card-header">
                            <span class="card-title">Update failed</span>
                          </div>
                          <div class="card-body">
                            ${data.error ? data.error : "An unknown error occurred."}
                          </div>
                        </div>
                      `;
                    } else {
                      setError("");
                      renderResultCards(data);
                    }
                  })
                  .catch(function (err) {
                    console.error(err);
                    setError("Request failed: " + err.message);
                    resultsRoot.innerHTML = `
                      <div class="card card-error">
                        <div class="card-header">
                          <span class="card-title">Request error</span>
                        </div>
                        <div class="card-body">
                          ${err.message}
                        </div>
                      </div>
                    `;
                  });
              });

              // Wire up dropdown changes
              entitySelect.addEventListener("change", renderFields);
              opSelect.addEventListener("change", renderFields);

              // Initial fields + result state
              renderFields();
              clearResults();
            })();
          </script>
        </body>
        </html>
        )HTML";
            response = buildHttpResponse(body, "text/html; charset=UTF-8");
        }
        else if (pathOnly == "/airport-ui" || pathOnly == "/airport-ui/") {
                    std::string body = R"HTML(
                    <!DOCTYPE html>
                    <html lang="en">
                    <head>
                      <meta charset="UTF-8" />
                      <title>Airport Airlines Explorer</title>
                      <meta name="viewport" content="width=device-width, initial-scale=1" />
                      <link href="https://api.mapbox.com/mapbox-gl-js/v3.4.0/mapbox-gl.css" rel="stylesheet" />
                      <script src="https://api.mapbox.com/mapbox-gl-js/v3.4.0/mapbox-gl.js"></script>
                      <style>
                        body {
                          font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
                          margin: 0;
                          padding: 1.5rem;
                          background: #f5f5f5;
                          color: #222;
                        }
                        .container {
                          max-width: 880px;
                          margin: 0 auto;
                          background: #ffffff;
                          border-radius: 12px;
                          padding: 1.5rem 1.75rem 1.75rem;
                          box-shadow: 0 2px 8px rgba(0, 0, 0, 0.08);
                        }
                        .header-bar {
                          display: flex;
                          justify-content: space-between;
                          align-items: center;
                          gap: 1rem;
                          margin-bottom: 0.75rem;
                        }
                        .header-left h1 {
                          margin: 0;
                          font-size: 1.7rem;
                        }
                        .subtitle {
                          font-size: 0.85rem;
                          color: #6b7280;
                          margin-top: 0.15rem;
                        }
                        .header-right a {
                          font-size: 0.8rem;
                          text-decoration: none;
                          padding: 0.35rem 0.65rem;
                          border-radius: 999px;
                          border: 1px solid #e5e7eb;
                          color: #374151;
                          background: #f9fafb;
                        }
                        .header-right a:hover {
                          background: #e5e7eb;
                        }
                        form {
                          display: grid;
                          gap: 0.75rem;
                          margin-bottom: 1rem;
                        }
                        .field-group {
                          display: flex;
                          flex-wrap: wrap;
                          gap: 0.75rem;
                        }
                        .field {
                          display: flex;
                          flex-direction: column;
                          min-width: 140px;
                          flex: 1 1 0;
                        }
                        label {
                          font-size: 0.8rem;
                          font-weight: 600;
                          margin-bottom: 0.25rem;
                          text-transform: uppercase;
                          letter-spacing: 0.06em;
                        }

                        input[type="text"],
                        input[type="number"] {
                          padding: 0.4rem 0.5rem;
                          border-radius: 6px;
                          border: 1px solid #d1d5db;
                          font-size: 0.95rem;
                        }
                        .btn {
                          padding: 0.45rem 0.9rem;
                          border-radius: 999px;
                          border: none;
                          font-size: 0.95rem;
                          font-weight: 600;
                          cursor: pointer;
                          display: inline-flex;
                          align-items: center;
                          gap: 0.25rem;
                        }
                        .btn-primary {
                          background: #2563eb;
                          color: #ffffff;
                        }
                        .btn-primary:hover {
                          background: #1d4ed8;
                        }
                        #error {
                          margin-top: 0.25rem;
                          color: #b91c1c;
                          font-size: 0.9rem;
                        }
                        .hint {
                          font-size: 0.85rem;
                          color: #6b7280;
                          margin-top: 0.25rem;
                        }
                        h3 {
                          margin-top: 1.25rem;
                          margin-bottom: 0.5rem;
                          font-size: 1.05rem;
                        }
                        .results-list {
                          margin-top: 0.5rem;
                          display: flex;
                          flex-direction: column;
                          gap: 0.75rem;
                        }
                        .card {
                          background: #ffffff;
                          border-radius: 12px;
                          padding: 0.75rem 1rem;
                          box-shadow: 0 1px 4px rgba(15, 23, 42, 0.08);
                          font-size: 0.93rem;
                        }
                        .main-line {
                          display: flex;
                          align-items: center;
                          gap: 0.4rem;
                          font-weight: 600;
                          margin-bottom: 0.25rem;
                        }
                        .name-text {
                          font-size: 0.96rem;
                        }
                        .pill {
                          font-size: 0.72rem;
                          font-weight: 600;
                          padding: 0.1rem 0.55rem;
                          border-radius: 999px;
                          margin-left: 0.2rem;
                          white-space: nowrap;
                          background: #e8f0fe;
                          color: #174ea6;
                        }
                        .sub-line {
                          font-size: 0.83rem;
                          color: #4b5563;
                          display: flex;
                          flex-wrap: wrap;
                          gap: 0.4rem;
                        }
                        .no-results {
                          font-size: 0.9rem;
                          color: #6b7280;
                        }

                        .view-mode-tabs {
                          display: inline-flex;
                          gap: 0.25rem;
                          margin-top: 2.0rem;
                          margin-bottom: 0.45rem;
                          font-size: 0.8rem;
                        }
                        .view-tab {
                          border: none;
                          cursor: pointer;
                          background: #e5e7eb;
                          color: #111827;
                          padding: 0.15rem 0.7rem;
                          border-radius: 999px;
                          font-weight: 500;
                        }
                        .view-tab.active {
                          background: #111827;
                          color: #f9fafb;
                        }

                        /* Map container */
                        #airportRouteMap {
                          height: 400px;
                          margin-top: 1.25rem;
                          margin-bottom: 0.75rem;
                          border-radius: 8px;
                          overflow: hidden;
                        }

                        /* Airline pill (same style as airline-ui) */
                        .airline-pill {
                          display: inline-flex;
                          align-items: center;
                          background: #f1f5f9;
                          color: #1e293b;
                          padding: 0.15rem 0.6rem 0.15rem 0.25rem;
                          border-radius: 999px;
                          font-size: 0.78rem;
                          font-weight: 500;
                          margin-right: 0.4rem;
                          white-space: nowrap;
                          line-height: 1;
                        }
                        .airline-pill img {
                          height: 1.25rem;
                          width: 1.25rem;
                          border-radius: 50%;
                          object-fit: contain;
                          margin-right: 0.35rem;
                          background: #ffffff;
                          border: 1px solid #e2e8f0;
                          flex-shrink: 0;
                        }
                        .airline-pill-fallback {
                          width: 22px;
                          height: 22px;
                          border-radius: 999px;
                          display: inline-flex;
                          align-items: center;
                          justify-content: center;
                          font-size: 0.8rem;
                          font-weight: 600;
                          background: #4f46e5;
                          color: #f9fafb;
                        }
                        .airline-pill-name {
                          white-space: nowrap;
                        }

                        /* Per-airline rows under the airport summary */
                        .route-card {
                          background: #ffffff;
                          border-radius: 12px;
                          padding: 0.55rem 0.85rem;
                          box-shadow: 0 1px 3px rgba(15, 23, 42, 0.06);
                          border: 1px solid #e5e7eb;
                          display: flex;
                          flex-direction: column;
                          gap: 0.15rem;
                          font-size: 0.9rem;
                        }
                        .route-card.selectable {
                          cursor: pointer;
                          transition: box-shadow 0.18s ease, transform 0.18s ease, border-color 0.18s ease;
                        }
                        .route-card.selectable:hover {
                          box-shadow: 0 4px 10px rgba(15, 23, 42, 0.12);
                          transform: translateY(-1px);
                          border-color: #93c5fd;
                        }
                        .route-card.selectable.selected {
                          border-color: #2563eb;
                          box-shadow: 0 0 0 1px rgba(37, 99, 235, 0.65);
                        }

                        .route-main-line {
                          display: flex;
                          align-items: center;
                          justify-content: space-between;
                          gap: 0.75rem;
                        }
                        .route-tag {
                          font-size: 0.78rem;
                          font-weight: 600;
                          padding: 0.1rem 0.55rem;
                          border-radius: 999px;
                          background: #111827;
                          color: #f9fafb;
                          white-space: nowrap;
                        }
                        .route-sub-line {
                          font-size: 0.82rem;
                          color: #4b5563;
                          margin-left: 2.1rem;
                        }

                        .airline-summary-right {
                          margin-left: auto;
                          display: flex;
                          flex-direction: column;
                          align-items: flex-end;
                          gap: 0.2rem;
                        }

                        .status-pill {
                          display: inline-flex;
                          align-items: center;
                          padding: 0.15rem 0.7rem;
                          border-radius: 999px;
                          font-size: 0.78rem;
                          font-weight: 500;
                          white-space: nowrap;
                          margin-top: 0.05rem;
                        }
                        .status-pill.active {
                          background: #111827;
                          color: #f9fafb;
                        }
                        .status-pill.inactive {
                          background: #e5e7eb;
                          color: #4b5563;
                        }

                        /* Expanded airline details under each card */
                        .airline-details {
                          margin-top: 0.5rem;
                          padding-top: 0.5rem;
                          border-top: 1px solid #e5e7eb;
                          display: none;
                        }
                        .airport-flow-section {
                          margin-top: 0.75rem;
                        }
                        .airport-flow-title {
                          font-size: 0.78rem;
                          font-weight: 600;
                          text-transform: uppercase;
                          letter-spacing: 0.06em;
                          color: #6b7280;
                          margin-bottom: 0.35rem;
                        }
                        .flows-grid {
                          display: grid;
                          grid-template-columns: repeat(auto-fill, minmax(160px, 1fr));
                          column-gap: 1.75rem;
                          row-gap: 0.2rem;
                          font-size: 0.86rem;
                        }
                        .flow-item {
                          white-space: nowrap;
                          overflow: hidden;
                          text-overflow: ellipsis;
                        }
                        .airline-details-empty {
                          font-style: italic;
                          color: #9ca3af;
                          font-size: 0.82rem;
                        }
                      </style>
                    </head>
                    <body>
                      <div class="container">
                        <div class="header-bar">
                          <div class="header-left">
                            <h1>Airport Airlines Explorer</h1>
                            <p class="subtitle">
                              Enter an airport code (e.g. <code>SJC</code>, <code>JFK</code>, <code>ATL</code>)
                              to see all airlines that serve it.
                            </p>
                          </div>
                          <div class="header-right">
                            <a href="/">← Back to Route Finder</a>
                          </div>
                        </div>

                        <form id="airport-form">
                          <div class="field-group">
                            <div class="field" style="max-width: 260px;">
                              <label for="airport">Airport code (IATA)</label>
                              <input id="airport" name="airport" type="text" maxlength="8"
                                     list="airportsList"
                                     placeholder="e.g. SJC, JFK, ATL" />
                              <datalist id="airportsList">
                                <option value="SJC">Norman Y. Mineta San Jose International (SJC)</option>
                                <option value="SFO">San Francisco International (SFO)</option>
                                <option value="LAX">Los Angeles International (LAX)</option>
                                <option value="JFK">New York–JFK (JFK)</option>
                                <option value="LGA">LaGuardia (LGA)</option>
                                <option value="ORD">Chicago O'Hare (ORD)</option>
                                <option value="MDW">Chicago Midway (MDW)</option>
                                <option value="ATL">Hartsfield–Jackson Atlanta (ATL)</option>
                                <option value="DFW">Dallas–Fort Worth (DFW)</option>
                                <option value="DEN">Denver International (DEN)</option>
                                <option value="SEA">Seattle–Tacoma (SEA)</option>
                                <option value="PHX">Phoenix Sky Harbor (PHX)</option>
                                <option value="LAS">Las Vegas McCarran (LAS)</option>
                                <option value="BOS">Boston Logan (BOS)</option>
                                <option value="MCO">Orlando International (MCO)</option>
                              </datalist>
                            </div>
                            <div class="field" style="max-width: 120px;">
                              <label for="limit">Max airlines</label>
                              <input id="limit" name="limit" type="number" min="1" value="50" />
                            </div>
                          </div>
                          <button type="submit" class="btn btn-primary">
                            Search Airport Airlines
                          </button>
                          <div id="error"></div>
                          <div class="hint">
                            Uses the <code>/airport/routes?code=XX</code> API from the server.
                          </div>
                        </form>

                        <h3>Results</h3>

                        <div id="airportRouteMap"></div>

                        <div class="view-mode-tabs" id="viewModeTabs">
                          <button class="view-tab active" data-mode="cards">Cards</button>
                          <button class="view-tab" data-mode="json">JSON</button>
                        </div>

                        <div id="resultsList" class="results-list">
                          <p class="no-results">(no results yet)</p>
                        </div>
                      </div>

                      <script>
                        (function () {
                          const form = document.getElementById("airport-form");
                          const airportInput = document.getElementById("airport");
                          const limitInput = document.getElementById("limit");
                          const errorEl = document.getElementById("error");
                          const resultsList = document.getElementById("resultsList");

                          const viewModeTabs = document.getElementById("viewModeTabs");
                          const viewTabButtons = viewModeTabs.querySelectorAll(".view-tab");

                          let viewMode = "cards";
                          let lastData = null;
                          let lastTotalAirlines = 0;

                          // Mapbox setup (same token as /airline-ui)
                          mapboxgl.accessToken = "pk.eyJ1IjoiY2hyaXN0aW5lcnlhbjkzIiwiYSI6ImNtaWJnZDhiMDAxN2sya29sNXZvNHExMXkifQ.6K3wdjmIhFRQD3bre4f_DA";

                          const INITIAL_CENTER = [-98.5, 39.8];
                          const INITIAL_ZOOM   = 2;

                          const airportMap = new mapboxgl.Map({
                            container: "airportRouteMap",
                            style: "mapbox://styles/mapbox/light-v11",
                            center: INITIAL_CENTER,
                            zoom: INITIAL_ZOOM
                          });

                          let airportMapIsLoaded = false;
                          let airportLastGeoJsonAllAirlines = null;
                          let airportLastGeoJsonByAirline = {};
                          let selectedAirportAirlineCode = null;

                          let airportFlowsByAirline = {};  // airlineCode -> { arrivals:[], departures:[] }
                          let currentAirportMeta = null;
                          let currentAirportCode = null;

                          airportMap.on("load", function () {
                            airportMapIsLoaded = true;
                          });

                          function clearAirportMapRoutes() {
                            if (!airportMapIsLoaded) return;
                            if (airportMap.getLayer("airport-routes-line")) {
                              airportMap.removeLayer("airport-routes-line");
                            }
                            if (airportMap.getSource("airport-routes")) {
                              airportMap.removeSource("airport-routes");
                            }
                          }

                          function renderAirportMapRoutes(geoJsonData) {
                            if (!airportMapIsLoaded) return;
                            clearAirportMapRoutes();
                            if (!geoJsonData || !geoJsonData.features || !geoJsonData.features.length) {
                              airportMap.flyTo({ center: INITIAL_CENTER, zoom: INITIAL_ZOOM });
                              return;
                            }

                            airportMap.addSource("airport-routes", {
                              type: "geojson",
                              data: geoJsonData
                            });

                            airportMap.addLayer({
                              id: "airport-routes-line",
                              type: "line",
                              source: "airport-routes",
                              layout: {
                                "line-join": "round",
                                "line-cap": "round"
                              },
                              paint: {
                                "line-color": "#2563eb",
                                "line-width": 1.5,
                                "line-opacity": 0.7
                              }
                            });

                            const bounds = new mapboxgl.LngLatBounds();
                            (geoJsonData.features || []).forEach(function (f) {
                              const geom = f.geometry || {};
                              if (geom.type === "LineString" && Array.isArray(geom.coordinates)) {
                                geom.coordinates.forEach(function (coord) {
                                  if (Array.isArray(coord) && coord.length >= 2) {
                                    bounds.extend(coord);
                                  }
                                });
                              }
                            });

                            if (!bounds.isEmpty()) {
                              airportMap.fitBounds(bounds, {
                                padding: 40,
                                maxZoom: 5
                              });
                            } else {
                              airportMap.flyTo({ center: INITIAL_CENTER, zoom: INITIAL_ZOOM });
                            }
                          }

                          function buildAirportFlowsFromGeojson(homeCode, geo) {
                            airportFlowsByAirline = {};
                            airportLastGeoJsonByAirline = {};
                            airportLastGeoJsonAllAirlines = geo;

                            if (!geo || !Array.isArray(geo.features)) {
                              clearAirportMapRoutes();
                              return;
                            }

                            const home = (homeCode || "").toUpperCase();
                            const arrivalsAgg = new Map();
                            const departuresAgg = new Map();

                            geo.features.forEach(function (f) {
                              const props = f.properties || {};
                              const src = (props.src_iata || "").toUpperCase();
                              const dst = (props.dst_iata || "").toUpperCase();
                              const airline = (props.airline_code || "").toUpperCase();
                              if (!airline) return;

                              const coords = (f.geometry && f.geometry.coordinates) || [];
                              if (!Array.isArray(coords) || coords.length < 2) return;
                              const srcCoord = coords[0];
                              const dstCoord = coords[1];

                              var direction = null;
                              var otherCode = null;
                              var otherCoord = null;

                              if (src === home && dst !== home) {
                                direction = "departures";
                                otherCode = dst;
                                otherCoord = dstCoord;
                              } else if (dst === home && src !== home) {
                                direction = "arrivals";
                                otherCode = src;
                                otherCoord = srcCoord;
                              } else {
                                return;
                              }

                              if (!airportFlowsByAirline[airline]) {
                                airportFlowsByAirline[airline] = { arrivals: [], departures: [] };
                              }
                              const flows = airportFlowsByAirline[airline];
                              const destObj = {
                                code: otherCode,
                                latitude: Array.isArray(otherCoord) ? otherCoord[1] : undefined,
                                longitude: Array.isArray(otherCoord) ? otherCoord[0] : undefined
                              };
                              flows[direction].push(destObj);

                              const aggMap = direction === "arrivals" ? arrivalsAgg : departuresAgg;
                              let agg = aggMap.get(otherCode);
                              if (!agg) {
                                agg = {
                                  code: otherCode,
                                  latitude: destObj.latitude,
                                  longitude: destObj.longitude,
                                  airlineCodes: new Set()
                                };
                                aggMap.set(otherCode, agg);
                              }
                              agg.airlineCodes.add(airline);
                            });
                            // We don't actually need the aggregated arrays for UI right now,
                            // but they are easy to derive from arrivalsAgg / departuresAgg if desired.
                          }

                          function getGeoJsonForAirportAirline(airlineCode) {
                            const code = (airlineCode || "").toUpperCase();
                            if (!code || !airportLastGeoJsonAllAirlines) return null;
                            if (airportLastGeoJsonByAirline[code]) {
                              return airportLastGeoJsonByAirline[code];
                            }
                            const features = (airportLastGeoJsonAllAirlines.features || []).filter(function (f) {
                              const props = f.properties || {};
                              return (props.airline_code || "").toUpperCase() === code;
                            });
                            const geo = { type: "FeatureCollection", features: features };
                            airportLastGeoJsonByAirline[code] = geo;
                            return geo;
                          }

                          function loadAirportGeoJson(homeCode) {
                            const code = (homeCode || "").toUpperCase();
                            if (!code) return;
                            airportLastGeoJsonAllAirlines = null;
                            airportLastGeoJsonByAirline = {};
                            selectedAirportAirlineCode = null;
                            clearAirportMapRoutes();

                            fetch("/api/airport/routes/geojson?airport=" + encodeURIComponent(code))
                              .then(function (resp) {
                                if (!resp.ok) {
                                  return resp.text().then(function (t) {
                                    throw new Error("GeoJSON error " + resp.status + ": " + t);
                                  });
                                }
                                return resp.json();
                              })
                              .then(function (geo) {
                                buildAirportFlowsFromGeojson(code, geo);
                                renderAirportMapRoutes(geo);
                              })
                              .catch(function (err) {
                                console.error("Failed to load airport GeoJSON", err);
                                clearAirportMapRoutes();
                              });
                          }

                          function populateAirlineDetailsForAirport(homeAirportCode, airlineCode, detailsEl) {
                            if (!detailsEl) return;
                            detailsEl.innerHTML = "";

                            const code = (airlineCode || "").toUpperCase();
                            const flows = airportFlowsByAirline[code];

                            if (!flows ||
                                ((!flows.arrivals || !flows.arrivals.length) &&
                                 (!flows.departures || !flows.departures.length))) {
                              const empty = document.createElement("div");
                              empty.className = "airline-details-empty";
                              empty.textContent = "No per-destination data available for this airline at this airport.";
                              detailsEl.appendChild(empty);
                              return;
                            }

                            function renderSection(title, items) {
                              if (!items || !items.length) return;
                              const sec = document.createElement("div");
                              sec.className = "airport-flow-section";

                              const titleEl = document.createElement("div");
                              titleEl.className = "airport-flow-title";
                              titleEl.textContent = title;
                              sec.appendChild(titleEl);

                              const grid = document.createElement("div");
                              grid.className = "flows-grid";

                              items.forEach(function (item) {
                                if (!item) return;
                                const label = item.code || "Unknown";
                                const cell = document.createElement("div");
                                cell.className = "flow-item";
                                cell.textContent = label;
                                grid.appendChild(cell);
                              });

                              sec.appendChild(grid);
                              detailsEl.appendChild(sec);
                            }

                            renderSection("Arrivals (to " + homeAirportCode + ")", flows.arrivals);
                            renderSection("Departures (from " + homeAirportCode + ")", flows.departures);
                          }

                          function setError(msg) {
                            errorEl.textContent = msg || "";
                          }

                          function clearResults() {
                            resultsList.innerHTML = "";
                          }

                          function updateViewTabs() {
                            viewTabButtons.forEach(function (btn) {
                              const mode = btn.getAttribute("data-mode");
                              if (mode === viewMode) btn.classList.add("active");
                              else btn.classList.remove("active");
                            });
                          }

                          viewTabButtons.forEach(function (btn) {
                            btn.addEventListener("click", function () {
                              const mode = btn.getAttribute("data-mode");
                              if (mode === viewMode) return;
                              viewMode = mode;
                              updateViewTabs();
                              renderForCurrentMode();
                            });
                          });

                          function getAirlineLogoUrl(code) {
                            if (!code || code === "\\N") {
                              return "https://placehold.co/40x40/f1f5f9/94a3b8?text=?";
                            }
                            return "https://images.kiwi.com/airlines/64/" + code.toUpperCase() + ".png";
                          }

                          function airlineDisplayName(al) {
                            if (!al) return "Unknown airline";
                            const code = al.iata || al.icao || al.code || "";
                            const base = al.name || code || "Unknown airline";
                            return code ? base + " (" + code + ")" : base;
                          }

                          function renderAirlinePillFromData(entry) {
                            const al   = entry.airline || {};
                            const code = al.iata || al.icao || al.code || "";
                            const name = airlineDisplayName(al);
                            const logoUrl = code ? getAirlineLogoUrl(code) : "";

                            if (logoUrl) {
                              return (
                                '<span class="airline-pill">' +
                                  '<img class="airline-pill-logo" src="' + logoUrl + '" alt="' + code + ' logo" />' +
                                  '<span class="airline-pill-name">' + name + "</span>" +
                                "</span>"
                              );
                            }

                            const initial = (name || code || "?").charAt(0).toUpperCase();
                            return (
                              '<span class="airline-pill">' +
                                '<span class="airline-pill-fallback">' + initial + "</span>" +
                                '<span class="airline-pill-name">' + name + "</span>" +
                              "</span>"
                            );
                          }

                          function wireAirlineCardInteractions() {
                            const homeCode = (currentAirportMeta && (currentAirportMeta.iata || currentAirportMeta.icao)) || "";
                            const cards = document.querySelectorAll(".route-card.selectable");
                            cards.forEach(function (card) {
                              card.addEventListener("click", function () {
                                const airlineCode = card.dataset.airlineCode || "";
                                const already = card.classList.contains("selected");

                                document.querySelectorAll(".route-card.selectable.selected").forEach(function (el) {
                                  el.classList.remove("selected");
                                  const d = el.querySelector(".airline-details");
                                  if (d) {
                                    d.style.display = "none";
                                    d.innerHTML = "";
                                  }
                                });

                                if (already) {
                                  selectedAirportAirlineCode = null;
                                  if (airportLastGeoJsonAllAirlines) {
                                    renderAirportMapRoutes(airportLastGeoJsonAllAirlines);
                                  }
                                  return;
                                }

                                selectedAirportAirlineCode = airlineCode;
                                card.classList.add("selected");
                                const detailsEl = card.querySelector(".airline-details");
                                if (detailsEl) {
                                  populateAirlineDetailsForAirport(homeCode, airlineCode, detailsEl);
                                  detailsEl.style.display = "block";
                                }

                                const filteredGeo = getGeoJsonForAirportAirline(airlineCode);
                                if (filteredGeo && filteredGeo.features && filteredGeo.features.length) {
                                  renderAirportMapRoutes(filteredGeo);
                                } else if (airportLastGeoJsonAllAirlines) {
                                  renderAirportMapRoutes(airportLastGeoJsonAllAirlines);
                                }
                              });
                            });
                          }

                          function renderCards(data) {
                            clearResults();

                            if (!data || !data.airport) {
                              resultsList.innerHTML =
                                '<p class="no-results">No results to display yet.</p>';
                              return;
                            }

                            const airport  = data.airport;
                            const airlines = data.airlines || [];

                            const homeCode = airport.iata || airport.icao || "";
                            const locBits = [];
                            if (airport.city)    locBits.push(airport.city);
                            if (airport.country) locBits.push(airport.country);

                            const summary = document.createElement("div");
                            summary.className = "card";

                            summary.innerHTML =
                              '<div class="main-line">' +
                                '<span class="name-text">' + (airport.name || "Unknown airport") + '</span>' +
                                (homeCode ? '<span class="pill">' + homeCode + "</span>" : "") +
                              "</div>" +
                              '<div class="sub-line">' +
                                (locBits.length ? "<span>" + locBits.join(", ") + "</span>" : "") +
                                '<span>Total airlines in dataset: ' + lastTotalAirlines + "</span>" +
                              "</div>";

                            resultsList.appendChild(summary);

                            // If no airlines, show message but KEEP the summary card
                            if (!airlines.length) {
                              const p = document.createElement("p");
                              p.className = "no-results";
                              p.textContent = "No airlines in the dataset for this airport.";
                              resultsList.appendChild(p);
                              return;   // nothing to wire up
                            }

                            // Otherwise, render airline cards as before
                            airlines.forEach(function (entry) {
                              const al          = entry.airline || {};
                              const count       = entry.routes || 0;
                              const country     = al.country || "";
                              const airlineCode = (al.iata || al.icao || al.code || "").toUpperCase();
                              const activeFlag  = (typeof al.active === "boolean") ? al.active : null;

                              const card = document.createElement("div");
                              card.className = "route-card selectable";
                              if (airlineCode) {
                                card.dataset.airlineCode = airlineCode;
                              }

                              const statusHtml = (activeFlag === true || activeFlag === false)
                                ? '<span class="status-pill ' + (activeFlag ? 'active' : 'inactive') +
                                  '">Active = ' + (activeFlag ? 'True' : 'False') + '</span>'
                                : "";

                              card.innerHTML =
                                '<div class="route-main-line">' +
                                  renderAirlinePillFromData(entry) +
                                  '<div class="airline-summary-right">' +
                                    '<span class="route-tag">' + count + ' route' + (count === 1 ? "" : "s") + "</span>" +
                                    statusHtml +
                                  '</div>' +
                                "</div>" +
                                '<div class="route-sub-line">' +
                                  (country ? "<span>Country: " + country + "</span>" : "") +
                                "</div>" +
                                '<div class="airline-details"></div>';

                              resultsList.appendChild(card);
                            });

                            // ✅ still wire up click/hover behavior
                            wireAirlineCardInteractions();
                          }

                          function renderJson() {
                            clearResults();

                            if (!lastData) {
                              resultsList.innerHTML =
                                '<p class="no-results">(run a search to see JSON)</p>';
                              return;
                            }

                            const jsonStr = JSON
                              .stringify(lastData, null, 2)
                              .replace(/</g, "&lt;");

                            resultsList.innerHTML =
                              '<pre style="background:#020617;color:#e5e7eb;' +
                              'padding:0.75rem;border-radius:0.5rem;font-size:0.8rem;' +
                              'overflow:auto;max-height:24rem;">' +
                              jsonStr +
                              "</pre>";
                          }

                          function renderForCurrentMode() {
                            if (viewMode === "json") renderJson();
                            else renderCards(lastData);
                          }

                          form.addEventListener("submit", function (e) {
                            e.preventDefault();
                            setError("");

                            var code = (airportInput.value || "").trim().toUpperCase();
                            if (!code) {
                              setError("Please enter an airport code (e.g. SJC, JFK, ATL).");
                              return;
                            }

                            var parts = code.split(/\s+/);
                            if (parts.length > 1) {
                              code = parts[0];
                            }

                            var limit = parseInt((limitInput.value || "").trim(), 10);
                            if (isNaN(limit) || limit <= 0) {
                              limit = 50;
                              limitInput.value = "50";
                            }

                            clearResults();
                            resultsList.innerHTML =
                              '<p class="no-results">Loading airport airlines…</p>';

                            fetch("/airport/routes?code=" + encodeURIComponent(code))
                              .then(function (resp) {
                                if (!resp.ok) {
                                  return resp.text().then(function (text) {
                                    throw new Error("Server returned " + resp.status + ": " + text);
                                  });
                                }
                                return resp.json();
                              })
                              .then(function (data) {
                                if (data && typeof data === "object" && data.error) {
                                  setError("Server error: " + data.error);
                                  clearResults();
                                  clearAirportMapRoutes();
                                  return;
                                }

                                var airlinesArr = Array.isArray(data.airlines) ? data.airlines : [];
                                lastTotalAirlines = airlinesArr.length;

                                var limitedAirlines = airlinesArr;
                                if (airlinesArr.length > limit) {
                                  limitedAirlines = airlinesArr.slice(0, limit);
                                }

                                lastData = Object.assign({}, data, { airlines: limitedAirlines });
                                currentAirportMeta = data.airport || null;
                                currentAirportCode = code;

                                // Load GeoJSON for this airport (all airlines) and update map + flows.
                                loadAirportGeoJson(code);

                                viewMode = "cards";
                                updateViewTabs();
                                renderCards(lastData);
                              })
                              .catch(function (err) {
                                console.error(err);
                                setError("Request failed. Please try again.");
                                clearResults();
                                clearAirportMapRoutes();
                              });
                          });
                        })();
                      </script>
                    </body>
                    </html>
                    )HTML";
                    response = buildHttpResponse(body, "text/html; charset=UTF-8");
                }

        else if (pathOnly == "/airport") {
            // your existing /airport?code= handler:
            auto params = parseQueryString(queryString);
            auto it = params.find("code");
            if (it == params.end() || it->second.empty()) {
                std::string body = R"({"error":"Missing 'code' query parameter"})";
                response = buildHttpResponse(body,
                                             "application/json; charset=UTF-8",
                                             "HTTP/1.1 400 Bad Request\r\n");
            } else {
                std::string code = it->second;
                for (char &c : code) c = static_cast<char>(std::toupper((unsigned char)c));
                
                auto ait = g_airportsByIata.find(code);
                if (ait == g_airportsByIata.end()) {
                    std::ostringstream body;
                    body << R"({"error":"Airport with code ')" << code << R"(' not found"})";
                    response = buildHttpResponse(body.str(),
                                                 "application/json; charset=UTF-8",
                                                 "HTTP/1.1 404 Not Found\r\n");
                } else {
                    std::string body = airportToJson(*(ait->second));
                    response = buildHttpResponse(body, "application/json; charset=UTF-8");
                }
            }
        }
        else if (pathOnly == "/code") {
            // Adjust this filename if your source file is named differently.
            constexpr const char* SOURCE_FILE = "main.cpp";
            std::ifstream file(SOURCE_FILE);
            if (!file) {
                std::string body = "Could not open main.cpp on the server.";
                response = buildHttpResponse(
                                             body,
                                             "text/plain; charset=UTF-8",
                                             "HTTP/1.1 500 Internal Server Error\r\n"
                                             );
            } else {
                std::ostringstream buf;
                buf << file.rdbuf();
                std::string body = buf.str();
                response = buildHttpResponse(body, "text/plain; charset=UTF-8");
            }
        }
        else if (pathOnly == "/direct") {
            auto params = parseQueryString(queryString);
            
            auto itSrc = params.find("src");
            auto itDst = params.find("dst");
            
            if (itSrc == params.end() || itSrc->second.empty() ||
                itDst == params.end() || itDst->second.empty()) {
                
                std::string body = R"({"error":"Missing 'src' and/or 'dst' query parameters"})";
                response = buildHttpResponse(body,
                                             "application/json; charset=UTF-8",
                                             "HTTP/1.1 400 Bad Request\r\n");
            } else {
                std::string src = itSrc->second;
                std::string dst = itDst->second;
                
                // Normalize to uppercase
                for (char &c : src) c = static_cast<char>(std::toupper((unsigned char)c));
                for (char &c : dst) c = static_cast<char>(std::toupper((unsigned char)c));
                
                // Validate airports exist
                auto srcIt = g_airportsByIata.find(src);
                auto dstIt = g_airportsByIata.find(dst);
                
                if (srcIt == g_airportsByIata.end()) {
                    std::ostringstream body;
                    body << R"({"error":"Source airport with code ')" << src << R"(' not found"})";
                    response = buildHttpResponse(body.str(),
                                                 "application/json; charset=UTF-8",
                                                 "HTTP/1.1 404 Not Found\r\n");
                } else if (dstIt == g_airportsByIata.end()) {
                    std::ostringstream body;
                    body << R"({"error":"Destination airport with code ')" << dst << R"(' not found"})";
                    response = buildHttpResponse(body.str(),
                                                 "application/json; charset=UTF-8",
                                                 "HTTP/1.1 404 Not Found\r\n");
                } else {
                    const Airport *srcAp = srcIt->second;
                    const Airport *dstAp = dstIt->second;
                    
                    // Find direct routes: src -> dst with stops == 0
                    std::vector<const Route*> matches;
                    for (const auto &r : g_routes) {
                        if (r.stops == 0 &&
                            r.srcAirport == src &&
                            r.dstAirport == dst) {
                            matches.push_back(&r);
                        }
                    }
                    
                    std::string body = directFlightsToJson(*srcAp, *dstAp, matches);
                    response = buildHttpResponse(body,
                                                 "application/json; charset=UTF-8");
                }
            }
        }
        else if (pathOnly == "/onehop") {
            auto params = parseQueryString(queryString);
            
            auto itSrc = params.find("src");
            auto itDst = params.find("dst");
            
            if (itSrc == params.end() || itSrc->second.empty() ||
                itDst == params.end() || itDst->second.empty()) {
                
                std::string body = R"({"error":"Missing 'src' and/or 'dst' query parameters"})";
                response = buildHttpResponse(body,
                                             "application/json; charset=UTF-8",
                                             "HTTP/1.1 400 Bad Request\r\n");
            } else {
                std::string src = itSrc->second;
                std::string dst = itDst->second;
                
                // Normalize to uppercase
                for (char &c : src) c = static_cast<char>(std::toupper((unsigned char)c));
                for (char &c : dst) c = static_cast<char>(std::toupper((unsigned char)c));
                
                // Validate airports exist
                auto srcIt = g_airportsByIata.find(src);
                auto dstIt = g_airportsByIata.find(dst);
                
                if (srcIt == g_airportsByIata.end()) {
                    std::ostringstream body;
                    body << R"({"error":"Source airport with code ')" << src << R"(' not found"})";
                    response = buildHttpResponse(body.str(),
                                                 "application/json; charset=UTF-8",
                                                 "HTTP/1.1 404 Not Found\r\n");
                } else if (dstIt == g_airportsByIata.end()) {
                    std::ostringstream body;
                    body << R"({"error":"Destination airport with code ')" << dst << R"(' not found"})";
                    response = buildHttpResponse(body.str(),
                                                 "application/json; charset=UTF-8",
                                                 "HTTP/1.1 404 Not Found\r\n");
                } else {
                    const Airport *srcAp = srcIt->second;
                    const Airport *dstAp = dstIt->second;
                    
                    // Determine recommended flag (default: true).
                    bool recommended = true;
                    auto itRec = params.find("recommended");
                    if (itRec != params.end() && !itRec->second.empty()) {
                        std::string recVal = itRec->second;
                        for (char &ch : recVal) {
                            ch = static_cast<char>(std::tolower((unsigned char)ch));
                        }
                        if (recVal == "false") {
                            recommended = false;
                        }
                        // Any other value is treated as "true" by default.
                    }
                    
                    // Limit: default 20 for recommended view.
                    int limit  = getIntQueryParam(params, "limit", 20);
                    if (limit < 0) limit = 20;
                    
                    // Offset is kept for metadata compatibility, but not used
                    // to slice results in the new recommended behavior.
                    int offset = getIntQueryParam(params, "offset", 0);
                    if (offset < 0) offset = 0;
                    
                    // Build full list of one-hop routes (S -> X -> D with 0 stops).
                    std::vector<OneHopRoute> hops;
                    
                    // Find all S -> X legs with 0 stops
                    for (const auto &r1 : g_routes) {
                        if (r1.stops != 0) continue;
                        if (r1.srcAirport != src) continue;
                        
                        const std::string &viaCode = r1.dstAirport;
                        auto viaIt = g_airportsByIata.find(viaCode);
                        if (viaIt == g_airportsByIata.end()) continue; // skip if we don't know X
                        
                        const Airport *viaAp = viaIt->second;
                        
                        // For that via airport X, find all X -> D legs with 0 stops
                        for (const auto &r2 : g_routes) {
                            if (r2.stops != 0) continue;
                            if (r2.srcAirport != viaCode) continue;
                            if (r2.dstAirport != dst) continue;
                            
                            // Compute total distance S -> X -> D
                            double d1 = haversineMiles(srcAp->latitude, srcAp->longitude,
                                                       viaAp->latitude, viaAp->longitude);
                            double d2 = haversineMiles(viaAp->latitude, viaAp->longitude,
                                                       dstAp->latitude, dstAp->longitude);
                            double total = d1 + d2;
                            
                            OneHopRoute hop;
                            hop.firstLeg   = &r1;
                            hop.secondLeg  = &r2;
                            hop.viaAirport = viaAp;
                            hop.totalMiles = total;
                            
                            // --- NEW: compute sameAirline and hasCodeshare here ---
                            bool sameAirline = false;
                            // Match by numeric ID if both known
                            if (r1.airlineId != -1 && r2.airlineId != -1 &&
                                r1.airlineId == r2.airlineId) {
                                sameAirline = true;
                            } else if (!r1.airline.empty() && !isNullField(r1.airline) &&
                                       !r2.airline.empty() && !isNullField(r2.airline) &&
                                       r1.airline == r2.airline) {
                                // Or match by airline code
                                sameAirline = true;
                            }
                            
                            hop.sameAirline  = sameAirline;
                            hop.hasCodeshare = (r1.codeshare || r2.codeshare);
                            
                            hops.push_back(hop);
                        }
                    }
                    
                    std::size_t total = hops.size();
                    
                    if (recommended) {
                        // --- NEW: enhanced ranking for recommended view ---
                        std::sort(hops.begin(), hops.end(),
                                  [](const OneHopRoute &a, const OneHopRoute &b) {
                            if (a.totalMiles != b.totalMiles)
                                return a.totalMiles < b.totalMiles;
                            if (a.sameAirline != b.sameAirline)
                                return a.sameAirline && !b.sameAirline;
                            if (a.hasCodeshare != b.hasCodeshare)
                                return !a.hasCodeshare && b.hasCodeshare;
                            return false;
                        });
                        
                        // Apply limit: keep only top N recommended routes.
                        if (limit == 0) {
                            hops.clear();
                        } else if (limit > 0 &&
                                   static_cast<std::size_t>(limit) < hops.size()) {
                            hops.resize(static_cast<std::size_t>(limit));
                        }
                    } else {
                        // Original behavior: sort ONLY by totalMiles ascending,
                        // and return ALL one-hop routes (no limiting).
                        std::sort(hops.begin(), hops.end(),
                                  [](const OneHopRoute &a, const OneHopRoute &b) {
                            return a.totalMiles < b.totalMiles;
                        });
                        // For JSON metadata, it's reasonable to reflect
                        // how many routes we actually returned.
                        limit = static_cast<int>(hops.size());
                        offset = 0;
                    }
                    
                    std::string body =
                    oneHopRoutesToJson(*srcAp, *dstAp, hops, total, limit, offset);
                    response = buildHttpResponse(body,
                                                 "application/json; charset=UTF-8");
                }
            }
        }
        else if (pathOnly == "/airport/airlines") {
            // NEW: given an airport IATA (or ICAO) code, list airlines ordered by
            // number of routes to/from that airport.
            auto params = parseQueryString(queryString);
            auto it = params.find("code");
            if (it == params.end() || it->second.empty()) {
                std::string body = R"({"error":"Missing 'code' query parameter"})";
                response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                             "HTTP/1.1 400 Bad Request\r\n");
            } else {
                std::string code = it->second;
                for (char &c : code) c = static_cast<char>(std::toupper((unsigned char)c));
                
                // Look up airport by IATA
                auto ait = g_airportsByIata.find(code);
                if (ait == g_airportsByIata.end()) {
                    std::ostringstream body;
                    body << R"({"error":"Airport with code ')" << code << R"(' not found"})";
                    response = buildHttpResponse(body.str(),
                                                 "application/json; charset=UTF-8",
                                                 "HTTP/1.1 404 Not Found\r\n");
                } else {
                    const Airport *ap = ait->second;
                    std::string body = airportAirlinesReportToJson(*ap);
                    response = buildHttpResponse(body, "application/json; charset=UTF-8");
                }
            }
        }
        else if (pathOnly == "/airlines") {
            auto params = parseQueryString(queryString);
            
            // Default pagination values:
            // If missing or invalid, we use limit=100, offset=0.
            int limit  = getIntQueryParam(params, "limit", 100);
            int offset = getIntQueryParam(params, "offset", 0);
            
            std::string body = airlinesOrderedByIataToJson(limit, offset);
            response = buildHttpResponse(body, "application/json; charset=UTF-8");
        }
        else if (pathOnly == "/airports") {
            auto params = parseQueryString(queryString);
            
            int limit  = getIntQueryParam(params, "limit", 100);
            int offset = getIntQueryParam(params, "offset", 0);
            
            std::string body = airportsOrderedByIataToJson(limit, offset);
            response = buildHttpResponse(body, "application/json; charset=UTF-8");
        }
        else if (pathOnly == "/favicon.ico") {
            std::string body;
            response = buildHttpResponse(body,
                                         "image/x-icon",
                                         "HTTP/1.1 204 No Content\r\n");
        }
        else if (pathOnly == "/airline/routes") {
            auto params = parseQueryString(queryString);
            auto it = params.find("code");
            if (it == params.end() || it->second.empty()) {
                std::string body = R"({"error":"Missing 'code' query parameter"})";
                response = buildHttpResponse(body,
                                             "application/json; charset=UTF-8",
                                             "HTTP/1.1 400 Bad Request\r\n");
            } else {
                std::string code = it->second;
                // Normalize to uppercase
                for (char &c : code) c = static_cast<char>(std::toupper((unsigned char)c));

                // ✅ NEW: use findAirlineByCode so this can be IATA *or* ICAO
                const Airline *al = findAirlineByCode(code);

                if (!al) {
                    std::ostringstream body;
                    body << R"({"error":"Airline with code ')" << code << R"(' not found"})";
                    response = buildHttpResponse(body.str(),
                                                 "application/json; charset=UTF-8",
                                                 "HTTP/1.1 404 Not Found\r\n");
                } else {
                    std::string body = airlineRoutesReportToJson(*al);
                    response = buildHttpResponse(body,
                                                 "application/json; charset=UTF-8");
                }
            }
        }
        else if (pathOnly == "/airlines/search") {
                    auto params = parseQueryString(queryString);
                    auto itQ = params.find("q");
                    std::string q = (itQ == params.end() ? "" : itQ->second);

                    int limit = getIntQueryParam(params, "limit", 10);
                    if (limit <= 0) limit = 10;

                    std::string body = airlinesSearchToJson(q, limit);
                    response = buildHttpResponse(body,
                                                 "application/json; charset=UTF-8");
                }
        else if (pathOnly == "/airports/search") {
            auto params = parseQueryString(queryString);
            auto itQ = params.find("q");
            std::string q = (itQ == params.end() ? "" : itQ->second);
            
            int limit = getIntQueryParam(params, "limit", 10);
            if (limit <= 0) limit = 10;
            
            std::string body = airportsSearchToJson(q, limit);
            response = buildHttpResponse(body, "application/json; charset=UTF-8");
        }
        // ... (existing handlers before this)
        
        else if (pathOnly == "/api/airport/routes/geojson") {
                    auto params = parseQueryString(queryString);

                    auto itAirport = params.find("airport");
                    if (itAirport == params.end() || itAirport->second.empty()) {
                        std::string body = R"({"error":"Missing 'airport' query parameter"})";
                        response = buildHttpResponse(body,
                                                     "application/json; charset=UTF-8",
                                                     "HTTP/1.1 400 Bad Request\r\n");
                    } else {
                        std::string airportCode = itAirport->second;
                        for (char &c : airportCode) {
                            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                        }

                        std::string airlineCode;
                        auto itAirline = params.find("airline");
                        if (itAirline != params.end() && !itAirline->second.empty()) {
                            airlineCode = itAirline->second;
                            for (char &c : airlineCode) {
                                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                            }
                        }

                        std::vector<const Route*> routes;

                        if (airlineCode.empty()) {
                            // All airlines, just filter by airport (either src or dst).
                            auto itSrc = g_routesBySrcAirport.find(airportCode);
                            if (itSrc != g_routesBySrcAirport.end()) {
                                routes.insert(routes.end(), itSrc->second.begin(), itSrc->second.end());
                            }
                            auto itDst = g_routesByDstAirport.find(airportCode);
                            if (itDst != g_routesByDstAirport.end()) {
                                routes.insert(routes.end(), itDst->second.begin(), itDst->second.end());
                            }
                        } else {
                            // Filter by airline AND airport.
                            const Airline *al = findAirlineByCode(airlineCode);
                            if (!al) {
                                std::ostringstream body;
                                body << R"({"error":"Airline with code ')" << airlineCode
                                     << R"(' not found"})";
                                response = buildHttpResponse(body.str(),
                                                             "application/json; charset=UTF-8",
                                                             "HTTP/1.1 404 Not Found\r\n");
                                // early return from handler chain
                                send(client_fd, response.c_str(), response.size(), 0);
                                close(client_fd);
                                continue;
                            }

                            std::vector<const Route*> allAirlineRoutes;
                            if (al->id != -1) {
                                auto itR = g_routesByAirlineId.find(al->id);
                                if (itR != g_routesByAirlineId.end()) {
                                    allAirlineRoutes = itR->second;
                                }
                            } else {
                                for (const auto &r : g_routes) {
                                    if (!r.airline.empty() &&
                                        !isNullField(r.airline) &&
                                        r.airline == airlineCode) {
                                        allAirlineRoutes.push_back(&r);
                                    }
                                }
                            }

                            for (const Route *rPtr : allAirlineRoutes) {
                                if (!rPtr) continue;
                                if (rPtr->srcAirport == airportCode ||
                                    rPtr->dstAirport == airportCode) {
                                    routes.push_back(rPtr);
                                }
                            }
                        }

                        std::string body = routesToGeoJson(routes);
                        response = buildHttpResponse(body, "application/json; charset=UTF-8");
                    }
                }
        
        else if (pathOnly == "/api/routes/geojson") {
            auto params = parseQueryString(queryString);
            auto it = params.find("airline");
            
            if (it == params.end() || it->second.empty()) {
                std::string body = R"({"error":"Missing 'airline' query parameter"})";
                response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                             "HTTP/1.1 400 Bad Request\r\n");
            } else {
                std::string code = it->second;
                for (char &c : code) c = static_cast<char>(std::toupper((unsigned char)c));
                
                // 1. Find the Airline
                const Airline *al = findAirlineByCode(code);
                
                if (!al || al->id == -1) {
                    // Fallback: search by IATA/ICAO code if ID is missing (can be slow)
                    std::vector<const Route*> routes;
                    for(const auto &r : g_routes) {
                        if (r.airline == code) {
                            routes.push_back(&r);
                        }
                    }
                    if (routes.empty()) {
                        std::string body = R"({"error":"Airline not found or has no routes"})";
                        response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                                     "HTTP/1.1 404 Not Found\r\n");
                        
                    } else {
                        // Serve routes found via code fallback
                        std::string body = routesToGeoJson(routes);
                        response = buildHttpResponse(body, "application/json; charset=UTF-8");
                    }
                    
                } else {
                    // 2. Use the fast index (O(1) lookup)
                    auto itRoutes = g_routesByAirlineId.find(al->id);
                    std::vector<const Route*> routes = (itRoutes != g_routesByAirlineId.end())
                    ? itRoutes->second
                    : std::vector<const Route*>();
                    
                    // 3. Generate GeoJSON
                    std::string body = routesToGeoJson(routes);
                    response = buildHttpResponse(body, "application/json; charset=UTF-8");
                }
            }
        }
        
        // ... (existing handlers after this)
        else if (pathOnly == "/id") {
            std::string body = R"({"name":"Christine Hatch","student_id":"20174104"})";
            response = buildHttpResponse(body, "application/json; charset=UTF-8");
        }
        else if (pathOnly == "/airport/routes") {
            // Parse query string: code=XXX
            auto params = parseQueryString(queryString);
            std::string code;
            auto it = params.find("code");
            if (it != params.end()) {
                code = it->second;
            }
            
            // Trim spaces and uppercase the code, e.g. "jfk" -> "JFK"
            auto trim = [](std::string &s) {
                while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
                    s.erase(s.begin());
                }
                while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
                    s.pop_back();
                }
            };
            trim(code);
            for (char &ch : code) {
                ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            }
            
            std::cerr << "[/airport/routes] code = '" << code << "'\n";
            
            const Airport *ap = nullptr;
            auto itAp = g_airportsByIata.find(code);
            if (itAp != g_airportsByIata.end()) {
                ap = itAp->second;
            }
            
            std::string body;
            std::string statusLine = "HTTP/1.1 200 OK\r\n";
            
            if (!ap) {
                body = "{\"error\":\"Unknown airport code\"}";
                statusLine = "HTTP/1.1 404 Not Found\r\n";
            } else {
                body = airportAirlinesReportToJson(*ap);
            }
            
            // Just set response; the send() happens once at the bottom with client_fd.
            response = buildHttpResponse(
                                         body,
                                         "application/json; charset=UTF-8",
                                         statusLine
                                         );
        }
        
        else if (pathOnly == "/update") {
            auto params = parseQueryString(queryString);
            
            auto getParam = [&](const std::string &key) -> std::string {
                auto it = params.find(key);
                if (it == params.end()) return "";
                return it->second;
            };
            
            std::string entity    = getParam("entity");
            std::string operation = getParam("operation");
            
            // Normalize to lowercase
            auto toLower = [](std::string s) {
                for (char &c : s) c = static_cast<char>(std::tolower((unsigned char)c));
                return s;
            };
            entity    = toLower(entity);
            operation = toLower(operation);
            
            if (entity.empty() || operation.empty()) {
                std::string body = buildUpdateResponse(
                                                       entity.empty() ? "" : entity,
                                                       operation.empty() ? "" : operation,
                                                       false,
                                                       "",
                                                       "",
                                                       "Missing 'entity' or 'operation' parameter"
                                                       );
                response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                             "HTTP/1.1 400 Bad Request\r\n");
            } else if (entity == "airport") {
                // -------- Airport operations --------
                std::string idStr = getParam("id");
                int id = !idStr.empty() ? parseIntOr(idStr, -1) : -1;
                
                if (operation == "insert") {
                    if (id == -1) {
                        std::string body = buildUpdateResponse(
                                                               "airport","insert",false,"","",
                                                               "Insert airport requires a valid 'id' parameter");
                        response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                                     "HTTP/1.1 400 Bad Request\r\n");
                    } else if (findAirportIndexById(id) != -1) {
                        std::string body = buildUpdateResponse(
                                                               "airport","insert",false,"","",
                                                               "Airport ID already exists");
                        response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                                     "HTTP/1.1 409 Conflict\r\n");
                    } else {
                        Airport a;
                        a.id            = id;
                        a.name          = getParam("name");
                        a.city          = getParam("city");
                        a.country       = getParam("country");
                        a.iata          = upperCopy(getParam("iata"));
                        a.icao          = upperCopy(getParam("icao"));
                        a.latitude      = parseDoubleOr(getParam("latitude"), 0.0);
                        a.longitude     = parseDoubleOr(getParam("longitude"), 0.0);
                        a.altitude      = parseIntOr(getParam("altitude"), 0);
                        a.timezone      = parseDoubleOr(getParam("timezone"), 0.0);
                        a.dst           = getParam("dst");
                        a.tzdbTimezone  = getParam("tzdbTimezone");
                        a.type          = getParam("type");
                        a.source        = getParam("source");
                        
                        // Optional: ensure IATA uniqueness as well.
                        if (!a.iata.empty() && g_airportsByIata.count(a.iata) > 0) {
                            std::string body = buildUpdateResponse(
                                                                   "airport","insert",false,"","",
                                                                   "Another airport already uses this IATA code");
                            response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                                         "HTTP/1.1 409 Conflict\r\n");
                        } else {
                            g_airports.push_back(a);
                            rebuildAirportIndex();
                            
                            std::string afterJson = airportToJson(g_airports.back());
                            std::string body = buildUpdateResponse(
                                                                   "airport","insert",true,"",afterJson,"");
                            response = buildHttpResponse(body, "application/json; charset=UTF-8");
                        }
                    }
                }
                else if (operation == "modify") {
                    if (id == -1) {
                        std::string body = buildUpdateResponse(
                                                               "airport","modify",false,"","",
                                                               "Modify airport requires 'id'");
                        response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                                     "HTTP/1.1 400 Bad Request\r\n");
                    } else {
                        int idx = findAirportIndexById(id);
                        if (idx == -1) {
                            std::string body = buildUpdateResponse(
                                                                   "airport","modify",false,"","",
                                                                   "Airport id not found");
                            response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                                         "HTTP/1.1 404 Not Found\r\n");
                        } else {
                            Airport before = g_airports[idx];
                            Airport &a = g_airports[idx];
                            
                            // IATA uniqueness check if iata provided
                            if (params.count("iata")) {
                                std::string newIata = upperCopy(getParam("iata"));
                                if (!newIata.empty()) {
                                    for (std::size_t i = 0; i < g_airports.size(); ++i) {
                                        if (i == static_cast<std::size_t>(idx)) continue;
                                        if (g_airports[i].iata == newIata) {
                                            std::string body = buildUpdateResponse(
                                                                                   "airport","modify",false,
                                                                                   airportToJson(before),
                                                                                   "",
                                                                                   "Another airport already uses this IATA code");
                                            response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                                                         "HTTP/1.1 409 Conflict\r\n");
                                            goto airport_modify_done;
                                        }
                                    }
                                }
                                a.iata = newIata;
                            }
                            if (params.count("name"))         a.name         = getParam("name");
                            if (params.count("city"))         a.city         = getParam("city");
                            if (params.count("country"))      a.country      = getParam("country");
                            if (params.count("icao"))         a.icao         = upperCopy(getParam("icao"));
                            if (params.count("latitude"))     a.latitude     = parseDoubleOr(getParam("latitude"), a.latitude);
                            if (params.count("longitude"))    a.longitude    = parseDoubleOr(getParam("longitude"), a.longitude);
                            if (params.count("altitude"))     a.altitude     = parseIntOr(getParam("altitude"), a.altitude);
                            if (params.count("timezone"))     a.timezone     = parseDoubleOr(getParam("timezone"), a.timezone);
                            if (params.count("dst"))          a.dst          = getParam("dst");
                            if (params.count("tzdbTimezone")) a.tzdbTimezone = getParam("tzdbTimezone");
                            if (params.count("type"))         a.type         = getParam("type");
                            if (params.count("source"))       a.source       = getParam("source");
                            
                            rebuildAirportIndex();
                            
                            {
                                std::string body = buildUpdateResponse(
                                                                       "airport","modify",true,
                                                                       airportToJson(before),
                                                                       airportToJson(a),
                                                                       "");
                                response = buildHttpResponse(body, "application/json; charset=UTF-8");
                            }
                            
                        airport_modify_done:
                            (void)0;
                        }
                    }
                }
                else if (operation == "remove") {
                    const Airport *targetAp = nullptr;
                    int idx = -1;
                    
                    if (id != -1) {
                        idx = findAirportIndexById(id);
                        if (idx != -1) targetAp = &g_airports[idx];
                    } else if (params.count("iata")) {
                        std::string code = upperCopy(getParam("iata"));
                        targetAp = findAirportByIata(code);
                        if (targetAp) {
                            idx = findAirportIndexById(targetAp->id);
                        }
                    }
                    
                    if (!targetAp || idx == -1) {
                        std::string body = buildUpdateResponse(
                                                               "airport","remove",false,"","",
                                                               "Airport not found for removal");
                        response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                                     "HTTP/1.1 404 Not Found\r\n");
                    } else {
                        Airport before = *targetAp;
                        std::string code = before.iata;
                        int airportId = before.id;
                        
                        // Remove airport
                        g_airports.erase(g_airports.begin() + idx);
                        rebuildAirportIndex();
                        
                        // Remove all routes touching this airport
                        g_routes.erase(
                                       std::remove_if(g_routes.begin(), g_routes.end(),
                                                      [&](const Route &r) {
                                                          if (r.srcAirportId == airportId ||
                                                              r.dstAirportId == airportId) return true;
                                                          if (!code.empty() &&
                                                              (r.srcAirport == code || r.dstAirport == code)) return true;
                                                          return false;
                                                      }),
                                       g_routes.end());
                        rebuildRouteIndices();
                        
                        std::string body = buildUpdateResponse(
                                                               "airport","remove",true,
                                                               airportToJson(before),
                                                               "", // after = null
                                                               "");
                        response = buildHttpResponse(body, "application/json; charset=UTF-8");
                    }
                }
                else {
                    std::string body = buildUpdateResponse(
                                                           "airport", operation, false,"","",
                                                           "Unsupported airport operation");
                    response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                                 "HTTP/1.1 400 Bad Request\r\n");
                }
            }
            else if (entity == "airline") {
                // -------- Airline operations --------
                std::string idStr = getParam("id");
                int id = !idStr.empty() ? parseIntOr(idStr, -1) : -1;
                
                if (operation == "insert") {
                    if (id == -1) {
                        std::string body = buildUpdateResponse(
                                                               "airline","insert",false,"","",
                                                               "Insert airline requires a valid 'id'");
                        response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                                     "HTTP/1.1 400 Bad Request\r\n");
                    } else if (findAirlineIndexById(id) != -1) {
                        std::string body = buildUpdateResponse(
                                                               "airline","insert",false,"","",
                                                               "Airline ID already exists");
                        response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                                     "HTTP/1.1 409 Conflict\r\n");
                    } else {
                        Airline al;
                        al.id        = id;
                        al.name      = getParam("name");
                        al.alias     = getParam("alias");
                        al.iata      = upperCopy(getParam("iata"));
                        al.icao      = upperCopy(getParam("icao"));
                        al.callsign  = getParam("callsign");
                        al.country   = getParam("country");
                        std::string activeStr = toLower(getParam("active"));
                        al.active    = (!activeStr.empty() &&
                                        (activeStr == "true" || activeStr == "y" || activeStr == "yes"));
                        
                        if (!al.iata.empty() && g_airlinesByIata.count(al.iata) > 0) {
                            std::string body = buildUpdateResponse(
                                                                   "airline","insert",false,"","",
                                                                   "Another airline already uses this IATA code");
                            response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                                         "HTTP/1.1 409 Conflict\r\n");
                        } else {
                            g_airlines.push_back(al);
                            rebuildAirlineIndex();
                            
                            std::string body = buildUpdateResponse(
                                                                   "airline","insert",true,"",
                                                                   airlineToJson(g_airlines.back()),
                                                                   "");
                            response = buildHttpResponse(body, "application/json; charset=UTF-8");
                        }
                    }
                }
                else if (operation == "modify") {
                    if (id == -1) {
                        std::string body = buildUpdateResponse(
                                                               "airline","modify",false,"","",
                                                               "Modify airline requires 'id'");
                        response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                                     "HTTP/1.1 400 Bad Request\r\n");
                    } else {
                        int idx = findAirlineIndexById(id);
                        if (idx == -1) {
                            std::string body = buildUpdateResponse(
                                                                   "airline","modify",false,"","",
                                                                   "Airline id not found");
                            response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                                         "HTTP/1.1 404 Not Found\r\n");
                        } else {
                            Airline before = g_airlines[idx];
                            Airline &al = g_airlines[idx];
                            
                            if (params.count("iata")) {
                                std::string newIata = upperCopy(getParam("iata"));
                                if (!newIata.empty()) {
                                    for (std::size_t i = 0; i < g_airlines.size(); ++i) {
                                        if (i == static_cast<std::size_t>(idx)) continue;
                                        if (g_airlines[i].iata == newIata) {
                                            std::string body = buildUpdateResponse(
                                                                                   "airline","modify",false,
                                                                                   airlineToJson(before),
                                                                                   "",
                                                                                   "Another airline already uses this IATA code");
                                            response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                                                         "HTTP/1.1 409 Conflict\r\n");
                                            goto airline_modify_done;
                                        }
                                    }
                                }
                                al.iata = newIata;
                            }
                            
                            if (params.count("name"))     al.name     = getParam("name");
                            if (params.count("alias"))    al.alias    = getParam("alias");
                            if (params.count("icao"))     al.icao     = upperCopy(getParam("icao"));
                            if (params.count("callsign")) al.callsign = getParam("callsign");
                            if (params.count("country"))  al.country  = getParam("country");
                            if (params.count("active")) {
                                std::string activeStr = toLower(getParam("active"));
                                al.active = (!activeStr.empty() &&
                                             (activeStr == "true" || activeStr == "y" || activeStr == "yes"));
                            }
                            
                            rebuildAirlineIndex();
                            
                            {
                                std::string body = buildUpdateResponse(
                                                                       "airline","modify",true,
                                                                       airlineToJson(before),
                                                                       airlineToJson(al),
                                                                       "");
                                response = buildHttpResponse(body, "application/json; charset=UTF-8");
                            }
                            
                        airline_modify_done:
                            (void)0;
                        }
                    }
                }
                else if (operation == "remove") {
                    const Airline *targetAl = nullptr;
                    int idx = -1;
                    
                    if (id != -1) {
                        idx = findAirlineIndexById(id);
                        if (idx != -1) targetAl = &g_airlines[idx];
                    } else if (params.count("iata")) {
                        std::string code = upperCopy(getParam("iata"));
                        targetAl = findAirlineByCode(code);
                        if (targetAl) {
                            idx = findAirlineIndexById(targetAl->id);
                        }
                    }
                    
                    if (!targetAl || idx == -1) {
                        std::string body = buildUpdateResponse(
                                                               "airline","remove",false,"","",
                                                               "Airline not found for removal");
                        response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                                     "HTTP/1.1 404 Not Found\r\n");
                    } else {
                        Airline before = *targetAl;
                        int airlineId = before.id;
                        std::string code = !before.iata.empty() ? before.iata : before.icao;
                        
                        // Remove airline
                        g_airlines.erase(g_airlines.begin() + idx);
                        rebuildAirlineIndex();
                        
                        // Remove all routes for this airline
                        g_routes.erase(
                                       std::remove_if(g_routes.begin(), g_routes.end(),
                                                      [&](const Route &r) {
                                                          if (r.airlineId == airlineId) return true;
                                                          if (!code.empty() && r.airline == code) return true;
                                                          return false;
                                                      }),
                                       g_routes.end());
                        rebuildRouteIndices();
                        
                        std::string body = buildUpdateResponse(
                                                               "airline","remove",true,
                                                               airlineToJson(before),
                                                               "",
                                                               "");
                        response = buildHttpResponse(body, "application/json; charset=UTF-8");
                    }
                }
                else {
                    std::string body = buildUpdateResponse(
                                                           "airline",operation,false,"","",
                                                           "Unsupported airline operation");
                    response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                                 "HTTP/1.1 400 Bad Request\r\n");
                }
            }
            else if (entity == "route") {
                // -------- Route operations --------
                if (operation == "insert") {
                    std::string airlineCode = upperCopy(getParam("airline"));
                    std::string srcCode     = upperCopy(getParam("src"));
                    std::string dstCode     = upperCopy(getParam("dst"));
                    
                    if (airlineCode.empty() || srcCode.empty() || dstCode.empty()) {
                        std::string body = buildUpdateResponse(
                                                               "route","insert",false,"","",
                                                               "Insert route requires 'airline', 'src', and 'dst'");
                        response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                                     "HTTP/1.1 400 Bad Request\r\n");
                    } else {
                        const Airline *al = findAirlineByCode(airlineCode);
                        const Airport *srcAp = findAirportByIata(srcCode);
                        const Airport *dstAp = findAirportByIata(dstCode);
                        
                        if (!al) {
                            std::string body = buildUpdateResponse(
                                                                   "route","insert",false,"","",
                                                                   "Airline not found");
                            response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                                         "HTTP/1.1 404 Not Found\r\n");
                        } else if (!srcAp || !dstAp) {
                            std::string body = buildUpdateResponse(
                                                                   "route","insert",false,"","",
                                                                   "Source and/or destination airport not found");
                            response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                                         "HTTP/1.1 404 Not Found\r\n");
                        } else {
                            Route r;
                            r.airline      = airlineCode;
                            r.airlineId    = al->id;
                            r.srcAirport   = srcCode;
                            r.srcAirportId = srcAp->id;
                            r.dstAirport   = dstCode;
                            r.dstAirportId = dstAp->id;
                            
                            std::string codeshareStr = toLower(getParam("codeshare"));
                            r.codeshare = (!codeshareStr.empty() &&
                                           (codeshareStr == "true" || codeshareStr == "y" || codeshareStr == "yes"));
                            r.stops     = parseIntOr(getParam("stops"), 0);
                            r.equipment = getParam("equipment");
                            
                            g_routes.push_back(r);
                            rebuildRouteIndices();
                            
                            std::string body = buildUpdateResponse(
                                                                   "route","insert",true,"",
                                                                   routeToJson(g_routes.back()),
                                                                   "");
                            response = buildHttpResponse(body, "application/json; charset=UTF-8");
                        }
                    }
                }
                else if (operation == "modify") {
                    std::string airlineCode = upperCopy(getParam("airline"));
                    std::string srcCode     = upperCopy(getParam("src"));
                    std::string dstCode     = upperCopy(getParam("dst"));
                    
                    if (airlineCode.empty() || srcCode.empty() || dstCode.empty()) {
                        std::string body = buildUpdateResponse(
                                                               "route","modify",false,"","",
                                                               "Modify route requires 'airline', 'src', and 'dst' to identify the route");
                        response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                                     "HTTP/1.1 400 Bad Request\r\n");
                    } else {
                        int idx = findRouteIndexByTriple(airlineCode, srcCode, dstCode);
                        if (idx == -1) {
                            std::string body = buildUpdateResponse(
                                                                   "route","modify",false,"","",
                                                                   "Route (airline, src, dst) not found");
                            response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                                         "HTTP/1.1 404 Not Found\r\n");
                        } else {
                            Route before = g_routes[idx];
                            Route &r = g_routes[idx];
                            
                            // We keep airline/src/dst fixed; allow mutating codeshare, stops, equipment.
                            if (params.count("codeshare")) {
                                std::string csStr = toLower(getParam("codeshare"));
                                r.codeshare = (!csStr.empty() &&
                                               (csStr == "true" || csStr == "y" || csStr == "yes"));
                            }
                            if (params.count("stops")) {
                                r.stops = parseIntOr(getParam("stops"), r.stops);
                            }
                            if (params.count("equipment")) {
                                r.equipment = getParam("equipment");
                            }
                            
                            rebuildRouteIndices();
                            
                            std::string body = buildUpdateResponse(
                                                                   "route","modify",true,
                                                                   routeToJson(before),
                                                                   routeToJson(r),
                                                                   "");
                            response = buildHttpResponse(body, "application/json; charset=UTF-8");
                        }
                    }
                }
                else if (operation == "remove") {
                    std::string airlineCode = upperCopy(getParam("airline"));
                    std::string srcCode     = upperCopy(getParam("src"));
                    std::string dstCode     = upperCopy(getParam("dst"));
                    
                    if (airlineCode.empty() || srcCode.empty() || dstCode.empty()) {
                        std::string body = buildUpdateResponse(
                                                               "route","remove",false,"","",
                                                               "Remove route requires 'airline', 'src', and 'dst'");
                        response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                                     "HTTP/1.1 400 Bad Request\r\n");
                    } else {
                        int idx = findRouteIndexByTriple(airlineCode, srcCode, dstCode);
                        if (idx == -1) {
                            std::string body = buildUpdateResponse(
                                                                   "route","remove",false,"","",
                                                                   "Route (airline, src, dst) not found");
                            response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                                         "HTTP/1.1 404 Not Found\r\n");
                        } else {
                            Route before = g_routes[idx];
                            g_routes.erase(g_routes.begin() + idx);
                            rebuildRouteIndices();
                            
                            std::string body = buildUpdateResponse(
                                                                   "route","remove",true,
                                                                   routeToJson(before),
                                                                   "",
                                                                   "");
                            response = buildHttpResponse(body, "application/json; charset=UTF-8");
                        }
                    }
                }
                else {
                    std::string body = buildUpdateResponse(
                                                           "route",operation,false,"","",
                                                           "Unsupported route operation");
                    response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                                 "HTTP/1.1 400 Bad Request\r\n");
                }
            }
            else {
                std::string body = buildUpdateResponse(
                                                       entity,operation,false,"","",
                                                       "Unsupported entity type");
                response = buildHttpResponse(body, "application/json; charset=UTF-8",
                                             "HTTP/1.1 400 Bad Request\r\n");
            }
        }
                else {
                            std::string body =
                                "<html><body><h1>404 Not Found</h1><p>No handler for " + pathOnly +
                                "</p></body></html>";
                            response = buildHttpResponse(body,
                                                         "text/html; charset=UTF-8",
                                                         "HTTP/1.1 404 Not Found\r\n");
                        }

                        // ✅ send response ONCE, after all branches (including /update)
                        send(client_fd, response.c_str(), response.size(), 0);
                        close(client_fd);
                    }  // end "if (client_fd >= 0)" block

    
    
    close(server_fd);
    return 0;
}
