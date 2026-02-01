# VICE Emulator CPU Profiler - Technical Documentation

## Overview

VICE includes a **complete CPU profiler implementation** for 6502-based systems. The profiler was written by Oskar Linde and provides detailed cycle-accurate profiling with call graph tracking, memory banking support, and per-instruction analysis.

**Current Status:** Fully implemented and integrated with the VICE monitor.

**Supported CPUs:** 6502 family only (as noted in NEWS: "added profiler support in the monitor, right now supports only 6502")

---

## Architecture

### File Structure

| File | Purpose | Lines |
|------|---------|-------|
| `src/profiler.h` | Public API header | 61 |
| `src/profiler.c` | Core profiler implementation | 379 |
| `src/profiler_data.h` | Data structure definitions | 85 |
| `src/monitor/mon_profile.h` | Monitor command interface header | 42 |
| `src/monitor/mon_profile.c` | Monitor command implementation | 1,142 |

### CPU Integration Points

The profiler hooks into `src/6510core.c` (and `src/6510dtvcore.c` for DTV variant):

```c
// Line 2370: Before each instruction
if (maincpu_profiling) {
    profile_sample_start(reg_pc);
}

// Line 3471: After each instruction
if (maincpu_profiling) {
    profile_sample_finish(CLK - profiling_clock_start, 0 /* stolen_cycles */);
}
```

---

## Core Data Structures

### 1. Per-Instruction Data (`profiler_data.h:40-44`)

```c
typedef struct profiling_data_s {
    profiling_counter_t num_samples;     // How many times instruction was executed
    profiling_counter_t num_cycles:31;   // Total cycles consumed
    profiling_counter_t touched:1;       // Flag for JSR source instructions
} profiling_data_t;
```

### 2. Memory Page (`profiler_data.h:46-48`)

```c
typedef struct profiling_page_s {
    profiling_data_t data[256];  // 256 bytes per page
} profiling_page_t;
```

### 3. Profiling Context (`profiler_data.h:50-70`)

The context represents a function call in the call graph:

```c
typedef struct profiling_context_s {
    profiling_page_t           *page[256];          // Full 64KB address space
    uint16_t                    pc_dst;             // Destination PC (function entry)
    uint16_t                    pc_src;             // Source PC (caller's JSR location)
    uint16_t                    memory_bank_config; // Memory banking state
    profiling_counter_t         num_enters;         // Times function was entered
    profiling_counter_t         num_exits;          // Times function was exited

    // Call tree relationships
    struct profiling_context_s *parent;             // Caller context
    struct profiling_context_s *child;              // First callee context
    struct profiling_context_s *next;               // Sibling (cyclic linked list)
    struct profiling_context_s *next_mem_config;    // Same function, different bank config

    // Cycle accounting
    profiling_counter_t total_stolen_cycles_self;   // DTV stolen cycles
    profiling_counter_t total_cycles;               // Including callees (computed)
    profiling_counter_t total_cycles_self;          // Excluding callees (computed)
    profiling_counter_t total_stolen_cycles;        // Total stolen (computed)

    int id;  // Unique ID for UI reference
} profiling_context_t;
```

### 4. Interrupt Magic Values (`profiler_data.h:32-36`)

Special PC values for interrupt tracking:
```c
enum CallstackMagic {
    NMI   = 0xfffa,  // NMI vector
    RESET = 0xfffc,  // RESET vector
    IRQ   = 0xfffe,  // IRQ vector
};
```

---

## Core API (`profiler.h`)

### Global State
```c
extern bool maincpu_profiling;  // true when profiling is active
```

### Control Functions
```c
void profile_start(void);   // Start profiling, clear old data
void profile_stop(void);    // Stop profiling
void profile_shutdown(void); // Clean up resources
```

### Sample Collection (called by CPU)
```c
void profile_sample_start(uint16_t pc);
void profile_sample_finish(uint16_t cycle_time, uint16_t stolen_cycles);
```

### Call Stack Tracking
```c
void profile_jsr(uint16_t pc_dst, uint16_t pc_src, uint8_t sp);
void profile_int(uint16_t pc_dst, uint16_t handler, uint8_t sp, uint16_t cycle_time);
void profile_rtx(uint8_t sp);  // RTS/RTI
```

---

## Call Stack Implementation (`profiler.c`)

### Stack Data (Lines 49-54)
```c
#define MAX_CALLSTACK_SIZE 129

uint16_t callstack_pc_dst[MAX_CALLSTACK_SIZE];
uint16_t callstack_pc_src[MAX_CALLSTACK_SIZE];
uint8_t  callstack_sp[MAX_CALLSTACK_SIZE];
uint16_t callstack_memory_bank_config[MAX_CALLSTACK_SIZE];
unsigned callstack_size = 0;
```

### Stack Push (JSR/Interrupt)
```c
static void callstack_push(uint16_t pc_dst, uint16_t pc_src, uint8_t sp) {
    if (callstack_size >= MAX_CALLSTACK_SIZE) return;  // Overflow protection

    callstack_pc_dst[callstack_size] = pc_dst;
    callstack_pc_src[callstack_size] = pc_src;
    callstack_sp[callstack_size] = sp;
    callstack_memory_bank_config[callstack_size] = mem_get_current_bank_config();
    callstack_size++;
    context_dirty = true;
}
```

### Stack Pop (RTS/RTI Detection)

The profiler handles "fake" RTS/RTI (indirect jumps via stack manipulation):

```c
static void callstack_pop_check(uint8_t sp) {
    // Only pop if SP reaches or exceeds the stored SP value
    while (callstack_size != 0 && sp >= callstack_sp[callstack_size-1]
           && callstack_sp[callstack_size-1] > 0x01) {
        callstack_size--;
    }
    context_dirty = true;
}
```

---

## Context Tree Management

### Finding/Creating Child Contexts (`profiler.c:183-215`)

```c
static profiling_context_t *get_child_context(profiling_context_t *parent,
                                              uint16_t pc_dst,
                                              uint16_t pc_src,
                                              uint16_t mem_config) {
    // Search existing children (cyclic linked list)
    if (parent->child) {
        profiling_context_t *c = parent->child;
        do {
            if (c->pc_dst == pc_dst && c->pc_src == pc_src &&
                c->memory_bank_config == mem_config) {
                return c;  // Found existing match
            }
            c = c->next;
        } while(c != parent->child);
    }

    // Create new context
    new_context = alloc_profiling_context();
    new_context->pc_dst = pc_dst;
    new_context->pc_src = pc_src;
    new_context->memory_bank_config = mem_config;
    new_context->parent = parent;

    // Insert into cyclic sibling list
    if (!parent->child) {
        parent->child = new_context;
        new_context->next = new_context;  // Self-referential
    } else {
        new_context->next = parent->child->next;
        parent->child->next = new_context;
    }
    return new_context;
}
```

### Memory Bank Configuration Handling

The profiler tracks different memory banking configurations separately:

```c
profiling_context_t *get_mem_config_context(profiling_context_t *main_context,
                                            uint16_t mem_config) {
    if (main_context->memory_bank_config == mem_config) {
        return main_context;
    }

    // Walk linked list to find matching config
    profiling_context_t *c = main_context;
    while (c->next_mem_config) {
        if (c->next_mem_config->memory_bank_config == mem_config) {
            return c->next_mem_config;
        }
        c = c->next_mem_config;
    }

    // Create new config variant
    c->next_mem_config = alloc_profiling_context();
    c->next_mem_config->memory_bank_config = mem_config;
    return c->next_mem_config;
}
```

---

## Aggregate Statistics Computation (`profiler.c:311-356`)

```c
void compute_aggregate_stats(profiling_context_t *context) {
    profiling_counter_t total_child_cycles = 0;
    profiling_counter_t total_self_cycles = 0;

    // Recursively compute for all children
    if (context->child) {
        profiling_context_t *c = context->child;
        do {
            // Mark source instruction of child calls as "touched"
            if (src < 0xfffa) {
                src -= 2;  // Point to JSR instruction
                profiling_get_page(...)->data[src & 0xff].touched = 1;
            }
            compute_aggregate_stats(c);
            total_child_cycles += c->total_cycles;
            c = c->next;
        } while(c != context->child);
    }

    // Sum self cycles from all memory configs
    c = context;
    while (c) {
        for (i = 0; i < 256; i++) {
            if (c->page[i]) {
                for (j = 0; j < 256; j++) {
                    total_self_cycles += c->page[i]->data[j].num_cycles;
                }
            }
        }
        c = c->next_mem_config;
    }

    context->total_cycles_self = total_self_cycles;
    context->total_cycles = total_self_cycles + total_child_cycles;
}
```

---

## Monitor Commands

### Available Commands (via `profile` or `prof`)

| Command | Description |
|---------|-------------|
| `prof on` | Start profiling (clears old data) |
| `prof off` | Stop profiling |
| `prof` | Show current profiling status |
| `prof flat [N]` | Show top N functions by self-time (default: 20) |
| `prof graph [ctx] [depth D]` | Show call graph up to D levels (default: 4) |
| `prof func <addr>` | Show function stats with callers/callees |
| `prof disass <addr>` | Per-instruction profiling with disassembly |
| `prof context <ctx>` | Detailed context info with disassembly |
| `prof clear <addr>` | Clear profiling data for a function |
| `prof export "file.out"` | Export to Callgrind format with pseudo-source |

### Output Examples

#### Flat Profile
```
        Total      %          Self      %
------------- ------ ------------- ------
       45,234  42.3%        12,456  11.6% main_loop
       32,100  30.0%        32,100  30.0% wait_vsync
       ...
```

#### Graph View
```
                        Total      %          Self      %
                  ------------- ------ ------------- ------
   [1] START               89123 100.0%          234   0.3%
     [2] RST -> init       89000  99.9%         1200   1.3%
       [3] 0810 -> game_loop  87800  98.5%         500   0.6%
         ...
```

#### Disassembly View
```
       Cycles      %         Times OPC Branch% Address  Disassembly
------------- ------ ------------- --- ------- -------  -------------------------
        1,234  5.2%           617   2          $0810  LDA #$00
          456  1.9%           456   2          $0812  STA $D020
        2,345  9.8%           782   3   65%    $0815  BNE $0820
          ...
```

### Branch Frequency Calculation (`mon_profile.c:1001-1027`)

The profiler calculates branch prediction frequency using cycle timing:
- Skip branch: 2 cycles
- Take branch (same page): 3 cycles
- Take branch (cross page): 4 cycles

```c
double branch_frequency = (double)num_cycles / num_samples - 2;
if ((dest_addr & 0xff00) != (next_inst & 0xff00)) {
    branch_frequency /= 2;  // Page crossing = 4 cycles when taken
}
```

---

## Context Aliasing and Merging

### Aliased Contexts
When the same function is called from multiple locations or with different memory configs, contexts are marked as "aliased" and shown with `{*}` suffix.

### Compatibility Checking (`mon_profile.c:149-198`)
Before merging contexts with different memory banking configs, the profiler verifies memory contents at executed addresses are identical:

```c
static bool are_aggregates_compatible(profiling_context_t *a,
                                      profiling_context_t *b) {
    // For each executed address, check if memory contents match
    for (k = 0; k < 3; k++) {  // Check opcode + up to 2 operand bytes
        if (mon_get_mem_val_nosfx(mem, a->memory_bank_config, loc + k) !=
            mon_get_mem_val_nosfx(mem, b->memory_bank_config, loc + k)) {
            return false;  // Different code = incompatible
        }
    }
    return true;
}
```

---

## Key Implementation Details

### 1. Cycle Tracking
- Cycles are recorded per-instruction, not per-function
- Total function time = self cycles + all callee cycles
- Stolen cycles tracked separately for DTV variant

### 2. Memory Layout
- 256 pages x 256 bytes = full 64KB address space per context
- Pages allocated lazily on first access
- Separate contexts for different memory banking configurations

### 3. Call Tree Structure
- Parent/child hierarchy for call graph
- Cyclic linked list for siblings (same parent)
- Linear linked list for memory config variants

### 4. Performance Considerations
- Global `maincpu_profiling` flag checked before every instruction
- Context lookup optimized with `context_dirty` flag
- Lazy page allocation minimizes memory usage

---

## Export Functionality

The profiler supports exporting data to Callgrind format with pseudo-source for analysis
with external tools like KCachegrind and Blacksmith.

### Export Command

```
prof export "profile.out"
```

This generates **two files**:
1. `profile.out` - Callgrind format data file
2. `profile.asm` - Pseudo-source assembly file

### Pseudo-Source File

The `.asm` file contains one instruction per line with the format:
```
  line  $ADDR: XX XX XX  DISASSEMBLY
```

Example:
```
     1  $C000: A9 01     LDA #$01
     2  $C002: 8D 20 D0  STA $D020
     3  $C005: 60        RTS
```

This allows Callgrind viewers to display per-instruction costs with actual disassembly.

### Callgrind File

The Callgrind file uses two metrics:
- **Ir** - Instruction executions (sample count)
- **Cy** - CPU cycles

Features:
- References pseudo-source file via `fl=` directive
- Uses line numbers that map to instructions in pseudo-source
- Functions in interrupt context shown as `MainLoop [IRQ]` or `Handler [IRQ>NMI]`
- Full call graph with caller/callee relationships

### Compatible Viewers

- **KCachegrind** (Linux) - `kcachegrind profile.out`
- **QCachegrind** (cross-platform) - `qcachegrind profile.out`
- **Blacksmith** - Advanced Callgrind viewer with source annotation

### Example Workflow

```bash
# In VICE monitor
(C:$e000) prof on
(C:$e000) x
# ... run your program ...
(C:$e000) prof off
(C:$e000) prof export "myprogram.out"

# In terminal
$ kcachegrind myprogram.out
```

The viewer will show:
- Function list with cycle counts
- Per-instruction costs in the pseudo-source view
- Call graph navigation between functions

---

## Limitations

1. **6502 Only** - No support for other CPU types (Z80, etc.)
2. **No Thread Safety** - Single-threaded design
3. **Max Call Stack Depth** - 129 levels before overflow
4. **No Sampling Mode** - Full instrumentation only (some overhead)

---

## Usage Tips

1. **Start Clean**: Use `prof on` which clears previous data
2. **Let It Run**: Execute the code path you want to profile
3. **Stop First**: Use `prof off` before analyzing (optional but cleaner)
4. **Start with Flat**: `prof flat 20` shows hottest functions
5. **Drill Down**: Use `prof func <addr>` to see callers/callees
6. **Instruction Detail**: Use `prof disass <addr>` for cycle-level analysis
7. **Clear Sections**: `prof clear <addr>` to zero out a function for delta measurement

---

## Example Workflow

```
# Start profiling
(C:$e000) prof on
Profiling started.

# Run your program (exit monitor, let it execute)
(C:$e000) x

# (later, re-enter monitor)

# Stop profiling
(C:$e000) prof off
Profiling stopped.

# View hottest functions
(C:$e000) prof flat 10

# Analyze a specific function
(C:$e000) prof func $0800

# Get instruction-level detail
(C:$e000) prof disass $0800
```
