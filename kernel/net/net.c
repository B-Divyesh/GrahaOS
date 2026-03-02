// kernel/net/net.c
// Phase 9b/9c: Mongoose TCP/IP stack integration for GrahaOS
// Provides: mg_millis, mg_random, E1000 driver bridge, HTTP server/client, DNS, poll task

#include "net.h"
#include "net_task.h"
#include "kmalloc.h"
#include "klib.h"
#include "mongoose.h"
#include "../../arch/x86_64/drivers/e1000/e1000.h"
#include "../../arch/x86_64/drivers/serial/serial.h"
#include "../../arch/x86_64/cpu/interrupts.h"
#include "../../arch/x86_64/cpu/sched/sched.h"
#include "../../arch/x86_64/mm/pmm.h"
#include "../capability.h"
#include "../sync/spinlock.h"

// ===== Mongoose Required Implementations =====

// Timer: LAPIC fires at ~100Hz, each tick = ~10ms
uint64_t mg_millis(void) {
    return g_timer_ticks * 10;
}

// Simple xorshift64 PRNG
static uint64_t prng_state = 0;

void mg_random(void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    if (prng_state == 0) {
        // Seed from timer ticks and MAC address
        uint8_t mac[6];
        e1000_get_mac(mac);
        prng_state = g_timer_ticks ^ (((uint64_t)mac[0] << 40) |
                     ((uint64_t)mac[1] << 32) | ((uint64_t)mac[2] << 24) |
                     ((uint64_t)mac[3] << 16) | ((uint64_t)mac[4] << 8) |
                     (uint64_t)mac[5]);
        if (prng_state == 0) prng_state = 0x12345678DEADBEEF;
    }
    for (size_t i = 0; i < len; i++) {
        prng_state ^= prng_state << 13;
        prng_state ^= prng_state >> 7;
        prng_state ^= prng_state << 17;
        p[i] = (uint8_t)(prng_state & 0xFF);
    }
}

// Logging: route MG_LOG to serial
// mg_log_level is defined in mongoose.c, just use extern
extern int mg_log_level;

void mg_log_prefix(int level, const char *file, int line, const char *fname) {
    (void)file;
    const char *lvl_str = "???";
    if (level == MG_LL_ERROR) lvl_str = "ERR";
    else if (level == MG_LL_INFO) lvl_str = "INF";
    else if (level == MG_LL_DEBUG) lvl_str = "DBG";
    else if (level == MG_LL_VERBOSE) lvl_str = "VRB";

    char prefix[128];
    const char *fn = fname ? fname : "?";
    snprintf(prefix, sizeof(prefix), "[NET %s] %s:%d ", lvl_str, fn, line);
    serial_write(prefix);
}

void mg_log(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_write(buf);
    serial_write("\n");
}

// ===== E1000 Driver Bridge =====

static uint32_t s_rx_count = 0;
static uint32_t s_tx_count = 0;

static bool e1000_drv_init(struct mg_tcpip_if *ifp) {
    (void)ifp;
    return e1000_is_present();
}

static size_t e1000_drv_tx(const void *buf, size_t len, struct mg_tcpip_if *ifp) {
    (void)ifp;
    if (len > 1514) len = 1514;  // Clamp to max Ethernet frame
    int ret = e1000_send(buf, (uint16_t)len);
    if (ret == 0) {
        s_tx_count++;
        return len;
    }
    return 0;
}

static size_t e1000_drv_rx(void *buf, size_t len, struct mg_tcpip_if *ifp) {
    (void)ifp;
    int ret = e1000_receive(buf, (uint16_t)len);
    if (ret > 0) {
        s_rx_count++;
        return (size_t)ret;
    }
    return 0;
}

static bool e1000_drv_up(struct mg_tcpip_if *ifp) {
    (void)ifp;
    return e1000_link_up();
}

static struct mg_tcpip_driver e1000_driver = {
    .init = e1000_drv_init,
    .tx   = e1000_drv_tx,
    .rx   = e1000_drv_rx,
    .up   = e1000_drv_up,
};

// ===== HTTP Server =====

static void http_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;

        if (mg_match(hm->uri, mg_str("/"), NULL)) {
            mg_http_reply(c, 200, "Content-Type: text/plain\r\n",
                          "Hello from GrahaOS!\n");
        } else if (mg_match(hm->uri, mg_str("/api/status"), NULL)) {
            uint64_t free_mem = pmm_get_free_memory();
            uint64_t total_mem = pmm_get_total_memory();
            uint64_t uptime_ms = mg_millis();
            mg_http_reply(c, 200, "Content-Type: text/plain\r\n",
                          "GrahaOS Status\n"
                          "Uptime: %llu ms\n"
                          "Free Memory: %llu / %llu bytes\n"
                          "Network RX: %u packets\n"
                          "Network TX: %u packets\n",
                          (unsigned long long)uptime_ms,
                          (unsigned long long)free_mem,
                          (unsigned long long)total_mem,
                          s_rx_count, s_tx_count);
        } else {
            mg_http_reply(c, 404, "", "Not found\n");
        }
    }
}

// ===== Global State =====

static struct mg_mgr s_mgr;
static struct mg_tcpip_if s_mif;
static bool s_net_initialized = false;

// ===== Phase 9c: HTTP Client + DNS Resolve =====

// Request types
#define NET_REQ_NONE     0
#define NET_REQ_HTTP_GET 1
#define NET_REQ_DNS      2

// Request states
#define NET_STATE_IDLE    0
#define NET_STATE_PENDING 1
#define NET_STATE_DONE    2
#define NET_STATE_ERROR   3

typedef struct {
    int type;                              // NONE, HTTP_GET, DNS
    volatile int state;                    // IDLE, PENDING, DONE, ERROR
    int task_id;                           // Owning task
    uint64_t start_time_ms;                // For timeout detection
    int error_code;                        // Error code if state == ERROR
    int http_status;                       // HTTP status code (200, 404, etc.)
    char url[512];                         // Kernel copy of URL
    char response[NET_MAX_RESPONSE_SIZE];  // Response body buffer
    int response_len;                      // Actual response length
    uint8_t resolved_ip[4];                // DNS result (IPv4)
    struct mg_connection *conn;            // Active Mongoose connection
} net_request_t;

static net_request_t s_requests[MAX_TASKS];

// Forward declarations
static void http_client_handler(struct mg_connection *c, int ev, void *ev_data);
static void dns_resolve_handler(struct mg_connection *c, int ev, void *ev_data);
static void net_wake_task(int task_id);

// Initialize request table (called from net_init)
static void net_requests_init(void) {
    extern void *memset(void *, int, size_t);
    memset(s_requests, 0, sizeof(s_requests));
}

// Wake a blocked task — callable from mongoose_poll_task context
static void net_wake_task(int task_id) {
    extern spinlock_t sched_lock;

    spinlock_acquire(&sched_lock);
    task_t *task = sched_get_task(task_id);
    if (task && task->state == TASK_STATE_BLOCKED) {
        task->state = TASK_STATE_READY;
    }
    spinlock_release(&sched_lock);
}

// ----- HTTP Client Callback -----

static void http_client_handler(struct mg_connection *c, int ev, void *ev_data) {
    int task_id = (int)(uintptr_t)c->fn_data;
    if (task_id < 0 || task_id >= MAX_TASKS) return;

    net_request_t *req = &s_requests[task_id];

    if (ev == MG_EV_CONNECT) {
        // TCP connected — send the HTTP GET request
        struct mg_str host = mg_url_host(req->url);
        const char *uri = mg_url_uri(req->url);
        mg_printf(c,
                  "GET %s HTTP/1.1\r\n"
                  "Host: %.*s\r\n"
                  "Connection: close\r\n"
                  "\r\n",
                  uri, (int)host.len, host.buf);
    } else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        extern void *memcpy(void *, const void *, size_t);

        // Copy response body to kernel buffer
        size_t copy_len = hm->body.len;
        if (copy_len > NET_MAX_RESPONSE_SIZE - 1)
            copy_len = NET_MAX_RESPONSE_SIZE - 1;

        memcpy(req->response, hm->body.buf, copy_len);
        req->response[copy_len] = '\0';
        req->response_len = (int)copy_len;
        req->http_status = mg_http_status(hm);
        req->state = NET_STATE_DONE;
        req->conn = NULL;
        c->is_draining = 1;

        net_wake_task(task_id);
    } else if (ev == MG_EV_ERROR) {
        const char *err = (const char *)ev_data;
        serial_write("[NET] HTTP client error: ");
        if (err) serial_write(err);
        serial_write("\n");

        req->error_code = NET_ERR_CONNECT;
        if (err && strstr(err, "DNS")) {
            req->error_code = NET_ERR_DNS_FAIL;
        }
        req->state = NET_STATE_ERROR;
        req->conn = NULL;
        net_wake_task(task_id);
    } else if (ev == MG_EV_CLOSE) {
        if (req->state == NET_STATE_PENDING) {
            req->error_code = NET_ERR_CONNECT;
            req->state = NET_STATE_ERROR;
            req->conn = NULL;
            net_wake_task(task_id);
        }
    }
}

// ----- DNS Resolve Callback -----

static void dns_resolve_handler(struct mg_connection *c, int ev, void *ev_data) {
    (void)ev_data;
    int task_id = (int)(uintptr_t)c->fn_data;
    if (task_id < 0 || task_id >= MAX_TASKS) return;

    net_request_t *req = &s_requests[task_id];

    if (ev == MG_EV_RESOLVE) {
        // DNS resolved — extract IPv4 from c->rem
        if (!c->rem.is_ip6) {
            req->resolved_ip[0] = c->rem.ip[0];
            req->resolved_ip[1] = c->rem.ip[1];
            req->resolved_ip[2] = c->rem.ip[2];
            req->resolved_ip[3] = c->rem.ip[3];
            req->state = NET_STATE_DONE;
        } else {
            req->error_code = NET_ERR_DNS_FAIL;
            req->state = NET_STATE_ERROR;
        }
        req->conn = NULL;
        c->is_closing = 1;
        net_wake_task(task_id);
    } else if (ev == MG_EV_ERROR) {
        req->error_code = NET_ERR_DNS_FAIL;
        req->state = NET_STATE_ERROR;
        req->conn = NULL;
        net_wake_task(task_id);
    } else if (ev == MG_EV_CLOSE) {
        if (req->state == NET_STATE_PENDING) {
            req->error_code = NET_ERR_DNS_FAIL;
            req->state = NET_STATE_ERROR;
            req->conn = NULL;
            net_wake_task(task_id);
        }
    }
}

// ----- Public API: HTTP GET -----

int net_http_get_start(int task_id, const char *url) {
    if (!s_net_initialized) return NET_ERR_NO_NET;
    if (task_id < 0 || task_id >= MAX_TASKS) return NET_ERR_BUSY;

    net_request_t *req = &s_requests[task_id];
    if (req->state == NET_STATE_PENDING) return NET_ERR_BUSY;

    extern void *memset(void *, int, size_t);
    extern size_t strlen(const char *);
    memset(req, 0, sizeof(*req));
    req->type = NET_REQ_HTTP_GET;
    req->state = NET_STATE_PENDING;
    req->task_id = task_id;
    req->start_time_ms = mg_millis();

    // Copy URL to kernel buffer for callback access
    size_t url_len = strlen(url);
    if (url_len >= sizeof(req->url)) url_len = sizeof(req->url) - 1;
    extern void *memcpy(void *, const void *, size_t);
    memcpy(req->url, url, url_len);
    req->url[url_len] = '\0';

    struct mg_connection *c = mg_http_connect(
        &s_mgr, req->url, http_client_handler, (void *)(uintptr_t)task_id);

    if (!c) {
        req->state = NET_STATE_IDLE;
        return NET_ERR_NOMEM;
    }

    req->conn = c;
    return 0;
}

int net_http_get_check(int task_id, char *user_buf, int max_len) {
    if (task_id < 0 || task_id >= MAX_TASKS) return -1;

    net_request_t *req = &s_requests[task_id];

    if (req->state == NET_STATE_DONE) {
        extern void *memcpy(void *, const void *, size_t);
        int copy_len = req->response_len;
        if (copy_len > max_len - 1) copy_len = max_len - 1;

        memcpy(user_buf, req->response, copy_len);
        user_buf[copy_len] = '\0';

        int result = copy_len;
        req->state = NET_STATE_IDLE;
        req->type = NET_REQ_NONE;
        return result;
    } else if (req->state == NET_STATE_ERROR) {
        int err = req->error_code;
        req->state = NET_STATE_IDLE;
        req->type = NET_REQ_NONE;
        return err;
    }

    // IDLE or PENDING
    return -99;
}

// ----- Public API: DNS Resolve -----

int net_dns_start(int task_id, const char *hostname) {
    if (!s_net_initialized) return NET_ERR_NO_NET;
    if (task_id < 0 || task_id >= MAX_TASKS) return NET_ERR_BUSY;

    net_request_t *req = &s_requests[task_id];
    if (req->state == NET_STATE_PENDING) return NET_ERR_BUSY;

    extern void *memset(void *, int, size_t);
    memset(req, 0, sizeof(*req));
    req->type = NET_REQ_DNS;
    req->state = NET_STATE_PENDING;
    req->task_id = task_id;
    req->start_time_ms = mg_millis();

    // Build a TCP URL so Mongoose triggers DNS resolution
    char url[320];
    snprintf(url, sizeof(url), "tcp://%s:80", hostname);

    struct mg_connection *c = mg_connect(
        &s_mgr, url, dns_resolve_handler, (void *)(uintptr_t)task_id);

    if (!c) {
        req->state = NET_STATE_IDLE;
        return NET_ERR_NOMEM;
    }

    req->conn = c;
    return 0;
}

int net_dns_check(int task_id, uint8_t *ip_buf) {
    if (task_id < 0 || task_id >= MAX_TASKS) return -1;

    net_request_t *req = &s_requests[task_id];

    if (req->state == NET_STATE_DONE) {
        extern void *memcpy(void *, const void *, size_t);
        memcpy(ip_buf, req->resolved_ip, 4);
        req->state = NET_STATE_IDLE;
        req->type = NET_REQ_NONE;
        return 0;
    } else if (req->state == NET_STATE_ERROR) {
        int err = req->error_code;
        req->state = NET_STATE_IDLE;
        req->type = NET_REQ_NONE;
        return err;
    }

    return -99;
}

// ----- Cleanup + Timeouts -----

void net_cleanup_task(int task_id) {
    if (task_id < 0 || task_id >= MAX_TASKS) return;

    net_request_t *req = &s_requests[task_id];
    if (req->state == NET_STATE_PENDING && req->conn) {
        req->conn->is_closing = 1;
        req->conn = NULL;
    }
    req->state = NET_STATE_IDLE;
    req->type = NET_REQ_NONE;
}

void net_check_timeouts(void) {
    uint64_t now = mg_millis();
    for (int i = 0; i < MAX_TASKS; i++) {
        if (s_requests[i].state == NET_STATE_PENDING) {
            if (now - s_requests[i].start_time_ms > NET_REQUEST_TIMEOUT_MS) {
                s_requests[i].error_code = NET_ERR_TIMEOUT;
                s_requests[i].state = NET_STATE_ERROR;
                if (s_requests[i].conn) {
                    s_requests[i].conn->is_closing = 1;
                    s_requests[i].conn = NULL;
                }
                net_wake_task(i);
                serial_write("[NET] Request timed out for task\n");
            }
        }
    }
}

// ===== Initialization =====

void net_init(void) {
    if (s_net_initialized) return;

    serial_write("[NET] Initializing Mongoose TCP/IP stack...\n");

    // Initialize kernel malloc arena
    kmalloc_init();
    serial_write("[NET] Kernel malloc arena ready (2MB)\n");

    // Check E1000 is present
    if (!e1000_is_present()) {
        serial_write("[NET] ERROR: E1000 NIC not found, aborting net_init\n");
        return;
    }

    // Initialize Mongoose event manager
    mg_mgr_init(&s_mgr);

    // Configure network interface
    extern void *memset(void *, int, size_t);
    memset(&s_mif, 0, sizeof(s_mif));

    // Get MAC from E1000
    e1000_get_mac(s_mif.mac);

    // Static IP configuration (QEMU user-mode defaults)
    s_mif.ip      = MG_IPV4(10, 0, 2, 15);
    s_mif.mask    = MG_IPV4(255, 255, 255, 0);
    s_mif.gw      = MG_IPV4(10, 0, 2, 2);
    s_mif.driver  = &e1000_driver;

    // Initialize TCP/IP stack
    mg_tcpip_init(&s_mgr, &s_mif);
    serial_write("[NET] TCP/IP stack initialized\n");

    // Log IP configuration
    {
        char ip_msg[64];
        snprintf(ip_msg, sizeof(ip_msg), "[NET] IP: 10.0.2.15, GW: 10.0.2.2\n");
        serial_write(ip_msg);
    }

    // Start HTTP server on port 80
    mg_http_listen(&s_mgr, "http://0.0.0.0:80", http_handler, NULL);
    serial_write("[NET] HTTP server listening on port 80\n");

    // Register CAN capability
    const char *tcp_deps[] = {"e1000_nic"};
    cap_register("tcp_ip", CAP_SERVICE, CAP_SUBTYPE_OTHER, -1,
                 tcp_deps, 1, NULL, NULL, NULL, 0, NULL);
    serial_write("[NET] Registered CAN capability: tcp_ip\n");

    // Initialize HTTP client request table
    net_requests_init();

    s_net_initialized = true;
    serial_write("[NET] Mongoose TCP/IP initialization complete\n");
}

// ===== Poll Task =====

__attribute__((used, noinline, aligned(16), section(".text")))
void mongoose_poll_task(void) {
    // Validate kernel space
    uint64_t rsp;
    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    if (rsp < 0xFFFF800000000000) {
        asm volatile("cli; hlt");
        while (1);
    }

    // Initial delay for system stability
    for (volatile int i = 0; i < 2000000; i++) {
        asm volatile("pause");
    }

    // Main polling loop
    while (1) {
        // Validate stack
        asm volatile("mov %%rsp, %0" : "=r"(rsp));
        if (rsp < 0xFFFF800000000000) break;

        // Poll Mongoose (non-blocking) and check for timed-out requests
        if (s_net_initialized) {
            mg_mgr_poll(&s_mgr, 0);
            net_check_timeouts();
        }

        // Short delay then yield
        for (volatile int i = 0; i < 5000; i++) {
            asm volatile("pause");
        }
        asm volatile("hlt");
    }

    asm volatile("cli; hlt");
    while (1);
}

// ===== Status Query =====

void net_get_status(net_status_t *status) {
    extern void *memset(void *, int, size_t);
    if (!status) return;
    memset(status, 0, sizeof(*status));
    status->stack_running = s_net_initialized ? 1 : 0;
    status->ip[0] = 10; status->ip[1] = 0; status->ip[2] = 2; status->ip[3] = 15;
    status->netmask[0] = 255; status->netmask[1] = 255; status->netmask[2] = 255; status->netmask[3] = 0;
    status->gateway[0] = 10; status->gateway[1] = 0; status->gateway[2] = 2; status->gateway[3] = 2;
    status->rx_count = s_rx_count;
    status->tx_count = s_tx_count;
}
