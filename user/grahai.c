// user/grahai.c - Phase 6c GCP Interpreter (Modified)
#include "syscalls.h"
#include "json.h"

// --- String and Print Helpers ---
void print(const char *str) {
    while (*str) {
        syscall_putc(*str++);
    }
}

int strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

size_t strlen(const char *s) {
    size_t i = 0;
    while (s[i]) i++;
    return i;
}

static long long string_to_long(const char* s) {
    long long res = 0;
    int sign = 1;
    if (*s == '-') {
        sign = -1;
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        res = res * 10 + (*s - '0');
        s++;
    }
    return res * sign;
}

// --- JSON Parsing Helpers ---
static int jsoneq(const char *json, jsmntok_t *tok, const char *s) {
    if (tok->type == JSMN_STRING && (int)strlen(s) == tok->end - tok->start &&
        strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
        return 0;
    }
    return -1;
}

// --- Main Program Logic ---
void _start(void) {
    print("grahai: Starting GCP interpreter...\n");

    const char *filepath = "etc/plan.json";
    int fd = syscall_open(filepath);
    if (fd < 0) {
        print("grahai: FAILED to open plan file.\n");
        while(1);
    }

    char file_buffer[900];
    ssize_t bytes_read = syscall_read(fd, file_buffer, sizeof(file_buffer) - 1);
    syscall_close(fd);

    if (bytes_read <= 0) {
        print("grahai: FAILED to read plan file.\n");
        while(1);
    }
    file_buffer[bytes_read] = '\0';

    // Parse the JSON file
    jsmn_parser p;
    jsmntok_t tokens[100];
    jsmn_init(&p);
    
    int r = jsmn_parse(&p, file_buffer, bytes_read, tokens, 100);
    
    if (r < 0) {
        print("grahai: FAILED to parse JSON.\n");
        while(1);
    }

    // Find the "commands" array
    int commands_array_idx = -1;
    for (int i = 1; i < r; i++) {
        if (jsoneq(file_buffer, &tokens[i], "commands") == 0) {
            commands_array_idx = i + 1;
            break;
        }
    }

    if (commands_array_idx == -1 || tokens[commands_array_idx].type != JSMN_ARRAY) {
        print("grahai: Could not find 'commands' array in plan.\n");
        while(1);
    }

    int num_commands = tokens[commands_array_idx].size;
    int current_token = commands_array_idx + 1;

    // Loop through each command object in the array
    for (int i = 0; i < num_commands; i++) {
        gcp_command_t cmd_to_exec = {0};
        int command_obj_token = current_token;
        int tokens_to_skip = 1; // Start with the command object itself
        
        current_token++; // Move to first key in command object

        // Find command type and params object
        char command_name[32] = {0};
        int params_obj_idx = -1;
        int params_obj_size = 0;

        // First pass: find command and params
        int temp_token = current_token;
        int num_obj_props = tokens[command_obj_token].size;
        
        for (int j = 0; j < num_obj_props; j++) {
            if (jsoneq(file_buffer, &tokens[temp_token], "command") == 0) {
                jsmntok_t *t = &tokens[temp_token + 1];
                int len = t->end - t->start;
                if (len < 31) {
                    for(int k=0; k<len; k++) command_name[k] = file_buffer[t->start + k];
                    command_name[len] = '\0';
                }
            } else if (jsoneq(file_buffer, &tokens[temp_token], "params") == 0) {
                params_obj_idx = temp_token + 1;
                params_obj_size = tokens[params_obj_idx].size;
            }
            temp_token += 2;
            tokens_to_skip += 2;
        }
        
        if (strlen(command_name) == 0 || params_obj_idx == -1) {
            current_token = command_obj_token + tokens_to_skip + (params_obj_size * 2);
            continue;
        }

        // Add tokens for all parameters in the params object
        tokens_to_skip += params_obj_size * 2;

        // Parse parameters based on command name
        current_token = params_obj_idx + 1;

        if (strncmp(command_name, "draw_rect", 9) == 0) {
            print("grahai: Executing draw_rect\n");
            cmd_to_exec.command_id = GCP_CMD_DRAW_RECT;
            for (int j = 0; j < params_obj_size; j++) {
                jsmntok_t *key = &tokens[current_token];
                jsmntok_t *val = &tokens[current_token + 1];
                char val_str[32] = {0};
                int len = val->end - val->start;
                if (len < 31) {
                    for(int k=0; k<len; k++) val_str[k] = file_buffer[val->start + k];
                    val_str[len] = '\0';
                }
                
                if (jsoneq(file_buffer, key, "x") == 0) cmd_to_exec.params.draw_rect.x = string_to_long(val_str);
                if (jsoneq(file_buffer, key, "y") == 0) cmd_to_exec.params.draw_rect.y = string_to_long(val_str);
                if (jsoneq(file_buffer, key, "width") == 0) cmd_to_exec.params.draw_rect.width = string_to_long(val_str);
                if (jsoneq(file_buffer, key, "height") == 0) cmd_to_exec.params.draw_rect.height = string_to_long(val_str);
                if (jsoneq(file_buffer, key, "color") == 0) cmd_to_exec.params.draw_rect.color = string_to_long(val_str);
                current_token += 2;
            }
            syscall_gcp_execute(&cmd_to_exec);
            print("grahai: draw_rect completed\n");
        } else if (strncmp(command_name, "draw_string", 11) == 0) {
            cmd_to_exec.command_id = GCP_CMD_DRAW_STRING;
            for (int j = 0; j < params_obj_size; j++) {
                jsmntok_t *key = &tokens[current_token];
                jsmntok_t *val = &tokens[current_token + 1];
                
                if (jsoneq(file_buffer, key, "text") == 0) {
                    int len = val->end - val->start;
                    if (len < GCP_MAX_STRING_LEN - 1) {
                        for(int k=0; k<len; k++) {
                            cmd_to_exec.params.draw_string.text[k] = file_buffer[val->start + k];
                        }
                        cmd_to_exec.params.draw_string.text[len] = '\0';
                    }
                } else {
                    char val_str[32] = {0};
                    int len = val->end - val->start;
                    if (len < 31) {
                        for(int k=0; k<len; k++) val_str[k] = file_buffer[val->start + k];
                        val_str[len] = '\0';
                    }
                    
                    if (jsoneq(file_buffer, key, "x") == 0) cmd_to_exec.params.draw_string.x = string_to_long(val_str);
                    if (jsoneq(file_buffer, key, "y") == 0) cmd_to_exec.params.draw_string.y = string_to_long(val_str);
                    if (jsoneq(file_buffer, key, "fg_color") == 0) cmd_to_exec.params.draw_string.fg_color = string_to_long(val_str);
                    if (jsoneq(file_buffer, key, "bg_color") == 0) cmd_to_exec.params.draw_string.bg_color = string_to_long(val_str);
                }
                current_token += 2;
            }
            syscall_gcp_execute(&cmd_to_exec);
        }
        
        // Move to next command - use our calculated tokens_to_skip
        current_token = command_obj_token + tokens_to_skip;
    }

    print("grahai: Plan execution complete.\n");
    
    // Make sure we flush output before halting
    asm volatile("" ::: "memory");  // Memory barrier
    
    // Exit cleanly OR infinite loop
    syscall_exit(0);

}   