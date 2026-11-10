#ifdef _WIN32
#include <Windows.h>
#include <winuser.h>
#include <shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")
#endif
#include "Extract.h"
#include "SetupScreen.h"
#include "2s2h/Android/StartupLog.h"
#include "portable-file-dialogs.h"
#include <ship/utils/binarytools/BitConverter.h>
#include "build.h"

#ifdef unix
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _MSC_VER
#define BSWAP32 _byteswap_ulong
#define BSWAP16 _byteswap_ushort
#elif __has_include(<byteswap.h>)
#include <byteswap.h>
#define BSWAP32 bswap_32
#define BSWAP16 bswap_16
#else
#define BSWAP16(value) ((((value) & 0xff) << 8) | ((value) >> 8))

#define BSWAP32(value) \
    (((uint32_t)BSWAP16((uint16_t)((value) & 0xffff)) << 16) | (uint32_t)BSWAP16((uint16_t)((value) >> 16)))
#endif

#if defined(_MSC_VER)
#define UNREACHABLE __assume(0)
#elif __llvm__
#define UNREACHABLE __builtin_assume(0)
#else
#define UNREACHABLE __builtin_unreachable();
#endif

#include <stdlib.h>

#include <SDL2/SDL_messagebox.h>

#ifdef __ANDROID__
#include <SDL2/SDL.h>
#include <jni.h>
#endif

#include <array>
#include <atomic>
#include <thread>
#include <fstream>
#include <filesystem>
#include <unordered_map>
#include <random>
#include <string>

extern "C" uint32_t CRC32C(unsigned char* data, size_t dataSize);

static constexpr uint32_t MM_US_10 = 0x5354631C;
static constexpr uint32_t MM_US_GC = 0xB443EB08;

static const std::unordered_map<uint32_t, const char*> verMap = {
    { MM_US_10, "US 1.0" },
    { MM_US_GC, "US GC" },
};

// TODO only check the first 54MB of the rom.
static constexpr std::array<const uint32_t, 10> goodCrcs = {
    0x96F49400, // MM US 1.0 32MB
    0xBB434787, // MM GC
};

enum class ButtonId : int {
    YES,
    NO,
    FIND,
};

#ifdef __ANDROID__
// The ROM is picked with Android's document picker in MainActivity.openFilePicker, which copies it into the
// app folder and reports the copy's path back through nativeHandleSelectedFile.
static const char* sJavaRomPath = nullptr;
static volatile bool sFileDialogOpen = false;

static void OpenAndroidFilePicker() {
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    sFileDialogOpen = true;
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID openFilePicker = env->GetMethodID(activityClass, "openFilePicker", "()V");
    env->CallVoidMethod(activity, openFilePicker);
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(activity);
}

extern "C" JNIEXPORT void JNICALL Java_com_dishii_mm_MainActivity_nativeHandleSelectedFile(JNIEnv* env, jobject obj,
                                                                                           jstring filePath) {
    const char* filePathStr = env->GetStringUTFChars(filePath, 0);
    sJavaRomPath = strdup(filePathStr);
    env->ReleaseStringUTFChars(filePath, filePathStr);
    sFileDialogOpen = false;
}
#endif

void Extractor::ShowErrorBox(const char* title, const char* text) {
    // The preparation screen, when it can open (SetupScreen.cpp; Java on Android); the system's box otherwise.
    if (SetupScreen::Ensure()) {
        SetupScreen::Ask(title, text, { "OK" }, 0, 0);
        return;
    }
#ifdef _WIN32
    MessageBoxA(nullptr, text, title, MB_OK | MB_ICONERROR);
#else
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, text, nullptr);
#endif
}

void Extractor::ShowSizeErrorBox() const {
    std::unique_ptr<char[]> boxBuffer = std::make_unique<char[]>(mCurrentRomPath.size() + 200);
    snprintf(boxBuffer.get(), mCurrentRomPath.size() + 200,
             "O arquivo de ROM %s não tem um tamanho válido. Tem %zu MB, o esperado é 32, 54 ou 64 MB.",
             mCurrentRomPath.c_str(), mCurRomSize / MB_BASE);
    ShowErrorBox("Tamanho de ROM inválido", boxBuffer.get());
}

void Extractor::ShowCrcErrorBox() const {
    ShowErrorBox("CRC da ROM inválido",
                 "O CRC da ROM não está na lista de ROMs compatíveis. Escolha outra.\n\n"
                 "Veja em https://2ship.equipment/ como validar a sua ROM e quais versões são compatíveis.");
}

void Extractor::ShowCompressedErrorBox() const {
    ShowErrorBox("Arquivo compactado",
                 "O arquivo escolhido parece estar compactado (.zip, .7z...). Extraia a ROM antes de usar.");
}

int Extractor::ShowRomPickBox(uint32_t verCrc) const {
    std::unique_ptr<char[]> boxBuffer = std::make_unique<char[]>(mCurrentRomPath.size() + 200);
    SDL_MessageBoxData boxData = { 0 };
    SDL_MessageBoxButtonData buttons[3] = { { 0 } };
    int ret;

    buttons[0].buttonid = 0;
    buttons[0].text = "Sim";
    buttons[0].flags = SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
    buttons[1].buttonid = 1;
    buttons[1].text = "Não";
    buttons[1].flags = SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
    buttons[2].buttonid = 2;
    buttons[2].text = "Procurar ROM";
    boxData.numbuttons = 3;
    boxData.flags = SDL_MESSAGEBOX_INFORMATION;
    boxData.message = boxBuffer.get();
    boxData.title = "ROM encontrada";
    boxData.window = nullptr;

    boxData.buttons = buttons;
    snprintf(boxBuffer.get(), mCurrentRomPath.size() + 200,
             "ROM encontrada: %s, CRC32 do cabeçalho: %8X. Parece ser: %s. Usar esta ROM?", mCurrentRomPath.c_str(),
             verCrc, verMap.at(verCrc));

    if (SetupScreen::Ensure()) {
        std::string file = std::filesystem::path(mCurrentRomPath).filename().string();
        std::string text = "Encontramos esta ROM:\n" + file + "\n\nVersão: " + verMap.at(verCrc) + ".\n\nUsar esta ROM?";
        return SetupScreen::Ask("ROM encontrada", text, { "Usar esta", "Não", "Procurar outra" }, 0, 1);
    }
    SDL_ShowMessageBox(&boxData, &ret);
    return ret;
}

int Extractor::ShowYesNoBox(const char* title, const char* box) {
    int ret;
    if (SetupScreen::Ensure()) {
        return SetupScreen::Ask(title, box, { "Sim", "Não" }, 0, 1) == 0 ? IDYES : IDNO;
    }
#ifdef _WIN32
    ret = MessageBoxA(nullptr, box, title, MB_YESNO | MB_ICONQUESTION);
#else
    SDL_MessageBoxData boxData = { 0 };
    SDL_MessageBoxButtonData buttons[2] = { { 0 } };

    buttons[0].buttonid = IDYES;
    buttons[0].text = "Sim";
    buttons[0].flags = SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
    buttons[1].buttonid = IDNO;
    buttons[1].text = "Não";
    buttons[1].flags = SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
    boxData.numbuttons = 2;
    boxData.flags = SDL_MESSAGEBOX_INFORMATION;
    boxData.message = box;
    boxData.title = title;
    boxData.buttons = buttons;
    SDL_ShowMessageBox(&boxData, &ret);
#endif
    return ret;
}

void Extractor::SetRomInfo(const std::string& path) {
    mCurrentRomPath = path;
    mCurRomSize = GetCurRomSize();
}

void Extractor::FilterRoms(std::vector<std::string>& roms, RomSearchMode searchMode) {
    std::ifstream inFile;
    std::vector<std::string>::iterator it = roms.begin();

    while (it != roms.end()) {
        std::string rom = *it;
        SetRomInfo(rom);

        // Skip. We will handle rom size errors later on after filtering
        if (!ValidateRomSize()) {
            it++;
            continue;
        }

        inFile.open(rom, std::ios::in | std::ios::binary);
        inFile.read((char*)mRomData.get(), mCurRomSize);
        inFile.clear();
        inFile.close();

        BitConverter::RomToBigEndian(mRomData.get(), mCurRomSize);

        // Rom doesn't claim to be valid
        // Game type doesn't match search mode
        if (!verMap.contains(GetRomVerCrc()) || (searchMode == RomSearchMode::Vanilla && IsMasterQuest()) ||
            (searchMode == RomSearchMode::MQ && !IsMasterQuest())) {
            it = roms.erase(it);
            continue;
        }

        it++;
    }
}

void Extractor::GetRoms(std::vector<std::string>& roms) {
#ifdef _WIN32
    WIN32_FIND_DATAA ffd;
    HANDLE h = FindFirstFileA(".\\*", &ffd);

    do {
        if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            char* ext = PathFindExtensionA(ffd.cFileName);

            // Check for any standard N64 rom file extensions.
            if ((strcmp(ext, ".z64") == 0) || (strcmp(ext, ".n64") == 0) || (strcmp(ext, ".v64") == 0))
                roms.push_back(ffd.cFileName);
        }
    } while (FindNextFileA(h, &ffd) != 0);
    // if (h != nullptr) {
    //    CloseHandle(h);
    //}
#elif unix
    // Open the directory of the app.
    DIR* d = opendir(mSearchPath.c_str());
    struct dirent* dir;

    if (d != NULL) {
        // Go through each file in the directory
        while ((dir = readdir(d)) != NULL) {
            struct stat path;

            auto fullPath = std::filesystem::path(mSearchPath) / dir->d_name;
            auto fullPathString = fullPath.string();
            const char* fullPathCStr = fullPathString.c_str();

            // Check if current entry is not folder
            stat(fullPathCStr, &path);
            if (S_ISREG(path.st_mode)) {

                // Get the position of the extension character.
                char* ext = strrchr(dir->d_name, '.');
                if (ext != NULL && (strcmp(ext, ".z64") == 0 || strcmp(ext, ".n64") == 0 || strcmp(ext, ".v64") == 0)) {
                    roms.push_back(fullPathCStr);
                }
            }
        }
    }
    closedir(d);
#else
    for (const auto& file : std::filesystem::directory_iterator(mSearchPath)) {
        if (file.is_directory())
            continue;
        if ((file.path().extension() == ".n64") || (file.path().extension() == ".z64") ||
            (file.path().extension() == ".v64")) {
            roms.push_back((file.path()));
        }
    }
#endif
}

bool Extractor::GetRomPathFromBox() {
#ifdef _WIN32
    OPENFILENAMEA box = { 0 };
    char nameBuffer[512];
    nameBuffer[0] = 0;

    box.lStructSize = sizeof(box);
    box.lpstrFile = nameBuffer;
    box.nMaxFile = sizeof(nameBuffer) / sizeof(nameBuffer[0]);
    box.lpstrTitle = "Open Rom";
    box.Flags =
        OFN_NOCHANGEDIR | OFN_ENABLESIZING | OFN_FILEMUSTEXIST | OFN_LONGNAMES | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    box.lpstrFilter = "N64 Roms\0*.z64;*.v64;*.n64\0\0";
    if (!GetOpenFileNameA(&box)) {
        DWORD err = CommDlgExtendedError();
        // GetOpenFileName will return 0 but no error is set if the user just closes the box.
        if (err != 0) {
            const char* errStr = nullptr;
            switch (err) {
                case FNERR_BUFFERTOOSMALL:
                    errStr = "Path buffer too small. Move file closer to root of your drive";
                    break;
                case FNERR_INVALIDFILENAME:
                    errStr = "File name for rom provided is invalid.";
                    break;
                case FNERR_SUBCLASSFAILURE:
                    errStr = "Failed to open a filebox because there is not enough RAM to do so.";
                    break;
            }
            MessageBoxA(nullptr, "Box Error", errStr, MB_OK | MB_ICONERROR);
            return false;
        }
    }
    // The box was closed without something being selected.
    if (nameBuffer[0] == 0) {
        return false;
    }
    mCurrentRomPath = nameBuffer;
#elif defined(__ANDROID__)
    OpenAndroidFilePicker();
    while (sFileDialogOpen) {
        SDL_Delay(250);
    }
    std::string picked = sJavaRomPath != nullptr ? sJavaRomPath : "";
    free((void*)sJavaRomPath);
    sJavaRomPath = nullptr;
    if (picked.empty()) {
        return false;
    }
    mCurrentRomPath = picked;
    StartupLog_Step("ROM escolhida: %s", picked.c_str());
#else
    auto selection = pfd::open_file("Select a file", mSearchPath, { "N64 Roms", "*.z64 *.n64 *.v64" }).result();

    if (selection.empty()) {
        return false;
    }

    mCurrentRomPath = selection[0];
#endif
    mCurRomSize = GetCurRomSize();
    return true;
}

uint32_t Extractor::GetRomVerCrc() const {
    return BSWAP32(((uint32_t*)mRomData.get())[4]);
}

size_t Extractor::GetCurRomSize() const {
    return std::filesystem::file_size(mCurrentRomPath);
}

bool Extractor::ValidateAndFixRom() {
    const uint32_t actualCrc = CRC32C(mRomData.get(), mCurRomSize);

    for (const uint32_t crc : goodCrcs) {
        if (actualCrc == crc) {
            return true;
        }
    }
    return false;
}

// The file box will only allow selecting an n64 rom but typing in the file name will allow selecting anything.
bool Extractor::ValidateNotCompressed() const {
    // ZIP file header
    if (mRomData[0] == 'P' && mRomData[1] == 'K' && mRomData[2] == 0x03 && mRomData[3] == 0x04) {
        return false;
    }
    // RAR file header. Only the first 4 bytes.
    if (mRomData[0] == 'R' && mRomData[1] == 'a' && mRomData[2] == 'r' && mRomData[3] == 0x21) {
        return false;
    }
    // 7z file header. 37 7A BC AF 27 1C
    if (mRomData[0] == '7' && mRomData[1] == 'z' && mRomData[2] == 0xBC && mRomData[3] == 0xAF && mRomData[4] == 0x27 &&
        mRomData[5] == 0x1C) {
        return false;
    }

    return true;
}

bool Extractor::ValidateRomSize() const {
    if (mCurRomSize != MB32 && mCurRomSize != MB54 && mCurRomSize != MB64) {
        return false;
    }
    return true;
}

bool Extractor::ValidateRom(bool skipCrcTextBox) {
    if (!ValidateNotCompressed()) {
        ShowCompressedErrorBox();
        return false;
    }
    if (!ValidateRomSize()) {
        ShowSizeErrorBox();
        return false;
    }
    if (!ValidateAndFixRom()) {
        if (!skipCrcTextBox) {
            ShowCrcErrorBox();
        }
        return false;
    }
    return true;
}

bool Extractor::ManuallySearchForRom() {
    std::ifstream inFile;

    if (!GetRomPathFromBox()) {
        ShowErrorBox("Nenhuma ROM escolhida", "Nenhuma ROM foi escolhida. Saindo.");
        return false;
    }

    inFile.open(mCurrentRomPath, std::ios::in | std::ios::binary);

    if (!inFile.is_open()) {
        return false; // TODO Handle error
    }

    inFile.read((char*)mRomData.get(), mCurRomSize);
    inFile.close();
    BitConverter::RomToBigEndian(mRomData.get(), mCurRomSize);

    if (!ValidateRom()) {
        return false;
    }

    return true;
}

bool Extractor::ManuallySearchForRomMatchingType(RomSearchMode searchMode) {
    if (!ManuallySearchForRom()) {
        return false;
    }

    char msgBuf[150];
    snprintf(msgBuf, 150, "A ROM escolhida não é do tipo esperado\nTipo esperado: %s.\n\nQuer procurar de novo?",
             searchMode == RomSearchMode::MQ ? "Master Quest" : "Vanilla");

    while ((searchMode == RomSearchMode::Vanilla && IsMasterQuest()) ||
           (searchMode == RomSearchMode::MQ && !IsMasterQuest())) {
        int ret = ShowYesNoBox("Tipo de jogo errado", msgBuf);
        switch (ret) {
            case IDYES:
                if (!ManuallySearchForRom()) {
                    return false;
                }
                continue;
            case IDNO:
                return false;
            default:
                UNREACHABLE;
                break;
        }
    }

    return true;
}

bool Extractor::Run(std::string searchPath, RomSearchMode searchMode) {
    std::vector<std::string> roms;
    std::ifstream inFile;

    mSearchPath = searchPath;

    GetRoms(roms);
    FilterRoms(roms, searchMode);

    if (roms.empty()) {
        int ret = ShowYesNoBox("Nenhuma ROM encontrada",
                               "Nenhuma ROM encontrada. Escolher o arquivo da ROM do Majora's Mask (N64)?");

        switch (ret) {
            case IDYES:
                if (!ManuallySearchForRomMatchingType(searchMode)) {
                    return false;
                }
                break;
            case IDNO:
                ShowErrorBox("Nenhuma ROM escolhida", "Nenhuma ROM foi escolhida. Saindo.");
                return false;
            default:
                UNREACHABLE;
                break;
        }
    }

    if (roms.size() > 1) {
        int ret = ShowYesNoBox("Várias ROMs encontradas", "Foram encontradas várias ROMs. Escolher uma manualmente?");
        if (ret == IDYES) {
            if (!ManuallySearchForRomMatchingType(searchMode)) {
                return false;
            }
            roms.clear();
            roms.push_back(mCurrentRomPath);
        }
    }

    for (const auto& rom : roms) {
        SetRomInfo(rom);

        if (!ValidateRomSize()) {
            ShowSizeErrorBox();
            continue;
        }

        inFile.open(rom, std::ios::in | std::ios::binary);
        inFile.read((char*)mRomData.get(), mCurRomSize);
        inFile.clear();
        inFile.close();
        BitConverter::RomToBigEndian(mRomData.get(), mCurRomSize);

        int option = ShowRomPickBox(GetRomVerCrc());

        if (option == (int)ButtonId::YES) {
            if (!ValidateRom(true)) {
                if (rom == roms.back()) {
                    ShowCrcErrorBox();
                } else {
                    ShowErrorBox(
                        "CRC da ROM inválido",
                        "O CRC da ROM não está na lista de ROMs compatíveis. Tentando a próxima...\n\n"
                        "Veja em https://2ship.equipment/ como validar a sua ROM e quais versões são compatíveis.");
                }
                continue;
            }
            break;
        } else if (option == (int)ButtonId::FIND) {
            if (!ManuallySearchForRomMatchingType(searchMode)) {
                return false;
            }
            break;
        } else if (option == (int)ButtonId::NO) {
            if (rom == roms.back()) {
                ShowErrorBox("Nenhuma ROM escolhida", "Nenhuma ROM foi escolhida. Saindo.");
                return false;
            }
            continue;
        }
        break;
    }
    return true;
}

bool Extractor::IsMasterQuest() const {
    return false;
}

const char* Extractor::GetZapdVerStr() const {
    switch (GetRomVerCrc()) {
        case MM_US_10:
            return "N64_US";
        case MM_US_GC:
            return "GC_US";
        default:
            // We should never be in a state where this path happens.
            UNREACHABLE;
            break;
    }
}

std::string Extractor::Mkdtemp() {
#ifdef __ANDROID__
    // There is no usable /tmp on Android; the app's own external files folder is writable.
    std::string temp_dir = SDL_AndroidGetExternalStoragePath();
#else
    std::string temp_dir = std::filesystem::temp_directory_path().string();
#endif

    // create 6 random alphanumeric characters
    static const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, sizeof(charset) - 1);

    char randchr[7];
    for (int i = 0; i < 6; i++) {
        randchr[i] = charset[dist(gen)];
    }
    randchr[6] = '\0';

    std::string tmppath = temp_dir + "/extractor-" + randchr;
    std::filesystem::create_directory(tmppath);
    return tmppath;
}

extern "C" int zapd_main(int argc, char** argv);
static void MessageboxWorker();

// ZAPD counts the files it has extracted (patches/zapdtr-android.patch, applied on every platform). On Android a
// thread passes the count to MainActivity.showExtractionProgress; on PC the preparation screen shows it.
extern "C" volatile int gZapdFilesDone;
extern "C" volatile int gZapdFilesTotal;

#ifdef __ANDROID__
static std::atomic<bool> sProgressRunning = false;
static std::thread sProgressThread;

static void CallProgress(JNIEnv* env, int done, int total) {
    jclass activityClass = env->FindClass("com/dishii/mm/MainActivity");
    if (activityClass == nullptr) {
        env->ExceptionClear();
        return;
    }
    jmethodID method = env->GetStaticMethodID(activityClass, "showExtractionProgress", "(II)V");
    if (method != nullptr) {
        env->CallStaticVoidMethod(activityClass, method, done, total);
    }
    env->ExceptionClear();
    env->DeleteLocalRef(activityClass);
}

static void StartAndroidProgress() {
    gZapdFilesDone = 0;
    gZapdFilesTotal = 0;
    sProgressRunning = true;
    // FindClass from a native thread only sees system classes, so the class is looked up here, on the SDL thread.
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    jclass local = env->FindClass("com/dishii/mm/MainActivity");
    jclass activityClass = local != nullptr ? (jclass)env->NewGlobalRef(local) : nullptr;
    env->ExceptionClear();
    if (local != nullptr) {
        env->DeleteLocalRef(local);
    }
    if (activityClass == nullptr) {
        return;
    }
    jmethodID method = env->GetStaticMethodID(activityClass, "showExtractionProgress", "(II)V");
    env->ExceptionClear();
    if (method == nullptr) {
        return;
    }
    env->CallStaticVoidMethod(activityClass, method, 0, 0);
    sProgressThread = std::thread([activityClass, method]() {
        JNIEnv* threadEnv = (JNIEnv*)SDL_AndroidGetJNIEnv(); // attaches this thread
        while (sProgressRunning) {
            threadEnv->CallStaticVoidMethod(activityClass, method, (jint)gZapdFilesDone, (jint)gZapdFilesTotal);
            threadEnv->ExceptionClear();
            SDL_Delay(250);
        }
    });
}

static void StopAndroidProgress() {
    sProgressRunning = false;
    if (sProgressThread.joinable()) {
        sProgressThread.join();
    }
    // -1 closes the bar.
    CallProgress((JNIEnv*)SDL_AndroidGetJNIEnv(), -1, -1);
}
#endif

bool Extractor::CallZapd(std::string installPath, std::string exportdir) {
    constexpr int argc = 22;
    char xmlPath[1024];
    char confPath[1024];
    char portVersion[18]; // 5 digits for int16_max (x3) + separators + terminator
    std::array<const char*, argc> argv;
    const char* version = GetZapdVerStr();
    const char* otrFile = "mm.o2r";

    std::string romPath = std::filesystem::absolute(mCurrentRomPath).string();
    installPath = std::filesystem::absolute(installPath).string();
    exportdir = std::filesystem::absolute(exportdir).string();
    // Work this out in the temporary folder
    std::string tempdir = Mkdtemp();
    std::string curdir = std::filesystem::current_path().string();
#if defined(_WIN32) || defined(__ANDROID__)
    // Shared storage on Android does not allow symlinks.
    std::error_code copyError;
    std::filesystem::copy(installPath + "/assets", tempdir + "/assets",
                          std::filesystem::copy_options::recursive | std::filesystem::copy_options::update_existing,
                          copyError);
    if (copyError) {
        std::string message =
            "Não foi possível copiar os arquivos do extrator (" + installPath + "/assets): " + copyError.message();
        ShowErrorBox("Erro na extração", message.c_str());
        exit(1);
    }
#else
    std::filesystem::create_symlink(installPath + "/assets", tempdir + "/assets");
#endif

    std::filesystem::current_path(tempdir);

    snprintf(xmlPath, 1024, "assets/xml/%s", version);
    snprintf(confPath, 1024, "assets/Config_%s.xml", version);
    snprintf(portVersion, 18, "%d.%d.%d", gBuildVersionMajor, gBuildVersionMinor, gBuildVersionPatch);

    argv[0] = "ZAPD";
    argv[1] = "ed";
    argv[2] = "-i";
    argv[3] = xmlPath;
    argv[4] = "-b";
    argv[5] = romPath.c_str();
    argv[6] = "-fl";
    argv[7] = "assets/filelists";
    argv[8] = "-gsf";
    argv[9] = "0";
    argv[10] = "-rconf";
    argv[11] = confPath;
    argv[12] = "-se";
    argv[13] = "OTR";
    argv[14] = "--otrfile";
    argv[15] = otrFile;
    argv[16] = "--portVer";
    argv[17] = portVersion;
    argv[18] = "-o";
    argv[19] = "placeholder";
    argv[20] = "-osf";
    argv[21] = "placeholder";

    // On PC the preparation screen shows the progress while ZAPD works on another thread (below); without it, the
    // command window (Windows) or a message box (Linux) says the extraction is running.
#ifndef __ANDROID__
    const bool setupScreen = SetupScreen::Ensure();
#else
    const bool setupScreen = false;
#endif
#ifdef _WIN32
    // Grab a handle to the command window.
    HWND cmdWindow = GetConsoleWindow();

    // Normally the command window is hidden. We want the window to be shown here so the user can see the progess of the
    // extraction.
    if (!setupScreen) {
        ShowWindow(cmdWindow, SW_SHOW);
        SetWindowPos(cmdWindow, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
    }
#elif defined(__ANDROID__)
    StartAndroidProgress();
#else
    if (!setupScreen) {
        std::thread mbThread(MessageboxWorker);
        mbThread.detach();
    }
#endif

#ifdef __ANDROID__
    // Android has no console: ZAPD's output goes to a file in the data folder, unbuffered so it is complete even if
    // the extraction stops halfway.
    const std::string logPath = exportdir + "/extracao.log";
    if (freopen(logPath.c_str(), "w", stdout) != nullptr) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        dup2(fileno(stdout), fileno(stderr));
    }
    printf("ROM: %s\nAssets: %s\nVersion: %s\n", romPath.c_str(), installPath.c_str(), version);
#endif

    StartupLog_Step("ZAPD começou (ROM %s, pasta %s)", romPath.c_str(), tempdir.c_str());
    // ZAPD reports a failure by throwing. Uncaught, that closes the app without a word.
    std::string failure;
    auto runZapd = [&]() {
        try {
            if (zapd_main(argc, (char**)argv.data()) != 0) {
                failure = "O ZAPD terminou com erro.";
            }
        } catch (const std::exception& e) { failure = e.what(); } catch (...) {
            failure = "Erro desconhecido no ZAPD.";
        }
    };
#ifndef __ANDROID__
    if (setupScreen) {
        gZapdFilesDone = 0;
        gZapdFilesTotal = 0;
        std::atomic<bool> finished = false;
        std::thread worker([&]() {
            runZapd();
            finished = true;
        });
        while (!finished) {
            int total = gZapdFilesTotal, done = gZapdFilesDone;
            std::string text = "Criando o mm.o2r com os arquivos da sua ROM. Isso só acontece uma vez e pode levar "
                               "alguns minutos.";
            if (total > 0) {
                text += "\n\n" + std::to_string(std::min(done, total)) + " de " + std::to_string(total) + " arquivos";
            }
            SetupScreen::Progress("Preparando o jogo", text, total > 0 ? (float)done / total : -1.0f);
        }
        worker.join();
    } else {
        runZapd();
    }
#else
    runZapd();
#endif

#ifdef _WIN32
    // Hide the command window again.
    if (!setupScreen) {
        ShowWindow(cmdWindow, SW_HIDE);
    }
#endif

#ifdef __ANDROID__
    StopAndroidProgress();
#endif
    StartupLog_Step("ZAPD terminou%s%s", failure.empty() ? "" : ": ", failure.c_str());
    std::error_code ec;
    if (failure.empty() && !std::filesystem::exists(otrFile, ec)) {
        failure = "O ZAPD terminou sem criar o mm.o2r.";
    }
    if (failure.empty()) {
        std::filesystem::copy(otrFile, exportdir + "/" + otrFile, std::filesystem::copy_options::overwrite_existing,
                              ec);
        if (ec) {
            failure = "Não foi possível copiar o mm.o2r para " + exportdir + ": " + ec.message();
        }
    }
    if (!failure.empty()) {
        printf("Falha: %s\n", failure.c_str());
        std::filesystem::current_path(curdir, ec);
        std::filesystem::remove_all(tempdir, ec);
        std::string message = "A criação do mm.o2r falhou:\n\n" + failure;
#ifdef __ANDROID__
        message += "\n\nO registro completo está em " + logPath;
#endif
        ShowErrorBox("Erro na extração", message.c_str());
        exit(1);
    }

    // Go back to where this game was executed from
    std::filesystem::current_path(curdir);
    std::filesystem::remove_all(tempdir);

    return false;
}

static void MessageboxWorker() {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Extraindo",
                             "A extração dos arquivos do jogo vai começar agora.\n\nAguarde até terminar (pode levar "
                             "alguns minutos). Não feche o app.",
                             nullptr);
}