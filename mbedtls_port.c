#include "riscv.h"
#include "timer.h"
#include "string.h"
#include "mbedtls/platform.h"
#include "mbedtls/platform_time.h"
#include "mbedtls/debug.h"
#include "psa/crypto.h"
#include <stdarg.h>

#ifndef SSH_DEBUG_LOG
#define SSH_DEBUG_LOG 0
#endif

#if !SSH_DEBUG_LOG
#define lib_printf(...) do { } while (0)
#endif

static void *mbedtls_os_calloc(size_t n, size_t size)
{
    size_t total = n * size;
    unsigned char *p = (unsigned char *)malloc(total ? total : 1);
    if (p == 0) {
        return 0;
    }
    memset(p, 0, total ? total : 1);
    return p;
}

static void mbedtls_os_free(void *p)
{
    if (p != 0) {
        free(p);
    }
}

mbedtls_time_t mbedtls_os_time(mbedtls_time_t *t);

void mbedtls_os_init(void)
{
    psa_status_t psa_status;
    lib_printf("[BOOT] mbedtls: set calloc/free\n");
    mbedtls_platform_set_calloc_free(mbedtls_os_calloc, mbedtls_os_free);
    lib_printf("[BOOT] mbedtls: set time\n");
    mbedtls_platform_set_time(mbedtls_os_time);
    lib_printf("[BOOT] mbedtls: set debug threshold\n");
    mbedtls_debug_set_threshold(1);
    lib_printf("[BOOT] mbedtls: before psa_crypto_init\n");
    psa_status = psa_crypto_init();
    lib_printf("[BOOT] mbedtls: after psa_crypto_init status=%d\n", (int) psa_status);
}

mbedtls_time_t mbedtls_os_time(mbedtls_time_t *t)
{
    mbedtls_time_t now = (mbedtls_time_t)get_wall_clock_seconds();
    if (t != 0) {
        *t = now;
    }
    return now;
}

mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)get_millisecond_timer();
}

#if defined(MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG)
static uint64_t rng_mix64(uint64_t x)
{
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    return x * 2685821657736338717ULL;
}

psa_status_t mbedtls_psa_external_get_random(
    mbedtls_psa_external_random_context_t *context,
    uint8_t *output, size_t output_size, size_t *output_length)
{
    uint64_t state;
    uint64_t seed;

    if (context == 0 || output == 0 || output_length == 0) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }

    seed = ((uint64_t)get_millisecond_timer() << 32) ^
           (uint64_t)get_wall_clock_seconds() ^
           ((uint64_t)r_mhartid() << 48) ^
           (uint64_t)(uintptr_t)context ^
           (uint64_t)(uintptr_t)&seed;
    if (seed == 0) {
        seed = 0x9e3779b97f4a7c15ULL;
    }

    state = ((uint64_t)context->MBEDTLS_PRIVATE(opaque)[0] << 32) ^
            (uint64_t)context->MBEDTLS_PRIVATE(opaque)[1];
    if (state == 0) {
        state = seed;
    }

    for (size_t i = 0; i < output_size; ) {
        state = rng_mix64(state ^ seed ^ (uint64_t)i);
        for (size_t j = 0; j < sizeof(state) && i < output_size; ++j, ++i) {
            output[i] = (uint8_t) (state >> (8U * j));
        }
    }

    context->MBEDTLS_PRIVATE(opaque)[0] = (uintptr_t)(state >> 32);
    context->MBEDTLS_PRIVATE(opaque)[1] = (uintptr_t)(state & 0xffffffffu);
    *output_length = output_size;
    return PSA_SUCCESS;
}
#endif

#undef MBEDTLS_PLATFORM_STD_VSNPRINTF
#undef MBEDTLS_PLATFORM_STD_SNPRINTF

int mbedtls_os_vsnprintf(char *s, size_t n, const char *fmt, va_list ap)
{
    return lib_vsnprintf(s, n, fmt, ap);
}

int mbedtls_os_snprintf(char *s, size_t n, const char *fmt, ...)
{
    int ret;
    va_list ap;
    va_start(ap, fmt);
    ret = lib_vsnprintf(s, n, fmt, ap);
    va_end(ap);
    return ret;
}

int MBEDTLS_PLATFORM_STD_VSNPRINTF(char *s, size_t n, const char *fmt, va_list ap)
{
    return lib_vsnprintf(s, n, fmt, ap);
}

int MBEDTLS_PLATFORM_STD_SNPRINTF(char *s, size_t n, const char *fmt, ...)
{
    int ret;
    va_list ap;
    va_start(ap, fmt);
    ret = lib_vsnprintf(s, n, fmt, ap);
    va_end(ap);
    return ret;
}

#if defined(MBEDTLS_PSA_DRIVER_GET_ENTROPY)
int mbedtls_platform_get_entropy(psa_driver_get_entropy_flags_t flags,
                                 size_t *estimate_bits,
                                 unsigned char *output,
                                 size_t output_size)
{
    unsigned int tick = get_millisecond_timer();
    unsigned long long mix = ((unsigned long long)tick << 32) ^
                             (unsigned long long)r_mhartid() ^
                             (unsigned long long)(uintptr_t)output ^
                             (unsigned long long)(uintptr_t)&tick;

    if (flags != 0) {
        return PSA_ERROR_NOT_SUPPORTED;
    }

    if (output == 0 || estimate_bits == 0) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }

    lib_printf("[BOOT] mbedtls_platform_get_entropy: size=%u tick=%u\n",
               (unsigned) output_size, tick);

    for (size_t i = 0; i < output_size; ) {
        mix ^= (mix << 13);
        mix ^= (mix >> 7);
        mix ^= (mix << 17);
        size_t chunk = output_size - i;
        if (chunk > sizeof(mix)) chunk = sizeof(mix);
        memcpy(output + i, &mix, chunk);
        i += chunk;
    }

    *estimate_bits = output_size * 8U;
    lib_printf("[BOOT] mbedtls_platform_get_entropy: done bits=%u\n",
               (unsigned) *estimate_bits);
    return 0;
}
#endif
