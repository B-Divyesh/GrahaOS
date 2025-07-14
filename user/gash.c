// user/gash.c - The GrahaOS Shell
#include "syscalls.h"

// --- Helper Functions ---
int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

void print(const char *str) {
    while (*str) {
        syscall_putc(*str++);
    }
}

// Simple integer to string conversion
void int_to_string(int num, char *str) {
    if (num == 0) {
        str[0] = '0';
        str[1] = '\0';
        return;
    }
    
    int i = 0;
    int is_negative = 0;
    
    if (num < 0) {
        is_negative = 1;
        num = -num;
    }
    
    // Generate digits in reverse order
    char temp[12];
    while (num > 0) {
        temp[i++] = '0' + (num % 10);
        num /= 10;
    }
    
    // Add negative sign if needed
    int j = 0;
    if (is_negative) {
        str[j++] = '-';
    }
    
    // Copy digits in correct order
    while (i > 0) {
        str[j++] = temp[--i];
    }
    str[j] = '\0';
}

// Reads a line of input from the user
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

// --- Main Program Logic ---
void _start(void) {
    print("=== GrahaOS Shell v0.2 ===\n");
    print("Type 'help' for commands.\n\n");

    char command_buffer[128];

    while (1) {
        print("> ");
        readline(command_buffer, sizeof(command_buffer));

        if (strcmp(command_buffer, "help") == 0) {
            print("Available commands:\n");
            print("  help   - Show this message\n");
            print("  test   - Keyboard test\n");
            print("  grahai - Run GCP interpreter\n");
            print("  motd   - Show message of the day\n");
            print("  exit   - Exit the shell\n");
        } 
        else if (strcmp(command_buffer, "test") == 0) {
            print("Keyboard test - type 'q' to quit\n");
            char ch;
            while ((ch = syscall_getc()) != 'q') {
                print("You typed: ");
                syscall_putc(ch);
                print("\n");
            }
            print("Test complete.\n");
        } 
        else if (strcmp(command_buffer, "grahai") == 0) {
            print("Launching grahai...\n");
            int pid = syscall_exec("bin/grahai");
            if (pid < 0) {
                print("ERROR: Failed to execute 'bin/grahai'\n");
                if (pid == -1) print("  File not found in initrd\n");
                else if (pid == -2) print("  ELF loading failed\n");
                else if (pid == -3) print("  Process creation failed\n");
            } else {
                print("Successfully launched grahai (pid=");
                
                // Print PID
                char pid_str[12];
                int_to_string(pid, pid_str);
                print(pid_str);
                print(")\n");
                
                // Wait for the child process to complete
                print("Waiting for grahai to complete...\n");
                
                int exit_status;
                int wait_result = syscall_wait(&exit_status);
                
                // Debug: print the raw wait result
                print("DEBUG: wait() returned ");
                char wait_str[12];
                int_to_string(wait_result, wait_str);
                print(wait_str);
                print("\n");
                
                if (wait_result == pid) {
                    print("grahai completed with exit status: ");
                    char status_str[12];
                    int_to_string(exit_status, status_str);
                    print(status_str);
                    print("\n");
                } else if (wait_result < 0) {
                    print("ERROR: wait() failed with error code ");
                    print(wait_str);
                    print("\n");
                } else {
                    print("Unexpected: wait() returned pid ");
                    print(wait_str);
                    print(" but we expected pid ");
                    char pid_str2[12];
                    int_to_string(pid, pid_str2);
                    print(pid_str2);
                    print("\n");
                }
            }
        }
        else if (strcmp(command_buffer, "motd") == 0) {
            char motd_content[256] = {0};
            int fd = syscall_open("etc/motd.txt");
            if (fd < 0) {
                print("Could not open /etc/motd.txt\n");
            } else {
                syscall_read(fd, motd_content, 255);
                syscall_close(fd);
                print("--- MOTD ---\n");
                print(motd_content);
                print("\n------------\n");
            }
        }
        else if (strcmp(command_buffer, "exit") == 0) {
            print("Goodbye!\n");
            syscall_exit(0);
        }
        else if (command_buffer[0] == '\0') {
            // Empty command, do nothing
        } 
        else {
            print("Unknown command: '");
            print(command_buffer);
            print("'\n");
        }
    }
}