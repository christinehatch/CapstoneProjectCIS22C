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
            std::string body =
            "<html><body>"
            "<h1>OpenFlights Server (CIS 22C)</h1>"
            "<p>This is your C++ HTTP server on port 8080.</p>"
            "<ul>"
            "<li>Try <a href=\"/json\">/json</a> for a JSON test.</li>"
            "<li>Try <a href=\"/airports/sample\">/airports/sample</a> "
            "for sample airport data from airports.dat.</li>"
            "<li>Try <code>/airline?code=UA</code> for an airline lookup.</li>"
            "<li>Try <code>/airport?code=SFO</code> for an airport lookup.</li>"
            "<li>Try <code>/airlines</code> for all airlines ordered by IATA.</li>"
            "<li>Try <code>/airports</code> for all airports ordered by IATA.</li>"
            "<li>Try <code>/airline/routes?code=UA</code> to see airports ordered by number of routes for that airline.</li>"
            "<li>Try <code>/airport/airlines?code=SFO</code> to see airlines ordered by number of routes at that airport.</li>"
            "<li>Try <code>/direct?src=SFO&dst=LAX</code> to see all direct flights between two airports.</li>"
            "<li>Try <code>/onehop?src=SFO&dst=JFK</code> to see one-hop routes ordered by total distance.</li>"
            "</ul>"
            "<p>Loaded airports: " + std::to_string(g_airports.size()) + "</p>"
            "<p>Loaded airlines: " + std::to_string(g_airlines.size()) + "</p>"
            "<p>Loaded routes: "   + std::to_string(g_routes.size())   + "</p>"
            "</body></html>";
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
                    response = buildHttpResponse(body,
                                                 "application/json; charset=UTF-8");
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
