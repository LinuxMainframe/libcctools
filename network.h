/*
 * file: network.h
 * author: Aidan A. Bradley
 * date: 10/08/2025 (mm/dd/yyyy)
 * version: 0.0.1
 * _____________________________ 
 * Public API for the networking module.
 * 
 * This header defines the opaque NetworkMonitorHandle and public functions for creating,
 * configuring, querying, and destroying a NetworkMonitor object. It enables WAN and LAN
 * connectivity monitoring without exposing internal details, ensuring encapsulation and
 * ease of use. Users interact via constructors, getters, setters, and a debug function.
 * 
 * Why this API? It provides a simple, thread-safe interface for background network checks,
 * allowing main programs to query status (e.g., WAN up?) without managing threads or sockets.
 * Configurable via NetworkConfig struct and setters for flexibility in varied environments
 * (e.g., high-latency networks or custom test hosts).
 * 
 * Include this header in client code; implementation is in network.c.
 */

#ifndef NETWORK_H
#define NETWORK_H

#include <stdbool.h>  // For bool
#include <time.h>     // For time_t

// Opaque handle for NetworkMonitor
typedef void* NetworkMonitorHandle;

// Config struct for constructor
typedef struct {
    int timeout_ms;           // Socket timeout in ms
    int check_interval_sec;   // Check frequency in seconds
    const char* proxy_url;    // Optional HTTP proxy
    const char* wan_test_host;// WAN test host (e.g., "8.8.8.8")
    int wan_test_port;        // WAN test port (e.g., 53)
    const char* lan_interface;// LAN interface (e.g., "eth0")
} NetworkConfig;

// Public API

/**
 * Creates and initializes a NetworkMonitor.
 * Starts a background thread for periodic checks.
 * 
 * @param initial_cfg Optional config; NULL uses defaults.
 * @return Handle on success; NULL on failure (e.g., allocation error).
 */
NetworkMonitorHandle network_monitor_new(const NetworkConfig* initial_cfg);

/**
 * Destroys the monitor, stops the thread, and frees resources.
 * 
 * @param self The handle to destroy.
 */
void network_monitor_destroy(NetworkMonitorHandle self);

// Getters (thread-safe; return current state)

/**
 * Gets WAN status.
 * 
 * @param self The monitor handle.
 * @return true if WAN is up.
 */
bool get_wan_status(NetworkMonitorHandle self);

/**
 * Gets LAN status.
 * 
 * @param self The monitor handle.
 * @return true if LAN interface is up.
 */
bool get_lan_status(NetworkMonitorHandle self);

/**
 * Gets time of last check.
 * 
 * @param self The monitor handle.
 * @return Timestamp (time_t).
 */
time_t get_last_check_time(NetworkMonitorHandle self);

/**
 * Gets last error code.
 * 
 * @param self The monitor handle.
 * @return errno or 0 if no error.
 */
int get_last_error(NetworkMonitorHandle self);

// Setters (thread-safe; update config live)

/**
 * Sets socket timeout.
 * 
 * @param self The monitor handle.
 * @param ms Milliseconds (>0; else default).
 */
void set_timeout_ms(NetworkMonitorHandle self, int ms);

/**
 * Sets check interval.
 * 
 * @param self The monitor handle.
 * @param sec Seconds (>0; else default).
 */
void set_check_interval_sec(NetworkMonitorHandle self, int sec);

/**
 * Sets proxy URL.
 * 
 * @param self The monitor handle.
 * @param proxy_url String (NULL clears).
 */
void set_proxy(NetworkMonitorHandle self, const char* proxy_url);

/**
 * Sets WAN test host.
 * 
 * @param self The monitor handle.
 * @param host IP/hostname (NULL defaults).
 */
void set_wan_test_host(NetworkMonitorHandle self, const char* host);

/**
 * Sets WAN test port.
 * 
 * @param self The monitor handle.
 * @param port Port (>0; else default).
 */
void set_wan_test_port(NetworkMonitorHandle self, int port);

/**
 * Sets LAN interface.
 * 
 * @param self The monitor handle.
 * @param iface Name (NULL defaults).
 */
void set_lan_interface(NetworkMonitorHandle self, const char* iface);

/**
 * Gets string representation (allocated; caller frees).
 * 
 * @param self The monitor handle.
 * @return Allocated string or NULL on error.
 */
char* network_monitor_to_string(NetworkMonitorHandle self);

#endif  // NETWORK_H