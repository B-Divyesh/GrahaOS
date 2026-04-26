// libc/src/string.c
// Phase 7c: Standard C string and memory functions

#include <string.h>
#include <stdint.h>

// ===== MEMORY FUNCTIONS =====

/**
 * @brief Copy memory area
 */
void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    // Fast path: 8-byte copies for aligned addresses
    if (((uintptr_t)d & 7) == 0 && ((uintptr_t)s & 7) == 0) {
        while (n >= 8) {
            *(uint64_t *)d = *(const uint64_t *)s;
            d += 8;
            s += 8;
            n -= 8;
        }
    }

    // Copy remaining bytes
    while (n--) {
        *d++ = *s++;
    }

    return dest;
}

/**
 * @brief Copy memory area (handles overlapping regions)
 */
void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    if (d < s) {
        // Copy forward
        while (n--) {
            *d++ = *s++;
        }
    } else if (d > s) {
        // Copy backward
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }

    return dest;
}

/**
 * @brief Fill memory with constant byte
 */
void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    uint8_t val = (uint8_t)c;

    // Fast path: 8-byte fills for aligned addresses
    if (((uintptr_t)p & 7) == 0 && n >= 8) {
        uint64_t val64 = val;
        val64 |= val64 << 8;
        val64 |= val64 << 16;
        val64 |= val64 << 32;

        while (n >= 8) {
            *(uint64_t *)p = val64;
            p += 8;
            n -= 8;
        }
    }

    // Fill remaining bytes
    while (n--) {
        *p++ = val;
    }

    return s;
}

/**
 * @brief Compare memory areas
 */
int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;

    while (n--) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1++;
        p2++;
    }

    return 0;
}

/**
 * @brief Scan memory for a character
 */
void *memchr(const void *s, int c, size_t n) {
    const uint8_t *p = (const uint8_t *)s;
    uint8_t val = (uint8_t)c;

    while (n--) {
        if (*p == val) {
            return (void *)p;
        }
        p++;
    }

    return NULL;
}

// ===== STRING FUNCTIONS =====

/**
 * @brief Calculate length of string
 */
size_t strlen(const char *s) {
    size_t len = 0;
    while (*s++) {
        len++;
    }
    return len;
}

/**
 * @brief Copy string
 */
char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

/**
 * @brief Copy string with length limit
 */
char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;

    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }

    // Pad with null bytes
    for (; i < n; i++) {
        dest[i] = '\0';
    }

    return dest;
}

/**
 * @brief Concatenate strings
 */
char *strcat(char *dest, const char *src) {
    char *d = dest;

    // Find end of dest
    while (*d) {
        d++;
    }

    // Append src
    while ((*d++ = *src++));

    return dest;
}

/**
 * @brief Concatenate strings with length limit
 */
char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest;

    // Find end of dest
    while (*d) {
        d++;
    }

    // Append up to n characters
    while (n-- && (*d++ = *src++));

    // Ensure null termination
    if (n == (size_t)-1) {
        *d = '\0';
    }

    return dest;
}

/**
 * @brief Compare strings
 */
int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const uint8_t *)s1 - *(const uint8_t *)s2;
}

/**
 * @brief Compare strings with length limit
 */
int strncmp(const char *s1, const char *s2, size_t n) {
    if (n == 0) {
        return 0;
    }

    while (n-- && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }

    if (n == (size_t)-1) {
        return 0;
    }

    return *(const uint8_t *)s1 - *(const uint8_t *)s2;
}

/**
 * @brief Locate character in string
 */
char *strchr(const char *s, int c) {
    char ch = (char)c;

    while (*s) {
        if (*s == ch) {
            return (char *)s;
        }
        s++;
    }

    // Check for null terminator match
    if (ch == '\0') {
        return (char *)s;
    }

    return NULL;
}

/**
 * @brief Locate last occurrence of character in string
 */
char *strrchr(const char *s, int c) {
    char ch = (char)c;
    const char *last = NULL;

    while (*s) {
        if (*s == ch) {
            last = s;
        }
        s++;
    }

    // Check for null terminator match
    if (ch == '\0') {
        return (char *)s;
    }

    return (char *)last;
}

/**
 * @brief Locate substring
 */
char *strstr(const char *haystack, const char *needle) {
    if (*needle == '\0') {
        return (char *)haystack;
    }

    size_t needle_len = strlen(needle);

    while (*haystack) {
        if (*haystack == *needle) {
            if (strncmp(haystack, needle, needle_len) == 0) {
                return (char *)haystack;
            }
        }
        haystack++;
    }

    return NULL;
}

/**
 * @brief Get length of prefix substring
 */
size_t strspn(const char *s, const char *accept) {
    size_t count = 0;

    while (*s) {
        const char *a = accept;
        int found = 0;

        while (*a) {
            if (*s == *a) {
                found = 1;
                break;
            }
            a++;
        }

        if (!found) {
            break;
        }

        count++;
        s++;
    }

    return count;
}

/**
 * @brief Get length of prefix not containing reject characters
 */
size_t strcspn(const char *s, const char *reject) {
    size_t count = 0;

    while (*s) {
        const char *r = reject;

        while (*r) {
            if (*s == *r) {
                return count;
            }
            r++;
        }

        count++;
        s++;
    }

    return count;
}

// strnlen — Phase 22 closeout (G1.3): added for libtls-mg vendored Mongoose.
size_t strnlen(const char *s, size_t maxlen) {
    size_t n = 0;
    while (n < maxlen && s[n]) n++;
    return n;
}
