#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>
#include <curl/curl.h>

static const size_t CHUNK_SIZE = 90ull * 1024 * 1024; // 90 MB

// Helper function to handle the response data
size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
    size_t total_size = size * nmemb;
    output->append((char*)contents, total_size);
    return total_size;
}

static std::string get_field_string(const std::string &json, const std::string &key) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return "";
    size_t colon = json.find(":", pos + pattern.size());
    if (colon == std::string::npos) return "";
    size_t first_quote = json.find("\"", colon + 1);
    if (first_quote == std::string::npos) return "";
    size_t second_quote = json.find("\"", first_quote + 1);
    if (second_quote == std::string::npos) return "";
    return json.substr(first_quote + 1, second_quote - first_quote - 1);
}

static long get_content_length(const std::string &path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return (long)st.st_size;
    return -1;
}

// Credentials storage
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
    // restrict permissions: owner read/write
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

bool create_user(const std::string &username, const std::string &password) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    std::string url = "http://localhost:5000/api/user/create";
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
    std::string url = "http://localhost:5000/api/upload/init";
    std::ostringstream oss;
    oss << "{\"filename\":\"" << filename << "\",\"total_size\":" << total_size << "}";
    std::string json = oss.str();

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // Basic auth
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_USERNAME, username.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, password.c_str());

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    std::string file_id;
    if (res == CURLE_OK) {
        file_id = get_field_string(response, "file_id");
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
    std::string url = "http://localhost:5000/api/upload/chunk";

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

    // filename (optional)
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
        if ((long)to_read > (total_size - (long)i * (long)CHUNK_SIZE)) {
            to_read = (size_t)(total_size - (long)i * (long)CHUNK_SIZE);
        }
        std::vector<char> buffer;
        buffer.resize(to_read);
        in.read(buffer.data(), to_read);
        std::streamsize actually = in.gcount();
        out.write(buffer.data(), actually);
        out.close();

        bool ok = upload_chunk(file_id, i, total_chunks, filename, tmpname, username, password);
        // remove temp file
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

bool list_files(const std::string &username, const std::string &password) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    std::string url = "http://localhost:5000/api/files";
    std::string response;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);

    // Basic auth
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_USERNAME, username.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, password.c_str());

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    bool ok = (res == CURLE_OK);
    if (!ok) std::cerr << "list_files failed: " << curl_easy_strerror(res) << std::endl;
    else std::cout << "Files: " << response << std::endl;

    curl_easy_cleanup(curl);
    return ok;
}

bool download_file(const std::string &filename, const std::string &outpath, const std::string &username, const std::string &password) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    std::string url = "http://localhost:5000/api/download/" + filename;
    FILE* fout = fopen(outpath.c_str(), "wb");
    if (!fout) {
        curl_easy_cleanup(curl);
        std::cerr << "Cannot open output file: " << outpath << std::endl;
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    // Basic auth
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_USERNAME, username.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, password.c_str());

    // use default fwrite callback, provide FILE*
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

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "Usage:\n"
                  << "  create_user <username> <password>\n"
                  << "  login <username> <password>         # save credentials locally\n"
                  << "  logout                              # clear saved credentials\n"
                  << "  whoami                              # show saved username\n"
                  << "  upload <filepath> [username password]\n"
                  << "  list_files [username password]\n"
                  << "  download <filename> <outpath> [username password]\n";
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    std::string cmd = argv[1];
    if (cmd == "create_user") {
        if (argc != 4) { std::cerr << "create_user requires username and password\n"; return 1; }
        bool ok = create_user(argv[2], argv[3]);
        curl_global_cleanup();
        return ok ? 0 : 1;
    } else if (cmd == "login") {
        if (argc != 4) { std::cerr << "login requires username and password\n"; return 1; }
        bool ok = save_credentials(argv[2], argv[3]);
        std::cout << (ok ? "Credentials saved\n" : "Failed to save credentials\n");
        return ok ? 0 : 1;
    } else if (cmd == "logout") {
        bool ok = clear_credentials();
        std::cout << (ok ? "Logged out\n" : "No credentials found or failed to delete\n");
        return ok ? 0 : 1;
    } else if (cmd == "whoami") {
        std::string user, pass;
        if (load_credentials(user, pass)) {
            std::cout << "Saved username: " << user << "\n";
            return 0;
        } else {
            std::cout << "No saved credentials\n";
            return 1;
        }
    } else if (cmd == "upload") {
        std::string filepath;
        std::string user, pass;
        if (argc == 3) {
            filepath = argv[2];
            if (!load_credentials(user, pass)) { std::cerr << "No saved credentials; provide username and password\n"; return 1; }
        } else if (argc == 5) {
            filepath = argv[2];
            user = argv[3];
            pass = argv[4];
        } else {
            std::cerr << "upload requires filepath [username password]\n";
            return 1;
        }
        bool ok = upload_file(filepath, user, pass);
        curl_global_cleanup();
        return ok ? 0 : 1;
    } else if (cmd == "list_files") {
        std::string user, pass;
        if (argc == 2) {
            if (!load_credentials(user, pass)) { std::cerr << "No saved credentials; provide username and password\n"; return 1; }
        } else if (argc == 4) {
            user = argv[2];
            pass = argv[3];
        } else {
            std::cerr << "list_files requires [username password]\n";
            return 1;
        }
        bool ok = list_files(user, pass);
        curl_global_cleanup();
        return ok ? 0 : 1;
    } else if (cmd == "download") {
        std::string filename, outpath, user, pass;
        if (argc == 4) {
            filename = argv[2];
            outpath = argv[3];
            if (!load_credentials(user, pass)) { std::cerr << "No saved credentials; provide username and password\n"; return 1; }
        } else if (argc == 6) {
            filename = argv[2];
            outpath = argv[3];
            user = argv[4];
            pass = argv[5];
        } else {
            std::cerr << "download requires filename outpath [username password]\n";
            return 1;
        }
        bool ok = download_file(filename, outpath, user, pass);
        curl_global_cleanup();
        return ok ? 0 : 1;
    } else {
        std::cerr << "Unknown command\n";
        curl_global_cleanup();
        return 1;
    }
}
