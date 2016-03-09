#ifndef _S2E_BASE_INSTRUCTIONS_H
#define _S2E_BASE_INSTRUCTIONS_H

/**
 * Invoke an S2E opcode.
 *
 * @param name Name of the function triggering opcode
 * @param code Opcode to trigger
 * @param ret Return type of the function
 * @param assembler A string of additional assembler instructions to insert before the opcode is triggered
 * @param ... Function parameter types and names
 */
#define MAKE_S2E_FUNCTION_WITH_ASM(name, code, ret, assembler, ...) \
    static __attribute__((naked)) ret name(__VA_ARGS__) { \
        __asm__(assembler "\n" \
                ".long " #code "\n" \
                "bx lr\n"); \
    }

/**
 * Invoke an S2E opcode.
 * See previous documentation.
 */
#define MAKE_S2E_FUNCTION(name, code, ret, ...) \
    MAKE_S2E_FUNCTION_WITH_ASM(name, code, ret, "", __VA_ARGS__)

/**
 * Get the S2E version.
 * @return Currently returns 0 if not running under S2E, and 1 if running under S2E.
 */
MAKE_S2E_FUNCTION_WITH_ASM(s2e_version, 0xFF000000, int, "mov r0, #0", void)

/**
 * Enable symbolic execution.
 */
MAKE_S2E_FUNCTION(s2e_enable_symbolic, 0xFF010000, void, void)

/**
 * Disable symbolic execution.
 */
MAKE_S2E_FUNCTION(s2e_disable_symbolic, 0xFF020000, void, void)
MAKE_S2E_FUNCTION(s2e_make_symbolic, 0xFF030000, void, void* address, unsigned size, const char* name)
MAKE_S2E_FUNCTION(s2e_get_path_id, 0xFF050000, unsigned, void)
MAKE_S2E_FUNCTION(s2e_kill_state, 0xFF060000, void, int exit_code, const char *message)

MAKE_S2E_FUNCTION_WITH_ASM(s2e_print_expression, 0xFF070000, void, "mov r2, r1", int expression, const char* name)

MAKE_S2E_FUNCTION(s2e_print_memory, 0xFF080000, void, void* address, unsigned size, const char* name)
MAKE_S2E_FUNCTION(s2e_enable_forking, 0xFF090000, void, void)
MAKE_S2E_FUNCTION(s2e_disable_forking, 0xFF0A0000, void, void)
MAKE_S2E_FUNCTION(s2e_message, 0xFF100000, void, const char* message)
MAKE_S2E_FUNCTION(s2e_make_concolic, 0xFF110000, void, void* address, unsigned size, const char* name)
MAKE_S2E_FUNCTION(s2e_concretize, 0xFF200000, void, void* address, unsigned size)
MAKE_S2E_FUNCTION(s2e_get_example, 0xFF210000, unsigned long, void* address, unsigned size)
MAKE_S2E_FUNCTION(s2e_sleep, 0xFF320000, void, unsigned duration)
MAKE_S2E_FUNCTION(s2e_get_ram_object_bits, 0xFF520000, void, void)
MAKE_S2E_FUNCTION(s2e_merge_point, 0xFF700000, void, void)
MAKE_S2E_FUNCTION(s2e_rawmon_loadmodule, 0xFFAA0000, void, const char* module_name, void* loadbase, unsigned size)


#endif /* _S2E_BASE_INSTRUCTIONS_H */
