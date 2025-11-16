# Use a small, modern Debian base image
FROM debian:bookworm-slim

# Install build tools (g++, make is optional but handy)
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        g++ \
        make && \
    rm -rf /var/lib/apt/lists/*

# Create and switch to /app directory inside the container
WORKDIR /app

# Copy the C++ source file into the image
COPY main.cpp .

# Copy the data files into the same directory where the server will run.
# This is important so loadAirports("airports.dat") etc. work.
COPY airports.dat airlines.dat routes.dat ./

# Compile the server
RUN g++ -std=c++17 -O2 main.cpp -o server

# The server listens on port 8080, so expose it
EXPOSE 8080

# Default command: run the server when the container starts
CMD ["./server"]
