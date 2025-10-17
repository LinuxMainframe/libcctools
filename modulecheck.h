/*
 * modulecheck.h
 *
 * Author: Aidan A. Bradley
 * Date: 09/30/2025
 * 
 * Header file for Linux Kernel Module Checker
 * 
 * This header provides the public API for checking kernel modules.
 * Handles complex module naming scenarios like v4l2 (videodev) and
 * module families with different names.
 * 
 * Usage:
 *   #include "modulecheck.h"
 *   
 *   // Check single module with aliases
 *   Module mod;
 *   strcpy(mod.name, "videodev");
 *   strcpy(mod.aliases[0], "v4l2_core");
 *   mod.alias_count = 1;
 *   
 *   char kernel[256];
 *   get_kernel_version(kernel, sizeof(kernel));
 *   
 *   if (find_module(&mod, kernel)) {
 *       if (mod.loaded) {
 *           printf("Module is loaded as: %s\n", mod.found_as);
 *       }
 *   }
 *   
 *   // Check from JSON
 *   const char *json = "{\"modules\": [{\"name\": \"v4l2loopback\", \"aliases\": []}]}";
 *   int result = check_modules_from_json(json);
 * 
 * Thread Safety: NOT thread-safe (uses system(), popen())
 */

#ifndef MODULECHECK_H
#define MODULECHECK_H

#include <stdio.h>
#include <stdlib.h>

/*
 * Version information
 */
#define MODULECHECK_VERSION_MAJOR 1
#define MODULECHECK_VERSION_MINOR 0
#define MODULECHECK_VERSION_PATCH 0
#define MODULECHECK_VERSION_STRING "1.0.0"

/*
 * Constants
 */
#define MAX_PATH 4096
#define MAX_CMD 8192
#define MAX_MODULE_NAME 256
#define MAX_ALIASES 10

/*
 * Module
 * 
 * Structure containing information about a kernel module.
 * 
 * Fields:
 * - name: Primary module name (e.g., "v4l2loopback")
 * - aliases: Alternative names the module might be known as
 *            (e.g., ["videodev", "v4l2_core"] for v4l2)
 * - alias_count: Number of aliases defined
 * - found_as: Which name (primary or alias) was actually found
 * - path: Full path to .ko file, or "[built-in]" for built-in modules
 * - loaded: Boolean - is module currently loaded in kernel?
 * - available: Boolean - is module available to load?
 * - builtin: Boolean - is module compiled into kernel?
 * 
 * Module Naming Complexity:
 * 
 * Kernel modules have several naming challenges:
 * 
 * 1. Hyphen vs Underscore:
 *    - User space: "snd-hda-intel"
 *    - Kernel space: "snd_hda_intel"
 *    Solution: Normalize by converting - to _
 * 
 * 2. Aliases:
 *    - v4l2 core can be "videodev" or "v4l2_core"
 *    - Different distros may use different names
 *    Solution: Provide aliases array
 * 
 * 3. Module Families:
 *    - v4l2loopback is distinct from videodev
 *    - Both are part of v4l2 but have different functions
 *    Solution: Check exact names when needed
 * 
 * 4. Built-in vs Loadable:
 *    - Built-in modules don't appear in lsmod
 *    - But they're always "loaded"
 *    Solution: Check modules.builtin file
 * 
 * Example Use Cases:
 * 
 * 1. v4l2loopback (exact match required):
 *    Module mod = {
 *        .name = "v4l2loopback",
 *        .alias_count = 0
 *    };
 * 
 * 2. videodev (v4l2 core with aliases):
 *    Module mod = {
 *        .name = "videodev",
 *        .aliases = {"v4l2_core"},
 *        .alias_count = 1
 *    };
 * 
 * 3. Sound card (hyphen variants):
 *    Module mod = {
 *        .name = "snd_hda_intel",
 *        .aliases = {"snd-hda-intel"},
 *        .alias_count = 1
 *    };
 * 
 * State Combinations:
 * - loaded=1, available=1: Module currently in use
 * - loaded=0, available=1: Module exists but not loaded
 * - loaded=0, available=0: Module not found
 * - builtin=1: Module is built-in (implies loaded=1, available=1)
 */
typedef struct {
    char name[MAX_MODULE_NAME];
    char aliases[MAX_ALIASES][MAX_MODULE_NAME];
    int alias_count;
    char found_as[MAX_MODULE_NAME];
    char path[MAX_PATH];
    int loaded;
    int available;
    int builtin;
} Module;

/*
 * ============================================================================
 * CORE API FUNCTIONS
 * ============================================================================
 */

/*
 * get_kernel_version()
 * 
 * Gets the running kernel version string.
 * 
 * Parameters:
 * - version: Buffer to store version string
 * - len: Size of buffer
 * 
 * Returns:
 * - 1: Success
 * - 0: Failed to get kernel version
 * 
 * Uses uname() system call to get kernel release string.
 * Example: "5.15.0-91-generic", "6.1.0-13-amd64"
 * 
 * This is critical for finding module paths:
 * /lib/modules/<kernel-version>/kernel/...
 * 
 * Thread safety: Safe (uname is thread-safe)
 * Performance: Very fast (<1ms)
 * 
 * Example:
 *   char kernel[256];
 *   if (get_kernel_version(kernel, sizeof(kernel))) {
 *       printf("Running kernel: %s\n", kernel);
 *   }
 */
int get_kernel_version(char *version, size_t len);

/*
 * find_module()
 * 
 * Main search function for finding a kernel module.
 * 
 * Parameters:
 * - mod: Pointer to Module structure (name and aliases must be filled)
 * - kernel_version: Kernel version string (from get_kernel_version)
 * 
 * Returns:
 * - 1: Module found (loaded or available)
 * - 0: Module not found
 * 
 * Side effects:
 * - Fills mod structure with results (loaded, available, builtin, path, found_as)
 * - May execute shell commands (lsmod, modinfo, find)
 * - Reads /proc/modules and /lib/modules files
 * 
 * Search strategy (in order):
 * 1. Check if loaded via /proc/modules
 * 2. Try all aliases for loaded modules
 * 3. Check if built into kernel (modules.builtin)
 * 4. Search for .ko files in kernel module directories
 * 5. Use modinfo as final verification
 * 
 * Thread safety: NOT thread-safe (uses system(), popen())
 * Performance: Moderate (50-200ms typically)
 * 
 * Example:
 *   Module mod;
 *   memset(&mod, 0, sizeof(Module));
 *   strcpy(mod.name, "v4l2loopback");
 *   mod.alias_count = 0;
 *   
 *   char kernel[256];
 *   get_kernel_version(kernel, sizeof(kernel));
 *   
 *   if (find_module(&mod, kernel)) {
 *       if (mod.loaded) {
 *           printf("Module is loaded\n");
 *       } else {
 *           printf("Module available but not loaded\n");
 *       }
 *   }
 */
int find_module(Module *mod, const char *kernel_version);

/*
 * check_modules_from_json()
 * 
 * Parses JSON and checks multiple modules.
 * Main high-level API for batch module checking.
 * 
 * Parameters:
 * - json_str: Null-terminated JSON string
 * 
 * Expected JSON formats:
 * 
 * Format 1 - Simple array:
 *   {
 *     "modules": ["module1", "module2"]
 *   }
 * 
 * Format 2 - Objects with aliases:
 *   {
 *     "modules": [
 *       {
 *         "name": "videodev",
 *         "aliases": ["v4l2_core"]
 *       },
 *       {
 *         "name": "v4l2loopback",
 *         "aliases": []
 *       }
 *     ]
 *   }
 * 
 * Returns:
 * -  0: All modules available (loaded or loadable)
 * -  1: Some modules not found
 * - -1: Error (invalid JSON, missing modules array)
 * 
 * Side effects:
 * - Prints progress to stdout
 * - Prints kernel version info
 * - Prints summary report
 * - May execute multiple shell commands
 * 
 * Output format:
 *   Kernel version: 5.15.0-91-generic
 *   Checking 2 modules...
 *   
 *   [1/2] v4l2loopback: ✓ LOADED
 *     /lib/modules/5.15.0-91-generic/kernel/drivers/media/v4l2loopback.ko
 *   [2/2] videodev: ✓ LOADED (built-in)
 *   
 *   ========================================
 *   Summary:
 *     Loaded: 2/2
 *     Available: 2/2
 *   ========================================
 * 
 * Thread safety: NOT thread-safe
 * Memory: Allocates temporary buffers (freed before return)
 * 
 * Example:
 *   const char *json = 
 *     "{"
 *     "  \"modules\": ["
 *     "    {\"name\": \"v4l2loopback\", \"aliases\": []},"
 *     "    {\"name\": \"snd_hda_intel\", \"aliases\": [\"snd-hda-intel\"]}"
 *     "  ]"
 *     "}";
 *   
 *   int result = check_modules_from_json(json);
 *   if (result == 0) {
 *       printf("All modules satisfied\n");
 *   }
 */
int check_modules_from_json(const char *json_str);

/*
 * ============================================================================
 * UTILITY FUNCTIONS
 * ============================================================================
 */

/*
 * is_module_loaded()
 * 
 * Checks if a module is currently loaded in the kernel.
 * 
 * Parameters:
 * - module_name: Name of module to check
 * 
 * Returns:
 * - 1: Module is loaded
 * - 0: Module is not loaded
 * 
 * Detection methods:
 * 1. Parse /proc/modules (most reliable)
 * 2. Use lsmod command (fallback)
 * 
 * Automatically normalizes names (converts - to _) to handle
 * both user-space and kernel-space naming conventions.
 * 
 * Thread safety: NOT thread-safe (uses system())
 * Performance: Fast (5-20ms)
 * 
 * Example:
 *   if (is_module_loaded("v4l2loopback")) {
 *       printf("v4l2loopback is currently loaded\n");
 *   }
 */
int is_module_loaded(const char *module_name);

/*
 * is_module_builtin()
 * 
 * Checks if a module is compiled into the kernel (not loadable).
 * 
 * Parameters:
 * - module_name: Name of module to check
 * - kernel_version: Kernel version string
 * 
 * Returns:
 * - 1: Module is built-in
 * - 0: Module is not built-in (might be loadable or not exist)
 * 
 * Built-in modules:
 * - Compiled directly into kernel image
 * - Cannot be loaded/unloaded
 * - Always "loaded"
 * - Don't appear in lsmod
 * - Listed in /lib/modules/<kernel>/modules.builtin
 * 
 * Common built-in modules:
 * - Core filesystem drivers (ext4)
 * - Essential network protocols (tcp, ip)
 * - Critical hardware support
 * 
 * Thread safety: Safe (read-only file operations)
 * Performance: Moderate (must parse modules.builtin file)
 * 
 * Example:
 *   char kernel[256];
 *   get_kernel_version(kernel, sizeof(kernel));
 *   
 *   if (is_module_builtin("ext4", kernel)) {
 *       printf("ext4 is built into the kernel\n");
 *   }
 */
int is_module_builtin(const char *module_name, const char *kernel_version);

/*
 * find_module_file()
 * 
 * Searches for the .ko (kernel object) file for a module.
 * 
 * Parameters:
 * - module_name: Name of module to find
 * - kernel_version: Kernel version string
 * - result_path: Buffer to store path (must be MAX_PATH size)
 * 
 * Returns:
 * - 1: Module file found
 * - 0: Module file not found
 * 
 * Side effects:
 * - Fills result_path with full path to .ko file
 * - May execute find and modinfo commands
 * 
 * Search locations (in order):
 * 1. /lib/modules/<kernel>/kernel/ - standard modules
 * 2. /lib/modules/<kernel>/extra/ - third-party modules
 * 3. /lib/modules/<kernel>/updates/ - distribution updates
 * 4. modinfo command - system-wide search
 * 
 * The .ko file might be compressed (.ko.gz, .ko.xz, .ko.zst)
 * depending on distribution. This function handles all variants.
 * 
 * Thread safety: NOT thread-safe (uses popen())
 * Performance: Slow (100-500ms for filesystem search)
 * 
 * Example:
 *   char path[MAX_PATH];
 *   char kernel[256];
 *   get_kernel_version(kernel, sizeof(kernel));
 *   
 *   if (find_module_file("v4l2loopback", kernel, path)) {
 *       printf("Module file: %s\n", path);
 *   }
 */
int find_module_file(const char *module_name, const char *kernel_version, char *result_path);

/*
 * check_module_by_modinfo()
 * 
 * Uses modinfo command to get detailed module information.
 * 
 * Parameters:
 * - module_name: Name of module to query
 * - mod: Pointer to Module structure to fill with info
 * 
 * Returns:
 * - 1: modinfo succeeded and found module
 * - 0: modinfo failed or module not found
 * 
 * Side effects:
 * - Fills mod->path with filename from modinfo output
 * - Executes modinfo command
 * 
 * modinfo provides:
 * - filename: path to .ko file
 * - description: what the module does
 * - author: who wrote it
 * - license: kernel license
 * - alias: alternative names
 * - depends: required modules
 * - parameters: runtime options
 * 
 * This is useful for:
 * - Confirming module exists
 * - Getting canonical path
 * - Finding dependencies
 * - Checking module metadata
 * 
 * Thread safety: NOT thread-safe (uses popen())
 * Performance: Moderate (50-150ms)
 * 
 * Example:
 *   Module mod;
 *   if (check_module_by_modinfo("v4l2loopback", &mod)) {
 *       printf("Module file: %s\n", mod.path);
 *   }
 */
int check_module_by_modinfo(const char *module_name, Module *mod);

/*
 * ============================================================================
 * ERROR CODES AND STATUS
 * ============================================================================
 */

/*
 * Return code definitions for check_modules_from_json()
 */
#define MODULECHECK_SUCCESS 0        /* All modules available */
#define MODULECHECK_MISSING_MODS 1   /* Some modules not found */
#define MODULECHECK_ERROR -1         /* Fatal error (invalid JSON, etc.) */

/*
 * ============================================================================
 * USAGE NOTES
 * ============================================================================
 * 
 * Compilation:
 *   gcc -o modulecheck modulecheck.c -lcjson -Wall -Wextra
 * 
 * Linking:
 *   Requires cJSON library: apt-get install libcjson-dev
 * 
 * Permissions:
 *   - Reading /proc/modules: any user
 *   - Reading /lib/modules: any user
 *   - Loading modules: requires root (not done by this tool)
 * 
 * Module Naming Best Practices:
 * 
 * 1. For exact modules (like v4l2loopback):
 *    {
 *      "name": "v4l2loopback",
 *      "aliases": []
 *    }
 * 
 * 2. For core modules with variant names:
 *    {
 *      "name": "videodev",
 *      "aliases": ["v4l2_core"]
 *    }
 * 
 * 3. For modules with hyphen/underscore variants:
 *    {
 *      "name": "snd_hda_intel",
 *      "aliases": ["snd-hda-intel"]
 *    }
 * 
 * 4. For module families, check each specifically:
 *    {
 *      "modules": [
 *        {"name": "snd_hda_intel", "aliases": []},
 *        {"name": "snd_hda_codec_hdmi", "aliases": []},
 *        {"name": "snd_hda_codec_realtek", "aliases": []}
 *      ]
 *    }
 * 
 * Common Module Scenarios:
 * 
 * Video4Linux (V4L2):
 *   - videodev: core V4L2 module (often built-in)
 *   - v4l2loopback: virtual video device (loadable)
 *   - uvcvideo: USB webcam support
 * 
 * Sound (ALSA):
 *   - snd: core sound system (built-in)
 *   - snd_hda_intel: Intel HD Audio
 *   - snd_usb_audio: USB audio devices
 * 
 * Network:
 *   - tun: virtual network interface
 *   - bridge: network bridging
 *   - veth: virtual ethernet
 * 
 * Filesystem:
 *   - ext4: often built-in
 *   - btrfs: might be loadable
 *   - fuse: userspace filesystem support
 * 
 * Thread Safety Summary:
 * - get_kernel_version(): Thread-safe
 * - is_module_builtin(): Thread-safe (read-only)
 * - All other functions: NOT thread-safe
 * 
 * For multi-threaded use, serialize calls with mutexes.
 * 
 * Performance Tips:
 * - Cache kernel version (doesn't change during runtime)
 * - Check loaded modules first (fastest)
 * - Batch checks with JSON format (more efficient output)
 * - Built-in check is faster than file search
 */

#endif /* MODULECHECK_H */