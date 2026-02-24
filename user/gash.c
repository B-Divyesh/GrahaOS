// user/gash.c - COMPLETE FILE
#include "syscalls.h"
#include "../kernel/state.h"

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

    print("\n=== End System State ===\n");
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
            print("  help            - Show this message\n");
            print("  ls [path]       - List directory contents\n");
            print("  cat <file>      - Display file contents\n");
            print("  touch <file>    - Create empty file\n");
            print("  mkdir <dir>     - Create directory\n");
            print("  echo <text>     - Print text\n");
            print("  echo <text> > <file> - Write text to file\n");
            print("  memstate        - Show memory & filesystem state\n");
            print("  ps              - List running processes\n");
            print("  drivers         - List registered drivers\n");
            print("  sysstate        - Full system state dump\n");
            print("  pid             - Show current process ID\n");
            print("  kill <pid>      - Terminate a process\n");
            print("  sync            - Flush filesystem to disk\n");
            print("  test            - Keyboard test\n");
            print("  grahai          - Run GCP interpreter\n");
            print("  exit            - Exit the shell\n");
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