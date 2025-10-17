# Network-Terminal

A small file-upload/download project with a Python HTTP server and a C++ command-line client.

## Components

- Server: Flask-based API implemented in [server.py](server.py). Key endpoints / helpers:
  - [`server.create_user`](server.py) — POST /api/user/create
  - [`server.init_upload`](server.py) — POST /api/upload/init
  - [`server.upload_chunk`](server.py) — POST /api/upload/chunk
  - [`server.list_files`](server.py) — GET /api/files
  - [`server.download_file`](server.py) — GET /api/download/<filename>
  - Auth helpers: [`server.require_auth`](server.py), [`server._parse_basic_auth`](server.py)
- Client CLI: C++ single-file client implemented in [main.cpp](main.cpp). Main helpers:
  - [`main.create_user`](main.cpp)
  - [`main.init_upload`](main.cpp)
  - [`main.upload_chunk`](main.cpp)
  - [`main.upload_file`](main.cpp)
  - [`main.list_files`](main.cpp)
  - [`main.download_file`](main.cpp)
  - Helpers: [`main.WriteCallback`](main.cpp), [`main.get_field_string`](main.cpp), credential helpers in [main.cpp](main.cpp)
- Example/aux files and binaries:
  - Local binaries: `netserve`, `netserve2`, `network-terminal`
  - Stored uploads and metadata: [uploads/metadata.json](uploads/metadata.json), [uploads/users.json](uploads/users.json), completed files in [uploads/complete/network-terminal](uploads/complete/network-terminal)

## Features

- Chunked uploads with server-side assembly. Server enforces a 90 MB chunk max (`CHUNK_SIZE` in [server.py](server.py)).
- Per-file ownership and metadata stored in [uploads/metadata.json](uploads/metadata.json).
- Basic HTTP Basic auth for all upload/download operations (see [`server.require_auth`](server.py)).
- Client supports saving credentials locally in the user's home (see credential helpers in [main.cpp](main.cpp)).

## Quickstart

1. Start the server
   - From the workspace root:
     - Python: python3 server.py
     - The server serves API on http://0.0.0.0:5000

2. Use the C++ CLI (prebuilt `network-terminal` or build from [main.cpp](main.cpp)):
   - Create a user:
     - CLI: network-terminal create_user <username> <password>
     - Or using the C++ client function: see [`main.create_user`](main.cpp)
   - Upload a file (client will split into chunks automatically):
     - network-terminal upload <filepath> [username password]
   - List files:
     - network-terminal list_files [username password]
   - Download:
     - network-terminal download <filename> <outpath> [username password]

## Building the client

If you want to build the C++ client yourself:

- Ensure libcurl and a C++ compiler are installed.
- Example build:
  - g++ -std=c++17 main.cpp -o network-terminal -lcurl
- See [main.cpp](main.cpp) for the client implementation and the functions listed above.

## Storage layout

- uploads/
  - incomplete/ — per-upload chunk folders
  - complete/ — assembled files (owner-restricted downloads)
  - metadata.json — upload metadata ([uploads/metadata.json](uploads/metadata.json))
  - users.json — stored user hashes ([uploads/users.json](uploads/users.json))

## Notes & troubleshooting

- The server enforces chunk size and checks for complete chunk sets before assembly. Review [`server.init_upload`](server.py) and [`server.upload_chunk`](server.py) for behavior details.
- If using saved credentials, the client stores them in a file path determined by [`main.credentials_path`](main.cpp).
- Prebuilt ELF binaries (`netserve`, `netserve2`) are present for convenience.

## Contributing

- Modify the server logic in [server.py](server.py).
- Update or extend the client in [main.cpp](main.cpp).
- Keep metadata in [uploads/metadata.json](uploads/metadata.json) and user hashes in [uploads/users.json](uploads/users.json).

License: MIT — see [LICENSE](LICENSE)
