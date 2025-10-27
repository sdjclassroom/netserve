// netserve.cpp
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>
#include <cerrno>
#include <algorithm>
#include <iomanip>
#include <cctype>
#include <curl/curl.h>

static const size_t CHUNK_SIZE = 90ull * 1024 * 1024; // 90 MB

// ---------------- Helpers ----------------
size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
    size_t total_size = size * nmemb;
    output->append((char*)contents, total_size);
    return total_size;
}

static long get_content_length(const std::string &path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return (long)st.st_size;
    return -1;
}

static std::string credentials_path() {
    const char* home = getenv("HOME");
    if (home && home[0] != '\0') {
        return std::string(home) + "/.network_terminal_credentials";
    }
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir) {
        return std::string(pw->pw_dir) + "/.network_terminal_credentials";
    }
    return std::string(".") + "/.network_terminal_credentials";
}

static std::string server_config_path() {
    const char* home = getenv("HOME");
    if (home && home[0] != '\0') {
        return std::string(home) + "/.network_terminal_server";
    }
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir) {
        return std::string(pw->pw_dir) + "/.network_terminal_server";
    }
    return std::string(".") + "/.network_terminal_server";
}

static bool save_server_url(const std::string &url) {
    std::string path = server_config_path();
    std::string tmp = path + ".tmp";
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return false;
    out << url << "\n";
    out.close();
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    chmod(path.c_str(), S_IRUSR | S_IWUSR);
    return true;
}

static bool load_server_url(std::string &url_out) {
    std::string path = server_config_path();
    std::ifstream in(path);
    if (!in) return false;
    if (!std::getline(in, url_out)) return false;
    // trim whitespace
    url_out.erase(url_out.begin(), std::find_if(url_out.begin(), url_out.end(), [](int ch){ return !std::isspace(ch); }));
    url_out.erase(std::find_if(url_out.rbegin(), url_out.rend(), [](int ch){ return !std::isspace(ch); }).base(), url_out.end());
    return true;
}

static bool save_credentials(const std::string &username, const std::string &password) {
    std::string path = credentials_path();
    std::string tmp = path + ".tmp";
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return false;
    out << username << "\n" << password << "\n";
    out.close();
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    chmod(path.c_str(), S_IRUSR | S_IWUSR);
    return true;
}

static bool load_credentials(std::string &username, std::string &password) {
    std::string path = credentials_path();
    std::ifstream in(path);
    if (!in) return false;
    if (!std::getline(in, username)) return false;
    if (!std::getline(in, password)) return false;
    return true;
}

static bool clear_credentials() {
    std::string path = credentials_path();
    return unlink(path.c_str()) == 0 || errno == ENOENT;
}

// ---------------- Server endpoint builder ----------------
static std::string g_base_url = "http://10.0.1.128:5001"; // default

static std::string endpoint(const std::string &path) {
    if (g_base_url.empty()) return path;
    if (g_base_url.back() == '/' && path.size() && path.front() == '/') {
        return g_base_url + path.substr(1);
    } else if (g_base_url.back() != '/' && path.size() && path.front() != '/') {
        return g_base_url + "/" + path;
    } else {
        return g_base_url + path;
    }
}

// ---------------- Network operations ----------------
bool create_user(const std::string &username, const std::string &password) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    std::string url = endpoint("/api/user/create");
    std::string json = "{\"username\":\"" + username + "\",\"password\":\"" + password + "\"}";

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    bool ok = (res == CURLE_OK);
    if (!ok) std::cerr << "create_user failed: " << curl_easy_strerror(res) << std::endl;

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ok;
}

// returns file_id or empty on error
std::string init_upload(const std::string &filename, long total_size, const std::string &username, const std::string &password) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";
    std::string url = endpoint("/api/upload/init");
    std::ostringstream oss;
    oss << "{\"filename\":\"" << filename << "\",\"total_size\":" << total_size << "}";
    std::string json = oss.str();

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_USERNAME, username.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, password.c_str());

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    std::string file_id;
    if (res == CURLE_OK) {
        size_t p = response.find("\"file_id\"");
        if (p != std::string::npos) {
            size_t colon = response.find(":", p);
            if (colon != std::string::npos) {
                size_t q1 = response.find("\"", colon);
                if (q1 != std::string::npos) {
                    size_t q2 = response.find("\"", q1 + 1);
                    if (q2 != std::string::npos) file_id = response.substr(q1 + 1, q2 - q1 - 1);
                }
            }
        }
        if (file_id.empty()) {
            std::cerr << "init_upload: no file_id in response: " << response << std::endl;
        }
    } else {
        std::cerr << "init_upload failed: " << curl_easy_strerror(res) << std::endl;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return file_id;
}

bool upload_chunk(const std::string &file_id, int chunk_index, int total_chunks, const std::string &file_field_name,
                  const std::string &chunk_path, const std::string &username, const std::string &password) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    std::string url = endpoint("/api/upload/chunk");

    curl_mime *form = curl_mime_init(curl);

    // file_id
    curl_mimepart *part = curl_mime_addpart(form);
    curl_mime_name(part, "file_id");
    curl_mime_data(part, file_id.c_str(), CURL_ZERO_TERMINATED);

    // chunk_index
    part = curl_mime_addpart(form);
    curl_mime_name(part, "chunk_index");
    std::string idxs = std::to_string(chunk_index);
    curl_mime_data(part, idxs.c_str(), CURL_ZERO_TERMINATED);

    // total_chunks
    part = curl_mime_addpart(form);
    curl_mime_name(part, "total_chunks");
    std::string tots = std::to_string(total_chunks);
    curl_mime_data(part, tots.c_str(), CURL_ZERO_TERMINATED);

    // filename
    part = curl_mime_addpart(form);
    curl_mime_name(part, "filename");
    curl_mime_data(part, file_field_name.c_str(), CURL_ZERO_TERMINATED);

    // chunk file
    part = curl_mime_addpart(form);
    curl_mime_name(part, "chunk");
    curl_mime_filedata(part, chunk_path.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, form);

    // Basic auth
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_USERNAME, username.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, password.c_str());

    std::string response;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    bool ok = (res == CURLE_OK);
    if (!ok) {
        std::cerr << "upload_chunk failed: " << curl_easy_strerror(res) << std::endl;
    } else {
        std::cout << "Uploaded chunk " << chunk_index << " response: " << response << std::endl;
    }

    curl_mime_free(form);
    curl_easy_cleanup(curl);
    return ok;
}

bool upload_file(const std::string &path, const std::string &username, const std::string &password) {
    long total_size = get_content_length(path);
    if (total_size < 0) {
        std::cerr << "Cannot stat file: " << path << std::endl;
        return false;
    }

    int total_chunks = (int)((total_size + (long)CHUNK_SIZE - 1) / (long)CHUNK_SIZE);
    std::string filename;
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) filename = path;
    else filename = path.substr(pos + 1);

    std::string file_id = init_upload(filename, total_size, username, password);
    if (file_id.empty()) {
        std::cerr << "init_upload failed" << std::endl;
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "Unable to open file for reading: " << path << std::endl;
        return false;
    }

    for (int i = 0; i < total_chunks; ++i) {
        std::string tmpname = "/tmp/nt_chunk_" + file_id + "_" + std::to_string(i) + ".part";
        std::ofstream out(tmpname, std::ios::binary);
        if (!out) {
            std::cerr << "Unable to create temp chunk file: " << tmpname << std::endl;
            in.close();
            return false;
        }

        size_t to_read = CHUNK_SIZE;
        long remaining = total_size - (long)i * (long)CHUNK_SIZE;
        if ((long)to_read > remaining) to_read = (size_t)remaining;

        std::vector<char> buffer;
        buffer.resize(to_read);
        in.read(buffer.data(), to_read);
        std::streamsize actually = in.gcount();
        out.write(buffer.data(), actually);
        out.close();

        bool ok = upload_chunk(file_id, i, total_chunks, filename, tmpname, username, password);
        unlink(tmpname.c_str());
        if (!ok) {
            std::cerr << "Failed uploading chunk " << i << std::endl;
            in.close();
            return false;
        }
    }

    in.close();
    std::cout << "Upload complete for " << path << std::endl;
    return true;
}

// ---------------- Listing / metadata ----------------
struct FileEntry { std::string file_id; std::string filename; long size; };

static std::string human_readable_size(long filesize) {
    if (filesize < 0) return "unknown";
    if (filesize < 1024) return std::to_string(filesize) + " B";
    if (filesize < 1024 * 1024) {
        long kb = filesize / 1024;
        return std::to_string(kb) + " KB";
    }
    double mb = (double)filesize / (1024.0 * 1024.0);
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << mb << " MB";
    return oss.str();
}

// fetch /api/files and parse small JSON payload into vector<FileEntry>
bool get_files_meta(const std::string &username, const std::string &password, std::vector<FileEntry> &out_items) {
    out_items.clear();
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    std::string url = endpoint("/api/files");
    std::string response;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_USERNAME, username.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, password.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    bool ok = (res == CURLE_OK);
    if (!ok) {
        std::cerr << "get_files_meta failed: " << curl_easy_strerror(res) << std::endl;
        curl_easy_cleanup(curl);
        return false;
    }

    // find array start for "files"
    size_t files_key = response.find("\"files\"");
    size_t arr_start = std::string::npos;
    if (files_key != std::string::npos) arr_start = response.find("[", files_key);
    if (arr_start == std::string::npos) arr_start = response.find("[");
    if (arr_start == std::string::npos) { curl_easy_cleanup(curl); return true; }

    size_t arr_end = response.find("]", arr_start);
    if (arr_end == std::string::npos) arr_end = response.size();

    size_t pos = arr_start;
    while (true) {
        size_t obj_start = response.find("{", pos);
        if (obj_start == std::string::npos || obj_start > arr_end) break;
        size_t obj_end = response.find("}", obj_start);
        if (obj_end == std::string::npos || obj_end > arr_end) break;
        std::string obj = response.substr(obj_start, obj_end - obj_start + 1);

        FileEntry e; e.file_id = ""; e.filename = ""; e.size = -1;

        // file_id
        size_t fid_pos = obj.find("\"file_id\"");
        if (fid_pos != std::string::npos) {
            size_t colon = obj.find(":", fid_pos);
            size_t q1 = obj.find("\"", colon);
            if (q1 != std::string::npos) {
                size_t q2 = obj.find("\"", q1 + 1);
                if (q2 != std::string::npos) e.file_id = obj.substr(q1 + 1, q2 - q1 - 1);
            }
        }

        // filename
        size_t fname_pos = obj.find("\"filename\"");
        if (fname_pos != std::string::npos) {
            size_t colon = obj.find(":", fname_pos);
            size_t q1 = obj.find("\"", colon);
            if (q1 != std::string::npos) {
                size_t q2 = obj.find("\"", q1 + 1);
                if (q2 != std::string::npos) e.filename = obj.substr(q1 + 1, q2 - q1 - 1);
            }
        }

        // size
        size_t size_pos = obj.find("\"size\"");
        if (size_pos != std::string::npos) {
            size_t colon = obj.find(":", size_pos);
            if (colon != std::string::npos) {
                size_t endpos = obj.find_first_of(",}", colon);
                if (endpos == std::string::npos) endpos = obj.size();
                std::string size_str = obj.substr(colon + 1, endpos - colon - 1);
                // trim
                size_str.erase(size_str.begin(), std::find_if(size_str.begin(), size_str.end(), [](int ch){ return !std::isspace(ch); }));
                size_str.erase(std::find_if(size_str.rbegin(), size_str.rend(), [](int ch){ return !std::isspace(ch); }).base(), size_str.end());
                // digits only
                std::string digits;
                for (char c : size_str) if (std::isdigit((unsigned char)c)) digits.push_back(c);
                if (!digits.empty()) {
                    try { e.size = std::stol(digits); } catch(...) { e.size = -1; }
                }
            }
        }

        if (!e.filename.empty() || !e.file_id.empty()) out_items.push_back(e);
        pos = obj_end + 1;
    }

    curl_easy_cleanup(curl);
    return true;
}

bool list_files(const std::string &username, const std::string &password) {
    std::vector<FileEntry> items;
    if (!get_files_meta(username, password, items)) return false;

    if (items.empty()) {
        std::cout << "Files: (none)\n";
        return true;
    }

    size_t max_name = std::string("Filename").size();
    size_t id_width = std::string("FileID").size();
    for (auto &e : items) {
        max_name = std::max(max_name, e.filename.size());
        id_width = std::max(id_width, (size_t)8);
    }

    std::cout << std::left << std::setw((int)id_width + 2) << "FileID"
              << std::left << std::setw((int)max_name + 2) << "Filename"
              << "Size\n";
    std::cout << std::string(id_width + 2 + max_name + 2 + 6, '-') << "\n";

    for (auto &e : items) {
        std::string short_id = e.file_id.size() > 8 ? e.file_id.substr(0,8) : e.file_id;
        std::cout << std::left << std::setw((int)id_width + 2) << short_id
                  << std::left << std::setw((int)max_name + 2) << e.filename
                  << human_readable_size(e.size) << "\n";
    }
    return true;
}

// ---------------- Share / Delete client ops ----------------
bool resolve_file_id(const std::string &id_or_name, const std::string &username, const std::string &password, std::string &out_file_id) {
    out_file_id.clear();
    bool looks_like_id = false;
    if (id_or_name.find("-") != std::string::npos) looks_like_id = true;
    if (id_or_name.size() >= 8 && !looks_like_id) {
        size_t count_hex = 0;
        for (char c : id_or_name) if (isxdigit((unsigned char)c)) count_hex++;
        if (count_hex >= 8) looks_like_id = true;
    }

    std::vector<FileEntry> items;
    if (!get_files_meta(username, password, items)) return false;

    if (looks_like_id) {
        for (auto &e : items) if (e.file_id == id_or_name) { out_file_id = e.file_id; return true; }
        for (auto &e : items) if (e.file_id.size() >= id_or_name.size() && e.file_id.substr(0, id_or_name.size()) == id_or_name) { out_file_id = e.file_id; return true; }
    }

    for (auto &e : items) if (e.filename == id_or_name) { out_file_id = e.file_id; return true; }

    return false;
}

bool client_share(const std::string &id_or_name, const std::string &share_with, const std::string &username, const std::string &password) {
    std::string file_id;
    if (!resolve_file_id(id_or_name, username, password, file_id)) {
        std::cerr << "Could not find file matching '" << id_or_name << "'\n";
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) return false;
    std::string url = endpoint("/api/file/share");
    std::string json = "{\"file_id\":\"" + file_id + "\",\"share_with\":\"" + share_with + "\"}";

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_USERNAME, username.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, password.c_str());

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    bool ok = (res == CURLE_OK);
    if (!ok) std::cerr << "share failed: " << curl_easy_strerror(res) << std::endl;
    else std::cout << "Share response: " << response << std::endl;

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ok;
}

bool client_delete(const std::string &id_or_name, const std::string &username, const std::string &password) {
    std::string file_id;
    if (!resolve_file_id(id_or_name, username, password, file_id)) {
        std::cerr << "Could not find file matching '" << id_or_name << "'\n";
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) return false;
    std::string url = endpoint("/api/file/") + file_id;

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");

    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_USERNAME, username.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, password.c_str());

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    bool ok = (res == CURLE_OK);
    if (!ok) std::cerr << "delete failed: " << curl_easy_strerror(res) << std::endl;
    else std::cout << "Delete response: " << response << std::endl;

    curl_easy_cleanup(curl);
    return ok;
}

// ---------------- Download ----------------
bool download_file(const std::string &filename, const std::string &username, const std::string &password) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    std::string url = endpoint("/api/download/") + filename;

    const char* home = getenv("HOME");
    std::string downloads_dir;
    if (home) {
        downloads_dir = std::string(home) + "/Downloads";
        struct stat st;
        if (stat(downloads_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            downloads_dir = std::string(home);
        }
    } else downloads_dir = ".";

    std::string outpath = downloads_dir + "/" + filename;

    FILE* fout = fopen(outpath.c_str(), "wb");
    if (!fout) {
        curl_easy_cleanup(curl);
        std::cerr << "Cannot open output file: " << outpath << std::endl;
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_USERNAME, username.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, password.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fout);

    CURLcode res = curl_easy_perform(curl);
    fclose(fout);

    bool ok = (res == CURLE_OK);
    if (!ok) {
        std::cerr << "download_file failed: " << curl_easy_strerror(res) << std::endl;
        unlink(outpath.c_str());
    } else {
        std::cout << "Downloaded to " << outpath << std::endl;
    }

    curl_easy_cleanup(curl);
    return ok;
}

// ---------------- CLI ----------------
int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "Usage:\n"
                  << "  server [url]                               # show or set server URL           \n"
                  << "  create <username> <password>               # creates an account on the server \n"
                  << "  login <username> <password>                # save credentials locally         \n"
                  << "  logout                                     # clear saved credentials          \n"
                  << "  user                                       # show saved username              \n"
                  << "  upload <filepath>                          # uploads the specified file       \n"
                  << "  list                                       # lists the files owned by user    \n"
                  << "  share <file_id_or_filename> <user>         # shares ownership of the file     \n"
                  << "  delete <file_id_or_filename>               # deletes the specified file       \n"
                  << "  download <filename>                        # downloads the specified file     \n";
        return 1;
    }

    std::string configured_url;
    if (load_server_url(configured_url)) g_base_url = configured_url;

    curl_global_init(CURL_GLOBAL_DEFAULT);

    std::string cmd = argv[1];
    if (cmd == "server") {
        if (argc == 2) {
            std::cout << "Current server: " << g_base_url << "\n";
            curl_global_cleanup();
            return 0;
        } else if (argc == 3) {
            std::string url = argv[2];
            if (!save_server_url(url)) {
                std::cerr << "Failed to save server URL\n";
                curl_global_cleanup();
                return 1;
            }
            g_base_url = url;
            std::cout << "Server set to: " << g_base_url << "\n";
            curl_global_cleanup();
            return 0;
        } else {
            std::cerr << "serve takes zero or one argument: serve [url]\n";
            curl_global_cleanup();
            return 1;
        }
    } else if (cmd == "create") {
        if (argc != 4) { std::cerr << "create_user requires username and password\n"; curl_global_cleanup(); return 1; }
        bool ok = create_user(argv[2], argv[3]);
        curl_global_cleanup();
        return ok ? 0 : 1;
    } else if (cmd == "login") {
        if (argc != 4) { std::cerr << "login requires username and password\n"; curl_global_cleanup(); return 1; }
        bool ok = save_credentials(argv[2], argv[3]);
        std::cout << (ok ? "Credentials saved\n" : "Failed to save credentials\n");
        curl_global_cleanup();
        return ok ? 0 : 1;
    } else if (cmd == "logout") {
        bool ok = clear_credentials();
        std::cout << (ok ? "Logged out\n" : "No credentials found or failed to delete\n");
        curl_global_cleanup();
        return ok ? 0 : 1;
    } else if (cmd == "user") {
        std::string user, pass;
        if (load_credentials(user, pass)) {
            std::cout << "Saved username: " << user << "\n";
            curl_global_cleanup();
            return 0;
        } else {
            std::cout << "No saved credentials\n";
            curl_global_cleanup();
            return 1;
        }
    } else if (cmd == "upload") {
        std::string filepath, user, pass;
        if (argc == 3) {
            filepath = argv[2];
            if (!load_credentials(user, pass)) { std::cerr << "No saved credentials; provide username and password\n"; curl_global_cleanup(); return 1; }
        } else if (argc == 5) {
            filepath = argv[2];
            user = argv[3]; pass = argv[4];
        } else {
            std::cerr << "upload requires filepath [username password]\n";
            curl_global_cleanup();
            return 1;
        }
        bool ok = upload_file(filepath, user, pass);
        curl_global_cleanup();
        return ok ? 0 : 1;
    } else if (cmd == "list") {
        std::string user, pass;
        if (argc == 2) {
            if (!load_credentials(user, pass)) { std::cerr << "No saved credentials; provide username and password\n"; curl_global_cleanup(); return 1; }
        } else if (argc == 4) {
            user = argv[2]; pass = argv[3];
        } else {
            std::cerr << "list requires [username password]\n";
            curl_global_cleanup();
            return 1;
        }
        bool ok = list_files(user, pass);
        curl_global_cleanup();
        return ok ? 0 : 1;
    } else if (cmd == "share") {
        if (!(argc == 4 || argc == 6)) { std::cerr << "Usage: share <file_id_or_filename> <target_user> [username password]\n"; curl_global_cleanup(); return 1; }
        std::string target = argv[2], share_with = argv[3], user, pass;
        if (argc == 4) {
            if (!load_credentials(user, pass)) { std::cerr << "No saved credentials; provide username and password\n"; curl_global_cleanup(); return 1; }
        } else { user = argv[4]; pass = argv[5]; }
        bool ok = client_share(target, share_with, user, pass);
        curl_global_cleanup();
        return ok ? 0 : 1;
    } else if (cmd == "delete") {
        if (!(argc == 3 || argc == 5)) { std::cerr << "Usage: delete <file_id_or_filename> [username password]\n"; curl_global_cleanup(); return 1; }
        std::string id = argv[2], user, pass;
        if (argc == 3) {
            if (!load_credentials(user, pass)) { std::cerr << "No saved credentials; provide username and password\n"; curl_global_cleanup(); return 1; }
        } else { user = argv[3]; pass = argv[4]; }
        bool ok = client_delete(id, user, pass);
        curl_global_cleanup();
        return ok ? 0 : 1;
    } else if (cmd == "download") {
        std::string filename, user, pass;
        if (argc == 3) {
            filename = argv[2];
            if (!load_credentials(user, pass)) { std::cerr << "No saved credentials; provide username and password\n"; curl_global_cleanup(); return 1; }
        } else if (argc == 5) {
            filename = argv[2]; user = argv[3]; pass = argv[4];
        } else {
            std::cerr << "download requires filename [username password]\n";
            curl_global_cleanup();
            return 1;
        }
        bool ok = download_file(filename, user, pass);
        curl_global_cleanup();
        return ok ? 0 : 1;
    } else {
        std::cerr << "Unknown command\n";
        curl_global_cleanup();
        return 1;
    }
}
