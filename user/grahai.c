// user/grahai.c - Phase 9e: Enhanced GCP Interpreter + AI Agent
// Two modes:
//   - Legacy: reads etc/plan.json, executes draw commands (GCP)
//   - AI Agent: reads ai_prompt.txt, calls Gemini API, validates & executes plan
#include "syscalls.h"
#include "json.h"
#include "libhttp/libhttp.h"
#include "../kernel/state.h"
#include "../kernel/fs/grahafs.h"

// --- String and Print Helpers ---
void print(const char *str) {
    while (*str) syscall_putc(*str++);
}

void print_int(int n) {
    char buf[12];
    if (n < 0) { syscall_putc('-'); n = -n; }
    if (n == 0) { syscall_putc('0'); return; }
    int i = 0;
    char tmp[12];
    while (n > 0) { tmp[i++] = '0' + (n % 10); n /= 10; }
    while (i > 0) { buf[0] = tmp[--i]; syscall_putc(buf[0]); }
}

// Phase 22 Stage E U21c: strcmp/strncmp/strlen/strcpy now come from libc
// (pulled in via libhttp's transitive deps). Local definitions deleted to
// avoid multiple-definition link errors.
extern int strcmp(const char *s1, const char *s2);
extern int strncmp(const char *s1, const char *s2, size_t n);
extern size_t strlen(const char *s);
extern char *strcpy(char *dest, const char *src);

void *memset_local(void *ptr, int val, size_t n) {
    uint8_t *p = (uint8_t *)ptr;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)val;
    return ptr;
}

void *memcpy_local(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dest;
}

static long long string_to_long(const char* s) {
    long long res = 0;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') {
        res = res * 10 + (*s - '0');
        s++;
    }
    return res * sign;
}

void uint64_to_str(uint64_t num, char *str) {
    if (num == 0) { str[0] = '0'; str[1] = '\0'; return; }
    char tmp[21];
    int i = 0;
    while (num > 0) { tmp[i++] = '0' + (num % 10); num /= 10; }
    int j = 0;
    while (i > 0) str[j++] = tmp[--i];
    str[j] = '\0';
}

// --- String buffer helpers ---
static int str_append(char *buf, int pos, int max, const char *s) {
    while (*s && pos < max - 1) buf[pos++] = *s++;
    buf[pos] = '\0';
    return pos;
}

static int json_append(char *buf, int pos, int max, const char *s) {
    while (*s && pos < max - 1) {
        if (*s == '"' || *s == '\\') {
            if (pos + 2 >= max) break;
            buf[pos++] = '\\'; buf[pos++] = *s;
        } else if (*s == '\n') {
            if (pos + 2 >= max) break;
            buf[pos++] = '\\'; buf[pos++] = 'n';
        } else if (*s == '\r') {
            if (pos + 2 >= max) break;
            buf[pos++] = '\\'; buf[pos++] = 'r';
        } else if (*s == '\t') {
            if (pos + 2 >= max) break;
            buf[pos++] = '\\'; buf[pos++] = 't';
        } else {
            buf[pos++] = *s;
        }
        s++;
    }
    buf[pos] = '\0';
    return pos;
}

static int num_append(char *buf, int pos, int max, uint64_t num) {
    char tmp[21];
    uint64_to_str(num, tmp);
    return str_append(buf, pos, max, tmp);
}

static int int_append(char *buf, int pos, int max, int num) {
    char tmp[12];
    if (num < 0) { tmp[0] = '-'; num = -num; }
    // Use uint64_to_str for the absolute value
    if (num == 0) { return str_append(buf, pos, max, "0"); }
    char abs_buf[12];
    int i = 0;
    int n = num;
    while (n > 0) { abs_buf[i++] = '0' + (n % 10); n /= 10; }
    int j = 0;
    while (i > 0) tmp[j++] = abs_buf[--i];
    tmp[j] = '\0';
    return str_append(buf, pos, max, tmp);
}

// --- JSON Parsing Helpers ---
static int jsoneq(const char *json, jsmntok_t *tok, const char *s) {
    if (tok->type == JSMN_STRING && (int)strlen(s) == tok->end - tok->start &&
        strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
        return 0;
    }
    return -1;
}

// Extract a token string value into a buffer
static void tok_to_str(const char *json, jsmntok_t *tok, char *buf, int max) {
    int len = tok->end - tok->start;
    if (len >= max) len = max - 1;
    memcpy_local(buf, json + tok->start, len);
    buf[len] = '\0';
}

// --- Capability Validation ---
// Check if a command is valid given current capability state
static int validate_command(const char *cmd_name, state_cap_list_t *caps, char *reason, int reason_max) {
    // Commands requiring filesystem capability ON
    if (strcmp(cmd_name, "create_file") == 0 || strcmp(cmd_name, "write_file") == 0 ||
        strcmp(cmd_name, "read_file") == 0 || strcmp(cmd_name, "list_dir") == 0 ||
        strcmp(cmd_name, "tag_file") == 0 || strcmp(cmd_name, "set_importance") == 0 ||
        strcmp(cmd_name, "search_files") == 0) {
        for (uint32_t i = 0; i < caps->count; i++) {
            if (strcmp(caps->caps[i].name, "filesystem") == 0) {
                if (caps->caps[i].state == 2) return 1;  // CAP_STATE_ON
                int p = str_append(reason, 0, reason_max, "requires 'filesystem' capability (currently OFF)");
                (void)p;
                return 0;
            }
        }
        str_append(reason, 0, reason_max, "requires 'filesystem' capability (not found)");
        return 0;
    }

    // Commands always allowed
    if (strcmp(cmd_name, "activate_cap") == 0 || strcmp(cmd_name, "deactivate_cap") == 0 ||
        strcmp(cmd_name, "respond") == 0 ||
        strcmp(cmd_name, "draw_rect") == 0 || strcmp(cmd_name, "draw_string") == 0) {
        return 1;
    }

    // Unknown command
    str_append(reason, 0, reason_max, "unknown command");
    return 0;
}

// --- Command Execution ---

static int exec_create_file(const char *path) {
    int ret = syscall_create(path, 0);  // 0 = regular file
    return ret;
}

static int exec_write_file(const char *path, const char *content) {
    // Create file if it doesn't exist
    syscall_create(path, 0);
    int fd = syscall_open(path);
    if (fd < 0) return fd;
    int len = strlen(content);
    int ret = syscall_write(fd, content, len);
    syscall_close(fd);
    return (ret >= 0) ? 0 : ret;
}

static int exec_read_file(const char *path) {
    int fd = syscall_open(path);
    if (fd < 0) {
        print("  (file not found)\n");
        return fd;
    }
    char buf[1024];
    int n = syscall_read(fd, buf, sizeof(buf) - 1);
    syscall_close(fd);
    if (n < 0) return n;
    buf[n] = '\0';
    print("  Content: ");
    print(buf);
    print("\n");
    return 0;
}

static int exec_list_dir(const char *path) {
    user_dirent_t entry;
    int printed = 0;
    print("  ");
    for (uint32_t idx = 0; idx < 64; idx++) {
        int ret = syscall_readdir(path, idx, &entry);
        if (ret != 0) break;
        if (printed) print("  ");
        print(entry.name);
        if (entry.type == 2) print("/");  // directory
        printed = 1;
    }
    if (!printed) print("(empty)");
    print("\n");
    return 0;
}

static int exec_tag_file(const char *path, const char *tags) {
    grahafs_ai_metadata_t meta;
    memset_local(&meta, 0, sizeof(meta));
    meta.flags = GRAHAFS_META_FLAG_TAGS;
    int len = strlen(tags);
    if (len > 511) len = 511;
    memcpy_local(meta.tags, tags, len);
    meta.tags[len] = '\0';
    return syscall_set_ai_metadata(path, &meta);
}

static int exec_set_importance(const char *path, int value) {
    grahafs_ai_metadata_t meta;
    memset_local(&meta, 0, sizeof(meta));
    meta.flags = GRAHAFS_META_FLAG_IMPORTANCE;
    meta.importance = (uint32_t)value;
    return syscall_set_ai_metadata(path, &meta);
}

static int exec_search_files(const char *tag) {
    grahafs_search_results_t results;
    memset_local(&results, 0, sizeof(results));
    int ret = syscall_search_by_tag(tag, &results, 16);
    if (ret < 0) return ret;
    print("  Found ");
    print_int(results.count);
    print(" match(es):\n");
    for (uint32_t i = 0; i < results.count; i++) {
        print("    ");
        print(results.results[i].path);
        print(" (importance=");
        print_int(results.results[i].importance);
        print(")\n");
    }
    return 0;
}

static int exec_activate_cap(const char *name) {
    return syscall_cap_activate(name);
}

static int exec_deactivate_cap(const char *name) {
    return syscall_cap_deactivate(name);
}

// --- AI Agent Mode ---

static void run_ai_mode(const char *user_prompt) {
    print("grahai: AI Agent mode\n");

    // 1. Read API key
    int fd = syscall_open("etc/ai.conf");
    if (fd < 0) {
        print("grahai: cannot read API key (etc/ai.conf)\n");
        return;
    }
    char api_key[128];
    memset_local(api_key, 0, sizeof(api_key));
    int key_len = syscall_read(fd, api_key, sizeof(api_key) - 1);
    syscall_close(fd);

    // Trim whitespace
    while (key_len > 0 && (api_key[key_len-1] == '\n' || api_key[key_len-1] == '\r' ||
                            api_key[key_len-1] == ' '))
        api_key[--key_len] = '\0';

    if (key_len <= 0) {
        print("grahai: API key is empty\n");
        return;
    }

    // 2. Collect system state
    state_memory_t mem;
    state_process_list_t procs;
    state_cap_list_t caps;
    state_filesystem_t fs;
    memset_local(&mem, 0, sizeof(mem));
    memset_local(&procs, 0, sizeof(procs));
    memset_local(&caps, 0, sizeof(caps));
    memset_local(&fs, 0, sizeof(fs));
    syscall_get_system_state(STATE_CAT_MEMORY, &mem, sizeof(mem));
    syscall_get_system_state(STATE_CAT_PROCESSES, &procs, sizeof(procs));
    syscall_get_system_state(STATE_CAT_CAPABILITIES, &caps, sizeof(caps));
    syscall_get_system_state(STATE_CAT_FILESYSTEM, &fs, sizeof(fs));

    // 3. Build structured system prompt
    char prompt[3072];
    int p = 0;

    p = str_append(prompt, p, sizeof(prompt),
        "You are the GrahaOS AI agent running inside a custom bare-metal x86_64 OS. "
        "You can execute structured plans. Available commands:\\n\\n"
        "FILE OPERATIONS (requires 'filesystem' capability ON):\\n"
        "  {\\\"command\\\":\\\"create_file\\\",\\\"params\\\":{\\\"path\\\":\\\"/path\\\"}}\\n"
        "  {\\\"command\\\":\\\"write_file\\\",\\\"params\\\":{\\\"path\\\":\\\"/path\\\",\\\"content\\\":\\\"data\\\"}}\\n"
        "  {\\\"command\\\":\\\"read_file\\\",\\\"params\\\":{\\\"path\\\":\\\"/path\\\"}}\\n"
        "  {\\\"command\\\":\\\"list_dir\\\",\\\"params\\\":{\\\"path\\\":\\\"/\\\"}}\\n\\n"
        "AI METADATA (requires 'filesystem' capability ON):\\n"
        "  {\\\"command\\\":\\\"tag_file\\\",\\\"params\\\":{\\\"path\\\":\\\"/path\\\",\\\"tags\\\":\\\"tag1,tag2\\\"}}\\n"
        "  {\\\"command\\\":\\\"set_importance\\\",\\\"params\\\":{\\\"path\\\":\\\"/path\\\",\\\"value\\\":85}}\\n"
        "  {\\\"command\\\":\\\"search_files\\\",\\\"params\\\":{\\\"tag\\\":\\\"config\\\"}}\\n\\n"
        "CAPABILITY MANAGEMENT:\\n"
        "  {\\\"command\\\":\\\"activate_cap\\\",\\\"params\\\":{\\\"name\\\":\\\"cap_name\\\"}}\\n"
        "  {\\\"command\\\":\\\"deactivate_cap\\\",\\\"params\\\":{\\\"name\\\":\\\"cap_name\\\"}}\\n\\n"
        "OUTPUT:\\n"
        "  {\\\"command\\\":\\\"respond\\\",\\\"params\\\":{\\\"text\\\":\\\"response to user\\\"}}\\n\\n"
        "GRAPHICS (via GCP):\\n"
        "  {\\\"command\\\":\\\"draw_rect\\\",\\\"params\\\":{\\\"x\\\":10,\\\"y\\\":10,\\\"width\\\":100,\\\"height\\\":50,\\\"color\\\":255}}\\n"
        "  {\\\"command\\\":\\\"draw_string\\\",\\\"params\\\":{\\\"text\\\":\\\"hello\\\",\\\"x\\\":10,\\\"y\\\":10,\\\"fg_color\\\":16777215,\\\"bg_color\\\":0}}\\n\\n");

    // System state
    p = str_append(prompt, p, sizeof(prompt), "SYSTEM STATE:\\n");
    p = str_append(prompt, p, sizeof(prompt), "  Memory: total=");
    p = num_append(prompt, p, sizeof(prompt), mem.total_physical);
    p = str_append(prompt, p, sizeof(prompt), " free=");
    p = num_append(prompt, p, sizeof(prompt), mem.free_physical);
    p = str_append(prompt, p, sizeof(prompt), "\\n  Processes:");
    for (uint32_t i = 0; i < procs.count && i < 8; i++) {
        if (procs.procs[i].pid <= 0) continue;
        p = str_append(prompt, p, sizeof(prompt), " ");
        p = str_append(prompt, p, sizeof(prompt), procs.procs[i].name);
        p = str_append(prompt, p, sizeof(prompt), "(pid=");
        p = int_append(prompt, p, sizeof(prompt), procs.procs[i].pid);
        p = str_append(prompt, p, sizeof(prompt), ")");
    }
    p = str_append(prompt, p, sizeof(prompt), "\\n  Capabilities:");
    for (uint32_t i = 0; i < caps.count; i++) {
        if (caps.caps[i].deleted) continue;
        p = str_append(prompt, p, sizeof(prompt), " ");
        p = str_append(prompt, p, sizeof(prompt), caps.caps[i].name);
        p = str_append(prompt, p, sizeof(prompt), "[");
        p = str_append(prompt, p, sizeof(prompt), caps.caps[i].state == 2 ? "ON" : "OFF");
        p = str_append(prompt, p, sizeof(prompt), "]");
    }
    p = str_append(prompt, p, sizeof(prompt), "\\n  Filesystem: free_blocks=");
    p = num_append(prompt, p, sizeof(prompt), fs.grahafs_free_blocks);
    p = str_append(prompt, p, sizeof(prompt), " free_inodes=");
    p = num_append(prompt, p, sizeof(prompt), fs.grahafs_free_inodes);
    p = str_append(prompt, p, sizeof(prompt), "\\n\\n");

    // User request
    p = str_append(prompt, p, sizeof(prompt), "USER REQUEST: ");
    p = json_append(prompt, p, sizeof(prompt), user_prompt);
    p = str_append(prompt, p, sizeof(prompt),
        "\\n\\nRespond with ONLY valid JSON: {\\\"commands\\\":[...]}\\n"
        "Use \\\"respond\\\" command to communicate text to the user.\\n"
        "Do NOT include any text outside the JSON object.");

    // 4. Build Gemini request body
    char body[4096];
    int b = 0;
    b = str_append(body, b, sizeof(body), "{\"contents\":[{\"parts\":[{\"text\":\"");
    b = str_append(body, b, sizeof(body), prompt);
    b = str_append(body, b, sizeof(body), "\"}]}]}");

    // 5. Build URL
    char url[512];
    int u = 0;
    u = str_append(url, u, sizeof(url),
        "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent?key=");
    u = str_append(url, u, sizeof(url), api_key);

    print("grahai: Calling Gemini API...\n");

    // 6. HTTP POST via libhttp (Phase 22 Stage E migration; pre-Phase-22
    //    this called SYS_HTTP_POST directly).
    //
    //    NOTE: until P22.D.2 lands the Mongoose TLS extraction, http_post
    //    over https:// returns -EPROTO. We surface a clear error so the
    //    rest of the agent path remains debuggable; callers can rerun
    //    after extraction lands.
    char response[8192];
    memset_local(response, 0, sizeof(response));
    int ret = 0;
    int response_len = 0;

    {
        http_response_t resp;
        memset_local(&resp, 0, sizeof(resp));
        int rc = http_post(&resp, url,
                           (const uint8_t *)body, (uint32_t)b,
                           "application/json",
                           /*timeout_ms=*/15000);

        // Handle 429 rate limit by inspecting the body for "429".
        int is_429 = 0;
        if (rc == 0 && resp.body && resp.body_len >= 3) {
            for (uint32_t i = 0; i + 2 < resp.body_len; i++) {
                if (resp.body[i] == '4' && resp.body[i+1] == '2' && resp.body[i+2] == '9') {
                    is_429 = 1; break;
                }
            }
        }
        if (is_429) {
            print("grahai: Rate limited, retrying in 2s...\n");
            for (volatile int d = 0; d < 200000000; d++) {}
            http_response_free(&resp);
            memset_local(&resp, 0, sizeof(resp));
            rc = http_post(&resp, url,
                           (const uint8_t *)body, (uint32_t)b,
                           "application/json", /*timeout_ms=*/15000);
        }

        if (rc < 0) {
            print("grahai: HTTP POST failed (error ");
            print_int(rc);
            print(")\n");
            http_response_free(&resp);
            return;
        }
        if (resp.body == 0 || resp.body_len == 0) {
            print("grahai: empty response\n");
            http_response_free(&resp);
            return;
        }

        // Copy body into the existing fixed buffer so the rest of the
        // function (jsmn_parse, scanning for "text", etc.) keeps working
        // unchanged. body_len is capped by libhttp's LIBHTTP_MAX_BODY_BYTES
        // (128 KiB); we further cap to the local buffer.
        uint32_t copy_len = resp.body_len;
        if (copy_len > sizeof(response) - 1) copy_len = sizeof(response) - 1;
        memcpy_local(response, resp.body, copy_len);
        response[copy_len] = '\0';
        response_len = (int)copy_len;
        ret = response_len;

        http_response_free(&resp);
    }
    (void)response_len;

    // 7. Parse Gemini response (outer JSON)
    jsmn_parser parser;
    jsmntok_t outer_tokens[256];
    jsmn_init(&parser);
    int outer_count = jsmn_parse(&parser, response, ret, outer_tokens, 256);

    if (outer_count < 0) {
        print("grahai: JSON parse error\n");
        return;
    }

    // Find candidates[0].content.parts[0].text
    char ai_text[4096];
    int ai_text_len = 0;
    int found_text = 0;

    for (int i = 0; i < outer_count - 1; i++) {
        if (jsoneq(response, &outer_tokens[i], "text") == 0) {
            jsmntok_t *val = &outer_tokens[i + 1];
            if (val->type == JSMN_STRING) {
                ai_text_len = val->end - val->start;
                if (ai_text_len >= (int)sizeof(ai_text)) ai_text_len = sizeof(ai_text) - 1;

                // Copy and unescape the text
                int dst = 0;
                for (int j = val->start; j < val->end && dst < (int)sizeof(ai_text) - 1; j++) {
                    if (response[j] == '\\' && j + 1 < val->end) {
                        char next = response[j + 1];
                        if (next == 'n') { ai_text[dst++] = '\n'; j++; }
                        else if (next == 't') { ai_text[dst++] = '\t'; j++; }
                        else if (next == '\\') { ai_text[dst++] = '\\'; j++; }
                        else if (next == '"') { ai_text[dst++] = '"'; j++; }
                        else { ai_text[dst++] = response[j]; }
                    } else {
                        ai_text[dst++] = response[j];
                    }
                }
                ai_text[dst] = '\0';
                ai_text_len = dst;
                found_text = 1;
                break;
            }
        }
    }

    if (!found_text) {
        print("grahai: could not extract AI response\n");
        // Check for error message
        for (int i = 0; i < outer_count - 1; i++) {
            if (jsoneq(response, &outer_tokens[i], "message") == 0) {
                jsmntok_t *val = &outer_tokens[i + 1];
                if (val->type == JSMN_STRING) {
                    print("grahai: API error: ");
                    for (int j = val->start; j < val->end; j++) syscall_putc(response[j]);
                    print("\n");
                    return;
                }
            }
        }
        return;
    }

    // Strip markdown code fences if present (```json ... ```)
    char *plan_json = ai_text;
    int plan_json_len = ai_text_len;

    // Skip leading whitespace
    while (plan_json_len > 0 && (*plan_json == ' ' || *plan_json == '\n' || *plan_json == '\r' || *plan_json == '\t')) {
        plan_json++; plan_json_len--;
    }

    // Strip ```json or ``` prefix
    if (plan_json_len > 7 && strncmp(plan_json, "```json", 7) == 0) {
        plan_json += 7; plan_json_len -= 7;
        // Skip newline after ```json
        while (plan_json_len > 0 && (*plan_json == '\n' || *plan_json == '\r')) {
            plan_json++; plan_json_len--;
        }
    } else if (plan_json_len > 3 && strncmp(plan_json, "```", 3) == 0) {
        plan_json += 3; plan_json_len -= 3;
        while (plan_json_len > 0 && (*plan_json == '\n' || *plan_json == '\r')) {
            plan_json++; plan_json_len--;
        }
    }

    // Strip trailing ``` if present
    if (plan_json_len > 3) {
        char *end = plan_json + plan_json_len - 1;
        // Skip trailing whitespace
        while (end > plan_json && (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t')) end--;
        if (end - 2 >= plan_json && end[-2] == '`' && end[-1] == '`' && end[0] == '`') {
            end[-2] = '\0';
            plan_json_len = (end - 2) - plan_json;
        }
    }

    // 8. Parse the AI's plan JSON (inner JSON)
    jsmn_parser plan_parser;
    jsmntok_t plan_tokens[256];
    jsmn_init(&plan_parser);
    int plan_count = jsmn_parse(&plan_parser, plan_json, plan_json_len, plan_tokens, 256);

    if (plan_count < 0) {
        print("grahai: failed to parse AI plan JSON (error ");
        print_int(plan_count);
        print(")\n");
        print("AI response:\n");
        print(ai_text);
        print("\n");
        return;
    }

    // Find "commands" array
    int cmds_array_idx = -1;
    for (int i = 0; i < plan_count; i++) {
        if (jsoneq(plan_json, &plan_tokens[i], "commands") == 0) {
            cmds_array_idx = i + 1;
            break;
        }
    }

    if (cmds_array_idx < 0 || plan_tokens[cmds_array_idx].type != JSMN_ARRAY) {
        print("grahai: no 'commands' array in AI response\n");
        print("AI response:\n");
        print(ai_text);
        print("\n");
        return;
    }

    int num_commands = plan_tokens[cmds_array_idx].size;
    print("grahai: received plan with ");
    print_int(num_commands);
    print(" command(s)\n");

    // 9. VALIDATE all commands before execution
    // First pass: extract command names and validate
    int validation_failed = 0;
    int cur_tok = cmds_array_idx + 1;

    // Save token position for execution pass
    int exec_start_tok = cur_tok;

    for (int i = 0; i < num_commands; i++) {
        if (plan_tokens[cur_tok].type != JSMN_OBJECT) {
            cur_tok++;
            continue;
        }
        int obj_size = plan_tokens[cur_tok].size;
        int inner = cur_tok + 1;
        char cmd_name[32];
        memset_local(cmd_name, 0, sizeof(cmd_name));

        for (int j = 0; j < obj_size; j++) {
            if (jsoneq(plan_json, &plan_tokens[inner], "command") == 0) {
                tok_to_str(plan_json, &plan_tokens[inner + 1], cmd_name, sizeof(cmd_name));
            }
            // Skip key + value (value might be object with children)
            int skip = 2;
            if (plan_tokens[inner + 1].type == JSMN_OBJECT || plan_tokens[inner + 1].type == JSMN_ARRAY) {
                // Count all tokens inside this value
                int count_inner = 1;
                int t = inner + 2;
                for (int depth_tokens = plan_tokens[inner + 1].size * 2; depth_tokens > 0; depth_tokens--) {
                    count_inner++;
                    t++;
                }
                skip = 1 + count_inner;
            }
            inner += skip;
        }

        if (cmd_name[0] != '\0') {
            char reason[128];
            memset_local(reason, 0, sizeof(reason));
            if (!validate_command(cmd_name, &caps, reason, sizeof(reason))) {
                if (!validation_failed) {
                    print("\ngrahai: VALIDATION FAILED\n");
                }
                print("  Command '");
                print(cmd_name);
                print("' ");
                print(reason);
                print("\n");
                validation_failed = 1;
            }
        }

        // Skip to next command object (navigate all tokens in this object)
        // Use a simple heuristic: find the next token at or beyond the end of this object
        int obj_end = plan_tokens[cur_tok].end;
        cur_tok++;
        while (cur_tok < plan_count && plan_tokens[cur_tok].start < obj_end) {
            cur_tok++;
        }
    }

    if (validation_failed) {
        print("Plan NOT executed (0/");
        print_int(num_commands);
        print(" commands).\n");
        return;
    }

    print("grahai: validation passed, executing plan...\n\n");

    // 10. Execute validated plan with per-command feedback
    cur_tok = exec_start_tok;
    int success_count = 0;
    int fail_count = 0;

    for (int i = 0; i < num_commands; i++) {
        if (cur_tok >= plan_count || plan_tokens[cur_tok].type != JSMN_OBJECT) {
            cur_tok++;
            continue;
        }

        int obj_size = plan_tokens[cur_tok].size;
        int obj_end = plan_tokens[cur_tok].end;
        int inner = cur_tok + 1;

        // Extract command name and params
        char cmd_name[32];
        memset_local(cmd_name, 0, sizeof(cmd_name));
        int params_idx = -1;

        for (int j = 0; j < obj_size; j++) {
            if (inner >= plan_count) break;
            if (jsoneq(plan_json, &plan_tokens[inner], "command") == 0) {
                tok_to_str(plan_json, &plan_tokens[inner + 1], cmd_name, sizeof(cmd_name));
                inner += 2;
            } else if (jsoneq(plan_json, &plan_tokens[inner], "params") == 0) {
                params_idx = inner + 1;
                // Skip params object and its children
                int skip = 1;
                if (plan_tokens[inner + 1].type == JSMN_OBJECT) {
                    int params_end = plan_tokens[inner + 1].end;
                    int t = inner + 2;
                    while (t < plan_count && plan_tokens[t].start < params_end) {
                        skip++;
                        t++;
                    }
                }
                inner += 1 + skip;
            } else {
                inner += 2;
            }
        }

        // Print progress
        print("[");
        print_int(i + 1);
        print("/");
        print_int(num_commands);
        print("] ");
        print(cmd_name);
        print(": ");

        int result = -1;

        // Extract params and execute
        if (params_idx >= 0 && params_idx < plan_count) {
            int params_size = plan_tokens[params_idx].size;
            int pk = params_idx + 1;

            // Helper: extract common param values
            char path[128], content[512], tags[256], name[64], text[256];
            int value = 0;
            int x = 0, y = 0, width = 0, height = 0;
            uint32_t color = 0, fg_color = 0xFFFFFF, bg_color = 0;
            memset_local(path, 0, sizeof(path));
            memset_local(content, 0, sizeof(content));
            memset_local(tags, 0, sizeof(tags));
            memset_local(name, 0, sizeof(name));
            memset_local(text, 0, sizeof(text));

            for (int j = 0; j < params_size && pk + 1 < plan_count; j++) {
                char pval[512];
                memset_local(pval, 0, sizeof(pval));
                tok_to_str(plan_json, &plan_tokens[pk + 1], pval, sizeof(pval));

                if (jsoneq(plan_json, &plan_tokens[pk], "path") == 0) strcpy(path, pval);
                else if (jsoneq(plan_json, &plan_tokens[pk], "content") == 0) strcpy(content, pval);
                else if (jsoneq(plan_json, &plan_tokens[pk], "tags") == 0) strcpy(tags, pval);
                else if (jsoneq(plan_json, &plan_tokens[pk], "tag") == 0) strcpy(tags, pval);
                else if (jsoneq(plan_json, &plan_tokens[pk], "name") == 0) strcpy(name, pval);
                else if (jsoneq(plan_json, &plan_tokens[pk], "text") == 0) strcpy(text, pval);
                else if (jsoneq(plan_json, &plan_tokens[pk], "value") == 0) value = (int)string_to_long(pval);
                else if (jsoneq(plan_json, &plan_tokens[pk], "x") == 0) x = (int)string_to_long(pval);
                else if (jsoneq(plan_json, &plan_tokens[pk], "y") == 0) y = (int)string_to_long(pval);
                else if (jsoneq(plan_json, &plan_tokens[pk], "width") == 0) width = (int)string_to_long(pval);
                else if (jsoneq(plan_json, &plan_tokens[pk], "height") == 0) height = (int)string_to_long(pval);
                else if (jsoneq(plan_json, &plan_tokens[pk], "color") == 0) color = (uint32_t)string_to_long(pval);
                else if (jsoneq(plan_json, &plan_tokens[pk], "fg_color") == 0) fg_color = (uint32_t)string_to_long(pval);
                else if (jsoneq(plan_json, &plan_tokens[pk], "bg_color") == 0) bg_color = (uint32_t)string_to_long(pval);
                pk += 2;
            }

            // Dispatch command
            if (strcmp(cmd_name, "create_file") == 0) {
                result = exec_create_file(path);
            } else if (strcmp(cmd_name, "write_file") == 0) {
                result = exec_write_file(path, content);
            } else if (strcmp(cmd_name, "read_file") == 0) {
                result = exec_read_file(path);
            } else if (strcmp(cmd_name, "list_dir") == 0) {
                result = exec_list_dir(path);
            } else if (strcmp(cmd_name, "tag_file") == 0) {
                result = exec_tag_file(path, tags);
            } else if (strcmp(cmd_name, "set_importance") == 0) {
                result = exec_set_importance(path, value);
            } else if (strcmp(cmd_name, "search_files") == 0) {
                result = exec_search_files(tags);
            } else if (strcmp(cmd_name, "activate_cap") == 0) {
                result = exec_activate_cap(name);
            } else if (strcmp(cmd_name, "deactivate_cap") == 0) {
                result = exec_deactivate_cap(name);
            } else if (strcmp(cmd_name, "respond") == 0) {
                print("OK\n  ");
                print(text);
                print("\n");
                result = 0;
                success_count++;
                goto next_cmd;
            } else if (strcmp(cmd_name, "draw_rect") == 0) {
                gcp_command_t gcp = {0};
                gcp.command_id = GCP_CMD_DRAW_RECT;
                gcp.params.draw_rect.x = x;
                gcp.params.draw_rect.y = y;
                gcp.params.draw_rect.width = width;
                gcp.params.draw_rect.height = height;
                gcp.params.draw_rect.color = color;
                result = syscall_gcp_execute(&gcp);
            } else if (strcmp(cmd_name, "draw_string") == 0) {
                gcp_command_t gcp = {0};
                gcp.command_id = GCP_CMD_DRAW_STRING;
                gcp.params.draw_string.x = x;
                gcp.params.draw_string.y = y;
                gcp.params.draw_string.fg_color = fg_color;
                gcp.params.draw_string.bg_color = bg_color;
                int tlen = strlen(text);
                if (tlen >= GCP_MAX_STRING_LEN) tlen = GCP_MAX_STRING_LEN - 1;
                memcpy_local(gcp.params.draw_string.text, text, tlen);
                gcp.params.draw_string.text[tlen] = '\0';
                result = syscall_gcp_execute(&gcp);
            } else {
                print("SKIPPED (unknown)\n");
                fail_count++;
                goto next_cmd;
            }
        }

        if (result == 0) {
            print("OK\n");
            success_count++;
        } else {
            print("FAILED (error ");
            print_int(result);
            print(")\n");
            fail_count++;
        }

next_cmd:
        // Advance to next command object
        cur_tok++;
        while (cur_tok < plan_count && plan_tokens[cur_tok].start < obj_end) {
            cur_tok++;
        }
    }

    print("\nPlan complete: ");
    print_int(success_count);
    print(" succeeded, ");
    print_int(fail_count);
    print(" failed.\n");
}

// --- Legacy Plan Mode ---

static void run_legacy_mode(void) {
    print("grahai: Starting GCP interpreter...\n");

    const char *filepath = "etc/plan.json";
    int fd = syscall_open(filepath);
    if (fd < 0) {
        print("grahai: FAILED to open plan file.\n");
        syscall_exit(1);
    }

    char file_buffer[900];
    ssize_t bytes_read = syscall_read(fd, file_buffer, sizeof(file_buffer) - 1);
    syscall_close(fd);

    if (bytes_read <= 0) {
        print("grahai: FAILED to read plan file.\n");
        syscall_exit(1);
    }
    file_buffer[bytes_read] = '\0';

    jsmn_parser parser;
    jsmntok_t tokens[100];
    jsmn_init(&parser);
    int r = jsmn_parse(&parser, file_buffer, bytes_read, tokens, 100);

    if (r < 0) {
        print("grahai: FAILED to parse JSON.\n");
        syscall_exit(1);
    }

    int commands_array_idx = -1;
    for (int i = 1; i < r; i++) {
        if (jsoneq(file_buffer, &tokens[i], "commands") == 0) {
            commands_array_idx = i + 1;
            break;
        }
    }

    if (commands_array_idx == -1 || tokens[commands_array_idx].type != JSMN_ARRAY) {
        print("grahai: Could not find 'commands' array in plan.\n");
        syscall_exit(1);
    }

    int num_commands = tokens[commands_array_idx].size;
    int current_token = commands_array_idx + 1;

    for (int i = 0; i < num_commands; i++) {
        gcp_command_t cmd_to_exec = {0};
        int command_obj_token = current_token;
        int tokens_to_skip = 1;

        current_token++;

        char command_name[32] = {0};
        int params_obj_idx = -1;
        int params_obj_size = 0;

        int temp_token = current_token;
        int num_obj_props = tokens[command_obj_token].size;

        for (int j = 0; j < num_obj_props; j++) {
            if (jsoneq(file_buffer, &tokens[temp_token], "command") == 0) {
                jsmntok_t *t = &tokens[temp_token + 1];
                int len = t->end - t->start;
                if (len < 31) {
                    for(int k = 0; k < len; k++) command_name[k] = file_buffer[t->start + k];
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

        tokens_to_skip += params_obj_size * 2;
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
                    for(int k = 0; k < len; k++) val_str[k] = file_buffer[val->start + k];
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
                        for(int k = 0; k < len; k++)
                            cmd_to_exec.params.draw_string.text[k] = file_buffer[val->start + k];
                        cmd_to_exec.params.draw_string.text[len] = '\0';
                    }
                } else {
                    char val_str[32] = {0};
                    int len = val->end - val->start;
                    if (len < 31) {
                        for(int k = 0; k < len; k++) val_str[k] = file_buffer[val->start + k];
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

        current_token = command_obj_token + tokens_to_skip;
    }

    print("grahai: Plan execution complete.\n");
}

// --- Entry Point ---
void _start(void) {
    // Check for AI agent mode: if ai_prompt.txt exists, use AI mode
    int fd = syscall_open("ai_prompt.txt");
    if (fd >= 0) {
        char prompt[512];
        memset_local(prompt, 0, sizeof(prompt));
        int n = syscall_read(fd, prompt, sizeof(prompt) - 1);
        syscall_close(fd);

        if (n > 0) {
            prompt[n] = '\0';
            // Trim trailing whitespace
            while (n > 0 && (prompt[n-1] == '\n' || prompt[n-1] == '\r' || prompt[n-1] == ' '))
                prompt[--n] = '\0';

            run_ai_mode(prompt);
        } else {
            print("grahai: ai_prompt.txt is empty, running legacy mode\n");
            run_legacy_mode();
        }
    } else {
        run_legacy_mode();
    }

    asm volatile("" ::: "memory");
    syscall_exit(0);
}
