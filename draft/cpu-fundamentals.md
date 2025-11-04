# CPU Fundamentals: How Processors Really Work

A practical explanation of CPU architecture, from basic concepts to x86-64 specifics, including how executable files map to process memory.

## Table of Contents
- [Von Neumann Architecture](#von-neumann-architecture)
- [Memory and Addressing](#memory-and-addressing)
- [Instructions and Execution](#instructions-and-execution)
- [CPU Bitness Explained](#cpu-bitness-explained)
- [x86-64 Architecture](#x86-64-architecture)
- [Canonical Addressing](#canonical-addressing)
- [Protected Mode](#protected-mode)
- [The Stack: Code vs Data](#the-stack-code-vs-data)
- [From ELF to Process Memory](#from-elf-to-process-memory)
- [Key Takeaways](#key-takeaways)

---

## Von Neumann Architecture

The fundamental design that most modern computers follow, named after John von Neumann.

### Core Principle

**Programs and data share the same memory space**

```
┌──────────────────────────────────┐
│         CPU                      │
│  ┌────────────┐  ┌────────────┐  │
│  │  Registers │  │   ALU      │  │
│  │  (R1, IP)  │  │ (Add/Sub)  │  │
│  └────────────┘  └────────────┘  │
└──────┬──────┬──────┬─────────┬───┘
       │      │      │         │
     Address Bus     │Data Bus │
       │3-bit │      │ 8-bit   │
┌──────┴──────┴──────┴─────────┴───────┐
│          Memory (RAM)                │
│  ┌────────────────────────────────┐  │
│  │ 0x0 | mov 5  (instruction)     │  │
│  │ 0x1 | add 6  (instruction)     │  │
│  │ 0x2 | sub 7  (instruction)     │  │
│  │ 0x3 | nop    (instruction)     │  │
│  │ 0x4 | nop                      │  │
│  │ 0x5 | 200    (data)            │  │
│  │ 0x6 | 99     (data)            │  │
│  │ 0x7 | 2      (data)            │  │
│  └────────────────────────────────┘  │
└──────────────────────────────────────┘
```

**Key insight**: Instructions and data are stored together in the same memory. The CPU fetches instructions sequentially (using the Instruction Pointer register), executes them, and they operate on data in the same memory space.

---

## Memory and Addressing

### Simple Memory Example

Consider an 8-byte memory with a 3-bit address bus:

```
Address (3-bit) | Binary Data | Hex Data
─────────────────────────────────────────
000             | 1001 0101   | 0x95
001             | 0011 0110   | 0x36
010             | 1100 0111   | 0xc7
011             | 0000 0000   | 0x00
100             | 0000 0000   | 0x00
101             | 1100 1000   | 0xc8
110             | 0110 0011   | 0x63
111             | 0000 0010   | 0x02
```

### Important Concepts

**Address Bus**: Determines how much memory can be addressed
- 3-bit address bus → 2³ = 8 addresses (8 bytes)
- 32-bit address bus → 2³² = 4 GB
- 48-bit address bus → 2⁴⁸ = 256 TB

**Random Access Memory (RAM)**: Any byte can be accessed directly using its address, without needing to read through previous bytes.

**Question**: If a CPU has a 3-bit address bus, is it a 3-bit CPU?
**Answer**: No! CPU bitness is determined by the size of its **general-purpose registers**, not its address bus width.

---

## Instructions and Execution

### Simple Instruction Set

A minimal CPU instruction set (4 instructions):

```
Instruction | Binary     | Hex  | Description
──────────────────────────────────────────────────────
mov         | 1001 aaaa  | 9 a  | R1 = memory[address]
add         | 0011 aaaa  | 3 a  | R1 = R1 + memory[address]
sub         | 1100 aaaa  | c a  | R1 = R1 - memory[address]
nop         | 0000 xxxx  | 0 x  | No operation
```

### Example Program: 200 + 99 - 2

**Assembly code:**
```
0x0 | mov 5    ; R1 = memory[5] = 200
0x1 | add 6    ; R1 = R1 + memory[6] = 200 + 99
0x2 | sub 7    ; R1 = R1 - memory[7] = 299 - 2
0x3 | nop
0x4 | nop
0x5 | 200      (data)
0x6 | 99       (data)
0x7 | 2        (data)
```

**In memory (hex):**
```
0x0 | 95  (mov 5)
0x1 | 36  (add 6)
0x2 | c7  (sub 7)
0x3 | 00  (nop)
0x4 | 00  (nop)
0x5 | c8  (200 in decimal)
0x6 | 63  (99 in decimal)
0x7 | 02  (2 in decimal)
```

### CPU Registers

**R1 (General Purpose Register)**: Stores data for computations
**IP (Instruction Pointer)**: Points to the next instruction to execute
**SP (Stack Pointer)**: Points to the top of the stack

### Execution Flow

1. IP = 0x0 → Fetch `mov 5` → R1 = 200 → IP++
2. IP = 0x1 → Fetch `add 6` → R1 = 200 + 99 = 299 → IP++
3. IP = 0x2 → Fetch `sub 7` → R1 = 299 - 2 = 297 → IP++
4. IP = 0x3 → Fetch `nop` → Do nothing → IP++
5. Continue...

---

## CPU Bitness Explained

### What Determines CPU Bitness?

**CPU bitness = Size of General Purpose Registers (GPR)**

Not:
- ❌ Address bus width
- ❌ Data bus width
- ❌ SIMD register width

### Examples

**8-bit CPU**: 8-bit registers (Intel 8080, 6502, AVR ATmega328P (Arduino))
**16-bit CPU**: 16-bit registers (Intel 8086, Motorola 68000)
**32-bit CPU**: 32-bit registers (Intel 80386, ARM Cortex-A7)
**64-bit CPU**: 64-bit registers (Intel x86-64, ARM Cortex-A53)

### The "128-bit Console" Myth

**PlayStation 2**: Uses R5900 "Emotion Engine"
- General Purpose Registers: **64-bit**
- SIMD Registers: **128-bit**
- **Verdict**: It's a **64-bit** CPU, not 128-bit

**Sega Dreamcast**: Uses SH-4 RISC
- General Purpose Registers: **32-bit**
- FPU SIMD: **128-bit**
- **Verdict**: It's a **32-bit** CPU, not 128-bit

**By this logic**: Intel Core i9 would be a "512-bit CPU" because of its 512-bit AVX-512 SIMD registers. But it's actually a **64-bit** CPU.

---

## x86-64 Architecture

### Register Layout

**General Purpose Registers (64-bit):**
```
RAX, RBX, RCX, RDX        (Accumulator, Base, Counter, Data)
RSI, RDI                  (Source Index, Destination Index)
RBP, RSP                  (Base Pointer, Stack Pointer)
R8, R9, R10, ..., R15     (Additional registers)
```

**Special Registers:**
```
RIP                       (Instruction Pointer)
RFLAGS                    (Status flags)
```

**Control Registers:**
```
CR0, CR2, CR3, CR4, CR8   (System control)
```

### Simple x86-64 Program

Calculate: 200 + 99 - 2

```nasm
mov eax, 200     ; eax = 200
mov ebx, 99      ; ebx = 99
add eax, ebx     ; eax = eax + ebx = 299
sub eax, 2       ; eax = eax - 2 = 297
```

### 64-bit Advantage: Large Numbers

**C code:**
```c
long long a = 10, b = 20;
long long c;

int main() {
    c = a + b;
    return 0;
}
```

**x86-64 Assembly (simple):**
```nasm
mov rdx, qword [a]    ; rdx = a (64-bit in one register)
mov rax, qword [b]    ; rax = b (64-bit in one register)
add rax, rdx          ; rax = rax + rdx
mov qword [c], rax    ; c = rax
```

**x86 Assembly (complex, needs 2 registers per value):**
```nasm
; long long a is split across edi:esi
mov esi, dword [a]        ; esi = lower 32 bits of a
mov edi, dword [a + 4]    ; edi = upper 32 bits of a

; long long b is split across ebx:ecx
mov ecx, dword [b]        ; ecx = lower 32 bits of b
mov ebx, dword [b + 4]    ; ebx = upper 32 bits of b

; Add with carry
add ecx, esi              ; ecx = lower a + lower b
adc ebx, edi              ; ebx = upper a + upper b + carry

; Store result
mov dword [c], ecx        ; Store lower 32 bits
mov dword [c + 4], ebx    ; Store upper 32 bits
```

**Why 64-bit is better**: Single instruction vs. multiple instructions with carry handling.

---

## Canonical Addressing

### The Problem

x86-64 has:
- **64-bit registers** (can store 64-bit pointers)
- **48-bit address bus** (can only address 256 TB, not 16 EB)

**Question**: How do we store 48-bit addresses in 64-bit pointers?

### The Solution: Canonical Form

**Canonical address**: The upper 16 bits must be copies of bit 47

```
64-bit pointer:
┌──────────────┬────────────────────────────────────────────────┐
│  Sign Extend │            48-bit Address                      │
│  (16 bits)   │            (bits 0-47)                         │
└──────────────┴────────────────────────────────────────────────┘
    ↑
    └─ Must be all 0s or all 1s (copy of bit 47)
```

### Valid Address Ranges

**User space** (bit 47 = 0):
```
0x0000_0000_0000_0000  to  0x0000_7FFF_FFFF_FFFF
```

**Kernel space** (bit 47 = 1):
```
0xFFFF_8000_0000_0000  to  0xFFFF_FFFF_FFFF_FFFF
```

**Invalid (non-canonical)** - Middle range:
```
0x0000_8000_0000_0000  to  0xFFFF_7FFF_FFFF_FFFF
```

### Why This Matters

- Accessing a non-canonical address causes a **General Protection Fault**
- Creates a natural split between user space (low addresses) and kernel space (high addresses)
- Allows future expansion to 56-bit or 64-bit addressing without breaking existing software

### Simple Analogy

Imagine you have a 4-bit address but only want to use 3 bits:

```
Valid addresses (canonical):
0000  →  000  (top bit duplicated)
0001  →  001
0010  →  010
0011  →  011
1100  →  100  (top bit duplicated)
1101  →  101
1110  →  110
1111  →  111

Invalid addresses (non-canonical):
0100, 0101, 0110, 0111  (top 2 bits not the same)
1000, 1001, 1010, 1011  (top 2 bits not the same)
```

Address space: 16 bytes reduced to 8 usable bytes.

---

## Protected Mode

Modern CPUs don't just execute instructions blindly. They enforce security through **protected mode**.

### Protection Mechanisms

**1. Ring Levels (Privilege Levels)**
```
Ring 0 (Kernel)      - Full access to everything
Ring 1 (Drivers)     - Limited access (rarely used)
Ring 2 (Drivers)     - Limited access (rarely used)
Ring 3 (User apps)   - Restricted access
```

**2. Page Tables**
- Control which memory addresses are accessible
- Define read/write/execute permissions
- Enable virtual memory

**3. Interrupts**
- Hardware can signal the CPU
- CPU can switch from user code to kernel code
- Enables preemptive multitasking

**4. User Space / Kernel Space Separation**
- User programs run in Ring 3 (unprivileged)
- Kernel runs in Ring 0 (privileged)
- Syscalls allow controlled transitions between rings

### Key Control Registers (x86-64)

**CR0**: Enables protected mode, paging
**CR2**: Stores address that caused page fault
**CR3**: Points to page table base
**CR4**: Enables various CPU features
**CR8**: Task priority level

---

## The Stack: Code vs Data

One of the most important concepts to understand: **the function code lives in `.text`, but the local variables live far away in the stack segment**.

### The Key Distinction

When you write this in C:

```c
int foo(int a) {
    int local = 1;
    return local + a;
}
```

**Where things actually are in memory:**

```
Process Memory Layout:

High Addresses
┌────────────────────────────┐
│         Stack              │  ← local variables ARE HERE
│                            │     (local, a, return address)
│         ...                │
│         (far away)         │
│         ...                │
├────────────────────────────┤
│         .text              │  ← function code IS HERE
│    foo:                    │     (instructions: push, mov, sub, add)
│      push ebp              │
│      mov ebp, esp          │
│      sub esp, 4            │
│      ...                   │
└────────────────────────────┘
Low Addresses
```

**The crucial insight**:
- The **instructions** that manipulate `local` and `a` are in `.text` (low addresses)
- The **actual data** for `local` and `a` is in the stack (high addresses)
- These could be **megabytes or gigabytes apart** in memory!

### The Stack Pointer (SP) Register

**RSP (64-bit) / ESP (32-bit) / SP (16-bit)**: Always points to the **top of the stack**

```
Stack grows DOWN (from high to low addresses):

Before function call:
                           RSP
                            ↓
0x7FFFFFFFE000  ┌───────────┬───────────┐
                │  (empty)  │           │
                │           │           │
                │           │           │

After pushing data:
                                   RSP
                                    ↓
0x7FFFFFFFE000  ┌───────────┬───────┬───┐
                │   data    │  data │   │
                │           │       │   │
                            ← pushed here
```

**Key principle**: Stack pointer moves **down** as you allocate, **up** as you deallocate.

### Stack Operations: Allocation and Deallocation

#### Example Function

```c
int calculate(int x, int y) {
    int result;
    int temp = 10;
    result = x + y + temp;
    return result;
}
```

#### Assembly Breakdown (x86-64)

```nasm
calculate:
    ; === FUNCTION PROLOGUE (Setup) ===
    push rbp                    ; Save old base pointer
    mov rbp, rsp                ; Set new base pointer

    ; === LOCAL VARIABLE ALLOCATION ===
    sub rsp, 16                 ; Allocate 16 bytes for locals
                                ; (result: 4 bytes, temp: 4 bytes, + padding)

    ; === FUNCTION BODY ===
    mov DWORD PTR [rbp-4], 10   ; temp = 10
    mov eax, DWORD PTR [rbp+8]  ; eax = x (from stack)
    add eax, DWORD PTR [rbp+16] ; eax = x + y
    add eax, DWORD PTR [rbp-4]  ; eax = x + y + temp
    mov DWORD PTR [rbp-8], eax  ; result = eax

    ; === FUNCTION EPILOGUE (Cleanup) ===
    mov rsp, rbp                ; Deallocate locals (restore RSP)
    pop rbp                     ; Restore old base pointer
    ret                         ; Return to caller
```

### Stack Frame Visualization

Let's trace what happens step-by-step when `calculate(5, 3)` is called:

#### Before Function Call

```
Caller's code (in .text):
    push 3              ; Argument y
    push 5              ; Argument x
    call calculate      ; Push return address & jump

Stack (grows downward):
Higher addresses
    ┌──────────────┬──────────────┬─────┐
    │      3       │      5       │ ret │
    │  (arg y)     │  (arg x)     │ addr│
    └──────────────┴──────────────┴─────┘
    ↑              ↑              ↑
  (future         (future        RSP
   rbp+16)         rbp+8)      (future rbp)
Lower addresses
```

#### After Prologue (`push rbp`, `mov rbp, rsp`)

```
Higher addresses
    ┌──────────────┬──────────────┬─────┬────────┐
    │      3       │      5       │ ret │ old rbp│
    │  (arg y)     │  (arg x)     │ addr│        │
    └──────────────┴──────────────┴─────┴────────┘
    ↑              ↑              ↑               ↑
    rbp+24         rbp+16         rbp+8      RSP, RBP
Lower addresses

#### After `sub rsp, 16` (Allocate Locals)

```
Higher addresses
    ┌──────────────┬──────────────┬─────┬────────┬──────┬──────┐
    │      3       │      5       │ ret │ old rbp│result│ temp │
    │  (arg y)     │  (arg x)     │ addr│        │  ?   │  10  │
    └──────────────┴──────────────┴─────┴────────┴──────┴──────┘
    ↑              ↑              ↑               ↑      ↑      ↑
    rbp+24         rbp+16         rbp+8           rbp    rbp-4  rbp-8
                                                                 ↓
                                                                RSP
Lower addresses
```

#### After Epilogue (`mov rsp, rbp`, `pop rbp`)

```
                                         RSP
                                          ↓
    ┌──────────────┬──────────────┬──────┐
    │      3       │      5       │ ret  │
    │  (arg y)     │  (arg x)     │ addr │
    └──────────────┴──────────────┴──────┘

    Locals are DEALLOCATED (just moved RSP back up)
    The data is still there, but considered "garbage"
```

### How Allocation/Deallocation Works

#### Allocation (Growing the Stack)

**Method 1: Subtract from RSP**
```nasm
sub rsp, 16         ; Allocate 16 bytes
                    ; RSP moves DOWN (lower address)
```

**Method 2: Push values**
```nasm
push rax            ; Allocate 8 bytes, store rax
                    ; RSP moves DOWN by 8
```

#### Deallocation (Shrinking the Stack)

**Method 1: Add to RSP**
```nasm
add rsp, 16         ; Deallocate 16 bytes
                    ; RSP moves UP (higher address)
```

**Method 2: Move base pointer**
```nasm
mov rsp, rbp        ; Restore RSP to old position
                    ; Deallocates everything below RBP
```

**Method 3: Pop values**
```nasm
pop rax             ; Read value into rax, deallocate 8 bytes
                    ; RSP moves UP by 8
```

### Important: Deallocation is Just Moving the Pointer

**The data is NOT erased!**

```
After deallocating:
                  RSP (after deallocation)
                   ↓
    ┌──────┬──────┬──────────────┬─────┐
    │  10  │  8   │      3       │ ... │
    │(garbage)    │              │     │
    └──────┴──────┴──────────────┴─────┘
         ↑
    Old data is still there!
    But it's considered "free space"
    Next function call will overwrite it
```

This is why:
- Local variables have **undefined values** if not initialized
- Stack overflow can expose data from previous function calls
- Security vulnerabilities can arise from reading uninitialized stack data

### Complete Example: Nested Function Calls

```c
int add(int a, int b) {
    int sum = a + b;
    return sum;
}

int calculate(int x) {
    int result;
    result = add(x, 10);
    return result;
}

int main() {
    int value = calculate(5);
    return value;
}
```

#### Stack Evolution

**Step 1: main() starts**
```
                                    RSP, RBP
                                        ↓
    ┌──────────────────────────────────┐
    │  (main's stack frame)            │
    │  value: ?                        │
    └──────────────────────────────────┘
```

**Step 2: main() calls calculate(5)**
```
    Caller pushes: argument (5), return address

Higher addresses
    ┌────────────────┬─────┬──────┬─────┐
    │  main's frame  │  5  │ ret  │rbp  │...allocated later
    │                │     │ addr │save │   (result)
    └────────────────┴─────┴──────┴─────┘
    ↑                ↑     ↑             ↑
    (main's data)   rbp+16 rbp+8   RSP, RBP  rbp-4
                           (calculate's frame starts)
Lower addresses
```

**Step 3: calculate() calls add(x, 10)**
```
Higher addresses
    ┌────────┬─────┬──────┬─────┬──────┬──────┬─────┬──────┬─────┐
    │ main's │  5  │ ret  │ old │result│  10  │  5  │ ret  │ old │...locals
    │ frame  │     │ addr │ rbp │  ?   │      │     │ addr │ rbp │   (sum)
    └────────┴─────┴──────┴─────┴──────┴──────┴─────┴──────┴─────┘
    ↑        ↑                          ↑      ↑            ↑      ↑
    (main)   (calculate's args)      (calc's  rbp+16      rbp+8   RSP  rbp-4
                                      local)          (add's frame)   , RBP
Lower addresses
```

**Step 4: add() returns, its frame is deallocated**
```
Higher addresses
    ┌────────────────┬─────┬──────┬─────┬──────┐
    │  main's frame  │  5  │ ret  │ old │result│
    │                │     │ addr │ rbp │  15  │
    └────────────────┴─────┴──────┴─────┴──────┘
    ↑                ↑     ↑            ↑RSP    ↑
    (main's data)   rbp+16 rbp+8        ,RBP   rbp-4
                          (calculate continues)
Lower addresses
    (calculate continues, result = 15)
```

**Step 5: calculate() returns, main resumes**
```
                                    RSP
                                     ↓
    ┌──────────────────────────────┐
    │  main's frame                │
    │  value: 15                   │
    └──────────────────────────────┘
```

### The Separation: Code vs Data

Let's be crystal clear about what's where:

**In .text (Code Segment):**
```nasm
; These INSTRUCTIONS are in .text at LOW addresses
; Example addresses: 0x00400000 - 0x00401000

add:
    0x00400500:  push rbp           ; Instruction bytes: 55
    0x00400501:  mov rbp, rsp       ; Instruction bytes: 48 89 e5
    0x00400504:  mov [rbp-4], edi   ; Instruction bytes: 89 7d fc
    0x00400507:  mov [rbp-8], esi   ; ...
    0x0040050a:  mov eax, [rbp-4]
    0x0040050d:  add eax, [rbp-8]
    0x00400510:  pop rbp
    0x00400511:  ret

calculate:
    0x00400520:  push rbp
    0x00400521:  mov rbp, rsp
    ...
```

**In Stack (Data Segment):**
```
; The ACTUAL DATA is in the stack at HIGH addresses
; Example addresses: 0x7FFFFFFFE000 - 0x7FFFFFFFF000

0x7FFFFFFFE000:  sum = 15       (add's local variable)
0x7FFFFFFFE004:  return addr
0x7FFFFFFFE008:  b = 10         (add's argument)
0x7FFFFFFFE00C:  a = 5          (add's argument)
0x7FFFFFFFE010:  result = ?     (calculate's local variable)
0x7FFFFFFFE014:  return addr
0x7FFFFFFFE018:  x = 5          (calculate's argument)
```

**When CPU executes** `mov eax, [rbp-4]`:
1. The **instruction** `mov eax, [rbp-4]` is at `0x0040050a` (in .text)
2. The **data** being read is at `0x7FFFFFFFE000` (in stack)
3. These are **billions of bytes apart** in the address space!

### Why This Matters

**1. Security**: Code and data are separate
- `.text` is marked read-only and executable
- Stack is marked read-write but **not executable**
- Prevents executing injected data (stack-based exploits)

**2. Understanding pointers**:
```c
int local = 5;
printf("Address of local: %p\n", &local);
// Prints: 0x7FFFFFFFE000 (high address, in stack)

printf("Address of main: %p\n", &main);
// Prints: 0x00400580 (low address, in .text)
```

**3. Debugging**:
- When you see `[rbp-4]`, you're accessing **stack data**
- When you see `call 0x00400500`, you're jumping to **code in .text**
- Understanding this helps reading assembly and debugging

### Stack Pointer Rules Summary

**RSP/ESP always points to the top of the stack**

- **Push** (allocate): `sub rsp, N` or `push value` → RSP moves DOWN
- **Pop** (deallocate): `add rsp, N` or `pop register` → RSP moves UP
- **Local variables**: Allocated by subtracting from RSP
- **Deallocation**: Just restore RSP (data not erased, just "abandoned")
- **Stack frames**: Each function has its own frame between RBP and RSP

**The stack is automatic**:
- Allocate: `sub rsp, 16` (instant)
- Deallocate: `mov rsp, rbp` (instant)
- No need to track what's allocated (unlike heap with `malloc`/`free`)

---

## From ELF to Process Memory

### Executable and Linkable Format (ELF)

An ELF file contains:

```
┌─────────────────────────────┐
│  ELF Header                 │  ← Magic bytes, architecture, entry point
├─────────────────────────────┤
│  Program Headers            │  ← Segments (how to load into memory)
│  (Segment Table)            │
├─────────────────────────────┤
│  .text section              │  ← Machine code (executable instructions)
├─────────────────────────────┤
│  .rodata section            │  ← Read-only data (string literals)
├─────────────────────────────┤
│  .data section              │  ← Initialized global variables
├─────────────────────────────┤
│  .bss section               │  ← Uninitialized global variables
├─────────────────────────────┤
│  .symtab                    │  ← Symbol table (for debugging)
├─────────────────────────────┤
│  .strtab                    │  ← String table
├─────────────────────────────┤
│  Section Headers            │  ← Section metadata
└─────────────────────────────┘
```

**Segments vs Sections:**
- **Sections**: Fine-grained division for linking/debugging (`.text`, `.data`, `.bss`)
- **Segments**: Coarse-grained division for loading (one segment may contain multiple sections)

### Linux Process Memory Layout

When an ELF file is loaded, it becomes a process with this memory layout:

```
High Addresses (0xFFFFFFFF...)
┌────────────────────────────┐
│       Kernel Space         │  ← Ring 0, OS kernel
│      (not accessible)      │
├────────────────────────────┤  ← 0xC0000000 (32-bit) / 0x00007FFFFFFFFFFF (64-bit)
│          Stack             │  ← Local variables, function calls
│            ↓               │     Grows downward
│                            │
│      (unused space)        │
│                            │
│            ↑               │
│          Heap              │  ← malloc(), dynamic memory
│                            │     Grows upward
├────────────────────────────┤
│          .bss              │  ← Uninitialized globals (zeroed)
├────────────────────────────┤
│          .data             │  ← Initialized global variables
├────────────────────────────┤
│        .rodata             │  ← Read-only data (strings, constants)
├────────────────────────────┤
│          .text             │  ← Program code (instructions)
├────────────────────────────┤
│        (reserved)          │  ← NULL pointer guard
└────────────────────────────┘  ← 0x00000000
Low Addresses
```

### Mapping C Code to Memory

**C source code:**
```c
int global = 5;                    // .data
int arr[] = {1, 2, 3, 4, 5};       // .data

int uninitialized_global;          // .bss
int uninitialized_arr[1024];       // .bss

int foo(int a)                     // .text (code)
{
    int local = 1;                 // stack
    return local + a;              // stack
}

int main()                         // .text (code)
{
    uninitialized_global = 10;
    uninitialized_arr[0] = 1;
    foo(global);
}
```

**Assembly code:**
```nasm
.text                                    ; Code section
.globl main

main:
    push ebp                             ; Save base pointer (stack)
    mov ebp, esp                         ; Set up stack frame
    mov DWORD PTR uninitialized_global, 10  ; Write to .bss
    mov eax, 1
    mov DWORD PTR [uninitialized_arr], eax  ; Write to .bss
    push DWORD PTR global                ; Push from .data (stack)
    call foo                             ; Call function
    mov esp, ebp                         ; Clean up stack
    pop ebp                              ; Restore base pointer
    ret                                  ; Return

foo:
    push ebp                             ; Save base pointer (stack)
    mov ebp, esp                         ; Set up stack frame
    mov eax, DWORD PTR [ebp + 8]         ; Read argument from stack
    sub esp, 4                           ; Allocate local variable (stack)
    mov DWORD PTR [esp], 1               ; local = 1 (stack)
    add eax, DWORD PTR [esp]             ; eax = local + a
    mov esp, ebp                         ; Clean up stack
    pop ebp                              ; Restore base pointer
    ret                                  ; Return

.bss                                     ; Uninitialized data section
    uninitialized_global: .resb 4        ; Reserve 4 bytes
    uninitialized_arr: .resb 4096        ; Reserve 4096 bytes

.data                                    ; Initialized data section
    global: .int 5                       ; 4 bytes, value = 5
    arr: .int 1, 2, 3, 4, 5              ; 20 bytes
```

### Memory Region Purposes

| Region    | Purpose                          | Lifetime         | Growth   |
|-----------|----------------------------------|------------------|----------|
| `.text`   | Executable instructions          | Program lifetime | Fixed    |
| `.rodata` | String literals, constants       | Program lifetime | Fixed    |
| `.data`   | Initialized global variables     | Program lifetime | Fixed    |
| `.bss`    | Uninitialized globals (zeroed)   | Program lifetime | Fixed    |
| `heap`    | Dynamic allocations (`malloc`)   | Until `free()`   | Grows up |
| `stack`   | Local vars, function calls       | Until return     | Grows down|

---

## Key Takeaways

### Core Concepts

1. **Von Neumann Architecture**: Instructions and data share the same memory
2. **CPU Bitness**: Determined by general-purpose register width, not address bus
3. **Memory Addressing**: Address bus width determines maximum addressable memory
4. **Instruction Sets**: Unique to each CPU architecture (x86, ARM, RISC-V, etc.)

### x86-64 Specifics

5. **64-bit registers** but **48-bit address bus** (256 TB addressable)
6. **Canonical addressing**: Upper 16 bits of pointers must extend bit 47
7. **Protected mode**: Ring levels, page tables, and privilege separation
8. **Control registers**: CR0, CR3, etc. control CPU security features

### Execution Model

9. **CPU runs code directly**, not the OS (OS just loads the program)
10. **No OS required**: CPUs can execute code in bare-metal environments
11. **Executable files (ELF)** contain both code and data sections
12. **Process memory layout**: Stack, heap, data, BSS, text regions

### Important Realizations

13. **Addresses in disassembly**: Could be virtual addresses (after loading) or file offsets (in ELF file)
14. **Stack grows down**, **heap grows up**: Prevents immediate collision
15. **`.bss` is zeroed**: Uninitialized globals start at 0
16. **Security is hardware-enforced**: Page tables and ring levels are CPU features

### The Big Picture

The CPU is a simple machine that:
- Fetches instructions from memory (using IP/RIP)
- Decodes them (what operation?)
- Executes them (ALU performs calculation)
- Stores results (back to registers or memory)
- Repeats (IP++, fetch next instruction)

Everything else—operating systems, processes, virtual memory, protection—is built on top of this basic fetch-decode-execute cycle by cleverly using CPU features like page tables, privilege levels, and interrupts.

The OS doesn't "run" your program. The OS loads your program into memory, sets up page tables, and tells the CPU "start executing at this address." From that point on, the **CPU is directly executing your code**.
