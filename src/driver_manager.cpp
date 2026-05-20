/*
 * driver_manager.cpp
 *
 * Virtual audio device driver management for FxSound Engine.
 * Embeds the SetupDi-based driver install/uninstall logic from DfxInstall,
 * eliminating the need for the external fxdevcon.exe tool.
 *
 * License: AGPL-3.0 (inherits from FxSound source)
 */

#include "driver_manager.h"

#include <Windows.h>
#include <SetupAPI.h>
#include <Newdev.h>
#include <Mmdeviceapi.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <RegStr.h>
#include <Shlwapi.h>
#include <StrSafe.h>
#include <Tchar.h>
#include <TlHelp32.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>
#include <cstdio>

#pragma comment(lib, "SetupAPI.lib")
#pragma comment(lib, "Newdev.lib")

// MAX_CLASS_NAME_LEN is not always defined in SDK headers
#ifndef MAX_CLASS_NAME_LEN
#define MAX_CLASS_NAME_LEN 32
#endif

#ifndef LINE_LEN
#define LINE_LEN 256
#endif

// ============================================================================
// Constants
// ============================================================================

// Wide string to UTF-8 conversion
static std::string wide_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return std::string();
    std::string result(size - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &result[0], size, nullptr, nullptr);
    return result;
}

// Hardware ID for the virtual audio device (must match fxvad.inf)
static const wchar_t FXVAD_HARDWARE_ID[] = L"Root\\FXVAD";
static const wchar_t FXVAD_HARDWARE_ID_ALT[] = L"*FXVAD";   // Alternate HWID used in some FxSound versions

// The friendly name that appears in Windows sound settings
static const wchar_t DFX_DEVICE_NAME[] = L"FxSound Audio Enhancer";

// Registry path for MMDevices
static const wchar_t REG_PATH_DEVICES[] = LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\)";

// Ownership marker written only when Listening Wind created the shared FXVAD device.
// Stored under HKLM so non-elevated UI checks and elevated uninstall commands agree.
static const wchar_t OWNERSHIP_REG_KEY[] = LR"(SOFTWARE\ListeningWind\FxSoundDriver)";
static const wchar_t OWNERSHIP_VALUE_INSTALLED[] = L"InstalledByListeningWind";
static const wchar_t OWNERSHIP_VALUE_HWID[] = L"HardwareId";
static const wchar_t OWNERSHIP_VALUE_DEVICE[] = L"DeviceName";

static const DWORD ENABLE_DEVICE = 0x1;

static std::wstring getExecutableDirectory()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring exe_path(path);
    size_t last_sep = exe_path.find_last_of(L"\\/");
    if (last_sep == std::wstring::npos) {
        return L".";
    }
    return exe_path.substr(0, last_sep);
}

std::wstring getDriverOwnershipMarkerPath()
{
    std::wstring dir = getExecutableDirectory();
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') {
        dir += L'\\';
    }
    return dir + L"fxvad_ownership_marker.json";
}

// ============================================================================
// Internal: SetupDi-based driver install/update/remove
// (Ported from DfxInstall.cpp cmdInstall/cmdUpdate/cmdRemove)
// ============================================================================

// Step-by-step error diagnostics for driverCmdInstall
struct InstallDiag {
    int last_step;         // Which step failed (1-7)
    DWORD last_error;      // GetLastError() at point of failure
    std::string message;   // Human-readable description
};

static int driverCmdInstall(const wchar_t* inf_path, const wchar_t* hwid, InstallDiag* diag = nullptr)
{
    HDEVINFO DeviceInfoSet = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA DeviceInfoData;
    GUID ClassGUID;
    TCHAR ClassName[MAX_CLASS_NAME_LEN];
    TCHAR hwIdList[LINE_LEN + 4];
    TCHAR InfPathFull[MAX_PATH];
    int failcode = 2; // EXIT_FAIL
    bool device_registered = false;

    if (diag) { diag->last_step = 0; diag->last_error = 0; diag->message.clear(); }

    auto setDiag = [&](int step, const char* msg) {
        if (diag) {
            diag->last_step = step;
            diag->last_error = GetLastError();
            diag->message = msg;
        }
    };

    if (inf_path == NULL || hwid == NULL || !inf_path[0] || !hwid[0]) {
        setDiag(0, "Invalid parameters (null inf_path or hwid)");
        return 3; // EXIT_USAGE
    }

    // Step 1: Inf must be a full pathname
    if (GetFullPathName(inf_path, MAX_PATH, InfPathFull, NULL) >= MAX_PATH) {
        setDiag(1, "GetFullPathName failed (path too long?)");
        return 2;
    }

    // Step 1b: Check INF exists
    if (GetFileAttributes(InfPathFull) == INVALID_FILE_ATTRIBUTES) {
        setDiag(1, "INF file does not exist");
        return 2;
    }

    // Step 2: List of hardware ID's must be double zero-terminated
    ZeroMemory(hwIdList, sizeof(hwIdList));
    if (FAILED(StringCchCopy(hwIdList, LINE_LEN, hwid))) {
        setDiag(2, "StringCchCopy failed for hardware ID");
        goto cleanup;
    }

    // Step 3: Use the INF File to extract the Class GUID
    if (!SetupDiGetINFClass(InfPathFull, &ClassGUID, ClassName, sizeof(ClassName) / sizeof(ClassName[0]), 0)) {
        setDiag(3, "SetupDiGetINFClass failed - INF may be invalid or unsigned");
        goto cleanup;
    }

    // Step 4: Create the container for the to-be-created Device Information Element
    DeviceInfoSet = SetupDiCreateDeviceInfoList(&ClassGUID, 0);
    if (DeviceInfoSet == INVALID_HANDLE_VALUE) {
        setDiag(4, "SetupDiCreateDeviceInfoList failed - need admin?");
        goto cleanup;
    }

    // Step 5: Create the element using the Class GUID and Name from the INF file
    DeviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
    if (!SetupDiCreateDeviceInfo(DeviceInfoSet, ClassName, &ClassGUID, NULL, 0, DICD_GENERATE_ID, &DeviceInfoData)) {
        setDiag(5, "SetupDiCreateDeviceInfo failed");
        goto cleanup;
    }

    // Step 6: Add the HardwareID to the Device's HardwareID property
    if (!SetupDiSetDeviceRegistryProperty(DeviceInfoSet, &DeviceInfoData, SPDRP_HARDWAREID,
        (LPBYTE)hwIdList, ((DWORD)lstrlen(hwIdList) + 1 + 1) * sizeof(TCHAR))) {
        setDiag(6, "SetupDiSetDeviceRegistryProperty(HARDWAREID) failed");
        goto cleanup;
    }

    // Step 7: Transform the registry element into an actual devnode in the PnP HW tree
    if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, DeviceInfoSet, &DeviceInfoData)) {
        setDiag(7, "SetupDiCallClassInstaller(DIF_REGISTERDEVICE) failed - need admin?");
        goto cleanup;
    }
    device_registered = true;

    // Step 8: Update the driver for the device we just created
    {
        HMODULE newdevMod = LoadLibrary(TEXT("newdev.dll"));
        if (!newdevMod) {
            setDiag(8, "LoadLibrary(newdev.dll) failed");
            goto cleanup;
        }

        typedef BOOL(WINAPI* UpdateDriverFn)(HWND, LPCTSTR, LPCTSTR, DWORD, PBOOL);
#ifdef _UNICODE
        const char* fnName = "UpdateDriverForPlugAndPlayDevicesW";
#else
        const char* fnName = "UpdateDriverForPlugAndPlayDevicesA";
#endif
        UpdateDriverFn UpdateFn = (UpdateDriverFn)GetProcAddress(newdevMod, fnName);
        if (!UpdateFn) {
            setDiag(8, "GetProcAddress(UpdateDriverForPlugAndPlayDevices) failed");
            FreeLibrary(newdevMod);
            goto cleanup;
        }

        BOOL reboot = FALSE;
        DWORD flags = INSTALLFLAG_FORCE;
        if (!UpdateFn(NULL, hwid, InfPathFull, flags, &reboot)) {
            setDiag(8, "UpdateDriverForPlugAndPlayDevices failed - driver may be unsigned");
            FreeLibrary(newdevMod);
            goto cleanup;
        }

        failcode = reboot ? 1 : 0; // EXIT_REBOOT or EXIT_OK
        FreeLibrary(newdevMod);
    }

cleanup:
    if (failcode == 2 && device_registered && DeviceInfoSet != INVALID_HANDLE_VALUE) {
        // Roll back the devnode created by DIF_REGISTERDEVICE when driver update fails.
        SetupDiRemoveDevice(DeviceInfoSet, &DeviceInfoData);
    }
    if (DeviceInfoSet != INVALID_HANDLE_VALUE) {
        SetupDiDestroyDeviceInfoList(DeviceInfoSet);
    }
    return failcode;
}

static int driverCmdRemove(const wchar_t* hwid)
{
    if (!hwid || !hwid[0]) {
        return 3;
    }

    HDEVINFO devs = SetupDiGetClassDevs(NULL, NULL, 0, DIGCF_ALLCLASSES);
    if (devs == INVALID_HANDLE_VALUE) {
        return 2;
    }

    SP_DEVINFO_DATA dev_info;
    dev_info.cbSize = sizeof(SP_DEVINFO_DATA);
    WCHAR device_name[512];
    DWORD prop_type;
    int removed_count = 0;

    // Iterate ALL devices and remove every one matching the HWID
    // (multiple installs can leave duplicate ghost devices)
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &dev_info); ) {
        if (SetupDiGetDeviceRegistryProperty(devs, &dev_info, SPDRP_HARDWAREID, &prop_type, (PBYTE)device_name, sizeof(device_name), NULL)) {
            if (wcscmp(device_name, hwid) == 0) {
                if (SetupDiRemoveDevice(devs, &dev_info)) {
                    removed_count++;
                    // After removal, don't increment i — the device list shifts down
                    continue;
                }
            }
        }
        i++;
    }

    SetupDiDestroyDeviceInfoList(devs);
    return (removed_count > 0) ? 0 : 2;
}

// ============================================================================
// Internal: Audio device enumeration helpers
// ============================================================================

struct AudioDeviceInfo {
    std::wstring device_name;
    std::wstring device_guid;
    DWORD state = 0;
};

static bool enumAudioOutputs(std::vector<AudioDeviceInfo>& audio_devices)
{
    // Ensure COM is initialized for this thread (safe to call multiple times)
    HRESULT com_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool com_initialized = SUCCEEDED(com_hr);

    HRESULT h_res;
    IMMDeviceEnumerator* p_enumerator = NULL;
    IMMDeviceCollection* p_device_collection = NULL;

    h_res = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&p_enumerator);
    if (FAILED(h_res))
        return false;

    h_res = p_enumerator->EnumAudioEndpoints(eRender, DEVICE_STATEMASK_ALL & (~DEVICE_STATE_NOTPRESENT), &p_device_collection);
    if (SUCCEEDED(h_res)) {
        UINT count;
        h_res = p_device_collection->GetCount(&count);
        for (UINT index = 0; index < count; index++) {
            IMMDevice* p_device = NULL;
            h_res = p_device_collection->Item(index, &p_device);
            if (SUCCEEDED(h_res)) {
                AudioDeviceInfo audio_device;

                // Get friendly name
                IPropertyStore* p_prop_store = NULL;
                PROPVARIANT prop_variant;
                PropVariantInit(&prop_variant);
                if (SUCCEEDED(p_device->OpenPropertyStore(STGM_READ, &p_prop_store))) {
                    if (SUCCEEDED(p_prop_store->GetValue(PKEY_DeviceInterface_FriendlyName, &prop_variant))) {
                        if (prop_variant.vt == VT_LPWSTR) {
                            audio_device.device_name = prop_variant.pwszVal;
                        }
                    }
                    p_prop_store->Release();
                }
                PropVariantClear(&prop_variant);

                if (audio_device.device_name.empty()) {
                    p_device->Release();
                    continue;
                }

                p_device->GetState(&audio_device.state);

                // Get GUID from device ID
                LPWSTR p_id = NULL;
                if (SUCCEEDED(p_device->GetId(&p_id))) {
                    std::wstring id = p_id;
                    auto pos = id.rfind(L'{');
                    if (pos != std::wstring::npos) {
                        audio_device.device_guid = id.substr(pos);
                    }
                    CoTaskMemFree(p_id);
                }

                audio_devices.push_back(audio_device);
                p_device->Release();
            }
        }
        p_device_collection->Release();
    }

    p_enumerator->Release();
    
    // Uninitialize COM only if we initialized it in this call
    // (CoInitialize returns S_FALSE if already initialized on this thread)
    if (com_initialized && com_hr != S_FALSE) {
        CoUninitialize();
    }
    
    return SUCCEEDED(h_res);
}

static bool findDfxDevice(AudioDeviceInfo& device)
{
    std::vector<AudioDeviceInfo> devices;
    if (!enumAudioOutputs(devices)) {
        return false;
    }

    for (const auto& dev : devices) {
        if (dev.device_name.find(DFX_DEVICE_NAME) == 0) {
            device = dev;
            return true;
        }
    }
    return false;
}

static bool isDfxDevicePresent()
{
    AudioDeviceInfo device;
    return findDfxDevice(device) && !(device.state & DEVICE_STATE_DISABLED);
}

static bool hasRegistryOwnershipMarker()
{
    HKEY key = NULL;
    LSTATUS ret = RegOpenKeyExW(HKEY_LOCAL_MACHINE, OWNERSHIP_REG_KEY, 0,
                                KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
    if (ret == ERROR_INVALID_PARAMETER || ret == ERROR_FILE_NOT_FOUND) {
        ret = RegOpenKeyExW(HKEY_LOCAL_MACHINE, OWNERSHIP_REG_KEY, 0, KEY_QUERY_VALUE, &key);
    }
    if (ret != ERROR_SUCCESS) {
        return false;
    }

    DWORD value = 0;
    DWORD type = REG_DWORD;
    DWORD size = sizeof(value);
    ret = RegQueryValueExW(key, OWNERSHIP_VALUE_INSTALLED, NULL, &type,
                           reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);

    return ret == ERROR_SUCCESS && type == REG_DWORD && value == 1;
}

static bool hasLocalOwnershipMarker()
{
    std::wstring marker_path = getDriverOwnershipMarkerPath();
    DWORD attrs = GetFileAttributesW(marker_path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return false;
    }

    FILE* fp = _wfopen(marker_path.c_str(), L"rb");
    if (!fp) {
        return false;
    }

    char buffer[1024] = {};
    size_t read = fread(buffer, 1, sizeof(buffer) - 1, fp);
    fclose(fp);
    buffer[read] = '\0';

    std::string content(buffer);
    return content.find("\"owner\":\"ListeningWind\"") != std::string::npos &&
           content.find("\"hardware_id\":\"Root\\\\FXVAD\"") != std::string::npos &&
           content.find("\"device_name\":\"FxSound Audio Enhancer\"") != std::string::npos;
}

static bool hasOwnershipMarker()
{
    return hasRegistryOwnershipMarker() && hasLocalOwnershipMarker();
}

static bool writeRegistryOwnershipMarker(std::string* log = nullptr)
{
    HKEY key = NULL;
    DWORD disposition = 0;
    LSTATUS ret = RegCreateKeyExW(HKEY_LOCAL_MACHINE, OWNERSHIP_REG_KEY, 0, NULL, 0,
                                  KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &key, &disposition);
    if (ret == ERROR_INVALID_PARAMETER) {
        ret = RegCreateKeyExW(HKEY_LOCAL_MACHINE, OWNERSHIP_REG_KEY, 0, NULL, 0,
                              KEY_SET_VALUE, NULL, &key, &disposition);
    }
    if (ret != ERROR_SUCCESS) {
        if (log) {
            *log += " Warning: could not write ownership marker (Win32 error=" +
                    std::to_string(ret) + ").";
        }
        return false;
    }

    DWORD installed = 1;
    RegSetValueExW(key, OWNERSHIP_VALUE_INSTALLED, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&installed), sizeof(installed));
    RegSetValueExW(key, OWNERSHIP_VALUE_HWID, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(FXVAD_HARDWARE_ID),
                   static_cast<DWORD>((wcslen(FXVAD_HARDWARE_ID) + 1) * sizeof(wchar_t)));
    RegSetValueExW(key, OWNERSHIP_VALUE_DEVICE, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(DFX_DEVICE_NAME),
                   static_cast<DWORD>((wcslen(DFX_DEVICE_NAME) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return true;
}

static bool writeLocalOwnershipMarker(std::string* log = nullptr)
{
    std::wstring marker_path = getDriverOwnershipMarkerPath();
    FILE* fp = _wfopen(marker_path.c_str(), L"wb");
    if (!fp) {
        if (log) {
            *log += " Warning: could not write local ownership marker: " +
                    wide_to_utf8(marker_path) + ".";
        }
        return false;
    }

    SYSTEMTIME st = {};
    GetSystemTime(&st);
    char json[768] = {};
    snprintf(json, sizeof(json),
             "{\n"
             "  \"owner\":\"ListeningWind\",\n"
             "  \"purpose\":\"FXVAD ownership marker for uninstall protection\",\n"
             "  \"hardware_id\":\"Root\\\\FXVAD\",\n"
             "  \"device_name\":\"FxSound Audio Enhancer\",\n"
             "  \"created_utc\":\"%04u-%02u-%02uT%02u:%02u:%02uZ\"\n"
             "}\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    fputs(json, fp);
    fclose(fp);
    return true;
}

static bool writeOwnershipMarker(std::string* log = nullptr)
{
    const bool registry_ok = writeRegistryOwnershipMarker(log);
    const bool file_ok = writeLocalOwnershipMarker(log);
    return registry_ok && file_ok;
}

static void clearOwnershipMarker()
{
    LSTATUS ret = RegDeleteTreeW(HKEY_LOCAL_MACHINE, OWNERSHIP_REG_KEY);
    if (ret == ERROR_INVALID_PARAMETER || ret == ERROR_FILE_NOT_FOUND) {
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, OWNERSHIP_REG_KEY);
    }
    DeleteFileW(getDriverOwnershipMarkerPath().c_str());
}

static std::wstring lowerString(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return value;
}

static bool isKnownConflictProcess(const std::wstring& process_name)
{
    const std::wstring lower = lowerString(process_name);

    if (lower == L"fxsound_engine.exe" ||
        lower == L"fxsound-engine.exe" ||
        lower == L"listening_wind_app.exe") {
        return false;
    }

    if (lower == L"fxsound.exe" ||
        lower == L"fxsoundapp.exe" ||
        lower == L"fxsound_app.exe" ||
        lower == L"dfx.exe" ||
        lower == L"dfxui.exe" ||
        lower == L"dfxui_eng.exe") {
        return true;
    }

    if (lower.find(L"fxsound") != std::wstring::npos) {
        return true;
    }

    return lower.find(L"dfx") == 0 && lower.size() > 4 &&
           lower.rfind(L".exe") == lower.size() - 4;
}

bool isFxSoundConflictRunning(std::wstring* process_name)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    const DWORD current_pid = GetCurrentProcessId();
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);

    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == current_pid) {
                continue;
            }
            if (isKnownConflictProcess(entry.szExeFile)) {
                if (process_name) {
                    *process_name = entry.szExeFile;
                }
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
}

static bool enableDfxDevice()
{
    std::vector<AudioDeviceInfo> devices;
    if (!enumAudioOutputs(devices)) {
        return false;
    }

    for (const auto& dev : devices) {
        if (dev.device_name.find(DFX_DEVICE_NAME) == 0) {
            if (dev.state & DEVICE_STATE_DISABLED) {
                HKEY h_reg_key;
                std::wstring reg_path = REG_PATH_DEVICES + dev.device_guid;
                REGSAM access_mask = KEY_ALL_ACCESS | KEY_WOW64_64KEY;
                auto ret = RegOpenKeyEx(HKEY_LOCAL_MACHINE, reg_path.c_str(), 0, access_mask, &h_reg_key);
                if (ret == ERROR_SUCCESS) {
                    DWORD device_state = ENABLE_DEVICE;
                    RegSetValueEx(h_reg_key, L"DeviceState", NULL, REG_DWORD, (LPBYTE)&device_state, sizeof(DWORD));
                    RegCloseKey(h_reg_key);
                    Sleep(500);
                } else {
                    return false;
                }
            }
            return true;
        }
    }
    return false;
}

// ============================================================================
// Internal: CPU arch and OS detection
// ============================================================================

enum class CpuArch { Unknown = 0, x86, x64, ARM64 };

static CpuArch detectCpuArch()
{
    SYSTEM_INFO sys_info;
    GetNativeSystemInfo(&sys_info);

    if (sys_info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL)
        return CpuArch::x86;
    else if (sys_info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
        return CpuArch::x64;
    else if (sys_info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64)
        return CpuArch::ARM64;
    return CpuArch::Unknown;
}

static bool isWindows10OrLater()
{
    // Use RtlGetVersion for accurate version detection (VerifyVersionInfo lies in Win10+)
    typedef NTSTATUS(NTAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return true; // Assume Win10+ if we can't detect

    RtlGetVersionPtr RtlGetVersion = (RtlGetVersionPtr)GetProcAddress(ntdll, "RtlGetVersion");
    if (!RtlGetVersion) return true;

    RTL_OSVERSIONINFOW osvi = {};
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    RtlGetVersion(&osvi);

    return osvi.dwMajorVersion >= 10;
}

// ============================================================================
// Public API implementation
// ============================================================================

bool isAdmin()
{
    BOOL isAdmin = FALSE;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    PSID adminGroup = NULL;

    if (AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
        0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(NULL, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin != FALSE;
}

DriverStatusInfo queryDriverStatus()
{
    DriverStatusInfo info;

    AudioDeviceInfo device;
    const bool has_device = findDfxDevice(device);
    const bool owned = hasOwnershipMarker();

    std::wstring conflict_process;
    const bool has_conflict = isFxSoundConflictRunning(&conflict_process);
    if (has_conflict) {
        info.conflict_process = conflict_process;
    }

    if (!has_device) {
        if (owned) {
            clearOwnershipMarker();
        }
        info.result = has_conflict ? DRIVER_CONFLICT_RUNNING : DRIVER_NOT_INSTALLED;
        info.ownership = DRIVER_OWNERSHIP_UNKNOWN;
        return info;
    }

    info.device_name = device.device_name;
    info.hardware_id = FXVAD_HARDWARE_ID;
    info.ownership = owned ? DRIVER_OWNERSHIP_OWNED : DRIVER_OWNERSHIP_EXTERNAL;

    if (has_conflict) {
        info.result = DRIVER_CONFLICT_RUNNING;
        return info;
    }

    if (device.state & DEVICE_STATE_DISABLED) {
        info.result = DRIVER_DISABLED;
        return info;
    }

    info.result = DRIVER_OK;
    return info;
}

DriverResult installDriver(const std::wstring& driver_dir, std::string& log)
{
    AudioDeviceInfo existing_device;
    if (findDfxDevice(existing_device)) {
        const bool owned = hasOwnershipMarker();
        if (existing_device.state & DEVICE_STATE_DISABLED) {
            log = "Existing FxSound virtual audio device is disabled. Reusing it without reinstalling.";
            return DRIVER_DISABLED;
        }
        if (owned) {
            log = "Virtual audio device already installed by Listening Wind.";
            return DRIVER_ALREADY_INSTALLED;
        }
        log = "Existing FxSound virtual audio device detected. Reusing external driver without overwriting.";
        return DRIVER_EXTERNAL_INSTALLED;
    }

    if (!isAdmin()) {
        log = "Administrator privileges required. Please run as Administrator.";
        return DRIVER_NOT_ADMIN;
    }

    CpuArch arch = detectCpuArch();
    if (arch == CpuArch::Unknown) {
        log = "Unsupported CPU architecture.";
        return DRIVER_ARCH_UNSUPPORTED;
    }

    // Build driver path
    std::wstring driver_path = driver_dir;
    if (!driver_path.empty() && driver_path.back() != L'\\') {
        driver_path += L'\\';
    }

    // Determine subdirectory based on arch and OS
    if (isWindows10OrLater()) {
        if (arch == CpuArch::x86)
            driver_path += L"win10\\x86\\";
        else if (arch == CpuArch::x64)
            driver_path += L"win10\\x64\\";
        else if (arch == CpuArch::ARM64)
            driver_path += L"win10\\arm64\\";
    } else {
        if (arch == CpuArch::x86)
            driver_path += L"win7\\x86\\";
        else if (arch == CpuArch::x64)
            driver_path += L"win7\\x64\\";
        else {
            log = "ARM64 not supported on Windows 7.";
            return DRIVER_ARCH_UNSUPPORTED;
        }
    }

    std::wstring inf_path = driver_path + L"fxvad.inf";
    std::wstring sys_path = driver_path + L"fxvad.sys";

    // Build expected .cat file name based on arch (must match INF CatalogFile entries)
    std::wstring cat_path;
    if (arch == CpuArch::x64)
        cat_path = driver_path + L"fxvadntamd64.cat";
    else if (arch == CpuArch::x86)
        cat_path = driver_path + L"fxvadntx86.cat";
    else if (arch == CpuArch::ARM64)
        cat_path = driver_path + L"fxvadntarm64.cat";

    // Verify files exist
    if (GetFileAttributes(inf_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        log = "INF file not found: " + wide_to_utf8(inf_path);
        return DRIVER_INF_NOT_FOUND;
    }
    if (GetFileAttributes(sys_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        log = "SYS file not found: " + wide_to_utf8(sys_path);
        return DRIVER_INF_NOT_FOUND;
    }
    if (!cat_path.empty() && GetFileAttributes(cat_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        log = "CAT file not found: " + wide_to_utf8(cat_path);
        return DRIVER_INF_NOT_FOUND;
    }

    // Install using SetupDi APIs (with diagnostics)
    InstallDiag diag;
    int install_result = driverCmdInstall(inf_path.c_str(), FXVAD_HARDWARE_ID, &diag);
    if (install_result == 0 || install_result == 1) {
        log = install_result == 1 ? "Driver installed. Reboot required." : "Driver installed successfully.";

        // Wait for device to appear
        Sleep(1000);

        // Enable the device if disabled
        if (!enableDfxDevice()) {
            log += " Warning: could not auto-enable device. It may need manual enablement.";
        }

        // Set power override to prevent system from suspending audio
        {
            std::wstring powercmd = L"powercfg -REQUESTSOVERRIDE DRIVER \"FxSound Audio Enhancer\" SYSTEM";
            STARTUPINFO si = {};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi = {};
            wchar_t* cmd = _wcsdup(powercmd.c_str());
            CreateProcess(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
            free(cmd);
            if (pi.hProcess) {
                WaitForSingleObject(pi.hProcess, 5000);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            }
        }

        writeOwnershipMarker(&log);

        return install_result == 1 ? DRIVER_REBOOT_REQUIRED : DRIVER_OK;
    }

    // Detailed error: step number + Win32 error code + description
    log = "Step " + std::to_string(diag.last_step) + " failed: " + diag.message +
          " (Win32 error=" + std::to_string(diag.last_error) + ")";
    return DRIVER_FAIL;
}

DriverResult uninstallDriver(std::string& log)
{
    if (!isAdmin()) {
        log = "Administrator privileges required. Please run as Administrator.";
        return DRIVER_NOT_ADMIN;
    }

    AudioDeviceInfo existing_device;
    if (!findDfxDevice(existing_device)) {
        clearOwnershipMarker();
        log = "Virtual audio device not found (already uninstalled).";
        return DRIVER_NOT_INSTALLED;
    }

    if (!hasOwnershipMarker()) {
        log = "Existing FxSound virtual audio device is external. Listening Wind will not uninstall it.";
        return DRIVER_EXTERNAL_PROTECTED;
    }

    // Try removing with primary HWID first
    int remove_result = driverCmdRemove(FXVAD_HARDWARE_ID);
    if (remove_result != 0) {
        // Try alternate HWID
        remove_result = driverCmdRemove(FXVAD_HARDWARE_ID_ALT);
    }

    if (remove_result == 0) {
        clearOwnershipMarker();
        log = "Virtual audio device uninstalled successfully.";
        return DRIVER_OK;
    }

    log = "Failed to uninstall virtual audio device. Error: " + std::to_string(GetLastError());
    return DRIVER_FAIL;
}

DriverResult checkDriverStatus()
{
    return queryDriverStatus().result;
}

std::wstring findDriverDirectory(const std::wstring& exe_dir)
{
    // Look for drivers subdirectory relative to the executable.
    // Two supported layouts:
    //   1) exe_dir\drivers\win10\x64\fxvad.inf         (flat)
    //   2) exe_dir\drivers\Version14\win10\x64\fxvad.inf (versioned)
    // If flat layout doesn't have the INF, search Version* subdirectories.

    auto endsWithSep = [](const std::wstring& s) -> bool {
        return !s.empty() && (s.back() == L'\\' || s.back() == L'/');
    };

    // Helper: check if a driver INF exists at the expected arch path
    auto hasInf = [](const std::wstring& base) -> bool {
        // Detect arch/OS suffix
        CpuArch arch = detectCpuArch();
        std::wstring suffix;
        if (isWindows10OrLater()) {
            if (arch == CpuArch::x64)    suffix = L"win10\\x64\\";
            else if (arch == CpuArch::x86)  suffix = L"win10\\x86\\";
            else if (arch == CpuArch::ARM64) suffix = L"win10\\arm64\\";
        } else {
            if (arch == CpuArch::x64)    suffix = L"win7\\x64\\";
            else if (arch == CpuArch::x86)  suffix = L"win7\\x86\\";
        }
        if (suffix.empty()) return false;

        std::wstring inf_path = base;
        if (!inf_path.empty() && inf_path.back() != L'\\') inf_path += L'\\';
        inf_path += suffix + L"fxvad.inf";

        DWORD attrs = GetFileAttributesW(inf_path.c_str());
        return (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY));
    };

    std::wstring base = exe_dir;
    if (!endsWithSep(base)) base += L'\\';
    base += L"drivers";

    // Try flat layout first
    if (hasInf(base)) {
        return base;
    }

    // Search Version* subdirectories (newest first)
    WIN32_FIND_DATAW findData;
    std::wstring searchPattern = base + L"\\Version*";
    HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &findData);

    std::vector<std::wstring> versionDirs;
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0)
                    continue;
                versionDirs.push_back(base + L'\\' + findData.cFileName);
            }
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
    }

    // Sort descending so highest version number is tried first
    std::sort(versionDirs.begin(), versionDirs.end(), std::greater<std::wstring>());

    for (const auto& dir : versionDirs) {
        if (hasInf(dir)) {
            return dir;
        }
    }

    // Fallback: return base directory even if INF not found
    // (installDriver will report DRIVER_INF_NOT_FOUND)
    return base;
}

const char* driverResultToString(DriverResult result)
{
    switch (result) {
    case DRIVER_OK:                return "OK";
    case DRIVER_REBOOT_REQUIRED:   return "Reboot required";
    case DRIVER_FAIL:              return "Failed";
    case DRIVER_ALREADY_INSTALLED: return "Already installed";
    case DRIVER_NOT_INSTALLED:     return "Not installed";
    case DRIVER_NOT_ADMIN:         return "Administrator required";
    case DRIVER_INF_NOT_FOUND:     return "INF not found";
    case DRIVER_ARCH_UNSUPPORTED:  return "Architecture unsupported";
    case DRIVER_DISABLED:          return "Disabled";
    case DRIVER_CONFLICT_RUNNING:  return "Conflict";
    case DRIVER_EXTERNAL_INSTALLED:return "External installed";
    case DRIVER_EXTERNAL_PROTECTED:return "External protected";
    default:                       return "Unknown";
    }
}

const char* driverStatusResultToString(DriverResult result)
{
    switch (result) {
    case DRIVER_OK:
        return "OK";
    case DRIVER_CONFLICT_RUNNING:
        return "Conflict";
    case DRIVER_DISABLED:
        return "Disabled";
    case DRIVER_NOT_INSTALLED:
    case DRIVER_FAIL:
    default:
        return "NotInstalled";
    }
}

const char* driverOwnershipToString(DriverOwnership ownership)
{
    switch (ownership) {
    case DRIVER_OWNERSHIP_OWNED:
        return "owned";
    case DRIVER_OWNERSHIP_EXTERNAL:
        return "external";
    case DRIVER_OWNERSHIP_UNKNOWN:
    default:
        return "unknown";
    }
}
