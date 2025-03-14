/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasm_loader.h"
#include "bh_common.h"
#include "bh_log.h"
#include "wasm.h"
#include "wasm_opcode.h"
#include "wasm_runtime.h"
#include "wasm_native.h"

/* Read a value of given type from the address pointed to by the given
   pointer and increase the pointer to the position just after the
   value being read.  */
#define TEMPLATE_READ_VALUE(Type, p)                    \
    (p += sizeof(Type), *(Type *)(p - sizeof(Type)))

static void
set_error_buf(char *error_buf, uint32 error_buf_size, const char *string)
{
    if (error_buf != NULL)
        snprintf(error_buf, error_buf_size, "%s", string);
}

static bool
check_buf(const uint8 *buf, const uint8 *buf_end, uint32 length,
          char *error_buf, uint32 error_buf_size)
{
    if (buf + length > buf_end) {
        set_error_buf(error_buf, error_buf_size,
                      "WASM module load failed: "
                      "unexpected end of section or function");
        return false;
    }
    return true;
}

static bool
check_buf1(const uint8 *buf, const uint8 *buf_end, uint32 length,
           char *error_buf, uint32 error_buf_size)
{
    if (buf + length > buf_end) {
        set_error_buf(error_buf, error_buf_size,
                      "WASM module load failed: unexpected end");
        return false;
    }
    return true;
}

#define CHECK_BUF(buf, buf_end, length) do {    \
  if (!check_buf(buf, buf_end, length,          \
                 error_buf, error_buf_size)) {  \
      return false;                             \
  }                                             \
} while (0)

#define CHECK_BUF1(buf, buf_end, length) do {   \
  if (!check_buf1(buf, buf_end, length,         \
                  error_buf, error_buf_size)) { \
      return false;                             \
  }                                             \
} while (0)

static bool
skip_leb(const uint8 **p_buf, const uint8 *buf_end, uint32 maxbits,
         char* error_buf, uint32 error_buf_size)
{
    const uint8 *buf = *p_buf;
    uint32 offset = 0, bcnt = 0;
    uint64 byte;

    while (true) {
        if (bcnt + 1 > (maxbits + 6) / 7) {
            set_error_buf(error_buf, error_buf_size,
                          "WASM module load failed: "
                          "integer representation too long");
            return false;
        }

        CHECK_BUF(buf, buf_end, offset + 1);
        byte = buf[offset];
        offset += 1;
        bcnt += 1;
        if ((byte & 0x80) == 0) {
            break;
        }
    }

    *p_buf += offset;
    return true;
}

#define skip_leb_int64(p, p_end) do {               \
  if (!skip_leb(&p, p_end, 64,                      \
                error_buf, error_buf_size))         \
    return false;                                   \
} while (0)

#define skip_leb_uint32(p, p_end) do {              \
  if (!skip_leb(&p, p_end, 32,                      \
                error_buf, error_buf_size))         \
    return false;                                   \
} while (0)

#define skip_leb_int32(p, p_end) do {               \
  if (!skip_leb(&p, p_end, 32,                      \
                error_buf, error_buf_size))         \
    return false;                                   \
} while (0)

static bool
read_leb(uint8 **p_buf, const uint8 *buf_end,
         uint32 maxbits, bool sign, uint64 *p_result,
         char* error_buf, uint32 error_buf_size)
{
    const uint8 *buf = *p_buf;
    uint64 result = 0;
    uint32 shift = 0;
    uint32 offset = 0, bcnt = 0;
    uint64 byte;

    while (true) {
        if (bcnt + 1 > (maxbits + 6) / 7) {
            set_error_buf(error_buf, error_buf_size,
                          "WASM module load failed: "
                          "integer representation too long");
            return false;
        }

        CHECK_BUF(buf, buf_end, offset + 1);
        byte = buf[offset];
        offset += 1;
        result |= ((byte & 0x7f) << shift);
        shift += 7;
        bcnt += 1;
        if ((byte & 0x80) == 0) {
            break;
        }
    }

    if (!sign && maxbits == 32 && shift >= maxbits) {
        /* The top bits set represent values > 32 bits */
        if (((uint8)byte) & 0xf0)
            goto fail_integer_too_large;
    }
    else if (sign && maxbits == 32) {
        if (shift < maxbits) {
            /* Sign extend */
            result = (((int32)result) << (maxbits - shift))
                     >> (maxbits - shift);
        }
        else {
            /* The top bits should be a sign-extension of the sign bit */
            bool sign_bit_set = ((uint8)byte) & 0x8;
            int top_bits = ((uint8)byte) & 0xf0;
            if ((sign_bit_set && top_bits != 0x70)
                || (!sign_bit_set && top_bits != 0))
                goto fail_integer_too_large;
        }
    }
    else if (sign && maxbits == 64) {
        if (shift < maxbits) {
            /* Sign extend */
            result = (((int64)result) << (maxbits - shift))
                     >> (maxbits - shift);
        }
        else {
            /* The top bits should be a sign-extension of the sign bit */
            bool sign_bit_set = ((uint8)byte) & 0x1;
            int top_bits = ((uint8)byte) & 0xfe;

            if ((sign_bit_set && top_bits != 0x7e)
                || (!sign_bit_set && top_bits != 0))
                goto fail_integer_too_large;
        }
    }

    *p_buf += offset;
    *p_result = result;
    return true;

fail_integer_too_large:
    set_error_buf(error_buf, error_buf_size,
                  "WASM module load failed: integer too large");
    return false;
}

#define read_uint8(p)  TEMPLATE_READ_VALUE(uint8, p)
#define read_uint32(p) TEMPLATE_READ_VALUE(uint32, p)
#define read_bool(p)   TEMPLATE_READ_VALUE(bool, p)

#define read_leb_int64(p, p_end, res) do {          \
  uint64 res64;                                     \
  if (!read_leb((uint8**)&p, p_end, 64, true, &res64,\
                error_buf, error_buf_size))         \
    return false;                                   \
  res = (int64)res64;                               \
} while (0)

#define read_leb_uint32(p, p_end, res) do {         \
  uint64 res64;                                     \
  if (!read_leb((uint8**)&p, p_end, 32, false, &res64,\
                error_buf, error_buf_size))         \
    return false;                                   \
  res = (uint32)res64;                              \
} while (0)

#define read_leb_int32(p, p_end, res) do {          \
  uint64 res64;                                     \
  if (!read_leb((uint8**)&p, p_end, 32, true, &res64,\
                error_buf, error_buf_size))         \
    return false;                                   \
  res = (int32)res64;                               \
} while (0)

static void *
loader_malloc(uint64 size, char *error_buf, uint32 error_buf_size)
{
    void *mem;

    if (size >= UINT32_MAX
        || !(mem = wasm_runtime_malloc((uint32)size))) {
        set_error_buf(error_buf, error_buf_size,
                      "WASM module load failed: "
                      "allocate memory failed.");
        return NULL;
    }

    memset(mem, 0, (uint32)size);
    return mem;
}

static bool
check_utf8_str(const uint8* str, uint32 len)
{
    const uint8 *p = str, *p_end = str + len, *p_end1;
    uint8 chr, n_bytes;

    while (p < p_end) {
        chr = *p++;
        if (chr >= 0x80) {
            /* Calculate the byte count: the first byte must be
               110XXXXX, 1110XXXX, 11110XXX, 111110XX, or 1111110X,
               the count of leading '1' denotes the total byte count */
            n_bytes = 0;
            while ((chr & 0x80) != 0) {
                chr = (uint8)(chr << 1);
                n_bytes++;
            }

            /* Check byte count */
            if (n_bytes < 2 || n_bytes > 6
                || p + n_bytes - 1 > p_end)
                return false;

            /* Check the following bytes, which must be 10XXXXXX */
            p_end1 = p + n_bytes - 1;
            while (p < p_end1) {
                if (!(*p & 0x80) || (*p | 0x40))
                    return false;
                p++;
            }
        }
    }
    return true;
}

static char*
const_str_list_insert(const uint8 *str, uint32 len, WASMModule *module,
                     char* error_buf, uint32 error_buf_size)
{
    StringNode *node, *node_next;

    if (!check_utf8_str(str, len)) {
        set_error_buf(error_buf, error_buf_size,
                      "WASM module load failed: "
                      "invalid UTF-8 encoding");
        return NULL;
    }

    /* Search const str list */
    node = module->const_str_list;
    while (node) {
        node_next = node->next;
        if (strlen(node->str) == len
            && !memcmp(node->str, str, len))
            break;
        node = node_next;
    }

    if (node) {
        LOG_DEBUG("reuse %s", node->str);
        return node->str;
    }

    if (!(node = loader_malloc(sizeof(StringNode) + len + 1,
                               error_buf, error_buf_size))) {
        return NULL;
    }

    node->str = ((char*)node) + sizeof(StringNode);
    bh_memcpy_s(node->str, len + 1, str, len);
    node->str[len] = '\0';

    if (!module->const_str_list) {
        /* set as head */
        module->const_str_list = node;
        node->next = NULL;
    }
    else {
        /* insert it */
        node->next = module->const_str_list;
        module->const_str_list = node;
    }

    return node->str;
}

static bool
load_init_expr(const uint8 **p_buf, const uint8 *buf_end,
               InitializerExpression *init_expr, uint8 type,
               char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = *p_buf, *p_end = buf_end;
    uint8 flag, end_byte, *p_float;
    uint32 i;

    CHECK_BUF(p, p_end, 1);
    init_expr->init_expr_type = read_uint8(p);
    flag = init_expr->init_expr_type;

    switch (flag) {
        /* i32.const */
        case INIT_EXPR_TYPE_I32_CONST:
            if (type != VALUE_TYPE_I32)
                goto fail;
            read_leb_int32(p, p_end, init_expr->u.i32);
            break;
        /* i64.const */
        case INIT_EXPR_TYPE_I64_CONST:
            if (type != VALUE_TYPE_I64)
                goto fail;
            read_leb_int64(p, p_end, init_expr->u.i64);
            break;
        /* f32.const */
        case INIT_EXPR_TYPE_F32_CONST:
            if (type != VALUE_TYPE_F32)
                goto fail;
            CHECK_BUF(p, p_end, 4);
            p_float = (uint8*)&init_expr->u.f32;
            for (i = 0; i < sizeof(float32); i++)
                *p_float++ = *p++;
            break;
        /* f64.const */
        case INIT_EXPR_TYPE_F64_CONST:
            if (type != VALUE_TYPE_F64)
                goto fail;
            CHECK_BUF(p, p_end, 8);
            p_float = (uint8*)&init_expr->u.f64;
            for (i = 0; i < sizeof(float64); i++)
                *p_float++ = *p++;
            break;
        /* get_global */
        case INIT_EXPR_TYPE_GET_GLOBAL:
            read_leb_uint32(p, p_end, init_expr->u.global_index);
            break;
        default:
            goto fail;
    }
    CHECK_BUF(p, p_end, 1);
    end_byte = read_uint8(p);
    if (end_byte != 0x0b)
        goto fail;
    *p_buf = p;

    return true;
fail:
    set_error_buf(error_buf, error_buf_size,
                  "WASM module load failed: type mismatch or "
                  "constant expression required.");
    return false;
}

static bool
load_type_section(const uint8 *buf, const uint8 *buf_end, WASMModule *module,
                  char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = buf, *p_end = buf_end, *p_org;
    uint32 type_count, param_count, result_count, i, j;
    uint32 param_cell_num, ret_cell_num;
    uint64 total_size;
    uint8 flag;
    WASMType *type;

    read_leb_uint32(p, p_end, type_count);

    if (type_count) {
        module->type_count = type_count;
        total_size = sizeof(WASMType*) * (uint64)type_count;
        if (!(module->types = loader_malloc
                    (total_size, error_buf, error_buf_size))) {
            return false;
        }

        for (i = 0; i < type_count; i++) {
            CHECK_BUF(p, p_end, 1);
            flag = read_uint8(p);
            if (flag != 0x60) {
                set_error_buf(error_buf, error_buf_size,
                              "Load type section failed: invalid type flag.");
                return false;
            }

            read_leb_uint32(p, p_end, param_count);

            /* Resolve param count and result count firstly */
            p_org = p;
            CHECK_BUF(p, p_end, param_count);
            p += param_count;
            read_leb_uint32(p, p_end, result_count);
            CHECK_BUF(p, p_end, result_count);
            p = p_org;

            if (param_count > UINT16_MAX || result_count > UINT16_MAX) {
                set_error_buf(error_buf, error_buf_size,
                              "Load type section failed: "
                              "param count or result count too large");
                return false;
            }

            total_size = offsetof(WASMType, types) +
                         sizeof(uint8) * (uint64)(param_count + result_count);
            if (!(type = module->types[i] =
                        loader_malloc(total_size, error_buf, error_buf_size))) {
                return false;
            }

            /* Resolve param types and result types */
            type->param_count = (uint16)param_count;
            type->result_count = (uint16)result_count;
            for (j = 0; j < param_count; j++) {
                CHECK_BUF(p, p_end, 1);
                type->types[j] = read_uint8(p);
            }
            read_leb_uint32(p, p_end, result_count);
            for (j = 0; j < result_count; j++) {
                CHECK_BUF(p, p_end, 1);
                type->types[param_count + j] = read_uint8(p);
            }

            param_cell_num = wasm_get_cell_num(type->types, param_count);
            ret_cell_num = wasm_get_cell_num(type->types + param_count,
                                             result_count);
            if (param_cell_num > UINT16_MAX || ret_cell_num > UINT16_MAX) {
                set_error_buf(error_buf, error_buf_size,
                              "Load type section failed: "
                              "param count or result count too large");
                return false;
            }
            type->param_cell_num = (uint16)param_cell_num;
            type->ret_cell_num = (uint16)ret_cell_num;
        }
    }

    if (p != p_end) {
        set_error_buf(error_buf, error_buf_size,
                      "Load type section failed: section size mismatch");
        return false;
    }

    LOG_VERBOSE("Load type section success.\n");
    return true;
}



static bool
load_function_import(const WASMModule *parent_module, WASMModule *sub_module,
                     char *sub_module_name, char *function_name,
                     const uint8 **p_buf, const uint8 *buf_end,
                     WASMFunctionImport *function,
                     char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = *p_buf, *p_end = buf_end;
    uint32 declare_type_index = 0;
    WASMType *declare_func_type = NULL;
    WASMFunction *linked_func = NULL;
    const char *linked_signature = NULL;
    void *linked_attachment = NULL;
    bool linked_call_conv_raw = false;
    bool is_built_in_module = false;

    CHECK_BUF(p, p_end, 1);
    read_leb_uint32(p, p_end, declare_type_index);
    *p_buf = p;

    if (declare_type_index >= parent_module->type_count) {
        set_error_buf(error_buf, error_buf_size,
                      "Load import section failed: unknown type.");
        LOG_DEBUG("the type index is out of range");
        return false;
    }

    declare_func_type = parent_module->types[declare_type_index];

    is_built_in_module = wasm_runtime_is_built_in_module(sub_module_name);
    if (is_built_in_module) {
        LOG_DEBUG("%s is a function of a built-in module %s",
                  function_name,
                  sub_module_name);
        /* check built-in modules */
        linked_func = wasm_native_resolve_symbol(sub_module_name,
                                                 function_name,
                                                 declare_func_type,
                                                 &linked_signature,
                                                 &linked_attachment,
                                                 &linked_call_conv_raw);
    }
#if WASM_ENABLE_MULTI_MODULE != 0
    else {
        LOG_DEBUG("%s is a function of a sub-module %s",
                  function_name,
                  sub_module_name);
        linked_func = wasm_loader_resolve_function(sub_module_name,
                                                   function_name,
                                                   declare_func_type,
                                                   error_buf,
                                                   error_buf_size);
    }
#endif

    if (!linked_func) {
#if WASM_ENABLE_SPEC_TEST != 0
        set_error_buf(error_buf,
                      error_buf_size,
                      "unknown import or incompatible import type");
        return false;
#else
#if WASM_ENABLE_WAMR_COMPILER == 0
        LOG_WARNING(
          "warning: fail to link import function (%s, %s)",
          sub_module_name, function_name);
#endif
#endif
    }

    function->module_name = sub_module_name;
    function->field_name = function_name;
    function->func_type = declare_func_type;
    /* func_ptr_linked is for built-in functions */
    function->func_ptr_linked = is_built_in_module ? linked_func : NULL;
    function->signature = linked_signature;
    function->attachment = linked_attachment;
    function->call_conv_raw = linked_call_conv_raw;
#if WASM_ENABLE_MULTI_MODULE != 0
    function->import_module = is_built_in_module ? NULL : sub_module;
    /* can not set both func_ptr_linked and import_func_linked not NULL */
    function->import_func_linked = is_built_in_module ? NULL : linked_func;
#endif
    return true;
}

static bool
check_table_max_size(uint32 init_size, uint32 max_size,
                     char *error_buf, uint32 error_buf_size)
{
    if (max_size < init_size) {
        set_error_buf(error_buf, error_buf_size,
                      "size minimum must not be greater than maximum");
        return false;
    }
    return true;
}

static bool
load_table_import(WASMModule *sub_module, const char *sub_module_name,
                  const char *table_name, const uint8 **p_buf,
                  const uint8 *buf_end, WASMTableImport *table,
                  char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = *p_buf, *p_end = buf_end;
    uint32 declare_elem_type = 0;
    uint32 declare_max_size_flag = 0;
    uint32 declare_init_size = 0;
    uint32 declare_max_size = 0;
#if WASM_ENABLE_MULTI_MODULE != 0
    WASMTable *linked_table = NULL;
#endif

    CHECK_BUF(p, p_end, 1);
    /* 0x70 */
    declare_elem_type = read_uint8(p);
    if (TABLE_ELEM_TYPE_ANY_FUNC != declare_elem_type) {
        set_error_buf(error_buf, error_buf_size, "incompatible import type");
        return false;
    }

    read_leb_uint32(p, p_end, declare_max_size_flag);
    read_leb_uint32(p, p_end, declare_init_size);
    if (declare_max_size_flag & 1) {
        read_leb_uint32(p, p_end, declare_max_size);
        if (!check_table_max_size(table->init_size, table->max_size,
                                  error_buf, error_buf_size))
            return false;
    } else {
        declare_max_size = 0x10000;
    }
    *p_buf = p;

#if WASM_ENABLE_MULTI_MODULE != 0
    if (!wasm_runtime_is_built_in_module(sub_module_name)) {
        linked_table = wasm_loader_resolve_table(
                            sub_module_name, table_name,
                            declare_init_size, declare_max_size,
                            error_buf, error_buf_size);
        if (!linked_table) {
            LOG_DEBUG("(%s, %s) is not an exported from one of modules",
                      table_name, sub_module_name);
            return false;
        }

        /**
         * reset with linked table limit
         */
        declare_elem_type = linked_table->elem_type;
        declare_init_size = linked_table->init_size;
        declare_max_size = linked_table->max_size;
        declare_max_size_flag = linked_table->flags;
        table->import_table_linked = linked_table;
        table->import_module = sub_module;
    }
#endif

    /* (table (export "table") 10 20 funcref) */
    if (!strcmp("spectest", sub_module_name)) {
        uint32 spectest_table_init_size = 10;
        uint32 spectest_table_max_size = 20;

        if (strcmp("table", table_name)) {
            set_error_buf(error_buf, error_buf_size,
                          "incompatible import type or unknown import");
            return false;
        }

        if (declare_init_size > spectest_table_init_size
            || declare_max_size < spectest_table_max_size) {
            set_error_buf(error_buf, error_buf_size,
                          "incompatible import type");
            return false;
        }

        declare_init_size = spectest_table_init_size;
        declare_max_size = spectest_table_max_size;
    }

    /* now we believe all declaration are ok */
    table->elem_type = declare_elem_type;
    table->init_size = declare_init_size;
    table->flags = declare_max_size_flag;
    table->max_size = declare_max_size;
    return true;
}

// defined in /wasm_memory.c
unsigned
wasm_runtime_memory_pool_size();

static bool
check_memory_init_size(uint32 init_size,
                       char *error_buf, uint32 error_buf_size)
{
    if (init_size > 65536) {
        set_error_buf(error_buf, error_buf_size,
                      "memory size must be at most 65536 pages (4GiB)");
        return false;
    }
    return true;
}

static bool
check_memory_max_size(uint32 init_size, uint32 max_size,
                      char *error_buf, uint32 error_buf_size)
{
    if (max_size < init_size) {
        set_error_buf(error_buf, error_buf_size,
                      "size minimum must not be greater than maximum");
        return false;
    }

    if (max_size > 65536) {
        set_error_buf(error_buf, error_buf_size,
                      "memory size must be at most 65536 pages (4GiB)");
        return false;
    }
    return true;
}

static bool
load_memory_import(WASMModule *sub_module, const char *sub_module_name,
                   const char *memory_name, const uint8 **p_buf,
                   const uint8 *buf_end, WASMMemoryImport *memory,
                   char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = *p_buf, *p_end = buf_end;
    uint32 pool_size = wasm_runtime_memory_pool_size();
#if WASM_ENABLE_APP_FRAMEWORK != 0
    uint32 max_page_count = pool_size * APP_MEMORY_MAX_GLOBAL_HEAP_PERCENT
                            / DEFAULT_NUM_BYTES_PER_PAGE;
#else
    uint32 max_page_count = pool_size / DEFAULT_NUM_BYTES_PER_PAGE;
#endif /* WASM_ENABLE_APP_FRAMEWORK */
    uint32 declare_max_page_count_flag = 0;
    uint32 declare_init_page_count = 0;
    uint32 declare_max_page_count = 0;
#if WASM_ENABLE_MULTI_MODULE != 0
    WASMMemory *linked_memory = NULL;
#endif

    read_leb_uint32(p, p_end, declare_max_page_count_flag);
    read_leb_uint32(p, p_end, declare_init_page_count);
    if (!check_memory_init_size(declare_init_page_count, error_buf,
                                error_buf_size)) {
        return false;
    }

    if (declare_max_page_count_flag & 1) {
        read_leb_uint32(p, p_end, declare_max_page_count);
        if (!check_memory_max_size(declare_init_page_count,
                                   declare_max_page_count, error_buf,
                                   error_buf_size)) {
            return false;
        }
        if (declare_max_page_count > max_page_count) {
            declare_max_page_count = max_page_count;
        }
    }
    else {
        /* Limit the maximum memory size to max_page_count */
        declare_max_page_count = max_page_count;
    }

#if WASM_ENABLE_MULTI_MODULE != 0
    if (!wasm_runtime_is_built_in_module(sub_module_name)) {
        linked_memory = wasm_loader_resolve_memory(
                    sub_module_name, memory_name,
                    declare_init_page_count, declare_max_page_count,
                    error_buf, error_buf_size);
        if (!linked_memory) {
            return false;
        }

        /**
         * reset with linked memory limit
         */
        memory->import_module = sub_module;
        memory->import_memory_linked = linked_memory;
        declare_init_page_count = linked_memory->init_page_count;
        declare_max_page_count = linked_memory->max_page_count;
    }
#endif

    /* (memory (export "memory") 1 2) */
    if (!strcmp("spectest", sub_module_name)) {
        uint32 spectest_memory_init_page = 1;
        uint32 spectest_memory_max_page = 2;

        if (strcmp("memory", memory_name)) {
            set_error_buf(error_buf, error_buf_size,
                          "incompatible import type or unknown import");
            return false;
        }

        if (declare_init_page_count > spectest_memory_init_page
            || declare_max_page_count < spectest_memory_max_page) {
            set_error_buf(error_buf, error_buf_size,
                          "incompatible import type");
            return false;
        }

        declare_init_page_count = spectest_memory_init_page;
        declare_max_page_count = spectest_memory_max_page;
    }

    /* now we believe all declaration are ok */
    memory->flags = declare_max_page_count_flag;
    memory->init_page_count = declare_init_page_count;
    memory->max_page_count = declare_max_page_count;
    memory->num_bytes_per_page = DEFAULT_NUM_BYTES_PER_PAGE;

    *p_buf = p;
    return true;
}

static bool
load_global_import(const WASMModule *parent_module,
                   WASMModule *sub_module,
                   char *sub_module_name, char *global_name,
                   const uint8 **p_buf, const uint8 *buf_end,
                   WASMGlobalImport *global,
                   char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = *p_buf, *p_end = buf_end;
    uint8 declare_type = 0;
    uint8 declare_mutable = 0;
    bool is_mutable = false;

    CHECK_BUF(p, p_end, 2);
    declare_type = read_uint8(p);
    declare_mutable = read_uint8(p);
    *p_buf = p;

    if (declare_mutable >= 2) {
        set_error_buf(error_buf, error_buf_size,
                      "Load import section failed: "
                      "invalid mutability");
        return false;
    }

    is_mutable = declare_mutable & 1 ? true : false;



    global->module_name = sub_module_name;
    global->field_name = global_name;
    global->type = declare_type;
    global->is_mutable = is_mutable;
    return true;
}

static bool
load_table(const uint8 **p_buf, const uint8 *buf_end, WASMTable *table,
           char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = *p_buf, *p_end = buf_end;

    CHECK_BUF(p, p_end, 1);
    /* 0x70 */
    table->elem_type = read_uint8(p);
    if (TABLE_ELEM_TYPE_ANY_FUNC != table->elem_type) {
        set_error_buf(error_buf, error_buf_size, "incompatible import type");
        return false;
    }

    read_leb_uint32(p, p_end, table->flags);
    read_leb_uint32(p, p_end, table->init_size);
    if (table->flags & 1) {
        read_leb_uint32(p, p_end, table->max_size);
        if (!check_table_max_size(table->init_size, table->max_size,
                                  error_buf, error_buf_size))
            return false;
    }
    else
        table->max_size = 0x10000;

    if ((table->flags & 1) && table->init_size > table->max_size) {
        set_error_buf(error_buf, error_buf_size,
                      "size minimum must not be greater than maximum");
        return false;
    }

    *p_buf = p;
    return true;
}

static bool
load_memory(const uint8 **p_buf, const uint8 *buf_end, WASMMemory *memory,
            char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = *p_buf, *p_end = buf_end;
    uint32 pool_size = wasm_runtime_memory_pool_size();

    uint32 max_page_count = pool_size / DEFAULT_NUM_BYTES_PER_PAGE;


    read_leb_uint32(p, p_end, memory->flags);
    read_leb_uint32(p, p_end, memory->init_page_count);
    if (!check_memory_init_size(memory->init_page_count,
                                error_buf, error_buf_size))
        return false;

    if (memory->flags & 1) {
        read_leb_uint32(p, p_end, memory->max_page_count);
        if (!check_memory_max_size(memory->init_page_count,
                                   memory->max_page_count,
                                   error_buf, error_buf_size))
                return false;
        if (memory->max_page_count > max_page_count)
            memory->max_page_count = max_page_count;
    }
    else
        /* Limit the maximum memory size to max_page_count */
        memory->max_page_count = max_page_count;

    memory->num_bytes_per_page = DEFAULT_NUM_BYTES_PER_PAGE;

    *p_buf = p;
    return true;
}



static bool
load_import_section(const uint8 *buf, const uint8 *buf_end, WASMModule *module,
                    char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = buf, *p_end = buf_end, *p_old;
    uint32 import_count, name_len, type_index, i, u32, flags;
    uint64 total_size;
    WASMImport *import;
    WASMImport *import_functions = NULL, *import_tables = NULL;
    WASMImport *import_memories = NULL, *import_globals = NULL;
    char *sub_module_name, *field_name;
    uint8 u8, kind;

    read_leb_uint32(p, p_end, import_count);

    if (import_count) {
        module->import_count = import_count;
        total_size = sizeof(WASMImport) * (uint64)import_count;
        if (!(module->imports = loader_malloc
                    (total_size, error_buf, error_buf_size))) {
            return false;
        }

        p_old = p;

        /* Scan firstly to get import count of each type */
        for (i = 0; i < import_count; i++) {
            /* module name */
            read_leb_uint32(p, p_end, name_len);
            CHECK_BUF(p, p_end, name_len);
            p += name_len;

            /* field name */
            read_leb_uint32(p, p_end, name_len);
            CHECK_BUF(p, p_end, name_len);
            p += name_len;

            CHECK_BUF(p, p_end, 1);
            /* 0x00/0x01/0x02/0x03 */
            kind = read_uint8(p);

            switch (kind) {
                case IMPORT_KIND_FUNC: /* import function */
                    read_leb_uint32(p, p_end, type_index);
                    module->import_function_count++;
                    break;

                case IMPORT_KIND_TABLE: /* import table */
                    CHECK_BUF(p, p_end, 1);
                    /* 0x70 */
                    u8 = read_uint8(p);
                    read_leb_uint32(p, p_end, flags);
                    read_leb_uint32(p, p_end, u32);
                    if (flags & 1)
                        read_leb_uint32(p, p_end, u32);
                    module->import_table_count++;
                    if (module->import_table_count > 1) {
                        set_error_buf(error_buf, error_buf_size,
                                      "Load import section failed: multiple tables");
                        return false;
                    }
                    break;

                case IMPORT_KIND_MEMORY: /* import memory */
                    read_leb_uint32(p, p_end, flags);
                    read_leb_uint32(p, p_end, u32);
                    if (flags & 1)
                        read_leb_uint32(p, p_end, u32);
                    module->import_memory_count++;
                    if (module->import_memory_count > 1) {
                        set_error_buf(error_buf, error_buf_size,
                                      "Load import section failed: multiple memories");
                        return false;
                    }
                    break;

                case IMPORT_KIND_GLOBAL: /* import global */
                    CHECK_BUF(p, p_end, 2);
                    p += 2;
                    module->import_global_count++;
                    break;

                default:
                    set_error_buf(error_buf, error_buf_size,
                                  "Load import section failed: invalid import type.");
                    return false;
            }
        }

        if (module->import_function_count)
            import_functions = module->import_functions = module->imports;
        if (module->import_table_count)
            import_tables = module->import_tables =
                module->imports + module->import_function_count;
        if (module->import_memory_count)
            import_memories = module->import_memories =
                module->imports + module->import_function_count + module->import_table_count;
        if (module->import_global_count)
            import_globals = module->import_globals =
                module->imports + module->import_function_count + module->import_table_count
                + module->import_memory_count;

        p = p_old;

        // TODO: move it out of the loop
        /* insert "env", "wasi_unstable" and "wasi_snapshot_preview1" to const str list */
        if (!const_str_list_insert((uint8*)"env", 3, module, error_buf, error_buf_size)
            || !const_str_list_insert((uint8*)"wasi_unstable", 13, module,
                                     error_buf, error_buf_size)
            || !const_str_list_insert((uint8*)"wasi_snapshot_preview1", 22, module,
                                     error_buf, error_buf_size)) {
            return false;
        }

        /* Scan again to read the data */
        for (i = 0; i < import_count; i++) {
            WASMModule *sub_module = NULL;

            /* load module name */
            read_leb_uint32(p, p_end, name_len);
            CHECK_BUF(p, p_end, name_len);
            if (!(sub_module_name = const_str_list_insert(
                    p, name_len, module, error_buf, error_buf_size))) {
                return false;
            }
            p += name_len;

            /* load field name */
            read_leb_uint32(p, p_end, name_len);
            CHECK_BUF(p, p_end, name_len);
            if (!(field_name = const_str_list_insert(
                    p, name_len, module, error_buf, error_buf_size))) {
                return false;
            }
            p += name_len;

            LOG_DEBUG("import #%d: (%s, %s)", i, sub_module_name, field_name);


            CHECK_BUF(p, p_end, 1);
            /* 0x00/0x01/0x02/0x03 */
            kind = read_uint8(p);
            switch (kind) {
                case IMPORT_KIND_FUNC: /* import function */
                    bh_assert(import_functions);
                    import = import_functions++;
                    if (!load_function_import(module, sub_module,
                                              sub_module_name, field_name, &p,
                                              p_end, &import->u.function,
                                              error_buf, error_buf_size)) {
                        return false;
                    }
                    break;

                case IMPORT_KIND_TABLE: /* import table */
                    bh_assert(import_tables);
                    import = import_tables++;
                    if (!load_table_import(sub_module,
                                           sub_module_name,
                                           field_name,
                                           &p,
                                           p_end,
                                           &import->u.table,
                                           error_buf,
                                           error_buf_size)) {
                        LOG_DEBUG("can not import such a table (%s,%s)",
                                  sub_module_name, field_name);
                        return false;
                    }
                    break;

                case IMPORT_KIND_MEMORY: /* import memory */
                    bh_assert(import_memories);
                    import = import_memories++;
                    if (!load_memory_import(sub_module,
                                            sub_module_name,
                                            field_name,
                                            &p,
                                            p_end,
                                            &import->u.memory,
                                            error_buf,
                                            error_buf_size)) {
                        return false;
                    }
                    break;

                case IMPORT_KIND_GLOBAL: /* import global */
                    bh_assert(import_globals);
                    import = import_globals++;
                    if (!load_global_import(module, sub_module,
                                            sub_module_name, field_name,
                                            &p, p_end, &import->u.global,
                                            error_buf, error_buf_size)) {
                        return false;
                    }
                    break;

                default:
                    set_error_buf(error_buf, error_buf_size,
                                  "Load import section failed: "
                                  "invalid import type.");
                    return false;
            }
            import->kind = kind;
            import->u.names.module_name = sub_module_name;
            import->u.names.field_name = field_name;
            (void)sub_module;
        }


    }

    if (p != p_end) {
        set_error_buf(error_buf, error_buf_size,
                      "Load import section failed: section size mismatch");
        return false;
    }

    LOG_VERBOSE("Load import section success.\n");
    (void)u8;
    (void)u32;
    (void)type_index;
    return true;
}

static bool
init_function_local_offsets(WASMFunction *func,
                            char *error_buf, uint32 error_buf_size)
{
    WASMType *param_type = func->func_type;
    uint32 param_count = param_type->param_count;
    uint8 *param_types = param_type->types;
    uint32 local_count = func->local_count;
    uint8 *local_types = func->local_types;
    uint32 i, local_offset = 0;
    uint64 total_size = sizeof(uint16) * ((uint64)param_count + local_count);

    if (!(func->local_offsets =
                loader_malloc(total_size, error_buf, error_buf_size))) {
        return false;
    }

    for (i = 0; i < param_count; i++) {
        func->local_offsets[i] = (uint16)local_offset;
        local_offset += wasm_value_type_cell_num(param_types[i]);
    }

    for (i = 0; i < local_count; i++) {
        func->local_offsets[param_count + i] = (uint16)local_offset;
        local_offset += wasm_value_type_cell_num(local_types[i]);
    }

    bh_assert(local_offset == func->param_cell_num + func->local_cell_num);
    return true;
}

static bool
load_function_section(const uint8 *buf, const uint8 *buf_end,
                      const uint8 *buf_code, const uint8 *buf_code_end,
                      WASMModule *module,
                      char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = buf, *p_end = buf_end;
    const uint8 *p_code = buf_code, *p_code_end, *p_code_save;
    uint32 func_count;
    uint64 total_size;
    uint32 code_count = 0, code_size, type_index, i, j, k, local_type_index;
    uint32 local_count, local_set_count, sub_local_count;
    uint8 type;
    WASMFunction *func;

    read_leb_uint32(p, p_end, func_count);

    if (buf_code)
        read_leb_uint32(p_code, buf_code_end, code_count);

    if (func_count != code_count) {
        set_error_buf(error_buf, error_buf_size,
                      "Load function section failed: "
                      "function and code section have inconsistent lengths");
        return false;
    }

    if (func_count) {
        module->function_count = func_count;
        total_size = sizeof(WASMFunction*) * (uint64)func_count;
        if (!(module->functions =
                    loader_malloc(total_size, error_buf, error_buf_size))) {
            return false;
        }

        for (i = 0; i < func_count; i++) {
            /* Resolve function type */
            read_leb_uint32(p, p_end, type_index);
            if (type_index >= module->type_count) {
                set_error_buf(error_buf, error_buf_size,
                              "Load function section failed: "
                              "unknown type.");
                return false;
            }

            read_leb_uint32(p_code, buf_code_end, code_size);
            if (code_size == 0
                || p_code + code_size > buf_code_end) {
                set_error_buf(error_buf, error_buf_size,
                              "Load function section failed: "
                              "invalid function code size.");
                return false;
            }

            /* Resolve local set count */
            p_code_end = p_code + code_size;
            local_count = 0;
            read_leb_uint32(p_code, buf_code_end, local_set_count);
            p_code_save = p_code;

            /* Calculate total local count */
            for (j = 0; j < local_set_count; j++) {
                read_leb_uint32(p_code, buf_code_end, sub_local_count);
                if (sub_local_count > UINT32_MAX - local_count) {
                    set_error_buf(error_buf, error_buf_size,
                                  "Load function section failed: "
                                  "too many locals");
                    return false;
                }
                CHECK_BUF(p_code, buf_code_end, 1);
                /* 0x7F/0x7E/0x7D/0x7C */
                type = read_uint8(p_code);
                local_count += sub_local_count;
            }

            /* Alloc memory, layout: function structure + local types */
            code_size = (uint32)(p_code_end - p_code);

            total_size = sizeof(WASMFunction) + (uint64)local_count;
            if (!(func = module->functions[i] =
                        loader_malloc(total_size, error_buf, error_buf_size))) {
                return false;
            }

            /* Set function type, local count, code size and code body */
            func->func_type = module->types[type_index];
            func->local_count = local_count;
            if (local_count > 0)
                func->local_types = (uint8*)func + sizeof(WASMFunction);
            func->code_size = code_size;
            /*
             * we shall make a copy of code body [p_code, p_code + code_size]
             * when we are worrying about inappropriate releasing behaviour.
             * all code bodies are actually in a buffer which user allocates in
             * his embedding environment and we don't have power on them.
             * it will be like:
             * code_body_cp = malloc(code_size);
             * memcpy(code_body_cp, p_code, code_size);
             * func->code = code_body_cp;
             */
            func->code = (uint8*)p_code;

            /* Load each local type */
            p_code = p_code_save;
            local_type_index = 0;
            for (j = 0; j < local_set_count; j++) {
                read_leb_uint32(p_code, buf_code_end, sub_local_count);
                if (local_type_index + sub_local_count <= local_type_index
                    || local_type_index + sub_local_count > local_count) {
                    set_error_buf(error_buf, error_buf_size,
                                  "Load function section failed: "
                                  "invalid local count.");
                    return false;
                }
                CHECK_BUF(p_code, buf_code_end, 1);
                /* 0x7F/0x7E/0x7D/0x7C */
                type = read_uint8(p_code);
                if (type < VALUE_TYPE_F64 || type > VALUE_TYPE_I32) {
                    set_error_buf(error_buf, error_buf_size,
                                  "Load function section failed: "
                                  "invalid local type.");
                    return false;
                }
                for (k = 0; k < sub_local_count; k++) {
                    func->local_types[local_type_index++] = type;
                }
            }

            func->param_cell_num = func->func_type->param_cell_num;
            func->ret_cell_num = func->func_type->ret_cell_num;
            func->local_cell_num =
                wasm_get_cell_num(func->local_types, func->local_count);

            if (!init_function_local_offsets(func, error_buf, error_buf_size))
                return false;

            p_code = p_code_end;
        }
    }

    if (p != p_end) {
        set_error_buf(error_buf, error_buf_size,
                      "Load function section failed: section size mismatch");
        return false;
    }

    LOG_VERBOSE("Load function section success.\n");
    return true;
}

static bool
load_table_section(const uint8 *buf, const uint8 *buf_end, WASMModule *module,
                   char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = buf, *p_end = buf_end;
    uint32 table_count, i;
    uint64 total_size;
    WASMTable *table;

    read_leb_uint32(p, p_end, table_count);
    /* a total of one table is allowed */
    if (module->import_table_count + table_count > 1) {
        set_error_buf(error_buf, error_buf_size, "multiple tables");
        return false;
    }

    if (table_count) {
        module->table_count = table_count;
        total_size = sizeof(WASMTable) * (uint64)table_count;
        if (!(module->tables = loader_malloc
                    (total_size, error_buf, error_buf_size))) {
            return false;
        }

        /* load each table */
        table = module->tables;
        for (i = 0; i < table_count; i++, table++)
            if (!load_table(&p, p_end, table, error_buf, error_buf_size))
                return false;
    }

    if (p != p_end) {
        set_error_buf(error_buf, error_buf_size,
                      "Load table section failed: section size mismatch");
        return false;
    }

    LOG_VERBOSE("Load table section success.\n");
    return true;
}

static bool
load_memory_section(const uint8 *buf, const uint8 *buf_end, WASMModule *module,
                    char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = buf, *p_end = buf_end;
    uint32 memory_count, i;
    uint64 total_size;
    WASMMemory *memory;

    read_leb_uint32(p, p_end, memory_count);
    /* a total of one memory is allowed */
    if (module->import_memory_count + memory_count > 1) {
        set_error_buf(error_buf, error_buf_size, "multiple memories");
        return false;
    }

    if (memory_count) {
        module->memory_count = memory_count;
        total_size = sizeof(WASMMemory) * (uint64)memory_count;
        if (!(module->memories = loader_malloc
                    (total_size, error_buf, error_buf_size))) {
            return false;
        }

        /* load each memory */
        memory = module->memories;
        for (i = 0; i < memory_count; i++, memory++)
            if (!load_memory(&p, p_end, memory, error_buf, error_buf_size))
                return false;
    }

    if (p != p_end) {
        set_error_buf(error_buf, error_buf_size,
                      "Load memory section failed: section size mismatch");
        return false;
    }

    LOG_VERBOSE("Load memory section success.\n");
    return true;
}

static bool
load_global_section(const uint8 *buf, const uint8 *buf_end, WASMModule *module,
                    char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = buf, *p_end = buf_end;
    uint32 global_count, i;
    uint64 total_size;
    WASMGlobal *global;
    uint8 mutable;

    read_leb_uint32(p, p_end, global_count);

    if (global_count) {
        module->global_count = global_count;
        total_size = sizeof(WASMGlobal) * (uint64)global_count;
        if (!(module->globals = loader_malloc
                    (total_size, error_buf, error_buf_size))) {
            return false;
        }

        global = module->globals;

        for(i = 0; i < global_count; i++, global++) {
            CHECK_BUF(p, p_end, 2);
            global->type = read_uint8(p);
            mutable = read_uint8(p);
            if (mutable >= 2) {
                set_error_buf(error_buf, error_buf_size,
                              "Load import section failed: "
                              "invalid mutability");
                return false;
            }
            global->is_mutable = mutable ? true : false;

            /* initialize expression */
            if (!load_init_expr(&p, p_end, &(global->init_expr),
                                global->type, error_buf, error_buf_size))
                return false;

            if (INIT_EXPR_TYPE_GET_GLOBAL == global->init_expr.init_expr_type) {
                /**
                 * Currently, constant expressions occurring as initializers
                 * of globals are further constrained in that contained
                 * global.get instructions are
                 * only allowed to refer to imported globals.
                 */
                uint32 target_global_index = global->init_expr.u.global_index;
                if (target_global_index >= module->import_global_count) {
                    set_error_buf(error_buf, error_buf_size, "unknown global");
                    return false;
                }
            }
        }
    }

    if (p != p_end) {
        set_error_buf(error_buf, error_buf_size,
                      "Load global section failed: section size mismatch");
        return false;
    }

    LOG_VERBOSE("Load global section success.\n");
    return true;
}

static bool
load_export_section(const uint8 *buf, const uint8 *buf_end, WASMModule *module,
                    char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = buf, *p_end = buf_end;
    uint32 export_count, i, j, index;
    uint64 total_size;
    uint32 str_len;
    WASMExport *export;
    const char *name;

    read_leb_uint32(p, p_end, export_count);

    if (export_count) {
        module->export_count = export_count;
        total_size = sizeof(WASMExport) * (uint64)export_count;
        if (!(module->exports = loader_malloc
                    (total_size, error_buf, error_buf_size))) {
            return false;
        }

        export = module->exports;
        for (i = 0; i < export_count; i++, export++) {
            read_leb_uint32(p, p_end, str_len);
            CHECK_BUF(p, p_end, str_len);

            for (j = 0; j < i; j++) {
                name = module->exports[j].name;
                if (strlen(name) == str_len
                    && memcmp(name, p, str_len) == 0) {
                   set_error_buf(error_buf, error_buf_size,
                                 "duplicate export name");
                   return false;
                }
            }

            if (!(export->name = const_str_list_insert(p, str_len, module,
                            error_buf, error_buf_size))) {
                return false;
            }

            p += str_len;
            CHECK_BUF(p, p_end, 1);
            export->kind = read_uint8(p);
            read_leb_uint32(p, p_end, index);
            export->index = index;

            switch(export->kind) {
                /*function index*/
                case EXPORT_KIND_FUNC:
                    if (index >= module->function_count + module->import_function_count) {
                        set_error_buf(error_buf, error_buf_size,
                                      "Load export section failed: "
                                      "unknown function.");
                        return false;
                    }
                    break;
                /*table index*/
                case EXPORT_KIND_TABLE:
                    if (index >= module->table_count + module->import_table_count) {
                        set_error_buf(error_buf, error_buf_size,
                                      "Load export section failed: "
                                      "unknown table.");
                        return false;
                    }
                    break;
                /*memory index*/
                case EXPORT_KIND_MEMORY:
                    if (index >= module->memory_count + module->import_memory_count) {
                        set_error_buf(error_buf, error_buf_size,
                                      "Load export section failed: "
                                      "unknown memory.");
                        return false;
                    }
                    break;
                /*global index*/
                case EXPORT_KIND_GLOBAL:
                    if (index >= module->global_count + module->import_global_count) {
                        set_error_buf(error_buf, error_buf_size,
                                      "Load export section failed: "
                                      "unknown global.");
                        return false;
                    }
                    break;
                default:
                    set_error_buf(error_buf, error_buf_size,
                                  "Load export section failed: "
                                  "invalid export kind.");
                    return false;
            }
        }
    }

    if (p != p_end) {
        set_error_buf(error_buf, error_buf_size,
                      "Load export section failed: section size mismatch");
        return false;
    }

    LOG_VERBOSE("Load export section success.\n");
    return true;
}

static bool
load_table_segment_section(const uint8 *buf, const uint8 *buf_end, WASMModule *module,
                           char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = buf, *p_end = buf_end;
    uint32 table_segment_count, i, j, table_index, function_count, function_index;
    uint64 total_size;
    WASMTableSeg *table_segment;

    read_leb_uint32(p, p_end, table_segment_count);

    if (table_segment_count) {
        module->table_seg_count = table_segment_count;
        total_size = sizeof(WASMTableSeg) * (uint64)table_segment_count;
        if (!(module->table_segments = loader_malloc
                (total_size, error_buf, error_buf_size))) {
            return false;
        }

        table_segment = module->table_segments;
        for (i = 0; i < table_segment_count; i++, table_segment++) {
            if (p >= p_end) {
                set_error_buf(error_buf, error_buf_size,
                              "Load table segment section failed: "
                              "unexpected end");
                return false;
            }
            read_leb_uint32(p, p_end, table_index);
            if (table_index
                >= module->import_table_count + module->table_count) {
                LOG_DEBUG("table#%d does not exist", table_index);
                set_error_buf(error_buf, error_buf_size, "unknown table");
                return false;
            }

            table_segment->table_index = table_index;

            /* initialize expression */
            if (!load_init_expr(&p, p_end, &(table_segment->base_offset),
                                VALUE_TYPE_I32, error_buf, error_buf_size))
                return false;

            read_leb_uint32(p, p_end, function_count);
            table_segment->function_count = function_count;
            total_size = sizeof(uint32) * (uint64)function_count;
            if (!(table_segment->func_indexes = (uint32 *)
                    loader_malloc(total_size, error_buf, error_buf_size))) {
                return false;
            }
            for (j = 0; j < function_count; j++) {
                read_leb_uint32(p, p_end, function_index);
                if (function_index >= module->import_function_count
                                      + module->function_count) {
                    set_error_buf(error_buf, error_buf_size,
                                  "Load table segment section failed: "
                                  "unknown function");
                    return false;
                }
                table_segment->func_indexes[j] = function_index;
            }
        }
    }

    if (p != p_end) {
        set_error_buf(error_buf, error_buf_size,
                     "Load table segment section failed: section size mismatch");
        return false;
    }

    LOG_VERBOSE("Load table segment section success.\n");
    return true;
}

static bool
load_data_segment_section(const uint8 *buf, const uint8 *buf_end,
                          WASMModule *module,
                          char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = buf, *p_end = buf_end;
    uint32 data_seg_count, i, mem_index, data_seg_len;
    uint64 total_size;
    WASMDataSeg *dataseg;
    InitializerExpression init_expr;
    read_leb_uint32(p, p_end, data_seg_count);
    if (data_seg_count) {
        module->data_seg_count = data_seg_count;
        total_size = sizeof(WASMDataSeg*) * (uint64)data_seg_count;
        if (!(module->data_segments = loader_malloc
                    (total_size, error_buf, error_buf_size))) {
            return false;
        }

        for (i = 0; i < data_seg_count; i++) {
            read_leb_uint32(p, p_end, mem_index);
#if WASM_ENABLE_BULK_MEMORY != 0
            is_passive = false;
            mem_flag = mem_index & 0x03;
            switch (mem_flag) {
                case 0x01:
                    is_passive = true;
                    break;
                case 0x00:
                    /* no memory index, treat index as 0 */
                    mem_index = 0;
                    goto check_mem_index;
                case 0x02:
                    /* read following memory index */
                    read_leb_uint32(p, p_end, mem_index);
check_mem_index:
                    if (mem_index
                        >= module->import_memory_count + module->memory_count) {
                        LOG_DEBUG("memory#%d does not exist", mem_index);
                        set_error_buf(error_buf, error_buf_size, "unknown memory");
                        return false;
                    }
                    break;
                case 0x03:
                default:
                    set_error_buf(error_buf, error_buf_size, "unknown memory");
                        return false;
                    break;
            }
#else
            if (mem_index
                >= module->import_memory_count + module->memory_count) {
                LOG_DEBUG("memory#%d does not exist", mem_index);
                set_error_buf(error_buf, error_buf_size, "unknown memory");
                return false;
            }
#endif /* WASM_ENABLE_BULK_MEMORY */

#if WASM_ENABLE_BULK_MEMORY != 0
            if (!is_passive)
#endif
                if (!load_init_expr(&p, p_end, &init_expr, VALUE_TYPE_I32,
                                    error_buf, error_buf_size))
                    return false;

            read_leb_uint32(p, p_end, data_seg_len);

            if (!(dataseg = module->data_segments[i] = loader_malloc
                        (sizeof(WASMDataSeg), error_buf, error_buf_size))) {
                return false;
            }

#if WASM_ENABLE_BULK_MEMORY != 0
            dataseg->is_passive = is_passive;
            if (!is_passive)
#endif
            {
                bh_memcpy_s(&dataseg->base_offset, sizeof(InitializerExpression),
                            &init_expr, sizeof(InitializerExpression));

                dataseg->memory_index = mem_index;
            }

            dataseg->data_length = data_seg_len;
            CHECK_BUF(p, p_end, data_seg_len);
            dataseg->data = (uint8*)p;
            p += data_seg_len;
        }
    }
	//dump ("data_section",dataseg->data ,dataseg->data_length);
    if (p != p_end) {
        set_error_buf(error_buf, error_buf_size,
                      "Load data segment section failed: section size mismatch");
        return false;
    }

    LOG_VERBOSE("Load data segment section success.\n");
    return true;
}


static bool
load_code_section(const uint8 *buf, const uint8 *buf_end,
                  const uint8 *buf_func,
                  const uint8 *buf_func_end,
                  WASMModule *module,
                  char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = buf, *p_end = buf_end;
    const uint8 *p_func = buf_func;
    uint32 func_count = 0, code_count;

    /* code has been loaded in function section, so pass it here, just check
     * whether function and code section have inconsistent lengths */
    read_leb_uint32(p, p_end, code_count);

    if (buf_func)
        read_leb_uint32(p_func, buf_func_end, func_count);

    if (func_count != code_count) {
        set_error_buf(error_buf, error_buf_size,
                      "Load code section failed: "
                      "function and code section have inconsistent lengths");
        return false;
    }

    LOG_VERBOSE("Load code segment section success.\n");
    return true;
}

static bool
load_start_section(const uint8 *buf, const uint8 *buf_end, WASMModule *module,
                   char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = buf, *p_end = buf_end;
    WASMType *type;
    uint32 start_function;

    read_leb_uint32(p, p_end, start_function);

    if (start_function
        >= module->function_count + module->import_function_count) {
        set_error_buf(error_buf, error_buf_size,
                      "Load start section failed: "
                      "unknown function.");
        return false;
    }

    if (start_function < module->import_function_count)
        type = module->import_functions[start_function].u.function.func_type;
    else
        type =
          module->functions[start_function - module->import_function_count]
            ->func_type;
    if (type->param_count != 0 || type->result_count != 0) {
        set_error_buf(error_buf, error_buf_size,
                      "Load start section failed: "
                      "invalid start function.");
        return false;
    }

    module->start_function = start_function;

    if (p != p_end) {
        set_error_buf(error_buf, error_buf_size,
                      "Load start section failed: section size mismatch");
        return false;
    }

    LOG_VERBOSE("Load start section success.\n");
    return true;
}

static bool
load_user_section(const uint8 *buf, const uint8 *buf_end, WASMModule *module,
                  char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = buf, *p_end = buf_end;
    uint32 name_len;

    if (p >= p_end) {
        set_error_buf(error_buf, error_buf_size,
                      "Load custom section failed: unexpected end");
        return false;
    }

    read_leb_uint32(p, p_end, name_len);

    if (name_len == 0
        || p + name_len > p_end) {
        set_error_buf(error_buf, error_buf_size,
                      "Load custom section failed: unexpected end");
        return false;
    }

    if (!check_utf8_str(p, name_len)) {
        set_error_buf(error_buf, error_buf_size,
                      "WASM module load failed: "
                      "invalid UTF-8 encoding");
        return false;
    }

    LOG_VERBOSE("Load custom section success.\n");
    return true;
}


static bool
wasm_loader_prepare_bytecode(WASMModule *module, WASMFunction *func,
                             BlockAddr *block_addr_cache,
                             char *error_buf, uint32 error_buf_size);

#if WASM_ENABLE_FAST_INTERP != 0
void **
wasm_interp_get_handle_table();

static void **handle_table;
#endif

static bool
load_from_sections(WASMModule *module, WASMSection *sections,
                   char *error_buf, uint32 error_buf_size)
{
    WASMExport *export;
    WASMSection *section = sections;
    const uint8 *buf, *buf_end, *buf_code = NULL, *buf_code_end = NULL,
                *buf_func = NULL, *buf_func_end = NULL;
    WASMGlobal *llvm_data_end_global = NULL, *llvm_heap_base_global = NULL;
    WASMGlobal *llvm_stack_top_global = NULL, *global;
    uint32 llvm_data_end = UINT32_MAX, llvm_heap_base = UINT32_MAX;
    uint32 llvm_stack_top = UINT32_MAX, global_index, i;
    uint32 stack_top_global_index = UINT32_MAX;
    BlockAddr *block_addr_cache;
    uint64 total_size;

    /* Find code and function sections if have */
    while (section) {
        if (section->section_type == SECTION_TYPE_CODE) {
            buf_code = section->section_body;
            buf_code_end = buf_code + section->section_body_size;
        }
        else if (section->section_type == SECTION_TYPE_FUNC) {
            buf_func = section->section_body;
            buf_func_end = buf_func + section->section_body_size;
        }
        section = section->next;
    }

    section = sections;
    while (section) {
        buf = section->section_body;
        buf_end = buf + section->section_body_size;
        LOG_DEBUG("to section %d", section->section_type);
        switch (section->section_type) {
            case SECTION_TYPE_USER:
                /* unsupported user section, ignore it. */
                if (!load_user_section(buf, buf_end, module, error_buf, error_buf_size))
                    return false;
                break;
            case SECTION_TYPE_TYPE:
                if (!load_type_section(buf, buf_end, module, error_buf, error_buf_size))
                    return false;
                break;
            case SECTION_TYPE_IMPORT:
                if (!load_import_section(buf, buf_end, module, error_buf, error_buf_size))
                    return false;
                break;
            case SECTION_TYPE_FUNC:
                if (!load_function_section(buf, buf_end, buf_code, buf_code_end,
                            module, error_buf, error_buf_size))
                    return false;
                break;
            case SECTION_TYPE_TABLE:
                if (!load_table_section(buf, buf_end, module, error_buf, error_buf_size))
                    return false;
                break;
            case SECTION_TYPE_MEMORY:
                if (!load_memory_section(buf, buf_end, module, error_buf, error_buf_size))
                    return false;
                break;
            case SECTION_TYPE_GLOBAL:
                if (!load_global_section(buf, buf_end, module, error_buf, error_buf_size))
                    return false;
                break;
            case SECTION_TYPE_EXPORT:
                if (!load_export_section(buf, buf_end, module, error_buf, error_buf_size))
                    return false;
                break;
            case SECTION_TYPE_START:
                if (!load_start_section(buf, buf_end, module, error_buf, error_buf_size))
                    return false;
                break;
            case SECTION_TYPE_ELEM:
                if (!load_table_segment_section(buf, buf_end, module, error_buf, error_buf_size))
                    return false;
                break;
            case SECTION_TYPE_CODE:
                if (!load_code_section(buf, buf_end, buf_func, buf_func_end,
                                       module, error_buf, error_buf_size))
                    return false;
                break;
            case SECTION_TYPE_DATA:
                if (!load_data_segment_section(buf, buf_end, module, error_buf, error_buf_size))
                    return false;
                break;
#if WASM_ENABLE_BULK_MEMORY != 0
            case SECTION_TYPE_DATACOUNT:
                if (!load_datacount_section(buf, buf_end, module, error_buf, error_buf_size))
                    return false;
                break;
#endif
            default:
                set_error_buf(error_buf, error_buf_size,
                              "WASM module load failed: invalid section id");
                return false;
        }

        section = section->next;
    }
#if WASM_ENABLE_FAST_INTERP != 0
    handle_table = wasm_interp_get_handle_table();
#endif

    total_size = sizeof(BlockAddr) * (uint64)BLOCK_ADDR_CACHE_SIZE * BLOCK_ADDR_CONFLICT_SIZE;
    if (!(block_addr_cache = loader_malloc
                (total_size, error_buf, error_buf_size))) {
        return false;
    }

    for (i = 0; i < module->function_count; i++) {
        WASMFunction *func = module->functions[i];
        memset(block_addr_cache, 0, (uint32)total_size);
        if (!wasm_loader_prepare_bytecode(module, func, block_addr_cache,
                                          error_buf, error_buf_size)) {
            wasm_runtime_free(block_addr_cache);
            return false;
        }
    }
    wasm_runtime_free(block_addr_cache);

    /* Resolve llvm auxiliary data/stack/heap info and reset memory info */
    export = module->exports;
    for (i = 0; i < module->export_count; i++, export++) {
        if (export->kind == EXPORT_KIND_GLOBAL) {
            if (!strcmp(export->name, "__heap_base")) {
                global_index = export->index - module->import_global_count;
                global = module->globals + global_index;
                if (global->type == VALUE_TYPE_I32
                    && !global->is_mutable
                    && global->init_expr.init_expr_type ==
                            INIT_EXPR_TYPE_I32_CONST) {
                    llvm_heap_base_global = global;
                    llvm_heap_base = global->init_expr.u.i32;
                    LOG_VERBOSE("found llvm __heap_base global, value: %d\n",
                                llvm_heap_base);
                }
            }
            else if (!strcmp(export->name, "__data_end")) {
                global_index = export->index - module->import_global_count;
                global = module->globals + global_index;
                if (global->type == VALUE_TYPE_I32
                    && !global->is_mutable
                    && global->init_expr.init_expr_type ==
                            INIT_EXPR_TYPE_I32_CONST) {
                    llvm_data_end_global = global;
                    llvm_data_end = global->init_expr.u.i32;
                    LOG_VERBOSE("found llvm __data_end global, value: %d\n",
                                llvm_data_end);

                    llvm_data_end = align_uint(llvm_data_end, 16);
                }
            }

            /* For module compiled with -pthread option, the global is:
                [0] stack_top       <-- 0
                [1] tls_pointer
                [2] tls_size
                [3] data_end        <-- 3
                [4] global_base
                [5] heap_base       <-- 5
                [6] dso_handle

                For module compiled without -pthread option:
                [0] stack_top       <-- 0
                [1] data_end        <-- 1
                [2] global_base
                [3] heap_base       <-- 3
                [4] dso_handle
            */
            if (llvm_data_end_global && llvm_heap_base_global) {
                /* Resolve aux stack top global */
                for (global_index = 0; global_index < module->global_count; global_index++) {
                    global = module->globals + global_index;
                    if (global != llvm_data_end_global
                        && global != llvm_heap_base_global
                        && global->type == VALUE_TYPE_I32
                        && global->is_mutable
                        && global->init_expr.init_expr_type ==
                                    INIT_EXPR_TYPE_I32_CONST
                        && (global->init_expr.u.i32 <=
                                    llvm_heap_base_global->init_expr.u.i32
                            && llvm_data_end_global->init_expr.u.i32 <=
                                    llvm_heap_base_global->init_expr.u.i32)) {
                        llvm_stack_top_global = global;
                        llvm_stack_top = global->init_expr.u.i32;
                        stack_top_global_index = global_index;
                        LOG_VERBOSE("found llvm stack top global, "
                                    "value: %d, global index: %d\n",
                                    llvm_stack_top, global_index);
                        break;
                    }
                }

                module->llvm_aux_data_end = llvm_data_end;
                module->llvm_aux_stack_bottom = llvm_stack_top;
                module->llvm_aux_stack_size = llvm_stack_top > llvm_data_end
                                              ? llvm_stack_top - llvm_data_end
                                              : llvm_stack_top;
                module->llvm_aux_stack_global_index = stack_top_global_index;
                LOG_VERBOSE("aux stack bottom: %d, size: %d\n",
                            module->llvm_aux_stack_bottom,
                            module->llvm_aux_stack_size);
                break;
            }
        }
    }

    if (!module->possible_memory_grow) {
        if (llvm_data_end_global
            && llvm_heap_base_global
            && llvm_stack_top_global
            && llvm_stack_top <= llvm_heap_base) {
            WASMMemoryImport *memory_import;
            WASMMemory *memory;
            uint64 init_memory_size;
            uint32 shrunk_memory_size = llvm_heap_base > llvm_data_end
                                        ? llvm_heap_base : llvm_data_end;
            if (module->import_memory_count) {
                memory_import = &module->import_memories[0].u.memory;
                init_memory_size = (uint64)memory_import->num_bytes_per_page *
                                   memory_import->init_page_count;
                if (llvm_heap_base <= init_memory_size
                    && llvm_data_end <= init_memory_size) {
                    /* Reset memory info to decrease memory usage */
                    memory_import->num_bytes_per_page = shrunk_memory_size;
                    memory_import->init_page_count = 1;
                    LOG_VERBOSE("reset import memory size to %d\n",
                                shrunk_memory_size);
                }
            }
            if (module->memory_count) {
                memory = &module->memories[0];
                init_memory_size = (uint64)memory->num_bytes_per_page *
                                   memory->init_page_count;
                if (llvm_heap_base <= init_memory_size
                    && llvm_data_end <= init_memory_size) {
                    /* Reset memory info to decrease memory usage */
                    memory->num_bytes_per_page = shrunk_memory_size;
                    memory->init_page_count = 1;
                    LOG_VERBOSE("reset memory size to %d\n", shrunk_memory_size);
                }
            }
        }
    }

    return true;
}

#if BH_ENABLE_MEMORY_PROFILING != 0
static void wasm_loader_free(void *ptr)
{
    wasm_runtime_free(ptr);
}
#else
#define wasm_loader_free wasm_free
#endif

static WASMModule*
create_module(char *error_buf, uint32 error_buf_size)
{
    WASMModule *module = loader_malloc(sizeof(WASMModule),
                                       error_buf, error_buf_size);

    if (!module) {
        return NULL;
    }

    module->module_type = Wasm_Module_Bytecode;

    /* Set start_function to -1, means no start function */
    module->start_function = (uint32)-1;

#if WASM_ENABLE_MULTI_MODULE != 0
    module->import_module_list = &module->import_module_list_head;
#endif
    return module;
}

WASMModule *
wasm_loader_load_from_sections(WASMSection *section_list,
                               char *error_buf, uint32 error_buf_size)
{
    WASMModule *module = create_module(error_buf, error_buf_size);
    if (!module)
        return NULL;

    if (!load_from_sections(module, section_list, error_buf, error_buf_size)) {
        wasm_loader_unload(module);
        return NULL;
    }

    LOG_VERBOSE("Load module from sections success.\r\n");
    return module;
}

static void
destroy_sections(WASMSection *section_list)
{
    WASMSection *section = section_list, *next;
    while (section) {
        next = section->next;
        wasm_runtime_free(section);
        section = next;
    }
}

static uint8 section_ids[] = {
    SECTION_TYPE_USER,
    SECTION_TYPE_TYPE,
    SECTION_TYPE_IMPORT,
    SECTION_TYPE_FUNC,
    SECTION_TYPE_TABLE,
    SECTION_TYPE_MEMORY,
    SECTION_TYPE_GLOBAL,
    SECTION_TYPE_EXPORT,
    SECTION_TYPE_START,
    SECTION_TYPE_ELEM,
    SECTION_TYPE_CODE,
    SECTION_TYPE_DATA
};

static uint8
get_section_index(uint8 section_type)
{
    uint8 max_id = sizeof(section_ids) / sizeof(uint8);

    for (uint8 i = 0; i < max_id; i++) {
        if (section_type == section_ids[i])
            return i;
    }

    return (uint8)-1;
}

static bool
create_sections(const uint8 *buf, uint32 size,
                WASMSection **p_section_list,
                char *error_buf, uint32 error_buf_size)
{
    WASMSection *section_list_end = NULL, *section;
    const uint8 *p = buf, *p_end = buf + size/*, *section_body*/;
    uint8 section_type, section_index, last_section_index = (uint8)-1;
    uint32 section_size;

    bh_assert(!*p_section_list);

    p += 8;
    while (p < p_end) {
        CHECK_BUF(p, p_end, 1);
        section_type = read_uint8(p);
        section_index = get_section_index(section_type);
        if (section_index != (uint8)-1) {
            if (section_type != SECTION_TYPE_USER) {
                /* Custom sections may be inserted at any place,
                   while other sections must occur at most once
                   and in prescribed order. */
                if (last_section_index != (uint8)-1
                    && (section_index <= last_section_index)) {
                    set_error_buf(error_buf, error_buf_size,
                                  "WASM module load failed: "
                                  "junk after last section");
                    return false;
                }
                last_section_index = section_index;
            }
            CHECK_BUF1(p, p_end, 1);
            read_leb_uint32(p, p_end, section_size);
            CHECK_BUF1(p, p_end, section_size);

            if (!(section = loader_malloc(sizeof(WASMSection),
                                          error_buf, error_buf_size))) {
                return false;
            }

            section->section_type = section_type;
            section->section_body = (uint8*)p;
            section->section_body_size = section_size;

            if (!*p_section_list)
                *p_section_list = section_list_end = section;
            else {
                section_list_end->next = section;
                section_list_end = section;
            }
			//dump ("sections", p, section_size);
            p += section_size;
        }
        else {
            set_error_buf(error_buf, error_buf_size,
                          "WASM module load failed: invalid section id");
            return false;
        }
    }

    return true;
}

static void
exchange32(uint8* p_data)
{
    uint8 value = *p_data;
    *p_data = *(p_data + 3);
    *(p_data + 3) = value;

    value = *(p_data + 1);
    *(p_data + 1) = *(p_data + 2);
    *(p_data + 2) = value;
}

static union {
    int a;
    char b;
} __ue = { .a = 1 };

#define is_little_endian() (__ue.b == 1)

static bool
load(const uint8 *buf, uint32 size, WASMModule *module,
     char *error_buf, uint32 error_buf_size)
{
    const uint8 *buf_end = buf + size;
    const uint8 *p = buf, *p_end = buf_end;
    uint32 magic_number, version;
    WASMSection *section_list = NULL;

    CHECK_BUF1(p, p_end, sizeof(uint32));
    magic_number = read_uint32(p);
    if (!is_little_endian())
        exchange32((uint8*)&magic_number);

    if (magic_number != WASM_MAGIC_NUMBER) {
        set_error_buf(error_buf, error_buf_size,
                      "WASM module load failed: magic header not detected");
        return false;
    }

    CHECK_BUF1(p, p_end, sizeof(uint32));
    version = read_uint32(p);
    if (!is_little_endian())
        exchange32((uint8*)&version);

    if (version != WASM_CURRENT_VERSION) {
        set_error_buf(error_buf, error_buf_size,
                      "WASM module load failed: unknown binary version");
        return false;
    }

    if (!create_sections(buf, size, &section_list, error_buf, error_buf_size)
        || !load_from_sections(module, section_list, error_buf, error_buf_size)) {
        destroy_sections(section_list);
        return false;
    }

    destroy_sections(section_list);
    return true;
}

WASMModule*
wasm_loader_load(const uint8 *buf, uint32 size, char *error_buf, uint32 error_buf_size)
{
    WASMModule *module = create_module(error_buf, error_buf_size);
    if (!module) {
        return NULL;
    }

    if (!load(buf, size, module, error_buf, error_buf_size)) {
        LOG_VERBOSE("Load module failed, %s", error_buf);
        goto fail;
    }

    LOG_VERBOSE("Load module success");
    return module;

fail:
    wasm_loader_unload(module);
    return NULL;
}

void
wasm_loader_unload(WASMModule *module)
{
    uint32 i;

    if (!module)
        return;

    if (module->types) {
        for (i = 0; i < module->type_count; i++) {
            if (module->types[i])
                wasm_runtime_free(module->types[i]);
        }
        wasm_runtime_free(module->types);
    }

    if (module->imports)
        wasm_runtime_free(module->imports);

    if (module->functions) {
        for (i = 0; i < module->function_count; i++) {
            if (module->functions[i]) {
                if (module->functions[i]->local_offsets)
                    wasm_runtime_free(module->functions[i]->local_offsets);
#if WASM_ENABLE_FAST_INTERP != 0
                if (module->functions[i]->code_compiled)
                    wasm_runtime_free(module->functions[i]->code_compiled);
                if (module->functions[i]->consts)
                    wasm_runtime_free(module->functions[i]->consts);
#endif
                wasm_runtime_free(module->functions[i]);
            }
        }
        wasm_runtime_free(module->functions);
    }

    if (module->tables)
        wasm_runtime_free(module->tables);

    if (module->memories)
        wasm_runtime_free(module->memories);

    if (module->globals)
        wasm_runtime_free(module->globals);

    if (module->exports)
        wasm_runtime_free(module->exports);

    if (module->table_segments) {
        for (i = 0; i < module->table_seg_count; i++) {
            if (module->table_segments[i].func_indexes)
                wasm_runtime_free(module->table_segments[i].func_indexes);
        }
        wasm_runtime_free(module->table_segments);
    }

    if (module->data_segments) {
        for (i = 0; i < module->data_seg_count; i++) {
            if (module->data_segments[i])
                wasm_runtime_free(module->data_segments[i]);
        }
        wasm_runtime_free(module->data_segments);
    }

    if (module->const_str_list) {
        StringNode *node = module->const_str_list, *node_next;
        while (node) {
            node_next = node->next;
            wasm_runtime_free(node);
            node = node_next;
        }
    }


    wasm_runtime_free(module);
}

bool
wasm_loader_find_block_addr(BlockAddr *block_addr_cache,
                            const uint8 *start_addr,
                            const uint8 *code_end_addr,
                            uint8 label_type,
                            uint8 **p_else_addr,
                            uint8 **p_end_addr,
                            char *error_buf,
                            uint32 error_buf_size)
{
    const uint8 *p = start_addr, *p_end = code_end_addr;
    uint8 *else_addr = NULL;
    uint32 block_nested_depth = 1, count, i, j, t;
    uint8 opcode, u8;
    BlockAddr block_stack[16] = { 0 }, *block;

    i = ((uintptr_t)start_addr) % BLOCK_ADDR_CACHE_SIZE;
    block = block_addr_cache + BLOCK_ADDR_CONFLICT_SIZE * i;

    for (j = 0; j < BLOCK_ADDR_CONFLICT_SIZE; j++) {
        if (block[j].start_addr == start_addr) {
            /* Cache hit */
            *p_else_addr = block[j].else_addr;
            *p_end_addr = block[j].end_addr;
            return true;
        }
    }

    /* Cache unhit */
    block_stack[0].start_addr = start_addr;

    while (p < code_end_addr) {
        opcode = *p++;

        switch (opcode) {
            case WASM_OP_UNREACHABLE:
            case WASM_OP_NOP:
                break;

            case WASM_OP_BLOCK:
            case WASM_OP_LOOP:
            case WASM_OP_IF:
                CHECK_BUF(p, p_end, 1);
                /* block result type: 0x40/0x7F/0x7E/0x7D/0x7C */
                u8 = read_uint8(p);
                if (block_nested_depth < sizeof(block_stack)/sizeof(BlockAddr)) {
                    block_stack[block_nested_depth].start_addr = p;
                    block_stack[block_nested_depth].else_addr = NULL;
                }
                block_nested_depth++;
                break;

            case EXT_OP_BLOCK:
            case EXT_OP_LOOP:
            case EXT_OP_IF:
                /* block type */
                skip_leb_uint32(p, p_end);
                if (block_nested_depth < sizeof(block_stack)/sizeof(BlockAddr)) {
                    block_stack[block_nested_depth].start_addr = p;
                    block_stack[block_nested_depth].else_addr = NULL;
                }
                block_nested_depth++;
                break;

            case WASM_OP_ELSE:
                if (label_type == LABEL_TYPE_IF && block_nested_depth == 1)
                    else_addr = (uint8*)(p - 1);
                if (block_nested_depth - 1 < sizeof(block_stack)/sizeof(BlockAddr))
                    block_stack[block_nested_depth - 1].else_addr = (uint8*)(p - 1);
                break;

            case WASM_OP_END:
                if (block_nested_depth == 1) {
                    if (label_type == LABEL_TYPE_IF)
                        *p_else_addr = else_addr;
                    *p_end_addr = (uint8*)(p - 1);

                    block_stack[0].end_addr = (uint8*)(p - 1);
                    for (t = 0; t < sizeof(block_stack)/sizeof(BlockAddr); t++) {
                        start_addr = block_stack[t].start_addr;
                        if (start_addr) {
                            i = ((uintptr_t)start_addr) % BLOCK_ADDR_CACHE_SIZE;
                            block = block_addr_cache + BLOCK_ADDR_CONFLICT_SIZE * i;
                            for (j = 0; j < BLOCK_ADDR_CONFLICT_SIZE; j++)
                                if (!block[j].start_addr)
                                    break;

                            if (j == BLOCK_ADDR_CONFLICT_SIZE) {
                                memmove(block + 1, block, (BLOCK_ADDR_CONFLICT_SIZE - 1) *
                                                          sizeof(BlockAddr));
                                j = 0;

                            }
                            block[j].start_addr = block_stack[t].start_addr;
                            block[j].else_addr = block_stack[t].else_addr;
                            block[j].end_addr = block_stack[t].end_addr;
                        }
                        else
                            break;
                    }
                    return true;
                }
                else {
                    block_nested_depth--;
                    if (block_nested_depth < sizeof(block_stack)/sizeof(BlockAddr))
                        block_stack[block_nested_depth].end_addr = (uint8*)(p - 1);
                }
                break;

            case WASM_OP_BR:
            case WASM_OP_BR_IF:
                skip_leb_uint32(p, p_end); /* labelidx */
                break;

            case WASM_OP_BR_TABLE:
                read_leb_uint32(p, p_end, count); /* lable num */
                for (i = 0; i <= count; i++) /* lableidxs */
                    skip_leb_uint32(p, p_end);
                break;

            case WASM_OP_RETURN:
                break;

            case WASM_OP_CALL:
                skip_leb_uint32(p, p_end); /* funcidx */
                break;

            case WASM_OP_CALL_INDIRECT:
                skip_leb_uint32(p, p_end); /* typeidx */
                CHECK_BUF(p, p_end, 1);
                u8 = read_uint8(p); /* 0x00 */
                break;

            case WASM_OP_DROP:
            case WASM_OP_SELECT:
            case WASM_OP_DROP_64:
            case WASM_OP_SELECT_64:
                break;

            case WASM_OP_GET_LOCAL:
            case WASM_OP_SET_LOCAL:
            case WASM_OP_TEE_LOCAL:
            case WASM_OP_GET_GLOBAL:
            case WASM_OP_SET_GLOBAL:
            case WASM_OP_GET_GLOBAL_64:
            case WASM_OP_SET_GLOBAL_64:
            case WASM_OP_SET_GLOBAL_AUX_STACK:
                skip_leb_uint32(p, p_end); /* localidx */
                break;

            case EXT_OP_GET_LOCAL_FAST:
            case EXT_OP_SET_LOCAL_FAST:
            case EXT_OP_TEE_LOCAL_FAST:
                CHECK_BUF(p, p_end, 1);
                p++;
                break;

            case WASM_OP_I32_LOAD:
            case WASM_OP_I64_LOAD:
            case WASM_OP_F32_LOAD:
            case WASM_OP_F64_LOAD:
            case WASM_OP_I32_LOAD8_S:
            case WASM_OP_I32_LOAD8_U:
            case WASM_OP_I32_LOAD16_S:
            case WASM_OP_I32_LOAD16_U:
            case WASM_OP_I64_LOAD8_S:
            case WASM_OP_I64_LOAD8_U:
            case WASM_OP_I64_LOAD16_S:
            case WASM_OP_I64_LOAD16_U:
            case WASM_OP_I64_LOAD32_S:
            case WASM_OP_I64_LOAD32_U:
            case WASM_OP_I32_STORE:
            case WASM_OP_I64_STORE:
            case WASM_OP_F32_STORE:
            case WASM_OP_F64_STORE:
            case WASM_OP_I32_STORE8:
            case WASM_OP_I32_STORE16:
            case WASM_OP_I64_STORE8:
            case WASM_OP_I64_STORE16:
            case WASM_OP_I64_STORE32:
                skip_leb_uint32(p, p_end); /* align */
                skip_leb_uint32(p, p_end); /* offset */
                break;

            case WASM_OP_MEMORY_SIZE:
            case WASM_OP_MEMORY_GROW:
                skip_leb_uint32(p, p_end); /* 0x00 */
                break;

            case WASM_OP_I32_CONST:
                skip_leb_int32(p, p_end);
                break;
            case WASM_OP_I64_CONST:
                skip_leb_int64(p, p_end);
                break;
            case WASM_OP_F32_CONST:
                p += sizeof(float32);
                break;
            case WASM_OP_F64_CONST:
                p += sizeof(float64);
                break;

            case WASM_OP_I32_EQZ:
            case WASM_OP_I32_EQ:
            case WASM_OP_I32_NE:
            case WASM_OP_I32_LT_S:
            case WASM_OP_I32_LT_U:
            case WASM_OP_I32_GT_S:
            case WASM_OP_I32_GT_U:
            case WASM_OP_I32_LE_S:
            case WASM_OP_I32_LE_U:
            case WASM_OP_I32_GE_S:
            case WASM_OP_I32_GE_U:
            case WASM_OP_I64_EQZ:
            case WASM_OP_I64_EQ:
            case WASM_OP_I64_NE:
            case WASM_OP_I64_LT_S:
            case WASM_OP_I64_LT_U:
            case WASM_OP_I64_GT_S:
            case WASM_OP_I64_GT_U:
            case WASM_OP_I64_LE_S:
            case WASM_OP_I64_LE_U:
            case WASM_OP_I64_GE_S:
            case WASM_OP_I64_GE_U:
            case WASM_OP_F32_EQ:
            case WASM_OP_F32_NE:
            case WASM_OP_F32_LT:
            case WASM_OP_F32_GT:
            case WASM_OP_F32_LE:
            case WASM_OP_F32_GE:
            case WASM_OP_F64_EQ:
            case WASM_OP_F64_NE:
            case WASM_OP_F64_LT:
            case WASM_OP_F64_GT:
            case WASM_OP_F64_LE:
            case WASM_OP_F64_GE:
            case WASM_OP_I32_CLZ:
            case WASM_OP_I32_CTZ:
            case WASM_OP_I32_POPCNT:
            case WASM_OP_I32_ADD:
            case WASM_OP_I32_SUB:
            case WASM_OP_I32_MUL:
            case WASM_OP_I32_DIV_S:
            case WASM_OP_I32_DIV_U:
            case WASM_OP_I32_REM_S:
            case WASM_OP_I32_REM_U:
            case WASM_OP_I32_AND:
            case WASM_OP_I32_OR:
            case WASM_OP_I32_XOR:
            case WASM_OP_I32_SHL:
            case WASM_OP_I32_SHR_S:
            case WASM_OP_I32_SHR_U:
            case WASM_OP_I32_ROTL:
            case WASM_OP_I32_ROTR:
            case WASM_OP_I64_CLZ:
            case WASM_OP_I64_CTZ:
            case WASM_OP_I64_POPCNT:
            case WASM_OP_I64_ADD:
            case WASM_OP_I64_SUB:
            case WASM_OP_I64_MUL:
            case WASM_OP_I64_DIV_S:
            case WASM_OP_I64_DIV_U:
            case WASM_OP_I64_REM_S:
            case WASM_OP_I64_REM_U:
            case WASM_OP_I64_AND:
            case WASM_OP_I64_OR:
            case WASM_OP_I64_XOR:
            case WASM_OP_I64_SHL:
            case WASM_OP_I64_SHR_S:
            case WASM_OP_I64_SHR_U:
            case WASM_OP_I64_ROTL:
            case WASM_OP_I64_ROTR:
            case WASM_OP_F32_ABS:
            case WASM_OP_F32_NEG:
            case WASM_OP_F32_CEIL:
            case WASM_OP_F32_FLOOR:
            case WASM_OP_F32_TRUNC:
            case WASM_OP_F32_NEAREST:
            case WASM_OP_F32_SQRT:
            case WASM_OP_F32_ADD:
            case WASM_OP_F32_SUB:
            case WASM_OP_F32_MUL:
            case WASM_OP_F32_DIV:
            case WASM_OP_F32_MIN:
            case WASM_OP_F32_MAX:
            case WASM_OP_F32_COPYSIGN:
            case WASM_OP_F64_ABS:
            case WASM_OP_F64_NEG:
            case WASM_OP_F64_CEIL:
            case WASM_OP_F64_FLOOR:
            case WASM_OP_F64_TRUNC:
            case WASM_OP_F64_NEAREST:
            case WASM_OP_F64_SQRT:
            case WASM_OP_F64_ADD:
            case WASM_OP_F64_SUB:
            case WASM_OP_F64_MUL:
            case WASM_OP_F64_DIV:
            case WASM_OP_F64_MIN:
            case WASM_OP_F64_MAX:
            case WASM_OP_F64_COPYSIGN:
            case WASM_OP_I32_WRAP_I64:
            case WASM_OP_I32_TRUNC_S_F32:
            case WASM_OP_I32_TRUNC_U_F32:
            case WASM_OP_I32_TRUNC_S_F64:
            case WASM_OP_I32_TRUNC_U_F64:
            case WASM_OP_I64_EXTEND_S_I32:
            case WASM_OP_I64_EXTEND_U_I32:
            case WASM_OP_I64_TRUNC_S_F32:
            case WASM_OP_I64_TRUNC_U_F32:
            case WASM_OP_I64_TRUNC_S_F64:
            case WASM_OP_I64_TRUNC_U_F64:
            case WASM_OP_F32_CONVERT_S_I32:
            case WASM_OP_F32_CONVERT_U_I32:
            case WASM_OP_F32_CONVERT_S_I64:
            case WASM_OP_F32_CONVERT_U_I64:
            case WASM_OP_F32_DEMOTE_F64:
            case WASM_OP_F64_CONVERT_S_I32:
            case WASM_OP_F64_CONVERT_U_I32:
            case WASM_OP_F64_CONVERT_S_I64:
            case WASM_OP_F64_CONVERT_U_I64:
            case WASM_OP_F64_PROMOTE_F32:
            case WASM_OP_I32_REINTERPRET_F32:
            case WASM_OP_I64_REINTERPRET_F64:
            case WASM_OP_F32_REINTERPRET_I32:
            case WASM_OP_F64_REINTERPRET_I64:
            case WASM_OP_I32_EXTEND8_S:
            case WASM_OP_I32_EXTEND16_S:
            case WASM_OP_I64_EXTEND8_S:
            case WASM_OP_I64_EXTEND16_S:
            case WASM_OP_I64_EXTEND32_S:
                break;
            case WASM_OP_MISC_PREFIX:
            {
                opcode = read_uint8(p);
                switch (opcode) {
                    case WASM_OP_I32_TRUNC_SAT_S_F32:
                    case WASM_OP_I32_TRUNC_SAT_U_F32:
                    case WASM_OP_I32_TRUNC_SAT_S_F64:
                    case WASM_OP_I32_TRUNC_SAT_U_F64:
                    case WASM_OP_I64_TRUNC_SAT_S_F32:
                    case WASM_OP_I64_TRUNC_SAT_U_F32:
                    case WASM_OP_I64_TRUNC_SAT_S_F64:
                    case WASM_OP_I64_TRUNC_SAT_U_F64:
                        break;
#if WASM_ENABLE_BULK_MEMORY != 0
                    case WASM_OP_MEMORY_INIT:
                        skip_leb_uint32(p, p_end);
                        /* skip memory idx */
                        p++;
                        break;
                    case WASM_OP_DATA_DROP:
                        skip_leb_uint32(p, p_end);
                        break;
                    case WASM_OP_MEMORY_COPY:
                        /* skip two memory idx */
                        p += 2;
                        break;
                    case WASM_OP_MEMORY_FILL:
                        /* skip memory idx */
                        p++;
                        break;
#endif
                    default:
                        if (error_buf)
                            snprintf(error_buf, error_buf_size,
                                    "WASM loader find block addr failed: "
                                    "invalid opcode fc %02x.", opcode);
                        return false;
                }
                break;
            }

            default:
                if (error_buf)
                    snprintf(error_buf, error_buf_size,
                             "WASM loader find block addr failed: "
                             "invalid opcode %02x.", opcode);
                return false;
        }
    }

    (void)u8;
    return false;
}

#define REF_I32   VALUE_TYPE_I32
#define REF_F32   VALUE_TYPE_F32
#define REF_I64_1 VALUE_TYPE_I64
#define REF_I64_2 VALUE_TYPE_I64
#define REF_F64_1 VALUE_TYPE_F64
#define REF_F64_2 VALUE_TYPE_F64
#define REF_ANY   VALUE_TYPE_ANY

#if WASM_ENABLE_FAST_INTERP != 0

#if WASM_DEBUG_PREPROCESSOR != 0
#define LOG_OP(...)       os_printf(__VA_ARGS__)
#else
#define LOG_OP(...)
#endif

#define PATCH_ELSE 0
#define PATCH_END  1
typedef struct BranchBlockPatch {
    struct BranchBlockPatch *next;
    uint8 patch_type;
    uint8 *code_compiled;
} BranchBlockPatch;
#endif

typedef struct BranchBlock {
    uint8 label_type;
    BlockType block_type;
    uint8 *start_addr;
    uint8 *else_addr;
    uint8 *end_addr;
    uint32 stack_cell_num;
#if WASM_ENABLE_FAST_INTERP != 0
    uint16 dynamic_offset;
    uint8 *code_compiled;
    BranchBlockPatch *patch_list;
    /* This is used to save params frame_offset of of if block */
    int16 *param_frame_offsets;
#endif

    /* Indicate the operand stack is in polymorphic state.
     * If the opcode is one of unreachable/br/br_table/return, stack is marked
     * to polymorphic state until the block's 'end' opcode is processed.
     * If stack is in polymorphic state and stack is empty, instruction can
     * pop any type of value directly without decreasing stack top pointer
     * and stack cell num. */
    bool is_stack_polymorphic;
} BranchBlock;

typedef struct WASMLoaderContext {
    /* frame ref stack */
    uint8 *frame_ref;
    uint8 *frame_ref_bottom;
    uint8 *frame_ref_boundary;
    uint32 frame_ref_size;
    uint32 stack_cell_num;
    uint32 max_stack_cell_num;

    /* frame csp stack */
    BranchBlock *frame_csp;
    BranchBlock *frame_csp_bottom;
    BranchBlock *frame_csp_boundary;
    uint32 frame_csp_size;
    uint32 csp_num;
    uint32 max_csp_num;

} WASMLoaderContext;

typedef struct Const {
    WASMValue value;
    uint16 slot_index;
    uint8 value_type;
} Const;

static void*
memory_realloc(void *mem_old, uint32 size_old, uint32 size_new,
               char *error_buf, uint32 error_buf_size)
{
    uint8 *mem_new;
    bh_assert(size_new > size_old);
    if ((mem_new = loader_malloc
                (size_new, error_buf, error_buf_size))) {
        bh_memcpy_s(mem_new, size_new, mem_old, size_old);
        memset(mem_new + size_old, 0, size_new - size_old);
        wasm_runtime_free(mem_old);
    }
    return mem_new;
}

#define MEM_REALLOC(mem, size_old, size_new) do {                \
    void *mem_new = memory_realloc(mem, size_old, size_new,      \
                                   error_buf, error_buf_size);   \
    if (!mem_new)                                                \
        goto fail;                                               \
    mem = mem_new;                                               \
  } while (0)

#define CHECK_CSP_PUSH() do {                                    \
    if (ctx->frame_csp >= ctx->frame_csp_boundary) {             \
      MEM_REALLOC(ctx->frame_csp_bottom, ctx->frame_csp_size,    \
                  (uint32)(ctx->frame_csp_size                   \
                           + 8 * sizeof(BranchBlock)));          \
      ctx->frame_csp_size += (uint32)(8 * sizeof(BranchBlock));  \
      ctx->frame_csp_boundary = ctx->frame_csp_bottom +          \
                    ctx->frame_csp_size / sizeof(BranchBlock);   \
      ctx->frame_csp = ctx->frame_csp_bottom + ctx->csp_num;     \
    }                                                            \
  } while (0)

#define CHECK_CSP_POP() do {                                     \
    if (ctx->csp_num < 1) {                                      \
      set_error_buf(error_buf, error_buf_size,                   \
                  "WASM module load failed: type mismatch: "     \
                  "expect data but block stack was empty");      \
      goto fail;                                                 \
    }                                                            \
  } while (0)


static bool
check_stack_push(WASMLoaderContext *ctx,
                 char *error_buf, uint32 error_buf_size)
{
    if (ctx->frame_ref >= ctx->frame_ref_boundary) {
        MEM_REALLOC(ctx->frame_ref_bottom, ctx->frame_ref_size,
                    ctx->frame_ref_size + 16);
        ctx->frame_ref_size += 16;
        ctx->frame_ref_boundary = ctx->frame_ref_bottom + ctx->frame_ref_size;
        ctx->frame_ref = ctx->frame_ref_bottom + ctx->stack_cell_num;
    }
    return true;
fail:
    return false;
}


static bool
check_stack_top_values(uint8 *frame_ref, int32 stack_cell_num, uint8 type,
                       char *error_buf, uint32 error_buf_size)
{
    char *type_str[] = { "f64", "f32", "i64", "i32" };

    if (((type == VALUE_TYPE_I32 || type == VALUE_TYPE_F32)
         && stack_cell_num < 1)
        || ((type == VALUE_TYPE_I64 || type == VALUE_TYPE_F64)
            && stack_cell_num < 2)) {
        set_error_buf(error_buf, error_buf_size,
                      "WASM module load failed: "
                      "type mismatch: expect data but stack was empty");
        return false;
    }

    if ((type == VALUE_TYPE_I32 && *(frame_ref - 1) != REF_I32)
        || (type == VALUE_TYPE_F32 && *(frame_ref - 1) != REF_F32)
        || (type == VALUE_TYPE_I64
            && (*(frame_ref - 2) != REF_I64_1
                || *(frame_ref - 1) != REF_I64_2))
        || (type == VALUE_TYPE_F64
            && (*(frame_ref - 2) != REF_F64_1
                || *(frame_ref - 1) != REF_F64_2))) {
        if (error_buf != NULL)
            snprintf(error_buf, error_buf_size, "%s%s%s",
                     "WASM module load failed: type mismatch: expect ",
                     type_str[type - VALUE_TYPE_F64], " but got other");
        return false;
    }

    return true;
}

static bool
check_stack_pop(WASMLoaderContext *ctx, uint8 type,
                char *error_buf, uint32 error_buf_size)
{
    int32 block_stack_cell_num = (int32)
        (ctx->stack_cell_num - (ctx->frame_csp - 1)->stack_cell_num);

    if (block_stack_cell_num > 0
        && *(ctx->frame_ref - 1) == VALUE_TYPE_ANY) {
        /* the stack top is a value of any type, return success */
        return true;
    }

    if (!check_stack_top_values(ctx->frame_ref, block_stack_cell_num,
                                type, error_buf, error_buf_size))
        return false;

    return true;
}

static void
wasm_loader_ctx_destroy(WASMLoaderContext *ctx)
{
    if (ctx) {
        if (ctx->frame_ref_bottom)
            wasm_runtime_free(ctx->frame_ref_bottom);
        if (ctx->frame_csp_bottom) {
#if WASM_ENABLE_FAST_INTERP != 0
            free_all_label_patch_lists(ctx->frame_csp_bottom, ctx->csp_num);
#endif
            wasm_runtime_free(ctx->frame_csp_bottom);
        }
#if WASM_ENABLE_FAST_INTERP != 0
        if (ctx->frame_offset_bottom)
            wasm_runtime_free(ctx->frame_offset_bottom);
        if (ctx->const_buf)
            wasm_runtime_free(ctx->const_buf);
#endif
        wasm_runtime_free(ctx);
    }
}

static WASMLoaderContext*
wasm_loader_ctx_init(WASMFunction *func)
{
    WASMLoaderContext *loader_ctx =
        wasm_runtime_malloc(sizeof(WASMLoaderContext));
    if (!loader_ctx)
        return false;
    memset(loader_ctx, 0, sizeof(WASMLoaderContext));

    loader_ctx->frame_ref_size = 32;
    if (!(loader_ctx->frame_ref_bottom = loader_ctx->frame_ref =
            wasm_runtime_malloc(loader_ctx->frame_ref_size)))
        goto fail;
    memset(loader_ctx->frame_ref_bottom, 0, loader_ctx->frame_ref_size);
    loader_ctx->frame_ref_boundary = loader_ctx->frame_ref_bottom +
                                        loader_ctx->frame_ref_size;

    loader_ctx->frame_csp_size = sizeof(BranchBlock) * 8;
    if (!(loader_ctx->frame_csp_bottom = loader_ctx->frame_csp =
            wasm_runtime_malloc(loader_ctx->frame_csp_size)))
        goto fail;
    memset(loader_ctx->frame_csp_bottom, 0, loader_ctx->frame_csp_size);
    loader_ctx->frame_csp_boundary = loader_ctx->frame_csp_bottom + 8;

#if WASM_ENABLE_FAST_INTERP != 0
    loader_ctx->frame_offset_size = sizeof(int16) * 32;
    if (!(loader_ctx->frame_offset_bottom = loader_ctx->frame_offset =
            wasm_runtime_malloc(loader_ctx->frame_offset_size)))
        goto fail;
    memset(loader_ctx->frame_offset_bottom, 0,
           loader_ctx->frame_offset_size);
    loader_ctx->frame_offset_boundary = loader_ctx->frame_offset_bottom + 32;

    loader_ctx->num_const = 0;
    loader_ctx->const_buf_size = sizeof(Const) * 8;
    if (!(loader_ctx->const_buf = wasm_runtime_malloc(loader_ctx->const_buf_size)))
        goto fail;
    memset(loader_ctx->const_buf, 0, loader_ctx->const_buf_size);

    loader_ctx->start_dynamic_offset = loader_ctx->dynamic_offset =
        loader_ctx->max_dynamic_offset = func->param_cell_num +
                                            func->local_cell_num;
#endif
    return loader_ctx;

fail:
    wasm_loader_ctx_destroy(loader_ctx);
    return NULL;
}

static bool
wasm_loader_push_frame_ref(WASMLoaderContext *ctx, uint8 type,
                           char *error_buf, uint32 error_buf_size)
{
    if (type == VALUE_TYPE_VOID)
        return true;

    if (!check_stack_push(ctx, error_buf, error_buf_size))
        return false;

    *ctx->frame_ref++ = type;
    ctx->stack_cell_num++;
    if (ctx->stack_cell_num > ctx->max_stack_cell_num)
        ctx->max_stack_cell_num = ctx->stack_cell_num;

    if (type == VALUE_TYPE_I32
        || type == VALUE_TYPE_F32
        || type == VALUE_TYPE_ANY)
        return true;

    if (!check_stack_push(ctx, error_buf, error_buf_size))
        return false;
    *ctx->frame_ref++ = type;
    ctx->stack_cell_num++;
    if (ctx->stack_cell_num > ctx->max_stack_cell_num)
        ctx->max_stack_cell_num = ctx->stack_cell_num;
    return true;
}

static bool
wasm_loader_pop_frame_ref(WASMLoaderContext *ctx, uint8 type,
                          char *error_buf, uint32 error_buf_size)
{
    BranchBlock *cur_block = ctx->frame_csp - 1;
    int32 available_stack_cell = (int32)
        (ctx->stack_cell_num - cur_block->stack_cell_num);

    /* Directly return success if current block is in stack
     * polymorphic state while stack is empty. */
    if (available_stack_cell <= 0 && cur_block->is_stack_polymorphic)
        return true;

    if (type == VALUE_TYPE_VOID)
        return true;

    if (!check_stack_pop(ctx, type, error_buf, error_buf_size))
        return false;

    ctx->frame_ref--;
    ctx->stack_cell_num--;

    if (type == VALUE_TYPE_I32
        || type == VALUE_TYPE_F32
        || *ctx->frame_ref == VALUE_TYPE_ANY)
        return true;

    ctx->frame_ref--;
    ctx->stack_cell_num--;
    return true;
}

static bool
wasm_loader_push_pop_frame_ref(WASMLoaderContext *ctx, uint8 pop_cnt,
                               uint8 type_push, uint8 type_pop,
                               char *error_buf, uint32 error_buf_size)
{
    for (int i = 0; i < pop_cnt; i++) {
        if (!wasm_loader_pop_frame_ref(ctx, type_pop, error_buf, error_buf_size))
            return false;
    }
    if (!wasm_loader_push_frame_ref(ctx, type_push, error_buf, error_buf_size))
        return false;
    return true;
}

static bool
wasm_loader_push_frame_csp(WASMLoaderContext *ctx, uint8 label_type,
                           BlockType block_type, uint8* start_addr,
                           char *error_buf, uint32 error_buf_size)
{
    CHECK_CSP_PUSH();
    memset(ctx->frame_csp, 0, sizeof(BranchBlock));
    ctx->frame_csp->label_type = label_type;
    ctx->frame_csp->block_type = block_type;
    ctx->frame_csp->start_addr = start_addr;
    ctx->frame_csp->stack_cell_num = ctx->stack_cell_num;
#if WASM_ENABLE_FAST_INTERP != 0
    ctx->frame_csp->dynamic_offset = ctx->dynamic_offset;
    ctx->frame_csp->patch_list = NULL;
#endif
    ctx->frame_csp++;
    ctx->csp_num++;
    if (ctx->csp_num > ctx->max_csp_num)
        ctx->max_csp_num = ctx->csp_num;
    return true;
fail:
    return false;
}

static bool
wasm_loader_pop_frame_csp(WASMLoaderContext *ctx,
                          char *error_buf, uint32 error_buf_size)
{
    CHECK_CSP_POP();
#if WASM_ENABLE_FAST_INTERP != 0
    if ((ctx->frame_csp - 1)->param_frame_offsets)
        wasm_runtime_free((ctx->frame_csp - 1)->param_frame_offsets);
#endif
    ctx->frame_csp--;
    ctx->csp_num--;

    return true;
fail:
    return false;
}

#if WASM_ENABLE_FAST_INTERP != 0

#else /* WASM_ENABLE_FAST_INTERP */

#define PUSH_I32() do {                                             \
    if (!(wasm_loader_push_frame_ref(loader_ctx, VALUE_TYPE_I32,    \
                                     error_buf, error_buf_size)))   \
        goto fail;                                                  \
  } while (0)

#define PUSH_F32() do {                                             \
    if (!(wasm_loader_push_frame_ref(loader_ctx, VALUE_TYPE_F32,    \
                                     error_buf, error_buf_size)))   \
        goto fail;                                                  \
  } while (0)

#define PUSH_I64() do {                                             \
    if (!(wasm_loader_push_frame_ref(loader_ctx, VALUE_TYPE_I64,    \
                                     error_buf, error_buf_size)))   \
        goto fail;                                                  \
  } while (0)

#define PUSH_F64() do {                                             \
    if (!(wasm_loader_push_frame_ref(loader_ctx, VALUE_TYPE_F64,    \
                                     error_buf, error_buf_size)))   \
        goto fail;                                                  \
  } while (0)

#define POP_I32() do {                                              \
    if (!(wasm_loader_pop_frame_ref(loader_ctx, VALUE_TYPE_I32,     \
                                    error_buf, error_buf_size)))    \
        goto fail;                                                  \
  } while (0)

#define POP_F32() do {                                              \
    if (!(wasm_loader_pop_frame_ref(loader_ctx, VALUE_TYPE_F32,     \
                                    error_buf, error_buf_size)))    \
        goto fail;                                                  \
  } while (0)

#define POP_I64() do {                                              \
    if (!(wasm_loader_pop_frame_ref(loader_ctx, VALUE_TYPE_I64,     \
                                    error_buf, error_buf_size)))    \
        goto fail;                                                  \
  } while (0)

#define POP_F64() do {                                              \
    if (!(wasm_loader_pop_frame_ref(loader_ctx, VALUE_TYPE_F64,     \
                                    error_buf, error_buf_size)))    \
        goto fail;                                                  \
  } while (0)

#define POP_AND_PUSH(type_pop, type_push) do {                           \
    if (!(wasm_loader_push_pop_frame_ref(loader_ctx, 1,                  \
                                         type_push, type_pop,            \
                                         error_buf, error_buf_size)))    \
        goto fail;                                                       \
  } while (0)

/* type of POPs should be the same */
#define POP2_AND_PUSH(type_pop, type_push) do {                          \
    if (!(wasm_loader_push_pop_frame_ref(loader_ctx, 2,                  \
                                         type_push, type_pop,            \
                                         error_buf, error_buf_size)))    \
        goto fail;                                                       \
  } while (0)
#endif /* WASM_ENABLE_FAST_INTERP */

#if WASM_ENABLE_FAST_INTERP != 0

static bool
reserve_block_ret(WASMLoaderContext *loader_ctx,
                  uint8 opcode, bool disable_emit,
                  char *error_buf, uint32 error_buf_size)
{
    int16 operand_offset = 0;
    BranchBlock *block = (opcode == WASM_OP_ELSE) ?
                         loader_ctx->frame_csp - 1 : loader_ctx->frame_csp;
    BlockType *block_type = &block->block_type;
    uint8 *return_types = NULL;
    uint32 return_count = 0, value_count = 0, total_cel_num = 0;
    int32 i = 0;
    int16 dynamic_offset, dynamic_offset_org,
          *frame_offset = NULL, *frame_offset_org = NULL;

    return_count = block_type_get_result_types(block_type, &return_types);

    /* If there is only one return value, use EXT_OP_COPY_STACK_TOP/_I64 instead
     * of EXT_OP_COPY_STACK_VALUES for interpreter performance. */
    if (return_count == 1) {
        uint8 cell = wasm_value_type_cell_num(return_types[0]);
        if (block->dynamic_offset != *(loader_ctx->frame_offset - cell)) {
            /* insert op_copy before else opcode */
            if (opcode == WASM_OP_ELSE)
                skip_label();
            emit_label(cell == 1 ? EXT_OP_COPY_STACK_TOP : EXT_OP_COPY_STACK_TOP_I64);
            emit_operand(loader_ctx, *(loader_ctx->frame_offset - cell));
            emit_operand(loader_ctx, block->dynamic_offset);

            if (opcode == WASM_OP_ELSE) {
                *(loader_ctx->frame_offset - cell) = block->dynamic_offset;
            }
            else {
                loader_ctx->frame_offset -= cell;
                loader_ctx->dynamic_offset = block->dynamic_offset;
                PUSH_OFFSET_TYPE(return_types[0]);
                wasm_loader_emit_backspace(loader_ctx, sizeof(int16));
            }
            if (opcode == WASM_OP_ELSE)
                emit_label(opcode);
        }
        return true;
    }

    /* Copy stack top values to block's results which are in dynamic space.
     * The instruction format:
     *   Part a: values count
     *   Part b: all values total cell num
     *   Part c: each value's cell_num, src offset and dst offset
     *   Part d: each value's src offset and dst offset
     *   Part e: each value's dst offset
     */
    frame_offset = frame_offset_org = loader_ctx->frame_offset;
    dynamic_offset = dynamic_offset_org =
                              block->dynamic_offset
                               + wasm_get_cell_num(return_types, return_count);

    /* First traversal to get the count of values needed to be copied. */
    for (i = (int32)return_count - 1; i >= 0; i--) {
        uint8 cells = wasm_value_type_cell_num(return_types[i]);

        frame_offset -= cells;
        dynamic_offset -= cells;
        if (dynamic_offset != *frame_offset) {
            value_count++;
            total_cel_num += cells;
        }
    }

    if (value_count) {
        uint32 j = 0;
        uint8 *emit_data = NULL, *cells = NULL;
        int16 *src_offsets = NULL;
        uint16 *dst_offsets = NULL;
        uint64 size = (uint64)value_count * (sizeof(*cells)
                                             + sizeof(*src_offsets)
                                             + sizeof(*dst_offsets));

        /* Allocate memory for the emit data */
        if (!(emit_data = loader_malloc(size, error_buf, error_buf_size)))
            return false;

        cells = emit_data;
        src_offsets = (int16 *)(cells + value_count);
        dst_offsets = (uint16 *)(src_offsets + value_count);

        /* insert op_copy before else opcode */
        if (opcode == WASM_OP_ELSE)
            skip_label();
        emit_label(EXT_OP_COPY_STACK_VALUES);
        /* Part a) */
        emit_uint32(loader_ctx, value_count);
        /* Part b) */
        emit_uint32(loader_ctx, total_cel_num);

        /* Second traversal to get each value's cell num,  src offset and dst offset. */
        frame_offset = frame_offset_org;
        dynamic_offset = dynamic_offset_org;
        for (i = (int32)return_count - 1, j = 0; i >= 0; i--) {
            uint8 cell = wasm_value_type_cell_num(return_types[i]);
            frame_offset -= cell;
            dynamic_offset -= cell;
            if (dynamic_offset != *frame_offset) {
                /* cell num */
                cells[j] = cell;
                /* src offset */
                src_offsets[j] = *frame_offset;
                /* dst offset */
                dst_offsets[j] = dynamic_offset;
                j++;
            }
            if (opcode == WASM_OP_ELSE) {
                *frame_offset = dynamic_offset;
            }
            else {
                loader_ctx->frame_offset = frame_offset;
                loader_ctx->dynamic_offset = dynamic_offset;
                PUSH_OFFSET_TYPE(return_types[i]);
                wasm_loader_emit_backspace(loader_ctx, sizeof(int16));
                loader_ctx->frame_offset = frame_offset_org;
                loader_ctx->dynamic_offset = dynamic_offset_org;
            }
        }

        bh_assert(j == value_count);

        /* Emit the cells, src_offsets and dst_offsets */
        for (j = 0; j < value_count; j++)
            emit_byte(loader_ctx, cells[j]);
        for (j = 0; j < value_count; j++)
            emit_operand(loader_ctx, src_offsets[j]);
        for (j = 0; j < value_count; j++)
            emit_operand(loader_ctx, dst_offsets[j]);

        if (opcode == WASM_OP_ELSE)
            emit_label(opcode);

        wasm_runtime_free(emit_data);
    }

    return true;

fail:
    return false;
}
#endif /* WASM_ENABLE_FAST_INTERP */

#define RESERVE_BLOCK_RET() do {                                    \
     if (!reserve_block_ret(loader_ctx, opcode, disable_emit,       \
                            error_buf, error_buf_size))             \
        goto fail;                                                  \
  } while (0)

#define PUSH_TYPE(type) do {                                        \
    if (!(wasm_loader_push_frame_ref(loader_ctx, type,              \
                                     error_buf, error_buf_size)))   \
        goto fail;                                                  \
  } while (0)

#define POP_TYPE(type) do {                                         \
    if (!(wasm_loader_pop_frame_ref(loader_ctx, type,               \
                                    error_buf, error_buf_size)))    \
        goto fail;                                                  \
  } while (0)

#define PUSH_CSP(label_type, block_type, _start_addr) do {              \
    if (!wasm_loader_push_frame_csp(loader_ctx, label_type, block_type, \
                                    _start_addr, error_buf,             \
                                    error_buf_size))                    \
        goto fail;                                                      \
  } while (0)

#define POP_CSP() do {                                          \
    if (!wasm_loader_pop_frame_csp(loader_ctx,                  \
                                   error_buf, error_buf_size))  \
        goto fail;                                              \
  } while (0)

#define GET_LOCAL_INDEX_TYPE_AND_OFFSET() do {      \
    read_leb_uint32(p, p_end, local_idx);           \
    if (local_idx >= param_count + local_count) {   \
      set_error_buf(error_buf, error_buf_size,      \
                    "WASM module load failed: "     \
                    "unknown local.");              \
      goto fail;                                    \
    }                                               \
    local_type = local_idx < param_count            \
        ? param_types[local_idx]                    \
        : local_types[local_idx - param_count];     \
    local_offset = local_offsets[local_idx];        \
  } while (0)

#define CHECK_BR(depth) do {                                        \
    if (!wasm_loader_check_br(loader_ctx, depth,                    \
                              error_buf, error_buf_size))           \
        goto fail;                                                  \
  } while (0)

static bool
check_memory(WASMModule *module,
             char *error_buf, uint32 error_buf_size)
{
    if (module->memory_count == 0
        && module->import_memory_count == 0) {
        set_error_buf(error_buf, error_buf_size,
                      "WASM module load failed: unknown memory");
        return false;
    }
    return true;
}

#define CHECK_MEMORY() do {                                         \
    if (!check_memory(module, error_buf, error_buf_size))           \
      goto fail;                                                    \
  } while (0)

static bool
check_memory_access_align(uint8 opcode, uint32 align,
                          char *error_buf, uint32 error_buf_size)
{
    uint8 mem_access_aligns[] = {
       2, 3, 2, 3, 0, 0, 1, 1, 0, 0, 1, 1, 2, 2, /* loads */
       2, 3, 2, 3, 0, 1, 0, 1, 2                 /* stores */
    };
    bh_assert(opcode >= WASM_OP_I32_LOAD
              && opcode <= WASM_OP_I64_STORE32);
    if (align > mem_access_aligns[opcode - WASM_OP_I32_LOAD]) {
        set_error_buf(error_buf, error_buf_size,
                      "alignment must not be larger than natural");
        return false;
    }
    return true;
}

static bool
is_value_type(uint8 type)
{
    return type == VALUE_TYPE_I32 ||
           type == VALUE_TYPE_I64 ||
           type == VALUE_TYPE_F32 ||
           type == VALUE_TYPE_F64 ||
           type == VALUE_TYPE_VOID;
}

static bool
wasm_loader_check_br(WASMLoaderContext *loader_ctx, uint32 depth,
                     char *error_buf, uint32 error_buf_size)
{
    BranchBlock *target_block, *cur_block;
    BlockType *target_block_type;
    uint8 *types = NULL, *frame_ref;
    uint32 arity = 0;
    int32 i, available_stack_cell;
    uint16 cell_num;

    if (loader_ctx->csp_num < depth + 1) {
      set_error_buf(error_buf, error_buf_size,
                    "WASM module load failed: unknown label, "
                    "unexpected end of section or function");
      return false;
    }

    cur_block = loader_ctx->frame_csp - 1;
    target_block = loader_ctx->frame_csp - (depth + 1);
    target_block_type = &target_block->block_type;
    frame_ref = loader_ctx->frame_ref;

    /* Note: loop's arity is different from if and block. loop's arity is
     * its parameter count while if and block arity is result count.
     */
    if (target_block->label_type == LABEL_TYPE_LOOP)
        arity = block_type_get_param_types(target_block_type, &types);
    else
        arity = block_type_get_result_types(target_block_type, &types);

    /* If the stack is in polymorphic state, just clear the stack
     * and then re-push the values to make the stack top values
     * match block type. */
    if (cur_block->is_stack_polymorphic) {
        for (i = (int32)arity -1; i >= 0; i--) {
#if WASM_ENABLE_FAST_INTERP != 0
            POP_OFFSET_TYPE(types[i]);
#endif
            POP_TYPE(types[i]);
        }
        for (i = 0; i < (int32)arity; i++) {
#if WASM_ENABLE_FAST_INTERP != 0
            bool disable_emit = true;
            int16 operand_offset = 0;
            PUSH_OFFSET_TYPE(types[i]);
#endif
            PUSH_TYPE(types[i]);
        }
        return true;
    }

    available_stack_cell = (int32)
                           (loader_ctx->stack_cell_num - cur_block->stack_cell_num);

    /* Check stack top values match target block type */
    for (i = (int32)arity -1; i >= 0; i--) {
        if (!check_stack_top_values(frame_ref, available_stack_cell,
                                    types[i],
                                    error_buf, error_buf_size))
            return false;
        cell_num = wasm_value_type_cell_num(types[i]);
        frame_ref -= cell_num;
        available_stack_cell -= cell_num;
    }

    return true;

fail:
    return false;
}

static BranchBlock *
check_branch_block(WASMLoaderContext *loader_ctx,
                   uint8 **p_buf, uint8 *buf_end,
                   char *error_buf, uint32 error_buf_size)
{
    uint8 *p = *p_buf, *p_end = buf_end;
    BranchBlock *frame_csp_tmp;
    uint32 depth;

    read_leb_uint32(p, p_end, depth);
    CHECK_BR(depth);
    frame_csp_tmp = loader_ctx->frame_csp - depth - 1;
#if WASM_ENABLE_FAST_INTERP != 0
    emit_br_info(frame_csp_tmp);
#endif

    *p_buf = p;
    return frame_csp_tmp;
fail:
    return NULL;
}

static bool
check_block_stack(WASMLoaderContext *loader_ctx, BranchBlock *block,
                  char *error_buf, uint32 error_buf_size)
{
    BlockType *block_type = &block->block_type;
    uint8 *return_types = NULL;
    uint32 return_count = 0;
    int32 available_stack_cell, return_cell_num, i;
    uint8 *frame_ref = NULL;

    available_stack_cell = (int32)
                           (loader_ctx->stack_cell_num
                            - block->stack_cell_num);

    return_count = block_type_get_result_types(block_type, &return_types);
    return_cell_num = return_count > 0 ?
                      wasm_get_cell_num(return_types, return_count) : 0;

    /* If the stack is in polymorphic state, just clear the stack
     * and then re-push the values to make the stack top values
     * match block type. */
    if (block->is_stack_polymorphic) {
        for (i = (int32)return_count -1; i >= 0; i--) {
#if WASM_ENABLE_FAST_INTERP != 0
            POP_OFFSET_TYPE(return_types[i]);
#endif
            POP_TYPE(return_types[i]);
        }

        /* Check stack is empty */
        if (loader_ctx->stack_cell_num != block->stack_cell_num) {
            set_error_buf(error_buf, error_buf_size,
                          "WASM module load failed: "
                          "type mismatch: stack size does not match block type");
            goto fail;
        }

        for (i = 0; i < (int32)return_count; i++) {
#if WASM_ENABLE_FAST_INTERP != 0
            bool disable_emit = true;
            int16 operand_offset = 0;
            PUSH_OFFSET_TYPE(return_types[i]);
#endif
            PUSH_TYPE(return_types[i]);
        }
        return true;
    }

    /* Check stack cell num equals return cell num */
    if (available_stack_cell != return_cell_num) {
        set_error_buf(error_buf, error_buf_size,
                      "WASM module load failed: "
                      "type mismatch: stack size does not match block type");
        goto fail;
    }

    /* Check stack values match return types */
    frame_ref = loader_ctx->frame_ref;
    for (i = (int32)return_count -1; i >= 0; i--) {
        if (!check_stack_top_values(frame_ref, available_stack_cell,
                                    return_types[i],
                                    error_buf, error_buf_size))
            return false;
        frame_ref -= wasm_value_type_cell_num(return_types[i]);
        available_stack_cell -= wasm_value_type_cell_num(return_types[i]);
    }

    return true;

fail:
    return false;
}



/* reset the stack to the state of before entering the last block */

#define RESET_STACK() do {                                                \
    loader_ctx->stack_cell_num =                                          \
               (loader_ctx->frame_csp - 1)->stack_cell_num;               \
    loader_ctx->frame_ref =                                               \
               loader_ctx->frame_ref_bottom + loader_ctx->stack_cell_num; \
} while (0)

/* set current block's stack polymorphic state */
#define SET_CUR_BLOCK_STACK_POLYMORPHIC_STATE(flag) do {                  \
    BranchBlock *cur_block = loader_ctx->frame_csp - 1;                   \
    cur_block->is_stack_polymorphic = flag;                               \
} while (0)

#define BLOCK_HAS_PARAM(block_type) \
    (!block_type.is_value_type && block_type.u.type->param_count > 0)

static bool
wasm_loader_prepare_bytecode(WASMModule *module, WASMFunction *func,
                             BlockAddr *block_addr_cache,
                             char *error_buf, uint32 error_buf_size)
{
    uint8 *p = func->code, *p_end = func->code + func->code_size, *p_org;
    uint32 param_count, local_count, global_count;
    uint8 *param_types, *local_types, local_type, global_type;
    BlockType func_type;
    uint16 *local_offsets, local_offset;
    uint32 count, i, local_idx, global_idx, u32, align, mem_offset;
    int32 i32, i32_const = 0;
    int64 i64;
    uint8 opcode, u8;
    bool return_value = false;
    WASMLoaderContext *loader_ctx;
    BranchBlock *frame_csp_tmp;
#if WASM_ENABLE_BULK_MEMORY != 0
    uint32 segment_index;
#endif
#if WASM_ENABLE_FAST_INTERP != 0
    uint8 *func_const_end, *func_const;
    int16 operand_offset;
    uint8 last_op = 0;
    bool disable_emit, preserve_local = false;
    float32 f32;
    float64 f64;

    LOG_OP("\nProcessing func | [%d] params | [%d] locals | [%d] return\n",
        func->param_cell_num,
        func->local_cell_num,
        func->ret_cell_num);
#endif

    global_count = module->import_global_count + module->global_count;

    param_count = func->func_type->param_count;
    param_types = func->func_type->types;

    func_type.is_value_type = false;
    func_type.u.type = func->func_type;

    local_count = func->local_count;
    local_types = func->local_types;
    local_offsets = func->local_offsets;

    if (!(loader_ctx = wasm_loader_ctx_init(func))) {
        set_error_buf(error_buf, error_buf_size,
                      "WASM loader prepare bytecode failed: "
                      "allocate memory failed");
        goto fail;
    }

#if WASM_ENABLE_FAST_INTERP != 0
re_scan:
    if (loader_ctx->code_compiled_size > 0) {
        if (!wasm_loader_ctx_reinit(loader_ctx)) {
            set_error_buf(error_buf, error_buf_size,
                          "WASM loader prepare bytecode failed: "
                          "allocate memory failed");
            goto fail;
        }
        p = func->code;
        func->code_compiled = loader_ctx->p_code_compiled;
    }
#endif

    PUSH_CSP(LABEL_TYPE_FUNCTION, func_type, p);

    while (p < p_end) {
        opcode = *p++;
#if WASM_ENABLE_FAST_INTERP != 0
        p_org = p;
        disable_emit = false;
        emit_label(opcode);
#endif

        switch (opcode) {
            case WASM_OP_UNREACHABLE:
                RESET_STACK();
                SET_CUR_BLOCK_STACK_POLYMORPHIC_STATE(true);
                break;

            case WASM_OP_NOP:
#if WASM_ENABLE_FAST_INTERP != 0
                skip_label();
#endif
                break;

            case WASM_OP_IF:
                POP_I32();
                goto handle_op_block_and_loop;
            case WASM_OP_BLOCK:
            case WASM_OP_LOOP:
handle_op_block_and_loop:
            {
                uint8 value_type;
                BlockType block_type;

                value_type = read_uint8(p);
                if (is_value_type(value_type)) {
                    /* If the first byte is one of these special values:
                     * 0x40/0x7F/0x7E/0x7D/0x7C, take it as the type of
                     * the single return value. */
                    block_type.is_value_type = true;
                    block_type.u.value_type = value_type;
                }
                else {
                    uint32 type_index;
                    /* Resolve the leb128 encoded type index as block type */
                    p--;
                    read_leb_uint32(p, p_end, type_index);
                    if (type_index >= module->type_count) {
                        set_error_buf(error_buf, error_buf_size,
                                      "WASM loader prepare bytecode failed: "
                                      "unknown type");
                        goto fail;
                    }
                    block_type.is_value_type = false;
                    block_type.u.type = module->types[type_index];
#if WASM_ENABLE_FAST_INTERP == 0 \
    && WASM_ENABLE_WAMR_COMPILER == 0 \
    && WASM_ENABLE_JIT == 0
                    /* If block use type index as block type, change the opcode
                     * to new extended opcode so that interpreter can resolve the
                     * block quickly.
                     */
                    *(p - 2) = EXT_OP_BLOCK + (opcode - WASM_OP_BLOCK);
#endif
                }

                /* Pop block parameters from stack */
                if (BLOCK_HAS_PARAM(block_type)) {
                    WASMType *wasm_type = block_type.u.type;
                    for (i = 0; i < block_type.u.type->param_count; i++)
                        POP_TYPE(wasm_type->types[wasm_type->param_count - i - 1]);
                }

                PUSH_CSP(LABEL_TYPE_BLOCK + (opcode - WASM_OP_BLOCK), block_type, p);

                /* Pass parameters to block */
                if (BLOCK_HAS_PARAM(block_type)) {
                    for (i = 0; i < block_type.u.type->param_count; i++)
                        PUSH_TYPE(block_type.u.type->types[i]);
                }


                break;
            }

            case WASM_OP_ELSE:
            {
                BlockType block_type = (loader_ctx->frame_csp - 1)->block_type;

                if (loader_ctx->csp_num < 2
                    || (loader_ctx->frame_csp - 1)->label_type != LABEL_TYPE_IF) {
                    set_error_buf(error_buf, error_buf_size,
                                  "WASM loader prepare bytecode failed: "
                                  "opcode else found without matched opcode if");
                    goto fail;
                }

                /* check whether if branch's stack matches its result type */
                if (!check_block_stack(loader_ctx, loader_ctx->frame_csp - 1,
                                       error_buf, error_buf_size))
                    goto fail;

                (loader_ctx->frame_csp - 1)->else_addr = p - 1;


                RESET_STACK();
                SET_CUR_BLOCK_STACK_POLYMORPHIC_STATE(false);

                /* Pass parameters to if-false branch */
                if (BLOCK_HAS_PARAM(block_type)) {
                    for (i = 0; i < block_type.u.type->param_count; i++)
                        PUSH_TYPE(block_type.u.type->types[i]);
                }



                break;
            }

            case WASM_OP_END:
            {
                BranchBlock *cur_block = loader_ctx->frame_csp - 1;

                /* check whether block stack matches its result type */
                if (!check_block_stack(loader_ctx, cur_block,
                                       error_buf, error_buf_size))
                    goto fail;

                /* if no else branch, and return types do not match param types, fail */
                if (cur_block->label_type == LABEL_TYPE_IF
                    && !cur_block->else_addr) {
                    uint32 param_count = 0, ret_count = 0;
                    uint8 *param_types = NULL, *ret_types = NULL;
                    BlockType *block_type = &cur_block->block_type;
                    if (block_type->is_value_type) {
                        if (block_type->u.value_type != VALUE_TYPE_VOID) {
                            ret_count = 1;
                            ret_types = &block_type->u.value_type;
                        }
                    }
                    else {
                        param_count = block_type->u.type->param_count;
                        ret_count = block_type->u.type->result_count;
                        param_types = block_type->u.type->types;
                        ret_types = block_type->u.type->types + param_count;
                    }
                    if (param_count != ret_count
                        || (param_count && memcmp(param_types, ret_types, param_count))) {
                        set_error_buf(error_buf, error_buf_size,
                                      "WASM module load failed: "
                                      "type mismatch: else branch missing");
                        goto fail;
                    }
                }

                POP_CSP();

                if (loader_ctx->csp_num > 0) {
                    loader_ctx->frame_csp->end_addr = p - 1;
                }
                else {
                    /* end of function block, function will return,
                       ignore the following bytecodes */
                    p = p_end;

                    continue;
                }

                SET_CUR_BLOCK_STACK_POLYMORPHIC_STATE(false);
                break;
            }

            case WASM_OP_BR:
            {
                if (!(frame_csp_tmp = check_branch_block(loader_ctx, &p, p_end,
                                                         error_buf, error_buf_size)))
                    goto fail;

                RESET_STACK();
                SET_CUR_BLOCK_STACK_POLYMORPHIC_STATE(true);
                break;
            }

            case WASM_OP_BR_IF:
            {
                POP_I32();

                if (!(frame_csp_tmp = check_branch_block(loader_ctx, &p, p_end,
                                                         error_buf, error_buf_size)))
                    goto fail;

                break;
            }

            case WASM_OP_BR_TABLE:
            {
                uint8 *ret_types = NULL;
                uint32 ret_count = 0;

                read_leb_uint32(p, p_end, count);
                POP_I32();

                /* TODO: check the const */
                for (i = 0; i <= count; i++) {
                    if (!(frame_csp_tmp =
                            check_branch_block(loader_ctx, &p, p_end,
                                               error_buf, error_buf_size)))
                        goto fail;

                    if (i == 0) {
                        if (frame_csp_tmp->label_type != LABEL_TYPE_LOOP)
                            ret_count =
                                    block_type_get_result_types(&frame_csp_tmp->block_type,
                                                                &ret_types);
                    }
                    else {
                        uint8 *tmp_ret_types = NULL;
                        uint32 tmp_ret_count = 0;

                        /* Check whether all table items have the same return type */
                        if (frame_csp_tmp->label_type != LABEL_TYPE_LOOP)
                            tmp_ret_count =
                                    block_type_get_result_types(&frame_csp_tmp->block_type,
                                                                &tmp_ret_types);

                        if (ret_count != tmp_ret_count
                            || (ret_count
                                && 0 != memcmp(ret_types, tmp_ret_types, ret_count))) {
                            set_error_buf(error_buf, error_buf_size,
                                          "WASM loader prepare bytecode failed: "
                                          "type mismatch: br_table targets must "
                                          "all use same result type");
                            goto fail;
                        }
                    }
                }

                RESET_STACK();
                SET_CUR_BLOCK_STACK_POLYMORPHIC_STATE(true);
                break;
            }

            case WASM_OP_RETURN:
            {
                int32 idx;
                uint8 ret_type;
                for (idx = (int32)func->func_type->result_count - 1; idx >= 0; idx--) {
                    ret_type = *(func->func_type->types
                                 + func->func_type->param_count + idx);
                    POP_TYPE(ret_type);
                }

                RESET_STACK();
                SET_CUR_BLOCK_STACK_POLYMORPHIC_STATE(true);

                break;
            }

            case WASM_OP_CALL:
            {
                WASMType *func_type;
                uint32 func_idx;
                int32 idx;

                read_leb_uint32(p, p_end, func_idx);


                if (func_idx >= module->import_function_count + module->function_count) {
                    set_error_buf(error_buf, error_buf_size,
                                  "WASM loader prepare bytecode failed: "
                                  "unknown function.");
                    goto fail;
                }

                if (func_idx < module->import_function_count)
                    func_type = module->import_functions[func_idx].u.function.func_type;
                else
                    func_type =
                        module->functions[func_idx - module->import_function_count]->func_type;

                if (func_type->param_count > 0) {
                    for (idx = (int32)(func_type->param_count - 1); idx >= 0; idx--) {
                        POP_TYPE(func_type->types[idx]);
#if WASM_ENABLE_FAST_INTERP != 0
                        POP_OFFSET_TYPE(func_type->types[idx]);
#endif
                    }
                }

                for (i = 0; i < func_type->result_count; i++) {
                    PUSH_TYPE(func_type->types[func_type->param_count + i]);
                }

                func->has_op_func_call = true;
                break;
            }

            case WASM_OP_CALL_INDIRECT:
            {
                int32 idx;
                WASMType *func_type;
                uint32 type_idx;

                if (module->table_count == 0
                    && module->import_table_count == 0) {
                    set_error_buf(error_buf, error_buf_size,
                                  "WASM loader prepare bytecode failed: "
                                  "call indirect with unknown table");
                    goto fail;
                }

                read_leb_uint32(p, p_end, type_idx);

                /* reserved byte 0x00 */
                if (*p++ != 0x00) {
                    set_error_buf(error_buf, error_buf_size,
                                  "WASM loader prepare bytecode failed: "
                                  "zero flag expected");
                    goto fail;
                }

                POP_I32();

                if (type_idx >= module->type_count) {
                    set_error_buf(error_buf, error_buf_size,
                                  "WASM loader prepare bytecode failed: "
                                  "unknown type");
                    goto fail;
                }

                func_type = module->types[type_idx];

                if (func_type->param_count > 0) {
                    for (idx = (int32)(func_type->param_count - 1); idx >= 0; idx--) {
                        POP_TYPE(func_type->types[idx]);
                    }
                }

                for (i = 0; i < func_type->result_count; i++) {
                    PUSH_TYPE(func_type->types[func_type->param_count + i]);

                }

                func->has_op_func_call = true;
                break;
            }

            case WASM_OP_DROP:
            case WASM_OP_DROP_64:
            {
                BranchBlock *cur_block = loader_ctx->frame_csp - 1;
                int32 available_stack_cell = (int32)
                    (loader_ctx->stack_cell_num - cur_block->stack_cell_num);

                if (available_stack_cell <= 0
                    && !cur_block->is_stack_polymorphic) {
                    set_error_buf(error_buf, error_buf_size,
                                  "WASM loader prepare bytecode failed: "
                                  "type mismatch, opcode drop was found "
                                  "but stack was empty");
                    goto fail;
                }

                if (available_stack_cell > 0) {
                    if (*(loader_ctx->frame_ref - 1) == REF_I32
                        || *(loader_ctx->frame_ref - 1) == REF_F32) {
                        loader_ctx->frame_ref--;
                        loader_ctx->stack_cell_num--;

                    }
                    else {
                        loader_ctx->frame_ref -= 2;
                        loader_ctx->stack_cell_num -= 2;
#if (WASM_ENABLE_FAST_INTERP == 0) || (WASM_ENABLE_JIT != 0)
                        *(p - 1) = WASM_OP_DROP_64;
#endif
                    }
                }
                else {
#if WASM_ENABLE_FAST_INTERP != 0
                    skip_label();
#endif
                }
                break;
            }

            case WASM_OP_SELECT:
            case WASM_OP_SELECT_64:
            {
                uint8 ref_type;
                BranchBlock *cur_block = loader_ctx->frame_csp - 1;
                int32 available_stack_cell;

                POP_I32();

                available_stack_cell = (int32)
                    (loader_ctx->stack_cell_num - cur_block->stack_cell_num);

                if (available_stack_cell <= 0
                    && !cur_block->is_stack_polymorphic) {
                    set_error_buf(error_buf, error_buf_size,
                                  "WASM loader prepare bytecode failed: "
                                  "type mismatch, opcode select was found "
                                  "but stack was empty");
                    goto fail;
                }

                if (available_stack_cell > 0) {
                    switch (*(loader_ctx->frame_ref - 1)) {
                        case REF_I32:
                        case REF_F32:
                            break;
                        case REF_I64_2:
                        case REF_F64_2:
#if (WASM_ENABLE_FAST_INTERP == 0) || (WASM_ENABLE_JIT != 0)
                            *(p - 1) = WASM_OP_SELECT_64;
#endif
#if WASM_ENABLE_FAST_INTERP != 0
                            if (loader_ctx->p_code_compiled) {
#if WASM_ENABLE_ABS_LABEL_ADDR != 0
                                *(void**)(loader_ctx->p_code_compiled - 2 - sizeof(void*)) =
                                    handle_table[WASM_OP_SELECT_64];
#else
                                *((int16*)loader_ctx->p_code_compiled - 2) = (int16)
                                    (handle_table[WASM_OP_SELECT_64] - handle_table[0]);
#endif
                            }
#endif
                            break;
                    }

                    ref_type = *(loader_ctx->frame_ref - 1);
#if WASM_ENABLE_FAST_INTERP != 0
                    POP_OFFSET_TYPE(ref_type);
#endif
                    POP_TYPE(ref_type);
#if WASM_ENABLE_FAST_INTERP != 0
                    POP_OFFSET_TYPE(ref_type);
#endif
                    POP_TYPE(ref_type);
#if WASM_ENABLE_FAST_INTERP != 0
                    PUSH_OFFSET_TYPE(ref_type);
#endif
                    PUSH_TYPE(ref_type);
                }
                else {
#if WASM_ENABLE_FAST_INTERP != 0
                    PUSH_OFFSET_TYPE(VALUE_TYPE_ANY);
#endif
                    PUSH_TYPE(VALUE_TYPE_ANY);
                }
                break;
            }

            case WASM_OP_GET_LOCAL:
            {
                p_org = p - 1;
                GET_LOCAL_INDEX_TYPE_AND_OFFSET();
                PUSH_TYPE(local_type);

#if WASM_ENABLE_FAST_INTERP != 0
                /* Get Local is optimized out */
                skip_label();
                disable_emit = true;
                operand_offset = local_offset;
                PUSH_OFFSET_TYPE(local_type);
#else
#if (WASM_ENABLE_WAMR_COMPILER == 0) && (WASM_ENABLE_JIT == 0)
                if (local_offset < 0x80) {
                    *p_org++ = EXT_OP_GET_LOCAL_FAST;
                    if (local_type == VALUE_TYPE_I32
                        || local_type == VALUE_TYPE_F32)
                        *p_org++ = (uint8)local_offset;
                    else
                        *p_org++ = (uint8)(local_offset | 0x80);
                    while (p_org < p)
                        *p_org++ = WASM_OP_NOP;
                }
#endif
#endif
                break;
            }

            case WASM_OP_SET_LOCAL:
            {
                p_org = p - 1;
                GET_LOCAL_INDEX_TYPE_AND_OFFSET();
                POP_TYPE(local_type);

#if WASM_ENABLE_FAST_INTERP != 0
                if (!(preserve_referenced_local(loader_ctx, opcode, local_offset,
                                                local_type, &preserve_local,
                                                error_buf, error_buf_size)))
                    goto fail;

                if (local_offset < 256) {
                    skip_label();
                    if ((!preserve_local) && (LAST_OP_OUTPUT_I32())) {
                        if (loader_ctx->p_code_compiled)
                            *(int16*)(loader_ctx->p_code_compiled - 2) = local_offset;
                        loader_ctx->frame_offset --;
                        loader_ctx->dynamic_offset --;
                    }
                    else if ((!preserve_local) && (LAST_OP_OUTPUT_I64())) {
                        if (loader_ctx->p_code_compiled)
                            *(int16*)(loader_ctx->p_code_compiled - 2) = local_offset;
                        loader_ctx->frame_offset -= 2;
                        loader_ctx->dynamic_offset -= 2;
                    }
                    else {
                        if (local_type == VALUE_TYPE_I32
                            || local_type == VALUE_TYPE_F32) {
                            emit_label(EXT_OP_SET_LOCAL_FAST);
                            emit_byte(loader_ctx, local_offset);
                        }
                        else {
                            emit_label(EXT_OP_SET_LOCAL_FAST_I64);
                            emit_byte(loader_ctx, local_offset);
                        }
                        POP_OFFSET_TYPE(local_type);
                    }
                }
                else {   /* local index larger than 255, reserve leb */
                    p_org ++;
                    emit_leb();
                    POP_OFFSET_TYPE(local_type);
                }
#else
#if (WASM_ENABLE_WAMR_COMPILER == 0) && (WASM_ENABLE_JIT == 0)
                if (local_offset < 0x80) {
                    *p_org++ = EXT_OP_SET_LOCAL_FAST;
                    if (local_type == VALUE_TYPE_I32
                        || local_type == VALUE_TYPE_F32)
                        *p_org++ = (uint8)local_offset;
                    else
                        *p_org++ = (uint8)(local_offset | 0x80);
                    while (p_org < p)
                        *p_org++ = WASM_OP_NOP;
                }
#endif
#endif
                break;
            }

            case WASM_OP_TEE_LOCAL:
            {
                p_org = p - 1;
                GET_LOCAL_INDEX_TYPE_AND_OFFSET();
#if WASM_ENABLE_FAST_INTERP != 0
                /* If the stack is in polymorphic state, do fake pop and push on
                    offset stack to keep the depth of offset stack to be the same
                    with ref stack */
                BranchBlock *cur_block = loader_ctx->frame_csp - 1;
                if (cur_block->is_stack_polymorphic) {
                    POP_OFFSET_TYPE(local_type);
                    PUSH_OFFSET_TYPE(local_type);
                }
#endif
                POP_TYPE(local_type);
                PUSH_TYPE(local_type);

#if WASM_ENABLE_FAST_INTERP != 0
                if (!(preserve_referenced_local(loader_ctx, opcode, local_offset,
                                                local_type, &preserve_local,
                                                error_buf, error_buf_size)))
                    goto fail;

                if (local_offset < 256) {
                    skip_label();
                    if (local_type == VALUE_TYPE_I32
                        || local_type == VALUE_TYPE_F32) {
                        emit_label(EXT_OP_TEE_LOCAL_FAST);
                        emit_byte(loader_ctx, local_offset);
                    }
                    else {
                        emit_label(EXT_OP_TEE_LOCAL_FAST_I64);
                        emit_byte(loader_ctx, local_offset);
                    }
                }
                else {  /* local index larger than 255, reserve leb */
                    p_org ++;
                    emit_leb();
                }
                emit_operand(loader_ctx, *(loader_ctx->frame_offset -
                        wasm_value_type_cell_num(local_type)));
#else
#if (WASM_ENABLE_WAMR_COMPILER == 0) && (WASM_ENABLE_JIT == 0)
                if (local_offset < 0x80) {
                    *p_org++ = EXT_OP_TEE_LOCAL_FAST;
                    if (local_type == VALUE_TYPE_I32
                        || local_type == VALUE_TYPE_F32)
                        *p_org++ = (uint8)local_offset;
                    else
                        *p_org++ = (uint8)(local_offset | 0x80);
                    while (p_org < p)
                        *p_org++ = WASM_OP_NOP;
                }
#endif
#endif
                break;
            }

            case WASM_OP_GET_GLOBAL:
            {
                p_org = p - 1;
                read_leb_uint32(p, p_end, global_idx);
                if (global_idx >= global_count) {
                    set_error_buf(error_buf, error_buf_size,
                                  "WASM loader prepare bytecode failed: "
                                  "unknown global.");
                    goto fail;
                }

                global_type =
                  global_idx < module->import_global_count
                    ? module->import_globals[global_idx].u.global.type
                    : module->globals[global_idx - module->import_global_count]
                        .type;

                PUSH_TYPE(global_type);

#if WASM_ENABLE_FAST_INTERP == 0
#if (WASM_ENABLE_WAMR_COMPILER == 0) && (WASM_ENABLE_JIT == 0)
                if (global_type == VALUE_TYPE_I64
                    || global_type == VALUE_TYPE_F64) {
                    *p_org = WASM_OP_GET_GLOBAL_64;
                }
#endif
#else /* else of WASM_ENABLE_FAST_INTERP */
                if (global_type == VALUE_TYPE_I64
                    || global_type == VALUE_TYPE_F64) {
                    skip_label();
                    emit_label(WASM_OP_GET_GLOBAL_64);
                }
                emit_uint32(loader_ctx, global_idx);
                PUSH_OFFSET_TYPE(global_type);
#endif /* end of WASM_ENABLE_FAST_INTERP */
                break;
            }

            case WASM_OP_SET_GLOBAL:
            {
                bool is_mutable = false;

                p_org = p - 1;
                read_leb_uint32(p, p_end, global_idx);
                if (global_idx >= global_count) {
                    set_error_buf(error_buf, error_buf_size,
                                  "WASM loader prepare bytecode failed: "
                                  "unknown global.");
                    goto fail;
                }

                is_mutable =
                  global_idx < module->import_global_count
                    ? module->import_globals[global_idx].u.global.is_mutable
                    : module->globals[global_idx - module->import_global_count]
                        .is_mutable;
                if (!is_mutable) {
                    set_error_buf(error_buf,
                                  error_buf_size,
                                  "global is immutable");
                    goto fail;
                }

                global_type =
                  global_idx < module->import_global_count
                    ? module->import_globals[global_idx].u.global.type
                    : module->globals[global_idx - module->import_global_count]
                        .type;

                POP_TYPE(global_type);

#if WASM_ENABLE_FAST_INTERP == 0
#if (WASM_ENABLE_WAMR_COMPILER == 0) && (WASM_ENABLE_JIT == 0)
                if (global_type == VALUE_TYPE_I64
                    || global_type == VALUE_TYPE_F64) {
                    *p_org = WASM_OP_SET_GLOBAL_64;
                }
                else if (module->llvm_aux_stack_size > 0
                         && global_idx == module->llvm_aux_stack_global_index) {
                    *p_org = WASM_OP_SET_GLOBAL_AUX_STACK;
                }
#endif
#else /* else of WASM_ENABLE_FAST_INTERP */
                if (global_type == VALUE_TYPE_I64
                    || global_type == VALUE_TYPE_F64) {
                    skip_label();
                    emit_label(WASM_OP_SET_GLOBAL_64);
                }
                else if (module->llvm_aux_stack_size > 0
                         && global_idx == module->llvm_aux_stack_global_index) {
                    skip_label();
                    emit_label(WASM_OP_SET_GLOBAL_AUX_STACK);
                }
                emit_uint32(loader_ctx, global_idx);
                POP_OFFSET_TYPE(global_type);
#endif /* end of WASM_ENABLE_FAST_INTERP */
                break;
            }

            /* load */
            case WASM_OP_I32_LOAD:
            case WASM_OP_I32_LOAD8_S:
            case WASM_OP_I32_LOAD8_U:
            case WASM_OP_I32_LOAD16_S:
            case WASM_OP_I32_LOAD16_U:
            case WASM_OP_I64_LOAD:
            case WASM_OP_I64_LOAD8_S:
            case WASM_OP_I64_LOAD8_U:
            case WASM_OP_I64_LOAD16_S:
            case WASM_OP_I64_LOAD16_U:
            case WASM_OP_I64_LOAD32_S:
            case WASM_OP_I64_LOAD32_U:
            case WASM_OP_F32_LOAD:
            case WASM_OP_F64_LOAD:
            /* store */
            case WASM_OP_I32_STORE:
            case WASM_OP_I32_STORE8:
            case WASM_OP_I32_STORE16:
            case WASM_OP_I64_STORE:
            case WASM_OP_I64_STORE8:
            case WASM_OP_I64_STORE16:
            case WASM_OP_I64_STORE32:
            case WASM_OP_F32_STORE:
            case WASM_OP_F64_STORE:
            {
#if WASM_ENABLE_FAST_INTERP != 0
                /* change F32/F64 into I32/I64 */
                if (opcode == WASM_OP_F32_LOAD) {
                    skip_label();
                    emit_label(WASM_OP_I32_LOAD);
                }
                else if (opcode == WASM_OP_F64_LOAD) {
                    skip_label();
                    emit_label(WASM_OP_I64_LOAD);
                }
                else if (opcode == WASM_OP_F32_STORE) {
                    skip_label();
                    emit_label(WASM_OP_I32_STORE);
                }
                else if (opcode == WASM_OP_F64_STORE) {
                    skip_label();
                    emit_label(WASM_OP_I64_STORE);
                }
#endif
                CHECK_MEMORY();
                read_leb_uint32(p, p_end, align); /* align */
                read_leb_uint32(p, p_end, mem_offset); /* offset */
                if (!check_memory_access_align(opcode, align,
                                               error_buf, error_buf_size)) {
                    goto fail;
                }
#if WASM_ENABLE_FAST_INTERP != 0
                emit_uint32(loader_ctx, mem_offset);
#endif
                switch (opcode)
                {
                    /* load */
                    case WASM_OP_I32_LOAD:
                    case WASM_OP_I32_LOAD8_S:
                    case WASM_OP_I32_LOAD8_U:
                    case WASM_OP_I32_LOAD16_S:
                    case WASM_OP_I32_LOAD16_U:
                        POP_AND_PUSH(VALUE_TYPE_I32, VALUE_TYPE_I32);
                        break;
                    case WASM_OP_I64_LOAD:
                    case WASM_OP_I64_LOAD8_S:
                    case WASM_OP_I64_LOAD8_U:
                    case WASM_OP_I64_LOAD16_S:
                    case WASM_OP_I64_LOAD16_U:
                    case WASM_OP_I64_LOAD32_S:
                    case WASM_OP_I64_LOAD32_U:
                        POP_AND_PUSH(VALUE_TYPE_I32, VALUE_TYPE_I64);
                        break;
                    case WASM_OP_F32_LOAD:
                        POP_AND_PUSH(VALUE_TYPE_I32, VALUE_TYPE_F32);
                        break;
                    case WASM_OP_F64_LOAD:
                        POP_AND_PUSH(VALUE_TYPE_I32, VALUE_TYPE_F64);
                        break;
                    /* store */
                    case WASM_OP_I32_STORE:
                    case WASM_OP_I32_STORE8:
                    case WASM_OP_I32_STORE16:
                        POP_I32();
                        POP_I32();
                        break;
                    case WASM_OP_I64_STORE:
                    case WASM_OP_I64_STORE8:
                    case WASM_OP_I64_STORE16:
                    case WASM_OP_I64_STORE32:
                        POP_I64();
                        POP_I32();
                        break;
                    case WASM_OP_F32_STORE:
                        POP_F32();
                        POP_I32();
                        break;
                    case WASM_OP_F64_STORE:
                        POP_F64();
                        POP_I32();
                        break;
                    default:
                        break;
                }
                break;
            }

            case WASM_OP_MEMORY_SIZE:
                CHECK_MEMORY();
                /* reserved byte 0x00 */
                if (*p++ != 0x00) {
                    set_error_buf(error_buf, error_buf_size,
                                  "WASM loader prepare bytecode failed: "
                                  "zero flag expected");
                    goto fail;
                }
                PUSH_I32();
                break;

            case WASM_OP_MEMORY_GROW:
                CHECK_MEMORY();
                /* reserved byte 0x00 */
                if (*p++ != 0x00) {
                    set_error_buf(error_buf, error_buf_size,
                                  "WASM loader prepare bytecode failed: "
                                  "zero flag expected");
                    goto fail;
                }
                POP_AND_PUSH(VALUE_TYPE_I32, VALUE_TYPE_I32);

                func->has_op_memory_grow = true;
                module->possible_memory_grow = true;
                break;

            case WASM_OP_I32_CONST:
                read_leb_int32(p, p_end, i32_const);
#if WASM_ENABLE_FAST_INTERP != 0
                skip_label();
                disable_emit = true;
                GET_CONST_OFFSET(VALUE_TYPE_I32, i32_const);
#else
                (void)i32_const;
#endif
                PUSH_I32();
                break;

            case WASM_OP_I64_CONST:
                read_leb_int64(p, p_end, i64);
#if WASM_ENABLE_FAST_INTERP != 0
                skip_label();
                disable_emit = true;
                GET_CONST_OFFSET(VALUE_TYPE_I64, i64);
#endif
                PUSH_I64();
                break;

            case WASM_OP_F32_CONST:
                p += sizeof(float32);
                PUSH_F32();
                break;

            case WASM_OP_F64_CONST:
                p += sizeof(float64);
#if WASM_ENABLE_FAST_INTERP != 0
                skip_label();
                disable_emit = true;
                /* Some MCU may require 8-byte align */
                bh_memcpy_s((uint8*)&f64, sizeof(float64), p_org, sizeof(float64));
                GET_CONST_F64_OFFSET(VALUE_TYPE_F64, f64);
#endif
                PUSH_F64();
                break;

            case WASM_OP_I32_EQZ:
                POP_AND_PUSH(VALUE_TYPE_I32, VALUE_TYPE_I32);
                break;

            case WASM_OP_I32_EQ:
            case WASM_OP_I32_NE:
            case WASM_OP_I32_LT_S:
            case WASM_OP_I32_LT_U:
            case WASM_OP_I32_GT_S:
            case WASM_OP_I32_GT_U:
            case WASM_OP_I32_LE_S:
            case WASM_OP_I32_LE_U:
            case WASM_OP_I32_GE_S:
            case WASM_OP_I32_GE_U:
                POP2_AND_PUSH(VALUE_TYPE_I32, VALUE_TYPE_I32);
                break;

            case WASM_OP_I64_EQZ:
                POP_AND_PUSH(VALUE_TYPE_I64, VALUE_TYPE_I32);
                break;

            case WASM_OP_I64_EQ:
            case WASM_OP_I64_NE:
            case WASM_OP_I64_LT_S:
            case WASM_OP_I64_LT_U:
            case WASM_OP_I64_GT_S:
            case WASM_OP_I64_GT_U:
            case WASM_OP_I64_LE_S:
            case WASM_OP_I64_LE_U:
            case WASM_OP_I64_GE_S:
            case WASM_OP_I64_GE_U:
                POP2_AND_PUSH(VALUE_TYPE_I64, VALUE_TYPE_I32);
                break;

            case WASM_OP_F32_EQ:
            case WASM_OP_F32_NE:
            case WASM_OP_F32_LT:
            case WASM_OP_F32_GT:
            case WASM_OP_F32_LE:
            case WASM_OP_F32_GE:
                POP2_AND_PUSH(VALUE_TYPE_F32, VALUE_TYPE_I32);
                break;

            case WASM_OP_F64_EQ:
            case WASM_OP_F64_NE:
            case WASM_OP_F64_LT:
            case WASM_OP_F64_GT:
            case WASM_OP_F64_LE:
            case WASM_OP_F64_GE:
                POP2_AND_PUSH(VALUE_TYPE_F64, VALUE_TYPE_I32);
                break;

            case WASM_OP_I32_CLZ:
            case WASM_OP_I32_CTZ:
            case WASM_OP_I32_POPCNT:
                POP_AND_PUSH(VALUE_TYPE_I32, VALUE_TYPE_I32);
                break;

            case WASM_OP_I32_ADD:
            case WASM_OP_I32_SUB:
            case WASM_OP_I32_MUL:
            case WASM_OP_I32_DIV_S:
            case WASM_OP_I32_DIV_U:
            case WASM_OP_I32_REM_S:
            case WASM_OP_I32_REM_U:
            case WASM_OP_I32_AND:
            case WASM_OP_I32_OR:
            case WASM_OP_I32_XOR:
            case WASM_OP_I32_SHL:
            case WASM_OP_I32_SHR_S:
            case WASM_OP_I32_SHR_U:
            case WASM_OP_I32_ROTL:
            case WASM_OP_I32_ROTR:
                POP2_AND_PUSH(VALUE_TYPE_I32, VALUE_TYPE_I32);
                break;

            case WASM_OP_I64_CLZ:
            case WASM_OP_I64_CTZ:
            case WASM_OP_I64_POPCNT:
                POP_AND_PUSH(VALUE_TYPE_I64, VALUE_TYPE_I64);
                break;

            case WASM_OP_I64_ADD:
            case WASM_OP_I64_SUB:
            case WASM_OP_I64_MUL:
            case WASM_OP_I64_DIV_S:
            case WASM_OP_I64_DIV_U:
            case WASM_OP_I64_REM_S:
            case WASM_OP_I64_REM_U:
            case WASM_OP_I64_AND:
            case WASM_OP_I64_OR:
            case WASM_OP_I64_XOR:
            case WASM_OP_I64_SHL:
            case WASM_OP_I64_SHR_S:
            case WASM_OP_I64_SHR_U:
            case WASM_OP_I64_ROTL:
            case WASM_OP_I64_ROTR:
                POP2_AND_PUSH(VALUE_TYPE_I64, VALUE_TYPE_I64);
                break;

            case WASM_OP_F32_ABS:
            case WASM_OP_F32_NEG:
            case WASM_OP_F32_CEIL:
            case WASM_OP_F32_FLOOR:
            case WASM_OP_F32_TRUNC:
            case WASM_OP_F32_NEAREST:
            case WASM_OP_F32_SQRT:
                POP_AND_PUSH(VALUE_TYPE_F32, VALUE_TYPE_F32);
                break;

            case WASM_OP_F32_ADD:
            case WASM_OP_F32_SUB:
            case WASM_OP_F32_MUL:
            case WASM_OP_F32_DIV:
            case WASM_OP_F32_MIN:
            case WASM_OP_F32_MAX:
            case WASM_OP_F32_COPYSIGN:
                POP2_AND_PUSH(VALUE_TYPE_F32, VALUE_TYPE_F32);
                break;

            case WASM_OP_F64_ABS:
            case WASM_OP_F64_NEG:
            case WASM_OP_F64_CEIL:
            case WASM_OP_F64_FLOOR:
            case WASM_OP_F64_TRUNC:
            case WASM_OP_F64_NEAREST:
            case WASM_OP_F64_SQRT:
                POP_AND_PUSH(VALUE_TYPE_F64, VALUE_TYPE_F64);
                break;

            case WASM_OP_F64_ADD:
            case WASM_OP_F64_SUB:
            case WASM_OP_F64_MUL:
            case WASM_OP_F64_DIV:
            case WASM_OP_F64_MIN:
            case WASM_OP_F64_MAX:
            case WASM_OP_F64_COPYSIGN:
                POP2_AND_PUSH(VALUE_TYPE_F64, VALUE_TYPE_F64);
                break;

            case WASM_OP_I32_WRAP_I64:
                POP_AND_PUSH(VALUE_TYPE_I64, VALUE_TYPE_I32);
                break;

            case WASM_OP_I32_TRUNC_S_F32:
            case WASM_OP_I32_TRUNC_U_F32:
                POP_AND_PUSH(VALUE_TYPE_F32, VALUE_TYPE_I32);
                break;

            case WASM_OP_I32_TRUNC_S_F64:
            case WASM_OP_I32_TRUNC_U_F64:
                POP_AND_PUSH(VALUE_TYPE_F64, VALUE_TYPE_I32);
                break;

            case WASM_OP_I64_EXTEND_S_I32:
            case WASM_OP_I64_EXTEND_U_I32:
                POP_AND_PUSH(VALUE_TYPE_I32, VALUE_TYPE_I64);
                break;

            case WASM_OP_I64_TRUNC_S_F32:
            case WASM_OP_I64_TRUNC_U_F32:
                POP_AND_PUSH(VALUE_TYPE_F32, VALUE_TYPE_I64);
                break;

            case WASM_OP_I64_TRUNC_S_F64:
            case WASM_OP_I64_TRUNC_U_F64:
                POP_AND_PUSH(VALUE_TYPE_F64, VALUE_TYPE_I64);
                break;

            case WASM_OP_F32_CONVERT_S_I32:
            case WASM_OP_F32_CONVERT_U_I32:
                POP_AND_PUSH(VALUE_TYPE_I32, VALUE_TYPE_F32);
                break;

            case WASM_OP_F32_CONVERT_S_I64:
            case WASM_OP_F32_CONVERT_U_I64:
                POP_AND_PUSH(VALUE_TYPE_I64, VALUE_TYPE_F32);
                break;

            case WASM_OP_F32_DEMOTE_F64:
                POP_AND_PUSH(VALUE_TYPE_F64, VALUE_TYPE_F32);
                break;

            case WASM_OP_F64_CONVERT_S_I32:
            case WASM_OP_F64_CONVERT_U_I32:
                POP_AND_PUSH(VALUE_TYPE_I32, VALUE_TYPE_F64);
                break;

            case WASM_OP_F64_CONVERT_S_I64:
            case WASM_OP_F64_CONVERT_U_I64:
                POP_AND_PUSH(VALUE_TYPE_I64, VALUE_TYPE_F64);
                break;

            case WASM_OP_F64_PROMOTE_F32:
                POP_AND_PUSH(VALUE_TYPE_F32, VALUE_TYPE_F64);
                break;

            case WASM_OP_I32_REINTERPRET_F32:
                POP_AND_PUSH(VALUE_TYPE_F32, VALUE_TYPE_I32);
                break;

            case WASM_OP_I64_REINTERPRET_F64:
                POP_AND_PUSH(VALUE_TYPE_F64, VALUE_TYPE_I64);
                break;

            case WASM_OP_F32_REINTERPRET_I32:
                POP_AND_PUSH(VALUE_TYPE_I32, VALUE_TYPE_F32);
                break;

            case WASM_OP_F64_REINTERPRET_I64:
                POP_AND_PUSH(VALUE_TYPE_I64, VALUE_TYPE_F64);
                break;

            case WASM_OP_I32_EXTEND8_S:
            case WASM_OP_I32_EXTEND16_S:
                POP_AND_PUSH(VALUE_TYPE_I32, VALUE_TYPE_I32);
                break;

            case WASM_OP_I64_EXTEND8_S:
            case WASM_OP_I64_EXTEND16_S:
            case WASM_OP_I64_EXTEND32_S:
                POP_AND_PUSH(VALUE_TYPE_I64, VALUE_TYPE_I64);
                break;

            case WASM_OP_MISC_PREFIX:
            {
                opcode = read_uint8(p);
#if WASM_ENABLE_FAST_INTERP != 0
                emit_byte(loader_ctx, opcode);
#endif
                switch (opcode)
                {
                case WASM_OP_I32_TRUNC_SAT_S_F32:
                case WASM_OP_I32_TRUNC_SAT_U_F32:
                    POP_AND_PUSH(VALUE_TYPE_F32, VALUE_TYPE_I32);
                    break;
                case WASM_OP_I32_TRUNC_SAT_S_F64:
                case WASM_OP_I32_TRUNC_SAT_U_F64:
                    POP_AND_PUSH(VALUE_TYPE_F64, VALUE_TYPE_I32);
                    break;
                case WASM_OP_I64_TRUNC_SAT_S_F32:
                case WASM_OP_I64_TRUNC_SAT_U_F32:
                    POP_AND_PUSH(VALUE_TYPE_F32, VALUE_TYPE_I64);
                    break;
                case WASM_OP_I64_TRUNC_SAT_S_F64:
                case WASM_OP_I64_TRUNC_SAT_U_F64:
                    POP_AND_PUSH(VALUE_TYPE_F64, VALUE_TYPE_I64);
                    break;

                default:
                    if (error_buf != NULL)
                        snprintf(error_buf, error_buf_size,
                                 "WASM module load failed: "
                                 "invalid opcode 0xfc %02x.", opcode);
                    goto fail;
                    break;
                }
                break;
            }
            default:
                if (error_buf != NULL)
                    snprintf(error_buf, error_buf_size,
                             "WASM module load failed: "
                             "invalid opcode %02x.", opcode);
                goto fail;
        }

#if WASM_ENABLE_FAST_INTERP != 0
        last_op = opcode;
#endif
    }

    if (loader_ctx->csp_num > 0) {
        set_error_buf(error_buf, error_buf_size,
                      "WASM module load failed: "
                      "function body must end with END opcode.");
        goto fail;
    }


    func->max_stack_cell_num = loader_ctx->max_stack_cell_num;

    func->max_block_num = loader_ctx->max_csp_num;
    return_value = true;

fail:
    wasm_loader_ctx_destroy(loader_ctx);

    (void)u8;
    (void)u32;
    (void)i32;
    (void)i64;
    (void)local_offset;
    (void)p_org;
    (void)mem_offset;
    (void)align;
    return return_value;
}
