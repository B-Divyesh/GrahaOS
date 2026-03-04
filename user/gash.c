// user/gash.c - COMPLETE FILE
#include "syscalls.h"
#include "../kernel/state.h"
#include "../kernel/fs/grahafs.h"

// Helper functions
int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n && s1[i] && s2[i]; i++) {
        if (s1[i] != s2[i]) {
            return (unsigned char)s1[i] - (unsigned char)s2[i];
        }
    }
    return 0;
}

void print(const char *str) {
    while (*str) syscall_putc(*str++);
}

size_t strlen(const char *str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}

char* strcpy(char* dest, const char* src) {
    char* ret = dest;
    while ((*dest++ = *src++));
    return ret;
}

void int_to_string(int num, char *str) {
    if (num == 0) {
        str[0] = '0';
        str[1] = '\0';
        return;
    }

    int i = 0, is_negative = 0;
    if (num < 0) {
        is_negative = 1;
        num = -num;
    }

    char temp[12];
    while (num > 0) {
        temp[i++] = '0' + (num % 10);
        num /= 10;
    }

    int j = 0;
    if (is_negative) str[j++] = '-';
    while (i > 0) str[j++] = temp[--i];
    str[j] = '\0';
}

void uint64_to_str(uint64_t num, char *str) {
    if (num == 0) {
        str[0] = '0';
        str[1] = '\0';
        return;
    }
    char temp[21];
    int i = 0;
    while (num > 0) {
        temp[i++] = '0' + (num % 10);
        num /= 10;
    }
    int j = 0;
    while (i > 0) str[j++] = temp[--i];
    str[j] = '\0';
}

// Print uint64 with a label
void print_u64(const char *label, uint64_t val) {
    print(label);
    char buf[21];
    uint64_to_str(val, buf);
    print(buf);
    print("\n");
}

// Print uint32 with a label
void print_u32(const char *label, uint32_t val) {
    print_u64(label, (uint64_t)val);
}

void readline(char *buffer, int max_len) {
    int i = 0;
    while (i < max_len - 1) {
        char c = syscall_getc();
        
        if (c == '\n') {
            break;
        } else if (c == '\b') {
            if (i > 0) {
                i--;
                print("\b \b");
            }
        } else {
            buffer[i++] = c;
            syscall_putc(c);
        }
    }
    buffer[i] = '\0';
    print("\n");
}

// Parse command line into argv array
int parse_command(char* cmd, char* argv[], int max_args) {
    int argc = 0;
    char* p = cmd;
    
    while (*p && argc < max_args - 1) {
        // Skip whitespace
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        
        // Start of argument
        argv[argc++] = p;
        
        // Find end of argument
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    
    argv[argc] = NULL;
    return argc;
}

// Command implementations
void cmd_ls(char* path) {
    if (!path || strlen(path) == 0) path = "/";
    
    print("Directory listing of ");
    print(path);
    print(":\n");
    
    user_dirent_t dirent;
    uint32_t index = 0;
    int count = 0;
    
    while (1) {
        int result = syscall_readdir(path, index, &dirent);
        
        if (result <= 0) {
            if (result < 0 && index == 0) {
                print("ls: cannot access '");
                print(path);
                print("': ");
                if (result == -2) {
                    print("Not a directory\n");
                } else {
                    print("No such file or directory\n");
                }
            }
            break;
        }
        
        // Skip empty entries
        if (dirent.name[0] == '\0') {
            index++;
            continue;
        }
        
        // Print entry with type indicator
        if (dirent.type == 2) { // VFS_DIRECTORY
            print(dirent.name);
            print("/");
        } else {
            print(dirent.name);
        }
        print("\n");
        
        count++;
        index++;
    }
    
    if (count == 0 && index > 0) {
        print("(empty)\n");
    }
}

void cmd_cat(const char* filename) {
    int fd = syscall_open(filename);
    if (fd < 0) {
        print("cat: ");
        print(filename);
        print(": No such file or directory\n");
        return;
    }

    char buffer[129];
    ssize_t bytes_read;
    while ((bytes_read = syscall_read(fd, buffer, 128)) > 0) {
        buffer[bytes_read] = '\0';
        print(buffer);
    }
    syscall_close(fd);
}

void cmd_touch(const char* filename) {
    if (syscall_create(filename, 0644) < 0) {
        print("touch: cannot create '");
        print(filename);
        print("': File exists or error\n");
    } else {
        print("Created file: ");
        print(filename);
        print("\n");
    }
}

void cmd_mkdir(const char* dirname) {
    if (syscall_mkdir(dirname, 0755) < 0) {
        print("mkdir: cannot create directory '");
        print(dirname);
        print("': File exists or error\n");
    } else {
        print("Created directory: ");
        print(dirname);
        print("\n");
    }
}

void cmd_echo(char* argv[], int argc) {
    if (argc < 2) return;
    
    // Check for redirection
    int redirect_index = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], ">") == 0) {
            redirect_index = i;
            break;
        }
    }
    
    if (redirect_index > 0 && redirect_index < argc - 1) {
        // Write to file
        const char* filename = argv[redirect_index + 1];
        
        // Create or truncate file
        syscall_create(filename, 0644);
        
        int fd = syscall_open(filename);
        if (fd < 0) {
            print("echo: cannot open file\n");
            return;
        }
        
        // Write arguments before > to file
        for (int i = 1; i < redirect_index; i++) {
            syscall_write(fd, argv[i], strlen(argv[i]));
            if (i < redirect_index - 1) {
                syscall_write(fd, " ", 1);
            }
        }
        syscall_write(fd, "\n", 1);
        syscall_close(fd);
        
        print("Written to ");
        print(filename);
        print("\n");
    } else {
        // Print to screen
        for (int i = 1; i < argc; i++) {
            print(argv[i]);
            if (i < argc - 1) print(" ");
        }
        print("\n");
    }
}

void cmd_memstate(void) {
    state_memory_t mem;
    long ret = syscall_get_system_state(STATE_CAT_MEMORY, &mem, sizeof(mem));
    if (ret < 0) {
        print("memstate: failed to get memory state\n");
        return;
    }

    print("=== Memory State ===\n");
    print("Physical Memory:\n");
    print_u64("  Total:     ", mem.total_physical);
    print_u64("  Used:      ", mem.used_physical);
    print_u64("  Free:      ", mem.free_physical);
    print_u64("  Page Size: ", mem.page_size);
    print_u64("  Total Pages: ", mem.total_pages);
    print_u64("  Used Pages:  ", mem.used_pages);

    // Also show filesystem stats
    state_filesystem_t fs;
    ret = syscall_get_system_state(STATE_CAT_FILESYSTEM, &fs, sizeof(fs));
    if (ret > 0) {
        print("\nFilesystem:\n");
        print_u32("  Open Files:     ", fs.open_files);
        print_u32("  Block Devices:  ", fs.block_devices);
        print_u32("  Mounted FS:     ", fs.mounted_fs);
        if (fs.grahafs_mounted) {
            print("  GrahaFS: mounted\n");
            print_u32("    Total Blocks: ", fs.grahafs_total_blocks);
            print_u32("    Free Blocks:  ", fs.grahafs_free_blocks);
            print_u32("    Free Inodes:  ", fs.grahafs_free_inodes);
            print_u32("    Block Size:   ", fs.grahafs_block_size);
        } else {
            print("  GrahaFS: not mounted\n");
        }
    }
    print("====================\n");
}

static const char *proc_state_str(uint32_t state) {
    switch (state) {
        case STATE_PROC_ZOMBIE:  return "ZOMBIE ";
        case STATE_PROC_READY:   return "READY  ";
        case STATE_PROC_RUNNING: return "RUNNING";
        case STATE_PROC_BLOCKED: return "BLOCKED";
        default:                 return "UNKNOWN";
    }
}

void cmd_ps(void) {
    state_process_list_t procs;
    long ret = syscall_get_system_state(STATE_CAT_PROCESSES, &procs, sizeof(procs));
    if (ret < 0) {
        print("ps: failed to get process list\n");
        return;
    }

    print("PID  PPID PGID STATE   HEAP_USED  NAME\n");
    print("---- ---- ---- ------- ---------- ----------------\n");

    char buf[21];
    for (uint32_t i = 0; i < procs.count; i++) {
        state_process_t *p = &procs.procs[i];

        // PID (left-padded to 4)
        int_to_string(p->pid, buf);
        int len = strlen(buf);
        for (int j = 0; j < 4 - len; j++) print(" ");
        print(buf);
        print(" ");

        // PPID
        int_to_string(p->parent_pid, buf);
        len = strlen(buf);
        for (int j = 0; j < 4 - len; j++) print(" ");
        print(buf);
        print(" ");

        // PGID
        int_to_string(p->pgid, buf);
        len = strlen(buf);
        for (int j = 0; j < 4 - len; j++) print(" ");
        print(buf);
        print(" ");

        // State
        print(proc_state_str(p->state));
        print(" ");

        // Heap used
        uint64_to_str(p->heap_used, buf);
        len = strlen(buf);
        for (int j = 0; j < 10 - len; j++) print(" ");
        print(buf);
        print(" ");

        // Name
        if (p->name[0]) {
            print(p->name);
        } else {
            print("<unnamed>");
        }
        print("\n");
    }

    print("\nTotal: ");
    uint64_to_str(procs.count, buf);
    print(buf);
    print(" processes\n");
}

static const char *driver_type_str(uint32_t type) {
    switch (type) {
        case STATE_DRIVER_BLOCK:   return "BLOCK  ";
        case STATE_DRIVER_INPUT:   return "INPUT  ";
        case STATE_DRIVER_DISPLAY: return "DISPLAY";
        case STATE_DRIVER_TIMER:   return "TIMER  ";
        case STATE_DRIVER_SERIAL:  return "SERIAL ";
        case STATE_DRIVER_FS:      return "FS     ";
        default:                   return "OTHER  ";
    }
}

void cmd_drivers(void) {
    state_driver_list_t drivers;
    long ret = syscall_get_system_state(STATE_CAT_DRIVERS, &drivers, sizeof(drivers));
    if (ret < 0) {
        print("drivers: failed to get driver list\n");
        return;
    }

    char buf[21];

    print("=== Registered Drivers ===\n\n");
    for (uint32_t i = 0; i < drivers.count; i++) {
        state_driver_info_t *d = &drivers.drivers[i];

        print("[");
        uint64_to_str(i, buf);
        print(buf);
        print("] ");
        print(d->name);
        print("  type=");
        print(driver_type_str(d->type));
        print("\n");

        // Print stats
        if (d->stat_count > 0) {
            print("  Stats:\n");
            for (uint32_t j = 0; j < d->stat_count; j++) {
                print("    ");
                print(d->stats[j].key);
                print(" = ");
                uint64_to_str(d->stats[j].value, buf);
                print(buf);
                print("\n");
            }
        }

        // Print operations
        if (d->op_count > 0) {
            print("  Ops:");
            for (uint32_t j = 0; j < d->op_count; j++) {
                print(" ");
                print(d->ops[j].name);
                if (d->ops[j].flags == 1) {
                    print("(w)");
                } else {
                    print("(r)");
                }
            }
            print("\n");
        }
        print("\n");
    }

    print("Total: ");
    uint64_to_str(drivers.count, buf);
    print(buf);
    print(" drivers\n");
}

// --- Capability Activation Network Commands ---

static const char *cap_type_str(uint32_t type) {
    switch (type) {
        case 0: return "HARDWARE";
        case 1: return "DRIVER";
        case 2: return "SERVICE";
        case 3: return "APPLICATION";
        case 4: return "FEATURE";
        case 5: return "COMPOSITE";
        default: return "UNKNOWN";
    }
}

static const char *cap_state_str(uint32_t state) {
    switch (state) {
        case 0: return "OFF";
        case 1: return "STARTING";
        case 2: return "ON";
        case 3: return "ERROR";
        default: return "???";
    }
}

void cmd_caps(void) {
    state_cap_list_t caps;
    long ret = syscall_get_system_state(STATE_CAT_CAPABILITIES, &caps, sizeof(caps));
    if (ret < 0) {
        print("caps: failed to get capability list\n");
        return;
    }

    char buf[21];
    uint32_t active_count = 0;

    print("=== Capability Activation Map ===\n");

    // Print grouped by layer (type 0-5)
    for (uint32_t layer = 0; layer <= 5; layer++) {
        int has_entries = 0;
        for (uint32_t i = 0; i < caps.count; i++) {
            if (caps.caps[i].deleted) continue;
            if (caps.caps[i].type == layer) {
                if (!has_entries) {
                    print("--- ");
                    print(cap_type_str(layer));
                    print(" ---\n");
                    has_entries = 1;
                }

                // State indicator
                if (caps.caps[i].state == 2) {
                    print("  [ON]  ");
                    active_count++;
                } else if (caps.caps[i].state == 3) {
                    print("  [ERR] ");
                } else {
                    print("  [OFF] ");
                }

                print(caps.caps[i].name);

                // Show deps for non-HARDWARE caps
                if (caps.caps[i].dep_count > 0) {
                    print(" (deps: ");
                    for (uint32_t d = 0; d < caps.caps[i].dep_count; d++) {
                        uint32_t dep_idx = caps.caps[i].dep_indices[d];
                        if (dep_idx < caps.count) {
                            if (d > 0) print(", ");
                            print(caps.caps[dep_idx].name);
                        }
                    }
                    print(")");
                }
                print("\n");
            }
        }
    }

    print("\nTotal: ");
    uint64_to_str(caps.count, buf);
    print(buf);
    print(" capabilities, ");
    uint64_to_str(active_count, buf);
    print(buf);
    print(" active\n");
}

void cmd_activate(const char *name) {
    if (!name || name[0] == '\0') {
        print("activate: usage: activate <capability_name>\n");
        return;
    }
    int result = syscall_cap_activate(name);
    if (result == 0) {
        print("Activated: ");
        print(name);
        print("\n");
    } else {
        print("Failed to activate '");
        print(name);
        print("' (error=");
        char buf[12];
        int_to_string(result, buf);
        print(buf);
        print(")\n");
    }
}

void cmd_deactivate(const char *name) {
    if (!name || name[0] == '\0') {
        print("deactivate: usage: deactivate <capability_name>\n");
        return;
    }
    int result = syscall_cap_deactivate(name);
    if (result == 0) {
        print("Deactivated: ");
        print(name);
        print("\n");
    } else {
        print("Failed to deactivate '");
        print(name);
        print("' (error=");
        char buf[12];
        int_to_string(result, buf);
        print(buf);
        print(")\n");
    }
}

void cmd_why_not(const char *name) {
    if (!name || name[0] == '\0') {
        print("why_not: usage: why_not <capability_name>\n");
        return;
    }

    // Find the capability in the list
    state_cap_list_t caps;
    long ret = syscall_get_system_state(STATE_CAT_CAPABILITIES, &caps, sizeof(caps));
    if (ret < 0) {
        print("why_not: failed to query capabilities\n");
        return;
    }

    int found = -1;
    for (uint32_t i = 0; i < caps.count; i++) {
        if (strcmp(caps.caps[i].name, name) == 0) {
            found = (int)i;
            break;
        }
    }

    if (found < 0) {
        print("why_not: '");
        print(name);
        print("' not found in capability registry\n");
        return;
    }

    state_cap_entry_t *cap = &caps.caps[found];

    if (cap->state == 2) {
        print("'");
        print(name);
        print("' is already ON\n");
        return;
    }

    if (cap->state == 3) {
        print("'");
        print(name);
        print("' is in ERROR state (activation previously failed)\n");
    }

    // Check which deps are OFF
    int all_deps_on = 1;
    for (uint32_t d = 0; d < cap->dep_count; d++) {
        uint32_t dep_idx = cap->dep_indices[d];
        if (dep_idx < caps.count && caps.caps[dep_idx].state != 2) {
            print("  dep '");
            print(caps.caps[dep_idx].name);
            print("' is ");
            print(cap_state_str(caps.caps[dep_idx].state));
            print("\n");
            all_deps_on = 0;
        }
    }

    if (all_deps_on && cap->state != 3) {
        print("'");
        print(name);
        print("' can be activated (all deps are ON)\n");
    }
}

void cmd_available(void) {
    state_cap_list_t caps;
    long ret = syscall_get_system_state(STATE_CAT_CAPABILITIES, &caps, sizeof(caps));
    if (ret < 0) {
        print("available: failed to query capabilities\n");
        return;
    }

    print("=== Available for Activation ===\n");
    int found = 0;
    for (uint32_t i = 0; i < caps.count; i++) {
        if (caps.caps[i].state == 2) continue;  // skip already ON
        if (caps.caps[i].state == 1) continue;  // skip STARTING

        // Check if all deps are ON
        int all_deps_on = 1;
        for (uint32_t d = 0; d < caps.caps[i].dep_count; d++) {
            uint32_t dep_idx = caps.caps[i].dep_indices[d];
            if (dep_idx < caps.count && caps.caps[dep_idx].state != 2) {
                all_deps_on = 0;
                break;
            }
        }

        if (all_deps_on) {
            print("  ");
            print(caps.caps[i].name);
            print(" (");
            print(cap_type_str(caps.caps[i].type));
            print(")\n");
            found++;
        }
    }

    if (!found) {
        print("  (none - all capabilities are active)\n");
    }
}

void cmd_sysstate(void) {
    // Full system state dump - the AI-readable command
    state_snapshot_t state;
    long ret = syscall_get_system_state(STATE_CAT_ALL, &state, sizeof(state));
    if (ret < 0) {
        print("sysstate: failed to get system state\n");
        return;
    }

    char buf[21];

    print("=== GrahaOS System State v");
    uint64_to_str(state.version, buf);
    print(buf);
    print(" ===\n\n");

    // Memory
    print("[MEMORY]\n");
    print_u64("  total=", state.memory.total_physical);
    print_u64("  free=", state.memory.free_physical);
    print_u64("  used=", state.memory.used_physical);
    print_u64("  pages_total=", state.memory.total_pages);
    print_u64("  pages_used=", state.memory.used_pages);

    // CPU
    print("\n[SYSTEM]\n");
    print_u32("  cpus=", state.system.cpu_count);
    print_u32("  bsp_lapic=", state.system.bsp_lapic_id);
    print_u32("  schedules=", state.system.schedule_count);
    print_u32("  ctx_switches=", state.system.context_switches);

    // Processes
    print("\n[PROCESSES] count=");
    uint64_to_str(state.processes.count, buf);
    print(buf);
    print("\n");
    for (uint32_t i = 0; i < state.processes.count; i++) {
        state_process_t *p = &state.processes.procs[i];
        print("  pid=");
        int_to_string(p->pid, buf);
        print(buf);
        print(" state=");
        print(proc_state_str(p->state));
        print(" name=");
        print(p->name[0] ? p->name : "<unnamed>");
        print("\n");
    }

    // Filesystem
    print("\n[FILESYSTEM]\n");
    print_u32("  open_files=", state.filesystem.open_files);
    print_u32("  block_devices=", state.filesystem.block_devices);
    print_u32("  mounted_fs=", state.filesystem.mounted_fs);
    print_u32("  grahafs_mounted=", state.filesystem.grahafs_mounted);
    if (state.filesystem.grahafs_mounted) {
        print_u32("  grahafs_total_blocks=", state.filesystem.grahafs_total_blocks);
        print_u32("  grahafs_free_blocks=", state.filesystem.grahafs_free_blocks);
        print_u32("  grahafs_free_inodes=", state.filesystem.grahafs_free_inodes);
    }

    // Drivers
    print("\n[DRIVERS] count=");
    uint64_to_str(state.drivers.count, buf);
    print(buf);
    print("\n");
    for (uint32_t i = 0; i < state.drivers.count; i++) {
        state_driver_info_t *d = &state.drivers.drivers[i];
        print("  ");
        print(d->name);
        print(" type=");
        print(driver_type_str(d->type));
        print(" ops=");
        uint64_to_str(d->op_count, buf);
        print(buf);
        print(" stats=");
        uint64_to_str(d->stat_count, buf);
        print(buf);
        print("\n");
    }

    // Capabilities
    print("\n[CAPABILITIES] count=");
    uint64_to_str(state.capabilities.count, buf);
    print(buf);
    print("\n");
    for (uint32_t i = 0; i < state.capabilities.count; i++) {
        state_cap_entry_t *c = &state.capabilities.caps[i];
        print("  ");
        print(c->name);
        print(" type=");
        print(cap_type_str(c->type));
        print(" state=");
        print(cap_state_str(c->state));
        print(" deps=");
        uint64_to_str(c->dep_count, buf);
        print(buf);
        print(" ops=");
        uint64_to_str(c->op_count, buf);
        print(buf);
        print("\n");
    }

    print("\n=== End System State ===\n");
}

// --- Phase 8c: AI Metadata Commands ---

static void zero_mem(void *ptr, size_t n) {
    uint8_t *p = (uint8_t *)ptr;
    for (size_t i = 0; i < n; i++) p[i] = 0;
}

void cmd_tag(const char *path, const char *tags) {
    if (!path || !tags) {
        print("tag: usage: tag <path> <tags>\n");
        return;
    }

    grahafs_ai_metadata_t meta;
    zero_mem(&meta, sizeof(meta));
    meta.flags = GRAHAFS_META_FLAG_TAGS;

    // Copy tags string (max 511 chars)
    size_t len = strlen(tags);
    if (len > 511) len = 511;
    for (size_t i = 0; i < len; i++) meta.tags[i] = tags[i];
    meta.tags[len] = '\0';

    int ret = syscall_set_ai_metadata(path, &meta);
    if (ret == 0) {
        print("Tagged '");
        print(path);
        print("' with: ");
        print(tags);
        print("\n");
    } else {
        print("tag: failed (error=");
        char buf[12];
        int_to_string(ret, buf);
        print(buf);
        print(")\n");
    }
}

void cmd_meta(const char *path) {
    if (!path) {
        print("meta: usage: meta <path>\n");
        return;
    }

    grahafs_ai_metadata_t meta;
    zero_mem(&meta, sizeof(meta));

    int ret = syscall_get_ai_metadata(path, &meta);
    if (ret < 0) {
        print("meta: failed to get metadata (error=");
        char buf[12];
        int_to_string(ret, buf);
        print(buf);
        print(")\n");
        return;
    }

    char buf[21];

    print("=== AI Metadata: ");
    print(path);
    print(" ===\n");

    print("  Importance: ");
    uint64_to_str(meta.importance, buf);
    print(buf);
    print("\n");

    print("  Tags: ");
    if (meta.tags[0]) {
        print(meta.tags);
    } else {
        print("(none)");
    }
    print("\n");

    print("  Access Count: ");
    uint64_to_str(meta.access_count, buf);
    print(buf);
    print("\n");

    if (meta.flags & GRAHAFS_AI_HAS_SUMMARY) {
        print("  Summary: ");
        print(meta.summary);
        print("\n");
    }

    if (meta.flags & GRAHAFS_AI_HAS_EMBEDDING) {
        print("  Embedding: ");
        uint64_to_str(meta.embedding_dim, buf);
        print(buf);
        print(" dimensions\n");
    }

    print("  Flags: 0x");
    // Simple hex print for flags
    char hex[9];
    uint32_t f = meta.flags;
    for (int i = 7; i >= 0; i--) {
        hex[i] = "0123456789abcdef"[f & 0xf];
        f >>= 4;
    }
    hex[8] = '\0';
    print(hex);
    print("\n");
}

void cmd_importance(const char *path, const char *score_str) {
    if (!path || !score_str) {
        print("importance: usage: importance <path> <0-100>\n");
        return;
    }

    // Parse score
    int score = 0;
    for (int i = 0; score_str[i]; i++) {
        if (score_str[i] >= '0' && score_str[i] <= '9') {
            score = score * 10 + (score_str[i] - '0');
        }
    }

    grahafs_ai_metadata_t meta;
    zero_mem(&meta, sizeof(meta));
    meta.flags = GRAHAFS_META_FLAG_IMPORTANCE;
    meta.importance = (uint32_t)score;

    int ret = syscall_set_ai_metadata(path, &meta);
    if (ret == 0) {
        print("Set importance of '");
        print(path);
        print("' to ");
        print(score_str);
        print("\n");
    } else {
        print("importance: failed (error=");
        char buf[12];
        int_to_string(ret, buf);
        print(buf);
        print(")\n");
    }
}

void cmd_summary(const char *path, char *argv[], int argc, int start_arg) {
    if (!path || start_arg >= argc) {
        print("summary: usage: summary <path> <text...>\n");
        return;
    }

    grahafs_ai_metadata_t meta;
    zero_mem(&meta, sizeof(meta));
    meta.flags = GRAHAFS_META_FLAG_SUMMARY;

    // Join remaining args into summary string
    size_t pos = 0;
    for (int i = start_arg; i < argc && pos < 1023; i++) {
        size_t len = strlen(argv[i]);
        if (pos + len + 1 >= 1023) len = 1023 - pos - 1;
        for (size_t j = 0; j < len; j++) meta.summary[pos++] = argv[i][j];
        if (i < argc - 1 && pos < 1022) meta.summary[pos++] = ' ';
    }
    meta.summary[pos] = '\0';

    int ret = syscall_set_ai_metadata(path, &meta);
    if (ret == 0) {
        print("Set summary of '");
        print(path);
        print("'\n");
    } else {
        print("summary: failed (error=");
        char buf[12];
        int_to_string(ret, buf);
        print(buf);
        print(")\n");
    }
}

void cmd_search(const char *tag) {
    if (!tag || tag[0] == '\0') {
        print("search: usage: search <tag>\n");
        return;
    }

    grahafs_search_results_t results;
    zero_mem(&results, sizeof(results));

    int ret = syscall_search_by_tag(tag, &results, 16);
    if (ret < 0) {
        print("search: failed (error=");
        char buf[12];
        int_to_string(ret, buf);
        print(buf);
        print(")\n");
        return;
    }

    char buf[21];
    print("Search results for tag '");
    print(tag);
    print("': ");
    uint64_to_str(results.count, buf);
    print(buf);
    print(" match(es)\n");

    for (uint32_t i = 0; i < results.count; i++) {
        print("  ");
        print(results.results[i].path);
        print("  importance=");
        uint64_to_str(results.results[i].importance, buf);
        print(buf);
        print("  tags=");
        print(results.results[i].tags);
        print("\n");
    }
}

// --- Phase 8d: CAN Event Commands ---

void cmd_watch(const char *name) {
    if (!name || name[0] == '\0') {
        print("watch: usage: watch <capability_name>\n");
        return;
    }
    int ret = syscall_cap_watch(name);
    if (ret == 0) {
        print("Watching: ");
        print(name);
        print("\n");
    } else {
        print("watch: failed (error=");
        char buf[12];
        int_to_string(ret, buf);
        print(buf);
        print(")\n");
    }
}

void cmd_unwatch(const char *name) {
    if (!name || name[0] == '\0') {
        print("unwatch: usage: unwatch <capability_name>\n");
        return;
    }
    int ret = syscall_cap_unwatch(name);
    if (ret == 0) {
        print("Unwatched: ");
        print(name);
        print("\n");
    } else {
        print("unwatch: failed (error=");
        char buf[12];
        int_to_string(ret, buf);
        print(buf);
        print(")\n");
    }
}

static const char *event_type_str(uint32_t type) {
    switch (type) {
        case STATE_CAP_EVENT_ACTIVATED:   return "ACTIVATED";
        case STATE_CAP_EVENT_DEACTIVATED: return "DEACTIVATED";
        case STATE_CAP_EVENT_ERROR:       return "ERROR";
        default:                          return "UNKNOWN";
    }
}

void cmd_events(void) {
    // Non-blocking poll: get pending events without blocking
    state_cap_event_t events[16];
    int ret = syscall_cap_poll_nonblock(events, 16);

    if (ret == -99 || ret == 0) {
        print("No pending events.\n");
        return;
    }

    if (ret < 0) {
        print("events: poll failed (error=");
        char buf[12];
        int_to_string(ret, buf);
        print(buf);
        print(")\n");
        return;
    }

    char buf[21];
    print("=== CAN Events (");
    int_to_string(ret, buf);
    print(buf);
    print(" pending) ===\n");

    for (int i = 0; i < ret; i++) {
        print("  [");
        int_to_string(i, buf);
        print(buf);
        print("] ");
        print(events[i].cap_name);
        print(": ");
        print(event_type_str(events[i].type));
        print(" (");
        print(cap_state_str(events[i].old_state));
        print(" -> ");
        print(cap_state_str(events[i].new_state));
        print(")\n");
    }
}

// --- Phase 9a: Network Commands ---

void cmd_ifconfig(void) {
    uint8_t info[7];
    int ret = syscall_net_ifconfig(info);

    if (ret == -2) {
        print("ifconfig: no network interface found\n");
        return;
    }
    if (ret < 0) {
        print("ifconfig: failed\n");
        return;
    }

    print("eth0: MAC=");
    const char *hex = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        char h[3];
        h[0] = hex[(info[i] >> 4) & 0xf];
        h[1] = hex[info[i] & 0xf];
        h[2] = '\0';
        print(h);
        if (i < 5) print(":");
    }
    print(" Link=");
    print(info[6] ? "UP" : "DOWN");
    print("\n");
}

// Phase 9b: Network status
void cmd_netstat(void) {
    // net_status_t: stack_running(1) + ip(4) + netmask(4) + gateway(4) + rx_count(4) + tx_count(4) = 21 bytes
    uint8_t buf[21];
    for (int i = 0; i < 21; i++) buf[i] = 0;
    int ret = syscall_net_status(buf);
    if (ret < 0) {
        print("netstat: failed to get network status\n");
        return;
    }

    print("TCP/IP Stack: ");
    print(buf[0] ? "RUNNING" : "STOPPED");
    print("\n");

    // IP address
    char num[4];
    print("IP:      ");
    for (int i = 0; i < 4; i++) {
        // Convert byte to decimal
        int val = buf[1 + i];
        int idx = 0;
        if (val >= 100) { num[idx++] = '0' + val / 100; val %= 100; }
        if (val >= 10 || idx > 0) { num[idx++] = '0' + val / 10; val %= 10; }
        num[idx++] = '0' + val;
        num[idx] = '\0';
        print(num);
        if (i < 3) print(".");
    }
    print("\n");

    print("Netmask: ");
    for (int i = 0; i < 4; i++) {
        int val = buf[5 + i];
        int idx = 0;
        if (val >= 100) { num[idx++] = '0' + val / 100; val %= 100; }
        if (val >= 10 || idx > 0) { num[idx++] = '0' + val / 10; val %= 10; }
        num[idx++] = '0' + val;
        num[idx] = '\0';
        print(num);
        if (i < 3) print(".");
    }
    print("\n");

    print("Gateway: ");
    for (int i = 0; i < 4; i++) {
        int val = buf[9 + i];
        int idx = 0;
        if (val >= 100) { num[idx++] = '0' + val / 100; val %= 100; }
        if (val >= 10 || idx > 0) { num[idx++] = '0' + val / 10; val %= 10; }
        num[idx++] = '0' + val;
        num[idx] = '\0';
        print(num);
        if (i < 3) print(".");
    }
    print("\n");

    // RX/TX counts (uint32_t at offset 13 and 17)
    uint32_t rx = *(uint32_t *)&buf[13];
    uint32_t tx = *(uint32_t *)&buf[17];
    print_u32("RX packets: ", rx);
    print_u32("TX packets: ", tx);
}

void cmd_http(const char *url) {
    if (!url || url[0] == '\0') {
        print("http: usage: http <url>\n");
        return;
    }

    print("Fetching ");
    print(url);
    print("...\n");

    char response[4096];
    int ret = syscall_http_get(url, response, sizeof(response));

    if (ret < 0) {
        print("http: error ");
        char num[12];
        int_to_string(ret, num);
        print(num);
        if (ret == -10) print(" (timeout)");
        else if (ret == -11) print(" (DNS failed)");
        else if (ret == -12) print(" (connection failed)");
        else if (ret == -17) print(" (no network)");
        print("\n");
        return;
    }

    print("Response (");
    char len_str[12];
    int_to_string(ret, len_str);
    print(len_str);
    print(" bytes):\n");
    // Print response character by character (it's already null-terminated)
    for (int i = 0; i < ret; i++) {
        syscall_putc(response[i]);
    }
    print("\n");
}

void cmd_dns(const char *hostname) {
    if (!hostname || hostname[0] == '\0') {
        print("dns: usage: dns <hostname>\n");
        return;
    }

    print("Resolving ");
    print(hostname);
    print("...\n");

    uint8_t ip[4];
    int ret = syscall_dns_resolve(hostname, ip);

    if (ret < 0) {
        print("dns: resolution failed (error ");
        char num[12];
        int_to_string(ret, num);
        print(num);
        print(")\n");
        return;
    }

    print(hostname);
    print(" -> ");
    for (int i = 0; i < 4; i++) {
        char num[4];
        int val = ip[i];
        int idx = 0;
        if (val >= 100) { num[idx++] = '0' + val / 100; val %= 100; }
        if (val >= 10 || idx > 0) { num[idx++] = '0' + val / 10; val %= 10; }
        num[idx++] = '0' + val;
        num[idx] = '\0';
        print(num);
        if (i < 3) print(".");
    }
    print("\n");
}

// Helper: spawn and wait for a program
int run_program(const char *path) {
    int pid = syscall_spawn(path);
    if (pid < 0) return -1;

    int exit_status;
    syscall_wait(&exit_status);
    return exit_status;
}

// Main shell
void _start(void) {
    print("=== GrahaOS Shell v2.0 (spawn model) ===\n");
    print("Type 'help' for commands.\n\n");

    // Display our PID
    int my_pid = syscall_getpid();
    print("Shell PID: ");
    char pid_str[12];
    int_to_string(my_pid, pid_str);
    print(pid_str);
    print("\n\n");

    char command_buffer[256];
    char* argv[32];

    while (1) {
        print("gash> ");
        readline(command_buffer, sizeof(command_buffer));

        int argc = parse_command(command_buffer, argv, 32);
        if (argc == 0) continue;

        char* cmd = argv[0];

        if (strcmp(cmd, "help") == 0) {
            print("Available commands:\n");
            print("  help                - Show this message\n");
            print("  ls [path]           - List directory contents\n");
            print("  cat <file>          - Display file contents\n");
            print("  touch <file>        - Create empty file\n");
            print("  mkdir <dir>         - Create directory\n");
            print("  echo <text>         - Print text\n");
            print("  echo <text> > <file> - Write text to file\n");
            print("  memstate            - Show memory & filesystem state\n");
            print("  ps                  - List running processes\n");
            print("  drivers             - List registered drivers\n");
            print("  sysstate            - Full system state dump\n");
            print("  caps                - Capability activation map\n");
            print("  activate <name>     - Activate a capability\n");
            print("  deactivate <name>   - Deactivate a capability\n");
            print("  why_not <name>      - Explain why cap can't activate\n");
            print("  available           - Show activatable capabilities\n");
            print("  tag <path> <tags>   - Set AI tags on a file\n");
            print("  meta <path>         - Show AI metadata for a file\n");
            print("  importance <p> <n>  - Set importance (0-100)\n");
            print("  summary <path> <t>  - Set summary text\n");
            print("  search <tag>        - Search files by tag\n");
            print("  watch <cap>         - Watch capability state changes\n");
            print("  unwatch <cap>       - Stop watching a capability\n");
            print("  events              - Show pending CAN events\n");
            print("  ifconfig            - Show network interface info\n");
            print("  netstat             - Show TCP/IP stack status\n");
            print("  http <url>          - Fetch URL via HTTP/HTTPS GET\n");
            print("  dns <hostname>      - Resolve hostname to IP address\n");
            print("  pid                 - Show current process ID\n");
            print("  kill <pid>          - Terminate a process\n");
            print("  sync                - Flush filesystem to disk\n");
            print("  test                - Keyboard test\n");
            print("  grahai              - Run GCP interpreter\n");
            print("  exit                - Exit the shell\n");
            print("\nTest suites (also runnable directly):\n");
            print("  libctest            - libc/malloc test suite\n");
            print("  sbrk_test           - sbrk syscall test\n");
            print("  printf_test         - printf format test\n");
            print("  spawntest           - spawn/wait test suite\n");
            print("  cantest             - CAN capability test suite\n");
            print("  metatest            - AI metadata test suite\n");
            print("  eventtest           - CAN event test suite\n");
            print("  nettest             - E1000 NIC test suite\n");
            print("  httptest            - HTTP server test suite\n");
            print("  dnstest             - DNS + HTTP client test suite\n");
        }
        else if (strcmp(cmd, "ls") == 0) {
            cmd_ls(argc > 1 ? argv[1] : "/");
        }
        else if (strcmp(cmd, "cat") == 0) {
            if (argc < 2) {
                print("cat: missing operand\n");
            } else {
                cmd_cat(argv[1]);
            }
        }
        else if (strcmp(cmd, "touch") == 0) {
            if (argc < 2) {
                print("touch: missing operand\n");
            } else {
                cmd_touch(argv[1]);
            }
        }
        else if (strcmp(cmd, "mkdir") == 0) {
            if (argc < 2) {
                print("mkdir: missing operand\n");
            } else {
                cmd_mkdir(argv[1]);
            }
        }
        else if (strcmp(cmd, "echo") == 0) {
            cmd_echo(argv, argc);
        }
        else if (strcmp(cmd, "sync") == 0) {
            print("Syncing filesystem to disk...\n");
            syscall_sync();
            print("Sync complete.\n");
        }
        else if (strcmp(cmd, "memstate") == 0) {
            cmd_memstate();
        }
        else if (strcmp(cmd, "ps") == 0) {
            cmd_ps();
        }
        else if (strcmp(cmd, "drivers") == 0) {
            cmd_drivers();
        }
        else if (strcmp(cmd, "sysstate") == 0) {
            cmd_sysstate();
        }
        else if (strcmp(cmd, "caps") == 0) {
            cmd_caps();
        }
        else if (strcmp(cmd, "activate") == 0) {
            cmd_activate(argc > 1 ? argv[1] : "");
        }
        else if (strcmp(cmd, "deactivate") == 0) {
            cmd_deactivate(argc > 1 ? argv[1] : "");
        }
        else if (strcmp(cmd, "why_not") == 0) {
            cmd_why_not(argc > 1 ? argv[1] : "");
        }
        else if (strcmp(cmd, "available") == 0) {
            cmd_available();
        }
        else if (strcmp(cmd, "tag") == 0) {
            if (argc < 3) {
                print("tag: usage: tag <path> <tags>\n");
            } else {
                cmd_tag(argv[1], argv[2]);
            }
        }
        else if (strcmp(cmd, "meta") == 0) {
            if (argc < 2) {
                print("meta: usage: meta <path>\n");
            } else {
                cmd_meta(argv[1]);
            }
        }
        else if (strcmp(cmd, "importance") == 0) {
            if (argc < 3) {
                print("importance: usage: importance <path> <0-100>\n");
            } else {
                cmd_importance(argv[1], argv[2]);
            }
        }
        else if (strcmp(cmd, "summary") == 0) {
            if (argc < 3) {
                print("summary: usage: summary <path> <text...>\n");
            } else {
                cmd_summary(argv[1], argv, argc, 2);
            }
        }
        else if (strcmp(cmd, "search") == 0) {
            if (argc < 2) {
                print("search: usage: search <tag>\n");
            } else {
                cmd_search(argv[1]);
            }
        }
        else if (strcmp(cmd, "watch") == 0) {
            cmd_watch(argc > 1 ? argv[1] : "");
        }
        else if (strcmp(cmd, "unwatch") == 0) {
            cmd_unwatch(argc > 1 ? argv[1] : "");
        }
        else if (strcmp(cmd, "events") == 0) {
            cmd_events();
        }
        else if (strcmp(cmd, "ifconfig") == 0) {
            cmd_ifconfig();
        }
        else if (strcmp(cmd, "netstat") == 0) {
            cmd_netstat();
        }
        else if (strcmp(cmd, "http") == 0) {
            cmd_http(argc > 1 ? argv[1] : "");
        }
        else if (strcmp(cmd, "dns") == 0) {
            cmd_dns(argc > 1 ? argv[1] : "");
        }
        else if (strcmp(cmd, "pid") == 0) {
            int current_pid = syscall_getpid();
            print("PID: ");
            char buf[12];
            int_to_string(current_pid, buf);
            print(buf);
            print("\n");
        }
        else if (strcmp(cmd, "kill") == 0) {
            if (argc < 2) {
                print("kill: usage: kill <pid>\n");
            } else {
                // Simple string to int conversion
                int target_pid = 0;
                for (int i = 0; argv[1][i]; i++) {
                    if (argv[1][i] >= '0' && argv[1][i] <= '9') {
                        target_pid = target_pid * 10 + (argv[1][i] - '0');
                    }
                }
                int result = syscall_kill(target_pid, 1); // SIGTERM
                if (result < 0) {
                    print("kill: failed to kill process ");
                    print(argv[1]);
                    print("\n");
                } else {
                    print("Signal sent to process ");
                    print(argv[1]);
                    print("\n");
                }
            }
        }
        else if (strcmp(cmd, "test") == 0) {
            print("Keyboard test - type 'q' to quit\n");
            char ch;
            while ((ch = syscall_getc()) != 'q') {
                print("You typed: ");
                syscall_putc(ch);
                print("\n");
            }
            print("Test complete.\n");
        }
        else if (strcmp(cmd, "grahai") == 0) {
            print("Spawning grahai...\n");
            int pid = syscall_spawn("bin/grahai");
            if (pid < 0) {
                print("ERROR: Failed to spawn 'bin/grahai'\n");
            } else {
                print("grahai spawned (pid=");
                char spid[12];
                int_to_string(pid, spid);
                print(spid);
                print(")\n");

                int exit_status;
                syscall_wait(&exit_status);
                print("grahai completed (status=");
                int_to_string(exit_status, spid);
                print(spid);
                print(")\n");
            }
        }
        else if (strcmp(cmd, "exit") == 0) {
            print("Goodbye!\n");
            syscall_exit(0);
        }
        else {
            // Try to spawn as a program from /bin/
            char path[128];

            // Check if command starts with /
            if (cmd[0] == '/') {
                strcpy(path, cmd);
            } else {
                // Try bin/ prefix first
                strcpy(path, "bin/");
                int len = strlen(path);
                int cmd_len = strlen(cmd);
                if (len + cmd_len < 127) {
                    strcpy(path + len, cmd);
                }
            }

            int pid = syscall_spawn(path);
            if (pid < 0) {
                print("Unknown command: '");
                print(cmd);
                print("'\n");
                print("Type 'help' for available commands.\n");
            } else {
                // Wait for spawned program to complete
                int exit_status;
                syscall_wait(&exit_status);
            }
        }
    }
}