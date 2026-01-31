/*
 * mpsse_adapter.c - MPSSE JTAG Adapter Implementation
 * XVC Server for Digilent HS2
 * 
 * Implements MPSSE (Multi-Protocol Synchronous Serial Engine) mode
 * for high-speed JTAG operations using D2XX driver.
 * 
 * Based on FTDI's jtag.c example and supports:
 * - FT232H (single channel, 60 MHz)
 * - FT2232H (dual channel, 30 MHz per channel)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <stdarg.h>
#include "mpsse_adapter.h"
#include "logging.h"

#define ARRAY_SIZE(x) (sizeof((x))/sizeof((x)[0]))

/* Internal MPSSE context structure */
struct mpsse_context_s {
    FT_HANDLE ft_handle;
    chip_type_t chip_type;
    uint32_t current_freq;
    uint32_t max_freq;
    int interface;
    bool is_open;
    int verbose;
    char error[256];
    uint8_t *tx_buffer;
    uint8_t *rx_buffer;
    size_t tx_buf_size;
    size_t tx_buf_pos;
};

/* Map FTDI device type to our chip type */
static chip_type_t identify_chip_type(FT_DEVICE ft_device)
{
    switch (ft_device) {
        case FT_DEVICE_232H:
            return CHIP_TYPE_FT232H;
        case FT_DEVICE_2232H:
            return CHIP_TYPE_FT2232H;
        case FT_DEVICE_4232H:
            return CHIP_TYPE_FT4232H;
        case FT_DEVICE_2232C:
            return CHIP_TYPE_FT2232C;
        case FT_DEVICE_232R:
            return CHIP_TYPE_FT232R;
        default:
            return CHIP_TYPE_UNKNOWN;
    }
}

static const char* chip_type_to_string(chip_type_t type)
{
    switch (type) {
        case CHIP_TYPE_FT232H:  return "FT232H";
        case CHIP_TYPE_FT2232H: return "FT2232H";
        case CHIP_TYPE_FT4232H: return "FT4232H";
        case CHIP_TYPE_FT2232C: return "FT2232C";
        case CHIP_TYPE_FT232R:  return "FT232R";
        default:                return "Unknown";
    }
}

static void set_error(mpsse_context_t *ctx, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(ctx->error, sizeof(ctx->error), fmt, args);
    va_end(args);
    LOG_ERROR("%s", ctx->error);
}

mpsse_context_t* mpsse_adapter_create(void)
{
    mpsse_context_t *ctx = calloc(1, sizeof(mpsse_context_t));
    if (!ctx) {
        return NULL;
    }
    
    /* Allocate buffers */
    ctx->tx_buf_size = MPSSE_BUFFER_SIZE;
    ctx->tx_buffer = malloc(ctx->tx_buf_size);
    ctx->rx_buffer = malloc(ctx->tx_buf_size);
    
    if (!ctx->tx_buffer || !ctx->rx_buffer) {
        free(ctx->tx_buffer);
        free(ctx->rx_buffer);
        free(ctx);
        return NULL;
    }
    
    ctx->ft_handle = NULL;
    ctx->chip_type = CHIP_TYPE_UNKNOWN;
    ctx->current_freq = 0;
    ctx->max_freq = FT232H_MAX_FREQ;
    ctx->interface = 0;
    ctx->is_open = false;
    ctx->verbose = 0;
    ctx->tx_buf_pos = 0;
    
    return ctx;
}

void mpsse_adapter_destroy(mpsse_context_t *ctx)
{
    if (!ctx) return;
    
    if (ctx->is_open) {
        mpsse_adapter_close(ctx);
    }
    
    free(ctx->tx_buffer);
    free(ctx->rx_buffer);
    free(ctx);
}

static int flush_tx_buffer(mpsse_context_t *ctx)
{
    if (ctx->tx_buf_pos == 0) {
        return 0;
    }
    
    DWORD bytes_written = 0;
    FT_STATUS status = FT_Write(ctx->ft_handle, 
                                ctx->tx_buffer, 
                                (DWORD)ctx->tx_buf_pos, 
                                &bytes_written);
    
    if (status != FT_OK) {
        set_error(ctx, "FT_Write failed: %d", status);
        return -1;
    }
    
    if (bytes_written != ctx->tx_buf_pos) {
        set_error(ctx, "FT_Write partial: %d/%d", 
                 (int)bytes_written, (int)ctx->tx_buf_pos);
        return -1;
    }
    
    ctx->tx_buf_pos = 0;
    return 0;
}

static int write_command(mpsse_context_t *ctx, const uint8_t *data, size_t len)
{
    size_t remaining = len;
    size_t offset = 0;
    
    while (remaining > 0) {
        size_t space = ctx->tx_buf_size - ctx->tx_buf_pos;
        size_t to_copy = (remaining < space) ? remaining : space;
        
        memcpy(ctx->tx_buffer + ctx->tx_buf_pos, data + offset, to_copy);
        ctx->tx_buf_pos += to_copy;
        offset += to_copy;
        remaining -= to_copy;
        
        if (ctx->tx_buf_pos >= ctx->tx_buf_size) {
            if (flush_tx_buffer(ctx) != 0) {
                return -1;
            }
        }
    }
    
    return 0;
}

static int read_response(mpsse_context_t *ctx, uint8_t *buffer, DWORD bytes_to_read)
{
    DWORD bytes_available = 0;
    DWORD bytes_read = 0;
    DWORD total_read = 0;
    FT_STATUS status;
    int retries = 100;
    
    while (total_read < bytes_to_read && retries > 0) {
        status = FT_GetQueueStatus(ctx->ft_handle, &bytes_available);
        if (status != FT_OK) {
            set_error(ctx, "FT_GetQueueStatus failed: %d", status);
            return -1;
        }
        
        if (bytes_available > 0) {
            DWORD to_read = bytes_available;
            if (to_read > (bytes_to_read - total_read)) {
                to_read = bytes_to_read - total_read;
            }
            
            status = FT_Read(ctx->ft_handle, buffer + total_read, to_read, &bytes_read);
            if (status != FT_OK) {
                set_error(ctx, "FT_Read failed: %d", status);
                return -1;
            }
            
            total_read += bytes_read;
        }
        
        retries--;
    }
    
    if (total_read != bytes_to_read) {
        set_error(ctx, "FT_Read incomplete: %d/%d", (int)total_read, (int)bytes_to_read);
        return -1;
    }
    
    return 0;
}

int mpsse_adapter_open(mpsse_context_t *ctx, 
                       int vendor, int product,
                       const char *serial,
                       int index, int interface)
{
    FT_STATUS status;
    FT_DEVICE device_type;
    DWORD device_id = 0;
    char serial_number[64] = {0};
    char description[256] = {0};
    
    if (!ctx) return -1;
    if (ctx->is_open) {
        set_error(ctx, "Device already open");
        return -1;
    }
    
    (void)vendor;
    (void)product;
    
    /* Use FT_OpenEx for serial number, FT_Open for index */
    if (serial && serial[0] != '\0') {
        status = FT_OpenEx((PVOID)serial, FT_OPEN_BY_SERIAL_NUMBER, &ctx->ft_handle);
    } else {
        status = FT_Open((int)index, &ctx->ft_handle);
    }
    if (status != FT_OK) {
        set_error(ctx, "FT_Open%s failed: %d (index=%d, serial=%s)", 
                 (serial && serial[0] != '\0') ? "Ex" : "",
                 status, index, serial ? serial : "any");
        return -1;
    }
    
    /* Get device information */
    status = FT_GetDeviceInfo(ctx->ft_handle, &device_type, &device_id,
                             serial_number, description, NULL);
    if (status != FT_OK) {
        set_error(ctx, "FT_GetDeviceInfo failed: %d", status);
        FT_Close(ctx->ft_handle);
        ctx->ft_handle = NULL;
        return -1;
    }
    
    ctx->chip_type = identify_chip_type(device_type);
    
    /* Set interface for multi-port devices */
    ctx->interface = interface;
    if (ctx->chip_type == CHIP_TYPE_FT2232H || 
        ctx->chip_type == CHIP_TYPE_FT4232H) {
        status = FT_SetBitMode(ctx->ft_handle, 0x00, FT_BITMODE_RESET);
        if (status != FT_OK) {
            set_error(ctx, "FT_SetBitMode reset failed: %d", status);
            FT_Close(ctx->ft_handle);
            ctx->ft_handle = NULL;
            return -1;
        }
    }
    
    /* Reset device */
    status = FT_ResetDevice(ctx->ft_handle);
    if (status != FT_OK) {
        set_error(ctx, "FT_ResetDevice failed: %d", status);
        FT_Close(ctx->ft_handle);
        ctx->ft_handle = NULL;
        return -1;
    }
    
    /* Set latency timer */
    status = FT_SetLatencyTimer(ctx->ft_handle, MPSSE_LATENCY_MS);
    if (status != FT_OK) {
        set_error(ctx, "FT_SetLatencyTimer failed: %d", status);
        FT_Close(ctx->ft_handle);
        ctx->ft_handle = NULL;
        return -1;
    }
    
    /* Set timeouts */
    status = FT_SetTimeouts(ctx->ft_handle, MPSSE_TIMEOUT_MS, MPSSE_TIMEOUT_MS);
    if (status != FT_OK) {
        set_error(ctx, "FT_SetTimeouts failed: %d", status);
        FT_Close(ctx->ft_handle);
        ctx->ft_handle = NULL;
        return -1;
    }
    
    /* Reset bitmode before enabling MPSSE */
    status = FT_SetBitMode(ctx->ft_handle, 0x00, FT_BITMODE_RESET);
    if (status != FT_OK) {
        set_error(ctx, "FT_SetBitMode reset failed: %d", status);
        FT_Close(ctx->ft_handle);
        ctx->ft_handle = NULL;
        return -1;
    }
    
    /* Enable MPSSE mode with GPIO value = 0x00 (NOT the direction!) */
    status = FT_SetBitMode(ctx->ft_handle, 0x00, FT_BITMODE_MPSSE);
    if (status != FT_OK) {
        set_error(ctx, "FT_SetBitMode MPSSE failed: %d", status);
        FT_Close(ctx->ft_handle);
        ctx->ft_handle = NULL;
        return -1;
    }
    
    /* Purge buffers */
    status = FT_Purge(ctx->ft_handle, FT_PURGE_RX | FT_PURGE_TX);
    if (status != FT_OK) {
        set_error(ctx, "FT_Purge failed: %d", status);
        FT_Close(ctx->ft_handle);
        ctx->ft_handle = NULL;
        return -1;
    }
    
    /* Set maximum frequency based on chip type */
    switch (ctx->chip_type) {
        case CHIP_TYPE_FT232H:
            ctx->max_freq = FT232H_MAX_FREQ;
            break;
        case CHIP_TYPE_FT2232H:
        case CHIP_TYPE_FT4232H:
            ctx->max_freq = FT2232H_MAX_FREQ;
            break;
        default:
            ctx->max_freq = FT2232H_MAX_FREQ;
            break;
    }
    
    /* Initialize GPIO - TMS high, TDO input */
    uint8_t gpio_init[] = {
        MPSSE_SET_GPIO_LOW, JTAG_GPIO_LOW_INIT, JTAG_GPIO_LOW_DIR,
        MPSSE_SET_GPIO_HIGH, JTAG_GPIO_HIGH_INIT, JTAG_GPIO_HIGH_DIR
    };
    
    if (write_command(ctx, gpio_init, sizeof(gpio_init)) != 0) {
        FT_Close(ctx->ft_handle);
        ctx->ft_handle = NULL;
        return -1;
    }
    
    if (flush_tx_buffer(ctx) != 0) {
        FT_Close(ctx->ft_handle);
        ctx->ft_handle = NULL;
        return -1;
    }
    
    /* Set default frequency */
    mpsse_adapter_set_frequency(ctx, MPSSE_DEFAULT_FREQ);
    
    ctx->is_open = true;
    LOG_INFO("MPSSE device opened: %s (interface %d, max_freq=%u Hz)", 
             chip_type_to_string(ctx->chip_type), interface, ctx->max_freq);
    
    return 0;
}

void mpsse_adapter_close(mpsse_context_t *ctx)
{
    if (!ctx || !ctx->is_open) return;
    
    flush_tx_buffer(ctx);
    
    /* Reset bit mode */
    if (ctx->ft_handle) {
        FT_SetBitMode(ctx->ft_handle, 0x00, FT_BITMODE_RESET);
        FT_Close(ctx->ft_handle);
        ctx->ft_handle = NULL;
    }
    
    ctx->is_open = false;
    LOG_INFO("MPSSE device closed");
}

bool mpsse_adapter_is_open(const mpsse_context_t *ctx)
{
    return ctx && ctx->is_open;
}

chip_type_t mpsse_adapter_get_chip_type(const mpsse_context_t *ctx)
{
    if (!ctx) return CHIP_TYPE_UNKNOWN;
    return ctx->chip_type;
}

const char* mpsse_adapter_get_chip_name(const mpsse_context_t *ctx)
{
    if (!ctx) return "NULL";
    return chip_type_to_string(ctx->chip_type);
}

int mpsse_adapter_set_frequency(mpsse_context_t *ctx, uint32_t frequency_hz)
{
    if (!ctx || !ctx->is_open) return -1;
    
    /* Clamp frequency */
    if (frequency_hz > ctx->max_freq) {
        frequency_hz = ctx->max_freq;
    }
    if (frequency_hz < MPSSE_MIN_FREQ) {
        frequency_hz = MPSSE_MIN_FREQ;
    }
    
    /* Calculate divisor for 60MHz base clock
     * actual_freq = 60MHz / (1 + divisor)
     * divisor = (60MHz / freq) - 1
     */
    uint16_t divisor = (uint16_t)((MPSSE_BASE_CLK / frequency_hz) - 1);
    
    /* Enable clock divide */
    uint8_t clk_div[] = {
        MPSSE_SET_CLK_DIV, 
        (uint8_t)(divisor & 0xFF), 
        (uint8_t)((divisor >> 8) & 0xFF)
    };
    
    if (write_command(ctx, clk_div, sizeof(clk_div)) != 0) {
        return -1;
    }
    
    if (flush_tx_buffer(ctx) != 0) {
        return -1;
    }
    
    /* Calculate actual frequency */
    ctx->current_freq = MPSSE_BASE_CLK / (1 + divisor);
    
    LOG_DBG("Frequency set to %u Hz (requested %u Hz, divisor=%u)", 
            ctx->current_freq, frequency_hz, divisor);
    
    return ctx->current_freq;
}

uint32_t mpsse_adapter_get_frequency(const mpsse_context_t *ctx)
{
    if (!ctx) return 0;
    return ctx->current_freq;
}

int mpsse_adapter_scan(mpsse_context_t *ctx,
                       const uint8_t *tms,
                       const uint8_t *tdi,
                       uint8_t *tdo,
                       int bits)
{
    if (!ctx || !ctx->is_open || !tms || !tdi || !tdo) return -1;
    if (bits <= 0) return 0;
    
    /* Use TinyXVC approach:
     * - For regular bits (TMS=0): Use command 0x3B with TDI in bit 0
     * - For TMS control: Use command 0x4B with TDI in bit 7, TMS in bits 6-0
     * - The last bit always uses the TMS command to ensure proper state transition
     */
    
    for (int bit_idx = 0; bit_idx < bits - 1; bit_idx++) {
        int src_byte = bit_idx / 8;
        int src_bit = bit_idx % 8;
        
        /* Extract single TDI bit (LSB first = bit 0 is first) */
        uint8_t bit_tdi = (tdi[src_byte] >> src_bit) & 0x01;
        
        /* Extract single TMS bit (LSB first = bit 0 is first) */
        uint8_t bit_tms = (tms[src_byte] >> src_bit) & 0x01;
        
        if (bit_tms) {
            /* Use command 0x4B for TMS=1 bits: TDI in bit 7, TMS in bit 0 */
            uint8_t cmd[] = {
                0x4B,                    /* TMS command */
                0x00, 0x00,              /* Length: 1 bit (0 = 1-1) */
                (bit_tdi << 7) | 0x01    /* Bit 7 = TDI, Bit 0 = TMS=1 */
            };
            if (write_command(ctx, cmd, sizeof(cmd)) != 0) {
                return -1;
            }
        } else {
            /* Use command 0x3B for regular bits: TDI in bit 0 */
            uint8_t cmd[] = {
                0x3B,                    /* Clock bits out/in LSB first */
                0x00, 0x00,              /* Length: 1 bit (0 = 1-1) */
                bit_tdi                  /* Bit 0 = TDI */
            };
            if (write_command(ctx, cmd, sizeof(cmd)) != 0) {
                return -1;
            }
        }
    }
    
    /* Handle last bit with TMS command to ensure proper state transition */
    int last_idx = bits - 1;
    int src_byte = last_idx / 8;
    int src_bit = last_idx % 8;
    uint8_t bit_tdi = (tdi[src_byte] >> src_bit) & 0x01;
    uint8_t bit_tms = (tms[src_byte] >> src_bit) & 0x01;
    
    /* Always use TMS command for the last bit: TDI in bit 7, TMS value in bit 0 */
    uint8_t cmd_last[] = {
        0x4B,                            /* TMS command */
        0x00, 0x00,                      /* Length: 1 bit (0 = 1-1) */
        (bit_tdi << 7) | bit_tms         /* Bit 7 = TDI, Bit 0 = TMS value */
    };
    if (write_command(ctx, cmd_last, sizeof(cmd_last)) != 0) {
        return -1;
    }
    
    /* Send immediate command to ensure all data is clocked out */
    uint8_t send_imm = MPSSE_SEND_IMMEDIATE;
    if (write_command(ctx, &send_imm, 1) != 0) {
        return -1;
    }
    
    if (flush_tx_buffer(ctx) != 0) {
        return -1;
    }
    
    /* Read response - FTDI packs bits into bytes (LSB first) */
    DWORD bytes_to_read = (bits + 7) / 8;
    
    if (read_response(ctx, tdo, bytes_to_read) != 0) {
        return -1;
    }
    
    return 0;
}

void mpsse_adapter_set_verbose(mpsse_context_t *ctx, int level)
{
    if (ctx) {
        ctx->verbose = level;
    }
}

const char* mpsse_adapter_error(const mpsse_context_t *ctx)
{
    if (!ctx) return "NULL context";
    return ctx->error[0] ? ctx->error : "No error";
}

int mpsse_adapter_set_latency(mpsse_context_t *ctx, int latency_ms)
{
    if (!ctx || !ctx->is_open) return -1;
    
    if (latency_ms < 1 || latency_ms > 255) {
        set_error(ctx, "Invalid latency: %d (must be 1-255)", latency_ms);
        return -1;
    }
    
    FT_STATUS status = FT_SetLatencyTimer(ctx->ft_handle, (UCHAR)latency_ms);
    if (status != FT_OK) {
        set_error(ctx, "FT_SetLatencyTimer failed: %d", status);
        return -1;
    }
    
    LOG_DBG("Latency timer set to %dms", latency_ms);
    return 0;
}

int mpsse_adapter_purge(mpsse_context_t *ctx)
{
    if (!ctx || !ctx->is_open) return -1;
    
    FT_STATUS status = FT_Purge(ctx->ft_handle, FT_PURGE_RX | FT_PURGE_TX);
    if (status != FT_OK) {
        set_error(ctx, "FT_Purge failed: %d", status);
        return -1;
    }
    
    return 0;
}
