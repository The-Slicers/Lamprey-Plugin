/*
 * SlicerJNI.cpp
 *
 * JNI bridge between Quorum's Libraries.Game.Graphics.Models.Printing.Slicer
 * and OrcaSlicer.
 *
 * Java side (plugins.quorum.Libraries.Game.Graphics.Models.Printing.Slicer):
 *
 *     private static native String OrcaSlice(float[] vertices, int[] indices,
 *             String settings, String filament, String outputFolder);
 *     private static native void SetOrcaSlicerPath(String path);
 *
 * What it does
 *   1. Validates the mesh (xyz floats, triangle index list).
 *   2. Writes it as a binary STL into the output folder, converting Quorum's
 *      Y-up coordinates to the Z-up coordinates slicers use.
 *   3. Runs OrcaSlicer's command line slicer on it:
 *          --slice 0 --arrange 1 --ensure-on-bed
 *          --load-settings "machine.json;process.json"
 *          --load-filaments "filament.json"
 *          --outputdir <outputFolder> <model.stl>
 *   4. Returns the absolute path of the G-code it produced.
 *
 * Why OrcaSlicer runs as a child process
 *   OrcaSlicer.dll exports only one usable C function, orcaslicer_main(argc,
 *   wchar_t** argv), which is the CLI's main(); none of libslic3r's classes
 *   are exported. Calling it inside the JVM does not work: OrcaSlicer finds
 *   its "resources" folder (every printer/filament profile) relative to the
 *   running executable, which would be java.exe. It also installs process-
 *   wide crash and new_handlers. On Windows, orca-slicer.exe is OrcaSlicer's
 *   own tiny launcher that LoadLibrary()s OrcaSlicer.dll and calls
 *   orcaslicer_main, so running it is how OrcaSlicer.dll gets called. On
 *   macOS and Linux there is no separate library; the CLI is the main binary.
 *   Running out of process also means a crash in the slicer cannot take the
 *   Quorum program down with it.
 *
 * Finding OrcaSlicer (first match wins)
 *   1. The path given to SetOrcaSlicerPath(). It may be the executable, the
 *      folder that contains it (e.g. the folder holding OrcaSlicer.dll), a
 *      macOS .app bundle, or a Linux AppImage.
 *   2. The ORCASLICER_PATH environment variable (same forms as above).
 *   3. Standard install locations, then the PATH.
 *
 * Builds with any C++17 compiler: MSVC or MinGW on Windows, Clang on macOS,
 * GCC or Clang on Linux. See CMakeLists.txt next to this file.
 */

#include <jni.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <cerrno>
    #include <fcntl.h>
    #include <signal.h>
    #include <spawn.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <unistd.h>
    extern char** environ;
#endif

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

// Quorum (like libGDX) is Y-up; slicers are Z-up. Rotating +90 degrees about X
// maps (x, y, z) -> (x, -z, y), which keeps triangle winding (so normals stay
// pointing outward). Set to false if your meshes are already Z-up.
constexpr bool kConvertYUpToZUp = true;

// Give up on OrcaSlicer if it has not finished after this long.
constexpr auto kSliceTimeout = std::chrono::minutes(30);

// File names written into the output folder.
const char* const kModelFileName = "QuorumModel.stl";
const char* const kGcodeFileName = "QuorumModel.gcode";
const char* const kLogFileName   = "OrcaSlicer.log";

std::mutex  g_mutex;          // one slice at a time; guards g_orcaPath
fs::path    g_orcaPath;       // set by SetOrcaSlicerPath

// Thrown inside the bridge, turned into a java.lang.RuntimeException at the
// JNI boundary. C++ exceptions must never cross into the JVM.
struct SliceError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------------------
// Strings: Java strings are UTF-16. Windows paths are UTF-16, POSIX are UTF-8.
// (GetStringUTFChars returns *modified* UTF-8, which is wrong for paths with
// characters outside the BMP, so convert from UTF-16 ourselves.)
// ---------------------------------------------------------------------------

std::string Utf16ToUtf8(const std::u16string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        uint32_t c = in[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < in.size() &&
            in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (in[i + 1] - 0xDC00);
            ++i;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;  // lone surrogate
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::u16string Utf8ToUtf16(const std::string& in) {
    std::u16string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        unsigned char b = static_cast<unsigned char>(in[i]);
        uint32_t c;
        size_t extra;
        if (b < 0x80)                { c = b;        extra = 0; }
        else if ((b & 0xE0) == 0xC0) { c = b & 0x1F; extra = 1; }
        else if ((b & 0xF0) == 0xE0) { c = b & 0x0F; extra = 2; }
        else if ((b & 0xF8) == 0xF0) { c = b & 0x07; extra = 3; }
        else                         { out += u'�'; ++i; continue; }
        bool bad = false;
        for (size_t k = 1; k <= extra; ++k) {
            if (i + k >= in.size() ||
                (static_cast<unsigned char>(in[i + k]) & 0xC0) != 0x80) { bad = true; break; }
            c = (c << 6) | (static_cast<unsigned char>(in[i + k]) & 0x3F);
        }
        if (bad) { out += u'�'; ++i; continue; }
        i += extra + 1;
        if (c >= 0x10000) {
            c -= 0x10000;
            out += static_cast<char16_t>(0xD800 + (c >> 10));
            out += static_cast<char16_t>(0xDC00 + (c & 0x3FF));
        } else {
            out += static_cast<char16_t>(c);
        }
    }
    return out;
}

// Java string -> UTF-16. Returns empty for null.
std::u16string JavaToUtf16(JNIEnv* env, jstring s) {
    if (s == nullptr) return {};
    const jsize length = env->GetStringLength(s);
    std::u16string out(static_cast<size_t>(length), u'\0');
    if (length > 0)
        env->GetStringRegion(s, 0, length, reinterpret_cast<jchar*>(&out[0]));
    return out;
}

fs::path Utf16ToPath(const std::u16string& s) {
#ifdef _WIN32
    return fs::path(std::wstring(s.begin(), s.end()));
#else
    return fs::path(Utf16ToUtf8(s));
#endif
}

std::u16string PathToUtf16(const fs::path& p) {
#ifdef _WIN32
    const std::wstring& w = p.native();
    return std::u16string(w.begin(), w.end());
#else
    return Utf8ToUtf16(p.native());
#endif
}

// Human-readable path for error messages (UTF-8).
std::string Show(const fs::path& p) { return Utf16ToUtf8(PathToUtf16(p)); }

jstring NewJavaString(JNIEnv* env, const std::u16string& s) {
    return env->NewString(reinterpret_cast<const jchar*>(s.data()),
                          static_cast<jsize>(s.size()));
}

void ThrowJava(JNIEnv* env, const char* className, const std::string& message) {
    if (env->ExceptionCheck()) return;  // keep the first exception
    jclass cls = env->FindClass(className);
    if (cls == nullptr) return;         // FindClass already threw
    // ThrowNew wants modified UTF-8; going through NewString keeps any
    // non-ASCII path in the message intact.
    jmethodID ctor = env->GetMethodID(cls, "<init>", "(Ljava/lang/String;)V");
    jstring jmsg = NewJavaString(env, Utf8ToUtf16(message));
    if (ctor != nullptr && jmsg != nullptr) {
        jobject ex = env->NewObject(cls, ctor, jmsg);
        if (ex != nullptr) env->Throw(static_cast<jthrowable>(ex));
    }
}

// Split "a.json;b.json" (OrcaSlicer's own list separator) and drop blanks.
std::vector<std::u16string> SplitList(const std::u16string& s) {
    std::vector<std::u16string> parts;
    std::u16string cur;
    auto flush = [&] {
        size_t b = cur.find_first_not_of(u" \t\r\n");
        size_t e = cur.find_last_not_of(u" \t\r\n");
        if (b != std::u16string::npos) parts.push_back(cur.substr(b, e - b + 1));
        cur.clear();
    };
    for (char16_t c : s) {
        if (c == u';' || c == u'\n') flush();
        else cur += c;
    }
    flush();
    return parts;
}

// ---------------------------------------------------------------------------
// Mesh -> binary STL
// ---------------------------------------------------------------------------

struct Vec3 { float x, y, z; };

void PutFloat(std::vector<char>& buf, size_t& pos, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    // STL is little-endian regardless of host.
    buf[pos++] = static_cast<char>(bits & 0xFF);
    buf[pos++] = static_cast<char>((bits >> 8) & 0xFF);
    buf[pos++] = static_cast<char>((bits >> 16) & 0xFF);
    buf[pos++] = static_cast<char>((bits >> 24) & 0xFF);
}

void PutU32(std::vector<char>& buf, size_t& pos, uint32_t v) {
    for (int i = 0; i < 4; ++i) buf[pos++] = static_cast<char>((v >> (8 * i)) & 0xFF);
}

// Returns the number of triangles written.
size_t WriteBinaryStl(const fs::path& file,
                      const std::vector<float>& vertices,
                      const std::vector<int32_t>& indices) {
    const size_t vertexCount = vertices.size() / 3;

    std::vector<Vec3> points(vertexCount);
    for (size_t i = 0; i < vertexCount; ++i) {
        const float x = vertices[3 * i], y = vertices[3 * i + 1], z = vertices[3 * i + 2];
        points[i] = kConvertYUpToZUp ? Vec3{x, -z, y} : Vec3{x, y, z};
    }

    // Keep only real triangles: drop ones that reuse a vertex index.
    std::vector<size_t> kept;
    kept.reserve(indices.size() / 3);
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        const int32_t a = indices[t], b = indices[t + 1], c = indices[t + 2];
        if (a != b && b != c && a != c) kept.push_back(t);
    }
    if (kept.empty())
        throw SliceError("The mesh has no usable triangles (every triangle repeats a vertex).");
    if (kept.size() > 0xFFFFFFFFull)
        throw SliceError("The mesh has too many triangles for an STL file.");

    std::vector<char> buf(84 + 50 * kept.size(), 0);
    const char header[] = "Quorum mesh exported for OrcaSlicer";
    std::memcpy(buf.data(), header, sizeof(header) - 1);
    size_t pos = 80;
    PutU32(buf, pos, static_cast<uint32_t>(kept.size()));

    for (size_t t : kept) {
        const Vec3& p0 = points[static_cast<size_t>(indices[t])];
        const Vec3& p1 = points[static_cast<size_t>(indices[t + 1])];
        const Vec3& p2 = points[static_cast<size_t>(indices[t + 2])];

        // Facet normal from the winding (counter-clockwise = outward).
        const double ux = p1.x - p0.x, uy = p1.y - p0.y, uz = p1.z - p0.z;
        const double vx = p2.x - p0.x, vy = p2.y - p0.y, vz = p2.z - p0.z;
        double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 0) { nx /= len; ny /= len; nz /= len; }

        PutFloat(buf, pos, static_cast<float>(nx));
        PutFloat(buf, pos, static_cast<float>(ny));
        PutFloat(buf, pos, static_cast<float>(nz));
        for (const Vec3* p : {&p0, &p1, &p2}) {
            PutFloat(buf, pos, p->x);
            PutFloat(buf, pos, p->y);
            PutFloat(buf, pos, p->z);
        }
        pos += 2;  // attribute byte count = 0
    }

    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out)
        throw SliceError("Could not create the model file " + Show(file));
    out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    out.close();
    if (!out)
        throw SliceError("Could not write the model file " + Show(file) + " (is the disk full?)");
    return kept.size();
}

// ---------------------------------------------------------------------------
// Locating OrcaSlicer
// ---------------------------------------------------------------------------

bool IsFile(const fs::path& p) {
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(p, ec);
}

bool IsDir(const fs::path& p) {
    std::error_code ec;
    return !p.empty() && fs::is_directory(p, ec);
}

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

fs::path EnvPath(const char* name) {
#ifdef _WIN32
    std::wstring wname(name, name + std::strlen(name));
    DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) return {};
    std::wstring value(n, L'\0');
    n = GetEnvironmentVariableW(wname.c_str(), &value[0], n);
    value.resize(n);
    return fs::path(value);
#else
    const char* v = std::getenv(name);
    return (v && *v) ? fs::path(v) : fs::path();
#endif
}

// Turn whatever the user pointed at into the executable to run.
// Returns empty if it cannot be resolved.
fs::path ResolveExecutable(const fs::path& given) {
    if (given.empty()) return {};

#ifdef _WIN32
    if (IsFile(given)) {
        const std::string name = ToLower(Show(given.filename()));
        // Pointed at OrcaSlicer.dll: its launcher sits right beside it.
        if (name == "orcaslicer.dll") {
            fs::path exe = given.parent_path() / L"orca-slicer.exe";
            return IsFile(exe) ? exe : fs::path();
        }
        return given;
    }
    if (IsDir(given)) {
        for (const wchar_t* candidate : {L"orca-slicer.exe", L"OrcaSlicer.exe"}) {
            fs::path exe = given / candidate;
            if (IsFile(exe)) return exe;
        }
    }
#elif defined(__APPLE__)
    // OrcaSlicer.app/Contents/MacOS/OrcaSlicer
    const std::string ext = ToLower(given.extension().string());
    if (IsDir(given) && ext == ".app") {
        fs::path exe = given / "Contents" / "MacOS" / "OrcaSlicer";
        return IsFile(exe) ? exe : fs::path();
    }
    if (IsFile(given)) return given;
    if (IsDir(given)) {
        for (const char* candidate : {"OrcaSlicer.app/Contents/MacOS/OrcaSlicer",
                                      "Contents/MacOS/OrcaSlicer", "OrcaSlicer"}) {
            fs::path exe = given / candidate;
            if (IsFile(exe)) return exe;
        }
    }
#else
    // Linux: the binary, an AppImage, or an extracted/installed tree.
    if (IsFile(given)) return given;
    if (IsDir(given)) {
        for (const char* candidate : {"bin/orca-slicer", "orca-slicer", "AppRun",
                                      "squashfs-root/AppRun", "OrcaSlicer"}) {
            fs::path exe = given / candidate;
            if (IsFile(exe)) return exe;
        }
    }
#endif
    return {};
}

fs::path FindOnPath(const std::vector<std::string>& names) {
    fs::path pathVar = EnvPath("PATH");
    if (pathVar.empty()) return {};
#ifdef _WIN32
    const wchar_t sep = L';';
    const std::wstring all = pathVar.native();
#else
    const char sep = ':';
    const std::string all = pathVar.native();
#endif
    size_t start = 0;
    while (start <= all.size()) {
        size_t end = all.find(sep, start);
        if (end == all.npos) end = all.size();
        fs::path dir(all.substr(start, end - start));
        if (!dir.empty()) {
            for (const std::string& n : names) {
                fs::path exe = dir / fs::path(n);
                if (IsFile(exe)) return exe;
            }
        }
        start = end + 1;
    }
    return {};
}

std::vector<fs::path> DefaultLocations() {
    std::vector<fs::path> places;
#ifdef _WIN32
    for (const char* var : {"ProgramW6432", "ProgramFiles", "ProgramFiles(x86)"}) {
        fs::path base = EnvPath(var);
        if (!base.empty()) places.push_back(base / L"OrcaSlicer");
    }
    fs::path local = EnvPath("LOCALAPPDATA");
    if (!local.empty()) places.push_back(local / L"Programs" / L"OrcaSlicer");
#elif defined(__APPLE__)
    places.emplace_back("/Applications/OrcaSlicer.app");
    fs::path home = EnvPath("HOME");
    if (!home.empty()) places.push_back(home / "Applications" / "OrcaSlicer.app");
#else
    places.emplace_back("/usr/bin/orca-slicer");
    places.emplace_back("/usr/local/bin/orca-slicer");
    places.emplace_back("/opt/OrcaSlicer");
    places.emplace_back("/opt/orca-slicer");
    fs::path home = EnvPath("HOME");
    if (!home.empty()) {
        places.push_back(home / ".local" / "bin" / "orca-slicer");
        // AppImages are usually dropped in ~/Applications or ~/Downloads.
        for (const char* folder : {"Applications", "Downloads", "Desktop"}) {
            std::error_code ec;
            fs::path dir = home / folder;
            if (!IsDir(dir)) continue;
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                const std::string name = ToLower(entry.path().filename().string());
                if (name.rfind("orcaslicer", 0) == 0 &&
                    name.size() > 9 && name.substr(name.size() - 9) == ".appimage")
                    places.push_back(entry.path());
            }
        }
    }
#endif
    return places;
}

fs::path LocateOrcaSlicer(const fs::path& configured) {
    if (!configured.empty()) {
        fs::path exe = ResolveExecutable(configured);
        if (exe.empty())
            throw SliceError("OrcaSlicer was not found at " + Show(configured) +
                             ". Point the Slicer at the OrcaSlicer executable or the "
                             "folder it is installed in.");
        return exe;
    }

    fs::path fromEnv = EnvPath("ORCASLICER_PATH");
    if (!fromEnv.empty()) {
        fs::path exe = ResolveExecutable(fromEnv);
        if (exe.empty())
            throw SliceError("ORCASLICER_PATH is set to " + Show(fromEnv) +
                             ", but no OrcaSlicer executable was found there.");
        return exe;
    }

    for (const fs::path& place : DefaultLocations()) {
        fs::path exe = ResolveExecutable(place);
        if (!exe.empty()) return exe;
    }

#ifdef _WIN32
    fs::path onPath = FindOnPath({"orca-slicer.exe"});
#elif defined(__APPLE__)
    fs::path onPath = FindOnPath({"OrcaSlicer", "orca-slicer"});
#else
    fs::path onPath = FindOnPath({"orca-slicer", "OrcaSlicer"});
#endif
    if (!onPath.empty()) return onPath;

    throw SliceError(
        "OrcaSlicer could not be found. Install it from https://github.com/OrcaSlicer/OrcaSlicer/releases, "
        "then either call SetOrcaSlicerLocation on the Slicer or set the ORCASLICER_PATH "
        "environment variable to its install folder.");
}

// ---------------------------------------------------------------------------
// Running OrcaSlicer
// ---------------------------------------------------------------------------

struct RunResult {
    int  exitCode = -1;
    bool timedOut = false;
};

#ifdef _WIN32

// Quote one argument the way CommandLineToArgvW / the MSVC runtime parse it.
std::wstring QuoteWindowsArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return arg;
    std::wstring out = L"\"";
    for (size_t i = 0;; ++i) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') { ++i; ++backslashes; }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += L'"';
        } else {
            out.append(backslashes, L'\\');
            out += arg[i];
        }
    }
    out += L'"';
    return out;
}

std::string WindowsErrorText(DWORD code) {
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    std::string msg = "error " + std::to_string(code);
    if (text) {
        std::wstring w(text);
        LocalFree(text);
        while (!w.empty() && (w.back() == L'\n' || w.back() == L'\r')) w.pop_back();
        msg += ": " + Utf16ToUtf8(std::u16string(w.begin(), w.end()));
    }
    return msg;
}

RunResult RunProcess(const fs::path& exe, const std::vector<fs::path>& args,
                     const fs::path& logFile, const fs::path& workDir) {
    std::wstring cmd = QuoteWindowsArg(exe.native());
    for (const fs::path& a : args) cmd += L" " + QuoteWindowsArg(a.native());

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE log = CreateFileW(logFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE)
        throw SliceError("Could not create the log file " + Show(logFile) + " (" +
                         WindowsErrorText(GetLastError()) + ")");
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);

    // Hand the child only these three handles, not everything the JVM has open.
    HANDLE inherit[2] = {log, nul};
    const DWORD inheritCount = (nul != INVALID_HANDLE_VALUE) ? 2 : 1;
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<char> attrBuf(attrSize);
    auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    bool haveAttrs = InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
                     UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                               inherit, inheritCount * sizeof(HANDLE),
                                               nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput  = (nul != INVALID_HANDLE_VALUE) ? nul : nullptr;
    si.StartupInfo.hStdOutput = log;
    si.StartupInfo.hStdError  = log;
    if (haveAttrs) si.lpAttributeList = attrs;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    // orca-slicer.exe loads OrcaSlicer.dll from its own folder and calls
    // orcaslicer_main(argc, argv) with these arguments.
    BOOL ok = CreateProcessW(exe.c_str(), cmdBuf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                                 (haveAttrs ? EXTENDED_STARTUPINFO_PRESENT : 0),
                             nullptr, workDir.c_str(),
                             reinterpret_cast<STARTUPINFOW*>(&si), &pi);
    const DWORD createError = GetLastError();
    if (haveAttrs) DeleteProcThreadAttributeList(attrs);
    CloseHandle(log);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok)
        throw SliceError("Could not start " + Show(exe) + " (" + WindowsErrorText(createError) + ")");

    RunResult result;
    const DWORD waitMs = static_cast<DWORD>(
        std::chrono::duration_cast<std::chrono::milliseconds>(kSliceTimeout).count());
    if (WaitForSingleObject(pi.hProcess, waitMs) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
        result.timedOut = true;
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    result.exitCode = static_cast<int>(code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return result;
}

#else  // POSIX: macOS and Linux

RunResult RunProcess(const fs::path& exe, const std::vector<fs::path>& args,
                     const fs::path& logFile, const fs::path& /*workDir*/) {
    // All paths passed to OrcaSlicer are absolute, so the working directory
    // does not matter (and posix_spawn has no portable chdir).
    std::vector<std::string> storage;
    storage.push_back(exe.native());
    for (const fs::path& a : args) storage.push_back(a.native());
    std::vector<char*> argv;
    for (std::string& s : storage) argv.push_back(&s[0]);
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, logFile.c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);

    // The JVM blocks/ignores some signals; give the child the defaults.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t noSignals, allSignals;
    sigemptyset(&noSignals);
    sigfillset(&allSignals);
    posix_spawnattr_setsigmask(&attr, &noSignals);
    posix_spawnattr_setsigdefault(&attr, &allSignals);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    pid_t pid = 0;
    const int err = posix_spawn(&pid, exe.c_str(), &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (err != 0)
        throw SliceError("Could not start " + Show(exe) + " (" + std::strerror(err) + ")");

    RunResult result;
    const auto deadline = std::chrono::steady_clock::now() + kSliceTimeout;
    int status = 0;
    for (;;) {
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) {
            // Someone else reaped it (e.g. SIGCHLD set to SIG_IGN). Nothing to report.
            status = 0;
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            result.timedOut = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (WIFEXITED(status))
        result.exitCode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        result.exitCode = 128 + WTERMSIG(status);
    return result;
}

#endif

// Last few lines of OrcaSlicer's output, for error messages.
std::string LogTail(const fs::path& logFile, size_t maxLines = 15) {
    std::ifstream in(logFile, std::ios::binary);
    if (!in) return {};
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        lines.push_back(line);
        if (lines.size() > maxLines) lines.erase(lines.begin());
    }
    std::string out;
    for (const std::string& l : lines) out += "\n    " + l;
    return out;
}

// ---------------------------------------------------------------------------
// The slice itself
// ---------------------------------------------------------------------------

fs::path Slice(const std::vector<float>& vertices,
               std::vector<int32_t> indices,
               const std::u16string& settingsList,
               const std::u16string& filamentList,
               const std::u16string& outputFolderText) {
    // ---- Validate the mesh ------------------------------------------------
    if (vertices.empty() || vertices.size() % 3 != 0)
        throw SliceError("The vertex array must hold x, y, z for each vertex (its size must be a "
                         "non-zero multiple of 3); it has " + std::to_string(vertices.size()) +
                         " values.");
    for (size_t i = 0; i < vertices.size(); ++i)
        if (!std::isfinite(vertices[i]))
            throw SliceError("Vertex value " + std::to_string(i) + " is not a finite number.");

    const size_t vertexCount = vertices.size() / 3;
    if (indices.empty()) {
        // No index list: every three vertices form a triangle.
        if (vertexCount % 3 != 0)
            throw SliceError("Without indices, the vertex count must be a multiple of 3; it is " +
                             std::to_string(vertexCount) + ".");
        indices.resize(vertexCount);
        for (size_t i = 0; i < vertexCount; ++i) indices[i] = static_cast<int32_t>(i);
    }
    if (indices.size() % 3 != 0)
        throw SliceError("The index array must hold 3 indices per triangle; it has " +
                         std::to_string(indices.size()) + ".");
    for (size_t i = 0; i < indices.size(); ++i)
        if (indices[i] < 0 || static_cast<size_t>(indices[i]) >= vertexCount)
            throw SliceError("Index " + std::to_string(i) + " is " + std::to_string(indices[i]) +
                             ", but there are only " + std::to_string(vertexCount) + " vertices.");

    // ---- Settings files ---------------------------------------------------
    // OrcaSlicer takes printer + process presets in --load-settings and one
    // file per filament in --load-filaments, each list separated by ';'.
    const std::vector<std::u16string> settings  = SplitList(settingsList);
    const std::vector<std::u16string> filaments = SplitList(filamentList);
    std::vector<fs::path> settingsPaths, filamentPaths;
    for (const auto& s : settings) {
        fs::path p = fs::absolute(Utf16ToPath(s));
        if (!IsFile(p)) throw SliceError("Settings file not found: " + Show(p));
        settingsPaths.push_back(p);
    }
    for (const auto& s : filaments) {
        fs::path p = fs::absolute(Utf16ToPath(s));
        if (!IsFile(p)) throw SliceError("Filament file not found: " + Show(p));
        filamentPaths.push_back(p);
    }
    auto joinList = [](const std::vector<fs::path>& paths) {
        fs::path::string_type joined;
        for (size_t i = 0; i < paths.size(); ++i) {
            if (i) joined += fs::path::value_type(';');
            joined += paths[i].native();
        }
        return fs::path(joined);
    };

    // ---- Output folder ----------------------------------------------------
    if (outputFolderText.empty())
        throw SliceError("No output folder was given.");
    std::error_code ec;
    fs::path outDir = fs::absolute(Utf16ToPath(outputFolderText), ec);
    if (ec) throw SliceError("Bad output folder path: " + Utf16ToUtf8(outputFolderText));
    fs::create_directories(outDir, ec);
    if (!IsDir(outDir))
        throw SliceError("Could not create the output folder " + Show(outDir));
    outDir = fs::weakly_canonical(outDir, ec);

    const fs::path modelFile = outDir / kModelFileName;
    const fs::path logFile   = outDir / kLogFileName;
    const fs::path finalGcode = outDir / kGcodeFileName;
    const fs::path orcaGcode = outDir / "plate_1.gcode";  // OrcaSlicer's own name

    // Never hand back a G-code file left over from an earlier run.
    fs::remove(orcaGcode, ec);
    fs::remove(finalGcode, ec);

    WriteBinaryStl(modelFile, vertices, indices);

    // ---- Run OrcaSlicer ---------------------------------------------------
    // The caller holds g_mutex, so g_orcaPath can be read directly.
    const fs::path exe = LocateOrcaSlicer(g_orcaPath);

    std::vector<fs::path> args = {
        "--slice", "0",          // slice every plate (there is one)
        "--arrange", "1",        // centre the model on the bed
        "--ensure-on-bed",       // drop it so its lowest point sits on the bed
    };
    if (!settingsPaths.empty()) {
        args.emplace_back("--load-settings");
        args.push_back(joinList(settingsPaths));
    }
    if (!filamentPaths.empty()) {
        args.emplace_back("--load-filaments");
        args.push_back(joinList(filamentPaths));
    }
    args.emplace_back("--outputdir");
    args.push_back(outDir);
    args.push_back(modelFile);

    const RunResult run = RunProcess(exe, args, logFile, outDir);

    if (run.timedOut)
        throw SliceError("OrcaSlicer did not finish within " +
                         std::to_string(std::chrono::duration_cast<std::chrono::minutes>(kSliceTimeout).count()) +
                         " minutes and was stopped. Its output is in " + Show(logFile));
    if (run.exitCode != 0 || !IsFile(orcaGcode)) {
        std::ostringstream msg;
        msg << "OrcaSlicer could not slice the model (exit code " << run.exitCode << ").";
        if (settingsPaths.empty())
            msg << " No printer/process settings were given; add them with AddSettingsFile.";
        const std::string tail = LogTail(logFile);
        if (!tail.empty()) msg << "\n  Last lines of " << Show(logFile) << ":" << tail;
        throw SliceError(msg.str());
    }

    fs::rename(orcaGcode, finalGcode, ec);
    return ec ? orcaGcode : finalGcode;
}

}  // namespace

// ===========================================================================
// JNI entry points
// ===========================================================================

extern "C" {

JNIEXPORT jstring JNICALL
Java_plugins_quorum_Libraries_Game_Graphics_Models_Printing_Slicer_OrcaSlice(
        JNIEnv* env, jclass /*cls*/,
        jfloatArray jvertices, jintArray jindices,
        jstring jsettings, jstring jfilament, jstring joutputFolder) {
    try {
        if (jvertices == nullptr)
            throw SliceError("The vertex array is undefined.");

        std::vector<float> vertices(static_cast<size_t>(env->GetArrayLength(jvertices)));
        if (!vertices.empty())
            env->GetFloatArrayRegion(jvertices, 0, static_cast<jsize>(vertices.size()), vertices.data());

        std::vector<int32_t> indices;
        if (jindices != nullptr) {
            indices.resize(static_cast<size_t>(env->GetArrayLength(jindices)));
            if (!indices.empty())
                env->GetIntArrayRegion(jindices, 0, static_cast<jsize>(indices.size()),
                                       reinterpret_cast<jint*>(indices.data()));
        }
        if (env->ExceptionCheck()) return nullptr;

        const std::u16string settings = JavaToUtf16(env, jsettings);
        const std::u16string filament = JavaToUtf16(env, jfilament);
        const std::u16string output   = JavaToUtf16(env, joutputFolder);

        std::lock_guard<std::mutex> lock(g_mutex);
        const fs::path gcode = Slice(vertices, std::move(indices), settings, filament, output);
        return NewJavaString(env, PathToUtf16(gcode));
    } catch (const SliceError& e) {
        ThrowJava(env, "java/lang/RuntimeException", e.what());
    } catch (const fs::filesystem_error& e) {
        ThrowJava(env, "java/lang/RuntimeException", std::string("File error while slicing: ") + e.what());
    } catch (const std::bad_alloc&) {
        ThrowJava(env, "java/lang/OutOfMemoryError", "Out of memory while preparing the model for OrcaSlicer.");
    } catch (const std::exception& e) {
        ThrowJava(env, "java/lang/RuntimeException", std::string("Slicing failed: ") + e.what());
    } catch (...) {
        ThrowJava(env, "java/lang/RuntimeException", "Slicing failed with an unknown error.");
    }
    return nullptr;
}

JNIEXPORT void JNICALL
Java_plugins_quorum_Libraries_Game_Graphics_Models_Printing_Slicer_SetOrcaSlicerPath(
        JNIEnv* env, jclass /*cls*/, jstring jpath) {
    try {
        const std::u16string path = JavaToUtf16(env, jpath);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_orcaPath = path.empty() ? fs::path() : Utf16ToPath(path);
    } catch (const std::exception& e) {
        ThrowJava(env, "java/lang/RuntimeException", e.what());
    }
}

}  // extern "C"
