/* SPDX-License-Identifier: MIT */
#ifndef MIHAWK_WINE_SUPPORT_ALGORITHMS_H
#define MIHAWK_WINE_SUPPORT_ALGORITHMS_H
#include <stddef.h>
#include <stdint.h>

int MwCheckedProduct(size_t count, size_t size, size_t *result);
int MwCommandArguments(const uint16_t *command, size_t bound, size_t argument_limit,
                       size_t *argument_offset);
int MwValidateSelectionPayload(const void *payload, size_t length);
void *MwCopyBytes(void *destination, const void *source, size_t length);
void *MwFillBytes(void *destination, int value, size_t length);
int MwCompareBytes(const void *left, const void *right, size_t length);
size_t MwWideLength(const uint16_t *text);
int MwWideCompareOption(const uint16_t *left, const uint16_t *right, size_t count);
int MwIsCommandSpace(unsigned value);
#endif
