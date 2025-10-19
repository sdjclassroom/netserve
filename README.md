# Netserve

Private file-sharing server + CLI client (think: a lightweight Google Drive CLI).  
Designed for personal or small-team private use: upload large files in chunks, store per-file ownership, and download via a simple C++ command-line client or curl.
This is mainly designed to work over a cloudflare tunnel and to decentralize the global cloud storage.

## What it is for
- Quickly share files privately between users or machines.
- Scriptable CLI workflows for automated uploads/downloads.
- Keeps simple ownership metadata and enforces Basic HTTP auth for access.

## Components
- server.py — Flask-based HTTP API (user creation, upload init, chunk upload, file list, download).
- netserve.cpp — single-file C++ CLI client using libcurl (create user, upload/download, list files).
- Prebuilt binaries (if present): netserve clients.

## Quick setup (Ubuntu/Linux)
1. Install dependencies
   - For server (Python + Flask):
     sudo apt update
     sudo apt install -y python3 python3-venv python3-pip
     python3 -m venv .venv
     source .venv/bin/activate
     pip install flask
   - For client (C++ with libcurl):
     sudo apt install -y g++ libcurl4-openssl-dev build-essential

2. Start the server
   - From the workspace root:
     python3 server.py
   - The server listens on http://0.0.0.0:5000 by default. To open in the host browser:
     $BROWSER http://localhost:5000

3. Build the CLI client (optional, prebuilt binary may exist)
   g++ -std=c++17 main.cpp -o network-terminal -lcurl

## Basic usage
- Create a user (server must be running):
  ./netserve create <username> <password>
  or
  curl -X POST -d '{"username":"user","password":"pass"}' http://localhost:5000/api/user/create

- Log into an account:
   ./netserve login <username> <password>

- Upload a file (client splits into chunks automatically):
  ./netserve upload /path/to/file [username password]

- List files:
  ./netserve list [username password]

- Download a file:
  ./netserve download <filename> [username password]
  - This puts the file in your downloads directory by default.

Notes:
- After unning "netserve login <username> <password>", There is no need to log in again.
- The server enforces a max chunk size (CHUNK_SIZE in server.py — defaults to ~90 MB).

## Storage & data
- uploads/
  - incomplete/ — temporary chunk folders per upload
  - complete/ — assembled files available for download
  - metadata.json — per-file metadata and ownership
  - users.json — stored user hashes
- Ensure the server process user can read/write the uploads/ directory.

## Security & limitations
- Basic HTTP auth only. Not production-hardened (no TLS by default). Use behind a VPN, SSH tunnel, or add a reverse proxy with TLS for public use.
- Authentication and user storage are intentionally simple (suitable for private/self-hosted use).
- Chunk size and other policies are enforced server-side — see server.py for details.

## Troubleshooting
- Permission errors: ensure uploads/ is writable by the server user.
- Rebuild client if libcurl linking fails: confirm libcurl4-openssl-dev is installed.
- To reset state (loss of users/metadata): stop server, back up or delete uploads/users.json and uploads/metadata.json, then restart.

## Contributing
- Server logic: server.py
- Client: main.cpp
- Tests / fixes: open a PR or edit files in this workspace.

License: MIT — see LICENSE

Thank You for helping build a better, more decentralized web for the people!
