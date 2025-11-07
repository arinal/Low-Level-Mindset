# Hardware Memory Protection Mechanisms

This document explains how modern CPUs and hardware enforce memory protection, from basic virtual memory to advanced security features like ARM TrustZone and Intel SGX. Understanding these mechanisms is essential for comprehending how systems like Widevine DRM protect content in memory.

## Table of Contents

- [Overview](#overview)
- [1. Virtual Memory and MMU](#1-virtual-memory-and-mmu)
- [2. CPU Privilege Levels](#2-cpu-privilege-levels)
- [3. ARM TrustZone: Two-World Architecture](#3-arm-trustzone-two-world-architecture)
- [4. Intel SGX: Encrypted Enclaves](#4-intel-sgx-encrypted-enclaves)
- [5. IOMMU: Device Memory Protection](#5-iommu-device-memory-protection)
- [6. Complete Example: Widevine L1 Protection](#6-complete-example-widevine-l1-protection)
- [7. Linux Kernel Implementation](#7-linux-kernel-implementation)
- [8. Attack Scenarios and Defenses](#8-attack-scenarios-and-defenses)
- [Key Takeaways](#key-takeaways)
- [Quick Reference](#quick-reference)

## Overview

Memory protection is implemented through **hardware mechanisms** enforced by the CPU and related components. These protections prevent:

- User processes from accessing kernel memory
- One process from accessing another process's memory
- Any process from accessing specially protected regions (e.g., DRM content)
- Malicious devices from reading/writing arbitrary memory via DMA

**Key principle**: Protection is enforced in **hardware**, not software. Software (OS kernel) configures the protection rules, but the hardware physically blocks unauthorized access before it reaches memory.

### Protection Layers

```
┌─────────────────────────────────────────────┐
│  Software Layer (OS configures rules)       │
│  - Page table setup                         │
│  - IOMMU configuration                      │
│  - TrustZone/SGX management                 │
└────────────────┬────────────────────────────┘
                 ↓
┌─────────────────────────────────────────────┐
│  Hardware Layer (Enforces protection)       │
│  - MMU (Memory Management Unit)             │
│  - CPU privilege level checks               │
│  - TrustZone NS bit enforcement             │
│  - SGX Memory Encryption Engine             │
│  - IOMMU translation and blocking           │
└─────────────────────────────────────────────┘
```

## 1. Virtual Memory and MMU

Every modern CPU has an **MMU (Memory Management Unit)** that translates virtual addresses to physical addresses using **page tables** with permission bits.

### Basic Translation Flow

```
User Program:
    char *ptr = (char *)0x00401000;  // Virtual address
    char value = *ptr;
        ↓
    [CPU sends 0x00401000 to MMU]
        ↓
    MMU walks page tables:
        PML4 → PDPT → PD → PT
        ↓
    Finds Page Table Entry (PTE):
        - Physical address: 0x87654000
        - Permissions: Present, User, Read-only
        ↓
    MMU checks permissions:
        - Current mode: User mode ✓
        - Access type: Read ✓
        - Permission: Read-only ✓
        ↓
    Access granted: Read from physical 0x87654000
```

### x86-64 Page Table Entry Structure

A 64-bit page table entry contains both the physical address and permission flags:

```
Bit 63                                                    Bit 0
┌────┬────┬──────────────────────────────────────┬────────────┐
│ NX │... │  Physical Address (bits 51-12)       │   Flags    │
└────┴────┴──────────────────────────────────────┴────────────┘

Flags (bits 0-11):
  0: P   - Present (1 = page in memory, 0 = page fault)
  1: R/W - Read/Write (1 = writable, 0 = read-only)
  2: U/S - User/Supervisor (1 = user accessible, 0 = kernel only)
  3: PWT - Page Write-Through (cache control)
  4: PCD - Page Cache Disable
  5: A   - Accessed (set by CPU on access)
  6: D   - Dirty (set by CPU on write)
  7: PS  - Page Size (4KB vs larger pages)
  8: G   - Global (don't flush from TLB)
 63: NX  - No eXecute (1 = cannot execute code from this page)
```

### Hardware Permission Enforcement

The MMU performs these checks **in hardware** before every memory access:

```c
// Pseudo-code representing MMU hardware logic
bool mmu_check_access(virtual_address, access_type) {
    pte = walk_page_table(virtual_address);

    // Check 1: Page present?
    if (!pte.present) {
        trigger_page_fault(PAGE_NOT_PRESENT);
        return false;
    }

    // Check 2: Privilege level
    if (pte.user_supervisor == SUPERVISOR_ONLY) {
        if (cpu.current_privilege == USER_MODE) {
            trigger_page_fault(PROTECTION_VIOLATION);
            return false;
        }
    }

    // Check 3: Read permission
    if (access_type == READ) {
        // Always allowed if page is present and privilege OK
    }

    // Check 4: Write permission
    if (access_type == WRITE) {
        if (!pte.read_write) {
            trigger_page_fault(WRITE_PROTECTION_VIOLATION);
            return false;
        }
    }

    // Check 5: Execute permission
    if (access_type == EXECUTE) {
        if (pte.no_execute) {
            trigger_page_fault(EXECUTION_VIOLATION);
            return false;
        }
    }

    // All checks passed
    physical_address = pte.physical_address | (virtual_address & 0xFFF);
    return true;
}
```

### ARM64 Page Table Entry

ARM uses a similar structure with different flag names:

```
Bit 63                                                    Bit 0
┌────┬────┬──────────────────────────────────────┬────────────┐
│UXN │PXN │  Physical Address (bits 47-12)       │   Attrs    │
└────┴────┴──────────────────────────────────────┴────────────┘

Key attributes:
  0: Valid     - Page present
  6: AP[1]     - Access Permission (Read/Write)
  7: AP[0]     - Access Permission (EL0 accessible)
  5: NS        - Non-Secure (TrustZone)
 53: PXN       - Privileged eXecute Never
 54: UXN       - User eXecute Never
```

### Example: Kernel Memory Protection

```c
// User process tries to access kernel memory
void user_function() {
    char *kernel_ptr = (char *)0xffffffff81000000;  // Kernel virtual address
    char value = *kernel_ptr;  // What happens?
}
```

**Hardware enforcement:**

```
1. CPU MMU receives access request for 0xffffffff81000000
2. MMU walks page tables
3. Finds PTE with U/S bit = 0 (Supervisor only)
4. CPU is in Ring 3 (User mode)
5. MMU blocks access BEFORE reaching memory
6. CPU triggers #PF (Page Fault) exception
7. Exception handler in kernel logs "Segmentation fault"
8. Process terminated
```

The process **never** sees kernel memory - the CPU blocks it in hardware.

### Linux Kernel Source: MMU Fault Handling

```c
// arch/x86/mm/fault.c
dotraplinkage void do_page_fault(struct pt_regs *regs, unsigned long error_code)
{
    unsigned long address = read_cr2();  // CR2 has faulting address

    // error_code bits tell us what happened:
    // bit 0: 0 = page not present, 1 = protection violation
    // bit 1: 0 = read, 1 = write
    // bit 2: 0 = supervisor, 1 = user
    // bit 3: 1 = reserved bit violation
    // bit 4: 1 = instruction fetch

    if (error_code & X86_PF_PROT) {
        // Protection violation - page exists but access denied
        // This is where kernel memory access from user space is caught
    }

    // Handle the fault (could be valid, like copy-on-write)
    handle_mm_fault(vma, address, flags);
}
```

**Location**: `refs/linux/arch/x86/mm/fault.c`

## 2. CPU Privilege Levels

CPUs have multiple **privilege levels** (rings/exception levels) that control what instructions can be executed and what memory can be accessed.

### x86 Ring Model

```
┌──────────────────────────────────────────┐
│  Ring 0 - Kernel Mode                    │
│  - Full hardware access                  │
│  - Can modify page tables                │
│  - Can execute privileged instructions   │
│  - Can access all memory                 │
│  - Linux kernel runs here                │
├──────────────────────────────────────────┤
│  Ring 1 - Rarely Used                    │
│  - Originally for device drivers         │
│  - Modern systems skip this              │
├──────────────────────────────────────────┤
│  Ring 2 - Rarely Used                    │
│  - Original intent unclear               │
│  - Modern systems skip this              │
├──────────────────────────────────────────┤
│  Ring 3 - User Mode                      │
│  - Restricted access                     │
│  - Cannot execute privileged instructions│
│  - Limited memory access via page tables │
│  - All user applications run here        │
└──────────────────────────────────────────┘
```

### ARM Exception Levels

ARM uses a numbered hierarchy (higher = more privileged):

```
┌──────────────────────────────────────────┐
│  EL3 - Secure Monitor                    │
│  - Highest privilege                     │
│  - Controls switching between worlds     │
│  - Manages TrustZone                     │
│  - Firmware/Boot code                    │
├──────────────────────────────────────────┤
│  EL2 - Hypervisor                        │
│  - Virtual machine monitor               │
│  - KVM/Xen run here                      │
│  - Controls EL1/EL0 VMs                  │
├──────────────────────────────────────────┤
│  EL1 - Kernel                            │
│  - Operating system kernel               │
│  - Linux kernel runs here                │
│  - Can access EL0 memory                 │
├──────────────────────────────────────────┤
│  EL0 - User                              │
│  - User applications                     │
│  - Restricted access                     │
│  - Browser, media player, etc.           │
└──────────────────────────────────────────┘
```

### Privileged Instructions

Certain CPU instructions can **only** be executed in privileged modes:

**x86 Examples:**
```asm
; Ring 0 only:
mov cr3, rax        ; Load page table base register
lgdt [gdt_ptr]      ; Load GDT (Global Descriptor Table)
lidt [idt_ptr]      ; Load IDT (Interrupt Descriptor Table)
hlt                 ; Halt CPU
in al, 0x60         ; Direct I/O port access
out 0x64, al

; Ring 3 execution of these triggers #GP (General Protection Fault)
```

**ARM Examples:**
```asm
; EL1+ only:
msr ttbr0_el1, x0   ; Set page table base register
msr sctlr_el1, x1   ; Set system control register
msr vbar_el1, x2    ; Set exception vector base

; EL0 execution triggers synchronous exception
```

### Hardware Enforcement

```c
// CPU hardware logic (pseudo-code)
void execute_instruction(instruction) {
    if (instruction.is_privileged) {
        if (cpu.current_privilege_level < instruction.required_level) {
            // Hardware blocks execution
            trigger_general_protection_fault();
            return;
        }
    }

    // Execute instruction
    perform_operation(instruction);
}
```

### Mode Transitions: Syscalls

User programs transition to kernel mode via **syscalls**:

**x86-64: `syscall` instruction**

```asm
; User space (Ring 3)
mov rax, 0          ; syscall number (read)
mov rdi, 0          ; fd
mov rsi, buffer     ; buffer
mov rdx, 1024       ; count
syscall             ; ← CPU transitions to Ring 0

; [CPU now in Ring 0, executing kernel code]

; Kernel handles syscall, then:
sysretq             ; ← Return to Ring 3
```

**What the CPU does during `syscall`:**

```c
// Hardware operation
1. Save user RIP (instruction pointer) to RCX
2. Save user RFLAGS to R11
3. Load kernel RIP from MSR (Model-Specific Register)
4. Load kernel stack pointer
5. Switch to Ring 0
6. Disable interrupts
7. Jump to kernel syscall handler
```

**Linux kernel syscall entry:**

```c
// arch/x86/entry/entry_64.S
SYM_CODE_START(entry_SYSCALL_64)
    // Save user registers
    swapgs              // Switch to kernel GS base
    movq %rsp, PER_CPU_VAR(cpu_tss_rw + TSS_sp2)
    movq PER_CPU_VAR(cpu_current_top_of_stack), %rsp

    // Now in kernel mode with kernel stack
    pushq $__USER_DS        // User data segment
    pushq PER_CPU_VAR(...)  // User stack pointer
    pushq %r11              // Saved RFLAGS
    pushq $__USER_CS        // User code segment
    pushq %rcx              // User RIP

    // Execute syscall
    call do_syscall_64

    // Return to user mode
    sysretq
SYM_CODE_END(entry_SYSCALL_64)
```

**Location**: `refs/linux/arch/x86/entry/entry_64.S`

## 3. ARM TrustZone: Two-World Architecture

ARM TrustZone creates **two completely isolated execution environments** on the same physical CPU, allowing protection even from the operating system kernel.

### Physical Memory Split

```
Physical RAM (Example: 2GB)
┌────────────────────────────────────────────────┐
│  Normal World Memory                           │
│  Address: 0x00000000 - 0x7FFFFFFF (1.5GB)     │
│  - Linux kernel can access                     │
│  - Normal applications                         │
│  - Regular drivers                             │
│  - Android system                              │
├────────────────────────────────────────────────┤
│  Secure World Memory                           │
│  Address: 0x80000000 - 0xFFFFFFFF (512MB)     │
│  - Only accessible from Secure World           │
│  - Widevine L1 runs here                       │
│  - Key storage                                 │
│  - Trusted execution environment               │
└────────────────────────────────────────────────┘
```

The memory controller enforces this split at the **hardware level** - Normal World accesses to Secure memory are physically blocked.

### The NS (Non-Secure) Bit

Every memory transaction has an extra signal: **NS (Non-Secure) bit**

```
CPU → Memory Controller

Memory Transaction:
    Address: 0x80000000
    NS bit: 1 (Normal World access)
    ↓
Memory Controller checks:
    if (NS == 1 && address >= SECURE_MEMORY_BASE) {
        BLOCK ACCESS;
        return 0x00000000;  // Or trigger exception
    }
```

**Normal World (NS=1):**
- All memory accesses automatically tagged with NS=1
- Can only access Normal memory regions
- Cannot set NS=0 (hardware enforced)

**Secure World (NS=0 or NS=1):**
- Can choose NS bit value
- NS=0: Access Secure memory
- NS=1: Access Normal memory
- Can access **both** worlds

### Separate Page Tables

Each world has its own page table base register:

```
┌─────────────────────────────────────────┐
│  Normal World Page Tables               │
│  (TTBR0_EL1 when in Normal World)       │
├─────────────────────────────────────────┤
│  Virtual 0x00001000 → Physical 0x10000  │ (Normal memory)
│  Virtual 0x00002000 → Physical 0x20000  │ (Normal memory)
│  Virtual 0x00003000 → Physical 0x80000  │ (Secure - BLOCKED by NS bit)
└─────────────────────────────────────────┘

┌─────────────────────────────────────────┐
│  Secure World Page Tables               │
│  (TTBR0_EL1 when in Secure World)       │
├─────────────────────────────────────────┤
│  Virtual 0x00001000 → Physical 0x80000  │ (Secure memory, NS=0) ✓
│  Virtual 0x00002000 → Physical 0x10000  │ (Normal memory, NS=1) ✓
│  Virtual 0x00003000 → Physical 0x90000  │ (Secure memory, NS=0) ✓
└─────────────────────────────────────────┘
```

When the CPU switches worlds, it switches the entire address space:

```c
// Hardware operation during world switch
if (switching_to_secure_world) {
    TTBR0_EL1 = secure_ttbr0;
    TTBR1_EL1 = secure_ttbr1;
    VBAR_EL1 = secure_exception_vectors;
    // ... many other registers
} else {
    TTBR0_EL1 = normal_ttbr0;
    TTBR1_EL1 = normal_ttbr1;
    VBAR_EL1 = normal_exception_vectors;
}
```

### World Switching: SMC Instruction

Transition between worlds via **SMC (Secure Monitor Call)**:

```
┌─────────────────────────────────────────────┐
│  User Application (EL0, Normal World)       │
│  - Widevine API call                        │
└──────────────────┬──────────────────────────┘
                   ↓ (system call)
┌─────────────────────────────────────────────┐
│  Linux Kernel (EL1, Normal World)           │
│  - TEE driver receives request              │
│  - Prepares shared memory                   │
│  - Executes: SMC #0                         │
└──────────────────┬──────────────────────────┘
                   ↓ (SMC instruction)
        [Hardware World Switch]
        - Save Normal World state
        - Switch to Secure World
        - Load Secure page tables
        - Jump to EL3 (Secure Monitor)
                   ↓
┌─────────────────────────────────────────────┐
│  Secure Monitor (EL3)                       │
│  - Routes call based on function ID         │
│  - Validates caller                         │
│  - Drops to EL1 Secure                      │
└──────────────────┬──────────────────────────┘
                   ↓
┌─────────────────────────────────────────────┐
│  Secure OS / Widevine (EL1, Secure World)   │
│  - Decrypt video content                    │
│  - Access secure memory (0x80000000+)       │
│  - Process DRM keys                         │
│  - Return result                            │
└──────────────────┬──────────────────────────┘
                   ↓ (SMC return)
        [Hardware World Switch Back]
        - Save Secure World state
        - Restore Normal World state
        - Load Normal page tables
                   ↓
┌─────────────────────────────────────────────┐
│  Linux Kernel (EL1, Normal World)           │
│  - Receives result                          │
│  - Returns to user space                    │
└─────────────────────────────────────────────┘
```

### Example: Widevine Decryption Flow

```c
// 1. User space (Normal World EL0)
// Browser calls Widevine CDM
widevine_decrypt(encrypted_chunk, license);

// 2. Widevine library (Normal World EL0)
// Makes ioctl to TEE driver
fd = open("/dev/tee0", O_RDWR);
ioctl(fd, TEE_IOC_INVOKE, &invoke_args);

// 3. Linux kernel TEE driver (Normal World EL1)
// refs/linux/drivers/tee/optee/call.c
static u32 optee_do_call_with_arg(struct tee_context *ctx, phys_addr_t parg)
{
    struct arm_smccc_res res;

    // Execute SMC instruction - switches to Secure World
    arm_smccc_smc(OPTEE_SMC_CALL_WITH_ARG,
                  (unsigned long)parg, 0, 0, 0, 0, 0, 0,
                  &res);

    return res.a0;
}

// [CPU now in Secure World]

// 4. Secure Monitor (EL3)
// Routes to Widevine TEE application

// 5. Widevine in Secure World (EL1 Secure)
// - Reads encrypted chunk from shared memory (Normal memory, NS=1)
// - Decrypts using keys in Secure memory (NS=0)
// - Writes decrypted video to Secure buffer (NS=0)
// - Normal World cannot read this buffer!

// 6. GPU compositor
// - Has IOMMU mapping to Secure buffer
// - Reads via hardware secure path
// - Composites with UI from Normal memory
```

**Key insight:** Decrypted video exists in memory, but in a region that Linux (Normal World) **cannot access**. The hardware memory controller blocks it.

### Linux Kernel Source: TEE Driver

```c
// drivers/tee/optee/core.c
static int optee_probe(struct platform_device *pdev)
{
    // Set up communication with Secure World
    // Discovers what Secure services are available
    optee_enumerate_devices();
}

// drivers/tee/optee/call.c
void arm_smccc_smc(unsigned long a0, unsigned long a1,
                   unsigned long a2, unsigned long a3,
                   unsigned long a4, unsigned long a5,
                   unsigned long a6, unsigned long a7,
                   struct arm_smccc_res *res)
{
    // Assembly wrapper around SMC instruction
    // refs/linux/arch/arm64/kernel/smccc-call.S
}
```

**Locations:**
- `refs/linux/drivers/tee/optee/core.c`
- `refs/linux/drivers/tee/optee/call.c`
- `refs/linux/arch/arm64/kernel/smccc-call.S`

## 4. Intel SGX: Encrypted Enclaves

Intel SGX (Software Guard Extensions) takes a different approach: **encrypt memory regions** so they're protected even from the OS kernel and physical attacks.

### Enclave Memory Layout

```
Physical RAM
┌────────────────────────────────────────────┐
│  Regular Memory (Plaintext)                │
│  - OS kernel                               │
│  - Normal applications                     │
│  - Page tables                             │
│  Address: 0x00000000 - 0x7FFFFFFF          │
├────────────────────────────────────────────┤
│  PRM (Processor Reserved Memory)           │
│  - SGX enclave pages (ENCRYPTED in RAM)    │
│  - Cannot be accessed by OS                │
│  - Cannot be DMAed by devices              │
│  Address: 0x80000000 - 0x90000000          │
│  ← Widevine L1 can run here                │
└────────────────────────────────────────────┘
```

The OS cannot even map PRM pages - the CPU blocks any attempt to create page table entries pointing to PRM.

### Memory Encryption Engine (MEE)

Enclave memory is **encrypted in RAM** but **decrypted in CPU caches**:

```
┌─────────────────────────────────────────┐
│  CPU Core                               │
│  ├─ Registers (plaintext)               │
│  ├─ L1 Cache (plaintext inside CPU)     │
│  └─ L2 Cache (plaintext inside CPU)     │
└──────────────────┬──────────────────────┘
                   ↓
┌──────────────────────────────────────────┐
│  L3 Cache (plaintext, shared)            │
│  - Access controlled by CPU              │
└──────────────────┬───────────────────────┘
                   ↓
┌──────────────────────────────────────────┐
│  Memory Encryption Engine (MEE)          │
│  - Encrypts on write to RAM              │
│  - Decrypts on read from RAM             │
│  - Uses per-enclave keys                 │
└──────────────────┬───────────────────────┘
                   ↓
┌──────────────────────────────────────────┐
│  Physical RAM (ENCRYPTED)                │
│  - Encrypted data                        │
│  - Integrity tree (detect tampering)     │
│  - Useless if stolen or DMACopied        │
└──────────────────────────────────────────┘
```

### Accessing Enclave Memory

```c
// Outside enclave (OS, other apps)
char *enclave_addr = (char *)0x80000000;  // Points to enclave memory
char byte = *enclave_addr;

// What happens:
// 1. MMU translates address
// 2. CPU detects: address in PRM range
// 3. CPU checks: EPCM (Enclave Page Cache Map)
// 4. EPCM says: "This page belongs to enclave X"
// 5. CPU checks: Current context != enclave X
// 6. CPU blocks access, returns 0xFFFFFFFF or triggers fault

// Inside enclave (enclave code)
char *data = (char *)0x80000000;
char byte = *data;

// What happens:
// 1. MMU translates address
// 2. CPU detects: address in PRM range
// 3. EPCM check: "This page belongs to current enclave" ✓
// 4. CPU reads encrypted data from RAM
// 5. MEE decrypts using enclave key
// 6. Plaintext returned to CPU register
```

### Per-Enclave Encryption Keys

Each enclave has a **unique encryption key** derived from:

```
Enclave Sealing Key = KDF(
    CPU_Root_Key,           // Unique per CPU, burned in at manufacturing
    MRENCLAVE,              // Hash of enclave code and data
    MRSIGNER,               // Hash of enclave author's public key
    Additional inputs...
)
```

**Properties:**
- Different enclaves have different keys (isolated)
- Same enclave on different CPU has different key (sealed to hardware)
- Changing enclave code changes key (integrity)
- Even Intel doesn't know the CPU root key

### Enclave Entry/Exit

```
┌─────────────────────────────────────────┐
│  Regular Application                    │
│  (non-enclave code)                     │
├─────────────────────────────────────────┤
│  // Call enclave                        │
│  sgx_enclave_call(ecall_function,       │
│                   &args);                │
└──────────────────┬──────────────────────┘
                   ↓
        [CPU executes EENTER instruction]
        - Validate enclave
        - Check EPCM
        - Switch to enclave mode
        - Clear registers (prevent leaking data)
                   ↓
┌─────────────────────────────────────────┐
│  Inside Enclave                         │
│  (protected execution)                  │
├─────────────────────────────────────────┤
│  // Enclave code runs                   │
│  decrypt_video(encrypted, key);         │
│  // Can access enclave memory           │
│  // CPU decrypts memory transparently   │
└──────────────────┬──────────────────────┘
                   ↓
        [CPU executes EEXIT instruction]
        - Clear enclave state from registers
        - Clear CPU flags
        - Return to non-enclave mode
                   ↓
┌─────────────────────────────────────────┐
│  Regular Application                    │
│  (continues execution)                  │
└─────────────────────────────────────────┘
```

### Protection Against Physical Attacks

**Scenario: Attacker steals RAM stick**

```
Attacker extracts enclave memory pages
    ↓
Reads physical RAM: 0x8F3A7C2B1E9D...
    ↓
Tries to decrypt with known plaintext attack
    ↓
FAIL: Needs enclave key
    ↓
Enclave key = KDF(CPU_root_key, ...)
    ↓
CPU_root_key only exists in CPU, never in RAM
    ↓
Memory contents useless without original CPU
```

**Scenario: Malicious OS tries to read enclave**

```
Kernel tries to create page table entry:
    PTE: Virtual 0x7000 → Physical 0x80000000 (PRM)
    ↓
CPU checks: Physical address in PRM?
    ↓
CPU blocks: EPCM violation
    ↓
Cannot map PRM memory in page tables
    ↓
Kernel cannot read enclave memory
```

### Linux SGX Driver

The Linux kernel provides an SGX driver but **cannot access enclave contents**:

```c
// arch/x86/kernel/cpu/sgx/driver.c
static long sgx_enclave_ioctl(struct file *filp, unsigned int cmd,
                              unsigned long arg)
{
    switch (cmd) {
    case SGX_IOC_ENCLAVE_CREATE:
        // Allocate EPC pages from PRM
        return sgx_enclave_create(encl, arg);

    case SGX_IOC_ENCLAVE_ADD_PAGES:
        // Add pages to enclave (before initialization)
        return sgx_enclave_add_pages(encl, arg);

    case SGX_IOC_ENCLAVE_INIT:
        // Finalize enclave (measure and seal)
        return sgx_enclave_init(encl, arg);
    }
}

// Kernel can manage enclave lifecycle but cannot read enclave memory
```

**Location**: `refs/linux/arch/x86/kernel/cpu/sgx/driver.c`

## 5. IOMMU: Device Memory Protection

CPUs aren't the only things that access memory - **devices** use **DMA (Direct Memory Access)** to read/write memory without CPU involvement. The **IOMMU** provides page tables for devices.

### The DMA Problem

```
Without IOMMU:

Network Card receives packet
    ↓
Network Card uses DMA to write directly to physical address 0x10000000
    ↓
No permission checking - can write ANYWHERE in physical RAM
    ↓
Security problem: Malicious device can:
    - Read kernel memory
    - Overwrite page tables
    - Read other process memory
    - Read DRM-protected content
```

### IOMMU Solution

```
With IOMMU:

Network Card thinks it's writing to address 0x10000000
    ↓
[IOMMU Translation]
    ↓
IOMMU looks up device page table:
    Device Virtual 0x10000000 → Physical 0x20000000 ✓
    Permissions: Read/Write ✓
    ↓
IOMMU allows access to Physical 0x20000000
    ↓
Access to unmapped addresses → IOMMU blocks and logs fault
```

### Per-Device Page Tables

```
┌─────────────────────────────────────────┐
│  GPU Page Table (IOMMU)                 │
├─────────────────────────────────────────┤
│  Device Virtual 0x1000 → Physical       │
│    0x80000000 (Secure buffer) ALLOW     │
│  Device Virtual 0x2000 → Physical       │
│    0x81000000 (Secure buffer) ALLOW     │
│  All other addresses: DENY              │
└─────────────────────────────────────────┘

┌─────────────────────────────────────────┐
│  Network Card Page Table (IOMMU)        │
├─────────────────────────────────────────┤
│  Device Virtual 0x1000 → Physical       │
│    0x20000000 (Network RX buffer) ALLOW │
│  Device Virtual 0x2000 → Physical       │
│    0x21000000 (Network TX buffer) ALLOW │
│  All other addresses: DENY              │
└─────────────────────────────────────────┘
```

The kernel controls what each device can access:

```c
// Kernel sets up GPU to access secure video buffer
dma_addr_t gpu_addr = iommu_map(gpu_domain,
                                 0x1000,           // Device virtual
                                 secure_phys_addr,  // Physical (secure)
                                 buffer_size,
                                 IOMMU_READ);

// Now GPU can read secure buffer at device address 0x1000
// GPU cannot access any other physical address
```

### Architecture Diagram

```
┌──────────────────────────────────────────────────────────┐
│                      CPU                                 │
│  Accesses memory via MMU (CPU page tables)               │
└────────────────────────┬─────────────────────────────────┘
                         ↓
┌──────────────────────────────────────────────────────────┐
│                  System Memory (RAM)                     │
│  ├─ Normal memory (plaintext)                            │
│  ├─ Secure memory (TrustZone protected)                  │
│  └─ Enclave memory (SGX encrypted)                       │
└──────┬────────────────────────────────┬──────────────────┘
       ↑                                ↑
       │                                │
┌──────┴──────────┐            ┌────────┴──────────┐
│  IOMMU for GPU  │            │  IOMMU for NIC    │
│  (Device tables)│            │  (Device tables)  │
└──────┬──────────┘            └────────┬──────────┘
       ↑                                ↑
       │                                │
┌──────┴──────────┐            ┌────────┴──────────┐
│      GPU        │            │   Network Card    │
│  (Uses DMA)     │            │   (Uses DMA)      │
└─────────────────┘            └───────────────────┘
```

### Secure Video Playback with IOMMU

```
Encrypted Stream from Network
    ↓
Network Card DMAs to buffer (via IOMMU)
    ↓
Widevine in TrustZone/SGX decrypts
    ↓
Decrypted video in Secure memory (0x80000000)
    ↓
Kernel configures GPU IOMMU:
    iommu_map(gpu, 0x1000, 0x80000000, IOMMU_READ)
    ↓
GPU reads secure buffer at device address 0x1000
    - IOMMU translates 0x1000 → 0x80000000
    - GPU can ONLY access this specific range
    - GPU cannot access other processes
    - GPU cannot access kernel memory
    ↓
GPU composites with UI and outputs to display
```

### Linux Kernel IOMMU Implementation

```c
// drivers/iommu/iommu.c
int iommu_map(struct iommu_domain *domain, unsigned long iova,
              phys_addr_t paddr, size_t size, int prot)
{
    // Map device virtual address (iova) to physical address
    // prot: IOMMU_READ, IOMMU_WRITE, etc.

    return domain->ops->map(domain, iova, paddr, size, prot);
}

void iommu_unmap(struct iommu_domain *domain, unsigned long iova, size_t size)
{
    // Remove mapping - device can no longer access this memory
    domain->ops->unmap(domain, iova, size);
}

// Device drivers request IOMMU mappings:
// drivers/gpu/drm/drm_gem.c
int drm_gem_map_dma_buf(struct dma_buf_attachment *attach,
                        enum dma_data_direction dir)
{
    // Map GEM buffer for GPU access via IOMMU
    // Kernel controls exactly what GPU can see
}
```

**Locations:**
- `refs/linux/drivers/iommu/iommu.c`
- `refs/linux/include/linux/iommu.h`
- `refs/linux/drivers/gpu/drm/drm_gem.c`

## 6. Complete Example: Widevine L1 Protection

Let's trace how all these mechanisms work together to protect Netflix 4K video:

### Initial Setup

```
1. User opens Netflix in browser
2. Browser loads Widevine CDM
3. Widevine initializes in TrustZone/SGX enclave
4. Secure memory allocated: 0x80000000 - 0x82000000 (32MB)
```

### License Acquisition

```
5. Browser requests video → Netflix CDN
6. CDN sends encrypted video segments
7. Widevine needs decryption keys
8. Browser → license.netflix.com
9. License server validates:
   - Valid subscription? ✓
   - Device security level? L1 (TrustZone/SGX) ✓
   - Region check? ✓
10. License server sends: Decryption keys
```

### Memory Layout During Playback

```
Physical Memory Map:

0x00000000 ┌──────────────────────────────────┐
           │  User Space (Browser)            │
           │  - JavaScript engine             │
           │  - HTML/CSS renderer             │
           │  - Encrypted video segments      │
           │  MMU: User pages, Read/Write     │
           ├──────────────────────────────────┤
0x40000000 │  Kernel Memory                   │
           │  - Linux kernel code/data        │
           │  - Page tables                   │
           │  - Device drivers                │
           │  MMU: Supervisor only            │
           ├──────────────────────────────────┤
0x80000000 │  Secure World / SGX Enclave      │
           │  - Widevine CDM                  │
           │  - Decryption keys               │
           │  - Decrypted video frames        │
           │  TrustZone: NS bit blocks access │
           │  SGX: Encrypted in RAM           │
           └──────────────────────────────────┘
0x90000000
```

### Decryption Flow with All Protection Layers

```
Step 1: Download Encrypted Segment
    Browser (Normal World EL0):
        fetch("cdn.netflix.com/segment001.m4s")
        ↓
    Encrypted data in normal memory: 0x10000000
    ✓ MMU allows: User page
    ✓ No protection needed (data is encrypted)

Step 2: Request Decryption
    Browser calls: widevine.decrypt(segment_data, key_id)
        ↓
    Widevine library (Normal World EL0):
        ioctl(tee_fd, TEE_DECRYPT, &args)
        ↓
    Linux kernel (Normal World EL1):
        Prepares shared memory
        Sets up arguments
        Executes: SMC #0
        ↓
    [HARDWARE WORLD SWITCH]
        - CPU saves Normal World state
        - CPU loads Secure World page tables
        - CPU switches to Secure World
        - TTBR0_EL1 = secure_page_table_base
        ↓
    Secure Monitor (EL3):
        Validates call
        Routes to Widevine TEE app
        ↓
    Widevine in TrustZone (Secure World EL1):
        ✓ Can read encrypted data from 0x10000000 (NS=1 access)
        ✓ Decrypts using keys in secure memory 0x80000000 (NS=0)
        ✓ Writes plaintext to secure buffer 0x80100000 (NS=0)
        Returns handle to secure buffer
        ↓
    [HARDWARE WORLD SWITCH BACK]
        - CPU saves Secure World state
        - CPU restores Normal World state
        - TTBR0_EL1 = normal_page_table_base
        ↓
    Linux kernel (Normal World EL1):
        Receives secure buffer handle
        ❌ CANNOT read buffer (NS bit blocks)
        Configures GPU IOMMU to access buffer
        Returns handle to browser

Step 3: Display Video
    Browser tells GPU: Render frame from handle
        ↓
    GPU (via DMA):
        Reads from device address 0x1000
        ↓
    IOMMU translates:
        Device 0x1000 → Physical 0x80100000
        ✓ Mapping exists (kernel configured it)
        ✓ Permission: Read only
        ↓
    GPU reads decrypted video frame
        ✓ GPU has hardware secure path
        ✓ Can access Secure memory via IOMMU
        ↓
    GPU composites:
        Layer 0: Desktop (Normal memory)
        Layer 1: Browser UI (Normal memory)
        Layer 2: Video frame (Secure memory via IOMMU)
        ↓
    GPU outputs to Display via HDMI
        ✓ HDCP encryption on cable
```

### What Each Layer Blocks

**If Normal World process tries to read 0x80100000:**
```c
char *secure = (char *)0x80100000;
char byte = *secure;

// MMU walks page tables
// Finds PTE with NS bit enforcement
// Memory controller checks: NS=1 (Normal World), address=Secure
// HARDWARE BLOCKS: Returns 0x00 or triggers fault
// Process never sees decrypted video
```

**If kernel tries to DMA from network card to secure memory:**
```c
// Kernel attempts:
dma_addr = 0x80100000;  // Secure memory
network_dma_to(dma_addr, packet_data);

// IOMMU checks device page table
// Secure memory not mapped for network card
// IOMMU BLOCKS: Triggers IOMMU fault
// Kernel logs error
```

**If attacker steals RAM stick (SGX case):**
```
Attacker extracts physical memory contents
    ↓
Reads address 0x80100000:
    Result: 0x8F3A7C2B1E9D4F2A... (encrypted garbage)
    ↓
Tries to decrypt:
    Needs enclave key = KDF(CPU_root_key, MRENCLAVE, ...)
    ↓
CPU_root_key only exists in CPU, never in RAM
    ↓
Memory contents useless
```

### Access Control Matrix

| Entity                  | Normal Memory | Kernel Memory | Secure Memory |
|-------------------------|---------------|---------------|---------------|
| User process            | Own pages ✓   | ❌ MMU blocks | ❌ NS bit     |
| Linux kernel            | All ✓         | All ✓         | ❌ NS bit     |
| Widevine (TrustZone)    | ✓ NS=1        | ✓ NS=1        | ✓ NS=0        |
| GPU (via IOMMU)         | If mapped     | ❌            | If mapped ✓   |
| Network card (via IOMMU)| If mapped     | ❌            | ❌            |
| Physical attacker       | Read RAM ✓    | Read RAM ✓    | ❌ Encrypted  |

All "❌" entries are **hardware-enforced** blocks.

## 7. Linux Kernel Implementation

### Page Table Management

```c
// arch/arm64/include/asm/pgtable.h

// Page table entry bit definitions
#define PTE_VALID       (_AT(pteval_t, 1) << 0)  // Page present
#define PTE_USER        (_AT(pteval_t, 1) << 6)  // User accessible
#define PTE_RDONLY      (_AT(pteval_t, 1) << 7)  // Read-only
#define PTE_SHARED      (_AT(pteval_t, 3) << 8)  // Shareability
#define PTE_AF          (_AT(pteval_t, 1) << 10) // Access flag
#define PTE_NG          (_AT(pteval_t, 1) << 11) // Not global
#define PTE_NS          (_AT(pteval_t, 1) << 5)  // Non-secure (TrustZone)
#define PTE_UXN         (_AT(pteval_t, 1) << 54) // User execute never
#define PTE_PXN         (_AT(pteval_t, 1) << 53) // Privileged execute never

// mm/memory.c - Core memory management

static vm_fault_t handle_pte_fault(struct vm_fault *vmf)
{
    // Called when MMU triggers page fault
    // Hardware did the blocking, now kernel handles the fault

    if (!pte_present(vmf->orig_pte)) {
        // Page not in memory - load from disk/swap
        return do_fault(vmf);
    }

    if (vmf->flags & FAULT_FLAG_WRITE) {
        if (!pte_write(vmf->orig_pte)) {
            // Write to read-only page - might be COW
            return do_wp_page(vmf);
        }
    }

    return 0;
}

// mm/mmap.c - Memory mapping

unsigned long do_mmap(struct file *file, unsigned long addr,
                      unsigned long len, unsigned long prot,
                      unsigned long flags, unsigned long pgoff)
{
    // Creates virtual memory area (VMA)
    // Configures page table entries with protection bits

    vm_flags = calc_vm_prot_bits(prot, 0);  // Convert prot → page table flags

    if (!(flags & MAP_ANONYMOUS)) {
        // File-backed mapping
        vma->vm_file = file;
    }

    // Insert VMA into process address space
    vma_link(mm, vma, prev, rb_link, rb_parent);
}
```

**Locations:**
- `refs/linux/arch/arm64/include/asm/pgtable.h`
- `refs/linux/mm/memory.c`
- `refs/linux/mm/mmap.c`

### Page Fault Handler

```c
// arch/x86/mm/fault.c
dotraplinkage void do_page_fault(struct pt_regs *regs,
                                 unsigned long error_code,
                                 unsigned long address)
{
    /*
     * error_code bits:
     * bit 0: 0=no page present, 1=protection fault
     * bit 1: 0=read, 1=write
     * bit 2: 0=kernel, 1=user mode
     * bit 3: 1=reserved bit set
     * bit 4: 1=instruction fetch
     */

    if (unlikely(error_code & X86_PF_RSVD)) {
        // Reserved bit violation - page table corruption?
        pgtable_bad(regs, error_code, address);
        return;
    }

    if (error_code & X86_PF_USER) {
        // User-mode fault
        if (!(error_code & X86_PF_PROT)) {
            // Page not present - could be valid (demand paging)
        } else {
            // Protection violation - accessing kernel memory?
            // Or writing to read-only page?
            handle_protection_fault(regs, error_code, address);
        }
    } else {
        // Kernel-mode fault
        handle_kernel_fault(regs, error_code, address);
    }
}
```

**Location**: `refs/linux/arch/x86/mm/fault.c`

### TrustZone / TEE Driver

```c
// drivers/tee/optee/core.c
static const struct tee_driver_ops optee_ops = {
    .get_version = optee_get_version,
    .open = optee_open,
    .release = optee_release,
    .open_session = optee_open_session,
    .close_session = optee_close_session,
    .invoke_func = optee_invoke_func,  // Main entry point
};

// drivers/tee/optee/call.c
u32 optee_do_call_with_arg(struct tee_context *ctx, phys_addr_t parg)
{
    struct arm_smccc_res res;

    // Invoke Secure World via SMC instruction
    arm_smccc_smc(OPTEE_SMC_CALL_WITH_ARG,
                  (unsigned long)parg,
                  0, 0, 0, 0, 0, 0, &res);

    // After SMC:
    // 1. CPU switched to Secure World
    // 2. Secure code executed
    // 3. CPU switched back to Normal World
    // 4. Result in res.a0

    return res.a0;
}

// arch/arm64/kernel/smccc-call.S
SYM_FUNC_START(__arm_smccc_smc)
    smc #0              // Secure Monitor Call - hardware world switch
    ldr x5, [sp]
    stp x0, x1, [x5, #0]   // Store results
    stp x2, x3, [x5, #16]
    ret
SYM_FUNC_END(__arm_smccc_smc)
```

**Locations:**
- `refs/linux/drivers/tee/optee/core.c`
- `refs/linux/drivers/tee/optee/call.c`
- `refs/linux/arch/arm64/kernel/smccc-call.S`

### IOMMU Configuration

```c
// drivers/iommu/iommu.c
int iommu_map(struct iommu_domain *domain, unsigned long iova,
              phys_addr_t paddr, size_t size, int prot)
{
    // Map device virtual address to physical address
    const struct iommu_ops *ops = domain->ops;
    unsigned long orig_iova = iova;
    size_t orig_size = size;
    int ret;

    // Align to page boundaries
    // ...

    // Call hardware-specific mapping function
    ret = ops->map(domain, iova, paddr, size, prot);

    return ret;
}

void iommu_unmap(struct iommu_domain *domain, unsigned long iova, size_t size)
{
    // Remove device mapping
    domain->ops->unmap(domain, iova, size);
}

// drivers/iommu/dma-iommu.c
dma_addr_t iommu_dma_map_page(struct device *dev, struct page *page,
                              unsigned long offset, size_t size,
                              enum dma_data_direction dir,
                              unsigned long attrs)
{
    // Map page for device DMA access
    phys_addr_t phys = page_to_phys(page) + offset;
    dma_addr_t dma_addr;

    // Allocate IOVA (device virtual address)
    dma_addr = __iommu_dma_map(dev, phys, size, prot, dma_mask);

    return dma_addr;
}
```

**Locations:**
- `refs/linux/drivers/iommu/iommu.c`
- `refs/linux/drivers/iommu/dma-iommu.c`
- `refs/linux/include/linux/iommu.h`

### DRM Secure Buffer Allocation

```c
// drivers/gpu/drm/drm_gem.c
struct drm_gem_object *drm_gem_object_alloc(struct drm_device *dev,
                                            size_t size)
{
    // Allocate GEM (Graphics Execution Manager) buffer
    // Can be configured for secure memory
}

// drivers/gpu/drm/drm_prime.c
struct dma_buf *drm_gem_prime_export(struct drm_gem_object *obj, int flags)
{
    // Export GEM buffer as dma-buf
    // Can be shared between devices (GPU, display controller)
    // IOMMU controls access
}

// Example: Secure buffer for protected content
int drm_mode_create_protected_blob(struct drm_device *dev,
                                   struct drm_mode_create_blob *create_blob)
{
    // Create blob (buffer object) in protected memory
    // Used for secure video frames
    // Only GPU with proper IOMMU mapping can access
}
```

**Locations:**
- `refs/linux/drivers/gpu/drm/drm_gem.c`
- `refs/linux/drivers/gpu/drm/drm_prime.c`

## 8. Attack Scenarios and Defenses

### Attack 1: User Process Reads Kernel Memory

**Attack:**
```c
// Malicious user program
int main() {
    char *kernel_addr = (char *)0xffffffff81000000;
    char byte = *kernel_addr;  // Try to read kernel
    printf("Kernel data: %02x\n", byte);
}
```

**Defense (Hardware):**
```
CPU MMU checks:
1. Translate 0xffffffff81000000 via page tables
2. Find PTE with U/S bit = 0 (Supervisor only)
3. Current CPU mode = Ring 3 (User)
4. BLOCK ACCESS - trigger #PF (Page Fault)
5. Jump to kernel page fault handler

Kernel handler:
    - Detects: Protection violation from user space
    - Logs: "Segmentation fault"
    - Kills process with SIGSEGV
```

**Result:** Process terminated before seeing any kernel data.

### Attack 2: DMA Attack via Malicious Device

**Attack:**
```
Attacker connects malicious USB device
Device claims to be a network card
Device firmware attempts DMA to 0x81000000 (kernel memory)
Goal: Read kernel memory or overwrite page tables
```

**Defense (Hardware):**
```
Device initiates DMA transaction:
    Target address: 0x81000000
    ↓
IOMMU intercepts:
    1. Look up device in IOMMU groups
    2. Check device page table
    3. Is 0x81000000 mapped for this device? NO
    4. BLOCK ACCESS
    5. Trigger IOMMU fault interrupt
    ↓
Kernel IOMMU fault handler:
    - Logs: "IOMMU fault from device X"
    - Disables device
    - Alerts administrator
```

**Result:** DMA blocked, kernel memory safe.

### Attack 3: Normal World Reads Secure Memory (TrustZone)

**Attack:**
```c
// Attacker gains root on Android/Linux
// Tries to read Widevine secure memory
int main() {
    int fd = open("/dev/mem", O_RDONLY);  // Physical memory device
    void *secure = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd,
                        0x80000000);  // Secure World physical address
    char byte = *(char *)secure;
    printf("Secure data: %02x\n", byte);
}
```

**Defense (Hardware):**
```
Two layers of protection:

Layer 1 - Kernel /dev/mem checks:
    if (address >= SECURE_MEMORY_BASE) {
        return -EPERM;  // Kernel refuses
    }

Layer 2 - Hardware NS bit (if kernel is compromised):
    CPU in Normal World → All accesses tagged NS=1
    Memory controller sees:
        Address: 0x80000000 (Secure)
        NS bit: 1 (Normal World access)
    Memory controller logic:
        if (NS==1 && address >= SECURE_BASE) {
            BLOCK;
            return 0x00000000;  // Or trigger abort
        }
```

**Result:** Even root cannot read Secure World memory. Hardware enforces separation.

### Attack 4: Cold Boot Attack (Memory Remnance)

**Attack:**
```
Attacker physically accesses machine
    ↓
1. Freeze RAM with liquid nitrogen (slow decay)
2. Quickly reboot into custom OS
3. Dump all physical RAM
4. Search for encryption keys or video frames
```

**Defense (SGX):**
```
SGX Protection:
    - Enclave memory encrypted in RAM with ephemeral key
    - Key derived from CPU secret (unique per CPU)
    - Key exists only in CPU, never in RAM
    - On reboot/power loss: Key lost, memory useless

Even if attacker reads RAM:
    Physical 0x80000000: 0x8F3A7C2B1E9D...
    This is encrypted with key that no longer exists
    Cannot decrypt without the specific CPU's secret
```

**Defense (TrustZone):**
```
TrustZone systems can:
    - Wipe secure memory on reset
    - Use hardware memory encryption (some SoCs)
    - Detect tampering and clear secrets
```

**Result:** Decrypted content not recoverable from powered-off RAM.

### Attack 5: Rowhammer (Memory Bit Flips)

**Attack:**
```
Exploit DRAM behavior:
    - Repeatedly access same memory row (hammering)
    - Causes bit flips in adjacent rows
    - Target: Flip bits in page table entries
    - Goal: Change U/S bit from 0→1 (kernel→user accessible)
```

**Defense (Multiple Layers):**
```
1. ECC Memory (Error-Correcting Code):
    - Detects and corrects single-bit errors
    - Detects multi-bit errors
    - Expensive, mainly in servers

2. TRR (Target Row Refresh):
    - Memory controller detects hammering
    - Refreshes adjacent rows
    - Modern DDR4/DDR5 feature

3. Guard Pages:
    - Kernel places unmapped pages around sensitive data
    - Bit flips in guard pages trigger faults

4. Page Table Integrity Checks:
    - Kernel periodically validates page tables
    - Detects unexpected modifications

5. TrustZone/SGX:
    - Secure memory separate from DRAM issues
    - Even if page tables compromised, cannot access secure world
```

### Attack 6: Spectre/Meltdown (Speculative Execution)

**Attack:**
```c
// Spectre-style attack
if (user_controlled_index < array_size) {  // Bounds check
    // CPU speculatively executes before check completes
    char data = kernel_memory[user_controlled_index];
    // Even though this will fault, speculative execution
    // brings kernel data into cache
    // Side-channel timing attack reveals data
}
```

**Defense (Multiple Layers):**
```
1. Microcode Updates:
    - CPU microcode patched to reduce speculation

2. Kernel Page Table Isolation (KPTI):
    // mm/init.c
    - Separate page tables for kernel and user mode
    - When in user mode: Kernel pages not mapped
    - Cannot speculatively access kernel memory
    - Performance cost: TLB flush on syscall

3. Retpoline:
    - Replace indirect jumps with returns
    - Prevents branch prediction attacks

4. IBRS (Indirect Branch Restricted Speculation):
    - CPU feature to disable speculation across privilege boundaries

5. TrustZone/SGX:
    - Secure World/Enclave memory not in same address space
    - Cannot be speculatively accessed from Normal World
```

**Linux implementation:**
```c
// arch/x86/mm/pti.c - Page Table Isolation
void __init pti_init(void)
{
    // Set up shadow page tables
    // User mode uses page tables without kernel mappings
    pti_clone_kernel_text();  // Only minimal kernel needed for syscall entry
}

// arch/x86/entry/entry_64.S
// On syscall entry:
SWITCH_TO_KERNEL_CR3  // Load kernel page tables (includes kernel memory)
// ... syscall execution ...
SWITCH_TO_USER_CR3    // Load user page tables (kernel memory unmapped)
```

**Locations:**
- `refs/linux/arch/x86/mm/pti.c`
- `refs/linux/arch/x86/entry/entry_64.S`

## Key Takeaways

1. **Memory protection is hardware-enforced**, not software-enforced. The OS configures rules, but the CPU/MMU/IOMMU physically block unauthorized access.

2. **Multiple layers of protection**:
   - MMU + Page Tables: Basic virtual memory protection (user/kernel separation)
   - CPU Privilege Levels: Ring 0-3 (x86) or EL0-3 (ARM)
   - TrustZone: Two-world architecture with separate page tables and NS bit enforcement
   - SGX: Memory encryption with per-enclave keys
   - IOMMU: Device page tables to control DMA access

3. **Protected memory exists in RAM** but is inaccessible:
   - TrustZone: NS bit prevents Normal World from accessing Secure memory
   - SGX: Memory encrypted in RAM, only decrypted inside enclaves
   - IOMMU: Devices can only access explicitly mapped regions

4. **Even the kernel can be blocked** from accessing certain memory:
   - TrustZone: Linux kernel runs in Normal World, cannot access Secure World
   - SGX: Kernel cannot map or access enclave memory (PRM)

5. **Widevine L1 protection relies on these hardware features**:
   - Decrypted video in TrustZone Secure World or SGX enclave
   - Normal World (OS/apps) cannot access
   - GPU accesses via secure IOMMU mapping
   - Hardware secure path from decryption to display

6. **Attack surface**: Most practical attacks bypass protection rather than break it:
   - Screen recording (lossy, watermarked)
   - HDMI capture with HDCP stripper (illegal, detectable)
   - Forensic watermarking tracks leakers
   - Legal consequences (DMCA) deter casual piracy

## Quick Reference

### Memory Protection Mechanisms

| Feature | Purpose | Enforced By |
|---------|---------|-------------|
| Page Tables + MMU | Virtual memory, user/kernel separation | CPU MMU |
| CPU Privilege Levels | Instruction and memory access control | CPU |
| TrustZone NS Bit | Normal/Secure World separation | Memory Controller |
| SGX MEE | Enclave memory encryption | CPU + MEE Hardware |
| IOMMU | Device DMA protection | IOMMU Hardware |
| HDCP | Display link encryption | GPU + Display |

### ARM TrustZone

- **Normal World**: Linux, Android, applications (NS=1)
- **Secure World**: Widevine, key storage, biometrics (NS=0)
- **Switch**: SMC instruction (Secure Monitor Call)
- **Separation**: Hardware NS bit enforcement by memory controller

### Intel SGX

- **Enclave**: Protected execution environment
- **PRM**: Processor Reserved Memory (enclaves live here)
- **MEE**: Memory Encryption Engine (encrypts/decrypts automatically)
- **Entry**: EENTER instruction
- **Exit**: EEXIT instruction

### Linux Kernel Paths

- Page fault handling: `refs/linux/arch/x86/mm/fault.c`
- Page table definitions: `refs/linux/arch/arm64/include/asm/pgtable.h`
- TrustZone/TEE driver: `refs/linux/drivers/tee/optee/`
- IOMMU core: `refs/linux/drivers/iommu/iommu.c`
- DRM secure buffers: `refs/linux/drivers/gpu/drm/`
- SGX driver: `refs/linux/arch/x86/kernel/cpu/sgx/`

### Key Data Structures

**x86-64 PTE (Page Table Entry)**:
```c
typedef unsigned long pteval_t;
// Bits: 63=NX, 51-12=Physical Address, 7=PS, 6=D, 5=A, 2=U/S, 1=R/W, 0=P
```

**ARM64 PTE**:
```c
typedef unsigned long pteval_t;
// Bits: 54=UXN, 53=PXN, 47-12=Physical Address, 7=AP[0], 6=AP[1], 5=NS, 0=Valid
```

**IOMMU Domain**:
```c
struct iommu_domain {
    const struct iommu_ops *ops;  // Hardware-specific operations
    // Per-device page tables
};
```

### Protection Check Examples

**User accessing kernel memory**:
```
Virtual → MMU → PTE has U/S=0 → CPU in Ring 3 → BLOCK → #PF
```

**Normal World accessing Secure memory**:
```
Address=0x80000000 → NS=1 → Memory Controller → Address>=Secure → BLOCK
```

**Device DMA to unmapped address**:
```
Device→IOMMU → Device page table lookup → Not found → BLOCK → IOMMU fault
```

**Process accessing SGX enclave**:
```
Address in PRM → EPCM check → Not current enclave → CPU BLOCK → #GP
```

All blocks happen **before memory is accessed** - hardware enforcement.
