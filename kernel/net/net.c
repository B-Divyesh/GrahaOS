// kernel/net/net.c
// Phase 9b: Mongoose TCP/IP stack integration for GrahaOS
// Provides: mg_millis, mg_random, E1000 driver bridge, HTTP server, poll task

#include "net.h"
#include "net_task.h"
#include "kmalloc.h"
#include "klib.h"
#include "mongoose.h"
#include "../../arch/x86_64/drivers/e1000/e1000.h"
#include "../../arch/x86_64/drivers/serial/serial.h"
#include "../../arch/x86_64/cpu/interrupts.h"
#include "../../arch/x86_64/mm/pmm.h"
#include "../capability.h"

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

        // Poll Mongoose (non-blocking)
        if (s_net_initialized) {
            mg_mgr_poll(&s_mgr, 0);
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
