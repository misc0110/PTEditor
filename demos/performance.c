#include <stdio.h>
#include <stdint.h>
#include <memory.h>
#include "../ptedit_header.h"

#define COLOR_RED "\x1b[31m"
#define COLOR_GREEN "\x1b[32m"
#define COLOR_YELLOW "\x1b[33m"
#define COLOR_RESET "\x1b[0m"

#define TAG_OK COLOR_GREEN "[+]" COLOR_RESET " "
#define TAG_FAIL COLOR_RED "[-]" COLOR_RESET " "
#define TAG_PROGRESS COLOR_YELLOW "[~]" COLOR_RESET " "

unsigned long target;

int is_normal_page(size_t entry) {
#if defined(__i386__) || defined(__x86_64__)
  return !(entry & (1ull << PTEDIT_PAGE_BIT_PSE));
#elif defined(__aarch64__)
  return 1;
#endif
}

uint64_t rdtsc() {
  uint64_t a, d;
  asm volatile("mfence");
  asm volatile("rdtsc" : "=a"(a), "=d"(d));
  a = (d << 32) | a;
  asm volatile("mfence");
  return a;
}

#define REPEAT 10000

int main(int argc, char *argv[]) {
    if (ptedit_init()) {
      printf(TAG_FAIL "Error: Could not initalize PTEditor, did you load the kernel module?\n");
      return 1;
    }

    target = 'X';
    size_t phys = 0;

    int i;
    ptedit_entry_t entry, entry_us;
    uint64_t start, stop;
    
    ptedit_use_implementation(PTEDIT_IMPL_KERNEL);
    start = rdtsc();
    for(i = 0; i < REPEAT; i++) {
        entry = ptedit_resolve(&target, 0);
    }
    stop = rdtsc();
    printf(TAG_OK "Kernel implementation takes " COLOR_YELLOW "%d" COLOR_RESET " cycles/resolve\n", (int)((stop - start) / REPEAT));
    
    ptedit_use_implementation(PTEDIT_IMPL_USER);
    start = rdtsc();
    for(i = 0; i < REPEAT; i++) {
        entry_us = ptedit_resolve(&target, 0);
    }
    stop = rdtsc();
    printf(TAG_OK "User implementation takes " COLOR_YELLOW "%d" COLOR_RESET " cycles/resolve\n", (int)((stop - start) / REPEAT));

    if(memcmp(&entry, &entry_us, sizeof(entry))) {
        printf(TAG_FAIL "Kernel and user-space resolver do not agree!\n");
    }
    
    ptedit_use_implementation(PTEDIT_IMPL_USER_PREAD);
    start = rdtsc();
    for(i = 0; i < REPEAT; i++) {
        entry_us = ptedit_resolve(&target, 0);
    }
    stop = rdtsc();
    printf(TAG_OK "User (pread) implementation takes " COLOR_YELLOW "%d" COLOR_RESET " cycles/resolve\n", (int)((stop - start) / REPEAT));

    if(memcmp(&entry, &entry_us, sizeof(entry))) {
        printf(TAG_FAIL "Kernel and user-space resolver do not agree!\n");
    }
    
    
    if(is_normal_page(entry.pd)) {
        printf(TAG_PROGRESS "Page is 4KB\n");
        ptedit_print_entry(entry.pte);
        phys = (ptedit_get_pfn(entry.pte) << 12) | (((size_t)&target) & 0xfff);
    } else {
        printf(TAG_PROGRESS "Page is 2MB\n");
        ptedit_print_entry(entry.pd);
        phys = (ptedit_get_pfn(entry.pd) << 21) | (((size_t)&target) & 0x1fffff);
    }

    printf(TAG_OK "Virtual address: %p\n", &target);
    printf(TAG_OK "Physical address: 0x%zx\n", phys);

    ptedit_cleanup();

    printf(TAG_OK "Done\n");
}
