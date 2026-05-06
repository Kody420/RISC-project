## Benchmark Test Cases

RP2350 core options:
- ARM build target: dual Arm Cortex-M33
- RISC-V build target: dual Hazard3 RISC-V

Assembly notes:
- ARM assembly below was generated with the project release target flags for Cortex-M33.
- RISC-V assembly below was generated with the project release target flags for Hazard3: `-march=rv32ima_zicsr_zifencei_zba_zbb_zbs_zbkb_zca_zcb_zcmp -mabi=ilp32`.

### Test Add-Sub
```c
x += i + 3u;
x -= i ^ 0x5A5A5A5Au;
```

#### ARM Assembly (Cortex-M33)
```asm
bench_test_add_sub:
    adds    r0, r0, #3
    add     r0, r0, r1
    eor     r1, r1, #1515870810
    subs    r0, r0, r1
    bx      lr
```

#### RISC-V Assembly (Hazard3)
```asm
bench_test_add_sub:
    li      a5,1515872256
    addi    a5,a5,-1446
    addi    a0,a0,3
    add     a0,a1,a0
    xor     a1,a1,a5
    sub     a0,a0,a1
    ret
```

- RISC V architektura má O dvě instrukce více

### Test Mul
```c
x = (x * 1664525u) + (i | 1u);
```

#### ARM Assembly (Cortex-M33)
```asm
bench_test_mul:
    ldr     r3, .L4
    orr     r1, r1, #1
    mla     r0, r3, r0, r1
    bx      lr
```

#### RISC-V Assembly (Hazard3)
```asm
bench_test_mul:
    li      a5,1662976
    addi    a5,a5,1549
    mul     a0,a0,a5
    ori     a1,a1,1
    add     a0,a1,a0
    ret
```

### Test XOR-Shift
```c
x ^= x << 13;
x ^= x >> 17;
x ^= x << 5;
```

#### ARM Assembly (Cortex-M33)
```asm
bench_test_xor_shift:
    eor     r0, r0, r0, lsl #13
    eor     r0, r0, r0, lsr #17
    eor     r0, r0, r0, lsl #5
    bx      lr
```

#### RISC-V Assembly (Hazard3)
```asm
bench_test_xor_shift:
    slli    a5,a0,13
    xor     a5,a5,a0
    srli    a0,a5,17
    xor     a0,a0,a5
    slli    a5,a0,5
    xor     a0,a5,a0
    ret
```

### Test Div
```c
uint32_t d = (i | 1u) + ((x >> 24) & 31u);
x += (0xFEDCBA98u / d);
x ^= (0x76543210u % d);
```

#### ARM Assembly (Cortex-M33)
```asm
bench_test_div:
    push    {r4}
    orr     r1, r1, #1
    ldr     r4, .L10
    ubfx    r2, r0, #24, #5
    add     r2, r2, r1
    udiv    r1, r4, r2
    ldr     r3, .L10+4
    mls     r4, r2, r1, r4
    udiv    r3, r3, r2
    add     r0, r0, r3
    eors    r0, r0, r4
    ldr     r4, [sp], #4
    bx      lr
```

#### RISC-V Assembly (Hazard3)
```asm
bench_test_div:
    srli    a4,a0,24
    ori     a1,a1,1
    andi    a4,a4,31
    li      a5,-19087360
    add     a4,a4,a1
    addi    a5,a5,-1384
    li      a3,1985228800
    divu    a5,a5,a4
    addi    a3,a3,528
    remu    a4,a3,a4
    add     a0,a5,a0
    xor     a0,a4,a0
    ret
```

### Test Mix
```c
x += i * 33u;
x ^= x << 7;
x -= 0x9E3779B9u;
x = (x << 11) | (x >> 21);
x *= 3u;
```

#### ARM Assembly (Cortex-M33)
```asm
bench_test_mix:
    add     r1, r1, r1, lsl #5
    add     r1, r1, r0
    ldr     r3, .L13
    eor     r1, r1, r1, lsl #7
    add     r3, r3, r1
    ror     r0, r3, #21
    add     r0, r0, r0, lsl #1
    bx      lr
```

#### RISC-V Assembly (Hazard3)
```asm
bench_test_mix:
    slli    a5,a1,5
    add     a1,a5,a1
    add     a1,a1,a0
    slli    a0,a1,7
    li      a5,1640529920
    xor     a0,a0,a1
    addi    a5,a5,1607
    add     a0,a0,a5
    rori    a0,a0,21
    sh1add  a0,a0,a0
    ret
```

### Test Branch
```c
x = bench_next_rand(&x);
if (x & 1u)
    x += i ^ 0x13579BDFu;
else
    x -= i | 0x2468ACE0u;
```

#### ARM Assembly (Cortex-M33)
```asm
bench_test_branch:
    push    {r4, lr}
    sub     sp, sp, #8
    str     r0, [sp, #4]
    add     r0, sp, #4
    mov     r4, r1
    bl      bench_next_rand
    lsls    r3, r0, #31
    bpl     .L16
    ldr     r3, .L19
    eors    r3, r3, r4
    add     r0, r0, r3
    add     sp, sp, #8
    pop     {r4, pc}
.L16:
    ldr     r3, .L19+4
    orrs    r3, r3, r4
    subs    r0, r0, r3
    add     sp, sp, #8
    pop     {r4, pc}
```

#### RISC-V Assembly (Hazard3)
```asm
bench_test_branch:
    addi    sp,sp,-32
    sw      a0,12(sp)
    addi    a0,sp,12
    sw      a1,8(sp)
    sw      ra,28(sp)
    call    bench_next_rand
    andi    a5,a0,1
    lw      a1,8(sp)
    beq     a5,zero,.L9
    lw      ra,28(sp)
    li      a5,324509696
    addi    a5,a5,-1057
    xor     a1,a1,a5
    add     a0,a1,a0
    addi    sp,sp,32
    jr      ra
.L9:
    lw      ra,28(sp)
    li      a5,610840576
    addi    a5,a5,-800
    or      a1,a1,a5
    sub     a0,a0,a1
    addi    sp,sp,32
    jr      ra
```

### Test Memcpy
```c
g_bench_mem_src[(uint16_t)(i & (BENCH_MEMCPY_SIZE - 1u))] ^= (uint8_t)x;
memcpy(g_bench_mem_dst, g_bench_mem_src, BENCH_MEMCPY_SIZE);
x += g_bench_mem_dst[(uint16_t)(x & (BENCH_MEMCPY_SIZE - 1u))];
```

#### ARM Assembly (Cortex-M33)
```asm
bench_test_memcpy:
    mov     ip, r2
    push    {r4, lr}
    mov     lr, r1
    mov     r4, r0
    mov     r1, ip
    ubfx    r0, lr, #0, #10
    ldrb    ip, [ip, r0]
    mov     r2, #1024
    eor     ip, ip, r4
    strb    ip, [r1, r0]
    mov     r0, r3
    bl      memcpy
    ubfx    r2, r4, #0, #10
    ldrb    r0, [r0, r2]
    add     r0, r0, r4
    pop     {r4, pc}
```

#### RISC-V Assembly (Hazard3)
```asm
bench_test_memcpy:
    addi    sp,sp,-16
    andi    a5,a1,1023
    sw      s0,8(sp)
    sw      ra,12(sp)
    add     a5,a2,a5
    lbu     a4,0(a5)
    mv      s0,a0
    mv      a1,a2
    xor     a4,a4,a0
    sb      a4,0(a5)
    mv      a0,a3
    li      a2,1024
    call    memcpy
    andi    a5,s0,1023
    add     a3,a0,a5
    lbu     a0,0(a3)
    lw      ra,12(sp)
    add     a0,a0,s0
    lw      s0,8(sp)
    addi    sp,sp,16
    jr      ra
```

### Test Lines
```c
uint32_t r0 = bench_next_rand(&x);
uint32_t r1 = bench_next_rand(&x);
dogm128_line((int)(r0 & 127u),
             (int)((r0 >> 8) & 63u),
             (int)(r1 & 127u),
             (int)((r1 >> 8) & 63u),
             (r1 & 0x10000u) ? DISP_COL_BLACK : DISP_COL_WHITE);
```

#### ARM Assembly (Cortex-M33)
```asm
bench_test_lines:
    push    {r4, lr}
    sub     sp, sp, #16
    str     r0, [sp, #12]
    add     r0, sp, #12
    bl      bench_next_rand
    mov     r4, r0
    add     r0, sp, #12
    bl      bench_next_rand
    mov     r2, r0
    ubfx    r3, r2, #16, #1
    eor     r3, r3, #1
    and     r0, r4, #127
    str     r3, [sp]
    ubfx    r1, r4, #8, #6
    ubfx    r3, r2, #8, #6
    and     r2, r2, #127
    bl      dogm128_line
    ldr     r0, [sp, #12]
    add     sp, sp, #16
    pop     {r4, pc}
```

#### RISC-V Assembly (Hazard3)
```asm
bench_test_lines:
    addi    sp,sp,-32
    sw      a0,12(sp)
    addi    a0,sp,12
    sw      ra,28(sp)
    sw      s0,24(sp)
    call    bench_next_rand
    mv      s0,a0
    addi    a0,sp,12
    call    bench_next_rand
    srli    a1,s0,8
    bexti   a4,a0,16
    srli    a3,a0,8
    andi    a2,a0,127
    andi    a1,a1,63
    andi    a0,s0,127
    xori    a4,a4,1
    andi    a3,a3,63
    call    dogm128_line
    lw      ra,28(sp)
    lw      s0,24(sp)
    lw      a0,12(sp)
    addi    sp,sp,32
    jr      ra
```

### Test Raycast
```c
g_bench_player.angle = (fx_t)(g_bench_player.angle + 1);
g_bench_player.dirX = fx_cos(g_bench_player.angle);
g_bench_player.dirY = fx_sin(g_bench_player.angle);
g_bench_player.planeX = fx_mul(g_bench_player.dirY, (fx_t)0x00a9);
g_bench_player.planeY = fx_neg(fx_mul(g_bench_player.dirX, (fx_t)0x00a9));
RenderFrame(&g_bench_player, &Level3Map);
x += (uint32_t)g_bench_player.zBuffer[i % (48 - 1)];
```

#### ARM Assembly (Cortex-M33)
```asm
bench_test_raycast:
    push    {r3, r4, r5, r6, r7, lr}
    mov     r4, r2
    mov     r6, r0
    ldrh    r0, [r2]
    mov     r7, r3
    adds    r2, r0, #1
    sxth    r0, r2
    strh    r0, [r4]
    mov     r5, r1
    bl      fx_cos
    strh    r0, [r4, #2]
    ldrsh   r0, [r4]
    bl      fx_sin
    movs    r1, #169
    strh    r0, [r4, #4]
    bl      fx_mul
    movs    r1, #169
    strh    r0, [r4, #6]
    ldrsh   r0, [r4, #2]
    bl      fx_mul
    bl      fx_neg
    mov     r1, r7
    strh    r0, [r4, #8]
    mov     r0, r4
    bl      RenderFrame
    ldr     r3, .L27
    umull   r2, r3, r3, r5
    lsrs    r3, r3, #5
    add     r2, r3, r3, lsl #1
    rsb     r3, r3, r2, lsl #4
    subs    r5, r5, r3
    add     r4, r4, r5, lsl #1
    ldrsh   r0, [r4, #10]
    add     r0, r0, r6
    pop     {r3, r4, r5, r6, r7, pc}
```

#### RISC-V Assembly (Hazard3)
```asm
bench_test_raycast:
    lhu     a5,0(a2)
    addi    sp,sp,-32
    sw      s1,20(sp)
    sw      s2,16(sp)
    cm.mvsa01 s2,s1
    addi    a0,a5,1
    sext.h  a0,a0
    sh      a0,0(a2)
    sw      ra,28(sp)
    sw      s0,24(sp)
    sw      a3,12(sp)
    mv      s0,a2
    call    fx_cos
    sh      a0,2(s0)
    lh      a0,0(s0)
    call    fx_sin
    li      a1,169
    call    fx_mul
    sh      a0,6(s0)
    lh      a0,2(s0)
    li      a1,169
    call    fx_mul
    call    fx_neg
    lw      a1,12(sp)
    sh      a0,8(s0)
    mv      a0,s0
    call    RenderFrame
    li      a5,-1370734592
    addi    a5,a5,349
    mulhu   a5,s1,a5
    lw      ra,28(sp)
    srli    a5,a5,5
    sh1add  a4,a5,a5
    slli    a4,a4,4
    sub     a5,a4,a5
    sub     s1,s1,a5
    sh1add  s1,s1,s0
    lh      a0,10(s1)
    lw      s0,24(sp)
    lw      s1,20(sp)
    add     a0,a0,s2
    lw      s2,16(sp)
    addi    sp,sp,32
    jr      ra
```

### Test Entities
```c
buttons_t no_buttons = {0};
DrawEntities(&g_bench_player, g_bench_entities, MAX_ENTITIES, dogm_fb, no_buttons, &Level3Map);
x += (uint32_t)g_bench_entities[i % MAX_ENTITIES].distance;
```

#### ARM Assembly (Cortex-M33)
```asm
bench_test_entities:
    push    {r4, r5, r6, r7, lr}
    movs    r7, #0
    mov     r5, r3
    mov     lr, r2
    sub     sp, sp, #28
    ldr     r3, [sp, #52]
    mov     r4, r1
    str     r3, [sp, #8]
    add     ip, sp, #16
    str     r7, [sp, #16]
    strh    r7, [sp, #20]
    str     r7, [sp]
    mov     r6, r0
    ldm     ip, {r0, r1}
    movs    r2, #16
    mov     r0, lr
    strh    r1, [sp, #4]
    ldr     r3, [sp, #48]
    mov     r1, r5
    bl      DrawEntities
    and     r4, r4, #15
    ldr     r0, [r5, r4, lsl #2]
    add     r0, r0, r6
    add     sp, sp, #28
    pop     {r4, r5, r6, r7, pc}
```

#### RISC-V Assembly (Hazard3)
```asm
bench_test_entities:
    addi    sp,sp,-32
    sh      zero,12(sp)
    mv      a6,a5
    lw      a5,12(sp)
    sw      s0,24(sp)
    mv      s0,a3
    sw      s1,20(sp)
    sw      s2,16(sp)
    mv      a3,a4
    cm.mvsa01 s2,s1
    li      a4,0
    mv      a0,a2
    mv      a1,s0
    li      a2,16
    sw      ra,28(sp)
    sw      zero,8(sp)
    call    DrawEntities
    andi    a3,s1,15
    sh2add  a3,a3,s0
    lw      a0,0(a3)
    lw      ra,28(sp)
    lw      s0,24(sp)
    lw      s1,20(sp)
    add     a0,s2,a0
    lw      s2,16(sp)
    addi    sp,sp,32
    jr      ra
```