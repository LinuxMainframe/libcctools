/*
 * modulecheck.c
 * 
 * Author: Aidan A. Bradley
 * Date: 09/30/2025
 *
 * Purpose: Robustly check if required kernel modules are loaded or available
 * across multiple Linux distributions. Handles module aliases, dependencies,
 * and the complexities of kernel module naming (e.g., v4l2 vs videodev).
 * 
 * Design Philosophy:
 * - Handle module aliases (one feature, many names)
 * - Check both loaded modules and available modules
 * - Support module families (v4l2loopback, snd-*, etc.)
 * - JSON-based configuration with flexible naming
 * 
 * Compilation: gcc -o modulecheck modulecheck.c -lcjson -Wall
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>
#include <sys/utsname.h>
#include "cJSON.h"

#define MAX_PATH 4096
#define MAX_CMD 8192
#define MAX_MODULE_NAME 256
#define MAX_ALIASES 10

/*
 * Module structure
 * Stores information about a kernel module being checked
 * 
 * Handles complex scenarios like:
 * - v4l2loopback (exact name)
 * - videodev (alias for v4l2 core)
 * - snd-* (sound card family)
 */
typedef struct {
    char name[MAX_MODULE_NAME];              // Primary module name
    char aliases[MAX_ALIASES][MAX_MODULE_NAME];  // Alternative names
    int alias_count;                          // Number of aliases
    char found_as[MAX_MODULE_NAME];          // Which name was actually found
    char path[MAX_PATH];                     // Path to .ko file if available
    int loaded;                              // Is module currently loaded?
    int available;                           // Is module available to load?
    int builtin;                             // Is module built into kernel?
} Module;

/*
 * get_kernel_version()
 * 
 * Gets the running kernel version for finding module paths.
 * 
 * Why this matters:
 * - Modules are stored in /lib/modules/<kernel-version>/
 * - Different kernel versions have different modules
 * - Needed for module availability checks
 */
int get_kernel_version(char *version, size_t len) {
    struct utsname uts;
    
    if (uname(&uts) != 0) {
        return 0;
    }
    
    strncpy(version, uts.release, len - 1);
    version[len - 1] = '\0';
    return 1;
}

/*
 * is_module_loaded()
 * 
 * Checks if a module is currently loaded in the kernel.
 * 
 * Strategy:
 * 1. Check /proc/modules (list of loaded modules)
 * 2. Use lsmod command as fallback
 * 
 * /proc/modules format:
 * module_name size used_by_count [dependencies] state address
 * 
 * Example:
 * v4l2loopback 45056 0 - Live 0xffffffffc0a3e000
 * videodev 274432 2 v4l2loopback,uvcvideo Live 0xffffffffc09f1000
 */
int is_module_loaded(const char *module_name) {
    FILE *fp;
    char line[512];
    char search_name[MAX_MODULE_NAME];
    
    // Normalize module name (replace - with _)
    // Kernel uses underscores internally but module names can use hyphens
    strncpy(search_name, module_name, sizeof(search_name) - 1);
    search_name[sizeof(search_name) - 1] = '\0';
    
    for (char *p = search_name; *p; p++) {
        if (*p == '-') *p = '_';
    }
    
    /*
     * METHOD 1: Read /proc/modules directly
     * This is the most reliable method
     */
    fp = fopen("/proc/modules", "r");
    if (fp != NULL) {
        while (fgets(line, sizeof(line), fp)) {
            char loaded_module[MAX_MODULE_NAME];
            
            // Extract first field (module name)
            if (sscanf(line, "%255s", loaded_module) == 1) {
                // Normalize loaded module name too
                for (char *p = loaded_module; *p; p++) {
                    if (*p == '-') *p = '_';
                }
                
                if (strcmp(loaded_module, search_name) == 0) {
                    fclose(fp);
                    return 1;
                }
            }
        }
        fclose(fp);
    }
    
    /*
     * METHOD 2: Use lsmod command
     * Fallback if /proc/modules isn't accessible
     */
    char cmd[MAX_CMD];
    snprintf(cmd, sizeof(cmd), "lsmod 2>/dev/null | grep -q '^%s '", search_name);
    if (system(cmd) == 0) {
        return 1;
    }
    
    return 0;
}

/*
 * is_module_builtin()
 * 
 * Checks if a module is compiled into the kernel (not loadable).
 * 
 * Why this matters:
 * - Built-in modules don't appear in lsmod
 * - They don't need to be loaded (already in kernel)
 * - Common for essential drivers (ext4, tcp, etc.)
 * 
 * Location: /lib/modules/<kernel>/modules.builtin
 * Format: kernel/drivers/media/v4l2-core/videodev.ko
 */
int is_module_builtin(const char *module_name, const char *kernel_version) {
    char builtin_path[MAX_PATH];
    FILE *fp;
    char line[MAX_PATH];
    
    snprintf(builtin_path, sizeof(builtin_path), 
             "/lib/modules/%s/modules.builtin", kernel_version);
    
    fp = fopen(builtin_path, "r");
    if (fp == NULL) {
        return 0;
    }
    
    // Normalize search name
    char search_name[MAX_MODULE_NAME];
    strncpy(search_name, module_name, sizeof(search_name) - 1);
    for (char *p = search_name; *p; p++) {
        if (*p == '-') *p = '_';
    }
    
    while (fgets(line, sizeof(line), fp)) {
        // Extract filename from path
        char *filename = strrchr(line, '/');
        if (filename) {
            filename++; // Skip the '/'
            
            // Remove .ko extension
            char *ext = strstr(filename, ".ko");
            if (ext) {
                *ext = '\0';
            }
            
            // Normalize
            for (char *p = filename; *p; p++) {
                if (*p == '-') *p = '_';
            }
            
            if (strcmp(filename, search_name) == 0) {
                fclose(fp);
                return 1;
            }
        }
    }
    
    fclose(fp);
    return 0;
}

/*
 * find_module_file()
 * 
 * Searches for the .ko (kernel object) file for a module.
 * 
 * Search locations:
 * 1. /lib/modules/<kernel>/kernel/ (standard location)
 * 2. /lib/modules/<kernel>/extra/ (third-party modules)
 * 3. /lib/modules/<kernel>/updates/ (distribution updates)
 * 
 * Why this matters:
 * - Confirms module is available to load
 * - Can check module dependencies
 * - Useful for troubleshooting
 */
int find_module_file(const char *module_name, const char *kernel_version, char *result_path) {
    char search_paths[3][MAX_PATH];
    char cmd[MAX_CMD];
    FILE *fp;
    
    snprintf(search_paths[0], MAX_PATH, "/lib/modules/%s/kernel", kernel_version);
    snprintf(search_paths[1], MAX_PATH, "/lib/modules/%s/extra", kernel_version);
    snprintf(search_paths[2], MAX_PATH, "/lib/modules/%s/updates", kernel_version);
    
    // Normalize module name for search
    char search_name[MAX_MODULE_NAME];
    strncpy(search_name, module_name, sizeof(search_name) - 1);
    for (char *p = search_name; *p; p++) {
        if (*p == '-') *p = '_';
    }
    
    // Search each path hierarchy
    for (int i = 0; i < 3; i++) {
        // Use find command to search recursively
        snprintf(cmd, sizeof(cmd), 
                 "find %s -name '%s.ko*' 2>/dev/null | head -1", 
                 search_paths[i], search_name);
        
        fp = popen(cmd, "r");
        if (fp != NULL) {
            if (fgets(result_path, MAX_PATH, fp) != NULL) {
                // Remove newline
                result_path[strcspn(result_path, "\n")] = 0;
                pclose(fp);
                
                if (strlen(result_path) > 0 && access(result_path, F_OK) == 0) {
                    return 1;
                }
            }
            pclose(fp);
        }
    }
    
    // Try modinfo as fallback
    snprintf(cmd, sizeof(cmd), "modinfo -F filename %s 2>/dev/null", search_name);
    fp = popen(cmd, "r");
    if (fp != NULL) {
        if (fgets(result_path, MAX_PATH, fp) != NULL) {
            result_path[strcspn(result_path, "\n")] = 0;
            int status = pclose(fp);
            
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && 
                strlen(result_path) > 0) {
                return 1;
            }
        } else {
            pclose(fp);
        }
    }
    
    return 0;
}

/*
 * check_module_by_modinfo()
 * 
 * Uses modinfo command to get detailed module information.
 * 
 * Why this is useful:
 * - Shows module aliases
 * - Shows dependencies
 * - Confirms module exists in system
 * 
 * modinfo output includes:
 * - filename: /lib/modules/.../module.ko
 * - alias: alternative names
 * - depends: required modules
 * - description: what the module does
 */
int check_module_by_modinfo(const char *module_name, Module *mod) {
    char cmd[MAX_CMD];
    FILE *fp;
    char line[512];
    
    snprintf(cmd, sizeof(cmd), "modinfo %s 2>/dev/null", module_name);
    fp = popen(cmd, "r");
    
    if (fp == NULL) {
        return 0;
    }
    
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "filename:", 9) == 0) {
            found = 1;
            // Extract filename
            char *filename = line + 9;
            while (*filename == ' ' || *filename == '\t') filename++;
            
            strncpy(mod->path, filename, MAX_PATH - 1);
            mod->path[strcspn(mod->path, "\n")] = 0;
        }
    }
    
    int status = pclose(fp);
    return found && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/*
 * find_module()
 * 
 * Main search function - tries multiple strategies to find a module.
 * 
 * Search strategy:
 * 1. Check if loaded (is_module_loaded)
 * 2. Check if built-in (is_module_builtin)
 * 3. Search by primary name
 * 4. Try all aliases
 * 5. Use modinfo for confirmation
 * 6. Find .ko file location
 * 
 * Module naming complexity:
 * - v4l2loopback: exact match required
 * - videodev: core v4l2 module
 * - snd_hda_intel: sound card (underscores vs hyphens)
 */
int find_module(Module *mod, const char *kernel_version) {
    // Initialize
    mod->loaded = 0;
    mod->available = 0;
    mod->builtin = 0;
    mod->found_as[0] = '\0';
    mod->path[0] = '\0';
    
    /*
     * STRATEGY 1: Check if currently loaded
     * Start with primary name
     */
    if (is_module_loaded(mod->name)) {
        mod->loaded = 1;
        mod->available = 1;
        strncpy(mod->found_as, mod->name, sizeof(mod->found_as) - 1);
        
        // Try to get module file path
        check_module_by_modinfo(mod->name, mod);
        return 1;
    }
    
    /*
     * STRATEGY 2: Try all aliases
     * Module might be loaded under different name
     */
    for (int i = 0; i < mod->alias_count; i++) {
        if (is_module_loaded(mod->aliases[i])) {
            mod->loaded = 1;
            mod->available = 1;
            strncpy(mod->found_as, mod->aliases[i], sizeof(mod->found_as) - 1);
            
            check_module_by_modinfo(mod->aliases[i], mod);
            return 1;
        }
    }
    
    /*
     * STRATEGY 3: Check if built into kernel
     * Built-in modules are always "available"
     */
    if (is_module_builtin(mod->name, kernel_version)) {
        mod->builtin = 1;
        mod->available = 1;
        mod->loaded = 1; // Built-in = always loaded
        strncpy(mod->found_as, mod->name, sizeof(mod->found_as) - 1);
        strcpy(mod->path, "[built-in]");
        return 1;
    }
    
    // Check aliases for built-in
    for (int i = 0; i < mod->alias_count; i++) {
        if (is_module_builtin(mod->aliases[i], kernel_version)) {
            mod->builtin = 1;
            mod->available = 1;
            mod->loaded = 1;
            strncpy(mod->found_as, mod->aliases[i], sizeof(mod->found_as) - 1);
            strcpy(mod->path, "[built-in]");
            return 1;
        }
    }
    
    /*
     * STRATEGY 4: Search for module file (not loaded but available)
     * Check primary name first
     */
    if (find_module_file(mod->name, kernel_version, mod->path)) {
        mod->available = 1;
        strncpy(mod->found_as, mod->name, sizeof(mod->found_as) - 1);
        return 1;
    }
    
    // Try aliases
    for (int i = 0; i < mod->alias_count; i++) {
        if (find_module_file(mod->aliases[i], kernel_version, mod->path)) {
            mod->available = 1;
            strncpy(mod->found_as, mod->aliases[i], sizeof(mod->found_as) - 1);
            return 1;
        }
    }
    
    /*
     * STRATEGY 5: Use modinfo as final check
     * Sometimes modules exist but are in non-standard locations
     */
    if (check_module_by_modinfo(mod->name, mod)) {
        mod->available = 1;
        strncpy(mod->found_as, mod->name, sizeof(mod->found_as) - 1);
        return 1;
    }
    
    for (int i = 0; i < mod->alias_count; i++) {
        if (check_module_by_modinfo(mod->aliases[i], mod)) {
            mod->available = 1;
            strncpy(mod->found_as, mod->aliases[i], sizeof(mod->found_as) - 1);
            return 1;
        }
    }
    
    return 0;
}

/*
 * check_modules_from_json()
 * 
 * Parses JSON and checks multiple modules.
 * 
 * JSON format:
 * {
 *   "modules": [
 *     {
 *       "name": "v4l2loopback",
 *       "aliases": []
 *     },
 *     {
 *       "name": "videodev",
 *       "aliases": ["v4l2_core"]
 *     }
 *   ]
 * }
 * 
 * Flexible format also supported:
 * {
 *   "modules": ["v4l2loopback", "videodev"]
 * }
 */
int check_modules_from_json(const char *json_str) {
    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        fprintf(stderr, "Error parsing JSON: %s\n", cJSON_GetErrorPtr());
        return -1;
    }
    
    // Get kernel version
    char kernel_version[256];
    if (!get_kernel_version(kernel_version, sizeof(kernel_version))) {
        fprintf(stderr, "Failed to get kernel version\n");
        cJSON_Delete(root);
        return -1;
    }
    
    printf("Kernel version: %s\n", kernel_version);
    
    cJSON *modules = cJSON_GetObjectItem(root, "modules");
    if (modules == NULL || !cJSON_IsArray(modules)) {
        fprintf(stderr, "No modules array found in JSON\n");
        cJSON_Delete(root);
        return -1;
    }
    
    int total = cJSON_GetArraySize(modules);
    int loaded_count = 0;
    int available_count = 0;
    
    printf("Checking %d modules...\n\n", total);
    
    for (int i = 0; i < total; i++) {
        cJSON *item = cJSON_GetArrayItem(modules, i);
        Module mod;
        memset(&mod, 0, sizeof(Module));
        
        // Handle both string and object formats
        if (cJSON_IsString(item)) {
            // Simple string format
            const char *name = cJSON_GetStringValue(item);
            if (name == NULL) continue;
            
            strncpy(mod.name, name, sizeof(mod.name) - 1);
            mod.alias_count = 0;
        } else if (cJSON_IsObject(item)) {
            // Object format with aliases
            cJSON *name_obj = cJSON_GetObjectItem(item, "name");
            if (name_obj == NULL || !cJSON_IsString(name_obj)) continue;
            
            const char *name = cJSON_GetStringValue(name_obj);
            strncpy(mod.name, name, sizeof(mod.name) - 1);
            
            // Get aliases
            cJSON *aliases = cJSON_GetObjectItem(item, "aliases");
            if (aliases != NULL && cJSON_IsArray(aliases)) {
                int alias_count = cJSON_GetArraySize(aliases);
                mod.alias_count = (alias_count < MAX_ALIASES) ? alias_count : MAX_ALIASES;
                
                for (int j = 0; j < mod.alias_count; j++) {
                    cJSON *alias = cJSON_GetArrayItem(aliases, j);
                    if (cJSON_IsString(alias)) {
                        const char *alias_name = cJSON_GetStringValue(alias);
                        strncpy(mod.aliases[j], alias_name, sizeof(mod.aliases[j]) - 1);
                    }
                }
            } else {
                mod.alias_count = 0;
            }
        } else {
            continue;
        }
        
        // Check the module
        printf("[%d/%d] %s: ", i + 1, total, mod.name);
        
        if (find_module(&mod, kernel_version)) {
            if (mod.loaded) {
                printf("✓ LOADED");
                loaded_count++;
                available_count++;
                
                if (mod.builtin) {
                    printf(" (built-in)");
                } else if (strcmp(mod.found_as, mod.name) != 0) {
                    printf(" as '%s'", mod.found_as);
                }
                
                if (strlen(mod.path) > 0) {
                    printf("\n  %s", mod.path);
                }
                printf("\n");
            } else if (mod.available) {
                printf("○ AVAILABLE (not loaded)\n");
                available_count++;
                
                if (strlen(mod.path) > 0) {
                    printf("  %s\n", mod.path);
                }
            }
        } else {
            printf("✗ NOT FOUND\n");
        }
    }
    
    printf("\n========================================\n");
    printf("Summary:\n");
    printf("  Loaded: %d/%d\n", loaded_count, total);
    printf("  Available: %d/%d\n", available_count, total);
    printf("========================================\n");
    
    cJSON_Delete(root);
    
    // Return 0 if all modules are at least available
    return (available_count == total) ? 0 : 1;
}

int main(int argc, char *argv[]) {
    const char *json_example = 
        "{"
        "  \"modules\": ["
        "    {"
        "      \"name\": \"v4l2loopback\","
        "      \"aliases\": []"
        "    },"
        "    {"
        "      \"name\": \"videodev\","
        "      \"aliases\": [\"v4l2_core\"]"
        "    },"
        "    {"
        "      \"name\": \"snd_hda_intel\","
        "      \"aliases\": [\"snd-hda-intel\"]"
        "    }"
        "  ]"
        "}";
    
    if (argc > 1) {
        FILE *fp = fopen(argv[1], "r");
        if (fp == NULL) {
            fprintf(stderr, "Cannot open file: %s\n", argv[1]);
            return 1;
        }
        
        fseek(fp, 0, SEEK_END);
        long size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        
        char *json_str = malloc(size + 1);
        if (json_str == NULL) {
            fprintf(stderr, "Memory allocation failed\n");
            fclose(fp);
            return 1;
        }
        
        size_t read_size = fread(json_str, 1, size, fp);
        json_str[read_size] = '\0';
        fclose(fp);
        
        int result = check_modules_from_json(json_str);
        free(json_str);
        
        return result;
    } else {
        return check_modules_from_json(json_example);
    }
}