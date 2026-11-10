// Mods from Google Drive: a shared file link downloads that mod; a shared folder link lists the mods in it, and
// each one can then be downloaded from the menu. Files must be shared as "anyone with the link".
#include "ModInstaller.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>

#include <imgui.h>
#include <libultraship/libultraship.h>
#include <ship/utils/StringHelper.h>
#include <zip.h>
#include "2s2h/BenPort.h"

#ifdef __ANDROID__
#include <SDL2/SDL.h>
#include <jni.h>
#elif defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#endif

namespace ModInstaller {

void SetNotice(const std::string& text); // ModInstaller.cpp

namespace {

namespace fs = std::filesystem;

std::mutex sMutex;
DownloadStatus sStatus;
std::atomic<bool> sBusy = false;
std::vector<DriveEntry> sFolder;
std::string sFolderId;    // the folder sFolder was listed from
std::string sPendingLink; // typed in the Android box (UI thread), opened on the game thread
bool sHasPendingLink = false;

struct HttpResult {
    bool ok = false;
    std::string fileName;    // from Content-Disposition
    std::string contentType; // from Content-Type
    std::string error;
};

void Progress(uint64_t done, uint64_t total) {
    std::lock_guard<std::mutex> lock(sMutex);
    sStatus.done = done;
    sStatus.total = total;
}

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool IsIdChar(char c) {
    return std::isalnum((unsigned char)c) || c == '-' || c == '_';
}

std::string PercentDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) &&
            std::isxdigit((unsigned char)s[i + 2])) {
            out += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// filename*=UTF-8''name.o2r wins over filename="name.o2r".
std::string FileNameFromDisposition(const std::string& header) {
    std::smatch m;
    static const std::regex star(R"(filename\*\s*=\s*[^']*'[^']*'([^;\r\n]+))", std::regex::icase);
    if (std::regex_search(header, m, star)) {
        return PercentDecode(m[1].str());
    }
    static const std::regex quoted(R"re(filename\s*=\s*"([^"]+)")re", std::regex::icase);
    if (std::regex_search(header, m, quoted)) {
        return m[1].str();
    }
    static const std::regex plain(R"(filename\s*=\s*([^;\r\n]+))", std::regex::icase);
    if (std::regex_search(header, m, plain)) {
        return m[1].str();
    }
    return "";
}

std::string DecodeEntities(std::string s) {
    const std::pair<const char*, const char*> entities[] = {
        { "&amp;", "&" }, { "&quot;", "\"" }, { "&#39;", "'" }, { "&lt;", "<" }, { "&gt;", ">" }
    };
    for (const auto& [from, to] : entities) {
        size_t pos;
        while ((pos = s.find(from)) != std::string::npos) {
            s.replace(pos, strlen(from), to);
        }
    }
    return s;
}

std::string SafeName(std::string name) {
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
            (unsigned char)c < 32) {
            c = '_';
        }
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
        name.pop_back();
    }
    return name;
}

bool HasArchiveExtension(const std::string& name) {
    std::string ext = fs::path(name).extension().string();
    return StringHelper::IEquals(ext, ".o2r") || StringHelper::IEquals(ext, ".otr") ||
           StringHelper::IEquals(ext, ".zip");
}

// ----- HTTP GET into a file, per platform.

#ifdef __ANDROID__
std::atomic<uint64_t> sAndroidDone = 0, sAndroidTotal = 0;

HttpResult HttpDownload(const std::string& url, const fs::path& to) {
    HttpResult result;
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv(); // attaches this thread
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (env == nullptr || activity == nullptr) {
        result.error = "sem acesso ao app";
        return result;
    }
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID method =
        env->GetMethodID(activityClass, "httpDownload", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    if (method != nullptr) {
        jstring jUrl = env->NewStringUTF(url.c_str());
        jstring jPath = env->NewStringUTF(to.string().c_str());
        jstring answer = (jstring)env->CallObjectMethod(activity, method, jUrl, jPath);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            result.error = "erro no download";
        } else if (answer != nullptr) {
            const char* chars = env->GetStringUTFChars(answer, nullptr);
            std::string text = chars;
            env->ReleaseStringUTFChars(answer, chars);
            env->DeleteLocalRef(answer);
            // "ok\n<file name>\n<content type>" or "erro\n<message>"
            std::vector<std::string> lines = StringHelper::Split(text, "\n");
            result.ok = !lines.empty() && lines[0] == "ok";
            if (result.ok) {
                result.fileName = lines.size() > 1 ? lines[1] : "";
                result.contentType = lines.size() > 2 ? lines[2] : "";
            } else {
                result.error = lines.size() > 1 ? lines[1] : "erro no download";
            }
        }
        env->DeleteLocalRef(jUrl);
        env->DeleteLocalRef(jPath);
    } else {
        result.error = "app sem suporte a download";
    }
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(activity);
    return result;
}

#elif defined(_WIN32)
std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string QueryHeader(HINTERNET request, DWORD which) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, which, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (size == 0) {
        return "";
    }
    std::wstring value(size / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(request, which, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        return "";
    }
    value.resize(size / sizeof(wchar_t));
    return Narrow(value);
}

HttpResult HttpDownload(const std::string& url, const fs::path& to) {
    HttpResult result;
    std::wstring wurl = Widen(url);
    URL_COMPONENTS parts = { sizeof(parts) };
    wchar_t host[256] = {}, path[4096] = {}, extra[4096] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 4096;
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) {
        result.error = "link inválido";
        return result;
    }
    std::wstring fullPath = std::wstring(path) + extra;
    HINTERNET session = WinHttpOpen(L"MajorasMaskDefinitive-Mods/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        session = WinHttpOpen(L"MajorasMaskDefinitive-Mods/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (session == nullptr) {
        result.error = "sem internet";
        return result;
    }
    HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", fullPath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    if (request != nullptr &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        uint64_t total = 0;
        std::string length = QueryHeader(request, WINHTTP_QUERY_CONTENT_LENGTH);
        if (!length.empty()) {
            total = std::strtoull(length.c_str(), nullptr, 10);
        }
        result.fileName = FileNameFromDisposition(QueryHeader(request, WINHTTP_QUERY_CONTENT_DISPOSITION));
        result.contentType = QueryHeader(request, WINHTTP_QUERY_CONTENT_TYPE);
        if (status == 200) {
            std::ofstream out(to, std::ios::binary);
            result.ok = out.good();
            std::vector<char> buffer(1 << 16);
            uint64_t done = 0;
            while (result.ok) {
                DWORD read = 0;
                if (!WinHttpReadData(request, buffer.data(), (DWORD)buffer.size(), &read)) {
                    result.ok = false;
                    result.error = "a conexão caiu";
                    break;
                }
                if (read == 0) {
                    break;
                }
                out.write(buffer.data(), read);
                done += read;
                Progress(done, total);
            }
            if (!out.good()) {
                result.ok = false;
                result.error = "não foi possível gravar o arquivo";
            }
        } else {
            result.error = "o Google Drive respondeu " + std::to_string(status);
        }
    } else {
        result.error = "sem conexão com o Google Drive";
    }
    if (request != nullptr) {
        WinHttpCloseHandle(request);
    }
    if (connect != nullptr) {
        WinHttpCloseHandle(connect);
    }
    WinHttpCloseHandle(session);
    return result;
}

#else
// Linux and macOS: the system's curl.
std::string ShellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }
    return out + "'";
}

HttpResult HttpDownload(const std::string& url, const fs::path& to) {
    HttpResult result;
    fs::path headers = to;
    headers += ".headers";
    std::string command = "curl -L --fail -sS -o " + ShellQuote(to.string()) + " -D " + ShellQuote(headers.string()) +
                          " " + ShellQuote(url) + " 2>&1";
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        result.error = "curl não encontrado";
        return result;
    }
    std::string output;
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    int code = pclose(pipe);
    std::ifstream in(headers);
    std::string line;
    while (std::getline(in, line)) {
        // Each redirect starts a new block; the last one is the file's.
        if (StringHelper::StartsWith(line, "HTTP/")) {
            result.fileName.clear();
            result.contentType.clear();
        } else if (line.size() > 20 && StringHelper::IEquals(line.substr(0, 20), "content-disposition:")) {
            result.fileName = FileNameFromDisposition(line);
        } else if (line.size() > 13 && StringHelper::IEquals(line.substr(0, 13), "content-type:")) {
            result.contentType = Trim(line.substr(13));
        }
    }
    in.close();
    std::error_code ec;
    fs::remove(headers, ec);
    result.ok = code == 0;
    if (!result.ok) {
        result.error = output.empty() ? "erro no download" : Trim(output);
    }
    return result;
}
#endif

// ----- Links.

// The id in a Drive link, and whether it is a folder.
bool ParseLink(const std::string& rawLink, std::string& id, bool& folder) {
    std::string link = Trim(rawLink);
    std::smatch m;
    static const std::regex folderRe(R"((?:/folders/|embeddedfolderview\?(?:.*&)?id=)([A-Za-z0-9_-]{10,}))");
    static const std::regex fileRe(R"((?:/file/d/|/document/d/|[?&]id=)([A-Za-z0-9_-]{10,}))");
    if (std::regex_search(link, m, folderRe)) {
        id = m[1].str();
        folder = true;
        return true;
    }
    if (link.find("google.com") != std::string::npos && std::regex_search(link, m, fileRe)) {
        id = m[1].str();
        folder = false;
        return true;
    }
    // A bare id.
    if (link.size() >= 20 && std::all_of(link.begin(), link.end(), IsIdChar)) {
        id = link;
        folder = false;
        return true;
    }
    return false;
}

fs::path WorkFolder() {
    fs::path folder = Ship::Context::GetPathRelativeToAppDirectory("mods-baixando", appShortName);
    std::error_code ec;
    fs::create_directories(folder, ec);
    return folder;
}

void Finish(DownloadState state, const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sStatus.state = state;
        sStatus.message = message;
    }
    SetNotice(message);
    sBusy = false;
}

// ----- Installing a download. Same limits as Android's SafeZip.java (mods picked on the phone).

// A zip that only wraps mods holds a few .o2r and a readme.
constexpr zip_int64_t kMaxZipEntries = 10000;
constexpr size_t kMaxZipArchives = 64;
// HD texture packs can be large, but not this large.
constexpr uint64_t kMaxTotalBytes = 8ull << 30;
constexpr uint64_t kMaxEntryBytes = 4ull << 30;
// A deflated entry may unpack to at most this many times its packed size (zip bombs do far more). Entries under
// 1 MiB are not held to it.
constexpr uint64_t kMaxRatio = 200;
constexpr uint64_t kRatioFreeBytes = 1ull << 20;
// Kept free on the storage after an install.
constexpr uint64_t kFreeSpaceMargin = 64ull << 20;

bool IsGameArchive(const std::string& name) {
    std::string ext = fs::path(name).extension().string();
    return StringHelper::IEquals(ext, ".o2r") || StringHelper::IEquals(ext, ".otr");
}

bool IsSideFile(const std::string& name) {
    std::string ext = fs::path(name).extension().string();
    return StringHelper::IEquals(ext, ".txt") || StringHelper::IEquals(ext, ".md") ||
           StringHelper::IEquals(ext, ".pdf") || StringHelper::IEquals(ext, ".png") ||
           StringHelper::IEquals(ext, ".jpg");
}

// Absolute names, "..", backslashes, drive letters and control characters could write outside the mods folder.
bool IsSafeEntryName(const std::string& name) {
    if (name.empty() || name[0] == '/' || name.find('\\') != std::string::npos || (name.size() > 1 && name[1] == ':')) {
        return false;
    }
    for (unsigned char c : name) {
        if (c < 32) {
            return false;
        }
    }
    size_t start = 0;
    while (start <= name.size()) {
        size_t end = name.find('/', start);
        if (end == std::string::npos) {
            end = name.size();
        }
        if (name.compare(start, end - start, "..") == 0 && end - start == 2) {
            return false;
        }
        start = end + 1;
    }
    return true;
}

bool HasFreeSpace(const fs::path& folder, uint64_t bytes) {
    std::error_code ec;
    fs::space_info space = fs::space(folder, ec);
    if (ec) {
        return true; // unknown: the write itself reports a full storage
    }
    return space.available >= bytes + kFreeSpaceMargin;
}

// Empty when the file looks like a working game archive: a .o2r (or a mod .zip) must open as a zip with at least
// one entry, a .otr must start with the MPQ signature.
std::string ArchiveProblem(const fs::path& file) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec) || fs::file_size(file, ec) == 0) {
        return "arquivo vazio";
    }
    if (StringHelper::IEquals(file.extension().string(), ".otr")) {
        std::ifstream in(file, std::ios::binary);
        char magic[3] = {};
        in.read(magic, 3);
        return in.gcount() == 3 && magic[0] == 'M' && magic[1] == 'P' && magic[2] == 'Q' ? "" : "não é um .otr válido";
    }
    int zipError = 0;
    zip_t* zip = zip_open(file.string().c_str(), ZIP_RDONLY | ZIP_CHECKCONS, &zipError);
    if (zip == nullptr) {
        return "arquivo danificado";
    }
    zip_int64_t count = zip_get_num_entries(zip, 0);
    zip_discard(zip);
    return count > 0 ? "" : "arquivo vazio";
}

// Takes the .o2r / .otr out of a zip that only wraps mods, into `staging`, held to the limits above. Returns false
// with `error` set when the zip is damaged or over a limit; `ready` then holds nothing usable.
bool ExtractWrappedArchives(const fs::path& zipPath, const fs::path& staging, std::vector<fs::path>& ready,
                            bool& wrapsArchives, std::string& error) {
    wrapsArchives = false;
    int zipError = 0;
    zip_t* zip = zip_open(zipPath.string().c_str(), ZIP_RDONLY, &zipError);
    if (zip == nullptr) {
        error = "o zip está danificado";
        return false;
    }
    struct ArchiveEntry {
        zip_uint64_t index;
        std::string name;
        zip_uint64_t size, compressed;
        bool deflated;
    };
    std::vector<ArchiveEntry> archives;
    bool hasOther = false;
    uint64_t declared = 0;
    zip_int64_t count = zip_get_num_entries(zip, 0);
    for (zip_int64_t i = 0; i < count; i++) {
        const char* rawName = zip_get_name(zip, i, ZIP_FL_ENC_GUESS);
        if (rawName == nullptr) {
            hasOther = true; // unreadable entry: not a plain wrapper
            continue;
        }
        std::string entry = rawName;
        if (entry.empty() || entry.back() == '/') {
            continue;
        }
        std::string base = fs::path(entry).filename().string();
        if (IsGameArchive(base)) {
            zip_stat_t st;
            zip_stat_init(&st);
            if (zip_stat_index(zip, i, 0, &st) != 0) {
                error = "o zip está danificado";
                zip_discard(zip);
                return false;
            }
            ArchiveEntry a;
            a.index = (zip_uint64_t)i;
            a.name = entry;
            a.size = (st.valid & ZIP_STAT_SIZE) ? st.size : kMaxEntryBytes;
            a.compressed = (st.valid & ZIP_STAT_COMP_SIZE) ? st.comp_size : 0;
            a.deflated = (st.valid & ZIP_STAT_COMP_METHOD) && st.comp_method != ZIP_CM_STORE;
            declared += a.size;
            archives.push_back(a);
        } else if (!IsSideFile(base)) {
            hasOther = true;
        }
    }
    if (archives.empty() || hasOther) {
        zip_discard(zip);
        return true; // a mod archive itself: installed as it is
    }
    wrapsArchives = true;
    if (count > kMaxZipEntries) {
        error = "o zip tem arquivos demais";
    } else if (archives.size() > kMaxZipArchives) {
        error = "o zip tem mods demais";
    } else if (!HasFreeSpace(staging, declared)) {
        error = "não há espaço livre suficiente para descompactar";
    }
    uint64_t total = 0;
    std::vector<char> buffer(1 << 16);
    for (const ArchiveEntry& a : archives) {
        if (!error.empty()) {
            break;
        }
        if (!IsSafeEntryName(a.name)) {
            error = "o zip tem um caminho inválido";
            break;
        }
        std::string inner = SafeName(fs::path(a.name).filename().string());
        while (!inner.empty() && inner[0] == '.') {
            inner.erase(0, 1);
        }
        if (inner.empty()) {
            error = "o zip tem um mod sem nome";
            break;
        }
        for (const fs::path& earlier : ready) {
            if (StringHelper::IEquals(earlier.filename().string(), inner)) {
                error = "o zip tem dois mods com o nome " + inner;
            }
        }
        if (!error.empty()) {
            break;
        }
        uint64_t limit = std::min(kMaxEntryBytes, kMaxTotalBytes - total);
        if (a.deflated && a.compressed > 0) {
            limit = std::min(limit, std::max(kRatioFreeBytes, a.compressed * kMaxRatio));
        }
        zip_file_t* file = zip_fopen_index(zip, a.index, 0);
        if (file == nullptr) {
            error = "não foi possível ler " + inner + " no zip";
            break;
        }
        fs::path outPath = staging / inner;
        std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
        uint64_t written = 0;
        zip_int64_t read;
        while ((read = zip_fread(file, buffer.data(), buffer.size())) > 0) {
            written += (uint64_t)read;
            if (written > limit) {
                error = "o zip descompacta para mais que o limite (" + inner + ")";
                break;
            }
            out.write(buffer.data(), read);
        }
        if (read < 0 && error.empty()) {
            error = "o zip está danificado (" + inner + ")";
        }
        zip_fclose(file);
        out.close();
        if (error.empty() && !out) {
            error = "não foi possível gravar " + inner;
        }
        total += written;
        if (error.empty()) {
            ready.push_back(outPath);
        }
    }
    zip_discard(zip);
    if (!error.empty()) {
        ready.clear();
        return false;
    }
    return true;
}

// Moves checked files into the mods folder. A mod of the same name is moved aside first and comes back if any
// move fails, so the mods already installed stay as they were.
bool CommitIntoMods(const std::vector<fs::path>& ready, const fs::path& folder, const fs::path& staging,
                    std::string& error) {
    std::error_code ec;
    const fs::path backup = staging / "anterior";
    fs::create_directories(backup, ec);
    std::vector<std::pair<fs::path, fs::path>> backedUp; // mods folder path, backup path
    std::vector<fs::path> movedIn;
    bool ok = true;
    for (const fs::path& file : ready) {
        fs::path target = folder / file.filename();
        if (fs::exists(target, ec)) {
            fs::path kept = backup / file.filename();
            fs::rename(target, kept, ec);
            if (ec) {
                ok = false;
                break;
            }
            backedUp.emplace_back(target, kept);
        }
        fs::rename(file, target, ec);
        if (ec) {
            ok = false;
            break;
        }
        movedIn.push_back(target);
    }
    if (!ok) {
        for (const fs::path& target : movedIn) {
            fs::remove(target, ec);
        }
        for (const auto& [target, kept] : backedUp) {
            fs::rename(kept, target, ec);
        }
        error = "não foi possível gravar na pasta de mods";
        return false;
    }
    return true;
}

// Leftovers of installs stopped half way (the game closed during the moves): a mod moved aside whose place is
// empty goes back; the rest is deleted.
void RecoverInterruptedInstalls() {
    std::error_code ec;
    const fs::path folder = GetModsFolder(appShortName);
    for (const auto& work : fs::directory_iterator(WorkFolder(), ec)) {
        std::error_code ec2;
        if (!work.is_directory(ec2) || work.path().filename().string().rfind("instalando-", 0) != 0) {
            continue;
        }
        for (const auto& kept : fs::directory_iterator(work.path() / "anterior", ec2)) {
            fs::path target = folder / kept.path().filename();
            if (!fs::exists(target, ec2)) {
                fs::rename(kept.path(), target, ec2);
            }
        }
        fs::remove_all(work.path(), ec2);
    }
}

// Puts a downloaded file into the mods folder. A .zip that only wraps .o2r / .otr files (how mods are usually
// shared) has them taken out; any other .zip is a mod archive itself. Everything is made and checked in a work
// folder (limits above, each archive opened once) before it goes into mods/, so a damaged or malicious download
// never replaces a working mod. Returns the installed file names.
std::vector<std::string> Install(const fs::path& downloaded, std::string name, std::string& error) {
    std::vector<std::string> installed;
    const fs::path folder = GetModsFolder(appShortName);
    std::error_code ec;
    fs::create_directories(folder, ec);

    bool isZip = false;
    {
        std::ifstream in(downloaded, std::ios::binary);
        char magic[2] = {};
        in.read(magic, 2);
        isZip = in.gcount() == 2 && magic[0] == 'P' && magic[1] == 'K';
    }
    if (!HasArchiveExtension(name)) {
        if (!isZip) {
            error = "não é um mod (.o2r, .otr ou .zip)";
            return installed;
        }
        name += ".o2r";
    }
    while (!name.empty() && name[0] == '.') {
        name.erase(0, 1);
    }
    if (fs::file_size(downloaded, ec) > kMaxTotalBytes) {
        error = "o arquivo é grande demais";
        return installed;
    }

    static std::atomic<uint32_t> sInstallCount = 0;
    const fs::path staging = WorkFolder() / ("instalando-" + std::to_string((uint64_t)time(nullptr)) + "-" +
                                             std::to_string(sInstallCount++));
    fs::create_directories(staging, ec);
    if (ec) {
        error = "não foi possível criar a pasta de trabalho";
        return installed;
    }

    std::vector<fs::path> ready;
    if (isZip && StringHelper::IEquals(fs::path(name).extension().string(), ".zip")) {
        bool wraps = false;
        if (!ExtractWrappedArchives(downloaded, staging, ready, wraps, error)) {
            fs::remove_all(staging, ec);
            return installed;
        }
    }
    if (ready.empty()) {
        fs::path staged = staging / name;
        fs::rename(downloaded, staged, ec);
        if (ec) {
            ec.clear();
            fs::copy_file(downloaded, staged, fs::copy_options::overwrite_existing, ec);
        }
        if (ec) {
            error = "não foi possível gravar a cópia de trabalho";
            fs::remove_all(staging, ec);
            return installed;
        }
        ready.push_back(staged);
    }
    for (const fs::path& file : ready) {
        std::string problem = ArchiveProblem(file);
        if (!problem.empty()) {
            error = file.filename().string() + ": " + problem;
            fs::remove_all(staging, ec);
            return installed;
        }
    }
    if (!CommitIntoMods(ready, folder, staging, error)) {
        fs::remove_all(staging, ec);
        return installed;
    }
    for (const fs::path& file : ready) {
        installed.push_back(file.filename().string());
    }
    fs::remove_all(staging, ec);
    return installed;
}

void DownloadFileThread(DriveEntry entry) {
    RecoverInterruptedInstalls();
    const fs::path part = WorkFolder() / (entry.id + ".part");
    HttpResult result = HttpDownload(
        "https://drive.usercontent.google.com/download?id=" + entry.id + "&export=download&confirm=t", part);
    std::error_code ec;
    if (!result.ok) {
        fs::remove(part, ec);
        Finish(DownloadState::Failed, "Falha ao baixar: " + result.error + ".");
        return;
    }
    if (result.contentType.find("text/html") != std::string::npos) {
        fs::remove(part, ec);
        Finish(DownloadState::Failed, "O Google Drive não liberou o arquivo. No Drive, compartilhe como \"Qualquer "
                                      "pessoa com o link\".");
        return;
    }
    std::string name = SafeName(!result.fileName.empty() ? result.fileName : entry.name);
    if (name.empty()) {
        name = entry.id + ".o2r";
    }
    std::string error;
    std::vector<std::string> installed = Install(part, name, error);
    fs::remove(part, ec);
    if (installed.empty()) {
        Finish(DownloadState::Failed, "Não foi possível instalar " + name + ": " + error + ".");
        return;
    }
    std::string list;
    for (size_t i = 0; i < installed.size(); i++) {
        list += (i ? ", " : "") + installed[i];
    }
    if (LiveApplyEnabled()) {
        RequestApply();
        Finish(DownloadState::Done, "Mod baixado e ligado: " + list + ".");
    } else {
        Finish(DownloadState::Done, "Mod baixado: " + list + ". Vale quando o jogo abrir de novo.");
    }
}

// Drive's embeddable folder view is a plain page that lists the folder's files without signing in.
void ListFolderThread(std::string id) {
    const fs::path page = WorkFolder() / (id + ".html");
    HttpResult result = HttpDownload("https://drive.google.com/embeddedfolderview?id=" + id + "#list", page);
    std::error_code ec;
    if (!result.ok) {
        fs::remove(page, ec);
        Finish(DownloadState::Failed, "Não foi possível abrir a pasta: " + result.error + ".");
        return;
    }
    std::ifstream in(page, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    in.close();
    fs::remove(page, ec);
    const std::string html = ss.str();

    std::vector<DriveEntry> entries;
    // Each file is a block like: <div class="flip-entry" id="entry-ID"> ... <a href="..."> ...
    // <div class="flip-entry-title">NAME</div>. Plain searches (std::regex runs out of stack on a whole page).
    size_t pos = 0;
    const std::string idMark = "id=\"entry-";
    while ((pos = html.find(idMark, pos)) != std::string::npos) {
        pos += idMark.size();
        size_t idEnd = html.find('"', pos);
        size_t next = html.find(idMark, pos);
        size_t href = html.find("href=\"", pos);
        size_t title = html.find("flip-entry-title", pos);
        if (idEnd == std::string::npos || title == std::string::npos || (next != std::string::npos && title > next)) {
            continue;
        }
        size_t nameStart = html.find('>', title);
        size_t nameEnd = nameStart == std::string::npos ? std::string::npos : html.find('<', nameStart);
        if (nameEnd == std::string::npos) {
            break;
        }
        std::string link;
        if (href != std::string::npos && href < title) {
            size_t linkEnd = html.find('"', href + 6);
            link = html.substr(href + 6, linkEnd == std::string::npos ? 0 : linkEnd - href - 6);
        }
        if (link.find("/folders/") != std::string::npos) {
            continue; // a folder inside the folder
        }
        DriveEntry entry{ html.substr(pos, idEnd - pos),
                          Trim(DecodeEntities(html.substr(nameStart + 1, nameEnd - nameStart - 1))) };
        if (!entry.id.empty() && std::all_of(entry.id.begin(), entry.id.end(), IsIdChar) &&
            HasArchiveExtension(entry.name)) {
            entries.push_back(entry);
        }
    }
    std::sort(entries.begin(), entries.end(), [](const DriveEntry& a, const DriveEntry& b) { return a.name < b.name; });
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sFolder = entries;
        sFolderId = id;
    }
    if (entries.empty()) {
        Finish(DownloadState::Failed, "Nenhum mod (.o2r, .otr, .zip) na pasta, ou ela não está compartilhada como "
                                      "\"Qualquer pessoa com o link\".");
    } else {
        Finish(DownloadState::Done, std::to_string(entries.size()) + " mod(s) na pasta do Google Drive.");
    }
}

bool Start(DownloadState state, const std::string& name) {
    if (sBusy.exchange(true)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(sMutex);
    sStatus = DownloadStatus();
    sStatus.state = state;
    sStatus.name = name;
    return true;
}

} // namespace

DownloadStatus GetDownloadStatus() {
    std::lock_guard<std::mutex> lock(sMutex);
#ifdef __ANDROID__
    if (sStatus.state == DownloadState::Downloading) {
        sStatus.done = sAndroidDone;
        sStatus.total = sAndroidTotal;
    }
#endif
    return sStatus;
}

void TakePendingLink() {
    std::string link;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        if (!sHasPendingLink) {
            return;
        }
        sHasPendingLink = false;
        link = sPendingLink;
    }
    if (!OpenDriveLink(link)) {
        SetNotice("Esse não é um link do Google Drive.");
    }
}

bool DownloadBusy() {
    return sBusy;
}

void DownloadDriveFile(const DriveEntry& entry) {
    if (!Start(DownloadState::Downloading, entry.name.empty() ? entry.id : entry.name)) {
        return;
    }
#ifdef __ANDROID__
    sAndroidDone = 0;
    sAndroidTotal = 0;
#endif
    std::thread(DownloadFileThread, entry).detach();
}

bool OpenDriveLink(const std::string& link) {
    std::string id;
    bool folder = false;
    if (!ParseLink(link, id, folder)) {
        return false;
    }
    if (folder) {
        CVarSetString(CVAR_MODS_DRIVE_FOLDER, id.c_str());
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        if (Start(DownloadState::Listing, "")) {
            std::thread(ListFolderThread, id).detach();
        }
    } else {
        DownloadDriveFile({ id, "" });
    }
    return true;
}

std::vector<DriveEntry> GetDriveFolder() {
    std::lock_guard<std::mutex> lock(sMutex);
    if (sFolderId != CVarGetString(CVAR_MODS_DRIVE_FOLDER, "")) {
        return {};
    }
    return sFolder;
}

bool HasDriveFolder() {
    return CVarGetString(CVAR_MODS_DRIVE_FOLDER, "")[0] != '\0';
}

void RefreshDriveFolder() {
    std::string id = CVarGetString(CVAR_MODS_DRIVE_FOLDER, "");
    if (!id.empty() && Start(DownloadState::Listing, "")) {
        std::thread(ListFolderThread, id).detach();
    }
}

void ForgetDriveFolder() {
    CVarClear(CVAR_MODS_DRIVE_FOLDER);
    Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    std::lock_guard<std::mutex> lock(sMutex);
    sFolder.clear();
    sFolderId.clear();
}

bool IsInstalled(const std::string& appShortName, const std::string& fileName) {
    std::string stem = fs::path(fileName).stem().string();
    for (const ModFile& mod : List(appShortName)) {
        // A wrapping .zip installs the .o2r inside it, usually with the same name.
        if (StringHelper::IEquals(mod.name, fileName) ||
            StringHelper::IEquals(fs::path(mod.name).stem().string(), stem)) {
            return true;
        }
    }
    return false;
}

#ifdef __ANDROID__
bool AskDriveLink() {
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (env == nullptr || activity == nullptr) {
        return false;
    }
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID method = env->GetMethodID(activityClass, "askDriveLink", "()V");
    if (method != nullptr) {
        env->CallVoidMethod(activity, method);
    }
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(activity);
    return true;
}

extern "C" JNIEXPORT void JNICALL Java_com_dishii_mm_MainActivity_nativeDriveLinkEntered(JNIEnv* env, jobject,
                                                                                         jstring link) {
    const char* chars = env->GetStringUTFChars(link, nullptr);
    std::lock_guard<std::mutex> lock(sMutex);
    sPendingLink = chars;
    sHasPendingLink = true;
    env->ReleaseStringUTFChars(link, chars);
}

extern "C" JNIEXPORT void JNICALL Java_com_dishii_mm_MainActivity_nativeDownloadProgress(JNIEnv*, jobject, jlong done,
                                                                                         jlong total) {
    sAndroidDone = (uint64_t)done;
    sAndroidTotal = total > 0 ? (uint64_t)total : 0;
}
#else
bool AskDriveLink() {
    const char* clip = ImGui::GetClipboardText();
    return clip != nullptr && OpenDriveLink(clip);
}
#endif

} // namespace ModInstaller
