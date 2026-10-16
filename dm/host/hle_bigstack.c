// 大栈入口（hle.h HLE_FORCE_BIG）：接管函数直接跑在调用它的客体线程栈上，而游戏给线程的栈可以很小——
// 超级马力欧 3D 世界 + 狂怒世界的存档线程只有 8 KB（CreateThread 栈顶 0x…9fe0、崩在其下 0x21a0），它调 fs::MountSaveData 时，
// 宿主的挂载路径（h_fs_MountSave 两个 1 KB 级路径缓冲 + mount_ex 两个 1100 字节 + snprintf）越过栈底，SIGBUS 在 __vfprintf 里。
// 真机上这一调用只是一次 IPC，几百字节栈就够；宿主的文件系统接管按 PATH_MAX 开缓冲，一次打开文件就要 6–7 KB（-fstack-usage：OpenFile 1.3 KB → host_path_ex 1.9 KB → path_component 3.2 KB）。
// 做法：登记的是一段静态桩（每个接管函数一段，取实现地址进 x16 后跳到 dm_bigstack_enter），入口看当前 sp：在客体地址块里（客体线程栈）就换到本线程的宿主栈（512 KB，首用时映射，带保护页），
// 原样带着 x0–x8 / d0–d7 调实现，返回后换回原栈；不在客体块里（宿主线程、已在宿主栈上）直接跳过去。只转寄存器参数：参数超过 8 个整数 / 8 个浮点、可变参数从栈上取的函数不能用它。
// iOS 不许运行时生成可执行代码，所以桩在编译期由宏生成，不像 DM_HLECOUNT 那样运行时写
// Big-stack entry (hle.h HLE_FORCE_BIG): a take-over function runs directly on the stack of the guest thread that calls it, and the stack a game gives a thread can be small --
// the save thread of Super Mario 3D World + Bowser's Fury has only 8 KB (CreateThread stack top 0x…9fe0, crash 0x21a0 below it); when it calls fs::MountSaveData,
// the host's mount path (two 1 KB-class path buffers in h_fs_MountSave + two 1100-byte ones in mount_ex + snprintf) ran past the stack bottom, SIGBUS in __vfprintf.
// On hardware this call is one IPC that needs a few hundred bytes of stack; the host's file system take-overs size their buffers by PATH_MAX, and one file open takes 6-7 KB (-fstack-usage: OpenFile 1.3 KB -> host_path_ex 1.9 KB -> path_component 3.2 KB).
// How: what gets registered is a static stub (one per take-over function, which loads the implementation's address into x16 and jumps to dm_bigstack_enter). The entry looks at the current sp: inside the guest address block (a guest thread stack)
// it switches to this thread's host stack (512 KB, mapped on first use, with a guard page), calls the implementation with x0-x8 / d0-d7 untouched, and switches back after it returns; outside the guest block (a host thread, or already on the host stack) it jumps straight through.
// Only register arguments are carried over: functions with more than 8 integer / 8 floating-point arguments, or variadic ones reading from the stack, cannot use it.
// iOS does not allow generating executable code at run time, so the stubs are generated at compile time by the macro, not written at run time as DM_HLECOUNT does
#include <pthread.h>
#include <stdint.h>
#include <sys/mman.h>
#include "dm.h"

int dm_guest_as_contains(uintptr_t a);   // hle_svc.c

#define BS_SIZE (512u << 10)
#define BS_GUARD 16384u
static pthread_key_t g_bs_key;
static pthread_once_t g_bs_once = PTHREAD_ONCE_INIT;
static void bs_free(void *top) { if (top) munmap((uint8_t *)top - BS_SIZE - BS_GUARD, BS_SIZE + BS_GUARD); }
static void bs_init(void) { if (pthread_key_create(&g_bs_key, bs_free)) die("大栈 pthread_key_create"); }

// sp 在客体地址块里 → 本线程宿主栈的栈顶（16 字节对齐）；否则 0（不换栈）
// sp inside the guest address block -> the top of this thread's host stack (16-byte aligned); otherwise 0 (no switch)
uintptr_t dm_bigstack_top(uintptr_t sp);
uintptr_t dm_bigstack_top(uintptr_t sp) {
    if (!dm_guest_as_contains(sp)) return 0;
    pthread_once(&g_bs_once, bs_init);
    uint8_t *top = pthread_getspecific(g_bs_key);
    if (!top) {
        uint8_t *m = mmap(NULL, BS_SIZE + BS_GUARD, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m == MAP_FAILED) { LOG("【红】大栈映射失败，本次留在客体栈上"); return 0; }
        mprotect(m, BS_GUARD, PROT_NONE);   // 保护页：宿主栈自己用穿时崩在这里，而不是悄悄写坏别的映射 | guard page: running off the host stack itself crashes here instead of quietly corrupting another mapping
        top = m + BS_GUARD + BS_SIZE;
        pthread_setspecific(g_bs_key, top);
    }
    return (uintptr_t)top;
}

// x16 = 实现。帧（在原栈上，0xa0 字节）：+0x00 x0–x8、x16，+0x50 d0–d7，+0x90 x29 / x30。换栈后新栈顶存原 sp，实现返回后取回
// x16 = implementation. Frame (on the original stack, 0xa0 bytes): +0x00 x0-x8, x16, +0x50 d0-d7, +0x90 x29 / x30. After the switch the original sp is stored at the new stack top and restored when the implementation returns
__asm__(
    ".text\n"
    ".p2align 2\n"
    ".globl _dm_bigstack_enter\n"
    ".private_extern _dm_bigstack_enter\n"
    "_dm_bigstack_enter:\n"
    "    sub sp, sp, #0xa0\n"
    "    stp x29, x30, [sp, #0x90]\n"
    "    add x29, sp, #0x90\n"
    "    stp x0, x1, [sp, #0x00]\n"
    "    stp x2, x3, [sp, #0x10]\n"
    "    stp x4, x5, [sp, #0x20]\n"
    "    stp x6, x7, [sp, #0x30]\n"
    "    stp x8, x16, [sp, #0x40]\n"
    "    stp d0, d1, [sp, #0x50]\n"
    "    stp d2, d3, [sp, #0x60]\n"
    "    stp d4, d5, [sp, #0x70]\n"
    "    stp d6, d7, [sp, #0x80]\n"
    "    mov x0, sp\n"
    "    bl _dm_bigstack_top\n"
    "    mov x17, x0\n"
    "    ldp x0, x1, [sp, #0x00]\n"
    "    ldp x2, x3, [sp, #0x10]\n"
    "    ldp x4, x5, [sp, #0x20]\n"
    "    ldp x6, x7, [sp, #0x30]\n"
    "    ldp x8, x16, [sp, #0x40]\n"
    "    ldp d0, d1, [sp, #0x50]\n"
    "    ldp d2, d3, [sp, #0x60]\n"
    "    ldp d4, d5, [sp, #0x70]\n"
    "    ldp d6, d7, [sp, #0x80]\n"
    "    cbz x17, 1f\n"
    "    mov x9, sp\n"
    "    mov sp, x17\n"
    "    stp x9, xzr, [sp, #-16]!\n"
    "    blr x16\n"
    "    ldr x9, [sp]\n"
    "    mov sp, x9\n"
    "    ldp x29, x30, [sp, #0x90]\n"
    "    add sp, sp, #0xa0\n"
    "    ret\n"
    "1:\n"
    "    ldp x29, x30, [sp, #0x90]\n"
    "    add sp, sp, #0xa0\n"
    "    br x16\n");
