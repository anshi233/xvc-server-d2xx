/*
 * ftdi_adapter.c - FTDI Adapter Layer
 * XVC Server for Digilent HS2
 * 
 * Uses MPSSE mode with D2XX driver for high-speed JTAG (up to 30MHz).
 * 
 * Supports both x86_64 and arm64 platforms.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ftdi_adapter.h"
#include "mpsse_adapter.h"
#include "logging.h"

/* Internal context structure - now only uses MPSSE mode */
struct ftdi_context_s {
    mpsse_context_t *mpsse;
    bool is_open;
    int verbose;
    char error[256];
};

ftdi_context_t* ftdi_adapter_create(void)
{
    ftdi_context_t *ctx = calloc(1, sizeof(ftdi_context_t));
    if (!ctx) {
        LOG_ERROR("Failed to allocate FTDI context");
        return NULL;
    }
    
    /* Create MPSSE context */
    ctx->mpsse = mpsse_adapter_create();
    if (!ctx->mpsse) {
        free(ctx);
        return NULL;
    }
    
    return ctx;
}

void ftdi_adapter_destroy(ftdi_context_t *ctx)
{
    if (!ctx) return;
    
    if (ctx->is_open) {
        ftdi_adapter_close(ctx);
    }
    
    if (ctx->mpsse) {
        mpsse_adapter_destroy(ctx->mpsse);
        ctx->mpsse = NULL;
    }
    
    free(ctx);
}

adapter_mode_t ftdi_adapter_get_mode(const ftdi_context_t *ctx)
{
    (void)ctx;
    /* Only MPSSE mode is supported now */
    return ADAPTER_MODE_MPSSE;
}

int ftdi_adapter_open(ftdi_context_t *ctx, 
                      int vendor, int product,
                      const char *serial,
                      int index, int interface)
{
    return ftdi_adapter_open_with_mode(ctx, vendor, product, serial, 
                                       index, interface, ADAPTER_MODE_MPSSE);
}

int ftdi_adapter_open_with_mode(ftdi_context_t *ctx, 
                                int vendor, int product,
                                const char *serial,
                                int index, int interface,
                                adapter_mode_t mode)
{
    if (!ctx) return -1;
    
    /* Only MPSSE mode is supported - ignore mode parameter */
    (void)mode;
    
    if (!ctx->mpsse) {
        snprintf(ctx->error, sizeof(ctx->error), "MPSSE context not created");
        LOG_ERROR("%s", ctx->error);
        return -1;
    }
    
    if (mpsse_adapter_open(ctx->mpsse, vendor, product, serial, index, interface) != 0) {
        snprintf(ctx->error, sizeof(ctx->error), "%s", mpsse_adapter_error(ctx->mpsse));
        return -1;
    }
    
    ctx->is_open = true;
    LOG_INFO("FTDI device opened (MPSSE mode - high speed)");
    return 0;
}

int ftdi_adapter_open_bus(ftdi_context_t *ctx, int bus, int device, int interface)
{
    (void)bus;
    (void)device;
    (void)interface;
    
    snprintf(ctx->error, sizeof(ctx->error), "Bus-based opening not yet implemented");
    LOG_ERROR("%s", ctx->error);
    return -1;
}

void ftdi_adapter_close(ftdi_context_t *ctx)
{
    if (!ctx || !ctx->is_open) return;
    
    if (ctx->mpsse) {
        mpsse_adapter_close(ctx->mpsse);
    }
    
    ctx->is_open = false;
    LOG_INFO("FTDI device closed");
}

bool ftdi_adapter_is_open(const ftdi_context_t *ctx)
{
    return ctx && ctx->is_open;
}

int ftdi_adapter_set_period(ftdi_context_t *ctx, unsigned int period_ns)
{
    if (!ctx || !ctx->is_open) return -1;
    
    /* Convert period to frequency */
    if (period_ns == 0) {
        period_ns = 100;  /* Default to 10MHz if period is 0 */
    }
    
    uint32_t frequency_hz = 1000000000 / period_ns;
    
    int actual_freq = mpsse_adapter_set_frequency(ctx->mpsse, frequency_hz);
    if (actual_freq < 0) return -1;
    
    int actual_period = 1000000000 / actual_freq;
    LOG_INFO("TCK set: requested=%uns, actual=%uns, freq=%dHz", 
             period_ns, actual_period, actual_freq);
    
    return actual_period;
}

int ftdi_adapter_set_frequency(ftdi_context_t *ctx, uint32_t frequency_hz)
{
    if (!ctx || frequency_hz == 0) return -1;
    
    int actual = mpsse_adapter_set_frequency(ctx->mpsse, frequency_hz);
    
    return (actual > 0) ? 0 : -1;
}

int ftdi_adapter_scan(ftdi_context_t *ctx,
                      const uint8_t *tms,
                      const uint8_t *tdi,
                      uint8_t *tdo,
                      int bits)
{
    if (!ctx || !ctx->is_open || !tms || !tdi || !tdo) return -1;
    
    return mpsse_adapter_scan(ctx->mpsse, tms, tdi, tdo, bits);
}

const char* ftdi_adapter_error(const ftdi_context_t *ctx)
{
    if (!ctx) return "NULL context";
    return ctx->error[0] ? ctx->error : "No error";
}

void ftdi_adapter_set_verbose(ftdi_context_t *ctx, int level)
{
    if (ctx) {
        ctx->verbose = level;
        if (ctx->mpsse) {
            mpsse_adapter_set_verbose(ctx->mpsse, level);
        }
    }
}

int ftdi_adapter_set_latency(ftdi_context_t *ctx, int latency_ms)
{
    (void)ctx;
    (void)latency_ms;
    
    /* Latency is handled internally by D2XX driver in MPSSE mode */
    LOG_DBG("Latency timer set to %dms (handled by D2XX)", latency_ms);
    return 0;
}
