OpenFlights Route Explorer

Full-Stack C++ Airline & Airport Search Engine with Interactive UI, Map Visualization, and Automated Testing

Overview

OpenFlights Route Explorer is a complete end-to-end flight search platform built on a custom C++ HTTP server with a modern, interactive, client-side UI.
The system loads, indexes, and exposes the full OpenFlights dataset (~80,000 records) and provides:

Direct route search

One-hop route search

Airline-centered exploration

Airport-centered exploration

Autocomplete across airports and airlines

Interactive, filterable UI

GeoJSON route visualization with Mapbox GL JS

The project is fully Dockerized, includes backend API verification scripts, and features a comprehensive Playwright test suite for frontend quality assurance.

This repository demonstrates production-quality engineering across backend systems, frontend development, testing, deployment, and iterative UI design.

Key Features
1. Route Finder Page (/)

Intelligent autocomplete for origin & destination airports

Direct & one-hop route search

Airline and layover filter chips

Nonstop/recommended filters

Card view and JSON view modes

Clear Filters (preserves selected airports)

Mapbox-powered route visualization

2. Airline Routes Explorer (/airline-ui)

Search by IATA, ICAO, or airline name

Autocomplete suggestions

Card-based airport list

Active/Inactive airline indicators

Route counts and details per destination

Expandable detail sections

3. Airport Airlines Explorer (/airport-ui)

Search by airport code or name

Summary card showing airport metadata

Displays even airports with zero flights

Airline list with route counts

Expandable cards displaying:

Arrivals breakdown

Departures breakdown

Clean grid layout for route flows

4. Map Visualization (Route Finder)

GeoJSON-based rendering

Dynamic filtering of visible routes

Highlighted selections

Supports direct and multi-leg paths

Architecture
Backend (C++ Single-File Server)

Custom lightweight HTTP server (no external frameworks)

Manual HTTP parsing, routing, and response generation

In-memory loading of:

airports.dat

airlines.dat

routes.dat

Multi-level indexing:

Airport → inbound/outbound routes

Airline → all served routes

Code → airport/airline metadata

Keyword search for autocomplete

Frontend (Embedded HTML/CSS/JS)

UI pages stored inside main.cpp using raw C++ string literals

Minimal external dependencies (only Mapbox GL JS)

Responsive layout with reusable components:

.container

.card

.header-bar

.btn, .btn-primary

.filter-chip

.autocomplete-list

Progressive enhancement approach: UI works without JS; interaction improves with JS enabled

JSON APIs

All UI pages consume the same API endpoints exposed by the server:

/direct?src=XXX&dst=YYY

/onehop?src=XXX&dst=YYY&recommended=true

/airline/routes?code=XX

/airport/routes?code=XXX

/json (server health check)

/api/.../geojson endpoints for Mapbox visualization

Automated Testing

The project includes complete backend and frontend automation.

Backend Validation — test_openflights.sh

Ensures:

Server availability

Correct JSON responses

Validity of direct & one-hop routing logic

Airport and airline lookup correctness

Error handling conditions

Frontend Validation — Playwright Test Suite

Located in tests/openflights.spec.ts.

Validates:

Autocomplete behavior on all pages

Rendering and expandability of route cards

Layover and airline filters modifying results

Clear Filters behavior

JSON ↔ Card view toggles

Full end-to-end flow for route searches

Map visibility and basic integrity

All tests pass:

5 passed (8.9s)


This introduces a reliable QA workflow comparable to professional engineering teams.

Evolution of the UI

This repository also documents a substantial iterative redesign:

Phase 1 — Minimal HTML

Basic text-only pages returned from the server.

Phase 2 — Structured Layout

Added:

.container

.card

.header-bar

Initial CSS and spacing

Phase 3 — Autocomplete System

Dynamic suggestion lists for airports and airlines.

Phase 4 — Filter Chips & Interaction Model

Added:

Airline filters

Layover filters

Nonstop/recommended toggle

Clear Filters control

JSON/Card mode switching

Phase 5 — Map Integration

Full visual routing map using Mapbox GL JS.

Phase 6 — Airport Arrivals/Departures Grid

Structured breakdown of inbound/outbound traffic.

Phase 7 — Test Coverage, Docker, Deployment

Stabilized and productionized the project.

This journey demonstrates strong iterative development, UI refinement, and collaborative engineering.

Running the Project
Build
g++ -std=c++17 -O2 main.cpp -o openflights

Run
./openflights


Visit:

https://capstoneprojectcis22c.fly.dev/

Docker Support
Build Image
docker build -t openflights .

Run Container
docker run -p 8080:8080 openflights

Testing
Backend
bash test_openflights.sh

Frontend
npx playwright install
npx playwright test

Repository Structure
.
├── main.cpp                   # C++ backend + embedded HTML UIs
├── airlines.dat               # OpenFlights dataset
├── airports.dat
├── routes.dat
├── Dockerfile                 # Container build
├── playwright.config.ts       # Frontend test configuration
├── tests/
│   └── openflights.spec.ts    # Playwright UI test suite
├── test_openflights.sh        # API validation script
├── main_js_scratch.html       # Experimental UI prototype
└── README.md

Demonstrated Competencies

This project showcases:

Systems programming in C++

HTTP server implementation

Data structure design and indexing

API design

Vanilla JavaScript frontend engineering

UI/UX iteration and redesign

Automated browser testing (Playwright)

Backend verification scripting

Dockerization and portable deployment

Map visualization and geospatial rendering

End-to-end product development

It represents a complete, production-level software system developed from scratch.
