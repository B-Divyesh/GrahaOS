// kernel/gcp.h
#pragma once

#include <stdint.h>

// Defines the commands the kernel understands via the GCP syscall.
typedef enum {
    GCP_CMD_DRAW_RECT,
    GCP_CMD_DRAW_STRING
} gcp_command_id_t;

// Define a maximum length for strings passed via GCP to prevent buffer overflows.
#define GCP_MAX_STRING_LEN 128

// This is the core GCP command structure.
// A user-space program fills this and passes a pointer to it
// to the SYS_GCP_EXECUTE syscall.
typedef struct {
    gcp_command_id_t command_id;
    union {
        struct {
            int x, y, width, height;
            uint32_t color;
        } draw_rect;
        struct {
            char text[GCP_MAX_STRING_LEN];
            int x, y;
            uint32_t fg_color;
            uint32_t bg_color;
        } draw_string;
    } params;
} gcp_command_t;