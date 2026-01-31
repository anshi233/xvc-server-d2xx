/*
 * Copyright 2021 Sergey Guralnik
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "txvc/defs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct txvc_jtag_split_event;
typedef bool (*txvc_jtag_splitter_callback)(const struct txvc_jtag_split_event *event, void *extra);

struct txvc_jtag_splitter {
    int _state;
    txvc_jtag_splitter_callback _cb;
    void *_cbExtra;
};

extern bool txvc_jtag_splitter_init(struct txvc_jtag_splitter *splitter,
        txvc_jtag_splitter_callback cb, void *cbExtra);
extern bool txvc_jtag_splitter_deinit(struct txvc_jtag_splitter *splitter);
extern bool txvc_jtag_splitter_process(struct txvc_jtag_splitter *splitter,
        int numBits, const uint8_t* tms, const uint8_t* tdi, uint8_t* tdo);

#define TXVC_JTAG_SPLIT_EVENTS(X) \
    X(shift_tms, \
        const uint8_t *tms; \
        int fromBitIdx; \
        int toBitIdx;) \
    X(shift_tdi, \
        const uint8_t *tdi; \
        uint8_t *tdo; \
        int fromBitIdx; \
        int toBitIdx; \
        bool incomplete;) \
    X(flush_all, \
        char dummyStuffing;)

#define AS_EVENT_STRUCT(name, fields) struct txvc_jtag_split_ ## name { fields };
TXVC_JTAG_SPLIT_EVENTS(AS_EVENT_STRUCT)
#undef AS_EVENT_STRUCT

#define AS_ENUM(name, fields) JTAG_SPLIT_ ## name,
enum txvc_jtag_split_event_kind {
    TXVC_JTAG_SPLIT_EVENTS(AS_ENUM)
};
#undef AS_ENUM

struct txvc_jtag_split_event {
    enum txvc_jtag_split_event_kind _kind;
    union {
#define AS_EVENT_ALTERNATIVE(name, fields) struct txvc_jtag_split_ ## name _ ## name;
    TXVC_JTAG_SPLIT_EVENTS(AS_EVENT_ALTERNATIVE)
#undef AS_EVENT_ALTERNATIVE
    } _info;
};

#define AS_EVENT_CAST(name, fields) \
    TXVC_USED static inline const struct txvc_jtag_split_ ## name * \
    txvc_jtag_split_cast_to_ ## name(const struct txvc_jtag_split_event *event) { \
        return event->_kind == JTAG_SPLIT_ ## name ? &event->_info._ ## name : NULL; \
    }
TXVC_JTAG_SPLIT_EVENTS(AS_EVENT_CAST)
#undef AS_EVENT_CAST

#undef TXVC_JTAG_SPLIT_EVENTS
