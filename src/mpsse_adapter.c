/*
 * mpsse_adapter.c - FTDI MPSSE Adapter Layer (High-Speed JTAG)
 * XVC Server for Digilent HS2 using D2XX driver
 * 
 * Based on TinyXVC implementation by Sergey Guralnik.
 * Adapted for D2XX driver with MPSSE commands.
 * 
 * This implementation follows the Xilinx XVC protocol which sends TMS and TDI
 * vectors together and expects TDO data back.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include "ftd2xx.h"
#include "mpsse_adapter.h"
#include "logging.h"

/* MPSSE Commands */
#define OP_SET_DBUS_LOBYTE         0x80
#define OP_SET_DBUS_HIBYTE         0x82
#define OP_GET_DBUS_LOBYTE         0x81
#define OP_GET_DBUS_HIBYTE         0x83
#define OP_SET_TCK_DIVISOR         0x86
#define OP_SEND_IMMEDIATE          0x87
#define OP_DISABLE_CLK_DIVIDE_BY_5  0x8A
#define OP_ENABLE_CLK_DIVIDE_BY_5   0x8B
#define OP_DISABLE_3PHASE_CLOCK     0x8D
#define OP_LOOPBACK_OFF            0x85
#define OP_LOOPBACK_ON             0x84

/* Data shift commands - LSB first, negative clock edge for TDI, positive for TDO */
#define OP_SHIFT_WR_TMS_FLAG       0x40
#define OP_SHIFT_WR_TDI_FLAG       0x10
#define OP_SHIFT_RD_TDO_FLAG       0x20
#define OP_SHIFT_LSB_FIRST_FLAG    0x08
#define OP_SHIFT_BITMODE_FLAG      0x02
#define OP_SHIFT_WR_FALLING_FLAG   0x01

/* CLK_DATA_BYTES commands */
#define OP_CLK_DATA_BYTES_OUT_NEG  0x19   /* Clock data bytes out on -ve edge */
#define OP_CLK_DATA_BITS_OUT_NEG   0x1B   /* Clock data bits out on -ve edge */
#define OP_CLK_DATA_BYTES_IN_POS   0x28   /* Clock data bytes in on +ve edge */
#define OP_CLK_DATA_BITS_IN_POS    0x2A   /* Clock data bits in on +ve edge */
#define OP_CLK_DATA_BYTES_OUT_NEG_IN_POS 0x39  /* Write on -ve, read on +ve */
#define OP_CLK_DATA_BITS_OUT_NEG_IN_POS  0x3B  /* Write bits on -ve, read on +ve */

/* TMS commands */
#define OP_CLK_TMS_NO_READ         0x4B   /* Clock TMS out, no read */
#define OP_CLK_TMS_READ            0x6B   /* Clock TMS out, read TDO */

/* Latency timer removed - not needed for high-speed bulk transfers */
#define MPSSE_DEFAULT_CLOCK    30000000
#define MPSSE_MAX_FREQUENCY    30000000
#define MPSSE_MIN_FREQUENCY       457

/* JTAG state machine states */
enum jtag_state {
    TEST_LOGIC_RESET,
    RUN_TEST_IDLE,
    SELECT_DR_SCAN,
    CAPTURE_DR,
    SHIFT_DR,
    EXIT_1_DR,
    PAUSE_DR,
    EXIT_2_DR,
    UPDATE_DR,
    SELECT_IR_SCAN,
    CAPTURE_IR,
    SHIFT_IR,
    EXIT_1_IR,
    PAUSE_IR,
    EXIT_2_IR,
    UPDATE_IR
};

/* Memory pool for observers - increased for 64KB transfers */
#define MEMORY_POOL_SIZE 65536

struct memory_pool {
    uint8_t buffer[MEMORY_POOL_SIZE * 256];
    size_t used;
};

/* RX observer for handling read data */
typedef void (*rx_observer_fn)(const uint8_t *rxData, void *extra);

struct rx_observer_node {
    struct rx_observer_node *next;
    rx_observer_fn fn;
    const uint8_t *data;
    void *extra;
};

/* MPSSE buffer management */
struct mpsse_buffer {
    uint8_t *tx_buffer;
    int tx_num_bytes;
    int max_tx_buffer_bytes;
    uint8_t *rx_buffer;
    int rx_num_bytes;
    int max_rx_buffer_bytes;
    struct rx_observer_node *rx_observer_first;
    struct rx_observer_node *rx_observer_last;
};

/* Main context structure */
struct mpsse_context_s {
    FT_HANDLE ft_handle;
    struct mpsse_buffer buffer;
    enum jtag_state state;
    bool last_tdi;
    bool is_open;
    int verbose;
    int chip_buffer_size;
    struct memory_pool observer_pool;
    struct timeval last_flush_time;
    int total_flushes;
    int failed_flushes;
    char error[256];
    FILE *dump_file;
    const uint8_t *current_tms;
    const uint8_t *current_tdi;
    int current_bit_offset;
};

static inline int min(int a, int b)
{
    return a < b ? a : b;
}

static inline void* pool_alloc(struct memory_pool *pool, size_t size)
{
    if (pool->used + size > sizeof(pool->buffer)) {
        return NULL;
    }
    void *ptr = pool->buffer + pool->used;
    pool->used += size;
    return ptr;
}

static inline void pool_reset(struct memory_pool *pool)
{
    pool->used = 0;
}

static inline long time_diff_ms(struct timeval *start, struct timeval *end)
{
    return (end->tv_sec - start->tv_sec) * 1000 + 
           (end->tv_usec - start->tv_usec) / 1000;
}

static inline int get_bit(const uint8_t *p, int idx)
{
    return !!(p[idx / 8] & (1 << (idx % 8)));
}

static inline void set_bit(uint8_t *p, int idx, bool bit)
{
    uint8_t *octet = p + idx / 8;
    if (bit) *octet |= 1 << (idx % 8);
    else *octet &= ~(1 << (idx % 8));
}

/* Copy bits from source to destination */
static void copy_bits(const uint8_t *src, int fromIdx, uint8_t *dst, int toIdx, int numBits, bool duplicateLastBit)
{
    for (int i = 0; i < numBits; i++) {
        set_bit(dst, toIdx++, get_bit(src, fromIdx++));
    }
    if (duplicateLastBit) {
        set_bit(dst, toIdx, get_bit(src, fromIdx - 1));
    }
}

/* Format TMS/TDI data for logging */
static void format_tms_tdi_hex(mpsse_context_t *ctx, int bit_offset, int num_bits, 
                                char *buf, size_t buf_size)
{
    if (!ctx->current_tms || !ctx->current_tdi || num_bits <= 0) {
        snprintf(buf, buf_size, "N/A");
        return;
    }
    
    int start_byte = bit_offset / 8;
    int end_bit = bit_offset + num_bits - 1;
    int end_byte = end_bit / 8;
    int num_bytes = end_byte - start_byte + 1;
    
    buf[0] = '\0';
    size_t offset = 0;
    
    for (int i = 0; i < num_bytes && offset < buf_size - 10; i++) {
        int byte_idx = start_byte + i;
        offset += snprintf(buf + offset, buf_size - offset, 
                          "%s%02X%02X", i > 0 ? " " : "",
                          ctx->current_tms[byte_idx],
                          ctx->current_tdi[byte_idx]);
    }
}

mpsse_context_t* mpsse_adapter_create(void)
{
    mpsse_context_t *ctx = calloc(1, sizeof(mpsse_context_t));
    if (!ctx) return NULL;

    /* Initial buffer allocation with large size for 64KB transfers */
    ctx->buffer.max_tx_buffer_bytes = 3 * 65536;
    ctx->buffer.max_rx_buffer_bytes = 65536;

    ctx->buffer.tx_buffer = malloc(ctx->buffer.max_tx_buffer_bytes);
    ctx->buffer.rx_buffer = malloc(ctx->buffer.max_rx_buffer_bytes);

    if (!ctx->buffer.tx_buffer || !ctx->buffer.rx_buffer) {
        free(ctx->buffer.tx_buffer);
        free(ctx->buffer.rx_buffer);
        free(ctx);
        return NULL;
    }

    ctx->state = TEST_LOGIC_RESET;
    ctx->last_tdi = 0;
    ctx->chip_buffer_size = 65536;  /* Experiment: use 64KB for large transfers */
    pool_reset(&ctx->observer_pool);
    gettimeofday(&ctx->last_flush_time, NULL);
    ctx->total_flushes = 0;
    ctx->failed_flushes = 0;
    ctx->ft_handle = NULL;
    ctx->is_open = false;

    return ctx;
}

void mpsse_adapter_destroy(mpsse_context_t *ctx)
{
    if (!ctx) return;
    
    if (ctx->is_open) {
        mpsse_adapter_close(ctx);
    }
    
    if (ctx->dump_file) {
        fclose(ctx->dump_file);
        ctx->dump_file = NULL;
    }
    
    free(ctx->buffer.tx_buffer);
    free(ctx->buffer.rx_buffer);
    
    free(ctx);
}

int mpsse_adapter_set_dump_file(mpsse_context_t *ctx, const char *path)
{
    if (!ctx || !path) return -1;
    
    if (ctx->dump_file) {
        fclose(ctx->dump_file);
        ctx->dump_file = NULL;
    }
    
    ctx->dump_file = fopen(path, "w");
    if (!ctx->dump_file) {
        snprintf(ctx->error, sizeof(ctx->error), 
                 "Failed to open dump file: %s", path);
        return -1;
    }
    
    setbuf(ctx->dump_file, NULL);
    fprintf(ctx->dump_file, "# MPSSE command dump started\n");
    fprintf(ctx->dump_file, "# Format: INPUT: <TMS TDI bytes> followed by MPSSE command bytes\n");
    fflush(ctx->dump_file);
    return 0;
}

/* JTAG state machine transition */
static enum jtag_state next_state(enum jtag_state curState, bool tmsHigh)
{
    switch (curState) {
        case TEST_LOGIC_RESET: return tmsHigh ? TEST_LOGIC_RESET : RUN_TEST_IDLE;
        case RUN_TEST_IDLE: return tmsHigh ? SELECT_DR_SCAN : RUN_TEST_IDLE;
        case SELECT_DR_SCAN: return tmsHigh ? SELECT_IR_SCAN : CAPTURE_DR;
        case CAPTURE_DR: return tmsHigh ? EXIT_1_DR : SHIFT_DR;
        case SHIFT_DR: return tmsHigh ? EXIT_1_DR : SHIFT_DR;
        case EXIT_1_DR: return tmsHigh ? UPDATE_DR : PAUSE_DR;
        case PAUSE_DR: return tmsHigh ? EXIT_2_DR : PAUSE_DR;
        case EXIT_2_DR: return tmsHigh ? UPDATE_DR : SHIFT_DR;
        case UPDATE_DR: return tmsHigh ? SELECT_DR_SCAN : RUN_TEST_IDLE;
        case SELECT_IR_SCAN: return tmsHigh ? TEST_LOGIC_RESET : CAPTURE_IR;
        case CAPTURE_IR: return tmsHigh ? EXIT_1_IR : SHIFT_IR;
        case SHIFT_IR: return tmsHigh ? EXIT_1_IR : SHIFT_IR;
        case EXIT_1_IR: return tmsHigh ? UPDATE_IR : PAUSE_IR;
        case PAUSE_IR: return tmsHigh ? EXIT_2_IR : PAUSE_IR;
        case EXIT_2_IR: return tmsHigh ? UPDATE_IR : SHIFT_IR;
        case UPDATE_IR: return tmsHigh ? SELECT_DR_SCAN : RUN_TEST_IDLE;
    }
    return RUN_TEST_IDLE;
}

/* Flush pending data to device */
static int mpsse_buffer_flush(mpsse_context_t *ctx)
{
    struct mpsse_buffer *b = &ctx->buffer;
    FT_STATUS ftStatus;
    DWORD bytesWritten = 0;
    
    if (b->tx_num_bytes > 0) {
        /* OPTIMIZATION: Only compute timing stats at TRACE level */
        LOG_TRACE("Flushing TX buffer: %d bytes", b->tx_num_bytes);
        
        if (ctx->dump_file) {
            char tms_tdi_buf[2048];
            int estimated_bits = (b->tx_num_bytes + 1) / 2 * 8;
            format_tms_tdi_hex(ctx, ctx->current_bit_offset, estimated_bits, 
                              tms_tdi_buf, sizeof(tms_tdi_buf));
            fprintf(ctx->dump_file, "INPUT: %s\n", tms_tdi_buf);
            
            for (int i = 0; i < b->tx_num_bytes; i++) {
                fprintf(ctx->dump_file, "%02X", b->tx_buffer[i]);
                if (i < b->tx_num_bytes - 1) {
                    fprintf(ctx->dump_file, " ");
                }
            }
            fprintf(ctx->dump_file, "\n");
            fflush(ctx->dump_file);
        }
        
        ftStatus = FT_Write(ctx->ft_handle, b->tx_buffer, b->tx_num_bytes, &bytesWritten);
        ctx->current_bit_offset += (b->tx_num_bytes + 1) / 2 * 8;
        ctx->total_flushes++;
        
        if (ftStatus != FT_OK) {
            ctx->failed_flushes++;
            LOG_ERROR("USB write failed: status=%d, requested=%d bytes", 
                      (int)ftStatus, b->tx_num_bytes);
            snprintf(ctx->error, sizeof(ctx->error), "FT_Write failed: %d", (int)ftStatus);
            return -1;
        }
        if ((int)bytesWritten != b->tx_num_bytes) {
            ctx->failed_flushes++;
            LOG_ERROR("Partial write: only %lu of %d bytes", bytesWritten, b->tx_num_bytes);
            snprintf(ctx->error, sizeof(ctx->error), "Partial write: %u/%d", (unsigned int)bytesWritten, b->tx_num_bytes);
            return -1;
        }
        LOG_TRACE("TX flush successful: %lu bytes", bytesWritten);
        b->tx_num_bytes = 0;
    }
    
    if (b->rx_num_bytes > 0) {
        int bytes_read = 0;
        int timeout_us = 500000;  /* 500ms max timeout */
        int spin_count = 0;
        const int max_spin = 1000;  /* OPTIMIZATION: Reduced from 10000 to prevent excessive spinning */
        
        LOG_TRACE("Reading %d bytes with timeout %dus", b->rx_num_bytes, timeout_us);

        while (bytes_read < b->rx_num_bytes && timeout_us > 0) {
            DWORD rxQueue = 0;
            ftStatus = FT_GetQueueStatus(ctx->ft_handle, &rxQueue);
            if (ftStatus != FT_OK) {
                LOG_ERROR("FT_GetQueueStatus failed: %d", (int)ftStatus);
                return -1;
            }
            
            if (rxQueue > 0) {
                DWORD toRead = min(rxQueue, (DWORD)(b->rx_num_bytes - bytes_read));
                DWORD actualRead = 0;
                ftStatus = FT_Read(ctx->ft_handle, b->rx_buffer + bytes_read, toRead, &actualRead);
                if (ftStatus != FT_OK) {
                    LOG_ERROR("FT_Read failed: %d", (int)ftStatus);
                    return -1;
                }
                bytes_read += actualRead;
                spin_count = 0;
            } else {
                if (spin_count < max_spin) {
                    spin_count++;
                } else {
                    usleep(10);  /* OPTIMIZATION: Reduced from 100us to 10us for lower latency */
                    timeout_us -= 10;
                }
            }
        }

        if (bytes_read != b->rx_num_bytes) {
            LOG_ERROR("Only read %d of %d bytes after timeout", bytes_read, b->rx_num_bytes);
            snprintf(ctx->error, sizeof(ctx->error), "Read timeout: %d/%d", bytes_read, b->rx_num_bytes);
            return -1;
        }
        
        for (struct rx_observer_node *o = b->rx_observer_first; o; o = o->next) {
            o->fn(b->rx_buffer + (o->data - b->rx_buffer), o->extra);
        }
        
        b->rx_observer_first = b->rx_observer_last = NULL;
        b->rx_num_bytes = 0;
    }
    
    return 0;
}

/* RX observer structures */
struct bit_copier_extra {
    int from_bit;
    uint8_t *dst;
    int to_bit;
    int num_bits;
};

static void bit_copier_rx_observer_fn(const uint8_t *rxData, void *extra)
{
    const struct bit_copier_extra *e = extra;
    copy_bits(rxData, e->from_bit, e->dst, e->to_bit, e->num_bits, false);
}

struct byte_copier_extra {
    uint8_t *dst;
    int num_bytes;
};

static void byte_copier_rx_observer_fn(const uint8_t *rxData, void *extra)
{
    const struct byte_copier_extra *e = extra;
    memcpy(e->dst, rxData, e->num_bytes);
}

struct bulk_byte_copier_extra {
    uint8_t *dst;
    int total_bytes;
    int bytes_copied;
};

static void bulk_byte_copier_rx_observer_fn(const uint8_t *rxData, void *extra)
{
    struct bulk_byte_copier_extra *e = extra;
    memcpy(e->dst + e->bytes_copied, rxData, e->total_bytes - e->bytes_copied);
    e->bytes_copied = e->total_bytes;
}

/* Ensure buffer can hold additional data, flush if needed */
static int mpsse_buffer_ensure_can_append(mpsse_context_t *ctx, int tx_bytes, int rx_bytes)
{
    struct mpsse_buffer *b = &ctx->buffer;
    
    int tx_safety_threshold = (b->max_tx_buffer_bytes * 8) / 10;
    int rx_safety_threshold = (b->max_rx_buffer_bytes * 8) / 10;
    
    if (b->tx_num_bytes + tx_bytes > b->max_tx_buffer_bytes ||
        b->rx_num_bytes + rx_bytes > b->max_rx_buffer_bytes ||
        b->tx_num_bytes > tx_safety_threshold ||
        b->rx_num_bytes > rx_safety_threshold) {
        LOG_TRACE("Safety flush triggered: tx=%d/%d, rx=%d/%d, adding tx=%d, rx=%d",
                  b->tx_num_bytes, b->max_tx_buffer_bytes,
                  b->rx_num_bytes, b->max_rx_buffer_bytes,
                  tx_bytes, rx_bytes);
        if (mpsse_buffer_flush(ctx) < 0) {
            return -1;
        }
    }
    
    /* EXPERIMENT: Removed 512-byte early flush to allow 64KB transfers */
    
    return 0;
}

/* Append data to buffer */
static int mpsse_buffer_append(mpsse_context_t *ctx, const uint8_t *tx_data, int tx_bytes,
                               rx_observer_fn observer, void *observer_extra, int rx_bytes)
{
    struct mpsse_buffer *b = &ctx->buffer;
    
    int flush_threshold = 61440;  /* Experiment: 60KB threshold for 64KB transfers */
    
    bool would_overflow = (b->tx_num_bytes + tx_bytes > b->max_tx_buffer_bytes ||
                           b->rx_num_bytes + rx_bytes > b->max_rx_buffer_bytes);
    
    bool at_threshold = (b->tx_num_bytes >= flush_threshold ||
                         b->rx_num_bytes >= flush_threshold);
    
    if (would_overflow || at_threshold) {
        if (b->rx_num_bytes > 0) {
            LOG_TRACE("Pre-append flush with RX: tx=%d+%d, rx=%d+%d",
                      b->tx_num_bytes, tx_bytes, b->rx_num_bytes, rx_bytes);
        } else {
            LOG_TRACE("Pre-append flush TX only: tx=%d+%d",
                      b->tx_num_bytes, tx_bytes);
        }
        if (mpsse_buffer_flush(ctx) < 0) {
            return -1;
        }
    }
    
    if (tx_bytes > 0) {
        if (b->tx_num_bytes + tx_bytes > b->max_tx_buffer_bytes) {
            LOG_ERROR("TX buffer overflow after flush: %d+%d > %d",
                      b->tx_num_bytes, tx_bytes, b->max_tx_buffer_bytes);
            return -1;
        }
        memcpy(b->tx_buffer + b->tx_num_bytes, tx_data, tx_bytes);
        b->tx_num_bytes += tx_bytes;
    }
    
    if (rx_bytes > 0) {
        if (b->rx_num_bytes + rx_bytes > b->max_rx_buffer_bytes) {
            LOG_ERROR("RX buffer overflow after flush: %d+%d > %d",
                      b->rx_num_bytes, rx_bytes, b->max_rx_buffer_bytes);
            return -1;
        }
        if (observer) {
            struct rx_observer_node *node = pool_alloc(&ctx->observer_pool, sizeof(struct rx_observer_node));
            if (!node) {
                LOG_ERROR("Failed to allocate observer node from pool");
                return -1;
            }
            node->next = NULL;
            node->fn = observer;
            node->data = b->rx_buffer + b->rx_num_bytes;
            node->extra = observer_extra;
            
            if (!b->rx_observer_first) {
                b->rx_observer_first = b->rx_observer_last = node;
            } else {
                b->rx_observer_last->next = node;
                b->rx_observer_last = node;
            }
        }
        b->rx_num_bytes += rx_bytes;
    }
    
    return 0;
}

static int mpsse_buffer_add_write_with_readback(mpsse_context_t *ctx, const uint8_t *tx_data,
                                                  int tx_bytes, rx_observer_fn observer,
                                                  void *observer_extra, int rx_bytes)
{
    if (mpsse_buffer_ensure_can_append(ctx, tx_bytes, rx_bytes) < 0) {
        return -1;
    }
    return mpsse_buffer_append(ctx, tx_data, tx_bytes, observer, observer_extra, rx_bytes);
}

static int mpsse_buffer_add_write_simple(mpsse_context_t *ctx, const uint8_t *tx_data, int tx_bytes,
                                         uint8_t *rx_data, int rx_bytes)
{
    struct byte_copier_extra *extra = pool_alloc(&ctx->observer_pool, sizeof(struct byte_copier_extra));
    if (!extra) {
        LOG_ERROR("Failed to allocate byte copier extra from pool");
        return -1;
    }
    extra->dst = rx_data;
    extra->num_bytes = rx_bytes;
    
    return mpsse_buffer_add_write_with_readback(ctx, tx_data, tx_bytes,
                                                 byte_copier_rx_observer_fn, extra, rx_bytes);
}

/* Append TMS shift command - used for state transitions */
static int append_tms_shift(mpsse_context_t *ctx, const uint8_t *tms, int from_bit_idx, int to_bit_idx)
{
    while (from_bit_idx < to_bit_idx) {
        const int max_tms_bits_per_command = 6;
        const int bits_to_transfer = min(to_bit_idx - from_bit_idx, max_tms_bits_per_command);
        
        /* Build TMS byte: bit 7 = TDI value, bits 0-6 = TMS values */
        uint8_t tms_byte = 0;
        for (int i = 0; i < bits_to_transfer; i++) {
            if (get_bit(tms, from_bit_idx + i)) {
                tms_byte |= (1 << i);
            }
        }
        
        uint8_t cmd[] = {
            OP_CLK_TMS_NO_READ,
            bits_to_transfer - 1,
            tms_byte | (!!ctx->last_tdi << 7),
        };
        
        from_bit_idx += bits_to_transfer;
        
        if (mpsse_buffer_append(ctx, cmd, 3, NULL, NULL, 0) < 0) {
            return -1;
        }
    }
    return 0;
}

/* Append TDI shift command with TDO read - used for data shifting in SHIFT-DR/IR states */
static int append_tdi_shift(mpsse_context_t *ctx, const uint8_t *tdi, uint8_t *tdo,
                            int from_bit_idx, int to_bit_idx, bool last_tms_bit_high)
{
    const int last_bit_idx = to_bit_idx - 1;
    const int num_regular_bits = last_bit_idx - from_bit_idx;
    const int num_first_octet_bits = 8 - from_bit_idx % 8;
    const int num_leading_bits = min(num_first_octet_bits == 8 ? 0 : num_first_octet_bits, num_regular_bits);
    const bool leading_only = num_leading_bits == num_regular_bits;
    const int inner_end_idx = leading_only ? -1 : last_bit_idx - last_bit_idx % 8;
    const int num_trailing_bits = leading_only ? 0 : last_bit_idx % 8;
    
    struct bulk_byte_copier_extra *bulk_extra = NULL;
    int total_inner_bytes = 0;
    
    if (inner_end_idx > from_bit_idx && !leading_only) {
        total_inner_bytes = (inner_end_idx - (from_bit_idx + num_leading_bits)) / 8;
        if (total_inner_bytes > 0) {
            bulk_extra = pool_alloc(&ctx->observer_pool, sizeof(struct bulk_byte_copier_extra));
            if (!bulk_extra) {
                LOG_ERROR("Failed to allocate bulk byte copier extra from pool");
                return -1;
            }
            bulk_extra->dst = tdo + (from_bit_idx + num_leading_bits) / 8;
            bulk_extra->total_bytes = total_inner_bytes;
            bulk_extra->bytes_copied = 0;
        }
    }
    
    for (int cur_idx = from_bit_idx; cur_idx < to_bit_idx;) {
        /* Leading bits (not byte-aligned) */
        if (cur_idx == from_bit_idx && num_leading_bits > 0) {
            uint8_t cmd[] = {
                OP_CLK_DATA_BITS_OUT_NEG_IN_POS,
                num_leading_bits - 1,
                tdi[from_bit_idx / 8] >> (from_bit_idx % 8),
            };
            
            struct bit_copier_extra *extra = pool_alloc(&ctx->observer_pool, sizeof(struct bit_copier_extra));
            if (!extra) {
                LOG_ERROR("Failed to allocate bit copier extra from pool");
                return -1;
            }
            extra->from_bit = 8 - num_leading_bits;
            extra->dst = tdo;
            extra->to_bit = from_bit_idx;
            extra->num_bits = num_leading_bits;
            
            if (mpsse_buffer_add_write_with_readback(ctx, cmd, 3, bit_copier_rx_observer_fn, extra, 1) < 0) {
                return -1;
            }
            cur_idx += num_leading_bits;
        }
        
        /* Whole bytes */
        if (cur_idx < last_bit_idx && inner_end_idx > cur_idx) {
            const int inner_octets_to_send = min((inner_end_idx - cur_idx) / 8, ctx->chip_buffer_size);
            
            uint8_t cmd[] = {
                OP_CLK_DATA_BYTES_OUT_NEG_IN_POS,
                ((inner_octets_to_send - 1) >> 0) & 0xff,
                ((inner_octets_to_send - 1) >> 8) & 0xff,
            };
            
            int remaining_bytes = total_inner_bytes - (cur_idx - (from_bit_idx + num_leading_bits)) / 8;
            bool is_last_chunk = (inner_octets_to_send >= remaining_bytes);
            
            if (mpsse_buffer_append(ctx, cmd, 3, NULL, NULL, 0) < 0) {
                return -1;
            }
            
            if (is_last_chunk && bulk_extra) {
                if (mpsse_buffer_add_write_with_readback(ctx, tdi + cur_idx / 8, inner_octets_to_send,
                                                          bulk_byte_copier_rx_observer_fn, bulk_extra, inner_octets_to_send) < 0) {
                    return -1;
                }
            } else {
                if (mpsse_buffer_add_write_simple(ctx, tdi + cur_idx / 8, inner_octets_to_send,
                                                   tdo + cur_idx / 8, inner_octets_to_send) < 0) {
                    return -1;
                }
            }
            
            cur_idx += inner_octets_to_send * 8;
        }
        
        /* Trailing bits */
        if (num_trailing_bits > 0 && cur_idx < last_bit_idx) {
            uint8_t cmd[] = {
                OP_CLK_DATA_BITS_OUT_NEG_IN_POS,
                num_trailing_bits - 1,
                tdi[inner_end_idx / 8],
            };
            
            struct bit_copier_extra *extra = pool_alloc(&ctx->observer_pool, sizeof(struct bit_copier_extra));
            if (!extra) {
                LOG_ERROR("Failed to allocate bit copier extra from pool");
                return -1;
            }
            extra->from_bit = 8 - num_trailing_bits;
            extra->dst = tdo;
            extra->to_bit = inner_end_idx;
            extra->num_bits = num_trailing_bits;
            
            if (mpsse_buffer_add_write_with_readback(ctx, cmd, 3, bit_copier_rx_observer_fn, extra, 1) < 0) {
                return -1;
            }
            cur_idx += num_trailing_bits;
        }
        
        /* Last bit with TMS to exit shift state */
        if (cur_idx == last_bit_idx) {
            const int last_tdi_bit = !!get_bit(tdi, last_bit_idx);
            const int last_tms_bit = !!last_tms_bit_high;
            
            /* Use TMS command for last bit - allows simultaneous TMS control */
            uint8_t cmd[] = {
                OP_CLK_TMS_READ,
                0x00,  /* 1 bit */
                (last_tdi_bit << 7) | (last_tms_bit << 1) | last_tms_bit,
            };
            
            struct bit_copier_extra *extra = pool_alloc(&ctx->observer_pool, sizeof(struct bit_copier_extra));
            if (!extra) {
                LOG_ERROR("Failed to allocate bit copier extra from pool");
                return -1;
            }
            extra->from_bit = 7;  /* TDO is in bit 7 of response */
            extra->dst = tdo;
            extra->to_bit = last_bit_idx;
            extra->num_bits = 1;
            
            if (mpsse_buffer_add_write_with_readback(ctx, cmd, 3, bit_copier_rx_observer_fn, extra, 1) < 0) {
                return -1;
            }
            
            ctx->last_tdi = last_tdi_bit;
            cur_idx += 1;
        }
    }
    return 0;
}

/* Open device and initialize MPSSE */
int mpsse_adapter_open(mpsse_context_t *ctx, int vendor, int product, 
                       const char *serial, int index, int interface)
{
    if (!ctx) return -1;
    
    FT_STATUS ftStatus;
    DWORD numDevs = 0;
    
    (void)vendor;
    (void)product;
    (void)interface;
    
    /* Get number of devices */
    ftStatus = FT_CreateDeviceInfoList(&numDevs);
    if (ftStatus != FT_OK) {
        snprintf(ctx->error, sizeof(ctx->error), "FT_CreateDeviceInfoList failed: %d", (int)ftStatus);
        LOG_ERROR("%s", ctx->error);
        return -1;
    }
    
    if (numDevs == 0) {
        snprintf(ctx->error, sizeof(ctx->error), "No FTDI devices found");
        LOG_ERROR("%s", ctx->error);
        return -1;
    }
    
    LOG_INFO("Found %lu FTDI device(s)", numDevs);
    
    /* Open device by serial number or index */
    if (serial && serial[0]) {
        ftStatus = FT_OpenEx((PVOID)serial, FT_OPEN_BY_SERIAL_NUMBER, &ctx->ft_handle);
        if (ftStatus != FT_OK) {
            snprintf(ctx->error, sizeof(ctx->error), "FT_OpenEx by serial failed: %d", (int)ftStatus);
            LOG_ERROR("%s", ctx->error);
            return -1;
        }
    } else {
        ftStatus = FT_Open(index, &ctx->ft_handle);
        if (ftStatus != FT_OK) {
            snprintf(ctx->error, sizeof(ctx->error), "FT_Open failed: %d", (int)ftStatus);
            LOG_ERROR("%s", ctx->error);
            return -1;
        }
    }
    
    /* Reset device */
    ftStatus = FT_ResetDevice(ctx->ft_handle);
    if (ftStatus != FT_OK) {
        LOG_WARN("FT_ResetDevice failed: %d", (int)ftStatus);
    }
    
    /* Purge buffers */
    ftStatus = FT_Purge(ctx->ft_handle, FT_PURGE_RX | FT_PURGE_TX);
    if (ftStatus != FT_OK) {
        LOG_WARN("FT_Purge failed: %d", (int)ftStatus);
    }
    
    /* Set USB parameters - large transfer size for high throughput */
    ftStatus = FT_SetUSBParameters(ctx->ft_handle, 65536, 65536);
    if (ftStatus != FT_OK) {
        LOG_WARN("FT_SetUSBParameters failed: %d", (int)ftStatus);
    }
    
    /* Disable event/error characters */
    ftStatus = FT_SetChars(ctx->ft_handle, 0, 0, 0, 0);
    if (ftStatus != FT_OK) {
        LOG_WARN("FT_SetChars failed: %d", (int)ftStatus);
    }
    
    /* Set timeouts */
    ftStatus = FT_SetTimeouts(ctx->ft_handle, 5000, 5000);
    if (ftStatus != FT_OK) {
        LOG_WARN("FT_SetTimeouts failed: %d", (int)ftStatus);
    }
    
    /* Latency timer removed - relying on bulk USB transfers for performance */
    
    /* Reset bit mode to default */
    ftStatus = FT_SetBitMode(ctx->ft_handle, 0x00, FT_BITMODE_RESET);
    if (ftStatus != FT_OK) {
        LOG_WARN("FT_SetBitMode reset failed: %d", (int)ftStatus);
    }
    usleep(10000);  /* 10ms delay */
    
    /* Enable MPSSE mode */
    ftStatus = FT_SetBitMode(ctx->ft_handle, 0x00, FT_BITMODE_MPSSE);
    if (ftStatus != FT_OK) {
        snprintf(ctx->error, sizeof(ctx->error), "FT_SetBitMode MPSSE failed: %d", (int)ftStatus);
        LOG_ERROR("%s", ctx->error);
        FT_Close(ctx->ft_handle);
        ctx->ft_handle = NULL;
        return -1;
    }
    usleep(50000);  /* 50ms delay for MPSSE setup */
    
    /* Clear any junk in the receive buffer */
    DWORD rxBytes = 0;
    FT_GetQueueStatus(ctx->ft_handle, &rxBytes);
    if (rxBytes > 0) {
        uint8_t junk[256];
        DWORD bytesRead = 0;
        FT_Read(ctx->ft_handle, junk, min(rxBytes, sizeof(junk)), &bytesRead);
    }
    
    /* MPSSE setup commands */
    uint8_t setup_cmds[] = {
        /* Disable loopback */
        OP_LOOPBACK_OFF,
        
        /* Set clock divisor for default ~1MHz (60MHz / (2 * 29) = ~1.03MHz) */
        OP_SET_TCK_DIVISOR,
        29 & 0xFF,
        (29 >> 8) & 0xFF,
        
        /* Disable divide-by-5 for 60MHz clock on FT2232H/FT232H */
        OP_DISABLE_CLK_DIVIDE_BY_5,
        
        /* Set initial pin states: TCK=0, TDI=0, TMS=1 (high for RESET state) */
        OP_SET_DBUS_LOBYTE,
        0x08,  /* Value: TMS=1, TDI=0, TCK=0 */
        0x0B,  /* Direction: TCK=out, TDI=out, TDO=in, TMS=out (bits 0,1,3 out; bit 2 in) */
    };
    
    DWORD bytesWritten = 0;
    ftStatus = FT_Write(ctx->ft_handle, setup_cmds, sizeof(setup_cmds), &bytesWritten);
    if (ftStatus != FT_OK || bytesWritten != sizeof(setup_cmds)) {
        snprintf(ctx->error, sizeof(ctx->error), "FT_Write setup failed: %d", (int)ftStatus);
        LOG_ERROR("%s", ctx->error);
        FT_Close(ctx->ft_handle);
        ctx->ft_handle = NULL;
        return -1;
    }
    
    usleep(10000);  /* 10ms delay */
    
    /* Clear any response */
    FT_GetQueueStatus(ctx->ft_handle, &rxBytes);
    if (rxBytes > 0) {
        uint8_t junk[256];
        DWORD bytesRead = 0;
        FT_Read(ctx->ft_handle, junk, min(rxBytes, sizeof(junk)), &bytesRead);
    }
    
    /* Detect chip type based on device info */
    FT_DEVICE ftDevice;
    DWORD deviceID;
    char serialNumber[64];
    char description[128];
    
    ftStatus = FT_GetDeviceInfo(ctx->ft_handle, &ftDevice, &deviceID, 
                                 serialNumber, description, NULL);
    if (ftStatus == FT_OK) {
        LOG_INFO("Device: %s (ID: 0x%04X), Serial: %s", description, deviceID, serialNumber);
        
        /* Set buffer sizes for 64KB experiment (bypass chip limits) */
        ctx->chip_buffer_size = 65536;
        LOG_INFO("Detected FTDI device, using 64KB buffer for large transfer experiment");
    } else {
        ctx->chip_buffer_size = 65536;  /* Experiment: use 64KB even for undetected devices */
        LOG_WARN("Could not detect device type, using 64KB buffer for experiment");
    }
    
    /* Reallocate buffers for detected chip */
    ctx->buffer.max_tx_buffer_bytes = 3 * ctx->chip_buffer_size;
    ctx->buffer.max_rx_buffer_bytes = ctx->chip_buffer_size;
    
    free(ctx->buffer.tx_buffer);
    free(ctx->buffer.rx_buffer);
    
    ctx->buffer.tx_buffer = malloc(ctx->buffer.max_tx_buffer_bytes);
    ctx->buffer.rx_buffer = malloc(ctx->buffer.max_rx_buffer_bytes);
    
    if (!ctx->buffer.tx_buffer || !ctx->buffer.rx_buffer) {
        LOG_ERROR("Failed to allocate buffers");
        FT_Close(ctx->ft_handle);
        ctx->ft_handle = NULL;
        return -1;
    }
    
    ctx->is_open = true;
    ctx->state = TEST_LOGIC_RESET;
    ctx->last_tdi = 0;
    
    LOG_INFO("MPSSE adapter opened successfully");
    
    return 0;
}

void mpsse_adapter_close(mpsse_context_t *ctx)
{
    if (!ctx || !ctx->is_open) return;
    
    /* Flush any pending data */
    mpsse_buffer_flush(ctx);
    
    /* Reset to default mode */
    if (ctx->ft_handle) {
        FT_SetBitMode(ctx->ft_handle, 0x00, FT_BITMODE_RESET);
        FT_Close(ctx->ft_handle);
        ctx->ft_handle = NULL;
    }
    
    ctx->is_open = false;
    LOG_INFO("MPSSE adapter closed");
}

int mpsse_adapter_set_frequency(mpsse_context_t *ctx, uint32_t frequency_hz)
{
    if (!ctx || !ctx->is_open) return -1;
    
    if (frequency_hz > MPSSE_MAX_FREQUENCY) frequency_hz = MPSSE_MAX_FREQUENCY;
    if (frequency_hz < 1) frequency_hz = 1;
    
    /* Calculate divisor for 60MHz base clock (disable divide-by-5) */
    /* TCK = 60MHz / (2 * divisor) where divisor >= 1 */
    /* Using ceiling division to ensure we don't exceed requested frequency */
    unsigned int divisor = ((60000000 / 2) + frequency_hz - 1) / frequency_hz;
    if (divisor > 0xFFFF) divisor = 0xFFFF;
    if (divisor < 1) divisor = 1;
    
    unsigned int actual = 60000000 / (2 * divisor);
    
    uint8_t cmd[] = {
        OP_SET_TCK_DIVISOR,
        divisor & 0xFF,
        (divisor >> 8) & 0xFF,
        OP_DISABLE_CLK_DIVIDE_BY_5,
    };
    
    if (mpsse_buffer_append(ctx, cmd, sizeof(cmd), NULL, NULL, 0) < 0) {
        return -1;
    }
    
    /* Flush immediately to apply frequency change */
    if (mpsse_buffer_flush(ctx) < 0) {
        return -1;
    }
    
    LOG_INFO("MPSSE frequency: requested=%uHz, actual=%uHz (divisor=%u)", 
             frequency_hz, actual, divisor);
    
    return actual;
}

/* Main scan function - processes TMS/TDI vectors and returns TDO data */
int mpsse_adapter_scan(mpsse_context_t *ctx, const uint8_t *tms, const uint8_t *tdi,
                        uint8_t *tdo, int bits)
{
    if (!ctx || !ctx->is_open || !tms || !tdi || !tdo || bits <= 0) return -1;
    
    LOG_TRACE("MPSSE scan: bits=%d, bytes=%d", bits, (bits + 7) / 8);
    
    ctx->current_tms = tms;
    ctx->current_tdi = tdi;
    ctx->current_bit_offset = 0;
    
    int first_pending_bit_idx = 0;
    enum jtag_state jtag_state = ctx->state;
    
    /* Process bit by bit, handling state transitions */
    for (int bit_idx = 0; bit_idx < bits;) {
        uint8_t tms_byte = tms[bit_idx / 8];
        const int this_round_end_bit_idx = bit_idx + 8 > bits ? bits : bit_idx + 8;
        
        for (; bit_idx < this_round_end_bit_idx; tms_byte >>= 1, bit_idx++) {
            const bool tms_bit = tms_byte & 1;
            const enum jtag_state next_jtag_state = next_state(jtag_state, tms_bit);
            const bool is_shift = (jtag_state == SHIFT_DR || jtag_state == SHIFT_IR);
            const bool next_is_shift = (next_jtag_state == SHIFT_DR || next_jtag_state == SHIFT_IR);
            const bool entering_shift = !is_shift && next_is_shift;
            const bool leaving_shift = is_shift && !next_is_shift;
            const bool end_of_vector = (bit_idx == bits - 1);
            const bool event = end_of_vector || entering_shift || leaving_shift;
            
            if (event) {
                const int next_pending_bit_idx = bit_idx + 1;
                if (is_shift) {
                    /* In shift state - shift TDI data and read TDO */
                    if (append_tdi_shift(ctx, tdi, tdo, first_pending_bit_idx, 
                                          next_pending_bit_idx, leaving_shift) < 0) {
                        LOG_ERROR("MPSSE scan failed during TDI shift");
                        return -1;
                    }
                } else {
                    /* In non-shift state - just clock TMS transitions */
                    if (append_tms_shift(ctx, tms, first_pending_bit_idx, 
                                         next_pending_bit_idx) < 0) {
                        LOG_ERROR("MPSSE scan failed during TMS shift");
                        return -1;
                    }
                }
                first_pending_bit_idx = next_pending_bit_idx;
            }
            jtag_state = next_jtag_state;
        }
    }
    
    /* Flush all buffered commands */
    if (mpsse_buffer_flush(ctx) < 0) {
        LOG_ERROR("MPSSE flush failed");
        return -1;
    }
    
    ctx->current_bit_offset += bits;
    
    /* Reset memory pool after transaction completes */
    pool_reset(&ctx->observer_pool);
    
    ctx->state = jtag_state;
    
    return 0;
}

int mpsse_adapter_flush(mpsse_context_t *ctx)
{
    if (!ctx || !ctx->is_open) return -1;
    return mpsse_buffer_flush(ctx);
}

const char* mpsse_adapter_error(const mpsse_context_t *ctx)
{
    if (!ctx) return "NULL context";
    return ctx->error[0] ? ctx->error : "No error";
}

void mpsse_adapter_set_verbose(mpsse_context_t *ctx, int level)
{
    if (ctx) {
        ctx->verbose = level;
    }
}
