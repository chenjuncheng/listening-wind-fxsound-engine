/*
 * driver_manager.h
 *
 * Virtual audio device driver management for FxSound Engine.
 * Provides install/uninstall/check functionality for the fxvad.sys driver.
 *
 * The driver installation uses SetupDi APIs directly (no external fxdevcon.exe needed).
 * Must run as Administrator for install/uninstall operations.
 */

#ifndef _DRIVER_MANAGER_H_
#define _DRIVER_MANAGER_H_

#include <string>

// Driver install/uninstall result codes
enum DriverResult {
    DRIVER_OK = 0,
    DRIVER_REBOOT_REQUIRED = 1,
    DRIVER_FAIL = 2,
    DRIVER_ALREADY_INSTALLED = 3,
    DRIVER_NOT_INSTALLED = 4,
    DRIVER_NOT_ADMIN = 5,
    DRIVER_INF_NOT_FOUND = 6,
    DRIVER_ARCH_UNSUPPORTED = 7,
    DRIVER_DISABLED = 8,
    DRIVER_CONFLICT_RUNNING = 9,
    DRIVER_EXTERNAL_INSTALLED = 10,
    DRIVER_EXTERNAL_PROTECTED = 11,
};

enum DriverOwnership {
    DRIVER_OWNERSHIP_UNKNOWN = 0,
    DRIVER_OWNERSHIP_OWNED = 1,
    DRIVER_OWNERSHIP_EXTERNAL = 2,
};

struct DriverStatusInfo {
    DriverResult result = DRIVER_NOT_INSTALLED;
    DriverOwnership ownership = DRIVER_OWNERSHIP_UNKNOWN;
    std::wstring device_name;
    std::wstring hardware_id;
    std::wstring conflict_process;
};

/*
 * Install the virtual audio device driver (fxvad.sys).
 *
 * driver_dir: Path to the directory containing fxvad.inf and fxvad.sys
 *             (e.g., "C:\Program Files\MyApp\drivers\win10\x64\")
 * log: Output log string with details
 *
 * Returns: DRIVER_OK, DRIVER_REBOOT_REQUIRED, DRIVER_ALREADY_INSTALLED,
 *          DRIVER_FAIL, DRIVER_NOT_ADMIN, DRIVER_INF_NOT_FOUND
 */
DriverResult installDriver(const std::wstring& driver_dir, std::string& log);

/*
 * Uninstall the virtual audio device driver.
 *
 * log: Output log string with details
 *
 * Returns: DRIVER_OK, DRIVER_REBOOT_REQUIRED, DRIVER_NOT_INSTALLED,
 *          DRIVER_FAIL, DRIVER_NOT_ADMIN
 */
DriverResult uninstallDriver(std::string& log);

/*
 * Check if the virtual audio device driver is installed and enabled.
 *
 * Returns: DRIVER_OK (installed & enabled), DRIVER_NOT_INSTALLED, DRIVER_FAIL
 */
DriverResult checkDriverStatus();

/*
 * Query detailed driver status for UI/protocol output.
 *
 * result: OK, DRIVER_NOT_INSTALLED, DRIVER_DISABLED, or DRIVER_CONFLICT_RUNNING
 * ownership: owned only when this app wrote an ownership marker during install
 * device_name/hardware_id: populated when an FxSound virtual device is present
 */
DriverStatusInfo queryDriverStatus();

/*
 * Return true if a known FxSound/DFX user-mode app is running and may compete
 * for the virtual device/default playback route.
 */
bool isFxSoundConflictRunning(std::wstring* process_name = nullptr);

/*
 * Local ownership marker file written beside fxsound_engine.exe.
 * This is only auxiliary evidence; uninstall remains protected by ownership checks.
 */
std::wstring getDriverOwnershipMarkerPath();

/*
 * Get the driver directory path relative to the executable.
 * Looks for drivers in <exe_dir>/drivers/ based on CPU arch and OS version.
 */
std::wstring findDriverDirectory(const std::wstring& exe_dir);

/*
 * Check if current process has admin privileges.
 */
bool isAdmin();

/*
 * Get human-readable string for DriverResult code.
 */
const char* driverResultToString(DriverResult result);

/*
 * Protocol strings for --check-driver. Keep these stable for Flutter:
 * OK | NotInstalled | Conflict | Disabled
 */
const char* driverStatusResultToString(DriverResult result);
const char* driverOwnershipToString(DriverOwnership ownership);

#endif // _DRIVER_MANAGER_H_
