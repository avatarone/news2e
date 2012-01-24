

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;S2E test - this runs in protected mode
[bits 32]
s2e_test:
    ;call s2e_test_memspeed
    ;call s2e_sm_test
    call s2e_sm_succeed_test
    ;call s2e_test2
    ;call s2e_test_memobj
    ;call s2e_fork_test2
    ;call s2e_symbhwio_test
    ;call s2e_jmptbl_test
    ;call test_ndis
    ;jmp s2e_test
    ;call s2e_test1
    ;call s2e_isa_dev
    cli
    hlt



s2e_fork_test2:
    push 16
    call s2e_fork_depth
    add esp, 4

    ;Finish the test
    push msg_ok
    push 0
    call s2e_kill_state
    add esp, 8
    ret


;Fork lots of states
s2e_fork_depth:
    push ebp
    mov ebp, esp
    sub esp, 4

    call s2e_fork_enable

    mov eax, dword [ebp + 8]
    mov dword [ebp - 4], eax  ; Set forking depth to eax (2^^eax states)
ssf1:
    call s2e_int
    cmp eax, 0
    ja ssf2
ssf2:
    dec dword [ebp - 4]; Decrement forking depth
    jnz ssf1

    leave
    ret

s2e_test_memspeed:
    mov ecx, 1000000000

lbl1:
    push eax
    pop eax
    dec ecx
    jnz lbl1

    push 0
    push 0
    call s2e_kill_state
    add esp, 8
    ret


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;Testing small memory objects
%define S2E_PAGE_SZE
memok: db "Memory test passed ok", 0
membad: db "MEMORY TEST FAILED", 0
val: db "Value", 0

s2e_test_memobj:
    %push mycontext
    %stacksize flat
    %assign %$localsize 0
    %local root_id:dword, cur_count:dword, obj_bits:dword

    enter %$localsize,0

    mov dword [cur_count], 0

    call s2e_get_path_id
    mov [root_id], eax

    call s2e_get_ram_object_bits
    mov [obj_bits], eax

    ;Create 100 states
  stm0:
    cmp dword [cur_count], 10
    jz stm1

    call s2e_int
    cmp eax, 0
    jz stm1                 ;One state exits the loop
    inc dword [cur_count]   ;The other one continues it
    jmp stm0

  stm1:
    ;In each state, we have a loop that
    ;writes its state id in the page, yields,
    ;checks the content when it gets back the control,
    ;then exits


    call s2e_get_path_id ;Get current path id in eax
    mov edi, 0x100000     ;Start filling at 1MB

    mov ecx, [obj_bits]
    mov edx, 1
    shl edx, cl          ;edx contains the size of the page
    shr edx, 2           ;We want to store dwords

    pusha
    push val
    push edx
    call s2e_print_expression
    add esp, 8
    popa

  stm2:

    ;Fill the memory with the test pattern
    mov ecx, edx
    cld
    rep stosd

    ;Schedule another state
    call s2e_coop_yield

    ;Check that the memory is correct
    mov ecx, edx
    shl ecx, 2
    sub edi, ecx
    mov ecx, edx

  stm3:
    scasd
    loopz stm3
    jnz sterr

    pusha
    push val
    push edi
    call s2e_print_expression
    add esp, 8
    popa

    cmp edi, 0x1000000
    jb stm2

    ;------------------------
    ;Successfully completed the memory test
    push memok
    push 0
    call s2e_kill_state

    ;------------------------
    ;Error during memory check
  sterr:
    push membad
    push edi
    call s2e_kill_state

    leave
    ret



;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;Testing I/O with symbolic address/values

s2e_symbhwio_test:
    call s2e_enable

    call s2e_int        ;Get symbolic value
    mov edi, 0xFEC00000 ;APIC address
    mov [edi], eax      ;Write symbolic value to APIC

    mov dx, ax          ;Write to symbolic port a symbolic value
    out dx, ax
ret

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;Testing enable/disable forking

s2e_fork_test:
    call s2e_enable
    call s2e_fork_enable

    call s2e_int
    cmp eax, 0
    ja sft1

    nop

sft1:

    call s2e_fork_disable
    call s2e_int
    cmp eax, 0
    ja sft2

    nop

sft2:

    push 0
    push 0
    call s2e_kill_state
    add esp, 8

ret

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;Testing symbolic offsets in jump tables

s2e_jmptbl1:
    dd sj0
    dd sj1
    dd sj2
    dd sj3
    dd sj4
    dd sj5
    dd sj6
    dd sj7
    dd sj8
    dd sj9
    dd sj10
    dd sj11
    dd sj12
    dd sj13
    dd sj14
    dd sj15
    dd sj16

s2e_jmptbl1_m0: db "Case 0", 0
s2e_jmptbl1_m1: db "Case 1", 0
s2e_jmptbl1_m2: db "Case 2", 0
s2e_jmptbl1_m3: db "Case 3", 0
s2e_jmptbl1_m4: db "Case 4", 0
s2e_jmptbl1_m5: db "Case 5", 0
s2e_jmptbl1_m6: db "Case 6", 0
s2e_jmptbl1_m7: db "Case 7", 0
s2e_jmptbl1_m8: db "Case 8", 0
s2e_jmptbl1_m9: db "Case 9", 0
s2e_jmptbl1_m10: db "Case 10", 0
s2e_jmptbl1_m11: db "Case 11", 0
s2e_jmptbl1_m12: db "Case 12", 0
s2e_jmptbl1_m13: db "Case 13", 0
s2e_jmptbl1_m14: db "Case 14", 0
s2e_jmptbl1_m15: db "Case 15", 0
s2e_jmptbl1_m16: db "Case 16", 0

s2e_jmptbl_test:
    ;XXX: Cannot handle symbolic values in always concrete memory
    ;This is why we need to copy the jump table
    ;mov esi, s2e_jmptbl1
    ;mov edi, 0x90000
    ;mov ecx, 7
    ;cld
    ;rep movsd

    call s2e_enable

    call s2e_int
    cmp eax, 16
    ja sje
    jmp [s2e_jmptbl1 + eax*4]

sj0:
    mov esi, s2e_jmptbl1_m0
    jmp sje

sj1:
    mov esi, s2e_jmptbl1_m1
    jmp sje

sj2:
    mov esi, s2e_jmptbl1_m2
    jmp sje

sj3:
    mov esi, s2e_jmptbl1_m3
    jmp sje

sj4:
    mov esi, s2e_jmptbl1_m4
    jmp sje

sj5:
    mov esi, s2e_jmptbl1_m5
    jmp sje

sj6:
    mov esi, s2e_jmptbl1_m6
    jmp sje

sj7:
    mov esi, s2e_jmptbl1_m7
    jmp sje

sj8:
    mov esi, s2e_jmptbl1_m8
    jmp sje

sj9:
    mov esi, s2e_jmptbl1_m9
    jmp sje

sj10:
    mov esi, s2e_jmptbl1_m10
    jmp sje

sj11:
    mov esi, s2e_jmptbl1_m11
    jmp sje

sj12:
    mov esi, s2e_jmptbl1_m12
    jmp sje

sj13:
    mov esi, s2e_jmptbl1_m13
    jmp sje

sj14:
    mov esi, s2e_jmptbl1_m14
    jmp sje

sj15:
    mov esi, s2e_jmptbl1_m15
    jmp sje

sj16:
    mov esi, s2e_jmptbl1_m16
    jmp sje


sje:
    call s2e_disable

    push esi
    push eax
    call s2e_kill_state
    add esp, 8
    ret

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;Testing symbolic ISA devices
s2e_isa_dev:
    call s2e_enable

isadev1:
    mov dx, 0x100
    in  ax, dx
    cmp ax, 0
    ja isadev1

    jmp isadev1
    call s2e_disable
    call s2e_kill_state
    ret

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;Testing complicated symbolic expressions
s2e_simplifier1:
    call s2e_enable
    call s2e_int

    ;Testing chains of Zexts
    mov bl, al
    movzx cx, bl
    movzx edx, cx
    cmp edx, 1
    jae ss1

ss1:

    call s2e_disable
    call s2e_kill_state

    ret



;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;Testing symbolic memory

s2e_symbmem1:
    mov ebx, [0]
    call s2e_enable
    call s2e_int
    cmp dword [eax], 0x1000
    jae sm1
    mov ebx, [eax]
    sm1:
    call s2e_disable
    call s2e_kill_state
ret


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
msg_s2e_int1: db "Called the test handler", 0

s2e_test_int_hdlr:
    push int_msg
    push 0x80
    call s2e_print_expression
    add esp, 8


    ;Access the eflags register
    call s2e_int
    mov edi, [esp + 4]
    test eax, esi

    push 0
    push edi
    call s2e_print_expression
    add esp, 8

    iret

s2e_test_int1:
    ;Register the test interrupt handler
    mov edi, s2e_test_int_hdlr
    mov eax, 0x80
    call add_idt_desc

    ;Starts symbexec
    call s2e_enable
    call s2e_int
    cmp eax, 0
    jz sti_1
    int 0x80
sti_1:
    int 0x80
    call s2e_disable
    call s2e_kill_state
    ret

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;Infinite loop, without state killing
s2e_test2:
    call s2e_enable
    call s2e_fork_enable
s2etest2_1:
    call s2e_int
    cmp eax, 0
    jz __a
__a:
    jmp s2etest2_1

    ret

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;Loop that creates symbolic values on each iteration
s2e_test1:

    call s2e_enable

    mov ecx, 0


a:
    push ecx
    call s2e_int
    pop ecx

    ;;Print the counter
    push eax
    push ecx

    push 0
    push ecx
    call s2e_print_expression
    add esp, 8

    pop ecx
    pop eax

    ;;Print the symbolic value
    push eax
    push ecx

    push 0
    push eax
    call s2e_print_expression
    add esp, 8

    pop ecx
    pop eax

    test eax, 1
    jz exit1    ; if (i < symb) exit
    inc ecx
    jmp a

    call s2e_disable
    call s2e_kill_state

    exit1:
    call s2e_disable
    call s2e_kill_state


    ret


msg_ok: db "SUCCESS", 0
