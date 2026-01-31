/*
 * mpsse_adapter.h - MPSSE JTAG Adapter for FTDI chips
 * XVC Server for Digilent HS2
 * 
 * Implements MPSSE (Multi-Protocol Synchronous Serial Engine) mode
 * for high-speed JTAG operations using D2XX driver.
 * 
 * Supported chips:
 * - FT232H (single channel, 60 MHz)
 * - FT2232H (dual channel, 30 MHz per channel)
 */

#ifndef MPSSE_ADAPTER_H
#define MPSSE_ADAPTER_H

#include <stdint.h>
#include <stdbool.h>
#include "../vendor/d2xx/arm64/ftd2xx.h"

/* MPSSE command opcodes */
#define MPSSE_WRITE_TMS         0x01  
#define MPSSE_WRITE_BITS_TMS    0x02
#define MPSSE_READ_BITS_TMS     0x03
#define MPSSE_WRITE_BITS        0x10
#define MPSSE_READ_BITS         0x20
#define MPSSE_WRITE_READ_BITS   0x30
#define MPSSE_SET_GPIO_LOW      0x80
#define MPSSE_SET_GPIO_HIGH     0x82
#define MPSSE_GET_GPIO_LOW      0x81
#define MPSSE_GET_GPIO_HIGH     0x83
#define MPSSE_LOOPBACK_START    0x84
#define MPSSE_LOOPBACK_END      0x85
#define MPSSE_SET_CLK_DIV       0x86
#define MPSSE_SEND_IMMEDIATE    0x87
#define MPSSE_WAIT_ON_GPIO_HIGH 0x88
#define MPSSE_WAIT_ON_GPIO_LOW  0x89
#define MPSSE_DISABLE_CLK_DIV   0x8A
#define MPSSE_ENABLE_CLK_DIV    0x8B

/* JTAG GPIO configuration */
#define JTAG_TCK                0x01
#define JTAG_TDI                0x02
#define JTAG_TDO                0x04
#define JTAG_TMS                0x08
#define JTAG_GPIO_MASK          0x0B

/* Default GPIO values */
#define JTAG_GPIO_LOW_INIT      0x08
#define JTAG_GPIO_LOW_DIR       0x0B
#define JTAG_GPIO_HIGH_INIT     0x00
#define JTAG_GPIO_HIGH_DIR      0x00

/* Frequency limits for different chips */
#define FT232H_MAX_FREQ         60000000
#define FT2232H_MAX_FREQ        30000000
#define MPSSE_BASE_CLK          60000000

/* Default settings */
#define MPSSE_DEFAULT_FREQ      6000000
#define MPSSE_MIN_FREQ          500
#define MPSSE_LATENCY_MS        2
#define MPSSE_TIMEOUT_MS        3000
#define MPSSE_BUFFER_SIZE       (64 * 1024)

/* Device types */
typedef enum {
    CHIP_TYPE_UNKNOWN = 0,
    CHIP_TYPE_FT232H,
    CHIP_TYPE_FT2232H,
    CHIP_TYPE_FT4232H,
    CHIP_TYPE_FT2232C,
    CHIP_TYPE_FT232R
} chip_type_t;

/* MPSSE context (opaque) */
typedef struct mpsse_context_s mpsse_context_t;

/* MPSSE Adapter API */

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
 * Open FTDI device
 * @param ctx MPSSE context
 * @param vendor Vendor ID (-1 for default 0x0403)
 * @param product Product ID (-1 for default 0x6010/0x6014)
 * @param serial Serial number (NULL for any)
 * @param index Device index (0 for first)
 * @param interface Interface number (0-1 for FT2232H, 0 for FT232H)
 * @return 0 on success, -1 on error
 */
int mpsse_adapter_open(mpsse_context_t *ctx, 
                       int vendor, int product,
                       const char *serial,
                       int index, int interface);

/**
 * Close FTDI device
 */
void mpsse_adapter_close(mpsse_context_t *ctx);

/**
 * Check if device is open
 */
bool mpsse_adapter_is_open(const mpsse_context_t *ctx);

/**
 * Get chip type
 */
chip_type_t mpsse_adapter_get_chip_type(const mpsse_context_t *ctx);

/**
 * Get chip type name
 */
const char* mpsse_adapter_get_chip_name(const mpsse_context_t *ctx);

/**
 * Set TCK frequency
 * @param ctx MPSSE context
 * @param frequency_hz Frequency in Hz
 * @return Actual frequency in Hz, or -1 on error
 */
int mpsse_adapter_set_frequency(mpsse_context_t *ctx, uint32_t frequency_hz);

/**
 * Get current frequency
 */
uint32_t mpsse_adapter_get_frequency(const mpsse_context_t *ctx);

/**
 * Perform JTAG scan operation
 * @param ctx MPSSE context
 * @param tms TMS data bytes
 * @param tdi TDI data bytes
 * @param tdo Output TDO data bytes
 * @param bits Number of bits to scan
 * @return 0 on success, -1 on error
 */
int mpsse_adapter_scan(mpsse_context_t *ctx,
                       const uint8_t *tms,
                       const uint8_t *tdi,
                       uint8_t *tdo,
                       int bits);

/**
 * Set verbosity level
 */
void mpsse_adapter_set_verbose(mpsse_context_t *ctx, int level);

/**
 * Get last error message
 */
const char* mpsse_adapter_error(const mpsse_context_t *ctx);

/**
 * Set latency timer
 * @param ctx MPSSE context
 * @param latency_ms Latency in milliseconds (1-255)
 * @return 0 on success, -1 on error
 */
int mpsse_adapter_set_latency(mpsse_context_t *ctx, int latency_ms);

/**
 * Purge USB buffers
 * @return 0 on success, -1 on error
 */
int mpsse_adapter_purge(mpsse_context_t *ctx);

#endif /* MPSSE_ADAPTER_H */
