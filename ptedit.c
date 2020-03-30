#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include "module/pteditor.h"
#include "ptedit.h"

#define PTEDIT_COLOR_RED     "\x1b[31m"
#define PTEDIT_COLOR_GREEN   "\x1b[32m"
#define PTEDIT_COLOR_RESET   "\x1b[0m"

static int ptedit_fd;
static int ptedit_umem;
static int ptedit_pagesize;
static size_t ptedit_paging_root;
static unsigned char* ptedit_vmem;

typedef struct {
    int has_pgd, has_p4d, has_pud, has_pmd, has_pt;
    int pgd_entries, p4d_entries, pud_entries, pmd_entries, pt_entries;
    int page_offset;
} ptedit_paging_definition_t;

ptedit_paging_definition_t ptedit_paging_definition;



// ---------------------------------------------------------------------------
ptedit_entry_t ptedit_resolve_kernel(void* address, pid_t pid) {
    ptedit_entry_t vm;
    vm.vaddr = (size_t)address;
    vm.pid = (size_t)pid;
    ioctl(ptedit_fd, PTEDITOR_IOCTL_CMD_VM_RESOLVE, (size_t)&vm);
    return vm;
}

// ---------------------------------------------------------------------------
typedef size_t (*ptedit_phys_read_t)(size_t);

// ---------------------------------------------------------------------------
static inline size_t ptedit_phys_read_map(size_t address) {
    return *(size_t*)(ptedit_vmem + address);
}

// ---------------------------------------------------------------------------
static inline size_t ptedit_phys_read_pread(size_t address) {
    size_t val = 0;
    pread(ptedit_umem, &val, sizeof(size_t), address);
    return val;
}


// ---------------------------------------------------------------------------
static ptedit_entry_t ptedit_resolve_user_ext(void* address, pid_t pid, ptedit_phys_read_t deref) {
    size_t root = (pid == 0) ? ptedit_paging_root : ptedit_get_paging_root(pid);

    int pgdi, p4di, pudi, pmdi, pti;
    size_t addr = (size_t)address;
    pgdi = (addr >> (ptedit_paging_definition.page_offset 
                    + ptedit_paging_definition.pt_entries 
                    + ptedit_paging_definition.pmd_entries 
                    + ptedit_paging_definition.pud_entries 
                    + ptedit_paging_definition.p4d_entries)) % (1 << ptedit_paging_definition.pgd_entries);
    p4di = (addr >> (ptedit_paging_definition.page_offset 
                    + ptedit_paging_definition.pt_entries 
                    + ptedit_paging_definition.pmd_entries 
                    + ptedit_paging_definition.pud_entries)) % (1 << ptedit_paging_definition.p4d_entries);
    pudi = (addr >> (ptedit_paging_definition.page_offset 
                    + ptedit_paging_definition.pt_entries 
                    + ptedit_paging_definition.pmd_entries)) % (1 << ptedit_paging_definition.pud_entries);
    pmdi = (addr >> (ptedit_paging_definition.page_offset 
                    + ptedit_paging_definition.pt_entries)) % (1 << ptedit_paging_definition.pmd_entries);
    pti = (addr >> ptedit_paging_definition.page_offset) % (1 << ptedit_paging_definition.pt_entries);
    
    ptedit_entry_t resolved;
    memset(&resolved, 0, sizeof(resolved));
    resolved.vaddr = (size_t)address;
    resolved.pid = (size_t)pid;
    resolved.valid = 0;
    
    size_t pgd_entry, p4d_entry, pud_entry, pmd_entry, pt_entry;
    
//     printf("%zx + CR3(%zx) + PGDI(%zx) * 8 = %zx\n", ptedit_vmem, root, pgdi, ptedit_vmem + root + pgdi * sizeof(size_t));
    pgd_entry = deref(root + pgdi * sizeof(size_t));
    if(ptedit_cast(pgd_entry, ptedit_pgd_t).present != PTEDIT_PAGE_PRESENT) {
        return resolved;
    }
    resolved.pgd = pgd_entry;
    resolved.valid |= PTEDIT_VALID_MASK_PGD;
    if(ptedit_paging_definition.has_p4d) {    
        size_t pfn = (size_t)(ptedit_cast(pgd_entry, ptedit_pgd_t).pfn);
        p4d_entry = deref(pfn * ptedit_pagesize + p4di * sizeof(size_t));
        resolved.valid |= PTEDIT_VALID_MASK_P4D;
    } else {
        p4d_entry = pgd_entry;
    }
    resolved.p4d = p4d_entry;

    if(ptedit_cast(p4d_entry, ptedit_p4d_t).present != PTEDIT_PAGE_PRESENT) {
        return resolved;
    }

    
    if(ptedit_paging_definition.has_pud) {
        size_t pfn = (size_t)(ptedit_cast(p4d_entry, ptedit_p4d_t).pfn);
        pud_entry = deref(pfn * ptedit_pagesize + pudi * sizeof(size_t));
        resolved.valid |= PTEDIT_VALID_MASK_PUD;
    } else {
        pud_entry = p4d_entry;
    }
    resolved.pud = pud_entry;

    if(ptedit_cast(pud_entry, ptedit_pud_t).present != PTEDIT_PAGE_PRESENT) {
        return resolved;
    }
    
    if(ptedit_paging_definition.has_pmd) {
        size_t pfn = (size_t)(ptedit_cast(pud_entry, ptedit_pud_t).pfn);
        pmd_entry = deref(pfn * ptedit_pagesize + pmdi * sizeof(size_t));
        resolved.valid |= PTEDIT_VALID_MASK_PMD;
    } else {
        pmd_entry = pud_entry;
    }
    resolved.pmd = pmd_entry;

    if(ptedit_cast(pmd_entry, ptedit_pmd_t).present != PTEDIT_PAGE_PRESENT) {
        return resolved;
    }
    
#if defined(__i386__) || defined(__x86_64__)
    if(!ptedit_cast(pmd_entry, ptedit_pmd_t).size) {
#endif
        // normal 4kb page
        size_t pfn = (size_t)(ptedit_cast(pmd_entry, ptedit_pmd_t).pfn);
        pt_entry = deref(pfn * ptedit_pagesize + pti * sizeof(size_t)); //pt[pti];
        resolved.pte = pt_entry;
        resolved.valid |= PTEDIT_VALID_MASK_PTE;
        if(ptedit_cast(pt_entry, ptedit_pte_t).present != PTEDIT_PAGE_PRESENT) {
            return resolved;
        }
#if defined(__i386__) || defined(__x86_64__)
    }
#endif
    return resolved;
}


// ---------------------------------------------------------------------------
static ptedit_entry_t ptedit_resolve_user(void* address, pid_t pid) {
    return ptedit_resolve_user_ext(address, pid, ptedit_phys_read_pread);
}


// ---------------------------------------------------------------------------
static ptedit_entry_t ptedit_resolve_user_map(void* address, pid_t pid) {
    return ptedit_resolve_user_ext(address, pid, ptedit_phys_read_map);
}


// ---------------------------------------------------------------------------
void ptedit_update_kernel(void* address, pid_t pid, ptedit_entry_t* vm) {
    vm->vaddr = (size_t)address;
    vm->pid = (size_t)pid;
    ioctl(ptedit_fd, PTEDITOR_IOCTL_CMD_VM_UPDATE, (size_t)vm);
}

// ---------------------------------------------------------------------------
void ptedit_update_user(void* address, pid_t pid, ptedit_entry_t* vm) {
    size_t pgd[ptedit_pagesize / sizeof(size_t)], p4d[ptedit_pagesize / sizeof(size_t)], 
        pud[ptedit_pagesize / sizeof(size_t)], pmd[ptedit_pagesize / sizeof(size_t)], 
        pt[ptedit_pagesize / sizeof(size_t)];

    size_t root = (pid == 0) ? ptedit_paging_root : ptedit_get_paging_root(pid);
    ptedit_read_physical_page(root / ptedit_pagesize, (char *)pgd);

    int pgdi, p4di, pudi, pmdi, pti;
    size_t addr = (size_t)address;
    pgdi = (addr >> (ptedit_paging_definition.page_offset 
                    + ptedit_paging_definition.pt_entries 
                    + ptedit_paging_definition.pmd_entries 
                    + ptedit_paging_definition.pud_entries 
                    + ptedit_paging_definition.p4d_entries)) % (1 << ptedit_paging_definition.pgd_entries);
    p4di = (addr >> (ptedit_paging_definition.page_offset 
                    + ptedit_paging_definition.pt_entries 
                    + ptedit_paging_definition.pmd_entries 
                    + ptedit_paging_definition.pud_entries)) % (1 << ptedit_paging_definition.p4d_entries);
    pudi = (addr >> (ptedit_paging_definition.page_offset 
                    + ptedit_paging_definition.pt_entries 
                    + ptedit_paging_definition.pmd_entries)) % (1 << ptedit_paging_definition.pud_entries);
    pmdi = (addr >> (ptedit_paging_definition.page_offset 
                    + ptedit_paging_definition.pt_entries)) % (1 << ptedit_paging_definition.pmd_entries);
    pti = (addr >> ptedit_paging_definition.page_offset) % (1 << ptedit_paging_definition.pt_entries);
    
    size_t pgd_entry, p4d_entry, pud_entry, pmd_entry, pt_entry;
    int valid = 0;
    
    pgd_entry = pgd[pgdi];
    if(vm->valid & PTEDIT_VALID_MASK_PGD) pgd[pgdi] = vm->pgd;
    if(ptedit_cast(pgd_entry, ptedit_pgd_t).present != PTEDIT_PAGE_PRESENT) {
        goto update;
    }
    valid |= PTEDIT_VALID_MASK_PGD;
    
    if(ptedit_paging_definition.has_p4d) {    
        ptedit_read_physical_page(ptedit_get_pfn(pgd_entry), (char *)p4d);
        p4d_entry = p4d[p4di];
        if(vm->valid & PTEDIT_VALID_MASK_P4D) p4d[p4di] = vm->p4d;
        if(ptedit_cast(p4d_entry, ptedit_p4d_t).present != PTEDIT_PAGE_PRESENT) {
            goto update;
        }
    } else {
        p4d_entry = pgd_entry;
    }
    valid |= PTEDIT_VALID_MASK_P4D;

    if(ptedit_paging_definition.has_pud) {
        ptedit_read_physical_page(ptedit_get_pfn(p4d_entry), (char *)pud);
        pud_entry = pud[pudi];
        if(vm->valid & PTEDIT_VALID_MASK_PUD) pud[pudi] = vm->pud;
        if(ptedit_cast(pud_entry, ptedit_pud_t).present != PTEDIT_PAGE_PRESENT) {
            goto update;
        }
    } else {
        pud_entry = p4d_entry;
    }
    valid |= PTEDIT_VALID_MASK_PUD;
    
    if(ptedit_paging_definition.has_pmd) {
        ptedit_read_physical_page(ptedit_get_pfn(pud_entry), (char *)pmd);
        pmd_entry = pmd[pmdi];
        if(vm->valid & PTEDIT_VALID_MASK_PMD) pmd[pmdi] = vm->pmd;
        if(ptedit_cast(pmd_entry, ptedit_pmd_t).present != PTEDIT_PAGE_PRESENT) {
            goto update;
        }
    } else {
        pmd_entry = pud_entry;
    }
    valid |= PTEDIT_VALID_MASK_PMD;
    
#if defined(__i386__) || defined(__x86_64__)
    if(!ptedit_cast(pmd_entry, ptedit_pmd_t).size) {
#endif
        // normal 4kb page
        ptedit_read_physical_page(ptedit_get_pfn(pmd_entry), (char *)pt);
        pt_entry = pt[pti];
        if(vm->valid & PTEDIT_VALID_MASK_PTE) pt[pti] = vm->pte;
        if(ptedit_cast(pt_entry, ptedit_pte_t).present != PTEDIT_PAGE_PRESENT) {
            goto update;
        }
        valid |= PTEDIT_VALID_MASK_PTE;
#if defined(__i386__) || defined(__x86_64__)
    }
#endif
    
    update:
    if((vm->valid & PTEDIT_VALID_MASK_PTE) && (valid & PTEDIT_VALID_MASK_PTE)) {
        ptedit_write_physical_page(ptedit_get_pfn(pmd_entry), (char*)pt);
    }
    if((vm->valid & PTEDIT_VALID_MASK_PMD) && (valid & PTEDIT_VALID_MASK_PMD) && ptedit_paging_definition.has_pmd) {
        ptedit_write_physical_page(ptedit_get_pfn(pud_entry), (char*)pmd);
    }
    if((vm->valid & PTEDIT_VALID_MASK_PUD) && (valid & PTEDIT_VALID_MASK_PUD) && ptedit_paging_definition.has_pud) {
        ptedit_write_physical_page(ptedit_get_pfn(p4d_entry), (char*)pud);
    }
    if((vm->valid & PTEDIT_VALID_MASK_P4D) && (valid & PTEDIT_VALID_MASK_P4D) && ptedit_paging_definition.has_p4d) {
        ptedit_write_physical_page(ptedit_get_pfn(pgd_entry), (char*)p4d);
    }
    if((vm->valid & PTEDIT_VALID_MASK_PGD) && (valid & PTEDIT_VALID_MASK_PGD) && ptedit_paging_definition.has_pgd) {
        ptedit_write_physical_page(root / ptedit_pagesize, (char*)pgd);
    }
    
    ptedit_invalidate_tlb(address);
}

// ---------------------------------------------------------------------------
size_t ptedit_set_pfn(size_t pte, size_t pfn) {
#if defined(__i386__) || defined(__x86_64__)
  pte &= ~(((1ull << 40) - 1) << 12);
#elif defined(__aarch64__)
  pte &= ~(((1ull << 36) - 1) << 12);
#endif
  pte |= pfn << 12;
  return pte;
}


// ---------------------------------------------------------------------------
size_t ptedit_get_pfn(size_t pte) {
#if defined(__i386__) || defined(__x86_64__)
    return (pte & (((1ull << 40) - 1) << 12)) >> 12;
#elif defined(__aarch64__)
    return (pte & (((1ull << 36) - 1) << 12)) >> 12;
#endif
}


// ---------------------------------------------------------------------------
#define PTEDIT_B(val, bit) (!!((val) & (1ull << (bit))))

#define PEDIT_PRINT_B(fmt, bit)                                                \
  if ((bit)) {                                                                 \
    printf(PTEDIT_COLOR_GREEN);                                                       \
    printf((fmt), (bit));                                                      \
    printf(PTEDIT_COLOR_RESET);                                                       \
  } else {                                                                     \
    printf((fmt), (bit));                                                      \
  }                                                                            \
  printf("|");


// ---------------------------------------------------------------------------
void ptedit_print_entry_line(size_t entry, int line) {
#if defined(__i386__) || defined(__x86_64__)
  if(line == 0 || line == 3) printf("+--+------------------+-+-+-+-+-+-+-+-+--+--+-+-+-+\n");
  if(line == 1) printf("|NX|       PFN        |H|?|?|?|G|S|D|A|UC|WT|U|W|P|\n");
  if(line == 2) {
      printf("|");
      PEDIT_PRINT_B(" %d", PTEDIT_B(entry, PTEDIT_PAGE_BIT_NX));
      printf(" %16p |", (void *)((entry >> 12) & ((1ull << 40) - 1)));
      PEDIT_PRINT_B("%d", PTEDIT_B(entry, PTEDIT_PAGE_BIT_PAT_LARGE));
      PEDIT_PRINT_B("%d", PTEDIT_B(entry, PTEDIT_PAGE_BIT_SOFTW3));
      PEDIT_PRINT_B("%d", PTEDIT_B(entry, PTEDIT_PAGE_BIT_SOFTW2));
      PEDIT_PRINT_B("%d", PTEDIT_B(entry, PTEDIT_PAGE_BIT_SOFTW1));
      PEDIT_PRINT_B("%d", PTEDIT_B(entry, PTEDIT_PAGE_BIT_GLOBAL));
      PEDIT_PRINT_B("%d", PTEDIT_B(entry, PTEDIT_PAGE_BIT_PSE));
      PEDIT_PRINT_B("%d", PTEDIT_B(entry, PTEDIT_PAGE_BIT_DIRTY));
      PEDIT_PRINT_B("%d", PTEDIT_B(entry, PTEDIT_PAGE_BIT_ACCESSED));
      PEDIT_PRINT_B(" %d", PTEDIT_B(entry, PTEDIT_PAGE_BIT_PCD));
      PEDIT_PRINT_B(" %d", PTEDIT_B(entry, PTEDIT_PAGE_BIT_PWT));
      PEDIT_PRINT_B("%d", PTEDIT_B(entry, PTEDIT_PAGE_BIT_USER));
      PEDIT_PRINT_B("%d", PTEDIT_B(entry, PTEDIT_PAGE_BIT_RW));
      PEDIT_PRINT_B("%d", PTEDIT_B(entry, PTEDIT_PAGE_BIT_PRESENT));
      printf("\n");
  }
#elif defined(__aarch64__)
  if(line == 0 || line == 3) {
      printf("+--+--+--+---+-+--+------------------+--+-+-+-+--+---+-+\n");
  }
  if(line == 1) {
    printf("| ?| ?|XN|PXN|C| ?|        PFN       |NG|A|S|P|NS|MAI|T|\n");
  }
  if(line == 2) {
      printf("|");
      PEDIT_PRINT_B("%2d", (PTEDIT_B(entry, 63) << 4) | (PTEDIT_B(entry, 62) << 3) | (PTEDIT_B(entry, 61) << 2) | (PTEDIT_B(entry, 60) << 1) | PTEDIT_B(entry, 59));
      PEDIT_PRINT_B("%2d", (PTEDIT_B(entry, 58) << 3) | (PTEDIT_B(entry, 57) << 2) | (PTEDIT_B(entry, 56) << 1) | PTEDIT_B(entry, 55));
      PEDIT_PRINT_B(" %d", PTEDIT_B(entry, 54));
      PEDIT_PRINT_B(" %d ", PTEDIT_B(entry, 53));
      PEDIT_PRINT_B("%d", PTEDIT_B(entry, 52));
      PEDIT_PRINT_B("%2d", (PTEDIT_B(entry, 51) << 3) | (PTEDIT_B(entry, 50) << 2) | (PTEDIT_B(entry, 49) << 1) | PTEDIT_B(entry, 48));
      printf(" %16p |", (void *)((entry >> 12) & ((1ull << 36) - 1)));
      PEDIT_PRINT_B(" %d", PTEDIT_B(entry, 11));
      PEDIT_PRINT_B("%d", PTEDIT_B(entry, 10));
      PEDIT_PRINT_B("%d", (PTEDIT_B(entry, 9) << 1) | PTEDIT_B(entry, 8));
      PEDIT_PRINT_B("%d", (PTEDIT_B(entry, 7) << 1) | PTEDIT_B(entry, 6));
      PEDIT_PRINT_B(" %d", PTEDIT_B(entry, 5));
      PEDIT_PRINT_B(" %d ", (PTEDIT_B(entry, 4) << 2) | (PTEDIT_B(entry, 3) << 1) | PTEDIT_B(entry, 2));
      PEDIT_PRINT_B("%d", (PTEDIT_B(entry, 1) << 1) | PTEDIT_B(entry, 0));
      printf("\n");
  }
#endif
}


// ---------------------------------------------------------------------------
void ptedit_print_entry(size_t entry) {
    for(int i = 0; i < 4; i++) {
        ptedit_print_entry_line(entry, i);
    }
}

// ---------------------------------------------------------------------------
void ptedit_print_entry_t(ptedit_entry_t entry) {
  if(entry.valid & PTEDIT_VALID_MASK_PGD) {
    printf("PGD of address\n");
    ptedit_print_entry(entry.pgd);
  }
  if(entry.valid & PTEDIT_VALID_MASK_P4D) {
    printf("P4D of address\n");
    ptedit_print_entry(entry.p4d);
  }
  if(entry.valid & PTEDIT_VALID_MASK_PUD) {
    printf("PUD of address\n");
    ptedit_print_entry(entry.pud);
  }
  if(entry.valid & PTEDIT_VALID_MASK_PMD) {
    printf("PMD of address\n");
    ptedit_print_entry(entry.pmd);
  }
  if(entry.valid & PTEDIT_VALID_MASK_PTE) {
    printf("PTE of address\n");
    ptedit_print_entry(entry.pte);
  }
}

// ---------------------------------------------------------------------------
int ptedit_init() {
  ptedit_fd = open(PTEDITOR_DEVICE_PATH, O_RDONLY);
  if (ptedit_fd < 0) {
    fprintf(stderr, PTEDIT_COLOR_RED "[-]" PTEDIT_COLOR_RESET "Error: Could not open PTEditor device: %s\n", PTEDITOR_DEVICE_PATH);
    return -1;
  }
  ptedit_umem = open("/proc/umem", O_RDWR);
//   if(ptedit_umem > 0) {
//       ptedit_use_implementation(PTEDIT_IMPL_USER);
//   } else {
  ptedit_use_implementation(PTEDIT_IMPL_KERNEL);
//   }
  ptedit_pagesize = getpagesize();
#if defined(__i386__) || defined(__x86_64__)
    ptedit_paging_definition.has_pgd = 1;
    ptedit_paging_definition.has_p4d = 0;
    ptedit_paging_definition.has_pud = 1;
    ptedit_paging_definition.has_pmd = 1;
    ptedit_paging_definition.has_pt = 1;
    ptedit_paging_definition.pgd_entries = 9;
    ptedit_paging_definition.p4d_entries = 0;
    ptedit_paging_definition.pud_entries = 9;
    ptedit_paging_definition.pmd_entries = 9;
    ptedit_paging_definition.pt_entries = 9;
    ptedit_paging_definition.page_offset = 12;
    #elif defined(__aarch64__)
    ptedit_paging_definition.has_pgd = 1;
    ptedit_paging_definition.has_p4d = 0;
    ptedit_paging_definition.has_pud = 0;
    ptedit_paging_definition.has_pmd = 1;
    ptedit_paging_definition.has_pt = 1;
    ptedit_paging_definition.pgd_entries = 9;
    ptedit_paging_definition.p4d_entries = 0;
    ptedit_paging_definition.pud_entries = 0;
    ptedit_paging_definition.pmd_entries = 9;
    ptedit_paging_definition.pt_entries = 9;
    ptedit_paging_definition.page_offset = 12;
#endif
  return 0;
}


// ---------------------------------------------------------------------------
void ptedit_cleanup() {
   if(ptedit_fd >= 0) {
       close(ptedit_fd);
   }
   if(ptedit_umem > 0) {
       close(ptedit_umem);
   }
}


// ---------------------------------------------------------------------------
void ptedit_use_implementation(int implementation) {
    if(implementation == PTEDIT_IMPL_KERNEL) {
        ptedit_resolve = ptedit_resolve_kernel;
        ptedit_update = ptedit_update_kernel;
    } else if(implementation == PTEDIT_IMPL_USER_PREAD) {
        ptedit_resolve = ptedit_resolve_user;
        ptedit_update = ptedit_update_user;
        ptedit_paging_root = ptedit_get_paging_root(0);
    } else if(implementation == PTEDIT_IMPL_USER) {
        ptedit_resolve = ptedit_resolve_user_map;
        ptedit_update = ptedit_update_user;
        ptedit_paging_root = ptedit_get_paging_root(0);
        if(!ptedit_vmem) {
            ptedit_vmem = mmap(NULL, 32ull * 1024ull * 1024ull * 1024ull, PROT_READ, MAP_PRIVATE | MAP_NORESERVE, ptedit_umem, 0);
            fprintf(stderr, PTEDIT_COLOR_GREEN "[+]" PTEDIT_COLOR_RESET " Mapped physical memory to %p\n", ptedit_vmem);
        }
    } else {
        fprintf(stderr, PTEDIT_COLOR_RED "[-]" PTEDIT_COLOR_RESET " Error: PTEditor implementation not supported!\n");
    }
}


// ---------------------------------------------------------------------------
int ptedit_get_pagesize() {
  return (int)ioctl(ptedit_fd, PTEDITOR_IOCTL_CMD_GET_PAGESIZE, 0);
}


// ---------------------------------------------------------------------------
void ptedit_read_physical_page(size_t pfn, char* buffer) {
    if(ptedit_umem > 0) {
        pread(ptedit_umem, buffer, ptedit_pagesize, pfn * ptedit_pagesize);
    } else {
        ptedit_page_t page;
        page.buffer = (unsigned char*)buffer;
        page.pfn = pfn;
        ioctl(ptedit_fd, PTEDITOR_IOCTL_CMD_READ_PAGE, (size_t)&page);
    }
}


// ---------------------------------------------------------------------------
void ptedit_write_physical_page(size_t pfn, char* content) {
    if(ptedit_umem > 0) {
        pwrite(ptedit_umem, content, ptedit_pagesize, pfn * ptedit_pagesize);
    } else {
        ptedit_page_t page;
        page.buffer = (unsigned char*)content;
        page.pfn = pfn;
        ioctl(ptedit_fd, PTEDITOR_IOCTL_CMD_WRITE_PAGE, (size_t)&page);
    }
}


// ---------------------------------------------------------------------------
size_t ptedit_get_paging_root(pid_t pid) {
  ptedit_paging_t cr3;
  cr3.pid = (size_t)pid;
  cr3.root = 0;
  ioctl(ptedit_fd, PTEDITOR_IOCTL_CMD_GET_ROOT, (size_t)&cr3);
  return cr3.root;
}


// ---------------------------------------------------------------------------
void ptedit_set_paging_root(pid_t pid, size_t root) {
  ptedit_paging_t cr3;
  cr3.pid = (size_t)pid;
  cr3.root = root;
  ioctl(ptedit_fd, PTEDITOR_IOCTL_CMD_SET_ROOT, (size_t)&cr3);
}


// ---------------------------------------------------------------------------
void ptedit_invalidate_tlb(void* address) {
  ioctl(ptedit_fd, PTEDITOR_IOCTL_CMD_INVALIDATE_TLB, (size_t)address);
}


// ---------------------------------------------------------------------------
size_t ptedit_get_mts() {
    size_t mt;
    ioctl(ptedit_fd, PTEDITOR_IOCTL_CMD_GET_PAT, (size_t)&mt);
    return mt;
}


// ---------------------------------------------------------------------------
char ptedit_get_mt(unsigned char mt) {
    size_t mts = ptedit_get_mts();
#if defined(__i386__) || defined(__x86_64__)
    return ((mts >> (mt * 8)) & 7);
#elif defined(__aarch64__)
    return ((mts >> (mt * 8)) & 0xff);
#endif
}


// ---------------------------------------------------------------------------
const char* ptedit_mt_to_string(unsigned char mt) {
#if defined(__i386__) || defined(__x86_64__)
    const char* mts[] = {"UC", "WC", "Rsvd", "Rsvd", "WT", "WP", "WB", "UC-", "Rsvd"};
    if(mt <= 7) return mts[mt];
    return NULL;
#elif defined(__aarch64__)
    static char mts[16];
    int i;
    mts[0] = 0;
    for(i = 0; i < 2; i++) {
        strcat(mts, i == 0 ? "I" : "O");
        if((mt & 0xf) == ((mt >> 4) & 0xf)) strcpy(mts, "");
        switch((mt >> (i * 4)) & 0xf) {
            case 0:
                strcat(mts, "DM");
                break;
            case 1: /* Fall through */
            case 2: /* Fall through */
            case 3:
                strcat(mts, "WT");
                break;
            case 4:
                strcat(mts, "UC");
                break;
            case 5: /* Fall through */
            case 6: /* Fall through */
            case 7:
                strcat(mts, "WB");
                break;
            case 8: /* Fall through */
            case 9: /* Fall through */
            case 10: /* Fall through */
            case 11:
                strcat(mts, "WT");
                break;
            case 12: /* Fall through */
            case 13: /* Fall through */
            case 14: /* Fall through */
            case 15:
                strcat(mts, "WB");
        }
    }
    return mts;
#endif
}


// ---------------------------------------------------------------------------
void ptedit_set_mts(size_t mts) {
    ioctl(ptedit_fd, PTEDITOR_IOCTL_CMD_SET_PAT, mts);
}


// ---------------------------------------------------------------------------
void ptedit_set_mt(unsigned char mt, unsigned char value) {
    size_t mts = ptedit_get_mts();
#if defined(__i386__) || defined(__x86_64__)
    mts &=~(7 << (mt * 8));
#elif defined(__aarch64__)
    mts &=~(0xff << (mt * 8));
#endif
    mts |= (value << (mt * 8));
    ptedit_set_mts(mts);
}


// ---------------------------------------------------------------------------
unsigned char ptedit_find_mt(unsigned char type) {
    size_t mts = ptedit_get_mts();
    unsigned char found = 0;
    int i;
    for(i = 0; i < 8; i++) {
#if defined(__i386__) || defined(__x86_64__)
        if(((mts >> (i * 8)) & 7) == type) found |= (1 << i);
#elif defined(__aarch64__)
        if(((mts >> (i * 8)) & 0xff) == type) {
            found |= (1 << i);
        } else {
            unsigned char plow, phigh;
            plow = (mts >> (i * 8)) & 0xf;
            phigh = ((mts >> (i * 8)) >> 4) & 0xf;
            if((plow == phigh) && (plow == type)) {
                found |= (1 << i);
            }
        }
#endif
    }
    return found;
}


// ---------------------------------------------------------------------------
int ptedit_find_first_mt(unsigned char type) {
    return __builtin_ffs(ptedit_find_mt(type)) - 1;
}


// ---------------------------------------------------------------------------
size_t ptedit_apply_mt(size_t entry, unsigned char mt) {
#if defined(__i386__) || defined(__x86_64__)
    entry &=~((1ull << PTEDIT_PAGE_BIT_PWT) | (1ull << PTEDIT_PAGE_BIT_PCD) | (1ull << PTEDIT_PAGE_BIT_PAT));
    if(mt & 1) entry |= (1ull << PTEDIT_PAGE_BIT_PWT);
    if(mt & 2) entry |= (1ull << PTEDIT_PAGE_BIT_PCD);
    if(mt & 4) entry |= (1ull << PTEDIT_PAGE_BIT_PAT);
#elif defined(__aarch64__)
    entry &=~0x1c;
    entry |= (mt & 7) << 2;
#endif
    return entry;
}

// ---------------------------------------------------------------------------
unsigned char ptedit_extract_mt(size_t entry) {
#if defined(__i386__) || defined(__x86_64__)
    return (!!(entry & (1ull << PTEDIT_PAGE_BIT_PWT))) | ((!!(entry & (1ull << PTEDIT_PAGE_BIT_PCD))) << 1) | ((!!(entry & (1ull << PTEDIT_PAGE_BIT_PAT))) << 2);
#elif defined(__aarch64__)
    return (entry >> 2) & 7;
#endif
}

// ---------------------------------------------------------------------------
void ptedit_full_serializing_barrier() {
#if defined(__i386__) || defined(__x86_64__)
    asm volatile("mfence\nlfence\n" ::: "memory");
#elif defined(__aarch64__)
    asm volatile("DSB SY");
    asm volatile("DSB ISH");
    asm volatile("ISB");
#endif
    ptedit_set_paging_root(0, ptedit_get_paging_root(0));
#if defined(__i386__) || defined(__x86_64__)
    asm volatile("mfence\nlfence\n" ::: "memory");
#elif defined(__aarch64__)
    asm volatile("ISB");
    asm volatile("DSB ISH");
    asm volatile("DSB SY");
#endif
}


// ---------------------------------------------------------------------------
void ptedit_pte_set_bit(void* address, pid_t pid, int bit) {
   ptedit_entry_t vm = ptedit_resolve(address, pid);
   if(!(vm.valid & PTEDIT_VALID_MASK_PTE)) return;
   vm.pte |= (1ull << bit);
   vm.valid = PTEDIT_VALID_MASK_PTE;
   ptedit_update(address, pid, &vm);
}

// ---------------------------------------------------------------------------
void ptedit_pte_clear_bit(void* address, pid_t pid, int bit) {
   ptedit_entry_t vm = ptedit_resolve(address, pid);
   if(!(vm.valid & PTEDIT_VALID_MASK_PTE)) return;
   vm.pte &=~(1ull << bit);
   vm.valid = PTEDIT_VALID_MASK_PTE;
   ptedit_update(address, pid, &vm);
}

// ---------------------------------------------------------------------------
unsigned char ptedit_pte_get_bit(void* address, pid_t pid, int bit) {
   ptedit_entry_t vm = ptedit_resolve(address, pid);
   return !!(vm.pte & (1ull << bit));
}

// ---------------------------------------------------------------------------
size_t ptedit_pte_get_pfn(void* address, pid_t pid) {
   ptedit_entry_t vm = ptedit_resolve(address, pid);
   if(!(vm.valid & PTEDIT_VALID_MASK_PTE)) return 0;
   else return ptedit_get_pfn(vm.pte);
}

// ---------------------------------------------------------------------------
void ptedit_pte_set_pfn(void* address, pid_t pid, size_t pfn) {
   ptedit_entry_t vm = ptedit_resolve(address, pid);
   if(!(vm.valid & PTEDIT_VALID_MASK_PTE)) return;
   vm.pte = ptedit_set_pfn(vm.pte, pfn);
   vm.valid = PTEDIT_VALID_MASK_PTE;
   ptedit_update(address, pid, &vm);
}

