/*
 * file: network.c
 * author: Aidan A. Bradley
 * date: 10/08/2025 (mm/dd/yyyy)
 * version: 0.0.2
 * _____________________________ 
 * Networking API for setting up a WAN and LAN monitor.
 * 
 * This module provides an independent, thread-safe NetworkMonitor object for periodically
 * checking WAN (internet) and LAN (local network) connectivity. WAN checks attempt a TCP
 * connection to multiple configurable external hosts (defaults: Google, Cloudflare, Quad9, OpenDNS)
 * to verify internet reachability without requiring root privileges or sending unnecessary data—returns
 * UP if at least one succeeds for reliability. LAN checks auto-detect an interface with a default
 * gateway by parsing /proc/net/route, falling back to config or "lo" if none found, then use ioctl
 * to verify if it's up and running.
 * 
 * Design rationale: Prioritizes safety (no raw sockets/ICMP to avoid root requirements), speed
 * (timeouts and retries for <100ms checks), and configurability (setters for timeouts,
 * hosts, etc., to handle high-latency or proxied environments). Auto-detection reduces manual
 * config and handles dynamic networks. The background thread updates state asynchronously,
 * allowing main program loops to query status efficiently.
 * Proxy support is prepared for future HTTP-based checks (e.g., via libcurl) but not
 * active here to minimize dependencies.
 * 
 * Usage: Create via network_monitor_new(), query with getters, modify with setters,
 * and destroy when done. All access is thread-safe via internal mutex.
 */

#define _GNU_SOURCE  // Enable Linux extensions for IF_NAMESIZE, IFF_UP, usleep, etc.

// 1. Includes
#include <sys/socket.h>    // For socket operations
#include <sys/ioctl.h>     // For interface status checks
#include <sys/time.h>      // For struct timeval in timeouts
#include <netinet/in.h>    // For sockaddr_in
#include <arpa/inet.h>     // For inet_pton
#include <net/if.h>        // For ifreq and IFF flags
#include <unistd.h>        // For close, sleep, usleep
#include <fcntl.h>         // For fcntl (if non-blocking needed)
#include <errno.h>         // For errno
#include <string.h>        // For strncpy, memset
#include <stdlib.h>        // For malloc, free
#include <pthread.h>       // For pthreads
#include <time.h>          // For time_t and time
#include <stdio.h>         // For snprintf in to_string, fopen for /proc
#include <linux/route.h>
#include "network.h"       // For public API and types

// Maximum number of WAN servers to balance redundancy with check time
#define MAX_WAN_SERVERS 4

// Struct for WAN server (host/port pair)
typedef struct {
    char host[256];
    int port;
} WanServer;

// Private struct (hidden from users; they use NetworkMonitorHandle = void*)
typedef struct {
    // Configuration (modifiable via setters; defaults set in constructor)
    int timeout_ms;           // Socket timeout in milliseconds; caps check duration for responsiveness
    int check_interval_sec;   // Background check frequency in seconds; balances freshness vs. CPU use
    char proxy_url[256];      // Optional HTTP proxy; future-proof for proxied HTTP checks (currently unused)
    WanServer wan_servers[MAX_WAN_SERVERS];  // List of WAN test servers for redundancy
    int num_wan_servers;      // Number of active WAN servers
    char lan_interface[IF_NAMESIZE];  // Interface for LAN check; allows auto-detection or manual switching

    // State (updated by background thread; read via getters)
    bool wan_up;              // True if at least one WAN server is reachable
    bool lan_up;              // True if LAN interface is up/running
    time_t last_check_time;   // Timestamp of last successful/attempted check for staleness detection
    int last_error;           // Last errno or custom code; aids debugging without global state

    // Threading (detached thread for non-blocking periodic checks)
    pthread_t monitor_thread;  // Handle for the background thread
    pthread_mutex_t lock;      // Protects all fields from concurrent access
    bool running;             // Flag to signal thread shutdown for clean exit without pthread_cancel
} NetworkMonitor;

// --- Internal Helpers ---

/**
 * Auto-detect a LAN interface with a default gateway by parsing /proc/net/route.
 * 
 * Uses /proc/net/route as a standard, root-free method to efficiently find the default route
 * (destination 0.0.0.0 with gateway). Sets mon->lan_interface on success.
 * 
 * Returns true if detection succeeds, false otherwise (sets last_error on failure).
 */
static bool detect_lan_interface(NetworkMonitor* mon) {
    FILE* fp = fopen("/proc/net/route", "r");
    if (!fp) {
        mon->last_error = errno;
        return false;
    }

    char line[512];
    char iface[IF_NAMESIZE] = {0};
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        unsigned long dest, gw, flags;
        char dummy[256];
        if (sscanf(line, "%s %lx %lx %lx", dummy, &dest, &gw, &flags) != 4) continue;

        if (dest == 0 && (flags & RTF_GATEWAY) && (flags & RTF_UP) && gw != 0) {
            // Found default route with gateway; extract interface from first field
            sscanf(line, "%s", iface);
            found = true;
            break;
        }
    }
    fclose(fp);

    if (found) {
        strncpy(mon->lan_interface, iface, IF_NAMESIZE - 1);
        mon->lan_interface[IF_NAMESIZE - 1] = '\0';
        mon->last_error = 0;
        return true;
    } else {
        mon->last_error = 0;  // No error, just no gateway found
        return false;
    }
}

/**
 * Check WAN connectivity by attempting TCP connect to each configured wan_server.
 * 
 * Returns true if at least one connection succeeds, providing redundancy against
 * single-server failures. Applies timeouts and retries with exponential backoff
 * to handle transient network issues.
 */
static bool check_wan(NetworkMonitor* mon) {
    for (int i = 0; i < mon->num_wan_servers; i++) {
        WanServer* srv = &mon->wan_servers[i];
        struct sockaddr_in addr;
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            mon->last_error = errno;
            continue;
        }

        struct timeval tv = { .tv_sec = mon->timeout_ms / 1000, .tv_usec = (mon->timeout_ms % 1000) * 1000 };
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(srv->port);
        if (inet_pton(AF_INET, srv->host, &addr.sin_addr) <= 0) {
            mon->last_error = errno;
            close(sock);
            continue;
        }

        for (int retry = 0; retry < 3; retry++) {
            if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                close(sock);
                mon->last_error = 0;
                return true;
            }
            mon->last_error = errno;
            usleep(100000 << retry);  // Exponential backoff: 100ms, 200ms, 400ms
        }
        close(sock);
    }
    return false;
}

/**
 * Check LAN connectivity by verifying interface flags via ioctl.
 * 
 * Performs a fast check with no network traffic by directly inspecting the interface's
 * administrative and link status. Preserves last_error for debugging on failure.
 * 
 * Returns true if interface is both administratively up (IFF_UP) and link is detected (IFF_RUNNING).
 */
static bool check_lan(NetworkMonitor* mon) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        mon->last_error = errno;
        return false;
    }

    struct ifreq ifr;
    strncpy(ifr.ifr_name, mon->lan_interface, IF_NAMESIZE - 1);
    ifr.ifr_name[IF_NAMESIZE - 1] = '\0';  // Ensure null-termination to prevent buffer overflow
    if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0) {
        mon->last_error = errno;
        close(sock);
        return false;
    }
    close(sock);

    // Check both admin status (IFF_UP) and link detection (IFF_RUNNING)
    bool up = (ifr.ifr_flags & IFF_UP) && (ifr.ifr_flags & IFF_RUNNING);
    mon->last_error = up ? 0 : mon->last_error;  // Clear error on success
    return up;
}

/**
 * Background thread function for periodic connectivity checks.
 * 
 * Runs continuous monitoring loop at configurable intervals, updating WAN and LAN status.
 * Uses detached thread for automatic cleanup on exit. Sleeps based on check_interval_sec
 * to provide tunable monitoring frequency.
 */
static void* monitor_thread_func(void* arg) {
    NetworkMonitor* mon = (NetworkMonitor*)arg;
    while (true) {
        pthread_mutex_lock(&mon->lock);
        if (!mon->running) {
            pthread_mutex_unlock(&mon->lock);
            break;
        }
        int interval = mon->check_interval_sec;
        pthread_mutex_unlock(&mon->lock);

        // Run checks; separate functions provide modularity and update last_error internally
        bool new_wan = check_wan(mon);
        bool new_lan = check_lan(mon);

        // Update state; lock only for writes to minimize contention
        pthread_mutex_lock(&mon->lock);
        mon->wan_up = new_wan;
        mon->lan_up = new_lan;
        mon->last_check_time = time(NULL);
        pthread_mutex_unlock(&mon->lock);

        sleep(interval);  // Use nanosleep if sub-second precision needed
    }
    return NULL;
}

// --- Public API ---

/**
 * Constructor: Allocates and initializes a new network monitor.
 * 
 * Initializes configuration with provided values or sensible defaults, auto-detects
 * LAN interface if not specified, validates the interface, and starts the background
 * monitoring thread.
 * 
 * @param initial_cfg Optional configuration struct; NULL uses all defaults
 * @return NetworkMonitorHandle on success, NULL on failure (malloc/pthread errors)
 */
NetworkMonitorHandle network_monitor_new(const NetworkConfig* initial_cfg) {
    NetworkMonitor* mon = malloc(sizeof(NetworkMonitor));
    if (!mon) return NULL;

    // Initialize config with defaults or provided values; safe strncpy for bounds checking
    mon->timeout_ms = initial_cfg && initial_cfg->timeout_ms > 0 ? initial_cfg->timeout_ms : 1000;
    mon->check_interval_sec = initial_cfg && initial_cfg->check_interval_sec > 0 ? initial_cfg->check_interval_sec : 5;
    strncpy(mon->proxy_url, initial_cfg && initial_cfg->proxy_url ? initial_cfg->proxy_url : "", sizeof(mon->proxy_url) - 1);
    mon->proxy_url[sizeof(mon->proxy_url) - 1] = '\0';

    // Initialize WAN servers (defaults: Google, Cloudflare, Quad9, OpenDNS)
    mon->num_wan_servers = 4;
    const WanServer defaults[] = {
        { .host = "8.8.8.8", .port = 53 },
        { .host = "1.1.1.1", .port = 53 },
        { .host = "9.9.9.9", .port = 53 },
        { .host = "208.67.222.222", .port = 53 }
    };
    for (int i = 0; i < mon->num_wan_servers; i++) {
        mon->wan_servers[i] = defaults[i];
    }
    if (initial_cfg && initial_cfg->wan_test_host && initial_cfg->wan_test_port > 0) {
        // Override first server if provided
        strncpy(mon->wan_servers[0].host, initial_cfg->wan_test_host, sizeof(mon->wan_servers[0].host) - 1);
        mon->wan_servers[0].host[sizeof(mon->wan_servers[0].host) - 1] = '\0';
        mon->wan_servers[0].port = initial_cfg->wan_test_port;
    }

    // Initialize LAN interface (auto-detect if not provided)
    if (initial_cfg && initial_cfg->lan_interface && *initial_cfg->lan_interface) {
        strncpy(mon->lan_interface, initial_cfg->lan_interface, IF_NAMESIZE - 1);
        mon->lan_interface[IF_NAMESIZE - 1] = '\0';
    } else {
        if (!detect_lan_interface(mon)) {
            // Fallback to "lo" if no gateway found
            strncpy(mon->lan_interface, "lo", IF_NAMESIZE - 1);
            mon->lan_interface[IF_NAMESIZE - 1] = '\0';
        }
    }

    // Validate LAN interface; fail early if invalid
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0 || !check_lan(mon)) {
        if (sock >= 0) close(sock);
        free(mon);
        return NULL;
    }
    if (sock >= 0) close(sock);

    // Initialize state
    mon->wan_up = false;
    mon->lan_up = false;
    mon->last_check_time = 0;
    mon->last_error = 0;
    mon->running = true;

    // Initialize mutex; required for thread safety across all access
    if (pthread_mutex_init(&mon->lock, NULL) != 0) {
        free(mon);
        return NULL;
    }

    // Start background thread; detached for automatic cleanup, error handling on failure
    if (pthread_create(&mon->monitor_thread, NULL, monitor_thread_func, mon) != 0) {
        pthread_mutex_destroy(&mon->lock);
        free(mon);
        return NULL;
    }
    pthread_detach(mon->monitor_thread);

    return mon;
}

/**
 * Destructor: Signals thread shutdown, waits briefly, and frees resources.
 * 
 * Grace period allows thread to exit cleanly without complex signaling mechanisms.
 */
void network_monitor_destroy(NetworkMonitorHandle self) {
    if (!self) return;
    NetworkMonitor* mon = (NetworkMonitor*)self;
    pthread_mutex_lock(&mon->lock);
    mon->running = false;
    pthread_mutex_unlock(&mon->lock);
    usleep(100000);  // 0.1s grace period to avoid race on quick shutdown
    pthread_mutex_destroy(&mon->lock);
    free(mon);
}

/**
 * Get WAN connectivity status.
 * 
 * Thread-safe read of current WAN status. Returns true if at least one WAN server
 * is currently reachable.
 */
bool get_wan_status(NetworkMonitorHandle self) {
    NetworkMonitor* mon = (NetworkMonitor*)self;
    pthread_mutex_lock(&mon->lock);
    bool status = mon->wan_up;
    pthread_mutex_unlock(&mon->lock);
    return status;
}

/**
 * Get LAN connectivity status.
 * 
 * Thread-safe read of current LAN status. Returns true if monitored interface
 * is administratively up with link detected.
 */
bool get_lan_status(NetworkMonitorHandle self) {
    NetworkMonitor* mon = (NetworkMonitor*)self;
    pthread_mutex_lock(&mon->lock);
    bool status = mon->lan_up;
    pthread_mutex_unlock(&mon->lock);
    return status;
}

/**
 * Get timestamp of last connectivity check.
 * 
 * Thread-safe read of last check time. Useful for detecting stale data if background
 * thread has stopped or is lagging.
 */
time_t get_last_check_time(NetworkMonitorHandle self) {
    NetworkMonitor* mon = (NetworkMonitor*)self;
    pthread_mutex_lock(&mon->lock);
    time_t timestamp = mon->last_check_time;
    pthread_mutex_unlock(&mon->lock);
    return timestamp;
}

/**
 * Get last error code.
 * 
 * Thread-safe read of most recent errno or custom error code from connectivity checks.
 * Returns 0 if last operation succeeded.
 */
int get_last_error(NetworkMonitorHandle self) {
    NetworkMonitor* mon = (NetworkMonitor*)self;
    pthread_mutex_lock(&mon->lock);
    int err = mon->last_error;
    pthread_mutex_unlock(&mon->lock);
    return err;
}

/**
 * Set socket timeout for connectivity checks.
 * 
 * Thread-safe configuration update. Validates input to prevent invalid states;
 * defaults to 1000ms if invalid value provided.
 */
void set_timeout_ms(NetworkMonitorHandle self, int ms) {
    NetworkMonitor* mon = (NetworkMonitor*)self;
    pthread_mutex_lock(&mon->lock);
    mon->timeout_ms = (ms > 0) ? ms : 1000;
    pthread_mutex_unlock(&mon->lock);
}

/**
 * Set background check interval.
 * 
 * Thread-safe configuration update. Validates input; defaults to 5 seconds
 * if invalid value provided.
 */
void set_check_interval_sec(NetworkMonitorHandle self, int sec) {
    NetworkMonitor* mon = (NetworkMonitor*)self;
    pthread_mutex_lock(&mon->lock);
    mon->check_interval_sec = (sec > 0) ? sec : 5;
    pthread_mutex_unlock(&mon->lock);
}

/**
 * Set HTTP proxy URL for future connectivity checks.
 * 
 * Thread-safe configuration update. Currently unused; reserved for future
 * libcurl-based HTTP connectivity checks.
 */
void set_proxy(NetworkMonitorHandle self, const char* proxy_url) {
    NetworkMonitor* mon = (NetworkMonitor*)self;
    pthread_mutex_lock(&mon->lock);
    strncpy(mon->proxy_url, proxy_url ? proxy_url : "", sizeof(mon->proxy_url) - 1);
    mon->proxy_url[sizeof(mon->proxy_url) - 1] = '\0';
    pthread_mutex_unlock(&mon->lock);
}

/**
 * Set primary WAN test host.
 * 
 * Thread-safe configuration update. Updates the first WAN server in the redundancy list.
 * Defaults to Google DNS (8.8.8.8) if NULL provided.
 */
void set_wan_test_host(NetworkMonitorHandle self, const char* host) {
    NetworkMonitor* mon = (NetworkMonitor*)self;
    pthread_mutex_lock(&mon->lock);
    strncpy(mon->wan_servers[0].host, host ? host : "8.8.8.8", sizeof(mon->wan_servers[0].host) - 1);
    mon->wan_servers[0].host[sizeof(mon->wan_servers[0].host) - 1] = '\0';
    pthread_mutex_unlock(&mon->lock);
}

/**
 * Set primary WAN test port.
 * 
 * Thread-safe configuration update. Updates the first WAN server's port.
 * Defaults to port 53 (DNS) if invalid value provided.
 */
void set_wan_test_port(NetworkMonitorHandle self, int port) {
    NetworkMonitor* mon = (NetworkMonitor*)self;
    pthread_mutex_lock(&mon->lock);
    mon->wan_servers[0].port = (port > 0) ? port : 53;
    pthread_mutex_unlock(&mon->lock);
}

/**
 * Set LAN interface to monitor.
 * 
 * Thread-safe configuration update. Switches monitoring to specified interface.
 * Defaults to "eth0" if NULL provided. Does not validate interface existence.
 */
void set_lan_interface(NetworkMonitorHandle self, const char* iface) {
    NetworkMonitor* mon = (NetworkMonitor*)self;
    pthread_mutex_lock(&mon->lock);
    strncpy(mon->lan_interface, iface ? iface : "eth0", sizeof(mon->lan_interface) - 1);
    mon->lan_interface[sizeof(mon->lan_interface) - 1] = '\0';
    pthread_mutex_unlock(&mon->lock);
}

/**
 * Generate debug string representation of monitor state.
 * 
 * Thread-safe snapshot of current configuration and status. Allocates and returns
 * a formatted string; caller must free() the returned pointer.
 * 
 * @return Allocated string on success, NULL on malloc failure
 */
char* network_monitor_to_string(NetworkMonitorHandle self) {
    NetworkMonitor* mon = (NetworkMonitor*)self;
    pthread_mutex_lock(&mon->lock);
    char* buf = malloc(768);  // Sufficient for max formatted string length
    if (buf) {
        snprintf(buf, 768, "NetworkMonitor: WAN=%d, LAN=%d, LastCheck=%ld, Timeout=%dms, Proxy=%s, WANHost=%s:%d, LANIface=%s",
                 mon->wan_up, mon->lan_up, mon->last_check_time, mon->timeout_ms, mon->proxy_url,
                 mon->wan_servers[0].host, mon->wan_servers[0].port, mon->lan_interface);
    }
    pthread_mutex_unlock(&mon->lock);
    return buf;
}