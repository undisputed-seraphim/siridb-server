/*
 * qpack.h - efficient binary serialization format
 *
 * author       : Jeroen van der Heijden
 * email        : jeroen@transceptor.technology
 * copyright    : 2016, Transceptor Technology
 *
 * changes
 *  - initial version, 11-03-2016
 *
 */
#pragma once
#include <qpack.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define QP_SUGGESTED_SIZE 65536

typedef FILE qp_fpacker_t;
#define qp_open fopen    // returns NULL in case of an error
#define qp_close fclose   // 0 if successful, EOF in case of an error
#define qp_flush fflush   // 0 if successful, EOF in case of an error

/* extend functions */
int qp_packer_extend(qp_packer_t * packer, qp_packer_t * source);
int qp_packer_extend_fu(qp_packer_t * packer, qp_unpacker_t * unpacker);

/* unpacker: create and destroy functions */
void qp_unpacker_ff_free(qp_unpacker_t * unpacker);
qp_unpacker_t * qp_unpacker_ff(const char * fn);

/* step functions to be used with an unpacker */
qp_types_t qp_current(qp_unpacker_t * unpacker);
qp_types_t qp_skip_next(qp_unpacker_t * unpacker);


/* Add to packer functions */
int qp_add_raw(qp_packer_t * packer, const char * raw, size_t len);

/* shortcuts for qp_add_raw() */
static inline int qp_add_string(qp_packer_t * packer, const char * str)
{
    return qp_add_raw(packer, str, strlen(str));
}
static inline int qp_add_string_term(qp_packer_t * packer, const char * str)
{
    return qp_add_raw(packer, str, strlen(str) + 1);
}

int qp_add_raw_term(qp_packer_t * packer, const char * raw, size_t len);
int qp_add_double(qp_packer_t * packer, double real);
int qp_add_int8(qp_packer_t * packer, int8_t integer);
int qp_add_int16(qp_packer_t * packer, int16_t integer);
int qp_add_int32(qp_packer_t * packer, int32_t integer);
int qp_add_int64(qp_packer_t * packer, int64_t integer);
int qp_add_true(qp_packer_t * packer);
int qp_add_false(qp_packer_t * packer);
int qp_add_null(qp_packer_t * packer);
int qp_add_type(qp_packer_t * packer, qp_types_t tp);
int qp_add_fmt(qp_packer_t * packer, const char * fmt, ...);
int qp_add_fmt_safe(qp_packer_t * packer, const char * fmt, ...);

/* Add to file-packer functions */
int qp_fadd_type(qp_fpacker_t * fpacker, qp_types_t tp);
int qp_fadd_raw(qp_fpacker_t * fpacker, const char * raw, size_t len);
int qp_fadd_string(qp_fpacker_t * fpacker, const char * str);
int qp_fadd_int8(qp_fpacker_t * fpacker, int8_t integer);
int qp_fadd_int16(qp_fpacker_t * fpacker, int16_t integer);
int qp_fadd_int32(qp_fpacker_t * fpacker, int32_t integer);
int qp_fadd_int64(qp_fpacker_t * fpacker, int64_t integer);
int qp_fadd_double(qp_fpacker_t * fpacker, double real);

/* creates a valid qpack buffer of length 3 holding an int16 type. */
#define QP_PACK_INT16(buffer, n) \
char buffer[3];\
buffer[0] = QP_INT16; \
memcpy(&buffer[1], &n, 2);


