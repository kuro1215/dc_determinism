// dc_worker: runs the raw observation loop against the DigitalCurling C API loader.
// Usage: dc_worker --loader PATH --engine fcv1|fcv1plus --input PREPARED.bin --max-steps N
//                  [--engines fcv1,fcv1plus] [--capture-dir DIR]
//        dc_worker --loader PATH --engine fcv1|fcv1plus --seconds-per-frame
//        dc_worker --self-test
//        dc_worker --version
// stdin: one request per line; an empty line or EOF ends the session.
//   <pos>                  plain run (0-based record position)
//   O <pos> <steps|->      observe: plain run plus sampled boards at the listed steps
//   B <pos> <engine>       benchmark: one timed full simulation with the named engine
//   T <pos>                trajectory: plain run, also writes <DIR>/<pos>.raw (needs --capture-dir)
//   L <pos>                lockstep: fcv1 and fcv1plus stepped together (needs --engines fcv1,fcv1plus;
//                          --engine is not required in that mode)
// stdout per plain shot: <pos> <status> <steps> <trajectory_digest> <final_digest> <sim_ns>
// stdout additionally for O: S <pos> <n> <base64 payload>
// stdout for B: B <pos> <engine> <ns>
// stdout additionally for T: T <pos> <file_bytes>
// stdout for L: L <pos> <status_a> <steps_a> <status_b> <steps_b> <first_bit_diff_step>
//   <first_gt_1e-6> <first_gt_1e-3> <first_gt_1e-2> <max_distance> <max_step> <max_stone>
//   <presence_mismatch_pairs> <first_presence_mismatch_step> <base64(board_a + board_b)>
// .raw layout (little-endian): u32 active_mask (bit i set iff the 24 state bytes of stone i are
//   non-zero in any frame), u32 frame_count, then frame_count x popcount(mask) x 6 float32.
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
typedef std::wstring PathString;
#else
#include <dlfcn.h>
typedef std::string PathString;
#endif

// ---------------------------------------------------------------- SHA-256

class Sha256 {
public:
    Sha256() { reset(); }

    void reset() {
        static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        std::memcpy(h_, init, sizeof(h_));
        length_ = 0;
        fill_ = 0;
    }

    void update(const void* data, size_t size) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        length_ += size;
        if (fill_ > 0) {
            size_t take = 64 - fill_;
            if (take > size) take = size;
            std::memcpy(buf_ + fill_, p, take);
            fill_ += take;
            p += take;
            size -= take;
            if (fill_ == 64) {
                block(buf_);
                fill_ = 0;
            }
        }
        while (size >= 64) {
            block(p);
            p += 64;
            size -= 64;
        }
        if (size > 0) {
            std::memcpy(buf_, p, size);
            fill_ = size;
        }
    }

    std::string finish() {
        uint64_t bits = length_ * 8;
        uint8_t pad[72] = {0x80};
        size_t padLen = (fill_ < 56) ? (56 - fill_) : (120 - fill_);
        uint8_t tail[8];
        for (int i = 0; i < 8; ++i) tail[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
        update(pad, padLen);
        update(tail, 8);
        static const char* hex = "0123456789abcdef";
        std::string out;
        for (int i = 0; i < 8; ++i) {
            for (int s = 28; s >= 0; s -= 4) out.push_back(hex[(h_[i] >> s) & 0xf]);
        }
        reset();
        return out;
    }

private:
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void block(const uint8_t* p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
            0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
            0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
            0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
            0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
            0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) |
                   (uint32_t(p[4 * i + 2]) << 8) | uint32_t(p[4 * i + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
        uint32_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = h + s1 + ch + k[i] + w[i];
            uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
        h_[5] += f;
        h_[6] += g;
        h_[7] += h;
    }

    uint32_t h_[8];
    uint64_t length_;
    uint8_t buf_[64];
    size_t fill_;
};

static bool sha256SelfTest() {
    Sha256 s;
    if (s.finish() != "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") return false;
    s.update("abc", 3);
    if (s.finish() != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") return false;
    // Multi-block, split updates: 1000 bytes of 'a'.
    std::string a(1000, 'a');
    s.update(a.data(), 100);
    s.update(a.data() + 100, 900);
    return s.finish() == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3";
}

// ---------------------------------------------------------------- C API types

// DigitalCurling_HandleId: an opaque, nonzero ID issued by the loader (0 is never valid).
typedef uint64_t HandleId;
struct StoneState {
    float px, py, angle, vx, vy, angularVelocity;
};
struct StoneCoordinate {
    StoneState stones[16];
};
static_assert(sizeof(HandleId) == 8, "HandleId size");
static_assert(sizeof(StoneState) == 24, "StoneState size");
static_assert(sizeof(StoneCoordinate) == 384, "StoneCoordinate size");

typedef int (*FnCreateFactory)(const char*, const char*, HandleId*);
typedef int (*FnCreateSimulator)(HandleId, HandleId*);
typedef int (*FnRemove)(HandleId);
typedef int (*FnSetStones)(HandleId, const StoneCoordinate*);
typedef int (*FnStep)(HandleId, int, float);
typedef int (*FnGetStones)(HandleId, StoneCoordinate*);
typedef int (*FnStopped)(HandleId, bool*);
typedef int (*FnLastError)(char*, size_t, size_t*);
typedef int (*FnSimulate)(HandleId, float);
typedef int (*FnSecondsPerFrame)(HandleId, float*);
typedef int (*FnCapabilities)(HandleId, uint32_t*);

// DIGITALCURLING_PLUGIN_CAPABILITY_STEP: step, are_all_stones_stopped and get_seconds_per_frame are supported.
static const uint32_t kCapabilityStep = 1u << 2;

static FnSimulate pSimulate;
static FnCapabilities pCapabilities;
static FnSecondsPerFrame pSecondsPerFrame;
static FnCreateFactory pCreateFactory;
static FnCreateSimulator pCreateSimulator;
static FnRemove pRemove;
static FnSetStones pSetStones;
static FnStep pStep;
static FnGetStones pGetStones;
static FnStopped pStopped;
static FnLastError pLastError;

static void* loadSymbol(void* lib, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

static bool loadLoader(const PathString& path, std::string& error) {
    void* lib = nullptr;
#ifdef _WIN32
    // A missing dependency (e.g. the VC++ runtime) must fail immediately, not show a dialog.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX | SEM_NOGPFAULTERRORBOX);
    lib = LoadLibraryExW(path.c_str(), nullptr,
                         LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!lib) {
        error = "LoadLibraryExW failed, error " + std::to_string(GetLastError());
        return false;
    }
#else
    lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        const char* m = dlerror();
        error = std::string("dlopen failed: ") + (m ? m : "unknown");
        return false;
    }
#endif
    struct Entry {
        const char* name;
        void** slot;
    } entries[] = {
        {"dc_loader_create_simulator_factory", reinterpret_cast<void**>(&pCreateFactory)},
        {"dc_loader_create_simulator", reinterpret_cast<void**>(&pCreateSimulator)},
        {"dc_loader_remove_simulator_instance", reinterpret_cast<void**>(&pRemove)},
        {"dc_loader_simulator_set_stones", reinterpret_cast<void**>(&pSetStones)},
        {"dc_loader_simulator_step", reinterpret_cast<void**>(&pStep)},
        {"dc_loader_simulator_get_stones", reinterpret_cast<void**>(&pGetStones)},
        {"dc_loader_simulator_are_all_stones_stopped", reinterpret_cast<void**>(&pStopped)},
        {"dc_loader_get_last_error_message", reinterpret_cast<void**>(&pLastError)},
        {"dc_loader_simulator_simulate", reinterpret_cast<void**>(&pSimulate)},
        {"dc_loader_simulator_get_seconds_per_frame", reinterpret_cast<void**>(&pSecondsPerFrame)},
        {"dc_loader_simulator_get_capabilities", reinterpret_cast<void**>(&pCapabilities)},
    };
    for (const Entry& e : entries) {
        *e.slot = loadSymbol(lib, e.name);
        if (!*e.slot) {
            error = std::string("missing symbol ") + e.name;
            return false;
        }
    }
    return true;
}

static std::string lastError() {
    size_t required = 0;
    pLastError(nullptr, 0, &required);
    std::vector<char> buf(required > 256 ? required : 256, '\0');
    pLastError(buf.data(), buf.size(), &required);
    std::string s(buf.data());
    for (char& c : s) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    return s;
}

// Fails at start-up when the simulator of the factory cannot step: the shot runner needs step,
// are_all_stones_stopped and get_seconds_per_frame, which require the STEP capability.
static std::string checkStepCapability(HandleId factory, const std::string& engine) {
    HandleId sim = 0;
    if (pCreateSimulator(factory, &sim) != 0) return "create_simulator: " + lastError();
    uint32_t capabilities = 0;
    int rc = pCapabilities(sim, &capabilities);
    std::string err;
    if (rc != 0) {
        err = "get_capabilities: " + lastError();
    } else if ((capabilities & kCapabilityStep) == 0) {
        err = "engine " + engine + " does not support the STEP capability (step, are_all_stones_stopped, "
              "get_seconds_per_frame); this loader is not usable by dc_worker";
    }
    pRemove(sim);
    return err;
}

// ---------------------------------------------------------------- shots

struct Shot {
    uint32_t index;
    float sheetWidth;
    StoneCoordinate coord;
};

// The prepared file stays on disk: only the header is read up front, shots are read on demand.
static const long kHeaderSize = 12;
static const long kRecordSize = 8 + 384;

class ShotFile {
public:
    ~ShotFile() {
        if (f_) std::fclose(f_);
    }

    bool open(const PathString& path, std::string& error) {
#ifdef _WIN32
        f_ = _wfopen(path.c_str(), L"rb");
#else
        f_ = std::fopen(path.c_str(), "rb");
#endif
        if (!f_) {
            error = "cannot open input file";
            return false;
        }
        char magic[8];
        if (std::fread(magic, 1, 8, f_) != 8 || std::memcmp(magic, "DCSTIN01", 8) != 0 ||
            std::fread(&count_, 4, 1, f_) != 1) {
            error = "bad input header";
            return false;
        }
        return true;
    }

    uint32_t count() const { return count_; }

    bool read(uint64_t position, Shot& shot, std::string& error) {
#ifdef _WIN32
        int rc = _fseeki64(f_, static_cast<__int64>(kHeaderSize) + position * kRecordSize, SEEK_SET);
#else
        int rc = fseeko(f_, static_cast<off_t>(kHeaderSize) + position * kRecordSize, SEEK_SET);
#endif
        if (rc != 0 || std::fread(&shot.index, 4, 1, f_) != 1 ||
            std::fread(&shot.sheetWidth, 4, 1, f_) != 1 ||
            std::fread(&shot.coord, sizeof(StoneCoordinate), 1, f_) != 1) {
            error = "cannot read shot record";
            return false;
        }
        return true;
    }

private:
    FILE* f_ = nullptr;
    uint32_t count_ = 0;
};
static void writeError(const std::string& position, const std::string& message) {
    std::printf("ERR %s %s\n", position.c_str(), message.c_str());
    std::fflush(stdout);
}

// ---------------------------------------------------------------- board capture (O mode)

// Board encoding: u16 LE presence mask (bit i set iff stone i is present) followed by 16 x
// (f32 x, f32 y) little-endian; absent stones are written as 0,0. A stone is absent iff all six
// state values satisfy fabs((double)v) <= 1e-7.
static const size_t kBoardBytes = 2 + 16 * 8;

static void putF32(std::string& out, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((bits >> (8 * i)) & 0xff));
}

static void appendBoard(std::string& out, const StoneCoordinate& coord) {
    uint16_t mask = 0;
    float xy[16][2];
    for (int i = 0; i < 16; ++i) {
        const StoneState& s = coord.stones[i];
        const float values[6] = {s.px, s.py, s.angle, s.vx, s.vy, s.angularVelocity};
        bool absent = true;
        for (float v : values) {
            if (!(std::fabs(static_cast<double>(v)) <= 1e-7)) {
                absent = false;
                break;
            }
        }
        if (absent) {
            xy[i][0] = 0.0f;
            xy[i][1] = 0.0f;
        } else {
            mask = static_cast<uint16_t>(mask | (1u << i));
            xy[i][0] = s.px;
            xy[i][1] = s.py;
        }
    }
    out.push_back(static_cast<char>(mask & 0xff));
    out.push_back(static_cast<char>((mask >> 8) & 0xff));
    for (int i = 0; i < 16; ++i) {
        putF32(out, xy[i][0]);
        putF32(out, xy[i][1]);
    }
}

static std::string base64Encode(const std::string& data) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        uint32_t v = (uint32_t(uint8_t(data[i])) << 16) | (uint32_t(uint8_t(data[i + 1])) << 8) |
                     uint32_t(uint8_t(data[i + 2]));
        out.push_back(table[(v >> 18) & 63]);
        out.push_back(table[(v >> 12) & 63]);
        out.push_back(table[(v >> 6) & 63]);
        out.push_back(table[v & 63]);
    }
    size_t rest = data.size() - i;
    if (rest == 1) {
        uint32_t v = uint32_t(uint8_t(data[i])) << 16;
        out.push_back(table[(v >> 18) & 63]);
        out.push_back(table[(v >> 12) & 63]);
        out += "==";
    } else if (rest == 2) {
        uint32_t v = (uint32_t(uint8_t(data[i])) << 16) | (uint32_t(uint8_t(data[i + 1])) << 8);
        out.push_back(table[(v >> 18) & 63]);
        out.push_back(table[(v >> 12) & 63]);
        out.push_back(table[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

// Requested sampling steps (strictly ascending) and the captured result of one observe run.
struct Capture {
    std::vector<long> steps;
    size_t next = 0;
    int included = 0;
    std::string sampled;  // boards at the included steps, in step order
    StoneCoordinate last; // last observed frame (the final board)
    std::vector<StoneCoordinate>* trajectory = nullptr; // every observed frame (T mode)
};

// Returns an empty string on success, otherwise an error message. `capture` is optional.
static std::string runShot(HandleId factory, const Shot& shot, int maxSteps, bool& stopped,
                           int& steps, std::string& trajectoryDigest, std::string& finalDigest,
                           long long& simNs, Capture* capture = nullptr) {
    auto t0 = std::chrono::steady_clock::now();
    HandleId sim = 0;
    Sha256 hash;
    StoneCoordinate out;
    std::string err;
    steps = 0;
    stopped = false;
    simNs = 0;
    if (pCreateSimulator(factory, &sim) != 0) return "create_simulator: " + lastError();

    auto observe = [&]() -> bool {
        if (pGetStones(sim, &out) != 0) {
            err = "get_stones: " + lastError();
            return false;
        }
        hash.update(&out, sizeof(out));
        if (capture && capture->trajectory) capture->trajectory->push_back(out);
        // Frame number == steps executed so far; step 0 is captured before the first stopped check.
        if (capture && capture->next < capture->steps.size() &&
            capture->steps[capture->next] == steps) {
            appendBoard(capture->sampled, out);
            ++capture->included;
            ++capture->next;
        }
        bool s = false;
        if (pStopped(sim, &s) != 0) {
            err = "are_all_stones_stopped: " + lastError();
            return false;
        }
        stopped = s;
        return true;
    };

    do {
        if (pSetStones(sim, &shot.coord) != 0) {
            err = "set_stones: " + lastError();
            break;
        }
        if (!observe() || stopped) break;
        for (int i = 1; i <= maxSteps; ++i) {
            if (pStep(sim, 1, shot.sheetWidth) != 0) {
                err = "step: " + lastError();
                break;
            }
            ++steps;
            if (!observe() || stopped) break;
        }
    } while (false);

    pRemove(sim);
    simNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0).count();
    if (err.empty()) {
        if (capture) capture->last = out;
        trajectoryDigest = hash.finish();
        // The last observed frame is still in `out`.
        hash.update(&out, sizeof(out));
        finalDigest = hash.finish();
    }
    return err;
}

// ---------------------------------------------------------------- trajectory file (T mode)

static void putU32(std::string& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

// Writes <dir>/<pos>.raw atomically (temporary name, then rename). Returns an empty string on
// success, otherwise an error message; `bytes` receives the file size.
static std::string writeTrajectory(const PathString& dir, unsigned long long pos,
                                   const std::vector<StoneCoordinate>& frames, size_t& bytes) {
    uint32_t mask = 0;
    for (const StoneCoordinate& frame : frames) {
        for (int i = 0; i < 16; ++i) {
            const uint8_t* b = reinterpret_cast<const uint8_t*>(&frame.stones[i]);
            for (size_t k = 0; k < sizeof(StoneState); ++k) {
                if (b[k] != 0) {
                    mask |= 1u << i;
                    break;
                }
            }
        }
    }
    std::string data;
    putU32(data, mask);
    putU32(data, static_cast<uint32_t>(frames.size()));
    for (const StoneCoordinate& frame : frames) {
        for (int i = 0; i < 16; ++i) {
            if (mask & (1u << i)) {
                data.append(reinterpret_cast<const char*>(&frame.stones[i]), sizeof(StoneState));
            }
        }
    }
    bytes = data.size();

#ifdef _WIN32
    PathString base = dir;
    if (!base.empty() && base.back() != L'\\' && base.back() != L'/') base += L'\\';
    PathString finalPath = base + std::to_wstring(pos) + L".raw";
    PathString tempPath = finalPath + L".tmp";
    FILE* f = _wfopen(tempPath.c_str(), L"wb");
#else
    PathString base = dir;
    if (!base.empty() && base.back() != '/') base += '/';
    PathString finalPath = base + std::to_string(pos) + ".raw";
    PathString tempPath = finalPath + ".tmp";
    FILE* f = std::fopen(tempPath.c_str(), "wb");
#endif
    if (!f) return "cannot create capture file";
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (std::fflush(f) == 0) && ok;
    ok = (std::fclose(f) == 0) && ok;
    if (!ok) return "cannot write capture file";
#ifdef _WIN32
    if (!MoveFileExW(tempPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return "cannot rename capture file, error " + std::to_string(GetLastError());
    }
#else
    if (std::rename(tempPath.c_str(), finalPath.c_str()) != 0) return "cannot rename capture file";
#endif
    return std::string();
}

// ---------------------------------------------------------------- lockstep (L mode)

struct Lockstep {
    bool stoppedA = false, stoppedB = false;
    int stepsA = 0, stepsB = 0;
    long long firstBitDiff = -1;
    long long firstGt[3] = {-1, -1, -1}; // > 1e-6, > 1e-3, > 1e-2
    double maxDistance = 0.0;
    long long maxStep = -1;
    long long maxStone = -1;
    long long presenceMismatchPairs = 0;
    long long firstPresenceMismatch = -1;
    StoneCoordinate boardA, boardB;
};

static bool stonePresent(const StoneState& s) {
    const float values[6] = {s.px, s.py, s.angle, s.vx, s.vy, s.angularVelocity};
    for (float v : values) {
        if (!(std::fabs(static_cast<double>(v)) <= 1e-7)) return true;
    }
    return false;
}

static void compareFrame(Lockstep& r, long long step, const StoneCoordinate& a,
                         const StoneCoordinate& b) {
    static const double thresholds[3] = {1e-6, 1e-3, 1e-2};
    for (int i = 0; i < 16; ++i) {
        const StoneState& sa = a.stones[i];
        const StoneState& sb = b.stones[i];
        // Bit difference: any of the 24 raw bytes of any stone, regardless of presence.
        if (r.firstBitDiff < 0 && std::memcmp(&sa, &sb, sizeof(StoneState)) != 0) {
            r.firstBitDiff = step;
        }
        bool pa = stonePresent(sa), pb = stonePresent(sb);
        if (pa != pb) {
            ++r.presenceMismatchPairs;
            if (r.firstPresenceMismatch < 0) r.firstPresenceMismatch = step;
            continue;
        }
        if (!pa) continue;
        double d = std::hypot(static_cast<double>(sa.px) - static_cast<double>(sb.px),
                              static_cast<double>(sa.py) - static_cast<double>(sb.py));
        for (int t = 0; t < 3; ++t) {
            if (r.firstGt[t] < 0 && d > thresholds[t]) r.firstGt[t] = step;
        }
        if (d > r.maxDistance) {
            r.maxDistance = d;
            r.maxStep = step;
            r.maxStone = i;
        }
    }
}

// Steps both engines one frame at a time; an engine that has stopped keeps its final state.
// Returns an empty string on success, otherwise an error message.
static std::string runLockstep(HandleId factoryA, HandleId factoryB, const Shot& shot,
                               int maxSteps, Lockstep& r) {
    HandleId simA = 0, simB = 0;
    if (pCreateSimulator(factoryA, &simA) != 0) return "create_simulator: " + lastError();
    if (pCreateSimulator(factoryB, &simB) != 0) {
        std::string e = "create_simulator: " + lastError();
        pRemove(simA);
        return e;
    }
    std::string err;
    auto observe = [&](HandleId sim, StoneCoordinate& out, bool& stopped) -> bool {
        if (pGetStones(sim, &out) != 0) {
            err = "get_stones: " + lastError();
            return false;
        }
        bool s = false;
        if (pStopped(sim, &s) != 0) {
            err = "are_all_stones_stopped: " + lastError();
            return false;
        }
        stopped = s;
        return true;
    };

    do {
        if (pSetStones(simA, &shot.coord) != 0 || pSetStones(simB, &shot.coord) != 0) {
            err = "set_stones: " + lastError();
            break;
        }
        if (!observe(simA, r.boardA, r.stoppedA) || !observe(simB, r.boardB, r.stoppedB)) break;
        compareFrame(r, 0, r.boardA, r.boardB);
        for (int i = 1; i <= maxSteps && !(r.stoppedA && r.stoppedB); ++i) {
            if (!r.stoppedA) {
                if (pStep(simA, 1, shot.sheetWidth) != 0) {
                    err = "step: " + lastError();
                    break;
                }
                ++r.stepsA;
                if (!observe(simA, r.boardA, r.stoppedA)) break;
            }
            if (!r.stoppedB) {
                if (pStep(simB, 1, shot.sheetWidth) != 0) {
                    err = "step: " + lastError();
                    break;
                }
                ++r.stepsB;
                if (!observe(simB, r.boardB, r.stoppedB)) break;
            }
            compareFrame(r, i, r.boardA, r.boardB);
        }
    } while (false);

    pRemove(simA);
    pRemove(simB);
    return err;
}


// One benchmark measurement. The timed region is set_stones + simulate + the stopped check;
// simulator creation and removal are outside it. Returns an empty string on success.
static std::string benchShot(HandleId factory, const Shot& shot, long long& ns) {
    HandleId sim = 0;
    if (pCreateSimulator(factory, &sim) != 0) return "create_simulator: " + lastError();
    std::string err;
    bool stopped = false;
    auto t0 = std::chrono::steady_clock::now();
    auto t1 = t0;
    if (pSetStones(sim, &shot.coord) != 0) {
        err = "set_stones: " + lastError();
    } else if (pSimulate(sim, shot.sheetWidth) != 0) {
        err = "simulate: " + lastError();
    } else if (pStopped(sim, &stopped) != 0) {
        err = "are_all_stones_stopped: " + lastError();
    } else {
        t1 = std::chrono::steady_clock::now();
    }
    pRemove(sim);
    if (!err.empty()) return err;
    if (!stopped) return "simulation did not stop";
    ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    return std::string();
}

// Strict unsigned decimal: digits only (no sign, whitespace or suffix), at most `maxDigits`
// digits so that the value cannot overflow 64 bits.
static bool parseUnsigned(const std::string& s, size_t maxDigits, unsigned long long& value) {
    if (s.empty() || s.size() > maxDigits) return false;
    value = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<unsigned long long>(c - '0');
    }
    return true;
}

static std::vector<std::string> splitOn(const std::string& s, char sep) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        size_t at = s.find(sep, start);
        if (at == std::string::npos) {
            parts.push_back(s.substr(start));
            return parts;
        }
        parts.push_back(s.substr(start, at - start));
        start = at + 1;
    }
}

// Strictly ascending non-negative decimal steps separated by commas, or "-" for none.
static bool parseSteps(const std::string& s, std::vector<long>& steps) {
    steps.clear();
    if (s == "-") return true;
    for (const std::string& part : splitOn(s, ',')) {
        unsigned long long v = 0;
        if (!parseUnsigned(part, 9, v)) return false;
        if (!steps.empty() && static_cast<long>(v) <= steps.back()) return false;
        steps.push_back(static_cast<long>(v));
    }
    return true;
}

static int realMain(const std::vector<std::string>& args, const std::vector<PathString>& pathArgs) {
    PathString loaderPath, inputPath;
    std::string engine;
    std::vector<std::string> engines;
    bool enginesGiven = false;
    PathString captureDir;
    bool secondsPerFrameOnly = false;
    long maxSteps = -1;
    bool selfTestOnly = false;
    bool versionOnly = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        bool hasValue = i + 1 < args.size();
        if (a == "--self-test") {
            selfTestOnly = true;
        } else if (a == "--version") {
            versionOnly = true;
        } else if (a == "--loader" && hasValue) {
            loaderPath = pathArgs[++i];
        } else if (a == "--engine" && hasValue) {
            engine = args[++i];
        } else if (a == "--engines" && hasValue) {
            enginesGiven = true;
            engines = splitOn(args[++i], ',');
            for (size_t k = 0; k < engines.size(); ++k) {
                bool duplicate = false;
                for (size_t j = 0; j < k; ++j) duplicate = duplicate || engines[j] == engines[k];
                if ((engines[k] != "fcv1" && engines[k] != "fcv1plus") || duplicate) {
                    std::fprintf(stderr, "invalid --engines entry: %s\n", engines[k].c_str());
                    return 2;
                }
            }
        } else if (a == "--capture-dir" && hasValue) {
            captureDir = pathArgs[++i];
            if (captureDir.empty()) {
                std::fprintf(stderr, "invalid --capture-dir\n");
                return 2;
            }
        } else if (a == "--seconds-per-frame") {
            secondsPerFrameOnly = true;
        } else if (a == "--input" && hasValue) {
            inputPath = pathArgs[++i];
        } else if (a == "--max-steps" && hasValue) {
            // Strict: a positive decimal integer that fits in int, nothing else.
            const std::string& v = args[++i];
            char* numEnd = nullptr;
            errno = 0;
            long parsed = std::strtol(v.c_str(), &numEnd, 10);
            bool digitsOnly = !v.empty() && v.find_first_not_of("0123456789") == std::string::npos;
            if (!digitsOnly || errno != 0 || *numEnd != '\0' || parsed < 1 || parsed > INT_MAX) {
                std::fprintf(stderr, "invalid --max-steps: %s\n", v.c_str());
                return 2;
            }
            maxSteps = parsed;
        } else {
            std::fprintf(stderr, "unknown or incomplete argument: %s\n", a.c_str());
            return 2;
        }
    }

    if (!sha256SelfTest()) {
        std::fprintf(stderr, "SHA-256 self-test failed\n");
        return 2;
    }
    if (versionOnly) {
#ifdef DC_WORKER_SOURCE_SHA256
        std::printf("dc_worker %s\n", DC_WORKER_SOURCE_SHA256);
#else
        std::printf("dc_worker unknown\n");
#endif
        return 0;
    }
    if (selfTestOnly) {
        std::printf("OK\n");
        return 0;
    }
    if (secondsPerFrameOnly) {
        if (loaderPath.empty() || (engine != "fcv1" && engine != "fcv1plus")) {
            std::fprintf(stderr, "usage: dc_worker --loader PATH --engine fcv1|fcv1plus "
                                 "--seconds-per-frame\n");
            return 2;
        }
        std::string error;
        if (!loadLoader(loaderPath, error)) {
            writeError("-1", error);
            return 2;
        }
        HandleId factory = 0, sim = 0;
        if (pCreateFactory(engine.c_str(), "", &factory) != 0) {
            writeError("-1", "create_factory: " + lastError());
            return 2;
        }
        if (pCreateSimulator(factory, &sim) != 0) {
            writeError("-1", "create_simulator: " + lastError());
            return 2;
        }
        std::string capabilityError = checkStepCapability(factory, engine);
        if (!capabilityError.empty()) {
            writeError("-1", capabilityError);
            pRemove(sim);
            return 2;
        }
        float spf = 0.0f;
        int rc = pSecondsPerFrame(sim, &spf);
        if (rc != 0) {
            writeError("-1", "get_seconds_per_frame: " + lastError());
            pRemove(sim);
            return 2;
        }
        pRemove(sim);
        uint32_t bits;
        std::memcpy(&bits, &spf, sizeof(bits));
        std::printf("%08x\n", static_cast<unsigned>(bits));
        std::fflush(stdout);
        return 0;
    }
    // --engine may be omitted when --engines is given (L mode needs only --engines).
    const bool hasPrimary = !engine.empty();
    if (loaderPath.empty() || inputPath.empty() || maxSteps < 1 ||
        (hasPrimary ? (engine != "fcv1" && engine != "fcv1plus") : !enginesGiven)) {
        std::fprintf(stderr, "usage: dc_worker --loader PATH --engine fcv1|fcv1plus "
                             "--input PREPARED.bin --max-steps N [--engines fcv1,fcv1plus] "
                             "[--capture-dir DIR]\n");
        return 2;
    }
    if (!enginesGiven) engines.push_back(engine);

    std::string error;
    ShotFile shots;
    if (!shots.open(inputPath, error)) {
        writeError("-1", error);
        return 2;
    }
    if (!loadLoader(loaderPath, error)) {
        writeError("-1", error);
        return 2;
    }
    // Factories for the primary engine and every benchmark engine.
    std::vector<std::string> factoryNames;
    std::vector<HandleId> factories;
    auto addFactory = [&](const std::string& name) -> bool {
        for (const std::string& existing : factoryNames) {
            if (existing == name) return true;
        }
        HandleId id = 0;
        if (pCreateFactory(name.c_str(), "", &id) != 0) {
            writeError("-1", "create_factory: " + lastError());
            return false;
        }
        factoryNames.push_back(name);
        factories.push_back(id);
        return true;
    };
    if (hasPrimary && !addFactory(engine)) return 2;
    for (const std::string& name : engines) {
        if (!addFactory(name)) return 2;
    }
    for (size_t i = 0; i < factories.size(); ++i) {
        std::string capabilityError = checkStepCapability(factories[i], factoryNames[i]);
        if (!capabilityError.empty()) {
            writeError("-1", capabilityError);
            return 2;
        }
    }
    const HandleId factory = factories[0]; // the primary engine when --engine is given

    std::string line;
    Shot shot;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;

        // Request forms: "<pos>", "O <pos> <steps|->", "B <pos> <engine>", "T <pos>", "L <pos>".
        char mode = 'P';
        std::vector<std::string> fields = splitOn(line, ' ');
        if (!fields[0].empty() &&
            (fields[0] == "O" || fields[0] == "B" || fields[0] == "T" || fields[0] == "L")) {
            mode = fields[0][0];
        }
        const size_t expected = mode == 'P' ? 1 : ((mode == 'T' || mode == 'L') ? 2 : 3);
        // At most 19 digits cannot overflow 64 bits.
        unsigned long long pos = 0;
        const std::string& posField = mode == 'P' ? fields[0] : (fields.size() > 1 ? fields[1] : fields[0]);
        if (fields.size() != expected || !parseUnsigned(posField, 19, pos) || pos >= shots.count()) {
            writeError(mode == 'P' ? line : posField, "invalid position");
            return 2;
        }
        Capture capture;
        std::vector<StoneCoordinate> frames;
        if (mode == 'T') capture.trajectory = &frames;
        std::string benchEngine;
        if (mode == 'O') {
            if (!parseSteps(fields[2], capture.steps)) {
                writeError(fields[1], "invalid steps");
                return 2;
            }
        } else if (mode == 'B') {
            benchEngine = fields[2];
            bool known = false;
            for (const std::string& name : engines) known = known || name == benchEngine;
            if (!known) {
                writeError(fields[1], "unknown benchmark engine");
                return 2;
            }
        }
        std::string posText = std::to_string(pos);
        if (mode == 'T' && captureDir.empty()) {
            writeError(posText, "T mode requires --capture-dir");
            return 2;
        }
        size_t indexA = 0, indexB = 0;
        if (mode == 'L') {
            bool pair = enginesGiven && engines.size() == 2 && engines[0] == "fcv1" &&
                        engines[1] == "fcv1plus";
            if (!pair) {
                writeError(posText, "L mode requires --engines fcv1,fcv1plus");
                return 2;
            }
            while (factoryNames[indexA] != "fcv1") ++indexA;
            while (factoryNames[indexB] != "fcv1plus") ++indexB;
        } else if (mode != 'B' && !hasPrimary) {
            writeError(posText, "this mode requires --engine");
            return 2;
        }
        if (!shots.read(pos, shot, error)) {
            writeError(posText, error);
            return 2;
        }
        if (mode == 'L') {
            Lockstep result;
            std::string err = runLockstep(factories[indexA], factories[indexB], shot,
                                          static_cast<int>(maxSteps), result);
            if (!err.empty()) {
                writeError(posText, err);
                return 2;
            }
            std::string boards;
            appendBoard(boards, result.boardA);
            appendBoard(boards, result.boardB);
            std::printf("L %llu %s %d %s %d %lld %lld %lld %lld %.17g %lld %lld %lld %lld %s\n", pos,
                        result.stoppedA ? "stopped" : "truncated", result.stepsA,
                        result.stoppedB ? "stopped" : "truncated", result.stepsB,
                        result.firstBitDiff, result.firstGt[0], result.firstGt[1],
                        result.firstGt[2], result.maxDistance, result.maxStep, result.maxStone,
                        result.presenceMismatchPairs, result.firstPresenceMismatch,
                        base64Encode(boards).c_str());
            std::fflush(stdout);
            continue;
        }
        if (mode == 'B') {
            size_t index = 0;
            while (factoryNames[index] != benchEngine) ++index;
            long long ns = 0;
            std::string err = benchShot(factories[index], shot, ns);
            if (!err.empty()) {
                writeError(posText, err);
                return 2;
            }
            std::printf("B %llu %s %lld\n", pos, benchEngine.c_str(), ns);
            std::fflush(stdout);
            continue;
        }
        bool stopped = false;
        int steps = 0;
        long long simNs = 0;
        std::string trajectoryDigest, finalDigest;
        std::string err = runShot(factory, shot, static_cast<int>(maxSteps), stopped, steps,
                                  trajectoryDigest, finalDigest, simNs,
                                  (mode == 'O' || mode == 'T') ? &capture : nullptr);
        if (!err.empty()) {
            writeError(posText, err);
            return 2;
        }
        size_t rawBytes = 0;
        if (mode == 'T') {
            err = writeTrajectory(captureDir, pos, frames, rawBytes);
            if (!err.empty()) {
                writeError(posText, err);
                return 2;
            }
        }
        std::printf("%llu %s %d %s %s %lld\n", pos, stopped ? "stopped" : "truncated", steps,
                    trajectoryDigest.c_str(), finalDigest.c_str(), simNs);
        if (mode == 'T') std::printf("T %llu %zu\n", pos, rawBytes);
        if (mode == 'O') {
            std::string payload;
            payload.reserve(kBoardBytes * (1 + static_cast<size_t>(capture.included)));
            appendBoard(payload, capture.last);
            payload += capture.sampled;
            std::printf("S %llu %d %s\n", pos, capture.included, base64Encode(payload).c_str());
        }
        std::fflush(stdout);
    }
    return 0;
}

int main(int argc, char** argv) {
    std::vector<std::string> args;
    std::vector<PathString> pathArgs;
#ifdef _WIN32
    // Use the wide command line so non-ASCII paths survive; the ANSI argv is ignored.
    (void)argc;
    (void)argv;
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv) {
        std::fprintf(stderr, "cannot read command line\n");
        return 2;
    }
    for (int i = 1; i < wargc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string u8(n > 1 ? n - 1 : 0, '\0');
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &u8[0], n, nullptr, nullptr);
        args.push_back(u8);
        pathArgs.push_back(wargv[i]);
    }
    LocalFree(wargv);
#else
    for (int i = 1; i < argc; ++i) {
        args.push_back(argv[i]);
        pathArgs.push_back(argv[i]);
    }
#endif
    return realMain(args, pathArgs);
}
