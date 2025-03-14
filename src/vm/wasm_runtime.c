/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasm_runtime.h"
#include "wasm_loader.h"
#include "wasm_interp.h"
#include "bh_common.h"
#include "bh_log.h"
#include "mem_alloc.h"
#include "wasm_runtime_common.h"
#if WASM_ENABLE_SHARED_MEMORY != 0
#include "wasm_shared_memory.h"
#endif

static void
set_error_buf(char *error_buf, uint32 error_buf_size, const char *string)
{
    if (error_buf != NULL)
        snprintf(error_buf, error_buf_size, "%s", string);
}

WASMModule*
wasm_load(const uint8 *buf, uint32 size,
          char *error_buf, uint32 error_buf_size)
{
    return wasm_loader_load(buf, size, error_buf, error_buf_size);
}

WASMModule*
wasm_load_from_sections(WASMSection *section_list,
                        char *error_buf, uint32_t error_buf_size)
{
    return wasm_loader_load_from_sections(section_list,
                                          error_buf, error_buf_size);
}

void
wasm_unload(WASMModule *module)
{
    wasm_loader_unload(module);
}

static void *
runtime_malloc(uint64 size, char *error_buf, uint32 error_buf_size)
{
    void *mem;

    if (size >= UINT32_MAX
        || !(mem = wasm_runtime_malloc((uint32)size))) {
        set_error_buf(error_buf, error_buf_size,
                      "WASM module instantiate failed: "
                      "allocate memory failed.");
        return NULL;
    }

    memset(mem, 0, (uint32)size);
    return mem;
}


/**
 * Destroy memory instances.
 */
static void
memories_deinstantiate(WASMModuleInstance *module_inst,
                       WASMMemoryInstance **memories,
                       uint32 count)
{
    uint32 i;
    if (memories) {
        for (i = 0; i < count; i++)
            if (memories[i]) {
#if WASM_ENABLE_MULTI_MODULE != 0
                if (memories[i]->owner != module_inst)
                    continue;
#endif
#if WASM_ENABLE_SHARED_MEMORY != 0
                if (memories[i]->is_shared) {
                    int32 ref_count =
                        shared_memory_dec_reference(
                            (WASMModuleCommon *)module_inst->module);
                    bh_assert(ref_count >= 0);

                    /* if the reference count is not zero,
                        don't free the memory */
                    if (ref_count > 0)
                        continue;
                }
#endif
                if (memories[i]->heap_handle) {
                    mem_allocator_destroy(memories[i]->heap_handle);
                    memories[i]->heap_handle = NULL;
                }
                wasm_runtime_free(memories[i]);
            }
        wasm_runtime_free(memories);
  }
  (void)module_inst;
}

static WASMMemoryInstance*
memory_instantiate(WASMModuleInstance *module_inst,
                   uint32 num_bytes_per_page,
                   uint32 init_page_count, uint32 max_page_count,
                   uint32 heap_size, uint32 flags,
                   char *error_buf, uint32 error_buf_size)
{
    WASMMemoryInstance *memory;
    uint64 heap_and_inst_size = offsetof(WASMMemoryInstance, base_addr) +
                                (uint64)heap_size;
    uint64 total_size = heap_and_inst_size +
                        num_bytes_per_page * (uint64)init_page_count;


    /* Allocate memory space, addr data and global data */
    if (!(memory = runtime_malloc(total_size,
                                  error_buf, error_buf_size))) {
        return NULL;
    }

    memory->module_type = Wasm_Module_Bytecode;
    memory->num_bytes_per_page = num_bytes_per_page;
    memory->cur_page_count = init_page_count;
    memory->max_page_count = max_page_count;

    memory->heap_data = memory->base_addr;
    memory->memory_data = memory->heap_data + heap_size;
    {
        memory->end_addr = memory->memory_data +
                           num_bytes_per_page * memory->cur_page_count;
    }

    bh_assert(memory->end_addr - (uint8*)memory == (uint32)total_size);

    /* Initialize heap */
    if (heap_size > 0
        && !(memory->heap_handle =
               mem_allocator_create(memory->heap_data, heap_size))) {
        wasm_runtime_free(memory);
        return NULL;
    }

    memory->heap_base_offset = -(int32)heap_size;

    return memory;
}

/**
 * Instantiate memories in a module.
 */
static WASMMemoryInstance **
memories_instantiate(const WASMModule *module,
                     WASMModuleInstance *module_inst,
                     uint32 heap_size, char *error_buf, uint32 error_buf_size)
{
    WASMImport *import;
    uint32 mem_index = 0, i, memory_count =
        module->import_memory_count + module->memory_count;
    uint64 total_size;
    WASMMemoryInstance **memories, *memory;

    total_size = sizeof(WASMMemoryInstance*) * (uint64)memory_count;

    if (!(memories = runtime_malloc(total_size,
                                    error_buf, error_buf_size))) {
        return NULL;
    }

    /* instantiate memories from import section */
    import = module->import_memories;
    for (i = 0; i < module->import_memory_count; i++, import++) {
        uint32 num_bytes_per_page = import->u.memory.num_bytes_per_page;
        uint32 init_page_count = import->u.memory.init_page_count;
        uint32 max_page_count = import->u.memory.max_page_count;
        uint32 flags = import->u.memory.flags;
        uint32 actual_heap_size = heap_size;

        {
            if (!(memory = memories[mem_index++] = memory_instantiate(
                    module_inst, num_bytes_per_page, init_page_count,
                    max_page_count, actual_heap_size, flags,
                    error_buf, error_buf_size))) {
                set_error_buf(error_buf, error_buf_size,
                              "Instantiate memory failed: "
                              "allocate memory failed.");
                memories_deinstantiate(
                  module_inst,
                  memories, memory_count);
                return NULL;
            }
        }
    }

    /* instantiate memories from memory section */
    for (i = 0; i < module->memory_count; i++) {
        if (!(memory = memories[mem_index++] =
                    memory_instantiate(module_inst,
                                       module->memories[i].num_bytes_per_page,
                                       module->memories[i].init_page_count,
                                       module->memories[i].max_page_count,
                                       heap_size, module->memories[i].flags,
                                       error_buf, error_buf_size))) {
            set_error_buf(error_buf, error_buf_size,
                          "Instantiate memory failed: "
                          "allocate memory failed.");
            memories_deinstantiate(
              module_inst,
              memories, memory_count);
            return NULL;
        }
    }

    if (mem_index == 0) {
        /**
         * no import memory and define memory, but still need heap
         * for wasm code
         */
        if (!(memory = memories[mem_index++] =
                    memory_instantiate(module_inst, 0, 0, 0, heap_size, 0,
                                       error_buf, error_buf_size))) {
            set_error_buf(error_buf, error_buf_size,
                          "Instantiate memory failed: "
                          "allocate memory failed.\n");
            memories_deinstantiate(module_inst, memories, memory_count);
            return NULL;
        }
    }

    bh_assert(mem_index == memory_count);
    (void)module_inst;
    return memories;
}

/**
 * Destroy table instances.
 */
static void
tables_deinstantiate(WASMTableInstance **tables, uint32 count)
{
    uint32 i;
    if (tables) {
        for (i = 0; i < count; i++)
            if (tables[i])
                wasm_runtime_free(tables[i]);
        wasm_runtime_free(tables);
    }
}

/**
 * Instantiate tables in a module.
 */
static WASMTableInstance **
tables_instantiate(const WASMModule *module,
                   WASMModuleInstance *module_inst,
                   char *error_buf, uint32 error_buf_size)
{
    WASMImport *import;
    uint32 table_index = 0, i, table_count =
        module->import_table_count + module->table_count;
    uint64 total_size = sizeof(WASMTableInstance*) * (uint64)table_count;
    WASMTableInstance **tables, *table;

    if (!(tables = runtime_malloc(total_size,
                                  error_buf, error_buf_size))) {
        return NULL;
    }

    /* instantiate tables from import section */
    import = module->import_tables;
    for (i = 0; i < module->import_table_count; i++, import++) {
#if WASM_ENABLE_MULTI_MODULE != 0
        WASMTableInstance *table_inst_linked = NULL;
        WASMModuleInstance *module_inst_linked = NULL;
        if (import->u.table.import_module) {
            LOG_DEBUG("(%s, %s) is a table of a sub-module",
                      import->u.table.module_name,
                      import->u.memory.field_name);

            module_inst_linked =
              get_sub_module_inst(module_inst, import->u.table.import_module);
            bh_assert(module_inst_linked);

            table_inst_linked = wasm_lookup_table(module_inst_linked,
                                                  import->u.table.field_name);
            bh_assert(table_inst_linked);

            total_size = offsetof(WASMTableInstance, base_addr);
        }
        else
#endif
        {
            /* it is a built-in table */
            total_size = offsetof(WASMTableInstance, base_addr)
                         + sizeof(uint32) * (uint64)import->u.table.init_size;
        }

        if (!(table = tables[table_index++] = runtime_malloc
                    (total_size, error_buf, error_buf_size))) {
            tables_deinstantiate(tables, table_count);
            return NULL;
        }

        /* Set all elements to -1 to mark them as uninitialized elements */
        memset(table, -1, (uint32)total_size);
#if WASM_ENABLE_MULTI_MODULE != 0
        table->table_inst_linked = table_inst_linked;
        if (table_inst_linked != NULL) {
            table->elem_type = table_inst_linked->elem_type;
            table->cur_size = table_inst_linked->cur_size;
            table->max_size = table_inst_linked->max_size;
        }
        else
#endif
        {
            table->elem_type = import->u.table.elem_type;
            table->cur_size = import->u.table.init_size;
            table->max_size = import->u.table.max_size;
        }
    }

    /* instantiate tables from table section */
    for (i = 0; i < module->table_count; i++) {
        total_size = offsetof(WASMTableInstance, base_addr) +
                     sizeof(uint32) * (uint64)module->tables[i].init_size;
        if (!(table = tables[table_index++] = runtime_malloc
                    (total_size, error_buf, error_buf_size))) {
            tables_deinstantiate(tables, table_count);
            return NULL;
        }

        /* Set all elements to -1 to mark them as uninitialized elements */
        memset(table, -1, (uint32)total_size);
        table->elem_type = module->tables[i].elem_type;
        table->cur_size = module->tables[i].init_size;
        table->max_size = module->tables[i].max_size;
#if WASM_ENABLE_MULTI_MODULE != 0
        table->table_inst_linked = NULL;
#endif
    }

    bh_assert(table_index == table_count);
    (void)module_inst;
    return tables;
}

/**
 * Destroy function instances.
 */
static void
functions_deinstantiate(WASMFunctionInstance *functions, uint32 count)
{
    if (functions) {
        wasm_runtime_free(functions);
    }
}

/**
 * Instantiate functions in a module.
 */
static WASMFunctionInstance *
functions_instantiate(const WASMModule *module,
                      WASMModuleInstance *module_inst,
                      char *error_buf, uint32 error_buf_size)
{
    WASMImport *import;
    uint32 i, function_count =
        module->import_function_count + module->function_count;
    uint64 total_size = sizeof(WASMFunctionInstance) * (uint64)function_count;
    WASMFunctionInstance *functions, *function;

    if (!(functions = runtime_malloc(total_size,
                                     error_buf, error_buf_size))) {
        return NULL;
    }

    /* instantiate functions from import section */
    function = functions;
    import = module->import_functions;
    for (i = 0; i < module->import_function_count; i++, import++) {
        function->is_import_func = true;

#if WASM_ENABLE_MULTI_MODULE != 0
        if (import->u.function.import_module) {
            LOG_DEBUG("(%s, %s) is a function of a sub-module",
                      import->u.function.module_name,
                      import->u.function.field_name);

            function->import_module_inst =
              get_sub_module_inst(module_inst,
                                  import->u.function.import_module);
            bh_assert(function->import_module_inst);

            WASMFunction *function_linked =
              import->u.function.import_func_linked;

            function->u.func = function_linked;
            function->import_func_inst =
              wasm_lookup_function(function->import_module_inst,
                                   import->u.function.field_name,
                                   NULL);
            bh_assert(function->import_func_inst);

            function->param_cell_num = function->u.func->param_cell_num;
            function->ret_cell_num = function->u.func->ret_cell_num;
            function->local_cell_num = function->u.func->local_cell_num;
            function->param_count =
              (uint16)function->u.func->func_type->param_count;
            function->local_count = (uint16)function->u.func->local_count;
            function->param_types = function->u.func->func_type->types;
            function->local_types = function->u.func->local_types;
            function->local_offsets = function->u.func->local_offsets;
#if WASM_ENABLE_FAST_INTERP != 0
            function->const_cell_num = function->u.func->const_cell_num;
#endif
        }
        else
#endif /* WASM_ENABLE_MULTI_MODULE */
        {
            LOG_DEBUG("(%s, %s) is a function of native",
                      import->u.function.module_name,
                      import->u.function.field_name);
            function->u.func_import = &import->u.function;
            function->param_cell_num =
              import->u.function.func_type->param_cell_num;
            function->ret_cell_num =
              import->u.function.func_type->ret_cell_num;
            function->param_count =
              (uint16)function->u.func_import->func_type->param_count;
            function->param_types = function->u.func_import->func_type->types;
            function->local_cell_num = 0;
            function->local_count = 0;
            function->local_types = NULL;
        }

        function++;
    }

    /* instantiate functions from function section */
    for (i = 0; i < module->function_count; i++) {
        function->is_import_func = false;
        function->u.func = module->functions[i];

        function->param_cell_num = function->u.func->param_cell_num;
        function->ret_cell_num = function->u.func->ret_cell_num;
        function->local_cell_num = function->u.func->local_cell_num;

        function->param_count = (uint16)function->u.func->func_type->param_count;
        function->local_count = (uint16)function->u.func->local_count;
        function->param_types = function->u.func->func_type->types;
        function->local_types = function->u.func->local_types;

        function->local_offsets = function->u.func->local_offsets;


        function++;
    }

    bh_assert((uint32)(function - functions) == function_count);
    (void)module_inst;
    return functions;
}

/**
 * Destroy global instances.
 */
static void
globals_deinstantiate(WASMGlobalInstance *globals)
{
    if (globals)
        wasm_runtime_free(globals);
}

/**
 * init_expr->u ==> init_val
 */
static bool
parse_init_expr(const InitializerExpression *init_expr,
                const WASMGlobalInstance *global_inst_array,
                uint32 boundary, WASMValue *init_val)
{
    if (init_expr->init_expr_type == INIT_EXPR_TYPE_GET_GLOBAL) {
        uint32 target_global_index = init_expr->u.global_index;
        /**
         * a global gets the init value of another global
         */
        if (target_global_index >= boundary) {
            LOG_DEBUG("unknown target global, %d", target_global_index);
            return false;
        }

        /**
         * it will work if using WASMGlobalImport and WASMGlobal in
         * WASMModule, but will have to face complicated cases
         *
         * but we still have no sure the target global has been
         * initialized before
         */
        WASMValue target_value =
          global_inst_array[target_global_index].initial_value;
        bh_memcpy_s(init_val, sizeof(WASMValue), &target_value,
                    sizeof(target_value));
    }
    else {
        bh_memcpy_s(init_val, sizeof(WASMValue), &init_expr->u,
                    sizeof(init_expr->u));
    }
    return true;
}

/**
 * Instantiate globals in a module.
 */
static WASMGlobalInstance *
globals_instantiate(const WASMModule *module,
                    WASMModuleInstance *module_inst,
                    uint32 *p_global_data_size, char *error_buf,
                    uint32 error_buf_size)
{
    WASMImport *import;
    uint32 global_data_offset = 0;
    uint32 i, global_count =
        module->import_global_count + module->global_count;
    uint64 total_size = sizeof(WASMGlobalInstance) * (uint64)global_count;
    WASMGlobalInstance *globals, *global;

    if (!(globals = runtime_malloc(total_size,
                                   error_buf, error_buf_size))) {
        return NULL;
    }

    /* instantiate globals from import section */
    global = globals;
    import = module->import_globals;
    for (i = 0; i < module->import_global_count; i++, import++) {
        WASMGlobalImport *global_import = &import->u.global;
        global->type = global_import->type;
        global->is_mutable = global_import->is_mutable;

        {
            /* native globals share their initial_values in one module */
            global->initial_value = global_import->global_data_linked;
        }
        global->data_offset = global_data_offset;
        global_data_offset += wasm_value_type_size(global->type);

        global++;
    }

    /* instantiate globals from global section */
    for (i = 0; i < module->global_count; i++) {
        bool ret = false;
        uint32 global_count =
          module->import_global_count + module->global_count;
        InitializerExpression *init_expr = &(module->globals[i].init_expr);

        global->type = module->globals[i].type;
        global->is_mutable = module->globals[i].is_mutable;
        global->data_offset = global_data_offset;

        global_data_offset += wasm_value_type_size(global->type);

        /**
         * first init, it might happen that the target global instance
         * has not been initialize yet
         */
        if (init_expr->init_expr_type != INIT_EXPR_TYPE_GET_GLOBAL) {
            ret =
              parse_init_expr(init_expr, globals, global_count,
                              &(global->initial_value));
            if (!ret) {
                set_error_buf(error_buf, error_buf_size,
                              "Instantiate global failed: unknown global.");
                return NULL;
            }
        }
        global++;
    }

    bh_assert((uint32)(global - globals) == global_count);
    *p_global_data_size = global_data_offset;
    (void)module_inst;
    return globals;
}

static bool
globals_instantiate_fix(WASMGlobalInstance *globals,
                        const WASMModule *module,
                        char *error_buf, uint32 error_buf_size)
{
    WASMGlobalInstance *global = globals;
    uint32 i;
    uint32 global_count = module->import_global_count + module->global_count;

    /**
     * second init, only target global instances from global
     * (ignore import_global)
     * to fix skipped init_value in the previous round
     * hope two rounds are enough but how about a chain ?
     */
    for (i = 0; i < module->global_count; i++) {
        bool ret = false;
        InitializerExpression *init_expr = &module->globals[i].init_expr;

        if (init_expr->init_expr_type == INIT_EXPR_TYPE_GET_GLOBAL) {
            ret = parse_init_expr(init_expr, globals, global_count,
                                  &global->initial_value);
            if (!ret) {
                set_error_buf(error_buf, error_buf_size,
                              "Instantiate global failed: unknown global.");
                return false;
            }
        }

        global++;
    }
    return true;
}

/**
 * Return export function count in module export section.
 */
static uint32
get_export_count(const WASMModule *module, uint8 kind)
{
    WASMExport *export = module->exports;
    uint32 count = 0, i;

    for (i = 0; i < module->export_count; i++, export++)
        if (export->kind == kind)
            count++;

    return count;
}

/**
 * Destroy export function instances.
 */
static void
export_functions_deinstantiate(WASMExportFuncInstance *functions)
{
    if (functions)
        wasm_runtime_free(functions);
}

/**
 * Instantiate export functions in a module.
 */
static WASMExportFuncInstance*
export_functions_instantiate(const WASMModule *module,
                             WASMModuleInstance *module_inst,
                             uint32 export_func_count,
                             char *error_buf, uint32 error_buf_size)
{
    WASMExportFuncInstance *export_funcs, *export_func;
    WASMExport *export = module->exports;
    uint32 i;
    uint64 total_size = sizeof(WASMExportFuncInstance) * (uint64)export_func_count;

    if (!(export_func = export_funcs = runtime_malloc
                (total_size, error_buf, error_buf_size))) {
        return NULL;
    }

    for (i = 0; i < module->export_count; i++, export++)
        if (export->kind == EXPORT_KIND_FUNC) {
            export_func->name = export->name;
            export_func->function = &module_inst->functions[export->index];
            export_func++;
        }

    bh_assert((uint32)(export_func - export_funcs) == export_func_count);
    return export_funcs;
}


static bool
execute_post_inst_function(WASMModuleInstance *module_inst)
{
    WASMFunctionInstance *post_inst_func = NULL;
    WASMType *post_inst_func_type;
    uint32 i;

    for (i = 0; i < module_inst->export_func_count; i++)
        if (!strcmp(module_inst->export_functions[i].name, "__post_instantiate")) {
            post_inst_func = module_inst->export_functions[i].function;
            break;
        }

    if (!post_inst_func)
        /* Not found */
        return true;

    post_inst_func_type = post_inst_func->u.func->func_type;
    if (post_inst_func_type->param_count != 0
        || post_inst_func_type->result_count != 0)
        /* Not a valid function type, ignore it */
        return true;

    return wasm_create_exec_env_and_call_function(module_inst, post_inst_func,
                                                  0, NULL);
}


static bool
execute_start_function(WASMModuleInstance *module_inst)
{
    WASMFunctionInstance *func = module_inst->start_function;

    if (!func)
        return true;

    bh_assert(!func->is_import_func && func->param_cell_num == 0
              && func->ret_cell_num == 0);

    return wasm_create_exec_env_and_call_function(module_inst, func, 0, NULL);
}

/**
 * Instantiate module
 */
WASMModuleInstance*
wasm_instantiate(WASMModule *module, bool is_sub_inst,
                 uint32 stack_size, uint32 heap_size,
                 char *error_buf, uint32 error_buf_size)
{
    WASMModuleInstance *module_inst;
    WASMGlobalInstance *globals = NULL, *global;
    uint32 global_count, global_data_size = 0, i;
    uint32 base_offset, length;
    uint8 *global_data, *global_data_end;
#if WASM_ENABLE_MULTI_MODULE != 0
    bool ret = false;
#endif

    if (!module)
        return NULL;

    /* Check heap size */
    heap_size = align_uint(heap_size, 8);
    if (heap_size > APP_HEAP_SIZE_MAX)
        heap_size = APP_HEAP_SIZE_MAX;

    /* Allocate the memory */
    if (!(module_inst = runtime_malloc(sizeof(WASMModuleInstance),
                                       error_buf, error_buf_size))) {
        return NULL;
    }
    memset(module_inst, 0, (uint32)sizeof(WASMModuleInstance));

    module_inst->module = module;

    /* Instantiate global firstly to get the mutable data size */
    global_count = module->import_global_count + module->global_count;
    if (global_count && !(globals = globals_instantiate(
                            module,
                            module_inst,
                            &global_data_size, error_buf, error_buf_size))) {
        wasm_deinstantiate(module_inst, false);
        return NULL;
    }
    module_inst->global_count = global_count;
    module_inst->globals = globals;

    module_inst->memory_count =
        module->import_memory_count + module->memory_count;
    module_inst->table_count =
        module->import_table_count + module->table_count;
    module_inst->function_count =
        module->import_function_count + module->function_count;

    /* export */
    module_inst->export_func_count = get_export_count(module, EXPORT_KIND_FUNC);

    if (global_count > 0) {
        if (!(module_inst->global_data = runtime_malloc
                    (global_data_size, error_buf, error_buf_size))) {
            wasm_deinstantiate(module_inst, false);
            return NULL;
        }
    }

    /* Instantiate memories/tables/functions */
    if ((module_inst->memory_count > 0
         && !(module_inst->memories =
                memories_instantiate(module,
                                     module_inst,
                                     heap_size, error_buf, error_buf_size)))
        || (module_inst->table_count > 0
            && !(module_inst->tables =
                   tables_instantiate(module,
                                      module_inst,
                                      error_buf, error_buf_size)))
        || (module_inst->function_count > 0
            && !(module_inst->functions =
                   functions_instantiate(module,
                                         module_inst,
                                         error_buf, error_buf_size)))
        || (module_inst->export_func_count > 0
            && !(module_inst->export_functions = export_functions_instantiate(
                   module, module_inst, module_inst->export_func_count,
                   error_buf, error_buf_size)))
    ) {
        wasm_deinstantiate(module_inst, false);
        return NULL;
    }

    if (global_count > 0) {
        /**
         * since there might be some globals are not instantiate the first
         * instantiate round
         */
        if (!globals_instantiate_fix(globals, module,
                                     error_buf, error_buf_size)) {
            wasm_deinstantiate(module_inst, false);
            return NULL;
        }

        /* Initialize the global data */
        global_data = module_inst->global_data;
        global_data_end = global_data + global_data_size;
        global = globals;
        for (i = 0; i < global_count; i++, global++) {
            switch (global->type) {
                case VALUE_TYPE_I32:
                case VALUE_TYPE_F32:
                    *(int32*)global_data = global->initial_value.i32;
                    global_data += sizeof(int32);
                    break;
                case VALUE_TYPE_I64:
                case VALUE_TYPE_F64:
                    bh_memcpy_s(global_data, (uint32)(global_data_end - global_data),
                                &global->initial_value.i64, sizeof(int64));
                    global_data += sizeof(int64);
                    break;
                default:
                    bh_assert(0);
            }
        }
        bh_assert(global_data == global_data_end);
    }

    /* Initialize the memory data with data segment section */
    module_inst->default_memory =
      module_inst->memory_count ? module_inst->memories[0] : NULL;

    for (i = 0; i < module->data_seg_count; i++) {
        WASMMemoryInstance *memory = NULL;
        uint8 *memory_data = NULL;
        uint32 memory_size = 0;
        WASMDataSeg *data_seg = module->data_segments[i];
		dump ("data_seg",data_seg->data, data_seg->data_length);
        /* has check it in loader */
        memory = module_inst->memories[data_seg->memory_index];
        bh_assert(memory);

        memory_data = memory->memory_data;
        bh_assert(memory_data);

        memory_size = memory->num_bytes_per_page * memory->cur_page_count;

        bh_assert(data_seg->base_offset.init_expr_type
                    == INIT_EXPR_TYPE_I32_CONST
                  || data_seg->base_offset.init_expr_type
                       == INIT_EXPR_TYPE_GET_GLOBAL);

        if (data_seg->base_offset.init_expr_type
            == INIT_EXPR_TYPE_GET_GLOBAL) {
            bh_assert(data_seg->base_offset.u.global_index < global_count
                        && globals[data_seg->base_offset.u.global_index].type
                            == VALUE_TYPE_I32);
            data_seg->base_offset.u.i32 =
                globals[data_seg->base_offset.u.global_index]
                .initial_value.i32;
        }

        /* check offset since length might negative */
        base_offset = (uint32)data_seg->base_offset.u.i32;
        if (base_offset > memory_size) {
            LOG_DEBUG("base_offset(%d) > memory_size(%d)", base_offset,
                      memory_size);
            set_error_buf(error_buf, error_buf_size,
                          "data segment does not fit.");
            wasm_deinstantiate(module_inst, false);
            return NULL;
        }

        /* check offset + length(could be zero) */
        length = data_seg->data_length;
        if (base_offset + length > memory_size) {
            LOG_DEBUG("base_offset(%d) + length(%d) > memory_size(%d)",
                      base_offset, length, memory_size);
            set_error_buf(
              error_buf, error_buf_size,
              "Instantiate module failed: data segment does not fit.");
            wasm_deinstantiate(module_inst, false);
            return NULL;
        }

        bh_memcpy_s(memory_data + base_offset, memory_size - base_offset,
                    data_seg->data, length);
    }
	dump ("memory_data", module_inst->memories[0]->memory_data, 64);
    /* Initialize the table data with table segment section */
    module_inst->default_table =
      module_inst->table_count ? module_inst->tables[0] : NULL;
    for (i = 0; i < module->table_seg_count; i++) {
        WASMTableSeg *table_seg = module->table_segments + i;
        /* has check it in loader */
        WASMTableInstance *table = module_inst->tables[table_seg->table_index];
        bh_assert(table);

        uint32 *table_data = (uint32 *)table->base_addr;
#if WASM_ENABLE_MULTI_MODULE != 0
        table_data = table->table_inst_linked
                        ? (uint32 *)table->table_inst_linked->base_addr
                        : table_data;
#endif
        bh_assert(table_data);

        /* init vec(funcidx) */
        bh_assert(table_seg->base_offset.init_expr_type
                    == INIT_EXPR_TYPE_I32_CONST
                  || table_seg->base_offset.init_expr_type
                       == INIT_EXPR_TYPE_GET_GLOBAL);

        if (table_seg->base_offset.init_expr_type
            == INIT_EXPR_TYPE_GET_GLOBAL) {
            bh_assert(table_seg->base_offset.u.global_index < global_count
                      && globals[table_seg->base_offset.u.global_index].type
                           == VALUE_TYPE_I32);
            table_seg->base_offset.u.i32 =
              globals[table_seg->base_offset.u.global_index].initial_value.i32;
        }

        /* check offset since length might negative */
        if ((uint32)table_seg->base_offset.u.i32 > table->cur_size) {
            LOG_DEBUG("base_offset(%d) > table->cur_size(%d)",
                      table_seg->base_offset.u.i32, table->cur_size);
            set_error_buf(error_buf, error_buf_size,
                          "elements segment does not fit");
            wasm_deinstantiate(module_inst, false);
            return NULL;
        }

        /* check offset + length(could be zero) */
        length = table_seg->function_count;
        if ((uint32)table_seg->base_offset.u.i32 + length > table->cur_size) {
            LOG_DEBUG("base_offset(%d) + length(%d)> table->cur_size(%d)",
                      table_seg->base_offset.u.i32, length, table->cur_size);
            set_error_buf(error_buf, error_buf_size,
                          "elements segment does not fit");
            wasm_deinstantiate(module_inst, false);
            return NULL;
        }

        /**
         * Check function index in the current module inst for now.
         * will check the linked table inst owner in future.
         * so loader check is enough
         */
        bh_memcpy_s(
          table_data + table_seg->base_offset.u.i32,
          (uint32)((table->cur_size - (uint32)table_seg->base_offset.u.i32)
                   * sizeof(uint32)),
          table_seg->func_indexes, (uint32)(length * sizeof(uint32)));
    }

    if (module->start_function != (uint32)-1) {
        /* TODO: fix start function can be import function issue */
        if (module->start_function >= module->import_function_count)
            module_inst->start_function =
                &module_inst->functions[module->start_function];
    }

    /* module instance type */
    module_inst->module_type = Wasm_Module_Bytecode;

    /* Initialize the thread related data */
    if (stack_size == 0)
        stack_size = DEFAULT_WASM_STACK_SIZE;
    module_inst->default_wasm_stack_size = stack_size;

    /* Execute __post_instantiate function */
    if (!execute_post_inst_function(module_inst)
        || !execute_start_function(module_inst)) {
        set_error_buf(error_buf, error_buf_size,
                      module_inst->cur_exception);
        wasm_deinstantiate(module_inst, false);
        return NULL;
    }

    (void)global_data_end;
    return module_inst;
}

void
wasm_deinstantiate(WASMModuleInstance *module_inst, bool is_sub_inst)
{
    if (!module_inst)
        return;

#if WASM_ENABLE_MULTI_MODULE != 0
    sub_module_deinstantiate(module_inst);
#endif

#if WASM_ENABLE_LIBC_WASI != 0
    /* Destroy wasi resource before freeing app heap, since some fields of
       wasi contex are allocated from app heap, and if app heap is freed,
       these fields will be set to NULL, we cannot free their internal data
       which may allocated from global heap. */
    /* Only destroy wasi ctx in the main module instance */
    if (!is_sub_inst)
        wasm_runtime_destroy_wasi((WASMModuleInstanceCommon*)module_inst);
#endif

    if (module_inst->memory_count > 0)
        memories_deinstantiate(
          module_inst,
          module_inst->memories, module_inst->memory_count);

    tables_deinstantiate(module_inst->tables, module_inst->table_count);
    functions_deinstantiate(module_inst->functions, module_inst->function_count);
    globals_deinstantiate(module_inst->globals);
    export_functions_deinstantiate(module_inst->export_functions);
#if WASM_ENABLE_MULTI_MODULE != 0
    export_globals_deinstantiate(module_inst->export_globals);
#endif

    if (module_inst->global_data)
        wasm_runtime_free(module_inst->global_data);

    wasm_runtime_free(module_inst);
}

WASMFunctionInstance*
wasm_lookup_function(const WASMModuleInstance *module_inst,
                     const char *name, const char *signature)
{
    uint32 i;
    for (i = 0; i < module_inst->export_func_count; i++)
        if (!strcmp(module_inst->export_functions[i].name, name))
            return module_inst->export_functions[i].function;
    (void)signature;
    return NULL;
}

bool
wasm_call_function(WASMExecEnv *exec_env,
                   WASMFunctionInstance *function,
                   unsigned argc, uint32 argv[])
{
	WASMModuleInstance *module_inst = (WASMModuleInstance*)exec_env->module_inst;
    wasm_interp_call_wasm(module_inst, exec_env, function, argc, argv);
    return !wasm_get_exception(module_inst) ? true : false;
}

bool
wasm_create_exec_env_and_call_function(WASMModuleInstance *module_inst,
                                       WASMFunctionInstance *func,
                                       unsigned argc, uint32 argv[])
{
    WASMExecEnv *exec_env;
    bool ret;

    if (!(exec_env = wasm_exec_env_create(
                            (WASMModuleInstanceCommon*)module_inst,
                            module_inst->default_wasm_stack_size))) {
        wasm_set_exception(module_inst, "allocate memory failed.");
        return false;
    }

    /* set thread handle and stack boundary */
    wasm_exec_env_set_thread_info(exec_env);

    ret = wasm_call_function(exec_env, func, argc, argv);
    wasm_exec_env_destroy(exec_env);
    return ret;
}

void
wasm_set_exception(WASMModuleInstance *module_inst,
                   const char *exception)
{
    if (exception)
        snprintf(module_inst->cur_exception,
                 sizeof(module_inst->cur_exception),
                 "Exception: %s", exception);
    else
        module_inst->cur_exception[0] = '\0';
}

const char*
wasm_get_exception(WASMModuleInstance *module_inst)
{
    if (module_inst->cur_exception[0] == '\0')
        return NULL;
    else
        return module_inst->cur_exception;
}

int32
wasm_module_malloc(WASMModuleInstance *module_inst, uint32 size,
                   void **p_native_addr)
{
    WASMMemoryInstance *memory = module_inst->default_memory;
    uint8 *addr = mem_allocator_malloc(memory->heap_handle, size);
    if (!addr) {
        wasm_set_exception(module_inst, "out of memory");
        return 0;
    }
    if (p_native_addr)
        *p_native_addr = addr;
    return (int32)(addr - memory->memory_data);
}

void
wasm_module_free(WASMModuleInstance *module_inst, int32 ptr)
{
    if (ptr) {
        WASMMemoryInstance *memory = module_inst->default_memory;
        uint8 *addr = memory->memory_data + ptr;
        if (memory->heap_data < addr && addr < memory->memory_data)
            mem_allocator_free(memory->heap_handle, addr);
    }
}

int32
wasm_module_dup_data(WASMModuleInstance *module_inst,
                     const char *src, uint32 size)
{
    char *buffer;
    int32 buffer_offset = wasm_module_malloc(module_inst, size,
                                             (void**)&buffer);
    if (buffer_offset != 0) {
        buffer = wasm_addr_app_to_native(module_inst, buffer_offset);
        bh_memcpy_s(buffer, size, src, size);
    }
    return buffer_offset;
}

bool
wasm_validate_app_addr(WASMModuleInstance *module_inst,
                       int32 app_offset, uint32 size)
{
    WASMMemoryInstance *memory = module_inst->default_memory;
    int32 memory_data_size =
        (int32)(memory->num_bytes_per_page * memory->cur_page_count);

    /* integer overflow check */
    if (app_offset + (int32)size < app_offset) {
        goto fail;
    }

    if (memory->heap_base_offset <= app_offset
        && app_offset + (int32)size <= memory_data_size) {
        return true;
    }
fail:
    wasm_set_exception(module_inst, "out of bounds memory access");
    return false;
}

bool
wasm_validate_native_addr(WASMModuleInstance *module_inst,
                          void *native_ptr, uint32 size)
{
    uint8 *addr = (uint8*)native_ptr;
    WASMMemoryInstance *memory = module_inst->default_memory;
    int32 memory_data_size =
        (int32)(memory->num_bytes_per_page * memory->cur_page_count);

    if (addr + size < addr) {
        goto fail;
    }

    if (memory->heap_data <= addr
        && addr + size <= memory->memory_data + memory_data_size) {
        return true;
    }
fail:
    wasm_set_exception(module_inst, "out of bounds memory access");
    return false;
}

void *
wasm_addr_app_to_native(WASMModuleInstance *module_inst,
                        int32 app_offset)
{
    WASMMemoryInstance *memory = module_inst->default_memory;
    uint8 *addr = memory->memory_data + app_offset;
    int32 memory_data_size =
        (int32)(memory->num_bytes_per_page * memory->cur_page_count);

    if (memory->heap_data <= addr
        && addr < memory->memory_data + memory_data_size)
        return addr;
    return NULL;
}

int32
wasm_addr_native_to_app(WASMModuleInstance *module_inst,
                        void *native_ptr)
{
    WASMMemoryInstance *memory = module_inst->default_memory;
    uint8 *addr = (uint8*)native_ptr;
    int32 memory_data_size =
        (int32)(memory->num_bytes_per_page * memory->cur_page_count);

    if (memory->heap_data <= addr
        && addr < memory->memory_data + memory_data_size)
        return (int32)(addr - memory->memory_data);
    return 0;
}

bool
wasm_get_app_addr_range(WASMModuleInstance *module_inst,
                        int32 app_offset,
                        int32 *p_app_start_offset,
                        int32 *p_app_end_offset)
{
    WASMMemoryInstance *memory = module_inst->default_memory;
    int32 memory_data_size =
        (int32)(memory->num_bytes_per_page * memory->cur_page_count);

    if (memory->heap_base_offset <= app_offset
        && app_offset < memory_data_size) {
        if (p_app_start_offset)
            *p_app_start_offset = memory->heap_base_offset;
        if (p_app_end_offset)
            *p_app_end_offset = memory_data_size;
        return true;
    }
    return false;
}

bool
wasm_get_native_addr_range(WASMModuleInstance *module_inst,
                           uint8 *native_ptr,
                           uint8 **p_native_start_addr,
                           uint8 **p_native_end_addr)
{
    WASMMemoryInstance *memory = module_inst->default_memory;
    uint8 *addr = (uint8*)native_ptr;
    int32 memory_data_size =
        (int32)(memory->num_bytes_per_page * memory->cur_page_count);

    if (memory->heap_data <= addr
        && addr < memory->memory_data + memory_data_size) {
        if (p_native_start_addr)
            *p_native_start_addr = memory->heap_data;
        if (p_native_end_addr)
            *p_native_end_addr = memory->memory_data + memory_data_size;
        return true;
    }
    return false;
}

bool
wasm_enlarge_memory(WASMModuleInstance *module, uint32 inc_page_count)
{
    WASMMemoryInstance *memory = module->default_memory, *new_memory;
    uint32 heap_size = memory->memory_data - memory->heap_data;
    uint32 total_size_old = memory->end_addr - (uint8*)memory;
    uint32 total_page_count = inc_page_count + memory->cur_page_count;
    uint64 total_size = offsetof(WASMMemoryInstance, base_addr)
                        + (uint64)heap_size
                        + memory->num_bytes_per_page * (uint64)total_page_count;
    void *heap_handle_old = memory->heap_handle;

    if (inc_page_count <= 0)
        /* No need to enlarge memory */
        return true;

    if (total_page_count < memory->cur_page_count /* integer overflow */
        || total_page_count > memory->max_page_count) {
        wasm_set_exception(module, "fail to enlarge memory.");
        return false;
    }

    if (total_size >= UINT32_MAX) {
        wasm_set_exception(module, "fail to enlarge memory.");
        return false;
    }

#if WASM_ENABLE_SHARED_MEMORY != 0
    if (memory->is_shared) {
        /* For shared memory, we have reserved the maximum spaces during
            instantiate, only change the cur_page_count here */
        memory->cur_page_count = total_page_count;
        return true;
    }
#endif

    if (heap_size > 0) {
        /* Destroy heap's lock firstly, if its memory is re-allocated,
           we cannot access its lock again. */
        mem_allocator_destroy_lock(memory->heap_handle);
    }
    if (!(new_memory = wasm_runtime_realloc(memory, (uint32)total_size))) {
        if (!(new_memory = wasm_runtime_malloc((uint32)total_size))) {
            if (heap_size > 0) {
                /* Restore heap's lock if memory re-alloc failed */
                mem_allocator_reinit_lock(memory->heap_handle);
            }
            wasm_set_exception(module, "fail to enlarge memory.");
            return false;
        }
        bh_memcpy_s((uint8*)new_memory, (uint32)total_size,
                    (uint8*)memory, total_size_old);
        wasm_runtime_free(memory);
    }

    memset((uint8*)new_memory + total_size_old,
           0, (uint32)total_size - total_size_old);

    if (heap_size > 0) {
        new_memory->heap_handle = (uint8*)heap_handle_old +
                                  ((uint8*)new_memory - (uint8*)memory);
        if (mem_allocator_migrate(new_memory->heap_handle,
                                  heap_handle_old) != 0) {
            wasm_set_exception(module, "fail to enlarge memory.");
            return false;
        }
    }

    new_memory->cur_page_count = total_page_count;
    new_memory->heap_data = new_memory->base_addr;
    new_memory->memory_data = new_memory->base_addr + heap_size;
    new_memory->end_addr = new_memory->memory_data +
                            new_memory->num_bytes_per_page * total_page_count;

    module->memories[0] = module->default_memory = new_memory;
    return true;
}

bool
wasm_call_indirect(WASMExecEnv *exec_env,
                   uint32_t element_indices,
                   uint32_t argc, uint32_t argv[])
{
    WASMModuleInstance *module_inst = NULL;
    WASMTableInstance *table_inst = NULL;
    uint32_t function_indices = 0;
    WASMFunctionInstance *function_inst = NULL;

    module_inst =
        (WASMModuleInstance*)exec_env->module_inst;
    bh_assert(module_inst);

    table_inst = module_inst->default_table;
    if (!table_inst) {
        wasm_set_exception(module_inst, "unknown table");
        goto got_exception;
    }

    if (element_indices >= table_inst->cur_size) {
        wasm_set_exception(module_inst, "undefined element");
        goto got_exception;
    }

    /**
     * please be aware that table_inst->base_addr may point
     * to another module's table
     **/
    function_indices = ((uint32_t*)table_inst->base_addr)[element_indices];
    if (function_indices == 0xFFFFFFFF) {
        wasm_set_exception(module_inst, "uninitialized element");
        goto got_exception;
    }

    /**
     * we insist to call functions owned by the module itself
     **/
    if (function_indices >= module_inst->function_count) {
        wasm_set_exception(module_inst, "unknown function");
        goto got_exception;
    }

    function_inst = module_inst->functions + function_indices;

    wasm_interp_call_wasm(module_inst, exec_env, function_inst, argc, argv);

    return !wasm_get_exception(module_inst) ? true : false;

got_exception:
    return false;
}
