// user/gash.c - COMPLETE FILE
#include "syscalls.h"

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
    
    int fd = syscall_open(path);
    if (fd < 0) {
        print("ls: cannot access '");
        print(path);
        print("': No such file or directory\n");
        return;
    }
    
    // Read directory entries (simplified - assumes directory structure)
    char buffer[4096];
    ssize_t bytes = syscall_read(fd, buffer, sizeof(buffer));
    syscall_close(fd);
    
    if (bytes > 0) {
        // Parse directory entries (simplified)
        print("Directory listing of ");
        print(path);
        print(":\n");
        
        // For now, just print raw data
        // In a real implementation, we'd parse the directory structure
        print(".\n");
        print("..\n");
        // Additional entries would be parsed here
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
    print("=== Memory State ===\n");
    print("Physical Memory:\n");
    print("  Total: [Implemented in kernel]\n");
    print("  Used:  [Implemented in kernel]\n");
    print("  Free:  [Implemented in kernel]\n");
    print("\nVirtual Memory:\n");
    print("  Kernel Space: 0xFFFF800000000000 - 0xFFFFFFFFFFFFFFFF\n");
    print("  User Space:   0x0000000000000000 - 0x00007FFFFFFFFFFF\n");
    print("\nFilesystem:\n");
    print("  Mounted: GrahaFS on /\n");
    print("  Block Size: 4096 bytes\n");
    print("===================\n");
}

// Main shell
void _start(void) {
    print("=== GrahaOS Shell v1.0 (Full Filesystem) ===\n");
    print("Type 'help' for commands.\n\n");

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
            print("  memstate        - Show memory information\n");
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
            print("Launching grahai...\n");
            int pid = syscall_exec("bin/grahai");
            if (pid < 0) {
                print("ERROR: Failed to execute 'bin/grahai'\n");
            } else {
                print("grahai launched (pid=");
                char pid_str[12];
                int_to_string(pid, pid_str);
                print(pid_str);
                print(")\n");
                
                int exit_status;
                syscall_wait(&exit_status);
                print("grahai completed\n");
            }
        }
        else if (strcmp(cmd, "exit") == 0) {
            print("Goodbye!\n");
            syscall_exit(0);
        }
        else {
            print("Unknown command: '");
            print(cmd);
            print("'\n");
        }
    }
}