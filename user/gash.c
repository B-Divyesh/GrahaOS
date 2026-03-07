// user/gash.c - COMPLETE FILE
#include "syscalls.h"
#include "../kernel/state.h"
#include "../kernel/fs/grahafs.h"
#include "../kernel/fs/cluster.h"
#include "json.h"

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

// --- Command History (Phase 10d) ---
#define HISTORY_SIZE 32
#define HISTORY_LINE_MAX 256
static char history[HISTORY_SIZE][HISTORY_LINE_MAX];
static int history_count = 0;   // total entries stored
static int history_write = 0;   // next write slot (ring buffer)

static void history_add(const char *line) {
    if (line[0] == '\0') return;
    // Don't add duplicates of last entry
    if (history_count > 0) {
        int last = (history_write + HISTORY_SIZE - 1) % HISTORY_SIZE;
        if (strcmp(history[last], line) == 0) return;
    }
    int len = 0;
    while (line[len] && len < HISTORY_LINE_MAX - 1) {
        history[history_write][len] = line[len];
        len++;
    }
    history[history_write][len] = '\0';
    history_write = (history_write + 1) % HISTORY_SIZE;
    if (history_count < HISTORY_SIZE) history_count++;
}

static const char* history_get(int index) {
    // index 0 = most recent, 1 = second most recent, etc.
    if (index < 0 || index >= history_count) return ((void*)0);
    int pos = (history_write - 1 - index + HISTORY_SIZE * 2) % HISTORY_SIZE;
    return history[pos];
}

// --- Environment Variables (Phase 10d) ---
#define ENV_MAX 32
#define ENV_KEY_MAX 64
#define ENV_VAL_MAX 128
static char env_keys[ENV_MAX][ENV_KEY_MAX];
static char env_vals[ENV_MAX][ENV_VAL_MAX];
static int env_count = 0;
static int last_exit_status = 0;

static void env_set(const char *key, const char *val) {
    // Update existing
    for (int i = 0; i < env_count; i++) {
        if (strcmp(env_keys[i], key) == 0) {
            int j = 0;
            while (val[j] && j < ENV_VAL_MAX - 1) { env_vals[i][j] = val[j]; j++; }
            env_vals[i][j] = '\0';
            return;
        }
    }
    // Add new
    if (env_count < ENV_MAX) {
        int j = 0;
        while (key[j] && j < ENV_KEY_MAX - 1) { env_keys[env_count][j] = key[j]; j++; }
        env_keys[env_count][j] = '\0';
        j = 0;
        while (val[j] && j < ENV_VAL_MAX - 1) { env_vals[env_count][j] = val[j]; j++; }
        env_vals[env_count][j] = '\0';
        env_count++;
    }
}

static const char* env_get(const char *key) {
    for (int i = 0; i < env_count; i++) {
        if (strcmp(env_keys[i], key) == 0) return env_vals[i];
    }
    return ((void*)0);
}

// --- Background Jobs (Phase 10d) ---
#define MAX_BG_JOBS 8
static int bg_pids[MAX_BG_JOBS];
static char bg_names[MAX_BG_JOBS][64];
static int bg_count = 0;

static void bg_add(int pid, const char *name) {
    if (bg_count >= MAX_BG_JOBS) return;
    bg_pids[bg_count] = pid;
    int j = 0;
    while (name[j] && j < 63) { bg_names[bg_count][j] = name[j]; j++; }
    bg_names[bg_count][j] = '\0';
    bg_count++;
    print("[");
    char buf[12];
    int_to_string(bg_count, buf);
    print(buf);
    print("] ");
    int_to_string(pid, buf);
    print(buf);
    print("\n");
}

static void bg_check_completed(void) {
    // Check for completed background jobs by querying process state
    // Use system state to check if PIDs are still alive
    uint8_t state_buf[4096];
    long ret = syscall_get_system_state(2, state_buf, sizeof(state_buf));
    if (ret <= 0) return;

    // Parse process list to find which bg PIDs are still running
    state_process_t *procs = (state_process_t *)state_buf;
    int nprocs = ret / sizeof(state_process_t);

    for (int i = 0; i < bg_count; i++) {
        int found = 0;
        for (int j = 0; j < nprocs; j++) {
            if ((int)procs[j].pid == bg_pids[i]) {
                found = 1;
                break;
            }
        }
        if (!found) {
            // Process exited
            print("[");
            char buf[12];
            int_to_string(i + 1, buf);
            print(buf);
            print("] Done    ");
            print(bg_names[i]);
            print("\n");
            // Remove from list by shifting
            for (int k = i; k < bg_count - 1; k++) {
                bg_pids[k] = bg_pids[k + 1];
                strcpy(bg_names[k], bg_names[k + 1]);
            }
            bg_count--;
            i--; // Recheck this slot
        }
    }
}

// Expand environment variables in a command line
// Handles $KEY, $?, $$
static void expand_variables(char *input, char *output, int max_len) {
    int oi = 0;
    for (int ii = 0; input[ii] && oi < max_len - 1; ii++) {
        if (input[ii] == '$') {
            ii++;
            if (input[ii] == '?') {
                // $? = last exit status
                char num[12];
                int_to_string(last_exit_status, num);
                for (int k = 0; num[k] && oi < max_len - 1; k++)
                    output[oi++] = num[k];
            } else if (input[ii] == '$') {
                // $$ = current PID
                char num[12];
                int_to_string(syscall_getpid(), num);
                for (int k = 0; num[k] && oi < max_len - 1; k++)
                    output[oi++] = num[k];
            } else if ((input[ii] >= 'A' && input[ii] <= 'Z') ||
                       (input[ii] >= 'a' && input[ii] <= 'z') ||
                       input[ii] == '_') {
                // Extract variable name
                char varname[ENV_KEY_MAX];
                int vi = 0;
                while ((input[ii] >= 'A' && input[ii] <= 'Z') ||
                       (input[ii] >= 'a' && input[ii] <= 'z') ||
                       (input[ii] >= '0' && input[ii] <= '9') ||
                       input[ii] == '_') {
                    if (vi < ENV_KEY_MAX - 1) varname[vi++] = input[ii];
                    ii++;
                }
                varname[vi] = '\0';
                ii--; // Back up since outer loop will advance
                const char *val = env_get(varname);
                if (val) {
                    for (int k = 0; val[k] && oi < max_len - 1; k++)
                        output[oi++] = val[k];
                }
            } else {
                // Lone $ or $<digit> — output literally
                output[oi++] = '$';
                if (input[ii]) output[oi++] = input[ii];
            }
        } else {
            output[oi++] = input[ii];
        }
    }
    output[oi] = '\0';
}

// Enhanced readline with command history (up/down arrows)
void readline(char *buffer, int max_len) {
    int i = 0;
    int hist_pos = -1;  // -1 = current input, 0 = most recent, etc.
    char saved_input[256];  // save what user was typing before navigating history
    saved_input[0] = '\0';

    while (i < max_len - 1) {
        char c = syscall_getc();

        if (c == '\n') {
            break;
        } else if (c == '\b') {
            if (i > 0) {
                i--;
                print("\b \b");
            }
        } else if (c == '\033') {
            // Escape sequence — read next two chars
            char c2 = syscall_getc();
            if (c2 == '[') {
                char c3 = syscall_getc();
                if (c3 == 'A') {
                    // Up arrow — go back in history
                    if (hist_pos < history_count - 1) {
                        if (hist_pos == -1) {
                            // Save current input
                            buffer[i] = '\0';
                            int si = 0;
                            while (buffer[si]) { saved_input[si] = buffer[si]; si++; }
                            saved_input[si] = '\0';
                        }
                        hist_pos++;
                        const char *h = history_get(hist_pos);
                        if (h) {
                            // Clear current line on screen
                            while (i > 0) { print("\b \b"); i--; }
                            // Copy history entry
                            i = 0;
                            while (h[i] && i < max_len - 1) {
                                buffer[i] = h[i];
                                syscall_putc(h[i]);
                                i++;
                            }
                        }
                    }
                } else if (c3 == 'B') {
                    // Down arrow — go forward in history
                    if (hist_pos > -1) {
                        hist_pos--;
                        // Clear current line on screen
                        while (i > 0) { print("\b \b"); i--; }
                        if (hist_pos == -1) {
                            // Restore saved input
                            i = 0;
                            while (saved_input[i] && i < max_len - 1) {
                                buffer[i] = saved_input[i];
                                syscall_putc(saved_input[i]);
                                i++;
                            }
                        } else {
                            const char *h = history_get(hist_pos);
                            if (h) {
                                i = 0;
                                while (h[i] && i < max_len - 1) {
                                    buffer[i] = h[i];
                                    syscall_putc(h[i]);
                                    i++;
                                }
                            }
                        }
                    }
                }
                // Ignore Left/Right arrows for now (C/D)
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

// --- Phase 11a: SimHash Commands ---

void hex64(uint64_t val, char *buf) {
    const char hex[] = "0123456789abcdef";
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 15; i >= 0; i--) {
        buf[2 + (15 - i)] = hex[(val >> (i * 4)) & 0xF];
    }
    buf[18] = '\0';
}

void cmd_simhash(const char *path) {
    if (!path || path[0] == '\0') {
        print("simhash: usage: simhash <file>\n");
        return;
    }

    print("Computing SimHash for '");
    print(path);
    print("'...\n");

    uint64_t hash = syscall_compute_simhash(path);
    if (hash == 0) {
        print("simhash: failed (file not found or empty)\n");
        return;
    }

    char hexbuf[20];
    hex64(hash, hexbuf);
    print("  SimHash: ");
    print(hexbuf);
    print("\n");

    // Show binary representation for visual comparison
    print("  Binary:  ");
    for (int i = 63; i >= 0; i--) {
        syscall_putc((hash & (1ULL << i)) ? '1' : '0');
        if (i > 0 && i % 8 == 0) syscall_putc(' ');
    }
    print("\n");
}

void cmd_similar(const char *path, int threshold) {
    if (!path || path[0] == '\0') {
        print("similar: usage: similar <file> [threshold]\n");
        return;
    }

    if (threshold <= 0) threshold = 10; // default

    grahafs_search_results_t results;
    zero_mem(&results, sizeof(results));

    int ret = syscall_find_similar(path, threshold, &results);
    if (ret == -2) {
        print("similar: no SimHash computed for '");
        print(path);
        print("' — run 'simhash ");
        print(path);
        print("' first\n");
        return;
    }
    if (ret < 0) {
        print("similar: failed (error=");
        char buf[12];
        int_to_string(ret, buf);
        print(buf);
        print(")\n");
        return;
    }

    print("Files similar to '");
    print(path);
    print("' (threshold=");
    char buf[12];
    int_to_string(threshold, buf);
    print(buf);
    print("): ");
    uint64_to_str(results.count, buf);
    print(buf);
    print(" match(es)\n");

    for (uint32_t i = 0; i < results.count; i++) {
        print("  ");
        print(results.results[i].path);
        print("  distance=");
        uint64_to_str(results.results[i].importance, buf); // distance stored in importance field
        print(buf);
        if (results.results[i].tags[0]) {
            print("  tags=");
            print(results.results[i].tags);
        }
        print("\n");
    }
}

// --- Phase 11b: Cluster Commands ---

void cmd_clusters(void) {
    cluster_list_t list;
    zero_mem(&list, sizeof(list));

    int ret = syscall_cluster_list(&list);
    if (ret < 0) {
        print("clusters: failed to get cluster list\n");
        return;
    }

    print("Active clusters: ");
    char buf[12];
    uint64_to_str(list.count, buf);
    print(buf);
    print("\n");

    if (list.count == 0) {
        print("  (none — run 'simhash <file>' to start clustering)\n");
        return;
    }

    for (uint32_t i = 0; i < list.count; i++) {
        print("  Cluster ");
        uint64_to_str(list.clusters[i].id, buf);
        print(buf);
        print(": leader=");
        print(list.clusters[i].name);
        print("  members=");
        uint64_to_str(list.clusters[i].member_count, buf);
        print(buf);
        print("  centroid=");
        char hbuf[20];
        hex64(list.clusters[i].centroid, hbuf);
        print(hbuf);
        print("\n");
    }
}

void cmd_cluster(const char *id_str) {
    if (!id_str || id_str[0] == '\0') {
        print("cluster: usage: cluster <id>\n");
        return;
    }

    // Parse cluster ID
    uint32_t cid = 0;
    for (int i = 0; id_str[i]; i++) {
        if (id_str[i] >= '0' && id_str[i] <= '9')
            cid = cid * 10 + (id_str[i] - '0');
    }
    if (cid == 0) {
        print("cluster: invalid cluster ID\n");
        return;
    }

    cluster_members_t members;
    zero_mem(&members, sizeof(members));

    int ret = syscall_cluster_members(cid, &members);
    if (ret < 0) {
        print("cluster: cluster ");
        char buf[12];
        uint64_to_str(cid, buf);
        print(buf);
        print(" not found\n");
        return;
    }

    print("Cluster ");
    char buf[12];
    uint64_to_str(members.cluster_id, buf);
    print(buf);
    print(" (leader: ");
    print(members.leader_name);
    print(", ");
    uint64_to_str(members.count, buf);
    print(buf);
    print(" members):\n");

    for (uint32_t i = 0; i < members.count; i++) {
        print("  ");
        print(members.members[i].name);
        print("  distance=");
        int_to_string(members.members[i].distance, buf);
        print(buf);
        if (i == 0 && members.members[i].distance == 0) {
            print(" (leader)");
        }
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

// --- Phase 9e: AI Command ---

// JSON-safe string append: escapes special chars for JSON string values
static int json_append(char *buf, int pos, int max, const char *s) {
    while (*s && pos < max - 1) {
        if (*s == '"' || *s == '\\') {
            if (pos + 2 >= max) break;
            buf[pos++] = '\\';
            buf[pos++] = *s;
        } else if (*s == '\n') {
            if (pos + 2 >= max) break;
            buf[pos++] = '\\';
            buf[pos++] = 'n';
        } else if (*s == '\r') {
            if (pos + 2 >= max) break;
            buf[pos++] = '\\';
            buf[pos++] = 'r';
        } else if (*s == '\t') {
            if (pos + 2 >= max) break;
            buf[pos++] = '\\';
            buf[pos++] = 't';
        } else {
            buf[pos++] = *s;
        }
        s++;
    }
    buf[pos] = '\0';
    return pos;
}

// Raw string append (no escaping)
static int str_append(char *buf, int pos, int max, const char *s) {
    while (*s && pos < max - 1) {
        buf[pos++] = *s++;
    }
    buf[pos] = '\0';
    return pos;
}

// Append uint64 as decimal string
static int num_append(char *buf, int pos, int max, uint64_t num) {
    char tmp[21];
    uint64_to_str(num, tmp);
    return str_append(buf, pos, max, tmp);
}

// Append int as decimal string
static int int_append(char *buf, int pos, int max, int num) {
    char tmp[12];
    int_to_string(num, tmp);
    return str_append(buf, pos, max, tmp);
}

// JSMN helper: compare token string
static int jsoneq(const char *json, jsmntok_t *tok, const char *s) {
    if (tok->type == JSMN_STRING &&
        (int)strlen(s) == tok->end - tok->start &&
        strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
        return 0;
    }
    return -1;
}

static const char *proc_state_name(uint32_t s) {
    switch (s) {
        case 0: return "ZOMBIE";
        case 1: return "READY";
        case 2: return "RUNNING";
        case 3: return "BLOCKED";
        default: return "UNKNOWN";
    }
}

void cmd_ai(char *argv[], int argc) {
    if (argc < 2) {
        print("ai: usage: ai <prompt...>\n");
        print("  Interactive Q&A with Gemini AI, includes GrahaOS system context.\n");
        return;
    }

    // 1. Read API key from etc/ai.conf
    int fd = syscall_open("etc/ai.conf");
    if (fd < 0) {
        print("ai: cannot read API key (etc/ai.conf not found)\n");
        print("    Ensure api_keys.md exists and rebuild.\n");
        return;
    }
    char api_key[128];
    for (int i = 0; i < 128; i++) api_key[i] = 0;
    int key_len = syscall_read(fd, api_key, sizeof(api_key) - 1);
    syscall_close(fd);

    if (key_len <= 0) {
        print("ai: API key is empty\n");
        return;
    }
    // Trim trailing whitespace/newlines
    while (key_len > 0 && (api_key[key_len-1] == '\n' || api_key[key_len-1] == '\r' ||
                            api_key[key_len-1] == ' ')) {
        api_key[--key_len] = '\0';
    }
    if (key_len == 0) {
        print("ai: API key is empty after trimming\n");
        return;
    }

    // 2. Collect system state
    state_memory_t mem;
    state_process_list_t procs;
    state_cap_list_t caps;
    state_filesystem_t fs;
    zero_mem(&mem, sizeof(mem));
    zero_mem(&procs, sizeof(procs));
    zero_mem(&caps, sizeof(caps));
    zero_mem(&fs, sizeof(fs));
    syscall_get_system_state(STATE_CAT_MEMORY, &mem, sizeof(mem));
    syscall_get_system_state(STATE_CAT_PROCESSES, &procs, sizeof(procs));
    syscall_get_system_state(STATE_CAT_CAPABILITIES, &caps, sizeof(caps));
    syscall_get_system_state(STATE_CAT_FILESYSTEM, &fs, sizeof(fs));

    // 3. Build system context prompt
    // We'll build the prompt in a buffer, then JSON-encode it into the POST body
    char prompt[3072];
    int p = 0;

    p = str_append(prompt, p, sizeof(prompt),
        "You are GrahaOS AI, running inside a custom bare-metal x86_64 operating system. "
        "Answer concisely based on the system state below.\\n\\n");

    // Memory
    p = str_append(prompt, p, sizeof(prompt), "MEMORY: total=");
    p = num_append(prompt, p, sizeof(prompt), mem.total_physical);
    p = str_append(prompt, p, sizeof(prompt), " free=");
    p = num_append(prompt, p, sizeof(prompt), mem.free_physical);
    p = str_append(prompt, p, sizeof(prompt), " used=");
    p = num_append(prompt, p, sizeof(prompt), mem.used_physical);
    p = str_append(prompt, p, sizeof(prompt), "\\n");

    // Processes
    p = str_append(prompt, p, sizeof(prompt), "PROCESSES:");
    for (uint32_t i = 0; i < procs.count && i < 16; i++) {
        if (procs.procs[i].pid <= 0) continue;
        p = str_append(prompt, p, sizeof(prompt), " pid=");
        p = int_append(prompt, p, sizeof(prompt), procs.procs[i].pid);
        p = str_append(prompt, p, sizeof(prompt), " name=");
        p = str_append(prompt, p, sizeof(prompt), procs.procs[i].name);
        p = str_append(prompt, p, sizeof(prompt), " state=");
        p = str_append(prompt, p, sizeof(prompt), proc_state_name(procs.procs[i].state));
        p = str_append(prompt, p, sizeof(prompt), ",");
    }
    p = str_append(prompt, p, sizeof(prompt), "\\n");

    // Capabilities with operations manifest
    p = str_append(prompt, p, sizeof(prompt), "CAPABILITIES (");
    p = num_append(prompt, p, sizeof(prompt), caps.count);
    p = str_append(prompt, p, sizeof(prompt), "):\\n");
    for (uint32_t i = 0; i < caps.count; i++) {
        state_cap_entry_t *c = &caps.caps[i];
        if (c->deleted) continue;
        p = str_append(prompt, p, sizeof(prompt), "  ");
        p = str_append(prompt, p, sizeof(prompt), c->name);
        p = str_append(prompt, p, sizeof(prompt), " [");
        p = str_append(prompt, p, sizeof(prompt), c->state == 2 ? "ON" : "OFF");
        p = str_append(prompt, p, sizeof(prompt), "] (");
        switch (c->type) {
            case 0: p = str_append(prompt, p, sizeof(prompt), "HARDWARE"); break;
            case 1: p = str_append(prompt, p, sizeof(prompt), "DRIVER"); break;
            case 2: p = str_append(prompt, p, sizeof(prompt), "SERVICE"); break;
            case 3: p = str_append(prompt, p, sizeof(prompt), "APPLICATION"); break;
            case 4: p = str_append(prompt, p, sizeof(prompt), "FEATURE"); break;
            case 5: p = str_append(prompt, p, sizeof(prompt), "COMPOSITE"); break;
            default: p = str_append(prompt, p, sizeof(prompt), "UNKNOWN"); break;
        }
        p = str_append(prompt, p, sizeof(prompt), ")\\n");
    }

    // Filesystem
    p = str_append(prompt, p, sizeof(prompt), "FILESYSTEM: mounted=");
    p = num_append(prompt, p, sizeof(prompt), fs.grahafs_mounted);
    p = str_append(prompt, p, sizeof(prompt), " free_blocks=");
    p = num_append(prompt, p, sizeof(prompt), fs.grahafs_free_blocks);
    p = str_append(prompt, p, sizeof(prompt), " free_inodes=");
    p = num_append(prompt, p, sizeof(prompt), fs.grahafs_free_inodes);
    p = str_append(prompt, p, sizeof(prompt), "\\n\\n");

    // User prompt
    p = str_append(prompt, p, sizeof(prompt), "User: ");
    for (int i = 1; i < argc; i++) {
        p = json_append(prompt, p, sizeof(prompt), argv[i]);
        if (i < argc - 1) p = str_append(prompt, p, sizeof(prompt), " ");
    }

    // 4. Build Gemini JSON request body
    char body[4096];
    int b = 0;
    b = str_append(body, b, sizeof(body), "{\"contents\":[{\"parts\":[{\"text\":\"");
    b = str_append(body, b, sizeof(body), prompt);
    b = str_append(body, b, sizeof(body), "\"}]}]}");

    // 5. Build URL with API key
    char url[512];
    int u = 0;
    u = str_append(url, u, sizeof(url),
        "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent?key=");
    u = str_append(url, u, sizeof(url), api_key);

    print("Thinking...\n");

    // 6. Call HTTP POST
    char response[8192];
    for (int i = 0; i < (int)sizeof(response); i++) response[i] = 0;

    int ret = syscall_http_post(url, body, b, response, sizeof(response));

    // Handle 429 rate limit: check response for "429" and retry once
    if (ret > 0) {
        // Quick check for rate limiting in response
        int is_429 = 0;
        for (int i = 0; i < ret - 2; i++) {
            if (response[i] == '4' && response[i+1] == '2' && response[i+2] == '9') {
                is_429 = 1;
                break;
            }
        }
        if (is_429) {
            print("Rate limited, retrying in 2s...\n");
            // Simple delay: busy-wait loop (~2 seconds)
            for (volatile int d = 0; d < 200000000; d++) {}
            for (int i = 0; i < (int)sizeof(response); i++) response[i] = 0;
            ret = syscall_http_post(url, body, b, response, sizeof(response));
        }
    }

    if (ret < 0) {
        print("ai: HTTP POST failed (error ");
        char num[12];
        int_to_string(ret, num);
        print(num);
        if (ret == -10) print(" timeout");
        else if (ret == -11) print(" DNS failed");
        else if (ret == -12) print(" connection failed");
        else if (ret == -17) print(" no network");
        print(")\n");
        return;
    }

    if (ret == 0) {
        print("ai: empty response\n");
        return;
    }

    // 7. Parse JSON response with JSMN
    jsmn_parser parser;
    jsmntok_t tokens[256];
    jsmn_init(&parser);
    int tok_count = jsmn_parse(&parser, response, ret, tokens, 256);

    if (tok_count < 0) {
        print("ai: JSON parse error (");
        char num[12];
        int_to_string(tok_count, num);
        print(num);
        print(")\n");
        // Show raw response for debugging
        print("Raw response (first 512 bytes):\n");
        for (int i = 0; i < ret && i < 512; i++) {
            syscall_putc(response[i]);
        }
        print("\n");
        return;
    }

    // Find candidates[0].content.parts[0].text
    // Navigate: root object -> "candidates" -> array[0] -> object -> "content" -> object -> "parts" -> array[0] -> object -> "text" -> string
    int text_found = 0;
    for (int i = 0; i < tok_count - 1; i++) {
        if (jsoneq(response, &tokens[i], "text") == 0) {
            jsmntok_t *val = &tokens[i + 1];
            if (val->type == JSMN_STRING) {
                // Print the AI response, handling escape sequences
                for (int j = val->start; j < val->end; j++) {
                    if (response[j] == '\\' && j + 1 < val->end) {
                        char next = response[j + 1];
                        if (next == 'n') { syscall_putc('\n'); j++; }
                        else if (next == 't') { syscall_putc('\t'); j++; }
                        else if (next == '\\') { syscall_putc('\\'); j++; }
                        else if (next == '"') { syscall_putc('"'); j++; }
                        else { syscall_putc(response[j]); }
                    } else {
                        syscall_putc(response[j]);
                    }
                }
                syscall_putc('\n');
                text_found = 1;
                break;
            }
        }
    }

    // Check for error response
    if (!text_found) {
        // Look for "error" -> "message" pattern
        for (int i = 0; i < tok_count - 1; i++) {
            if (jsoneq(response, &tokens[i], "message") == 0) {
                jsmntok_t *val = &tokens[i + 1];
                if (val->type == JSMN_STRING) {
                    print("ai: API error: ");
                    for (int j = val->start; j < val->end; j++) {
                        syscall_putc(response[j]);
                    }
                    print("\n");
                    text_found = 1;
                    break;
                }
            }
        }
    }

    if (!text_found) {
        print("ai: could not extract response text\n");
        print("Raw (first 512 bytes):\n");
        for (int i = 0; i < ret && i < 512; i++) {
            syscall_putc(response[i]);
        }
        print("\n");
    }
}

// Helper: spawn and wait for a program
int run_program(const char *path) {
    int pid = syscall_spawn(path);
    if (pid < 0) return -1;

    int exit_status;
    syscall_wait(&exit_status);
    return exit_status;
}

// --- Phase 10c: Pipe & Redirect Infrastructure ---

// Spawn a program by name (try bin/ prefix if not absolute path)
// Returns: child PID on success, -1 on failure (does NOT wait)
static int spawn_program(const char *name) {
    char path[128];
    if (name[0] == '/') {
        strcpy(path, name);
    } else {
        strcpy(path, "bin/");
        int len = strlen(path);
        int nlen = strlen(name);
        if (len + nlen < 127) {
            strcpy(path + len, name);
        }
    }
    return syscall_spawn(path);
}

// Execute a built-in command.
// Returns: 1 if recognized and executed, 0 if not a built-in, -1 for 'exit'
static int execute_builtin(char *argv[], int argc);

// Process redirect operators (>, >>, <) in argv.
// Removes redirect tokens and filenames from argv.
// Sets up FD redirections via dup/dup2.
// saved_stdout/saved_stdin: set to saved FD numbers (-1 if not redirected)
// out_file_fd/in_file_fd: set to opened file FD numbers (-1 if none)
// Returns: new argc, or -1 on error
static int setup_redirects(char *argv[], int argc,
                            int *saved_stdout, int *saved_stdin,
                            int *out_file_fd, int *in_file_fd) {
    *saved_stdout = -1;
    *saved_stdin = -1;
    *out_file_fd = -1;
    *in_file_fd = -1;

    char *new_argv[32];
    int new_argc = 0;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], ">") == 0 && i + 1 < argc) {
            // Output redirect (overwrite — truncate first)
            const char *filename = argv[i + 1];
            syscall_create(filename, 0644);
            int fd = syscall_open(filename);
            if (fd < 0) {
                print("gash: cannot open '");
                print(filename);
                print("' for writing\n");
                return -1;
            }
            syscall_truncate(fd);  // Truncate existing content
            *saved_stdout = syscall_dup(1);
            syscall_dup2(fd, 1);
            *out_file_fd = fd;
            i++; // Skip filename
        }
        else if (strcmp(argv[i], ">>") == 0 && i + 1 < argc) {
            // Output redirect (append)
            const char *filename = argv[i + 1];
            syscall_create(filename, 0644);
            int fd = syscall_open(filename);
            if (fd < 0) {
                print("gash: cannot open '");
                print(filename);
                print("' for appending\n");
                return -1;
            }
            // Advance file position to EOF by reading through
            char discard[128];
            while (syscall_read(fd, discard, sizeof(discard)) > 0) {}
            *saved_stdout = syscall_dup(1);
            syscall_dup2(fd, 1);
            *out_file_fd = fd;
            i++; // Skip filename
        }
        else if (strcmp(argv[i], "<") == 0 && i + 1 < argc) {
            // Input redirect
            const char *filename = argv[i + 1];
            int fd = syscall_open(filename);
            if (fd < 0) {
                print("gash: cannot open '");
                print(filename);
                print("' for reading\n");
                return -1;
            }
            *saved_stdin = syscall_dup(0);
            syscall_dup2(fd, 0);
            *in_file_fd = fd;
            i++; // Skip filename
        }
        else {
            if (new_argc < 31) {
                new_argv[new_argc++] = argv[i];
            }
        }
    }

    // Copy filtered argv back
    for (int i = 0; i < new_argc; i++) {
        argv[i] = new_argv[i];
    }
    argv[new_argc] = ((void*)0);

    return new_argc;
}

// Restore FDs after redirect
static void cleanup_redirects(int saved_stdout, int saved_stdin,
                               int out_file_fd, int in_file_fd) {
    if (saved_stdout >= 0) {
        syscall_dup2(saved_stdout, 1);
        syscall_close(saved_stdout);
    }
    if (saved_stdin >= 0) {
        syscall_dup2(saved_stdin, 0);
        syscall_close(saved_stdin);
    }
    if (out_file_fd >= 0) {
        syscall_close(out_file_fd);
    }
    if (in_file_fd >= 0) {
        syscall_close(in_file_fd);
    }
}

// Execute a pipeline: left_cmd | right_cmd
static void execute_pipeline(char *left_str, char *right_str) {
    // Trim leading whitespace
    while (*left_str == ' ' || *left_str == '\t') left_str++;
    while (*right_str == ' ' || *right_str == '\t') right_str++;

    char *left_argv[32], *right_argv[32];
    int left_argc = parse_command(left_str, left_argv, 32);
    int right_argc = parse_command(right_str, right_argv, 32);

    if (left_argc == 0 || right_argc == 0) {
        print("gash: syntax error near '|'\n");
        return;
    }

    // Create pipe
    int pipe_fds[2];
    if (syscall_pipe(pipe_fds) < 0) {
        print("gash: pipe creation failed\n");
        return;
    }
    // pipe_fds[0] = read end, pipe_fds[1] = write end

    int left_pid = -1, right_pid = -1;

    // Step 1: Spawn right side (reader) with stdin = pipe_read
    // Do this first so it's ready to consume data
    {
        int saved = syscall_dup(0);
        syscall_dup2(pipe_fds[0], 0);  // FD 0 = pipe_read
        right_pid = spawn_program(right_argv[0]);
        syscall_dup2(saved, 0);        // Restore FD 0
        syscall_close(saved);
    }

    if (right_pid < 0) {
        print("gash: failed to spawn '");
        print(right_argv[0]);
        print("' for pipe\n");
        syscall_close(pipe_fds[0]);
        syscall_close(pipe_fds[1]);
        return;
    }

    // Step 2: Execute left side (writer) with stdout = pipe_write
    {
        int saved = syscall_dup(1);
        syscall_dup2(pipe_fds[1], 1);  // FD 1 = pipe_write

        // Check if left side is a built-in
        int builtin_result = execute_builtin(left_argv, left_argc);
        if (builtin_result == 0) {
            // Not a built-in — spawn as external
            left_pid = spawn_program(left_argv[0]);
            if (left_pid < 0) {
                print("gash: unknown command: '");
                print(left_argv[0]);
                print("'\n");
            }
        }

        syscall_dup2(saved, 1);        // Restore FD 1
        syscall_close(saved);
    }

    // Step 3: Close pipe ends in shell
    // This signals EOF to the reader when all writers are done
    syscall_close(pipe_fds[0]);
    syscall_close(pipe_fds[1]);

    // Step 4: Wait for spawned processes
    if (left_pid > 0) {
        int status;
        syscall_wait(&status);
    }
    if (right_pid > 0) {
        int status;
        syscall_wait(&status);
    }
}

// Process a complete command line (handles pipes, redirects, background, vars)
static void process_cmdline(char *line) {
    // Skip empty lines
    if (line[0] == '\0') return;

    // Step 1: Expand environment variables
    char expanded[512];
    expand_variables(line, expanded, sizeof(expanded));

    // Step 2: Check for background operator (&)
    int background = 0;
    {
        int len = strlen(expanded);
        // Trim trailing whitespace
        while (len > 0 && (expanded[len - 1] == ' ' || expanded[len - 1] == '\t'))
            len--;
        if (len > 0 && expanded[len - 1] == '&') {
            background = 1;
            expanded[len - 1] = '\0';
            // Trim again
            len--;
            while (len > 0 && (expanded[len - 1] == ' ' || expanded[len - 1] == '\t'))
                expanded[--len] = '\0';
        }
    }

    // Step 3: Check for pipe operator (find first unquoted '|')
    char *pipe_pos = ((void*)0);
    for (char *p = expanded; *p; p++) {
        if (*p == '|') {
            pipe_pos = p;
            break;
        }
    }

    if (pipe_pos) {
        // Split at pipe and execute as pipeline
        *pipe_pos = '\0';
        execute_pipeline(expanded, pipe_pos + 1);
        return;
    }

    // No pipe — parse command and handle redirects
    char *argv[32];
    int argc = parse_command(expanded, argv, 32);
    if (argc == 0) return;

    // Process redirects
    int saved_stdout = -1, saved_stdin = -1;
    int out_file_fd = -1, in_file_fd = -1;
    argc = setup_redirects(argv, argc, &saved_stdout, &saved_stdin,
                           &out_file_fd, &in_file_fd);
    if (argc < 0) return; // Redirect error

    if (argc == 0) {
        cleanup_redirects(saved_stdout, saved_stdin, out_file_fd, in_file_fd);
        return;
    }

    // Execute command
    int result = execute_builtin(argv, argc);
    if (result == 0) {
        // Not a built-in — try to spawn as external program
        int pid = spawn_program(argv[0]);
        if (pid < 0) {
            print("Unknown command: '");
            print(argv[0]);
            print("'\n");
            print("Type 'help' for available commands.\n");
        } else if (background) {
            // Background job — don't wait
            bg_add(pid, argv[0]);
        } else {
            int exit_status;
            syscall_wait(&exit_status);
            last_exit_status = exit_status;
        }
    }

    // Restore redirects
    cleanup_redirects(saved_stdout, saved_stdin, out_file_fd, in_file_fd);

    if (result == -1) {
        // 'exit' command
        syscall_exit(0);
    }
}

// Execute a built-in command.
// Returns: 1 if recognized and executed, 0 if not a built-in, -1 for 'exit'
static int execute_builtin(char *argv[], int argc) {
    char *cmd = argv[0];

    if (strcmp(cmd, "help") == 0) {
        print("Available commands:\n");
        print("  help                - Show this message\n");
        print("  ls [path]           - List directory contents\n");
        print("  cat <file>          - Display file contents\n");
        print("  touch <file>        - Create empty file\n");
        print("  mkdir <dir>         - Create directory\n");
        print("  echo <text>         - Print text\n");
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
        print("  simhash <file>      - Compute & show SimHash fingerprint\n");
        print("  similar <file> [n]  - Find similar files (threshold n)\n");
        print("  clusters            - List all file clusters\n");
        print("  cluster <id>        - Show members of a cluster\n");
        print("  watch <cap>         - Watch capability state changes\n");
        print("  unwatch <cap>       - Stop watching a capability\n");
        print("  events              - Show pending CAN events\n");
        print("  ifconfig            - Show network interface info\n");
        print("  netstat             - Show TCP/IP stack status\n");
        print("  http <url>          - Fetch URL via HTTP/HTTPS GET\n");
        print("  dns <hostname>      - Resolve hostname to IP address\n");
        print("  ai <prompt...>      - Ask AI (Gemini) with system context\n");
        print("  agent <prompt...>   - AI agent: plan + validate + execute\n");
        print("  pid                 - Show current process ID\n");
        print("  kill <pid>          - Terminate a process\n");
        print("  sync                - Flush filesystem to disk\n");
        print("  export KEY=VALUE    - Set environment variable\n");
        print("  env                 - List environment variables\n");
        print("  history             - Show command history\n");
        print("  jobs                - List background jobs\n");
        print("  test                - Keyboard test\n");
        print("  grahai              - Run GCP interpreter\n");
        print("  exit                - Exit the shell\n");
        print("\nPipe & Redirect:\n");
        print("  cmd > file          - Redirect output to file\n");
        print("  cmd >> file         - Append output to file\n");
        print("  cmd < file          - Redirect input from file\n");
        print("  cmd1 | cmd2         - Pipe output of cmd1 to cmd2\n");
        print("  cmd &               - Run in background\n");
        print("  $VAR                - Variable expansion\n");
        print("  Up/Down arrows      - Command history navigation\n");
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
        print("  aitest              - AI integration test suite\n");
        print("  fdtest              - FD table test suite\n");
        print("  pipetest            - Pipe & dup test suite\n");
        return 1;
    }
    else if (strcmp(cmd, "ls") == 0) {
        cmd_ls(argc > 1 ? argv[1] : "/");
        return 1;
    }
    else if (strcmp(cmd, "cat") == 0) {
        if (argc < 2) {
            print("cat: missing operand\n");
        } else {
            cmd_cat(argv[1]);
        }
        return 1;
    }
    else if (strcmp(cmd, "touch") == 0) {
        if (argc < 2) {
            print("touch: missing operand\n");
        } else {
            cmd_touch(argv[1]);
        }
        return 1;
    }
    else if (strcmp(cmd, "mkdir") == 0) {
        if (argc < 2) {
            print("mkdir: missing operand\n");
        } else {
            cmd_mkdir(argv[1]);
        }
        return 1;
    }
    else if (strcmp(cmd, "echo") == 0) {
        cmd_echo(argv, argc);
        return 1;
    }
    else if (strcmp(cmd, "sync") == 0) {
        print("Syncing filesystem to disk...\n");
        syscall_sync();
        print("Sync complete.\n");
        return 1;
    }
    else if (strcmp(cmd, "memstate") == 0) {
        cmd_memstate();
        return 1;
    }
    else if (strcmp(cmd, "ps") == 0) {
        cmd_ps();
        return 1;
    }
    else if (strcmp(cmd, "drivers") == 0) {
        cmd_drivers();
        return 1;
    }
    else if (strcmp(cmd, "sysstate") == 0) {
        cmd_sysstate();
        return 1;
    }
    else if (strcmp(cmd, "caps") == 0) {
        cmd_caps();
        return 1;
    }
    else if (strcmp(cmd, "activate") == 0) {
        cmd_activate(argc > 1 ? argv[1] : "");
        return 1;
    }
    else if (strcmp(cmd, "deactivate") == 0) {
        cmd_deactivate(argc > 1 ? argv[1] : "");
        return 1;
    }
    else if (strcmp(cmd, "why_not") == 0) {
        cmd_why_not(argc > 1 ? argv[1] : "");
        return 1;
    }
    else if (strcmp(cmd, "available") == 0) {
        cmd_available();
        return 1;
    }
    else if (strcmp(cmd, "tag") == 0) {
        if (argc < 3) {
            print("tag: usage: tag <path> <tags>\n");
        } else {
            cmd_tag(argv[1], argv[2]);
        }
        return 1;
    }
    else if (strcmp(cmd, "meta") == 0) {
        if (argc < 2) {
            print("meta: usage: meta <path>\n");
        } else {
            cmd_meta(argv[1]);
        }
        return 1;
    }
    else if (strcmp(cmd, "importance") == 0) {
        if (argc < 3) {
            print("importance: usage: importance <path> <0-100>\n");
        } else {
            cmd_importance(argv[1], argv[2]);
        }
        return 1;
    }
    else if (strcmp(cmd, "summary") == 0) {
        if (argc < 3) {
            print("summary: usage: summary <path> <text...>\n");
        } else {
            cmd_summary(argv[1], argv, argc, 2);
        }
        return 1;
    }
    else if (strcmp(cmd, "search") == 0) {
        if (argc < 2) {
            print("search: usage: search <tag>\n");
        } else {
            cmd_search(argv[1]);
        }
        return 1;
    }
    else if (strcmp(cmd, "simhash") == 0) {
        if (argc < 2) {
            print("simhash: usage: simhash <file>\n");
        } else {
            cmd_simhash(argv[1]);
        }
        return 1;
    }
    else if (strcmp(cmd, "similar") == 0) {
        if (argc < 2) {
            print("similar: usage: similar <file> [threshold]\n");
        } else {
            int thr = 10;
            if (argc >= 3) {
                thr = 0;
                for (int si = 0; argv[2][si]; si++) {
                    if (argv[2][si] >= '0' && argv[2][si] <= '9')
                        thr = thr * 10 + (argv[2][si] - '0');
                }
                if (thr == 0) thr = 10;
            }
            cmd_similar(argv[1], thr);
        }
        return 1;
    }
    else if (strcmp(cmd, "clusters") == 0) {
        cmd_clusters();
        return 1;
    }
    else if (strcmp(cmd, "cluster") == 0) {
        if (argc < 2) {
            print("cluster: usage: cluster <id>\n");
        } else {
            cmd_cluster(argv[1]);
        }
        return 1;
    }
    else if (strcmp(cmd, "watch") == 0) {
        cmd_watch(argc > 1 ? argv[1] : "");
        return 1;
    }
    else if (strcmp(cmd, "unwatch") == 0) {
        cmd_unwatch(argc > 1 ? argv[1] : "");
        return 1;
    }
    else if (strcmp(cmd, "events") == 0) {
        cmd_events();
        return 1;
    }
    else if (strcmp(cmd, "ifconfig") == 0) {
        cmd_ifconfig();
        return 1;
    }
    else if (strcmp(cmd, "netstat") == 0) {
        cmd_netstat();
        return 1;
    }
    else if (strcmp(cmd, "http") == 0) {
        cmd_http(argc > 1 ? argv[1] : "");
        return 1;
    }
    else if (strcmp(cmd, "dns") == 0) {
        cmd_dns(argc > 1 ? argv[1] : "");
        return 1;
    }
    else if (strcmp(cmd, "ai") == 0) {
        cmd_ai(argv, argc);
        return 1;
    }
    else if (strcmp(cmd, "agent") == 0) {
        if (argc < 2) {
            print("agent: usage: agent <prompt...>\n");
            print("  AI agent: generates plan, validates against capabilities, executes.\n");
        } else {
            char prompt_buf[512];
            int pos = 0;
            for (int i = 1; i < argc && pos < 510; i++) {
                int slen = strlen(argv[i]);
                if (pos + slen + 1 >= 510) slen = 510 - pos - 1;
                for (int j = 0; j < slen && pos < 510; j++)
                    prompt_buf[pos++] = argv[i][j];
                if (i < argc - 1 && pos < 510) prompt_buf[pos++] = ' ';
            }
            prompt_buf[pos] = '\0';

            syscall_create("ai_prompt.txt", 0);
            int pfd = syscall_open("ai_prompt.txt");
            if (pfd < 0) {
                print("agent: failed to create prompt file\n");
            } else {
                syscall_write(pfd, prompt_buf, pos);
                syscall_close(pfd);

                print("Spawning AI agent...\n");
                int pid = syscall_spawn("bin/grahai");
                if (pid < 0) {
                    print("agent: failed to spawn grahai\n");
                } else {
                    int exit_status;
                    syscall_wait(&exit_status);
                    print("Agent completed (status=");
                    char spid[12];
                    int_to_string(exit_status, spid);
                    print(spid);
                    print(")\n");
                }
            }
        }
        return 1;
    }
    else if (strcmp(cmd, "pid") == 0) {
        int current_pid = syscall_getpid();
        print("PID: ");
        char buf[12];
        int_to_string(current_pid, buf);
        print(buf);
        print("\n");
        return 1;
    }
    else if (strcmp(cmd, "kill") == 0) {
        if (argc < 2) {
            print("kill: usage: kill <pid>\n");
        } else {
            int target_pid = 0;
            for (int i = 0; argv[1][i]; i++) {
                if (argv[1][i] >= '0' && argv[1][i] <= '9') {
                    target_pid = target_pid * 10 + (argv[1][i] - '0');
                }
            }
            int result = syscall_kill(target_pid, 1);
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
        return 1;
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
        return 1;
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
        return 1;
    }
    else if (strcmp(cmd, "export") == 0) {
        if (argc < 2) {
            print("export: usage: export KEY=VALUE\n");
        } else {
            // Find '=' in argv[1]
            char *eq = ((void*)0);
            for (char *p = argv[1]; *p; p++) {
                if (*p == '=') { eq = p; break; }
            }
            if (eq) {
                *eq = '\0';
                env_set(argv[1], eq + 1);
            } else {
                print("export: missing '=' in '");
                print(argv[1]);
                print("'\n");
            }
        }
        return 1;
    }
    else if (strcmp(cmd, "env") == 0) {
        if (env_count == 0) {
            print("(no environment variables set)\n");
        } else {
            for (int i = 0; i < env_count; i++) {
                print(env_keys[i]);
                print("=");
                print(env_vals[i]);
                print("\n");
            }
        }
        return 1;
    }
    else if (strcmp(cmd, "history") == 0) {
        if (history_count == 0) {
            print("(no history)\n");
        } else {
            for (int i = history_count - 1; i >= 0; i--) {
                const char *h = history_get(i);
                if (h) {
                    char buf[12];
                    int_to_string(history_count - i, buf);
                    print("  ");
                    print(buf);
                    print("  ");
                    print(h);
                    print("\n");
                }
            }
        }
        return 1;
    }
    else if (strcmp(cmd, "jobs") == 0) {
        if (bg_count == 0) {
            print("(no background jobs)\n");
        } else {
            for (int i = 0; i < bg_count; i++) {
                print("[");
                char buf[12];
                int_to_string(i + 1, buf);
                print(buf);
                print("] Running    ");
                print(bg_names[i]);
                print(" (PID ");
                int_to_string(bg_pids[i], buf);
                print(buf);
                print(")\n");
            }
        }
        return 1;
    }
    else if (strcmp(cmd, "exit") == 0) {
        print("Goodbye!\n");
        return -1;
    }

    return 0; // Not a built-in
}

// Main shell
void _start(void) {
    print("=== GrahaOS Shell v4.0 ===\n");
    print("Type 'help' for commands.\n\n");

    // Display our PID
    int my_pid = syscall_getpid();
    print("Shell PID: ");
    char pid_str[12];
    int_to_string(my_pid, pid_str);
    print(pid_str);
    print("\n\n");

    char command_buffer[256];

    while (1) {
        // Check for completed background jobs
        if (bg_count > 0) {
            bg_check_completed();
        }

        print("gash> ");
        readline(command_buffer, sizeof(command_buffer));

        // Add to history before processing
        history_add(command_buffer);

        process_cmdline(command_buffer);
    }
}