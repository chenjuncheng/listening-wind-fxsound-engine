/*
 * fxsound_engine_main.cpp
 * 
 * FxSound Engine v2.0 - Standalone command-line audio enhancer
 * 
 * Usage:
 *   fxsound_engine.exe --preset <path> [--device <name>] [--device-index <n>]
 *                      [--fidelity <0-10>] [--ambience <0-10>] [--surround <0-10>]
 *                      [--dynamic-boost <0-10>] [--bass <0-10>]
 *                      [--buffer <ms>] [--list-devices] [--check-driver]
 *   fxsound_engine.exe --install-driver [driver_dir]
 *   fxsound_engine.exe --uninstall-driver
 *   fxsound_engine.exe --restore-default
 *   fxsound_engine.exe --list-crash-dumps
 * 
 * Architecture (Virtual Device Mode):
 *   System default = Virtual Device (fxvad.sys) → WASAPI Loopback Capture
 *   → DfxDsp Processing → WASAPI Render to Real Playback Device
 * 
 * Flutter Integration:
 *   Process.start("fxsound_engine.exe", ["--preset", "fps.fac"])
 *   stdout: JSON status messages
 *   Process.kill() or SIGTERM: Clean shutdown (restores default device)
 * 
 * Driver Management:
 *   --install-driver:  Install fxvad.sys virtual audio device (requires admin)
 *   --uninstall-driver: Remove fxvad.sys (requires admin)
 *   --check-driver:    Check if virtual device is installed and enabled
 *   --restore-default: Restore default playback device from recovery file
 *   --list-crash-dumps: List crash dump files for debugging
 * 
 * License: AGPL-3.0 (inherits from FxSound source)
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <csignal>
#include <new>
#include <chrono>
#include <thread>
#include <atomic>

// Windows headers for non-blocking stdin
#include <windows.h>

// Windows audio APIs for default device save/restore
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>

// FxSound headers
#include "codedefs.h"
#include "DfxDsp.h"
#include "AudioPassthru.h"

// IPolicyConfig for setting default endpoint (requires mmreg.h via Audioclient.h)
#include "PolicyConfig.h"

// Driver management
#include "driver_manager.h"

// Crash handler
#include "crash_handler.h"

// Performance instrumentation for benchmarking
#include "perf_instrumentation.h"

// ============================================================================
// Global state for signal handling
// ============================================================================
static volatile bool g_running = true;
static DfxDsp*       g_dsp = nullptr;
static AudioPassthru* g_passthru = nullptr;

// Performance instrumentation sampler for processTimer() benchmarking
static PerfSampler g_perfSampler;

// Saved default playback device (before engine modifies it)
static std::wstring  g_original_default_device_id;
static std::wstring  g_original_default_device_name;

// Realtime command processing state
static std::atomic<bool> g_command_pending{false};
static std::string g_command_buffer;

// Effect name to enum mapping for realtime commands
static DfxDsp::Effect effectNameToEnum(const std::string& name) {
    if (name == "fidelity") return DfxDsp::Fidelity;
    if (name == "ambience") return DfxDsp::Ambience;
    if (name == "surround") return DfxDsp::Surround;
    if (name == "dynamic_boost") return DfxDsp::DynamicBoost;
    if (name == "bass") return DfxDsp::Bass;
    return DfxDsp::NumEffects; // Invalid
}

static const char* effectEnumToName(DfxDsp::Effect effect) {
    switch (effect) {
        case DfxDsp::Fidelity: return "fidelity";
        case DfxDsp::Ambience: return "ambience";
        case DfxDsp::Surround: return "surround";
        case DfxDsp::DynamicBoost: return "dynamic_boost";
        case DfxDsp::Bass: return "bass";
        default: return "unknown";
    }
}

// Forward declarations (defined after wide_to_utf8)
static bool saveDefaultPlaybackDevice();
static bool restoreDefaultPlaybackDevice();
static void processRealtimeCommand(const std::string& cmd);

// ============================================================================
// Signal handler for graceful shutdown
// Uses both signal() and SetConsoleCtrlHandler() for maximum coverage on Windows.
// - signal(SIGINT/SIGTERM): catches Ctrl+C from terminal
// - SetConsoleCtrlHandler: catches Ctrl+C, Ctrl+Break, console close, logoff, shutdown
// ============================================================================
static void signal_handler(int sig) {
    fprintf(stderr, "[FxSound Engine] Received signal %d, shutting down...\n", sig);
    g_running = false;
}

static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            fprintf(stderr, "[FxSound Engine] Console ctrl event %lu, shutting down...\n", ctrl_type);
            g_running = false;
            return TRUE;  // We handled it
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            fprintf(stderr, "[FxSound Engine] System shutdown event %lu, cleaning up...\n", ctrl_type);
            g_running = false;
            // For close/logoff/shutdown, we have limited time — do restore immediately
            if (!g_original_default_device_id.empty()) {
                restoreDefaultPlaybackDevice();
            }
            return TRUE;
        default:
            return FALSE;
    }
}

// ============================================================================
// AudioPassthru callback - receives device change notifications
// ============================================================================
class EngineCallback : public AudioPassthruCallback {
public:
    void onSoundDeviceChange(std::vector<SoundDevice> sound_devices) override {
        // Called from processTimer() after reinit — thread died and was restarted
        // Debounce: only emit once per second at most
        static auto last_emit = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_emit).count() >= 1) {
            fprintf(stdout, "{\"type\":\"device_change\",\"devices\":%zu}\n", sound_devices.size());
            fflush(stdout);
            last_emit = now;
        }
    }
    
    void onSoundDeviceChange() override {
        // Called from WASAPI deviceChangeCallback — a device event happened
        // Debounce: only emit once per second at most
        static auto last_emit = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_emit).count() >= 1) {
            fprintf(stdout, "{\"type\":\"device_change\"}\n");
            fflush(stdout);
            last_emit = now;
        }
    }
};

// ============================================================================
// Command line argument parsing
// ============================================================================
enum class RunMode {
    NORMAL,           // Normal audio processing mode
    LIST_DEVICES,     // Just list devices and exit
    INSTALL_DRIVER,   // Install virtual audio driver and exit
    UNINSTALL_DRIVER, // Uninstall virtual audio driver and exit
    CHECK_DRIVER,     // Check driver status and exit
    RESTORE_DEFAULT,  // Restore default playback device from recovery file
    LIST_CRASH_DUMPS, // List crash dump files and exit
};

struct EngineConfig {
    RunMode mode = RunMode::NORMAL;
    std::wstring preset_path;
    std::wstring device_name;
    std::wstring driver_dir;      // Optional: override driver directory for --install-driver
    int device_index = -1;        // -1 means auto-select
    float fidelity = -1.0f;       // -1 means use preset default
    float ambience = -1.0f;
    float surround = -1.0f;
    float dynamic_boost = -1.0f;
    float bass = -1.0f;
    int buffer_ms = 40;           // Default buffer size in ms
    bool analyze_mode = false;    // Capture-only mode, no playback (for testing)
};

static std::wstring utf8_to_wide(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (size <= 0) return std::wstring();
    std::wstring result(size - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &result[0], size);
    return result;
}

static std::string wide_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return std::string();
    std::string result(size - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &result[0], size, nullptr, nullptr);
    return result;
}

// Escape a string for safe JSON embedding
static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// ============================================================================
// Realtime command processing via stdin
// Flutter sends JSON commands like: {"cmd":"set","effect":"fidelity","value":9}
// ============================================================================

// Simple JSON parser for command objects (sufficient for our simple format)
static bool parseCommandJson(const std::string& json, std::string& out_cmd, std::string& out_effect, float& out_value) {
    // Very simple parser: look for "cmd":"set", "effect":"xxx", "value":N
    out_cmd.clear();
    out_effect.clear();
    out_value = -1.0f;
    
    // Find cmd
    size_t cmd_pos = json.find("\"cmd\"");
    if (cmd_pos != std::string::npos) {
        size_t colon = json.find(':', cmd_pos);
        if (colon != std::string::npos) {
            size_t q1 = json.find('"', colon);
            if (q1 != std::string::npos) {
                size_t q2 = json.find('"', q1 + 1);
                if (q2 != std::string::npos) {
                    out_cmd = json.substr(q1 + 1, q2 - q1 - 1);
                }
            }
        }
    }
    
    // Find effect
    size_t eff_pos = json.find("\"effect\"");
    if (eff_pos != std::string::npos) {
        size_t colon = json.find(':', eff_pos);
        if (colon != std::string::npos) {
            size_t q1 = json.find('"', colon);
            if (q1 != std::string::npos) {
                size_t q2 = json.find('"', q1 + 1);
                if (q2 != std::string::npos) {
                    out_effect = json.substr(q1 + 1, q2 - q1 - 1);
                }
            }
        }
    }
    
    // Find value
    size_t val_pos = json.find("\"value\"");
    if (val_pos != std::string::npos) {
        size_t colon = json.find(':', val_pos);
        if (colon != std::string::npos) {
            // Skip whitespace
            size_t start = colon + 1;
            while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) start++;
            try {
                out_value = std::stof(json.substr(start));
            } catch (...) {
                out_value = -1.0f;
            }
        }
    }
    
    return !out_cmd.empty();
}

static void processRealtimeCommand(const std::string& cmd_line) {
    std::string cmd, effect_name;
    float value = -1.0f;
    
    if (!parseCommandJson(cmd_line, cmd, effect_name, value)) {
        fprintf(stderr, "[FxSound Engine] Invalid command JSON: %s\n", cmd_line.c_str());
        printf("{\"type\":\"command_error\",\"message\":\"invalid json\"}\n");
        fflush(stdout);
        return;
    }
    
    if (cmd == "set") {
        DfxDsp::Effect effect = effectNameToEnum(effect_name);
        if (effect == DfxDsp::NumEffects) {
            printf("{\"type\":\"command_error\",\"message\":\"unknown effect: %s\"}\n", json_escape(effect_name).c_str());
            fflush(stdout);
            return;
        }
        
        if (value < 0.0f || value > 10.0f) {
            printf("{\"type\":\"command_error\",\"message\":\"value out of range: %.1f\"}\n", value);
            fflush(stdout);
            return;
        }
        
        if (g_dsp) {
            g_dsp->setEffectValue(effect, value);
            printf("{\"type\":\"effect_updated\",\"effect\":\"%s\",\"value\":%.1f}\n", 
                   effectEnumToName(effect), value);
            fflush(stdout);
            fprintf(stderr, "[FxSound Engine] Realtime update: %s = %.1f\n", effectEnumToName(effect), value);
        } else {
            printf("{\"type\":\"command_error\",\"message\":\"dsp not initialized\"}\n");
            fflush(stdout);
        }
    } else if (cmd == "dump") {
        // Self-diagnostic: dump current DSP state for cross-process verification
        if (g_dsp) {
            printf("{\"type\":\"state_dump\"");
            printf(",\"fidelity\":%.1f", g_dsp->getEffectValue(DfxDsp::Fidelity));
            printf(",\"ambience\":%.1f", g_dsp->getEffectValue(DfxDsp::Ambience));
            printf(",\"surround\":%.1f", g_dsp->getEffectValue(DfxDsp::Surround));
            printf(",\"dynamic_boost\":%.1f", g_dsp->getEffectValue(DfxDsp::DynamicBoost));
            printf(",\"bass\":%.1f", g_dsp->getEffectValue(DfxDsp::Bass));
            printf(",\"power_on\":%s", g_dsp->isPowerOn() ? "true" : "false");
            printf("}\n");
            fflush(stdout);
            fprintf(stderr, "[FxSound Engine] State dump: fidelity=%.1f ambience=%.1f surround=%.1f dynamic_boost=%.1f bass=%.1f power=%s\n",
                    g_dsp->getEffectValue(DfxDsp::Fidelity),
                    g_dsp->getEffectValue(DfxDsp::Ambience),
                    g_dsp->getEffectValue(DfxDsp::Surround),
                    g_dsp->getEffectValue(DfxDsp::DynamicBoost),
                    g_dsp->getEffectValue(DfxDsp::Bass),
                    g_dsp->isPowerOn() ? "on" : "off");
        } else {
            printf("{\"type\":\"command_error\",\"message\":\"dsp not initialized\"}\n");
            fflush(stdout);
        }
    } else if (cmd == "eq") {
        // Return Graphic EQ band data (frequency & boost/cut) from the loaded preset
        if (g_dsp) {
            int num_bands = g_dsp->getNumEqBands();
            printf("{\"type\":\"eq_data\",\"bands\":");
            printf("[");
            for (int i = 0; i < num_bands; i++) {
                float freq = g_dsp->getEqBandFrequency(i);
                float boost = g_dsp->getEqBandBoostCut(i);
                if (i > 0) printf(",");
                printf("{\"freq\":%.1f,\"boost\":%.2f}", freq, boost);
            }
            printf("]}\n");
            fflush(stdout);
            fprintf(stderr, "[FxSound Engine] EQ data: %d bands returned\n", num_bands);
        } else {
            printf("{\"type\":\"command_error\",\"message\":\"dsp not initialized\"}\n");
            fflush(stdout);
        }
    } else if (cmd == "shutdown") {
        // Graceful shutdown request from Flutter — set g_running to false
        // so the main loop exits cleanly and cleanup code runs.
        printf("{\"type\":\"shutdown_ack\"}\n");
        fflush(stdout);
        fprintf(stderr, "[FxSound Engine] Shutdown command received, stopping...\n");
        g_running = false;
    } else if (cmd == "ping") {
        printf("{\"type\":\"pong\"}\n");
        fflush(stdout);
    } else {
        printf("{\"type\":\"command_error\",\"message\":\"unknown command: %s\"}\n", json_escape(cmd).c_str());
        fflush(stdout);
    }
}

// Check stdin for pending commands (non-blocking, works with pipes)
static void checkStdinForCommands() {
    static HANDLE hStdin = INVALID_HANDLE_VALUE;
    static std::string buffer;
    
    if (hStdin == INVALID_HANDLE_VALUE) {
        hStdin = GetStdHandle(STD_INPUT_HANDLE);
    }
    
    if (hStdin == INVALID_HANDLE_VALUE || hStdin == NULL) {
        return;
    }
    
    // Check if stdin is a pipe (from parent process) or console
    DWORD avail = 0;
    if (!PeekNamedPipe(hStdin, NULL, 0, NULL, &avail, NULL)) {
        // Not a pipe, might be console — try console input check
        return;
    }
    
    if (avail == 0) {
        return; // No data available
    }
    
    // Read available data
    char temp[512];
    DWORD read = 0;
    DWORD to_read = (avail < sizeof(temp) - 1) ? avail : sizeof(temp) - 1;
    
    if (!ReadFile(hStdin, temp, to_read, &read, NULL) || read == 0) {
        return;
    }
    temp[read] = '\0';
    buffer += temp;
    
    // Process complete lines (commands are one JSON object per line)
    size_t pos;
    while ((pos = buffer.find('\n')) != std::string::npos) {
        std::string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 1);
        
        // Remove trailing \r if present
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        
        if (!line.empty()) {
            processRealtimeCommand(line);
        }
    }
    
    // Prevent buffer from growing unbounded if no newline ever arrives
    if (buffer.size() > 4096) {
        buffer.clear();
    }
}

// ============================================================================
// Save and restore default playback device
// Since NO_REGISTRY disables sndDevicesRestoreDefaultDevice, we must handle
// environment recovery ourselves: save the default device ID before the engine
// changes it, and restore it on exit.
// ============================================================================

// Save the current default audio playback device ID and friendly name.
// Must be called BEFORE AudioPassthru::init() which changes the default device.
// If the current default is the FxSound virtual device (leftover from a previous crash),
// we skip it and find the first real playback device instead.
static bool saveDefaultPlaybackDevice() {
    IMMDeviceEnumerator* pEnumerator = nullptr;
    IMMDevice* pDevice = nullptr;
    
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr)) {
        fprintf(stderr, "[FxSound Engine] Warning: Failed to create device enumerator (0x%08X)\n", hr);
        return false;
    }
    
    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    if (FAILED(hr) || pDevice == nullptr) {
        fprintf(stderr, "[FxSound Engine] Warning: No default playback device found\n");
        pEnumerator->Release();
        return false;
    }
    
    // Get device ID and friendly name
    LPWSTR pwszID = nullptr;
    std::wstring device_id;
    std::wstring device_name;
    
    hr = pDevice->GetId(&pwszID);
    if (SUCCEEDED(hr) && pwszID) {
        device_id = pwszID;
        CoTaskMemFree(pwszID);
    }
    
    IPropertyStore* pProps = nullptr;
    hr = pDevice->OpenPropertyStore(STGM_READ, &pProps);
    if (SUCCEEDED(hr) && pProps) {
        PROPVARIANT varName;
        PropVariantInit(&varName);
        hr = pProps->GetValue(PKEY_Device_FriendlyName, &varName);
        if (SUCCEEDED(hr) && varName.pwszVal) {
            device_name = varName.pwszVal;
        }
        PropVariantClear(&varName);
        pProps->Release();
    }
    pDevice->Release();
    
    // Check if the default device is the FxSound virtual device.
    // If so, we need to find the first real playback device instead.
    bool is_dfx_device = (device_name.find(L"FxSound") != std::wstring::npos);
    
    if (is_dfx_device) {
        fprintf(stderr, "[FxSound Engine] Default device is FxSound virtual device, searching for real device...\n");
        
        IMMDeviceCollection* pCollection = nullptr;
        hr = pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
        if (SUCCEEDED(hr) && pCollection) {
            UINT count = 0;
            pCollection->GetCount(&count);
            for (UINT i = 0; i < count; i++) {
                IMMDevice* pDev = nullptr;
                if (SUCCEEDED(pCollection->Item(i, &pDev)) && pDev) {
                    LPWSTR pID = nullptr;
                    std::wstring dev_id, dev_name;
                    pDev->GetId(&pID);
                    if (pID) { dev_id = pID; CoTaskMemFree(pID); }
                    
                    IPropertyStore* pP = nullptr;
                    if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pP)) && pP) {
                        PROPVARIANT vn; PropVariantInit(&vn);
                        pP->GetValue(PKEY_Device_FriendlyName, &vn);
                        if (vn.pwszVal) dev_name = vn.pwszVal;
                        PropVariantClear(&vn);
                        pP->Release();
                    }
                    pDev->Release();
                    
                    // Skip FxSound virtual device — we want a real device
                    if (dev_name.find(L"FxSound") != std::wstring::npos) continue;
                    
                    // Found a real device
                    device_id = dev_id;
                    device_name = dev_name;
                    is_dfx_device = false;
                    break;
                }
            }
            pCollection->Release();
        }
    }
    
    pEnumerator->Release();
    
    if (!device_id.empty() && !is_dfx_device) {
        g_original_default_device_id = device_id;
        g_original_default_device_name = device_name;
        std::string name_str = wide_to_utf8(g_original_default_device_name);
        fprintf(stderr, "[FxSound Engine] Saved original device: %s\n", name_str.c_str());
        return true;
    }
    
    fprintf(stderr, "[FxSound Engine] Warning: Could not find a real playback device to save\n");
    return false;
}

// Restore the default audio playback device to the one saved before engine start.
// Uses IPolicyConfigVista::SetDefaultEndpoint (same API FxSound uses internally).
static bool restoreDefaultPlaybackDevice() {
    if (g_original_default_device_id.empty()) {
        fprintf(stderr, "[FxSound Engine] No saved default device to restore\n");
        return false;
    }

    HRESULT com_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    const bool should_uninit_com = (com_hr == S_OK || com_hr == S_FALSE);
    if (FAILED(com_hr) && com_hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[FxSound Engine] Failed to initialize COM for restore (0x%08X)\n", com_hr);
        return false;
    }
    
    IPolicyConfigVista* pPolicyConfig = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(CPolicyConfigVistaClient), nullptr, CLSCTX_ALL,
                                  __uuidof(IPolicyConfigVista), (LPVOID*)&pPolicyConfig);
    if (FAILED(hr)) {
        fprintf(stderr, "[FxSound Engine] Failed to create PolicyConfig for restore (0x%08X)\n", hr);
        if (should_uninit_com) CoUninitialize();
        return false;
    }
    
    // Set default for all roles (console, multimedia, communications)
    hr = S_OK;
    hr = pPolicyConfig->SetDefaultEndpoint(g_original_default_device_id.c_str(), eConsole);
    if (SUCCEEDED(hr)) {
        hr = pPolicyConfig->SetDefaultEndpoint(g_original_default_device_id.c_str(), eMultimedia);
    }
    if (SUCCEEDED(hr)) {
        hr = pPolicyConfig->SetDefaultEndpoint(g_original_default_device_id.c_str(), eCommunications);
    }
    
    pPolicyConfig->Release();
    if (should_uninit_com) CoUninitialize();
    
    if (SUCCEEDED(hr)) {
        std::string name_str = wide_to_utf8(g_original_default_device_name);
        fprintf(stderr, "[FxSound Engine] Restored default device: %s\n", name_str.c_str());
        return true;
    } else {
        fprintf(stderr, "[FxSound Engine] Failed to restore default device (0x%08X)\n", hr);
        return false;
    }
}

static int safe_stoi(const char* str, int default_val) {
    try { return std::stoi(str); }
    catch (...) { return default_val; }
}

static float safe_stof(const char* str, float default_val) {
    try { return std::stof(str); }
    catch (...) { return default_val; }
}

// Get the directory where the executable is located
static std::wstring getExeDirectory() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring exe_path(path);
    auto pos = exe_path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        return exe_path.substr(0, pos);
    }
    return L".";
}

static std::wstring getRecoveryFilePath() {
    std::wstring dir = getExeDirectory();
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') {
        dir += L'\\';
    }
    return dir + L".fxsound_recovery";
}

static EngineConfig parse_args(int argc, char* argv[]) {
    EngineConfig config;

    // CRITICAL FIX: On Windows, argv uses the system's ANSI code page (e.g., GBK
    // on Chinese Windows), NOT UTF-8. Passing Chinese paths through argv and then
    // calling utf8_to_wide() produces garbled text (e.g., "E:\听风者" becomes
    // "E:\������"). Solution: use CommandLineToArgvW() to get proper Unicode args.
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv) return config;

    for (int i = 1; i < wargc; i++) {
        std::wstring warg = wargv[i];
        // Options are pure ASCII — compare directly on wstring
        if ((warg == L"--preset" || warg == L"-p") && i + 1 < wargc) {
            config.preset_path = wargv[++i];
        } else if ((warg == L"--device" || warg == L"-d") && i + 1 < wargc) {
            config.device_name = wargv[++i];
        } else if ((warg == L"--device-index") && i + 1 < wargc) {
            config.device_index = safe_stoi(wide_to_utf8(wargv[++i]).c_str(), -1);
        } else if ((warg == L"--fidelity" || warg == L"-f") && i + 1 < wargc) {
            config.fidelity = safe_stof(wide_to_utf8(wargv[++i]).c_str(), -1.0f);
        } else if ((warg == L"--ambience" || warg == L"-a") && i + 1 < wargc) {
            config.ambience = safe_stof(wide_to_utf8(wargv[++i]).c_str(), -1.0f);
        } else if ((warg == L"--surround" || warg == L"-s") && i + 1 < wargc) {
            config.surround = safe_stof(wide_to_utf8(wargv[++i]).c_str(), -1.0f);
        } else if ((warg == L"--dynamic-boost" || warg == L"-b") && i + 1 < wargc) {
            config.dynamic_boost = safe_stof(wide_to_utf8(wargv[++i]).c_str(), -1.0f);
        } else if ((warg == L"--bass") && i + 1 < wargc) {
            config.bass = safe_stof(wide_to_utf8(wargv[++i]).c_str(), -1.0f);
        } else if ((warg == L"--buffer") && i + 1 < wargc) {
            config.buffer_ms = safe_stoi(wide_to_utf8(wargv[++i]).c_str(), 40);
        } else if (warg == L"--list-devices" || warg == L"-l") {
            config.mode = RunMode::LIST_DEVICES;
        } else if (warg == L"--analyze") {
            config.analyze_mode = true;
        } else if (warg == L"--install-driver") {
            config.mode = RunMode::INSTALL_DRIVER;
            // Optional: next arg can be driver directory
            if (i + 1 < wargc && wargv[i + 1][0] != L'-') {
                config.driver_dir = wargv[++i];
            }
        } else if (warg == L"--uninstall-driver") {
            config.mode = RunMode::UNINSTALL_DRIVER;
        } else if (warg == L"--check-driver") {
            config.mode = RunMode::CHECK_DRIVER;
        } else if (warg == L"--restore-default") {
            config.mode = RunMode::RESTORE_DEFAULT;
        } else if (warg == L"--list-crash-dumps") {
            config.mode = RunMode::LIST_CRASH_DUMPS;
        } else if (warg == L"--help" || warg == L"-h") {
            printf("FxSound Engine v2.0.0 (Virtual Device Mode)\n");
            printf("Usage: fxsound_engine [options]\n\n");
            printf("Audio Processing:\n");
            printf("  --preset <path>        Path to .fac preset file\n");
            printf("  --device <name>        Target playback device name\n");
            printf("  --device-index <n>     Target playback device index (from --list-devices)\n");
            printf("  --fidelity <0-10>      Fidelity effect level\n");
            printf("  --ambience <0-10>      Ambience effect level\n");
            printf("  --surround <0-10>      Surround effect level\n");
            printf("  --dynamic-boost <0-10> Dynamic Boost level\n");
            printf("  --bass <0-10>          Bass effect level\n");
            printf("  --buffer <ms>          Buffer size in milliseconds (default: 40)\n");
            printf("  --analyze              Capture-only mode (no playback, for testing)\n\n");
            printf("Driver Management (requires Administrator):\n");
            printf("  --install-driver [dir] Install fxvad.sys virtual audio device\n");
            printf("                         dir: optional path to drivers directory\n");
            printf("  --uninstall-driver     Remove fxvad.sys virtual audio device\n");
            printf("  --check-driver         Check if virtual device is installed\n");
            printf("  --restore-default      Restore default playback device (from recovery file)\n\n");
            printf("Info:\n");
            printf("  --list-devices         List available audio devices\n");
            printf("  --help                 Show this help\n\n");
            printf("Audio Path: Virtual Device (Loopback) -> DfxDsp -> Real Device (Render)\n");
            exit(0);
        } else {
            std::string unknown_arg = wide_to_utf8(warg);
            fprintf(stderr, "[FxSound Engine] Unknown argument: %s\n", unknown_arg.c_str());
        }
    }
    LocalFree(wargv);
    return config;
}

// ============================================================================
// List audio devices
// ============================================================================
static bool isFxSoundVirtualDeviceName(const std::wstring& name) {
    return name.find(L"FxSound") != std::wstring::npos ||
           name.find(L"DFX Audio Enhancer") != std::wstring::npos;
}

static std::wstring readRecoveryDeviceId() {
    FILE* rf = _wfopen(getRecoveryFilePath().c_str(), L"r");
    if (!rf) return L"";

    char saved_id[512] = {0};
    if (!fgets(saved_id, sizeof(saved_id) - 1, rf)) {
        fclose(rf);
        return L"";
    }
    fclose(rf);

    size_t len = strlen(saved_id);
    while (len > 0 && (saved_id[len - 1] == '\n' || saved_id[len - 1] == '\r')) {
        saved_id[--len] = '\0';
    }
    return len > 0 ? utf8_to_wide(saved_id) : L"";
}

static std::wstring getDefaultRenderDeviceId(EDataFlow flow, ERole role) {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    LPWSTR device_id = nullptr;
    std::wstring id;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (SUCCEEDED(hr)) {
        hr = enumerator->GetDefaultAudioEndpoint(flow, role, &device);
    }
    if (SUCCEEDED(hr) && device) {
        hr = device->GetId(&device_id);
    }
    if (SUCCEEDED(hr) && device_id) {
        id = device_id;
    }

    if (device_id) CoTaskMemFree(device_id);
    if (device) device->Release();
    if (enumerator) enumerator->Release();
    return id;
}

static int list_devices() {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDeviceCollection* collection = nullptr;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr)) {
        fprintf(stderr, "[FxSound Engine] Failed to create device enumerator (0x%08X)\n", hr);
        printf("{\"type\":\"device_list\",\"devices\":[]}\n");
        fflush(stdout);
        return 1;
    }

    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) {
        fprintf(stderr, "[FxSound Engine] Failed to enumerate render devices (0x%08X)\n", hr);
        enumerator->Release();
        printf("{\"type\":\"device_list\",\"devices\":[]}\n");
        fflush(stdout);
        return 1;
    }

    std::wstring default_console_id = getDefaultRenderDeviceId(eRender, eConsole);
    std::wstring default_multimedia_id = getDefaultRenderDeviceId(eRender, eMultimedia);
    std::wstring recovery_default_id = readRecoveryDeviceId();

    // Output as JSON for Flutter parsing — one JSON object per line.
    // Format: {"type":"device_list","devices":[{"index":0,"id":"...","name":"...","is_default":true,"is_real":true},...]}
    printf("{\"type\":\"device_list\",\"devices\":[");

    UINT count = 0;
    collection->GetCount(&count);
    bool first = true;
    int idx = 0;
    int playback_count = 0;

    for (UINT i = 0; i < count; i++) {
        IMMDevice* device = nullptr;
        IPropertyStore* props = nullptr;
        LPWSTR raw_id = nullptr;
        PROPVARIANT friendly_name;
        PropVariantInit(&friendly_name);

        hr = collection->Item(i, &device);
        if (FAILED(hr) || !device) {
            continue;
        }

        std::wstring device_id;
        if (SUCCEEDED(device->GetId(&raw_id)) && raw_id) {
            device_id = raw_id;
        }

        std::wstring name = L"Unknown";
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) &&
            SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &friendly_name)) &&
            friendly_name.vt == VT_LPWSTR && friendly_name.pwszVal) {
            name = friendly_name.pwszVal;
        }

        if (isFxSoundVirtualDeviceName(name)) {
            if (device_id == default_console_id || device_id == default_multimedia_id) {
                default_console_id = recovery_default_id;
                default_multimedia_id = recovery_default_id;
            }
            PropVariantClear(&friendly_name);
            if (raw_id) CoTaskMemFree(raw_id);
            if (props) props->Release();
            device->Release();
            continue;
        }

        if (!first) printf(",");
        first = false;

        bool is_default = (!device_id.empty() &&
                           (device_id == default_console_id || device_id == default_multimedia_id));
        std::string id_json = json_escape(wide_to_utf8(device_id));
        std::string name_json = json_escape(wide_to_utf8(name));
        printf("{\"index\":%d,\"id\":\"%s\",\"name\":\"%s\",\"is_default\":%s,\"is_real\":true}",
               idx, id_json.c_str(), name_json.c_str(),
               is_default ? "true" : "false");
        idx++;
        playback_count++;

        PropVariantClear(&friendly_name);
        if (raw_id) CoTaskMemFree(raw_id);
        if (props) props->Release();
        device->Release();
    }

    printf("]}\n");
    fflush(stdout);

    collection->Release();
    enumerator->Release();

    // Also print human-readable info to stderr for debugging
    fprintf(stderr, "[FxSound Engine] Found %u active render devices (%d real playback)\n",
            count, playback_count);

    return 0;
}

// ============================================================================
// Driver management commands
// ============================================================================
static int cmd_install_driver(const EngineConfig& config) {
    std::wstring driver_dir = config.driver_dir;
    if (driver_dir.empty()) {
        driver_dir = findDriverDirectory(getExeDirectory());
    }
    
    std::string log;
    DriverResult result = installDriver(driver_dir, log);
    DriverStatusInfo status = queryDriverStatus();
    
    // Output as JSON for Flutter parsing
    std::string json_line = "{\"type\":\"driver_install\",\"result\":\"" + std::string(driverResultToString(result)) +
                            "\",\"message\":\"" + json_escape(log) +
                            "\",\"ownership\":\"" + driverOwnershipToString(status.ownership) +
                            "\",\"device_name\":\"" + json_escape(wide_to_utf8(status.device_name)) +
                            "\",\"hardware_id\":\"" + json_escape(wide_to_utf8(status.hardware_id)) +
                            "\",\"ownership_marker\":\"" + json_escape(wide_to_utf8(getDriverOwnershipMarkerPath())) + "\"";
    if (!status.conflict_process.empty()) {
        json_line += ",\"conflict_process\":\"" + json_escape(wide_to_utf8(status.conflict_process)) + "\"";
    }
    json_line += "}\n";
    printf("%s", json_line.c_str());
    fflush(stdout);
    
    fprintf(stderr, "[FxSound Engine] Driver install: %s - %s\n", 
            driverResultToString(result), log.c_str());

    // ALSO write result to a temp file so elevated (UAC) invocations can
    // be read back by the non-elevated Flutter app.  The file is placed
    // next to the exe so it's easy to find.
    {
        std::wstring result_path = getExeDirectory() + L"\\driver_install_result.json";
        FILE* fp = _wfopen(result_path.c_str(), L"w");
        if (fp) {
            fputs(json_line.c_str(), fp);
            fclose(fp);
        }
    }
    
    return (result == DRIVER_OK || result == DRIVER_ALREADY_INSTALLED ||
            result == DRIVER_REBOOT_REQUIRED || result == DRIVER_EXTERNAL_INSTALLED) ? 0 : 1;
}

static int cmd_uninstall_driver() {
    std::string log;
    DriverResult result = uninstallDriver(log);
    DriverStatusInfo status = queryDriverStatus();
    
    std::string json_line = "{\"type\":\"driver_uninstall\",\"result\":\"" + std::string(driverResultToString(result)) +
                            "\",\"message\":\"" + json_escape(log) +
                            "\",\"ownership\":\"" + driverOwnershipToString(status.ownership) +
                            "\",\"device_name\":\"" + json_escape(wide_to_utf8(status.device_name)) +
                            "\",\"hardware_id\":\"" + json_escape(wide_to_utf8(status.hardware_id)) +
                            "\",\"ownership_marker\":\"" + json_escape(wide_to_utf8(getDriverOwnershipMarkerPath())) + "\"";
    if (!status.conflict_process.empty()) {
        json_line += ",\"conflict_process\":\"" + json_escape(wide_to_utf8(status.conflict_process)) + "\"";
    }
    json_line += "}\n";
    printf("%s", json_line.c_str());
    fflush(stdout);
    
    fprintf(stderr, "[FxSound Engine] Driver uninstall: %s - %s\n", 
            driverResultToString(result), log.c_str());

    // Write result to temp file for UAC scenario
    {
        std::wstring result_path = getExeDirectory() + L"\\driver_uninstall_result.json";
        FILE* fp = _wfopen(result_path.c_str(), L"w");
        if (fp) {
            fputs(json_line.c_str(), fp);
            fclose(fp);
        }
    }
    
    return (result == DRIVER_OK || result == DRIVER_NOT_INSTALLED) ? 0 : 1;
}

static int cmd_check_driver() {
    DriverStatusInfo status = queryDriverStatus();
    
    printf("{\"type\":\"driver_status\",\"result\":\"%s\",\"ownership\":\"%s\",\"device_name\":\"%s\",\"hardware_id\":\"%s\",\"ownership_marker\":\"%s\"",
           driverStatusResultToString(status.result),
           driverOwnershipToString(status.ownership),
           json_escape(wide_to_utf8(status.device_name)).c_str(),
           json_escape(wide_to_utf8(status.hardware_id)).c_str(),
           json_escape(wide_to_utf8(getDriverOwnershipMarkerPath())).c_str());
    if (!status.conflict_process.empty()) {
        printf(",\"conflict_process\":\"%s\"",
               json_escape(wide_to_utf8(status.conflict_process)).c_str());
    }
    printf("}\n");
    fflush(stdout);
    
    fprintf(stderr, "[FxSound Engine] Driver status: %s ownership=%s\n",
            driverStatusResultToString(status.result),
            driverOwnershipToString(status.ownership));
    
    return (status.result == DRIVER_OK) ? 0 : 1;
}

// ============================================================================
// Main entry point
// ============================================================================
int main(int argc, char* argv[]) {
    // Hide console window — all I/O is JSON over stdout, no user interaction needed
    HWND hWnd = GetConsoleWindow();
    if (hWnd != NULL) {
        ShowWindow(hWnd, SW_HIDE);
    }

    // Set console code page to UTF-8 so Chinese device names display correctly
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // Initialize COM (needed for audio device enumeration in all modes)
    HRESULT com_init = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    // Install crash handler FIRST — before anything else so all crashes are captured
    {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        std::wstring exeDir(exePath);
        size_t lastSlash = exeDir.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) exeDir = exeDir.substr(0, lastSlash);
        CrashHandler::install(exeDir);
    }
    
    // Install signal handlers for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    
    // Parse command line
    EngineConfig config = parse_args(argc, argv);
    
    // Output startup info
    printf("{\"type\":\"startup\",\"version\":\"2.0.0\",\"pid\":%d,\"mode\":\"virtual_device\"}\n", 
           GetCurrentProcessId());
    fflush(stdout);
    
    // =========================================================================
    // Driver management modes (one-shot commands, then exit)
    // =========================================================================
    if (config.mode == RunMode::INSTALL_DRIVER) {
        return cmd_install_driver(config);
    }
    if (config.mode == RunMode::UNINSTALL_DRIVER) {
        return cmd_uninstall_driver();
    }
    if (config.mode == RunMode::CHECK_DRIVER) {
        return cmd_check_driver();
    }
    if (config.mode == RunMode::LIST_CRASH_DUMPS) {
        // List crash dump files for Flutter to detect
        auto dumps = CrashHandler::findCrashDumps();
        printf("{\"type\":\"crash_dumps\",\"count\":%zu,\"dumps\":[", dumps.size());
        for (size_t i = 0; i < dumps.size(); i++) {
            std::string p = json_escape(wide_to_utf8(dumps[i]));
            printf("%s\"%s\"", (i > 0 ? "," : ""), p.c_str());
        }
        printf("]}\n");
        fflush(stdout);
        return 0;
    }
    if (config.mode == RunMode::RESTORE_DEFAULT) {
        // Read recovery file and restore the default playback device
        std::wstring recovery_file = getRecoveryFilePath();
        
        FILE* rf = _wfopen(recovery_file.c_str(), L"r");
        if (!rf) {
            printf("{\"type\":\"restore_default\",\"status\":\"no_recovery_file\"}\n");
            fprintf(stderr, "No recovery file found. Nothing to restore.\n");
            return 0;
        }
        
        char saved_id[512] = {0};
        char saved_name[512] = {0};
        if (fgets(saved_id, sizeof(saved_id) - 1, rf)) {
            size_t len = strlen(saved_id);
            while (len > 0 && (saved_id[len-1] == '\n' || saved_id[len-1] == '\r')) saved_id[--len] = '\0';
            if (fgets(saved_name, sizeof(saved_name) - 1, rf)) {
                size_t nlen = strlen(saved_name);
                while (nlen > 0 && (saved_name[nlen-1] == '\n' || saved_name[nlen-1] == '\r')) saved_name[--nlen] = '\0';
            }
        }
        fclose(rf);
        
        if (strlen(saved_id) == 0) {
            printf("{\"type\":\"restore_default\",\"status\":\"empty_recovery_file\"}\n");
            DeleteFileW(recovery_file.c_str());
            return 0;
        }
        
        std::wstring w_id = utf8_to_wide(saved_id);
        std::wstring w_name = utf8_to_wide(saved_name);
        
        // Skip if saved device is FxSound virtual device
        if (w_name.find(L"FxSound") != std::wstring::npos) {
            printf("{\"type\":\"restore_default\",\"status\":\"skipped\",\"reason\":\"dfx_device\"}\n");
            fprintf(stderr, "Recovery file contains FxSound device, skipping.\n");
            DeleteFileW(recovery_file.c_str());
            return 0;
        }
        
        IPolicyConfigVista* pPolicyConfig = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(CPolicyConfigVistaClient), nullptr, CLSCTX_ALL,
                                      __uuidof(IPolicyConfigVista), (LPVOID*)&pPolicyConfig);
        if (FAILED(hr)) {
            printf("{\"type\":\"restore_default\",\"status\":\"error\",\"reason\":\"policy_config_failed\"}\n");
            fprintf(stderr, "Failed to create PolicyConfig (0x%08X)\n", hr);
            return 1;
        }
        
        hr = pPolicyConfig->SetDefaultEndpoint(w_id.c_str(), eConsole);
        if (SUCCEEDED(hr)) {
            hr = pPolicyConfig->SetDefaultEndpoint(w_id.c_str(), eMultimedia);
        }
        if (SUCCEEDED(hr)) {
            hr = pPolicyConfig->SetDefaultEndpoint(w_id.c_str(), eCommunications);
        }
        pPolicyConfig->Release();
        if (FAILED(hr)) {
            printf("{\"type\":\"restore_default\",\"status\":\"error\",\"reason\":\"set_default_failed\",\"hresult\":\"0x%08X\"}\n", hr);
            fprintf(stderr, "Failed to restore default device (0x%08X)\n", hr);
            return 1;
        }
        
        DeleteFileW(recovery_file.c_str());
        
        std::string name_str = json_escape(saved_name);
        printf("{\"type\":\"restore_default\",\"status\":\"ok\",\"device\":\"%s\"}\n", name_str.c_str());
        fprintf(stderr, "Default device restored to: %s\n", saved_name);
        return 0;
    }
    
    // =========================================================================
    // List devices mode
    // =========================================================================
    if (config.mode == RunMode::LIST_DEVICES) {
        return list_devices();
    }
    
    // =========================================================================
    // Normal processing mode - validate required arguments
    // =========================================================================
    if (config.preset_path.empty()) {
        fprintf(stderr, "[FxSound Engine] Error: --preset is required\n");
        fprintf(stderr, "  Usage: fxsound_engine --preset <path_to_fac_file>\n");
        return 1;
    }
    
    // Check if preset file exists
    DWORD fileAttrs = GetFileAttributesW(config.preset_path.c_str());
    if (fileAttrs == INVALID_FILE_ATTRIBUTES) {
        std::string path_str = wide_to_utf8(config.preset_path);
        fprintf(stderr, "[FxSound Engine] Error: Preset file not found: %s\n", path_str.c_str());
        return 1;
    }
    
    // Check if virtual audio device is installed and not currently owned by another FxSound app.
    DriverStatusInfo driver_status = queryDriverStatus();
    if (driver_status.result == DRIVER_CONFLICT_RUNNING) {
        std::string process = json_escape(wide_to_utf8(driver_status.conflict_process));
        printf("{\"type\":\"error\",\"code\":\"fxsound_conflict\",\"message\":\"FxSound is already running. Close it before starting enhancement.\",\"conflict_process\":\"%s\"}\n",
               process.c_str());
        fflush(stdout);
        fprintf(stderr, "[FxSound Engine] Error: conflicting FxSound process is running: %s\n", process.c_str());
        return 1;
    }
    if (driver_status.result == DRIVER_DISABLED) {
        printf("{\"type\":\"error\",\"code\":\"driver_disabled\",\"message\":\"FxSound virtual audio device is disabled. Enable it before starting enhancement.\"}\n");
        fflush(stdout);
        fprintf(stderr, "[FxSound Engine] Error: FxSound virtual audio device is disabled.\n");
        return 1;
    }
    if (driver_status.result != DRIVER_OK) {
        printf("{\"type\":\"error\",\"code\":\"driver_not_found\",\"message\":\"Virtual audio device not installed. Run with --install-driver first.\"}\n");
        fflush(stdout);
        fprintf(stderr, "[FxSound Engine] Error: Virtual audio device not installed.\n");
        fprintf(stderr, "  Run: fxsound_engine --install-driver\n");
        return 1;
    }
    
    fprintf(stderr, "[FxSound Engine] Initializing...\n");
    
    // =========================================================================
    // Step 0: Save the current default playback device BEFORE any modification.
    // This is critical for environment recovery on exit, since NO_REGISTRY
    // disables the original sndDevicesRestoreDefaultDevice logic.
    // =========================================================================
    bool has_saved_device = saveDefaultPlaybackDevice();
    
    // =========================================================================
    // Step 0b: Crash recovery — check if previous run left a recovery file.
    // If the engine was killed without cleanup (e.g. Process.kill()), the
    // default device may still point to the DFX virtual device. We detect
    // this and restore before starting a new session.
    // =========================================================================
    {
        std::wstring recovery_file = getRecoveryFilePath();
        
        FILE* rf = _wfopen(recovery_file.c_str(), L"r");
        if (rf) {
            bool remove_recovery_file = false;
            char saved_id[512] = {0};
            char saved_name[512] = {0};
            if (fgets(saved_id, sizeof(saved_id) - 1, rf)) {
                // Strip trailing newline from ID
                size_t len = strlen(saved_id);
                while (len > 0 && (saved_id[len-1] == '\n' || saved_id[len-1] == '\r')) {
                    saved_id[--len] = '\0';
                }
                // Read second line: device name
                if (fgets(saved_name, sizeof(saved_name) - 1, rf)) {
                    size_t nlen = strlen(saved_name);
                    while (nlen > 0 && (saved_name[nlen-1] == '\n' || saved_name[nlen-1] == '\r')) {
                        saved_name[--nlen] = '\0';
                    }
                }
                
                if (len > 0) {
                    std::wstring saved_device_id = utf8_to_wide(saved_id);
                    std::wstring saved_device_name = utf8_to_wide(saved_name);
                    
                    // Don't restore if the saved device is the FxSound virtual device
                    bool is_dfx = (saved_device_name.find(L"FxSound") != std::wstring::npos);
                    
                    if (is_dfx) {
                        fprintf(stderr, "[FxSound Engine] Recovery file contains FxSound device, skipping restore\n");
                        printf("{\"type\":\"crash_recovery\",\"status\":\"skipped\",\"reason\":\"dfx_device\"}\n");
                        fflush(stdout);
                        remove_recovery_file = true;
                    } else {
                        fprintf(stderr, "[FxSound Engine] Found recovery file from previous crash, restoring device...\n");
                        
                        IPolicyConfigVista* pPolicyConfig = nullptr;
                        HRESULT hr = CoCreateInstance(__uuidof(CPolicyConfigVistaClient), nullptr, CLSCTX_ALL,
                                                      __uuidof(IPolicyConfigVista), (LPVOID*)&pPolicyConfig);
                        if (SUCCEEDED(hr)) {
                            hr = pPolicyConfig->SetDefaultEndpoint(saved_device_id.c_str(), eConsole);
                            if (SUCCEEDED(hr)) {
                                hr = pPolicyConfig->SetDefaultEndpoint(saved_device_id.c_str(), eMultimedia);
                            }
                            if (SUCCEEDED(hr)) {
                                hr = pPolicyConfig->SetDefaultEndpoint(saved_device_id.c_str(), eCommunications);
                            }
                            pPolicyConfig->Release();
                        }

                        if (SUCCEEDED(hr)) {
                            fprintf(stderr, "[FxSound Engine] Previous device restored from recovery file\n");
                            printf("{\"type\":\"crash_recovery\",\"status\":\"restored\"}\n");
                            remove_recovery_file = true;
                        } else {
                            fprintf(stderr, "[FxSound Engine] Crash recovery restore failed (0x%08X)\n", hr);
                            printf("{\"type\":\"crash_recovery\",\"status\":\"error\",\"reason\":\"restore_failed\",\"hresult\":\"0x%08X\"}\n", hr);
                        }
                        fflush(stdout);
                    }
                }
            }
            fclose(rf);
            // Remove the recovery file — it's served its purpose
            if (remove_recovery_file) {
                DeleteFileW(recovery_file.c_str());
            }
        }
    }
    
    // =========================================================================
    // Step 0c: Write recovery file with the original default device ID.
    // If the engine crashes or is killed, the next launch will read this file
    // and restore the device. On clean exit, we delete the file.
    // =========================================================================
    if (has_saved_device) {
        std::wstring recovery_file = getRecoveryFilePath();
        
        FILE* rf = _wfopen(recovery_file.c_str(), L"w");
        if (rf) {
            std::string id_utf8 = wide_to_utf8(g_original_default_device_id);
            std::string name_utf8 = wide_to_utf8(g_original_default_device_name);
            fprintf(rf, "%s\n%s\n", id_utf8.c_str(), name_utf8.c_str());
            fclose(rf);
            fprintf(stderr, "[FxSound Engine] Recovery file written\n");
        }
    }
    
    // Notify Flutter which device was the original default (so Flutter can show it in UI)
    if (has_saved_device) {
        std::string orig_device_str = json_escape(wide_to_utf8(g_original_default_device_name));
        printf("{\"type\":\"device_saved\",\"original_device\":\"%s\"}\n", orig_device_str.c_str());
        fflush(stdout);
    }
    
    // =========================================================================
    // Step 1: Create and initialize DfxDsp
    // =========================================================================
    g_dsp = new(std::nothrow) DfxDsp();
    if (!g_dsp) {
        fprintf(stderr, "[FxSound Engine] Failed to create DfxDsp\n");
        return 1;
    }
    
    // Load preset
    int result = g_dsp->loadPreset(config.preset_path);
    if (result != OKAY) {
        std::string path_str = wide_to_utf8(config.preset_path);
        fprintf(stderr, "[FxSound Engine] Failed to load preset: %s (error=%d)\n", 
                path_str.c_str(), result);
        delete g_dsp;
        g_dsp = nullptr;
        return 1;
    }
    
    // Apply effect overrides from command line (clamp to valid range 0-10)
    if (config.fidelity >= 0)    g_dsp->setEffectValue(DfxDsp::Fidelity,      (config.fidelity > 10.0f)      ? 10.0f : config.fidelity);
    if (config.ambience >= 0)    g_dsp->setEffectValue(DfxDsp::Ambience,      (config.ambience > 10.0f)      ? 10.0f : config.ambience);
    if (config.surround >= 0)    g_dsp->setEffectValue(DfxDsp::Surround,      (config.surround > 10.0f)      ? 10.0f : config.surround);
    if (config.dynamic_boost >= 0) g_dsp->setEffectValue(DfxDsp::DynamicBoost, (config.dynamic_boost > 10.0f) ? 10.0f : config.dynamic_boost);
    if (config.bass >= 0)        g_dsp->setEffectValue(DfxDsp::Bass,          (config.bass > 10.0f)          ? 10.0f : config.bass);
    
    // Power on DSP
    g_dsp->powerOn(true);
    
    fprintf(stderr, "[FxSound Engine] DSP initialized, preset loaded\n");
    
    // =========================================================================
    // Step 2: Create and initialize AudioPassthru
    // =========================================================================
    g_passthru = new(std::nothrow) AudioPassthru();
    if (!g_passthru) {
        fprintf(stderr, "[FxSound Engine] Failed to create AudioPassthru\n");
        delete g_dsp;
        g_dsp = nullptr;
        return 1;
    }
    
    // Register callback
    EngineCallback callback;
    g_passthru->registerCallback(&callback);
    
    // Set buffer length
    g_passthru->setBufferLength(config.buffer_ms);
    
    // Set DSP module
    g_passthru->setDspProcessingModule(g_dsp);
    
    // Initialize audio passthru
    result = g_passthru->init();
    if (result != OKAY) {
        fprintf(stderr, "[FxSound Engine] Failed to init AudioPassthru: %d\n", result);
        // init() may have changed the default device — restore it before exit
        if (has_saved_device) restoreDefaultPlaybackDevice();
        delete g_passthru;
        delete g_dsp;
        g_passthru = nullptr;
        g_dsp = nullptr;
        return 1;
    }
    
    // Select target playback device if specified
    // In virtual device mode, the playback device is the REAL device where
    // processed audio gets rendered. The capture device is automatically
    // set to the DFX virtual device.
    if (!config.device_name.empty() || config.device_index >= 0) {
        auto devices = g_passthru->getSoundDevices(true);
        bool found = false;
        
        if (config.device_index >= 0) {
            // Select by index (matches --list-devices numbering)
            int idx = 0;
            for (const auto& dev : devices) {
                if (dev.isRealDevice && !dev.isDFXDevice) {
                    if (idx == config.device_index) {
                        g_passthru->setAsPlaybackDevice(dev);
                        std::string name_str = wide_to_utf8(dev.deviceFriendlyName);
                        fprintf(stderr, "[FxSound Engine] Selected playback device [%d]: %s\n", config.device_index, name_str.c_str());
                        found = true;
                        break;
                    }
                    idx++;
                }
            }
            if (!found) {
                fprintf(stderr, "[FxSound Engine] Warning: Device index %d not found, using default\n", config.device_index);
            }
        } else {
            // Select by name substring match from real active render devices.
            for (const auto& dev : devices) {
                if (dev.isRealDevice && !dev.isDFXDevice) {
                    if (dev.deviceFriendlyName.find(config.device_name) != std::wstring::npos) {
                        g_passthru->setAsPlaybackDevice(dev);
                        std::string name_str = wide_to_utf8(dev.deviceFriendlyName);
                        fprintf(stderr, "[FxSound Engine] Selected playback device: %s\n", name_str.c_str());
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                std::string name_str = wide_to_utf8(config.device_name);
                fprintf(stderr, "[FxSound Engine] Warning: Playback device not found: %s, using default\n", 
                        name_str.c_str());
            }
        }
    }
    
    fprintf(stderr, "[FxSound Engine] AudioPassthru initialized (Virtual Device Mode)\n");
    
    // =========================================================================
    // Step 3: Main processing loop
    // =========================================================================
    if (config.analyze_mode) {
        // Analyze mode: mute playback, only capture and process
        g_passthru->mute(true);
        fprintf(stderr, "[FxSound Engine] Analyze mode: capture only, no playback\n");
    }
    
    printf("{\"type\":\"ready\",\"status\":\"processing\",\"mode\":\"virtual_device\"}\n");
    fflush(stdout);
    
    fprintf(stderr, "[FxSound Engine] Audio enhancement active. Press Ctrl+C to stop.\n");
    
    // Output current settings (self-diagnostic startup summary)
    printf("{\"type\":\"settings\"");
    printf(",\"preset\":\"%s\"", json_escape(wide_to_utf8(config.preset_path)).c_str());
    printf(",\"fidelity\":%.1f", g_dsp->getEffectValue(DfxDsp::Fidelity));
    printf(",\"ambience\":%.1f", g_dsp->getEffectValue(DfxDsp::Ambience));
    printf(",\"surround\":%.1f", g_dsp->getEffectValue(DfxDsp::Surround));
    printf(",\"dynamic_boost\":%.1f", g_dsp->getEffectValue(DfxDsp::DynamicBoost));
    printf(",\"bass\":%.1f", g_dsp->getEffectValue(DfxDsp::Bass));
    printf(",\"power_on\":%s", g_dsp->isPowerOn() ? "true" : "false");
    printf(",\"buffer_ms\":%d", config.buffer_ms);
    printf("}\n");
    fflush(stdout);
    fprintf(stderr, "[FxSound Engine] Startup summary: preset=%s fidelity=%.1f ambience=%.1f surround=%.1f dynamic_boost=%.1f bass=%.1f power=%s\n",
            wide_to_utf8(config.preset_path).c_str(),
            g_dsp->getEffectValue(DfxDsp::Fidelity),
            g_dsp->getEffectValue(DfxDsp::Ambience),
            g_dsp->getEffectValue(DfxDsp::Surround),
            g_dsp->getEffectValue(DfxDsp::DynamicBoost),
            g_dsp->getEffectValue(DfxDsp::Bass),
            g_dsp->isPowerOn() ? "on" : "off");
    
    unsigned long timer_count = 0;
    const unsigned long DEVICE_CHECK_START = 200; // Skip device checks for first ~2 seconds after init
    while (g_running) {
        // processTimer() drives the audio processing pipeline
        // PERF: Scoped timer to measure each processing call
        PERF_SCOPED_TIMER(g_perfSampler);
        int pt_result = g_passthru->processTimer();
        
        // Log first few results for debugging
        if (timer_count == 0) {
            if (pt_result == OKAY) {
                fprintf(stderr, "[FxSound Engine] Audio pipeline started successfully\n");
            } else {
                fprintf(stderr, "[FxSound Engine] Audio pipeline error: %d (will retry)\n", pt_result);
            }
        }
        
        // Check for device changes periodically (but not during startup stabilization)
        if (timer_count >= DEVICE_CHECK_START && timer_count % 100 == 0) {
            g_passthru->checkDeviceChanges();
        }
        
        timer_count++;
        
        // Check for realtime commands from Flutter (non-blocking)
        checkStdinForCommands();

        // PERF: Report statistics every N samples and periodically print process stats
        if (g_perfSampler.shouldReport()) {
            auto stats = g_perfSampler.computeStats();
            g_perfSampler.printStats("processTimer", stats);
            printProcessStats();
        }

        // Small sleep to avoid busy-waiting (processTimer has its own timing)
        Sleep(10);
    }
    
    // =========================================================================
    // Step 4: Cleanup — restore environment and shut down
    // IMPORTANT: Must restore the original default playback device before exit!
    // In virtual device mode, the DFX device was set as system default.
    // If we don't restore, the user loses audio after exit.
    // =========================================================================
    fprintf(stderr, "[FxSound Engine] Shutting down...\n");

    // PERF: Print any remaining samples before shutdown
    {
        auto stats = g_perfSampler.computeStats();
        if (stats.count > 0) {
            g_perfSampler.printStats("processTimer_final", stats);
        }
    }
    
    // Mute output before stopping
    if (g_passthru) {
        g_passthru->mute(true);
    }
    
    // Restore the original default playback device.
    // We do this ourselves (not via AudioPassthru::restoreDefaultPlaybackDevice)
    // because that function relies on registry which is disabled by NO_REGISTRY.
    bool restored = false;
    if (has_saved_device) {
        restored = restoreDefaultPlaybackDevice();
    }
    
    // Power off DSP
    if (g_dsp) {
        g_dsp->powerOn(false);
    }
    
    // Clean up recovery file on successful shutdown — it's no longer needed
    if (restored) {
        std::wstring recovery_file = getRecoveryFilePath();
        DeleteFileW(recovery_file.c_str());
        fprintf(stderr, "[FxSound Engine] Recovery file cleaned up\n");
    }
    
    // Output shutdown status with restoration result
    std::string restored_device_name = json_escape(wide_to_utf8(g_original_default_device_name));
    printf("{\"type\":\"shutdown\",\"status\":\"ok\",\"device_restored\":%s",
           restored ? "true" : "false");
    if (restored) {
        printf(",\"restored_device\":\"%s\"", restored_device_name.c_str());
    }
    printf("}\n");
    fflush(stdout);
    
    // Delete objects (AudioPassthru destructor stops threads)
    delete g_passthru;
    delete g_dsp;
    g_passthru = nullptr;
    g_dsp = nullptr;
    
    fprintf(stderr, "[FxSound Engine] Stopped.\n");
    return 0;
}
