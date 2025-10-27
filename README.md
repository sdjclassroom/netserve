# Netserve

A lightweight private file sharing stack: a Flask server plus a single-file C++ CLI client (think minimal Google Drive over the terminal).  
Built for personal use over a Cloudflare Tunnel or a direct IP, with large file uploads in chunks, per-file ownership, and simple authenticated downloads.

---

## Table of Contents
- [Features](#features)
- [Architecture](#architecture)
- [Requirements](#requirements)
- [Quick Start](#quick-start)
  - [Start the Server](#start-the-server)
  - [Build the CLI Client](#build-the-cli-client)
- [Usage](#usage)
- [Storage Layout](#storage-layout)
- [Deployment Notes](#deployment-notes)
  - [Cloudflare Tunnel](#cloudflare-tunnel)
  - [Security Considerations](#security-considerations)
- [Prebuilt Binaries](#prebuilt-binaries)
- [Roadmap](#roadmap)
- [Contributing](#contributing)
- [License](#license)

---

## Features
- Private file exchange between users or machines
- Chunked uploads for large files with resumability potential
- Per-file ownership tracked in metadata
- Simple CLI for create, login, upload, list, and download
- Works well behind a Cloudflare Tunnel, which improves reachability

---

## Architecture
```

[ netserve (CLI) ]  <----libcurl---->  [ Flask HTTP server ]
|                                        |
|                                uploads/ (on disk)
|                                ├─ incomplete/   (temp chunks)
|                                ├─ complete/     (assembled files)
|                                ├─ metadata.json (file metadata + ownership)
|                                └─ users.json    (user credential hashes)

````

**Components**
- `server.py` - Flask HTTP server that exposes all operations
- `netserve.cpp` - single-file C++ CLI client that uses libcurl

---

## Requirements
**Server**
- Python 3.8 or newer
- Flask

**Client**
- g++ or clang++
- `libcurl` development headers

---

## Quick Start

### Start the Server
```bash
# Ubuntu or Debian
sudo apt update
sudo apt install -y python3 python3-pip
pip3 install flask

# run the server
python3 server.py
# The server listens on http://0.0.0.0:5000 by default
# Open in your browser if desired
$BROWSER http://localhost:5000
````

### Build the CLI Client

```bash
# Ubuntu or Debian
sudo apt install -y g++ libcurl4-openssl-dev build-essential
g++ netserve.cpp -o netserve
```

> The client expects the server at `http://localhost:5000` unless you specify a different URL with a flag such as `--server URL` or an environment variable. If your build uses a different mechanism, set the server location accordingly when you run commands.

---

## Usage

Create a user

```bash
./netserve create <username> <password> [--server http://host:5000]
```

Log in

```bash
./netserve login <username> <password> [--server http://host:5000]
```

Upload a file
(client splits into chunks automatically)

```bash
./netserve upload /path/to/file [username password] [--server http://host:5000]
```

List files

```bash
./netserve list [username password] [--server http://host:5000]
```

Download a file
(saves to your Downloads directory by default)

```bash
./netserve download <filename> [username password] [--server http://host:5000]
```

**Notes**

* After you run `netserve login <username> <password>`, the client persists your session locally. You do not need to log in again unless you clear the session.
* The server enforces a maximum chunk size defined by `CHUNK_SIZE` in `server.py` with a default near 90 MB.

---

## Storage Layout

On the server host:

```
uploads/
├─ incomplete/        # temporary chunk folders per in-progress upload
├─ complete/          # assembled files available for download
├─ metadata.json      # per-file metadata including ownership
└─ users.json         # stored user credential hashes
```

Make sure the server process user can read and write the `uploads/` directory.

---

## Deployment Notes

### Cloudflare Tunnel

Netserve is suitable for exposure through a Cloudflare Tunnel, which gives you:

* Stable public hostname that forwards to your local server port
* TLS termination by Cloudflare
* Lower friction for NAT and firewall traversal

Typical steps at a high level

1. Install `cloudflared` on the server host
2. Authenticate `cloudflared` with your Cloudflare account
3. Create a tunnel that targets `http://localhost:5000`
4. Map a DNS record in Cloudflare to the tunnel hostname
5. Point your client to `https://your-tunnel-hostname`

### Security Considerations

* Prefer HTTPS termination at the edge or a reverse proxy. If you use Cloudflare Tunnel, the edge provides TLS.
* Use strong, unique passwords. Consider rate limiting and request size limits at the proxy.
* Back up `uploads/complete`, `metadata.json`, and `users.json` regularly to preserve data durability and provenance.
* Treat `users.json` as sensitive. It contains password hashes, not plaintext.

---

## Prebuilt Binaries

If the repository contains prebuilt artifacts, availability is as follows.

| Platform | x86     | x86_64  | ARM32   | ARM64   |
| -------- | ------- | ------- | ------- | ------- |
| Linux    |         | present |         | present |
| Windows  |         |         |         |         |
| MacOS    |         |         |         |         |

If your platform is not listed, compile from source using the steps above.

---

## Roadmap

**Clients**

* Public API surface for programmatic automation
* Additional client apps that can talk to the same server

**Server**

* More granular operator controls for quotas, retention, and policy
* Operational metrics for throughput, latency, and storage utilization

---

## Contributing

* Server logic lives in `server.py`
* CLI client lives in `netserve.cpp`

Please open an issue or pull request with a clear description, expected behavior, and steps to reproduce any defects. For features, describe the user journey and any configuration changes.

---

## License

MIT. See `LICENSE` for details.
