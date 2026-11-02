/* SPDX-License-Identifier: MIT
 * Small no-CRT helpers for compiling the real Wine Explorer source as a PE64
 * guest. Command-option folding is ASCII; file/path UTF-16 remains untouched.
 */
#include "SupportAlgorithms.h"

int MwCheckedProduct(size_t count, size_t size, size_t *result)
{
    if (!result) return 0;
    *result = 0;
    if (size && count > SIZE_MAX / size) return 0;
    *result = count * size;
    return 1;
}

int MwIsCommandSpace(unsigned value)
{
    return value == 0x20 || (value >= 0x09 && value <= 0x0d);
}

int MwCommandArguments(const uint16_t *command, size_t bound, size_t argument_limit,
                       size_t *argument_offset)
{
    size_t length = 0, position = 0;
    int quoted = 0;
    if (!command || !argument_offset) return 0;
    *argument_offset = 0;
    while (length < bound && command[length]) ++length;
    if (length == bound) return 0;
    while (position < length && MwIsCommandSpace(command[position])) ++position;
    while (position < length)
    {
        if (command[position] == '"') quoted = !quoted;
        else if (!quoted && MwIsCommandSpace(command[position])) break;
        ++position;
    }
    if (quoted) return 0;
    while (position < length && MwIsCommandSpace(command[position])) ++position;
    if (length - position > argument_limit) return 0;
    *argument_offset = position;
    return 1;
}

static unsigned read16(const unsigned char *bytes)
{
    return (unsigned)bytes[0] | ((unsigned)bytes[1] << 8);
}

static uint32_t read32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

int MwValidateSelectionPayload(const void *payload, size_t length)
{
    const unsigned char *bytes = (const unsigned char *)payload;
    size_t position = 8;
    uint32_t item, count;
    if (!bytes || length < 8 || length > 1024 * 1024) return 0;
    count = read32(bytes);
    if (count > 4096) return 0;
    for (item = 0; item < count; ++item)
    {
        for (;;)
        {
            unsigned size;
            if (length - position < 2) return 0;
            size = read16(bytes + position);
            if (!size) { position += 2; break; }
            if (size < 2 || size > length - position) return 0;
            position += size;
        }
    }
    return position == length;
}

void *MwCopyBytes(void *destination, const void *source, size_t length)
{
    /* Volatile access prevents an optimizing compiler from introducing an
     * implicit CRT memcpy dependency into this no-CRT support implementation. */
    volatile unsigned char *out = (volatile unsigned char *)destination;
    const volatile unsigned char *in = (const volatile unsigned char *)source;
    size_t index;
    for (index = 0; index < length; ++index) out[index] = in[index];
    return destination;
}

void *MwFillBytes(void *destination, int value, size_t length)
{
    volatile unsigned char *out = (volatile unsigned char *)destination;
    size_t index;
    for (index = 0; index < length; ++index) out[index] = (unsigned char)value;
    return destination;
}

int MwCompareBytes(const void *left, const void *right, size_t length)
{
    const unsigned char *a = (const unsigned char *)left, *b = (const unsigned char *)right;
    size_t index;
    for (index = 0; index < length; ++index)
        if (a[index] != b[index]) return a[index] < b[index] ? -1 : 1;
    return 0;
}

size_t MwWideLength(const uint16_t *text)
{
    size_t length = 0;
    while (text[length]) ++length;
    return length;
}

static uint16_t fold_option(uint16_t value)
{
    return value >= 'A' && value <= 'Z' ? (uint16_t)(value + ('a' - 'A')) : value;
}

int MwWideCompareOption(const uint16_t *left, const uint16_t *right, size_t count)
{
    size_t index;
    for (index = 0; index < count; ++index)
    {
        uint16_t a = fold_option(left[index]), b = fold_option(right[index]);
        if (a != b) return a < b ? -1 : 1;
        if (!a) return 0;
    }
    return 0;
}
