#pragma once
#define CC_SECTION(s)
#define CC_ALIGN(n) __attribute__((aligned(n)))
#define CACHE_LINE_SIZE 32
#define CACHE_SIZE_ALIGN(t, n) ((((((n) * sizeof(t)) - 1U) / CACHE_LINE_SIZE) + 1U) * CACHE_LINE_SIZE / sizeof(t))
