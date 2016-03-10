
#define _stringify(x) #x
#define stringify(x) _stringify(x)

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
                "." stringify(OPCODE_TYPE) " " stringify(OPCODE_ENCODE(code)) "\n" \
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
MAKE_S2E_FUNCTION_WITH_ASM(s2e_version, 0x00, int, "mov r0, #0", void)

/**
 * Enable symbolic execution.
 */
MAKE_S2E_FUNCTION(s2e_enable_symbolic, 0x01, void, void)

/**
 * Disable symbolic execution.
 */
MAKE_S2E_FUNCTION(s2e_disable_symbolic, 0x02, void, void)
MAKE_S2E_FUNCTION(s2e_make_symbolic, 0x03, void, void* address, unsigned size, const char* name)
MAKE_S2E_FUNCTION(s2e_get_path_id, 0x05, unsigned, void)
MAKE_S2E_FUNCTION(s2e_kill_state, 0x06, void, int exit_code, const char *message)

MAKE_S2E_FUNCTION_WITH_ASM(s2e_print_expression, 0x07, void, "mov r2, r1", int expression, const char* name)

MAKE_S2E_FUNCTION(s2e_print_memory, 0x08, void, void* address, unsigned size, const char* name)
MAKE_S2E_FUNCTION(s2e_enable_forking, 0x09, void, void)
MAKE_S2E_FUNCTION(s2e_disable_forking, 0x0A, void, void)
MAKE_S2E_FUNCTION(s2e_message, 0x10, void, const char* message)
MAKE_S2E_FUNCTION(s2e_make_concolic, 0x11, void, void* address, unsigned size, const char* name)
MAKE_S2E_FUNCTION(s2e_concretize, 0x20, void, void* address, unsigned size)
MAKE_S2E_FUNCTION(s2e_get_example, 0x21, unsigned long, void* address, unsigned size)
MAKE_S2E_FUNCTION(s2e_sleep, 0x32, void, unsigned duration)
MAKE_S2E_FUNCTION(s2e_get_ram_object_bits, 0x52, void, void)
MAKE_S2E_FUNCTION(s2e_merge_point, 0x70, void, void)
MAKE_S2E_FUNCTION(s2e_rawmon_loadmodule, 0xAA, void, const char* module_name, void* loadbase, unsigned size)
