from flask import Flask, jsonify, request, send_from_directory, g
from werkzeug.utils import secure_filename
from werkzeug.security import generate_password_hash, check_password_hash
import os
import math
import uuid
import threading
import json
import base64

app = Flask(__name__)

# configuration
CHUNK_SIZE = 90 * 1024 * 1024  # 90 MB
BASE_UPLOAD_DIR = os.path.join(os.getcwd(), "uploads")
INCOMPLETE_DIR = os.path.join(BASE_UPLOAD_DIR, "incomplete")
COMPLETE_DIR = os.path.join(BASE_UPLOAD_DIR, "complete")
USERS_FILE = os.path.join(BASE_UPLOAD_DIR, "users.json")
METADATA_FILE = os.path.join(BASE_UPLOAD_DIR, "metadata.json")

os.makedirs(INCOMPLETE_DIR, exist_ok=True)
os.makedirs(COMPLETE_DIR, exist_ok=True)

# persistent storage helpers (very simple file-backed JSON)
_storage_lock = threading.Lock()

def _load_json(path):
    if not os.path.exists(path):
        return {}
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}

def _save_json(path, data):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f)
    os.replace(tmp, path)

def load_users():
    with _storage_lock:
        return _load_json(USERS_FILE)

def save_users(users):
    with _storage_lock:
        _save_json(USERS_FILE, users)

def load_metadata():
    with _storage_lock:
        return _load_json(METADATA_FILE)

def save_metadata(meta):
    with _storage_lock:
        _save_json(METADATA_FILE, meta)

# simple in-memory locks for per-file assembly
_locks = {}
_locks_lock = threading.Lock()

def _get_lock(file_id):
    with _locks_lock:
        if file_id not in _locks:
            _locks[file_id] = threading.Lock()
        return _locks[file_id]

# Basic auth helper
def _parse_basic_auth(auth_header):
    if not auth_header or not auth_header.lower().startswith("basic "):
        return None, None
    try:
        b64 = auth_header.split(None, 1)[1]
        creds = base64.b64decode(b64).decode("utf-8")
        username, password = creds.split(":", 1)
        return username, password
    except Exception:
        return None, None

def require_auth(f):
    def wrapper(*args, **kwargs):
        auth = request.headers.get("Authorization")
        username, password = _parse_basic_auth(auth)
        if not username:
            return jsonify({"error": "Authorization required (Basic)"}), 401
        users = load_users()
        if username not in users:
            return jsonify({"error": "Invalid credentials"}), 403
        if not check_password_hash(users[username]["password_hash"], password):
            return jsonify({"error": "Invalid credentials"}), 403
        g.current_user = username
        return f(*args, **kwargs)
    wrapper.__name__ = f.__name__
    return wrapper

# Simple GET endpoint
@app.route('/api/greet', methods=['GET'])
def greet():
    return jsonify({"message": "Hello from Python!"})

# Create user endpoint (no auth required)
@app.route('/api/user/create', methods=['POST'])
def create_user():
    data = request.get_json(force=True)
    username = data.get("username")
    password = data.get("password")
    if not username or not password:
        return jsonify({"error": "username and password required"}), 400
    users = load_users()
    if username in users:
        return jsonify({"error": "user exists"}), 409
    users[username] = {"password_hash": generate_password_hash(password)}
    save_users(users)
    return jsonify({"status": "created", "username": username}), 201

# Initialize a new upload, returns a file_id and expected number of chunks (if total_size provided)
@app.route('/api/upload/init', methods=['POST'])
@require_auth
def init_upload():
    data = request.get_json(force=True)
    filename = data.get("filename")
    total_size = data.get("total_size")  # optional, in bytes

    if not filename:
        return jsonify({"error": "filename is required"}), 400

    safe_name = secure_filename(filename)
    file_id = str(uuid.uuid4())
    folder = os.path.join(INCOMPLETE_DIR, file_id)
    os.makedirs(folder, exist_ok=True)

    expected_chunks = None
    if isinstance(total_size, int) and total_size > 0:
        expected_chunks = math.ceil(total_size / CHUNK_SIZE)

    # store ownership metadata
    meta = load_metadata()
    meta[file_id] = {
        "owner": g.current_user,
        "filename": safe_name,
        "expected_chunks": expected_chunks,
        "assembled": False
    }
    save_metadata(meta)

    return jsonify({"file_id": file_id, "filename": safe_name, "expected_chunks": expected_chunks}), 201

# Upload a single chunk as multipart/form-data:
# fields: file_id, chunk_index (0-based), total_chunks (optional), filename
# file field name: chunk
@app.route('/api/upload/chunk', methods=['POST'])
@require_auth
def upload_chunk():
    file_id = request.form.get("file_id")
    chunk_index = request.form.get("chunk_index")
    total_chunks = request.form.get("total_chunks")
    filename = request.form.get("filename")

    if not file_id or chunk_index is None or 'chunk' not in request.files:
        return jsonify({"error": "file_id, chunk_index and chunk file are required"}), 400

    # verify ownership exists and belongs to current user
    meta = load_metadata()
    if file_id not in meta:
        return jsonify({"error": "invalid file_id"}), 404
    if meta[file_id]["owner"] != g.current_user:
        return jsonify({"error": "not authorized for this file_id"}), 403

    try:
        chunk_index = int(chunk_index)
    except ValueError:
        return jsonify({"error": "chunk_index must be an integer"}), 400

    try:
        total_chunks = int(total_chunks) if total_chunks is not None else None
    except ValueError:
        total_chunks = None

    safe_name = secure_filename(filename) if filename else meta[file_id].get("filename")
    dest_folder = os.path.join(INCOMPLETE_DIR, file_id)
    os.makedirs(dest_folder, exist_ok=True)

    chunk_file = request.files['chunk']
    chunk_filename = f"{chunk_index}.chunk"
    chunk_path = os.path.join(dest_folder, chunk_filename)

    # Save chunk to disk first
    chunk_file.save(chunk_path)

    # Enforce max chunk size
    size = os.path.getsize(chunk_path)
    if size > CHUNK_SIZE:
        os.remove(chunk_path)
        return jsonify({"error": f"Chunk too large ({size} bytes). Max allowed is {CHUNK_SIZE} bytes."}), 413

    # If total_chunks provided, check whether we have all chunks -> assemble
    assembled = False
    # prefer provided total_chunks, otherwise check metadata
    expect = total_chunks if total_chunks is not None else meta[file_id].get("expected_chunks")
    if expect is not None:
        present = [name for name in os.listdir(dest_folder) if name.endswith(".chunk")]
        if len(present) == expect:
            lock = _get_lock(file_id)
            with lock:
                # double-check presence inside lock
                present = [name for name in os.listdir(dest_folder) if name.endswith(".chunk")]
                if len(present) == expect and not meta[file_id].get("assembled", False):
                    # assemble
                    final_name = safe_name if safe_name else f"{file_id}.bin"
                    final_path = os.path.join(COMPLETE_DIR, final_name)
                    with open(final_path, "wb") as fout:
                        for i in range(expect):
                            part = os.path.join(dest_folder, f"{i}.chunk")
                            if not os.path.exists(part):
                                return jsonify({"error": f"Missing chunk {i} during assembly"}), 500
                            with open(part, "rb") as fin:
                                while True:
                                    data = fin.read(4 * 1024 * 1024)
                                    if not data:
                                        break
                                    fout.write(data)
                    # cleanup chunk folder
                    for f in os.listdir(dest_folder):
                        try:
                            os.remove(os.path.join(dest_folder, f))
                        except Exception:
                            pass
                    try:
                        os.rmdir(dest_folder)
                    except Exception:
                        pass
                    assembled = True
                    # update metadata
                    meta[file_id]["assembled"] = True
                    meta[file_id]["final_filename"] = final_name
                    save_metadata(meta)

    resp = {"status": "uploaded", "file_id": file_id, "chunk_index": chunk_index}
    if assembled:
        resp["assembled"] = True
        resp["filename"] = meta[file_id].get("final_filename", safe_name if safe_name else f"{file_id}.bin")

    return jsonify(resp), 200

# List completed files (only show files owned by caller)
@app.route('/api/files', methods=['GET'])
@require_auth
def list_files():
    files = []
    meta = load_metadata()
    for fid, info in meta.items():
        if info.get("assembled") and info.get("owner") == g.current_user:
            final_name = info.get("final_filename", info.get("filename", f"{fid}.bin"))
            path = os.path.join(COMPLETE_DIR, final_name)
            if os.path.isfile(path):
                stat = os.stat(path)
                files.append({"file_id": fid, "filename": final_name, "size": stat.st_size})
    return jsonify({"files": files})

# Download a completed file (only owner can download)
@app.route('/api/download/<path:filename>', methods=['GET'])
@require_auth
def download_file(filename):
    safe_name = secure_filename(filename)
    meta = load_metadata()
    # find matching metadata entry
    found = None
    for fid, info in meta.items():
        final = info.get("final_filename", info.get("filename"))
        if final == safe_name:
            found = (fid, info)
            break
    if not found:
        return jsonify({"error": "file not found"}), 404
    fid, info = found
    if info.get("owner") != g.current_user:
        return jsonify({"error": "not authorized to download this file"}), 403
    return send_from_directory(COMPLETE_DIR, safe_name, as_attachment=True)

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000)
