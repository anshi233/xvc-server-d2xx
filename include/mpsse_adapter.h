/*
 * mpsse_adapter.h - FTDI MPSSE Adapter Layer (High-Speed JTAG)
 * XVC Server for Digilent HS2 using D2XX driver
 * 
 * Based on TinyXVC implementation by Sergey Guralnik.
 * Adapted for D2XX driver.
 */

#ifndef XVC_MPSSE_ADAPTER_H
#define XVC_MPSSE_ADAPTER_H

#include <stdint.h>
#include <stdbool.h>

/* MPSSE context (opaque) */
typedef struct mpsse_context_s mpsse_context_t;

/**
 * Create MPSSE context
 * @return New context or NULL on error
 */
mpsse_context_t* mpsse_adapter_create(void);

/**
 * Destroy MPSSE context
 */
void mpsse_adapter_destroy(mpsse_context_t *ctx);

/**
 * Open MPSSE device using D2XX driver
 * @param ctx MPSSE context
 * @param vendor Vendor ID (ignored, use -1 for default 0x0403)
 * @param product Product ID (ignored, use -1 for default 0x6010)
 * @param serial Serial number (NULL for any)
 * @param index Device index (0 for first)
 * @param interface Interface number (0-3, for FT2232H/FT4232H)
 * @return 0 on success, -1 on error
 */
int mpsse_adapter_open(mpsse_context_t *ctx, int vendor, int product, 
                       const char *serial, int index, int interface);

/**
 * Close MPSSE device
 */
void mpsse_adapter_close(mpsse_context_t *ctx);

/**
 * Set TCK frequency
 * @param ctx MPSSE context
 * @param frequency_hz Frequency in Hz
 * @return Actual frequency in Hz, or -1 on error
 */
int mpsse_adapter_set_frequency(mpsse_context_t *ctx, uint32_t frequency_hz);

/**
 * Perform JTAG scan operation
 * @param ctx MPSSE context
 * @param tms TMS data bytes
 * @param tdi TDI data bytes
 * @param tdo Output TDO data bytes
 * @param bits Number of bits to scan
 * @return 0 on success, -1 on error
 */
int mpsse_adapter_scan(mpsse_context_t *ctx, const uint8_t *tms,
                       const uint8_t *tdi, uint8_t *tdo, int bits);

/**
 * Flush any pending data
 * @param ctx MPSSE context
 * @return 0 on success, -1 on error
 */
int mpsse_adapter_flush(mpsse_context_t *ctx);

/**
 * Get last error message
 * @param ctx MPSSE context
 * @return Error string
 */
const char* mpsse_adapter_error(const mpsse_context_t *ctx);

/**
 * Set verbosity level
 * @param ctx MPSSE context
 * @param level Verbosity level (0=none, 1=error, 2=warn, 3=info, 4=debug)
 */
void mpsse_adapter_set_verbose(mpsse_context_t *ctx, int level);

/**
 * Set MPSSE command dump file for debugging
 * @param ctx MPSSE context
 * @param path Path to dump file
 * @return 0 on success, -1 on error
 */
int mpsse_adapter_set_dump_file(mpsse_context_t *ctx, const char *path);

#endif /* XVC_MPSSE_ADAPTER_H */
