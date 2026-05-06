## Benchmark Test Cases

### Test Add-Sub
```c
x += i + 3u;
x -= i ^ 0x5A5A5A5Au;
```

### Test Mul
```c
x = (x * 1664525u) + (i | 1u);
```

### Test XOR-Shift
```c
x = bench_next_rand(&x);
```

### Test Div
```c
uint32_t d = (i | 1u) + ((x >> 24) & 31u);
x += (0xFEDCBA98u / d);
x ^= (0x76543210u % d);
```

### Test Mix
```c
x += i * 33u;
x ^= x << 7;
x -= 0x9E3779B9u;
x = (x << 11) | (x >> 21);
x *= 3u;
```

### Test Branch
```c
x = bench_next_rand(&x);
if (x & 1u)
    x += i ^ 0x13579BDFu;
else
    x -= i | 0x2468ACE0u;
```

### Test Memcpy
```c
g_bench_mem_src[(uint16_t)(i & (BENCH_MEMCPY_SIZE - 1u))] ^= (uint8_t)x;
memcpy(g_bench_mem_dst, g_bench_mem_src, BENCH_MEMCPY_SIZE);
x += g_bench_mem_dst[(uint16_t)(x & (BENCH_MEMCPY_SIZE - 1u))];
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

### Test Entities
```c
buttons_t no_buttons = {0};
DrawEntities(&g_bench_player, g_bench_entities, MAX_ENTITIES, dogm_fb, no_buttons, &Level3Map);
x += (uint32_t)g_bench_entities[i % MAX_ENTITIES].distance;
```