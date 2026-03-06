// user/syscalls.h
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "../kernel/gcp.h" // For gcp_command_t

// Standard C library types
typedef long ssize_t;

// --- System Call Numbers ---
#define SYS_PUTC        1001
#define SYS_OPEN        1002
#define SYS_READ        1003
#define SYS_CLOSE       1004
#define SYS_GCP_EXECUTE 1005
#define SYS_GETC        1006
#define SYS_EXEC        1007
#define SYS_EXIT        1008
#define SYS_WAIT        1009
#define SYS_WRITE  1010
#define SYS_CREATE 1011
#define SYS_MKDIR  1012
#define SYS_STAT   1013
#define SYS_READDIR 1014
#define SYS_SYNC    1015
#define SYS_SPAWN   1017
#define SYS_KILL    1018
#define SYS_SIGNAL  1019
#define SYS_GETPID  1020
#define SYS_GET_SYSTEM_STATE 1021
#define SYS_CAP_ACTIVATE     1031
#define SYS_CAP_DEACTIVATE   1032
#define SYS_CAP_REGISTER     1033
#define SYS_CAP_UNREGISTER   1034
#define SYS_SET_AI_METADATA  1035
#define SYS_GET_AI_METADATA  1036
#define SYS_SEARCH_BY_TAG    1037
#define SYS_CAP_WATCH        1038
#define SYS_CAP_UNWATCH      1039
#define SYS_CAP_POLL         1040
#define SYS_NET_IFCONFIG     1041
#define SYS_NET_STATUS       1042
#define SYS_HTTP_GET         1043
#define SYS_DNS_RESOLVE      1044
#define SYS_HTTP_POST        1045
#define SYS_PIPE             1046
#define SYS_DUP2             1047
#define SYS_DUP              1048
#define SYS_TRUNCATE         1049
#define SYS_COMPUTE_SIMHASH  1050
#define SYS_FIND_SIMILAR     1051

// Directory entry structure for user space
typedef struct {
    uint32_t type;
    char name[28];
} user_dirent_t;

// --- Syscall Wrappers (Inline Assembly) ---

static inline void syscall_putc(char c) {
    asm volatile("syscall" : : "a"(SYS_PUTC), "D"(c) : "rcx", "r11", "memory");
}

static inline int syscall_open(const char *pathname) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_OPEN), "D"(pathname) : "rcx", "r11", "memory");
    return (int)ret;
}

static inline ssize_t syscall_read(int fd, void *buf, size_t count) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_READ), "D"(fd), "S"(buf), "d"(count) : "rcx", "r11", "memory");
    return (ssize_t)ret;
}

static inline int syscall_close(int fd) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_CLOSE), "D"(fd) : "rcx", "r11", "memory");
    return (int)ret;
}

static inline int syscall_gcp_execute(gcp_command_t *cmd) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_GCP_EXECUTE), "D"(cmd) : "rcx", "r11", "memory");
    return (int)ret;
}

static inline char syscall_getc(void) {
    long ret;
    // This is a blocking call; the kernel won't return until a key is pressed.
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_GETC) : "rcx", "r11", "memory");
    return (char)ret;
}

static inline int syscall_exec(const char *pathname) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_EXEC), "D"(pathname) : "rcx", "r11", "memory");
    return (int)ret;
}

static inline void syscall_exit(int status) {
    asm volatile("syscall" : : "a"(SYS_EXIT), "D"(status) : "rcx", "r11", "memory");
    // Should never return
    while(1);
}

// NEW: wait() - Wait for any child process to exit
// Returns: child PID on success, -1 on error
// If status is non-NULL, the exit status of the child is stored there
static inline int syscall_wait(int *status) {
    long ret;
    
    // Keep retrying if we get the special retry value
    do {
        asm volatile("syscall" : "=a"(ret) : "a"(SYS_WAIT), "D"(status) : "rcx", "r11", "memory");
        
        // If we got -99, it means we were blocked and need to retry
        if (ret == -99) {
            // The kernel has woken us up, retry the syscall
            continue;
        }
        
        break;
    } while (1);
    
    return (int)ret;
}

// Convenience wrapper that ignores exit status
static inline int wait(void) {
    return syscall_wait(NULL);
}

static inline ssize_t syscall_write(int fd, const void *buf, size_t count) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_WRITE), "D"(fd), "S"(buf), "d"(count) : "rcx", "r11", "memory");
    return (ssize_t)ret;
}

static inline int syscall_create(const char *pathname, uint32_t mode) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_CREATE), "D"(pathname), "S"(mode) : "rcx", "r11", "memory");
    return (int)ret;
}

static inline int syscall_mkdir(const char *pathname, uint32_t mode) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_MKDIR), "D"(pathname), "S"(mode) : "rcx", "r11", "memory");
    return (int)ret;
}

static inline int syscall_readdir(const char *pathname, uint32_t index, user_dirent_t *dirent) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_READDIR), "D"(pathname), "S"(index), "d"(dirent) : "rcx", "r11", "memory");
    return (int)ret;
}

static inline void syscall_sync(void) {
    asm volatile("syscall" : : "a"(SYS_SYNC) : "rcx", "r11", "memory");
}

// Phase 7d: Spawn a new process (modern replacement for fork+exec)
static inline int syscall_spawn(const char *path) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_SPAWN), "D"(path) : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 7d: Send a signal to a process
static inline int syscall_kill(int pid, int signal) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_KILL), "D"(pid), "S"(signal) : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 7d: Get current process ID
static inline int syscall_getpid(void) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_GETPID) : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 8a: Get system state snapshot
// category: STATE_CAT_* from state.h
// buf: user buffer to fill, or NULL to query required size
// buf_size: size of user buffer
// Returns: bytes written on success, required size if buf is NULL, negative on error
static inline long syscall_get_system_state(uint32_t category, void *buf, size_t buf_size) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_GET_SYSTEM_STATE), "D"(category), "S"(buf), "d"(buf_size)
        : "rcx", "r11", "memory");
    return ret;
}

// Phase 8b: Capability Activation Network
static inline int syscall_cap_activate(const char *name) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_CAP_ACTIVATE), "D"(name)
        : "rcx", "r11", "memory");
    return (int)ret;
}

static inline int syscall_cap_deactivate(const char *name) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_CAP_DEACTIVATE), "D"(name)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 8b-ii: Register a user-owned capability
// type: CAP_APPLICATION(3), CAP_FEATURE(4), or CAP_COMPOSITE(5)
// dep_names: array of dependency name strings, dep_count: number of deps
// Returns: cap_id (>=0) on success, negative error on failure
static inline int syscall_cap_register(const char *name, uint32_t type,
                                       const char **dep_names, int dep_count) {
    long ret;
    register long r10 asm("r10") = (long)dep_count;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_CAP_REGISTER), "D"(name), "S"(type), "d"(dep_names), "r"(r10)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 8b-ii: Unregister a user-owned capability
// Returns: 0 on success, negative error on failure
static inline int syscall_cap_unregister(const char *name) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_CAP_UNREGISTER), "D"(name)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 8c: AI Metadata operations
// Set AI metadata on a file by path
static inline int syscall_set_ai_metadata(const char *path, const void *meta) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_SET_AI_METADATA), "D"(path), "S"(meta)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Get AI metadata from a file by path
static inline int syscall_get_ai_metadata(const char *path, void *meta) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_GET_AI_METADATA), "D"(path), "S"(meta)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Search files by tag substring
// Returns: number of matches found, negative on error
static inline int syscall_search_by_tag(const char *tag, void *results, int max) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_SEARCH_BY_TAG), "D"(tag), "S"(results), "d"(max)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 8d: Watch a capability for state change events
static inline int syscall_cap_watch(const char *name) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_CAP_WATCH), "D"(name)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 8d: Stop watching a capability
static inline int syscall_cap_unwatch(const char *name) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_CAP_UNWATCH), "D"(name)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 8d: Poll for CAN events (blocking with -99 retry)
// events: buffer for state_cap_event_t array
// max_events: max number of events to dequeue
// Returns: number of events dequeued, or -99 (retry/block)
static inline int syscall_cap_poll(void *events, int max_events) {
    long ret;
    do {
        asm volatile("syscall" : "=a"(ret)
            : "a"(SYS_CAP_POLL), "D"(events), "S"(max_events)
            : "rcx", "r11", "memory");
        if (ret == -99) continue;
        break;
    } while (1);
    return (int)ret;
}

// Phase 8d: Non-blocking poll for CAN events (returns immediately)
// Returns: number of events dequeued, 0 if none pending
static inline int syscall_cap_poll_nonblock(void *events, int max_events) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_CAP_POLL), "D"(events), "S"(max_events)
        : "rcx", "r11", "memory");
    // Don't retry on -99, just return it (caller handles)
    return (int)ret;
}

// Phase 9a: Get network interface info
// buf: at least 7 bytes (6 MAC + 1 link_up)
// Returns: 0 on success, -1 bad pointer, -2 no NIC
static inline int syscall_net_ifconfig(void *buf) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_NET_IFCONFIG), "D"(buf)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 9b: Get network stack status
// buf: pointer to net_status_t (21 bytes)
// Returns: 0 on success, -1 bad pointer
static inline int syscall_net_status(void *buf) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_NET_STATUS), "D"(buf)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 9c: HTTP GET (blocking, returns body length or negative error)
// url: HTTP URL string (e.g. "http://10.0.2.15/")
// response_buf: buffer for response body
// max_len: maximum bytes to store in response_buf
// Returns: body length (>=0) on success, negative error on failure
static inline int syscall_http_get(const char *url, char *response_buf, int max_len) {
    long ret;
    do {
        asm volatile("syscall" : "=a"(ret)
            : "a"(SYS_HTTP_GET), "D"(url), "S"(response_buf), "d"(max_len)
            : "rcx", "r11", "memory");
        if (ret == -99) continue;
        break;
    } while (1);
    return (int)ret;
}

// Phase 10b: Create a pipe
// fds[0] = read end, fds[1] = write end
// Returns: 0 on success, -1 on failure
static inline int syscall_pipe(int fds[2]) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_PIPE), "D"(fds)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 10b: Duplicate file descriptor to specific slot
// Returns: new_fd on success, -1 on failure
static inline int syscall_dup2(int old_fd, int new_fd) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_DUP2), "D"(old_fd), "S"(new_fd)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 10b: Duplicate file descriptor to lowest available slot
// Returns: new fd number on success, -1 on failure
static inline int syscall_dup(int old_fd) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_DUP), "D"(old_fd)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 10c: Truncate file to 0 bytes
// Returns: 0 on success, -1 on failure
static inline int syscall_truncate(int fd) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_TRUNCATE), "D"(fd)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 11a: Compute SimHash for a file
// Returns 64-bit SimHash on success, 0 on failure
static inline uint64_t syscall_compute_simhash(const char *path) {
    uint64_t ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_COMPUTE_SIMHASH), "D"(path)
        : "rcx", "r11", "memory");
    return ret;
}

// Phase 11a: Find files similar to path by SimHash Hamming distance
// threshold: max Hamming distance (0 = use default 10)
// results: pointer to grahafs_search_results_t
// Returns: number of matches, negative on error
static inline int syscall_find_similar(const char *path, int threshold, void *results) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_FIND_SIMILAR), "D"(path), "S"(threshold), "d"(results)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 9c: DNS resolve (blocking, returns 0 or negative error)
// hostname: hostname to resolve (e.g. "dns.google")
// ip_buf: buffer for 4-byte IPv4 address result
// Returns: 0 on success, negative error on failure
static inline int syscall_dns_resolve(const char *hostname, uint8_t *ip_buf) {
    long ret;
    do {
        asm volatile("syscall" : "=a"(ret)
            : "a"(SYS_DNS_RESOLVE), "D"(hostname), "S"(ip_buf)
            : "rcx", "r11", "memory");
        if (ret == -99) continue;
        break;
    } while (1);
    return (int)ret;
}

// Phase 9e: HTTP POST (blocking, returns body length or negative error)
// url: full URL (http:// or https://)
// body: POST body data
// body_len: length of POST body
// response_buf: buffer for response body
// max_len: maximum bytes to store in response_buf
// Returns: body length (>=0) on success, negative error on failure
static inline int syscall_http_post(const char *url, const char *body, int body_len,
                                     char *response_buf, int max_len) {
    long ret;
    register const char *r10 asm("r10") = response_buf;
    register long r8 asm("r8") = (long)max_len;
    do {
        asm volatile("syscall" : "=a"(ret)
            : "a"(SYS_HTTP_POST), "D"(url), "S"(body), "d"((long)body_len),
              "r"(r10), "r"(r8)
            : "rcx", "r11", "memory");
        if (ret == -99) continue;
        break;
    } while (1);
    return (int)ret;
}