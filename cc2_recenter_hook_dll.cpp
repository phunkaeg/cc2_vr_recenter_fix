#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>

namespace {

constexpr uintptr_t kRvaApplyVrRecenterTransform = 0x000F2070; // 0x004F2070 - 0x00400000
constexpr uintptr_t kRvaGetVrViewState          = 0x000F0160; // 0x004F0160 - 0x00400000
constexpr size_t    kHookPatchBytes             = 6;

struct Config {
    bool enabled = true;
    bool remove_pitch = true;
    bool remove_roll = true;
    bool remove_yaw = false;          // leave off
    bool use_world_up = true;
    bool recompute_translation = true;
    bool log_matrix = false;          // optional logging via env var path only
    float offset_x = 0.0f;
    float offset_y = 1.55f;
    float offset_z = 0.0f;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Basis {
    Vec3 right;
    Vec3 up;
    Vec3 forward;
    Vec3 position;
};

using ApplyVrRecenterTransformFn = void(__fastcall*)(float* param1);
using GetVrViewStateFn = void* (__cdecl*)();

HMODULE g_self = nullptr;
ApplyVrRecenterTransformFn g_originalApplyVrRecenterTransform = nullptr;
GetVrViewStateFn g_getVrViewState = nullptr;
void* g_trampoline = nullptr;
FILE* g_log = nullptr;
CRITICAL_SECTION g_logCs;
volatile bool g_logCsInit = false;
Config g_cfg{};

void Log(const char* fmt, ...);

inline uint8_t* ExeBase() {
    return reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
}

inline bool IsNearlyZero(float v, float eps = 1e-5f) {
    return std::fabs(v) < eps;
}

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator/(const Vec3& a, float s) { return {a.x / s, a.y / s, a.z / s}; }

inline float Dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

inline float Length(const Vec3& v) {
    return std::sqrt(Dot(v, v));
}

inline Vec3 Normalize(const Vec3& v) {
    const float len = Length(v);
    if (IsNearlyZero(len)) return {0.0f, 0.0f, 0.0f};
    return v / len;
}

inline Vec3 ProjectOnPlane(const Vec3& v, const Vec3& planeNormalUnit) {
    return v - planeNormalUnit * Dot(v, planeNormalUnit);
}

inline Basis ReadBasis(const float* p) {
    Basis b{};
    b.right   = {p[0], p[1], p[2]};
    b.up      = {p[3], p[4], p[5]};
    b.forward = {p[6], p[7], p[8]};
    b.position= {p[9], p[10], p[11]};
    return b;
}

inline void WriteBasis(float* p, const Basis& b) {
    p[0]  = b.right.x;   p[1]  = b.right.y;   p[2]  = b.right.z;
    p[3]  = b.up.x;      p[4]  = b.up.y;      p[5]  = b.up.z;
    p[6]  = b.forward.x; p[7]  = b.forward.y; p[8]  = b.forward.z;
    p[9]  = b.position.x;p[10] = b.position.y;p[11] = b.position.z;
}

Vec3 TransformOffsetByBasis(const Basis& b, const Vec3& local) {
    return {
        local.x * b.right.x   + local.y * b.up.x   + local.z * b.forward.x,
        local.x * b.right.y   + local.y * b.up.y   + local.z * b.forward.y,
        local.x * b.right.z   + local.y * b.up.z   + local.z * b.forward.z
    };
}

Basis CleanBasis(const Basis& src, const Config& cfg) {
    Basis out = src;

    Vec3 upRef = cfg.use_world_up ? Vec3{0.0f, 1.0f, 0.0f} : Normalize(src.up);
    if (Length(upRef) < 0.5f) {
        upRef = {0.0f, 1.0f, 0.0f};
    }

    Vec3 right = Normalize(src.right);
    Vec3 up = Normalize(src.up);
    Vec3 fwd = Normalize(src.forward);

    if (cfg.remove_yaw) {
        const float horiz = std::sqrt(std::max(0.0f, 1.0f - fwd.y * fwd.y));
        fwd = Normalize({0.0f, fwd.y, (fwd.z >= 0.0f ? horiz : -horiz)});
    }

    if (cfg.remove_roll && !cfg.remove_pitch) {
        Vec3 newRight = Normalize(Cross(upRef, fwd));
        if (Length(newRight) > 0.5f) {
            if (Dot(newRight, right) < 0.0f) newRight = newRight * -1.0f;
            Vec3 newUp = Normalize(Cross(fwd, newRight));
            if (Dot(newUp, upRef) < 0.0f) {
                newRight = newRight * -1.0f;
                newUp = newUp * -1.0f;
            }
            right = newRight;
            up = newUp;
            fwd = Normalize(Cross(right, up));
        }
    }

    if (cfg.remove_pitch && !cfg.remove_roll) {
        Vec3 newFwd = Normalize(ProjectOnPlane(fwd, upRef));
        if (Length(newFwd) > 0.5f) {
            if (Dot(newFwd, fwd) < 0.0f) newFwd = newFwd * -1.0f;
            Vec3 newUp = Normalize(ProjectOnPlane(up, newFwd));
            if (Length(newUp) < 0.5f) {
                newUp = upRef;
            }
            Vec3 newRight = Normalize(Cross(newUp, newFwd));
            if (Dot(newRight, right) < 0.0f) newRight = newRight * -1.0f;
            newUp = Normalize(Cross(newFwd, newRight));
            right = newRight;
            up = newUp;
            fwd = newFwd;
        }
    }

    if (cfg.remove_pitch && cfg.remove_roll) {
        Vec3 newFwd = Normalize(ProjectOnPlane(fwd, upRef));
        if (Length(newFwd) > 0.5f) {
            if (Dot(newFwd, fwd) < 0.0f) newFwd = newFwd * -1.0f;
            Vec3 newRight = Normalize(Cross(upRef, newFwd));
            if (Dot(newRight, right) < 0.0f) newRight = newRight * -1.0f;
            Vec3 newUp = Normalize(Cross(newFwd, newRight));
            if (Dot(newUp, upRef) < 0.0f) {
                newRight = newRight * -1.0f;
                newUp = newUp * -1.0f;
            }
            right = newRight;
            up = newUp;
            fwd = newFwd;
        }
    }

    out.right = Normalize(right);
    out.up = Normalize(up);
    out.forward = Normalize(fwd);
    return out;
}

bool MakeWritable(void* addr, size_t size, DWORD* oldProtect) {
    return VirtualProtect(addr, size, PAGE_EXECUTE_READWRITE, oldProtect) != 0;
}

void* BuildPushRetTrampoline(void* returnAddress, const uint8_t* stolenBytes, size_t stolenLen) {
    uint8_t* tramp = reinterpret_cast<uint8_t*>(VirtualAlloc(nullptr, stolenLen + 6, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return nullptr;

    std::memcpy(tramp, stolenBytes, stolenLen);
    tramp[stolenLen + 0] = 0x68;
    *reinterpret_cast<uint32_t*>(tramp + stolenLen + 1) = reinterpret_cast<uint32_t>(returnAddress);
    tramp[stolenLen + 5] = 0xC3;
    return tramp;
}

bool InstallPushRetHook(void* target, void* detour, size_t stolenLen, void** outTrampoline) {
    if (stolenLen < 6) return false;

    uint8_t original[16]{};
    std::memcpy(original, target, stolenLen);

    void* trampoline = BuildPushRetTrampoline(reinterpret_cast<uint8_t*>(target) + stolenLen, original, stolenLen);
    if (!trampoline) return false;

    DWORD oldProtect = 0;
    if (!MakeWritable(target, stolenLen, &oldProtect)) return false;

    uint8_t patch[16]{};
    patch[0] = 0x68;
    *reinterpret_cast<uint32_t*>(patch + 1) = reinterpret_cast<uint32_t>(detour);
    patch[5] = 0xC3;
    for (size_t i = 6; i < stolenLen; ++i) patch[i] = 0x90;

    std::memcpy(target, patch, stolenLen);
    FlushInstructionCache(GetCurrentProcess(), target, stolenLen);

    DWORD dummy = 0;
    VirtualProtect(target, stolenLen, oldProtect, &dummy);

    *outTrampoline = trampoline;
    return true;
}

std::string GetLogPath() {
    char buf[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableA("CC2_VR_HACK_LOG", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return std::string(buf);
    }

    char dllPath[MAX_PATH]{};
    GetModuleFileNameA(g_self, dllPath, MAX_PATH);
    std::string path = dllPath;
    const size_t slash = path.find_last_of("\\/");
    if (slash != std::string::npos) path.resize(slash + 1);
    path += "cc2_recenter_hook.log";
    return path;
}

void InitLoggingIfRequested() {
    char buf[16]{};
    const DWORD n = GetEnvironmentVariableA("CC2_VR_HACK_LOG_ENABLE", buf, sizeof(buf));
    if (n == 0) return;
    if (buf[0] == '0') return;

    g_cfg.log_matrix = true;
    if (!g_logCsInit) {
        InitializeCriticalSection(&g_logCs);
        g_logCsInit = true;
    }
    const std::string logPath = GetLogPath();
    fopen_s(&g_log, logPath.c_str(), "a");
    if (g_log) {
        std::fprintf(g_log, "\n--- baked hook started ---\n");
        std::fflush(g_log);
    }
}

void Log(const char* fmt, ...) {
    if (!g_log) return;
    if (g_logCsInit) EnterCriticalSection(&g_logCs);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_log, fmt, args);
    std::fprintf(g_log, "\n");
    std::fflush(g_log);
    va_end(args);
    if (g_logCsInit) LeaveCriticalSection(&g_logCs);
}

void MaybePatchRecenterOutput(const Config& cfg) {
    if (!cfg.enabled || !g_getVrViewState) return;

    auto* state = reinterpret_cast<uint8_t*>(g_getVrViewState());
    if (!state) return;

    float* current = reinterpret_cast<float*>(state + 0x50);
    float* output  = reinterpret_cast<float*>(state + 0x80);

    Basis src = ReadBasis(current);
    Basis out = ReadBasis(output);
    Basis cleaned = CleanBasis(src, cfg);

    out.right = cleaned.right;
    out.up = cleaned.up;
    out.forward = cleaned.forward;

    if (cfg.recompute_translation) {
        const Vec3 offset{cfg.offset_x, cfg.offset_y, cfg.offset_z};
        out.position = src.position + TransformOffsetByBasis(cleaned, offset * -1.0f);
    }

    WriteBasis(output, out);

    if (cfg.log_matrix) {
        Log("src basis R=(%.4f %.4f %.4f) U=(%.4f %.4f %.4f) F=(%.4f %.4f %.4f) P=(%.4f %.4f %.4f)",
            src.right.x, src.right.y, src.right.z,
            src.up.x, src.up.y, src.up.z,
            src.forward.x, src.forward.y, src.forward.z,
            src.position.x, src.position.y, src.position.z);
        Log("patched basis R=(%.4f %.4f %.4f) U=(%.4f %.4f %.4f) F=(%.4f %.4f %.4f) P=(%.4f %.4f %.4f)",
            out.right.x, out.right.y, out.right.z,
            out.up.x, out.up.y, out.up.z,
            out.forward.x, out.forward.y, out.forward.z,
            out.position.x, out.position.y, out.position.z);
    }
}

void __fastcall HookedApplyVrRecenterTransform(float* param1) {
    if (g_originalApplyVrRecenterTransform) {
        g_originalApplyVrRecenterTransform(param1);
    }
    MaybePatchRecenterOutput(g_cfg);
}

DWORD WINAPI InitThread(LPVOID) {
    InitLoggingIfRequested();
    const auto base = reinterpret_cast<uintptr_t>(ExeBase());
    g_getVrViewState = reinterpret_cast<GetVrViewStateFn>(base + kRvaGetVrViewState);

    void* target = reinterpret_cast<void*>(base + kRvaApplyVrRecenterTransform);
    if (InstallPushRetHook(target, reinterpret_cast<void*>(&HookedApplyVrRecenterTransform), kHookPatchBytes, &g_trampoline)) {
        g_originalApplyVrRecenterTransform = reinterpret_cast<ApplyVrRecenterTransformFn>(g_trampoline);
        Log("Installed baked recenter hook at %p; trampoline=%p; get_vr_view_state=%p", target, g_trampoline, reinterpret_cast<void*>(g_getVrViewState));
    } else {
        Log("Failed to install baked recenter hook");
    }
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = hModule;
        DisableThreadLibraryCalls(hModule);
        HANDLE thread = CreateThread(nullptr, 0, &InitThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_log) {
            std::fclose(g_log);
            g_log = nullptr;
        }
        if (g_logCsInit) {
            DeleteCriticalSection(&g_logCs);
            g_logCsInit = false;
        }
    }
    return TRUE;
}
