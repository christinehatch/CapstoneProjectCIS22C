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

// ---------- Helpers for parsing ----------

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

// ---------- Global collections ----------

std::vector<Airport> g_airports;
std::unordered_map<std::string, const Airport*> g_airportsByIata;

std::vector<Airline> g_airlines;
std::unordered_map<std::string, const Airline*> g_airlinesByIata;

std::vector<Route> g_routes;

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


std::string airlineRoutesReportToJson(const Airline &al) {
    // 1. Count how many times each airport code appears in routes for this airline
    std::unordered_map<std::string, int> counts;
    
    for (const auto &r : g_routes) {
        bool matches = false;
        
        // Prefer matching by airline ID when available
        if (r.airlineId != -1 && r.airlineId == al.id) {
            matches = true;
        } else {
            // Fall back to matching by code (IATA/ICAO)
            if (!r.airline.empty()) {
                if (!al.iata.empty() && r.airline == al.iata) {
                    matches = true;
                } else if (!al.icao.empty() && r.airline == al.icao) {
                    matches = true;
                }
            }
        }
        
        if (!matches) continue;
        
        // Count both source and destination airports for this airline
        if (!r.srcAirport.empty() && !isNullField(r.srcAirport)) {
            counts[r.srcAirport] += 1;
        }
        if (!r.dstAirport.empty() && !isNullField(r.dstAirport)) {
            counts[r.dstAirport] += 1;
        }
    }
    
    // 2. Move counts into a vector and attach Airport* if we can resolve the code
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
    
    // 3. Sort by route count descending, then by code ascending
    std::sort(list.begin(), list.end(),
              [](const AirportCount &a, const AirportCount &b) {
        if (a.count != b.count) return a.count > b.count;
        return a.code < b.code;
    });
    
    // 4. Build JSON object
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
std::string airportAirlinesReportToJson(const Airport &ap) {
    // 1. Count routes per airline involving this airport.
    std::unordered_map<int, int> countsById;        // airlineId -> count
    std::unordered_map<std::string, int> tempCodeCounts; // code -> count (for routes with airlineId == -1)
    
    auto matchesAirport = [&](const Route &r) -> bool {
        // Routes may use IATA or ICAO codes. Compare against both if available.
        bool srcMatch = (!r.srcAirport.empty() &&
                         (r.srcAirport == ap.iata || (!ap.icao.empty() && r.srcAirport == ap.icao)));
        bool dstMatch = (!r.dstAirport.empty() &&
                         (r.dstAirport == ap.iata || (!ap.icao.empty() && r.dstAirport == ap.icao)));
        return srcMatch || dstMatch;
    };
    
    for (const auto &r : g_routes) {
        if (!matchesAirport(r)) continue;
        
        if (r.airlineId != -1) {
            countsById[r.airlineId] += 1;
        } else if (!r.airline.empty() && !isNullField(r.airline)) {
            // Count by code when we have no numeric ID
            tempCodeCounts[r.airline] += 1;
        }
    }
    
    // 2. Try to fold code-based counts into ID-based counts when possible.
    std::unordered_map<std::string, int> unresolvedCodes; // codes we still couldn't resolve by ID
    
    for (const auto &kv : tempCodeCounts) {
        const std::string &code = kv.first;
        int count = kv.second;
        
        const Airline *al = findAirlineByCode(code);
        if (al && al->id != -1) {
            countsById[al->id] += count;
        } else {
            unresolvedCodes[code] += count;
        }
    }
    
    // 3. Build a list of airlines + counts.
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
    
    // 4. Sort by route count descending, then by airline name or codeOrId.
    std::sort(list.begin(), list.end(),
              [](const AirlineCount &a, const AirlineCount &b) {
        if (a.count != b.count) return a.count > b.count;
        
        // Tie-breaker: airline name if available, otherwise code/id
        std::string nameA = a.airline ? a.airline->name : a.codeOrId;
        std::string nameB = b.airline ? b.airline->name : b.codeOrId;
        return nameA < nameB;
    });
    
    // 5. Build JSON.
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
// ---------- Main server ----------

int main() {
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
                      <a href="/airline-ui" style="margin-left:0.5rem;font-size:0.8rem;
                          text-decoration:none;padding:0.3rem 0.6rem;border-radius:999px;
                          border:1px solid #e5e7eb;background:#f9fafb;color:#374151;">
                        Airline Routes
                      </a>
                    <a href="/airport-ui" style="margin-left:0.5rem;font-size:0.8rem;
                                            text-decoration:none;padding:0.3rem 0.6rem;border-radius:999px;
                                            border:1px solid #e5e7eb;background:#f9fafb;color:#374151;">
                      Airport Airlines
                    </a>
                
              </div>
            </div>

            <!-- This panel appears under the tabs when "Student" is selected -->
            <div id="idPanel" class="id-panel" style="display:none;">
              <div id="idPanelStatus" class="id-panel-status"></div>
              <div id="idPanelBody" class="id-panel-body"></div>
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

            <div class="view-mode-tabs" id="viewModeTabs">
                <button class="view-tab active" data-mode="cards">Cards</button>
                <button class="view-tab" data-mode="json">JSON</button>
            </div>

            <!-- Airline filter pills live here -->
            <div id="airlineFilters" class="airline-filter-bar"></div>

            <div id="resultsList" class="results-list">
                <p class="no-results">(no results yet)</p>
            </div>
        
        <script>
        (function () {
            const studentTab = document.getElementById("studentTab");
            const codeTab    = document.getElementById("codeTab");
            const idPanel    = document.getElementById("idPanel");
            const idPanelStatus = document.getElementById("idPanelStatus");
            const idPanelBody   = document.getElementById("idPanelBody");

            let idLoaded = false;
            let studentOpen = false;   // <--- new: tracks if panel is open

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
        function getAirlineLogoUrl(code) {
                    if (!code || code === "\\N") {
                        // Simple placeholder for unknown codes
                        return "https://placehold.co/40x40/f1f5f9/94a3b8?text=?";
                    }
                    // Kiwi pattern for airline logos by IATA code
                    return "https://images.kiwi.com/airlines/64/" + code.toUpperCase() + ".png";
                }
            // STUDENT tab: toggle open/closed
            studentTab.addEventListener("click", () => {
                if (studentOpen) {
                    // Currently open → close it
                    studentOpen = false;
                    idPanel.style.display = "none";
                    setActiveTopTab(null);  // no tab highlighted
                    return;
                }

                // Opening the student panel
                studentOpen = true;
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

            // CODE tab: always open code, close student panel
            codeTab.addEventListener("click", () => {
                studentOpen = false;
                idPanel.style.display = "none";
                setActiveTopTab("code");
                window.open("/code", "_blank");
            });const form = document.getElementById("route-form");
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
        
            /** Normalize airline code from an airline object */
            function getAirlineCodeFromObj(airlineObj) {
                if (!airlineObj) return "";
                const code = airlineObj.iata || airlineObj.icao || airlineObj.code || "";
                return code ? code.toUpperCase() : "";
            }
        
            function setError(msg) {
                errorEl.textContent = msg || "";
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
        
        function renderAirlineFilterBar(mode, data) {
            airlineFilters.innerHTML = "";
        
            const codes = (mode === "direct")
                ? collectAirlineCodesFromDirect(data)
                : collectAirlineCodesFromOneHop(data);
        
            if (!codes.size) {
                activeAirlineFilter = null;
                return; // nothing to filter on
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
                    return;
                }
        
                flights.forEach(function (flight) {
                    const airlineObj = flight.airline || {};
                    const codeshare = !!flight.codeshare;
                    const equipmentLabel = formatEquipment(flight.equipment || "");
                    const equipment = equipmentLabel ? ("Aircraft: " + equipmentLabel) : "";        
                    // Build airline pill with logo
                    const airlinePill = createAirlinePill(airlineObj);
        
                    const card = document.createElement("div");
                    card.className = "route-card";
        
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
            }
        
            function renderOneHopResults(data) {
                lastMode = "onehop";
                lastOneHopData = data;

                clearResults();
                renderAirlineFilterBar("onehop", data);

                const src = data.source;
                const dst = data.destination;
                const allHops = data.one_hop_routes || [];

                // Apply airline filter: show routes where ANY leg matches that airline
                const hops = allHops.filter(hop => {
                    if (!activeAirlineFilter) return true;
                    const first  = hop.first_leg  || {};
                    const second = hop.second_leg || {};
                    const c1 = getAirlineCodeFromObj(first.airline || {});
                    const c2 = getAirlineCodeFromObj(second.airline || {});
                    return c1 === activeAirlineFilter || c2 === activeAirlineFilter;
                });

                if (!hops.length) {
                    const msg = activeAirlineFilter
                        ? "No one-hop routes for " + activeAirlineFilter +
                          " on this city pair. Try another airline or 'All airlines'."
                        : "No one-hop routes found between " +
                          (src.iata || "???") + " and " + (dst.iata || "???") + ".";
                    resultsList.innerHTML = '<p class="no-results">' + msg + '</p>';
                    return;
                }

                hops.forEach(function (hop) {
                    const via    = hop.via;
                    const first  = hop.first_leg || {};
                    const second = hop.second_leg || {};

                    const sameAirline  = !!hop.same_airline;
                    const hasCodeshare = !!hop.has_codeshare;
                    const distance     = hop.total_distance_miles.toFixed(0);

                    const firstAirlinePill  = createAirlinePill(first.airline  || {});
                    const secondAirlinePill = createAirlinePill(second.airline || {});

                    // compute equipment labels here, where they’re needed
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
                    card.className = "route-card";
                    card.innerHTML =
                        '<div class="route-main-line">' +
                            '<span class="route-city">' + src.city + ' (' + src.iata + ')</span>' +
                            '<span class="route-arrow">→</span>' +
                            '<span class="route-city">' + dst.city + ' (' + dst.iata + ')</span>' +
                            '<span class="route-tag onehop">1 stop via ' +
                                via.city + ' (' + via.iata + ')</span>' +
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
                        if (listEl.style.display === "none") return;

                        const items = listEl.querySelectorAll(".autocomplete-item");
                        if (!items.length) return;

                        if (e.key === "ArrowDown" || e.key === "ArrowUp") {
                            e.preventDefault();
                            if (activeList !== listEl) {
                                activeList = listEl;
                                activeIndex = -1;
                            }
                            const dir = (e.key === "ArrowDown") ? 1 : -1;
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
            // ----- Network calls -----
        
            form.addEventListener("submit", function (event) {
                event.preventDefault();
                setError("");
        
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
        
                clearResults();
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
            .route-sub-line {
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
                <div class="field autocomplete-container" style="max-width: 220px;">
                  <label for="airline">Airline code (IATA)</label>
                  <input id="airline" name="airline" type="text" maxlength="40"
                         placeholder="e.g. AA, DL, UA or 'American'" autocomplete="off" />
                  <div id="airline-suggestions" class="autocomplete-list" style="display:none;"></div>
                </div>
                <div class="field" style="max-width: 120px;">
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
              const form = document.getElementById("airline-form");
              const airlineInput = document.getElementById("airline");
              const limitInput = document.getElementById("limit");
              const errorEl = document.getElementById("error");
              const resultsList = document.getElementById("resultsList");

              const viewModeTabs = document.getElementById("viewModeTabs");
              const viewTabButtons = viewModeTabs.querySelectorAll(".view-tab");

              let viewMode = "cards";      // "cards" | "json"
              let lastData = null;         // last JSON from /airline/routes
              let lastTotalAirports = 0;   // <-- total airports before client-side limiting
            const airlineSuggestions = document.getElementById("airline-suggestions");

            // Cache of all airlines from /airlines
            let allAirlines = null;
            let activeSuggestionIndex = -1;

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
                            // Simple placeholder for unknown codes
                            return "https://placehold.co/40x40/f1f5f9/94a3b8?text=?";
                        }
                        // Kiwi pattern for airline logos by IATA code
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
        
              function setError(msg) {
                errorEl.textContent = msg || "";
              }

              function clearResults() {
                resultsList.innerHTML = "";
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

              function renderCards(data) {
                clearResults();

                if (!data || !data.airline) {
                  resultsList.innerHTML =
                    '<p class="no-results">No results to display yet.</p>';
                  return;
                }

                const airline = data.airline;
                const airports = data.airports || [];
                const totalInDataset =
                  (typeof lastTotalAirports === "number" && lastTotalAirports > 0)
                    ? lastTotalAirports
                    : airports.length;

                if (!airports.length) {
                  resultsList.innerHTML =
                    '<p class="no-results">This airline has no routes in the dataset.</p>';
                  return;
                }

                // Airline summary card at top, with logo pill
                const summary = document.createElement("div");
                summary.className = "route-card";

                const code = airline.iata || airline.icao || "";

                // build top line with a placeholder span we’ll replace with the pill
                summary.innerHTML =
                  '<div class="route-main-line">' +
                    '<span class="__airline-pill-slot"></span>' +
                  '</div>' +
                  '<div class="route-sub-line">' +
                    (airline.country ? '<span>Country: ' + airline.country + '</span>' : '') +
                    '<span>Total airports in dataset: ' + totalInDataset + '</span>' +
                  '</div>';

                // create pill and insert it
                const pill = createAirlinePill({
                  name: airline.name,
                  iata: airline.iata,
                  icao: airline.icao,
                  code: code
                });
                summary.querySelector(".__airline-pill-slot").replaceWith(pill);

                resultsList.appendChild(summary);

                // One card per airport
                airports.forEach(function (entry) {
                  const ap = entry.airport || {};
                  const count = entry.routes || 0;

                  const city = ap.city || "";
                  const country = ap.country || "";
                  const iata = ap.iata || ap.icao || "???";

                  const card = document.createElement("div");
                  card.className = "route-card";
                  card.innerHTML =
                    '<div class="route-main-line">' +
                      '<span class="route-city">' +
                        (city ? city : "Unknown city") +
                        ' (' + iata + ')' +
                      '</span>' +
                      '<span class="route-tag">' + count + ' route' +
                        (count === 1 ? '' : 's') + '</span>' +
                    '</div>' +
                    '<div class="route-sub-line">' +
                      (country ? '<span>' + country + '</span>' : '') +
                    '</div>';

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

                // If the user picked "American Airlines (AA)", extract AA from parentheses
                let m = raw.match(/\(([A-Z0-9]{2,3})\)\s*$/i);
                let code;
                if (m) {
                  code = m[1].toUpperCase();
                } else {
                  // Fallback: treat first token as code
                  const parts = raw.toUpperCase().split(/\s+/);
                  code = parts[0];
                }

                let limit = parseInt((limitInput.value || "").trim(), 10);
                if (isNaN(limit) || limit <= 0) {
                  limit = 50;
                  limitInput.value = "50";
                }

                clearResults();
                resultsList.innerHTML =
                  '<p class="no-results">Loading airline routes…</p>';

                fetch("/airline/routes?code=" + encodeURIComponent(code))
                  .then(resp => {
                    if (!resp.ok) {
                      return resp.text().then(text => {
                        throw new Error("Server returned " + resp.status + ": " + text);
                      });
                    }
                    return resp.json();
                  })
                  .then(data => {
                    if (data && typeof data === "object" && data.error) {
                      setError("Server error: " + data.error);
                      clearResults();
                      return;
                    }

                    // 1) Remember the true total from the server
                      if (Array.isArray(data.airports)) {
                        lastTotalAirports = data.airports.length;
                      } else {
                        lastTotalAirports = 0;
                      }

                      // 2) Optionally apply a client-side limit on airports
                      if (Array.isArray(data.airports) && data.airports.length > limit) {
                        data = Object.assign({}, data, {
                          airports: data.airports.slice(0, limit)
                        });
                      }

                      lastData = data;
                      viewMode = "cards";
                      updateViewTabs();
                      renderCards(data);
                    })
                  .catch(err => {
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
        
        else if (path == "/airport-ui" || path == "/airport-ui/") {
            std::string body = R"HTML(
            <!DOCTYPE html>
            <html lang="en">
            <head>
              <meta charset="UTF-8" />
              <title>Airport Airlines Explorer</title>
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
                .view-tab.active {
                  background: #111827;
                  color: #f9fafb;
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
              margin-left: 2.1rem; /* lines up under the logo pill */
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
                        <!-- value is the IATA; label is the name shown in dropdown -->
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

                  let viewMode = "cards";   // "cards" | "json"
                  let lastData = null;      // last JSON from /airport/routes
                  let lastTotalAirlines = 0;

                  function setError(msg) {
                    errorEl.textContent = msg || "";
                  }

                  function clearResults() {
                    resultsList.innerHTML = "";
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

                  
            // --- Airline logo + pill helpers (same behavior as airline-ui) ---
            function getAirlineLogoUrl(code) {
                        if (!code || code === "\\N") {
                            // Simple placeholder for unknown codes
                            return "https://placehold.co/40x40/f1f5f9/94a3b8?text=?";
                        }
                        // Kiwi pattern for airline logos by IATA code
                        return "https://images.kiwi.com/airlines/64/" + code.toUpperCase() + ".png";
                    }
            

            function airlineDisplayName(al) {
              if (!al) return "Unknown airline";
              const code = al.iata || al.icao || al.code || "";
              const base = al.name || code || "Unknown airline";
              return code ? base + " (" + code + ")" : base;
            }

            // Now uses the exact structure of the .airline-pill CSS
            function createAirlinePillHtml(alObj) {
                const al   = alObj.airline || alObj || {};
                const code = al.iata || al.icao || al.code || "";
                const name = airlineDisplayName(al);
                const logoUrl = getAirlineLogoUrl(code);

                // This HTML structure matches the CSS defined for .airline-pill
                // It embeds an image that will use the Kiwi URL or the placeholder
                return (
                    '<span class="airline-pill">' +
                      '<img src="' + logoUrl + '" alt="' + name + ' logo" onerror="this.style.display=\'none\'" />' +
                      '<span class="airline-pill-name">' + name + '</span>' +
                    '</span>'
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

        
            
                  function renderCards(data) {
                    clearResults();

                    if (!data || !data.airport) {
                      resultsList.innerHTML =
                        '<p class="no-results">No results to display yet.</p>';
                      return;
                    }

                    const airport = data.airport;
                    const airlines = data.airlines || [];

                    if (!airlines.length) {
                      resultsList.innerHTML =
                        '<p class="no-results">No airlines in the dataset for this airport.</p>';
                      return;
                    }

                    // Airport summary card
                    const summary = document.createElement("div");
                    summary.className = "card";
                    const code = airport.iata || airport.icao || "";
                    const locBits = [];
                    if (airport.city) locBits.push(airport.city);
                    if (airport.country) locBits.push(airport.country);

                    summary.innerHTML =
                      '<div class="main-line">' +
                        '<span class="name-text">' + (airport.name || "Unknown airport") + '</span>' +
                        (code ? '<span class="pill">' + code + "</span>" : "") +
                      "</div>" +
                      '<div class="sub-line">' +
                        (locBits.length ? "<span>" + locBits.join(", ") + "</span>" : "") +
                        '<span>Total airlines in dataset: ' + lastTotalAirlines + "</span>" +
                      "</div>";

                    resultsList.appendChild(summary);

                    airlines.forEach(function (entry) {
                      const al = entry.airline || {};
                      const count = entry.routes || 0;
                      const country = al.country || "";

                      const card = document.createElement("div");
                      card.className = "route-card";
                      card.innerHTML =
                        '<div class="route-main-line">' +
                          createAirlinePillHtml(entry) +
                          '<span class="route-tag">' + count + ' route' + (count === 1 ? "" : "s") + "</span>" +
                        "</div>" +
                        '<div class="route-sub-line">' +
                          (country ? "<span>Country: " + country + "</span>" : "") +
                        "</div>";

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

                    let code = (airportInput.value || "").trim().toUpperCase();
                    if (!code) {
                      setError("Please enter an airport code (e.g. SJC, JFK, ATL).");
                      return;
                    }

                    // If user typed "SJC - San Jose", grab the first token
                    const parts = code.split(/\s+/);
                    if (parts.length > 1) {
                      code = parts[0];
                    }

                    let limit = parseInt((limitInput.value || "").trim(), 10);
                    if (isNaN(limit) || limit <= 0) {
                      limit = 50;
                      limitInput.value = "50";
                    }

                    clearResults();
                    resultsList.innerHTML =
                      '<p class="no-results">Loading airport airlines…</p>';

                    fetch("/airport/routes?code=" + encodeURIComponent(code))
                      .then(resp => {
                        if (!resp.ok) {
                          return resp.text().then(text => {
                            throw new Error("Server returned " + resp.status + ": " + text);
                          });
                        }
                        return resp.json();
                      })
                      .then(data => {
                        if (data && typeof data === "object" && data.error) {
                          setError("Server error: " + data.error);
                          clearResults();
                          return;
                        }

                        // Remember the total airline count BEFORE limiting
                        const airlinesArr = Array.isArray(data.airlines) ? data.airlines : [];
                        lastTotalAirlines = airlinesArr.length;

                        // Apply client-side limit for display
                        let limitedAirlines = airlinesArr;
                        if (airlinesArr.length > limit) {
                          limitedAirlines = airlinesArr.slice(0, limit);
                        }

                        lastData = Object.assign({}, data, { airlines: limitedAirlines });

                        viewMode = "cards";
                        updateViewTabs();
                        renderCards(lastData);
                      })
                      .catch(err => {
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
                    const Airline *al = ait->second;
                    std::string body = airlineRoutesReportToJson(*al);
                    response = buildHttpResponse(body,
                                                 "application/json; charset=UTF-8");
                }
            }
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
        else {
            std::string body =
            "<html><body><h1>404 Not Found</h1><p>No handler for " + pathOnly +
            "</p></body></html>";
            response = buildHttpResponse(body,
                                         "text/html; charset=UTF-8",
                                         "HTTP/1.1 404 Not Found\r\n");
        }
        
        send(client_fd, response.c_str(), response.size(), 0);
        close(client_fd);
    }
    
    
    close(server_fd);
    return 0;
}
