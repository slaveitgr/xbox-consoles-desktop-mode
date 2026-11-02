/* SPDX-License-Identifier: MIT */
#include "SupportAlgorithms.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>

static void ascii16(const char *text, uint16_t *wide, size_t capacity)
{
    size_t index = 0;
    while (text[index] && index + 1 < capacity) { wide[index] = (unsigned char)text[index]; ++index; }
    wide[index] = 0;
}

int main(void)
{
    uint16_t command[400], option[32], lower[32];
    size_t offset, bytes;
    unsigned char target[9] = {0}, source[5] = {1, 2, 3, 4, 5};
    unsigned char payload[] = {1,0,0,0, 0,0,0,0, 4,0, 0xaa,0xbb, 2,0, 0,0};
    unsigned char multiple[] = {2,0,0,0, 0,0,0,0, 2,0, 0,0, 3,0,0x45, 0,0};
    unsigned char empty[] = {0,0,0,0, 0,0,0,0};
    unsigned char malformed[] = {1,0,0,0, 0,0,0,0, 1,0, 0,0};
    size_t index;
    assert(MwCheckedProduct(0, SIZE_MAX, &bytes) && bytes == 0);
    assert(MwCheckedProduct(SIZE_MAX, 1, &bytes) && bytes == SIZE_MAX);
    assert(!MwCheckedProduct(SIZE_MAX / 2 + 1, 2, &bytes) && bytes == 0);
    assert(!MwCheckedProduct(1, 1, NULL));
    ascii16("\"C:\\Program Files\\Wine\\explorer.exe\" /e,C:\\", command, 400);
    assert(MwCommandArguments(command, 400, 259, &offset) && command[offset] == '/');
    assert(MwWideLength(command + offset) == 6);
    ascii16("explorer.exe", command, 400);
    assert(MwCommandArguments(command, 400, 259, &offset) && command[offset] == 0);
    ascii16("explorer.exe   \t/e,C:\\", command, 400);
    assert(MwCommandArguments(command, 400, 259, &offset) && command[offset] == '/');
    ascii16("\"unterminated", command, 400);
    assert(!MwCommandArguments(command, 400, 259, &offset));
    assert(!MwCommandArguments(NULL, 400, 259, &offset));
    ascii16("explorer.exe /e,C:\\", command, 400);
    assert(!MwCommandArguments(command, 4, 259, &offset));
    assert(!MwCommandArguments(command, 400, 5, &offset));
    for (index = 0; index < 300; ++index) command[index] = 'x';
    command[0] = 'e'; command[1] = ' '; command[300] = 0;
    assert(!MwCommandArguments(command, 400, 259, &offset));
    command[2] = 0x03a9; command[10] = 0;
    assert(MwCommandArguments(command, 400, 259, &offset) && command[offset] == 0x03a9);
    assert(MwIsCommandSpace('\t') && MwIsCommandSpace(' ') && !MwIsCommandSpace(0x03a9));
    ascii16("/DESKTOP", option, 32); ascii16("/desktop", lower, 32);
    assert(MwWideCompareOption(option, lower, 8) == 0);
    option[1] = 'x'; assert(MwWideCompareOption(option, lower, 8) > 0);
    assert(MwWideCompareOption(option, lower, 0) == 0);
    assert(MwCopyBytes(target + 2, source, 5) == target + 2);
    assert(target[1] == 0 && target[2] == 1 && target[6] == 5 && target[7] == 0);
    assert(MwFillBytes(target + 3, 0x1ff, 3) == target + 3);
    assert(target[2] == 1 && target[3] == 255 && target[5] == 255 && target[6] == 5);
    assert(MwCopyBytes(NULL, NULL, 0) == NULL && MwFillBytes(NULL, 0, 0) == NULL);
    assert(MwCompareBytes(source, source, sizeof(source)) == 0);
    assert(MwCompareBytes(source, target + 2, 1) == 0);
    assert(MwCompareBytes(source, target + 2, 2) < 0);
    assert(MwCompareBytes(target + 2, source, 2) > 0);
    assert(MwCompareBytes(NULL, NULL, 0) == 0);
    assert(MwValidateSelectionPayload(payload, sizeof(payload)));
    assert(MwValidateSelectionPayload(multiple, sizeof(multiple)));
    assert(MwValidateSelectionPayload(empty, sizeof(empty)));
    assert(!MwValidateSelectionPayload(NULL, 8));
    assert(!MwValidateSelectionPayload(payload, 7));
    assert(!MwValidateSelectionPayload(payload, sizeof(payload) - 1));
    assert(!MwValidateSelectionPayload(malformed, sizeof(malformed)));
    payload[8] = 0xff; payload[9] = 0xff;
    assert(!MwValidateSelectionPayload(payload, sizeof(payload)));
    empty[0] = 0xff; empty[1] = 0xff;
    assert(!MwValidateSelectionPayload(empty, sizeof(empty)));
    puts("Native Wine source adapter bounds/memory/UTF-16/selection tests passed.");
    return 0;
}
