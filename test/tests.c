#include "utest.h"
#include "../ptedit_header.h"
#include <time.h>

UTEST_STATE();

char __attribute__((aligned(4096))) page1[4096];
char __attribute__((aligned(4096))) page2[4096];
char __attribute__((aligned(4096))) scratch[4096];
char __attribute__((aligned(4096))) accessor[4096];

// =========================================================================
//                             Helper functions
// =========================================================================

size_t hrtime() {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return t1.tv_sec * 1000 * 1000 * 1000ULL + t1.tv_nsec;
}

typedef void (*access_time_callback_t)(void*);

int access_time_ext(void *ptr, size_t MEASUREMENTS, access_time_callback_t cb) {
  uint64_t start = 0, end = 0, sum = 0;

  for (int i = 0; i < MEASUREMENTS; i++) {
    start = hrtime();
    *((volatile size_t*)ptr);
    end = hrtime();
    sum += end - start;
    if(cb) cb(ptr);
  }

  return (int)(sum / MEASUREMENTS);
}

int access_time(void *ptr) {
  return access_time_ext(ptr, 1000000, NULL);
}

// =========================================================================
//                             Resolving addresses
// =========================================================================


UTEST(resolve, resolve_basic) {
    ptedit_entry_t vm = ptedit_resolve(page1, 0);
    ASSERT_TRUE(vm.pgd);
    ASSERT_TRUE(vm.pte);
    ASSERT_TRUE(vm.valid & PTEDIT_VALID_MASK_PTE);
    ASSERT_TRUE(vm.valid & PTEDIT_VALID_MASK_PGD);    
}

UTEST(resolve, resolve_valid_mask) {
    ptedit_entry_t vm = ptedit_resolve(page1, 0);
    if(vm.valid & PTEDIT_VALID_MASK_PGD) ASSERT_TRUE(vm.pgd);
    if(vm.valid & PTEDIT_VALID_MASK_P4D) ASSERT_TRUE(vm.p4d);
    if(vm.valid & PTEDIT_VALID_MASK_PMD) ASSERT_TRUE(vm.pmd);
    if(vm.valid & PTEDIT_VALID_MASK_PUD) ASSERT_TRUE(vm.pud);
    if(vm.valid & PTEDIT_VALID_MASK_PTE) ASSERT_TRUE(vm.pte);
}

UTEST(resolve, resolve_deterministic) {
    ptedit_entry_t vm1 = ptedit_resolve(page1, 0);
    ptedit_entry_t vm2 = ptedit_resolve(page1, 0);
    ASSERT_TRUE(!memcmp(&vm1, &vm2, sizeof(vm1)));
}

UTEST(resolve, resolve_different) {
    ptedit_entry_t vm1 = ptedit_resolve(page1, 0);
    ptedit_entry_t vm2 = ptedit_resolve(page2, 0);
    ASSERT_FALSE(!memcmp(&vm1, &vm2, sizeof(vm1)));
}

UTEST(resolve, resolve_invalid) {
    ptedit_entry_t vm1 = ptedit_resolve(0, 0);
    ASSERT_FALSE(vm1.valid & PTEDIT_VALID_MASK_PTE);
}

UTEST(resolve, resolve_invalid_pid) {
    ptedit_entry_t vm1 = ptedit_resolve(page1, -1);
    ASSERT_FALSE(vm1.valid);
}

UTEST(resolve, resolve_page_offset) {
    ptedit_entry_t vm1 = ptedit_resolve(page1, 0);
    ptedit_entry_t vm2 = ptedit_resolve(page1 + 1, 0);
    vm1.vaddr = vm2.vaddr = 0;
    ASSERT_TRUE(!memcmp(&vm1, &vm2, sizeof(vm1)));
    ptedit_entry_t vm3 = ptedit_resolve(page1 + 1024, 0);
    vm1.vaddr = vm3.vaddr = 0;
    ASSERT_TRUE(!memcmp(&vm1, &vm3, sizeof(vm1)));
    ptedit_entry_t vm4 = ptedit_resolve(page1 + 4095, 0);
    vm1.vaddr = vm4.vaddr = 0;
    ASSERT_TRUE(!memcmp(&vm1, &vm4, sizeof(vm1)));
}


// =========================================================================
//                             Updating addresses
// =========================================================================

UTEST(update, nop) {
    ptedit_entry_t vm1 = ptedit_resolve(scratch, 0);
    ASSERT_TRUE(vm1.valid);
    size_t valid = vm1.valid;
    vm1.valid = 0;
    ptedit_update(scratch, 0, &vm1);
    vm1.valid = valid;
    ptedit_entry_t vm2 = ptedit_resolve(scratch, 0);
    ASSERT_TRUE(!memcmp(&vm1, &vm1, sizeof(vm1)));
}

UTEST(update, pte_nop) {
    ptedit_entry_t vm1 = ptedit_resolve(scratch, 0);
    ASSERT_TRUE(vm1.valid);
    size_t valid = vm1.valid;
    vm1.valid = PTEDIT_VALID_MASK_PTE;
    ptedit_update(scratch, 0, &vm1);
    vm1.valid = valid;
    ptedit_entry_t vm2 = ptedit_resolve(scratch, 0);
    ASSERT_TRUE(!memcmp(&vm1, &vm1, sizeof(vm1)));
}

UTEST(update, new_pte) {
    ptedit_entry_t vm = ptedit_resolve(scratch, 0);
    ptedit_entry_t vm1 = ptedit_resolve(scratch, 0);
    ASSERT_TRUE(vm1.valid);
    size_t pte = vm1.pte;
    vm1.pte = ptedit_set_pfn(vm1.pte, 0x1234);
    vm1.valid = PTEDIT_VALID_MASK_PTE;
    ptedit_update(scratch, 0, &vm1);
    
    ptedit_entry_t check = ptedit_resolve(scratch, 0);
    ASSERT_NE((size_t)ptedit_cast(check.pte, ptedit_pte_t).pfn, ptedit_get_pfn(pte));
    ASSERT_EQ((size_t)ptedit_cast(check.pte, ptedit_pte_t).pfn, 0x1234);
    
    vm1.valid = PTEDIT_VALID_MASK_PTE;
    vm1.pte = pte;
    ptedit_update(scratch, 0, &vm1);
    
    ptedit_entry_t vm2 = ptedit_resolve(scratch, 0);
    ASSERT_TRUE(!memcmp(&vm, &vm2, sizeof(vm)));
}

// =========================================================================
//                                  PTEs
// =========================================================================

UTEST(pte, get_pfn) {
    ptedit_entry_t vm = ptedit_resolve(page1, 0);
    ASSERT_EQ(ptedit_get_pfn(vm.pte), (size_t)ptedit_cast(vm.pte, ptedit_pte_t).pfn);
}

UTEST(pte, get_pte_pfn) {
    ptedit_entry_t vm = ptedit_resolve(page1, 0);
    ASSERT_EQ(ptedit_pte_get_pfn(page1, 0), (size_t)ptedit_cast(vm.pte, ptedit_pte_t).pfn);
}

UTEST(pte, get_pte_pfn_invalid) {
    ASSERT_FALSE(ptedit_pte_get_pfn(0, 0));
}

UTEST(pte, pte_present) {
    ptedit_entry_t vm = ptedit_resolve(page1, 0);
    ASSERT_EQ((size_t)ptedit_cast(vm.pte, ptedit_pte_t).present, PTEDIT_PAGE_PRESENT);
}

UTEST(pte, pte_set_pfn_basic) {
    size_t entry = 0;
    ASSERT_EQ(entry, ptedit_set_pfn(entry, 0));
    ASSERT_NE(entry, ptedit_set_pfn(entry, 1));
    ASSERT_EQ(entry, ptedit_set_pfn(ptedit_set_pfn(entry, 1234), 0));
    ASSERT_GT(ptedit_set_pfn(entry, 2), ptedit_set_pfn(entry, 1));
    entry = -1ull;
    ASSERT_NE(0, ptedit_set_pfn(entry, 0));
}

UTEST(pte, pte_set_pfn) {
    ASSERT_TRUE(accessor[0] == 2);
    size_t accessor_pfn = ptedit_pte_get_pfn(accessor, 0);
    ASSERT_TRUE(accessor_pfn);
    size_t page1_pfn = ptedit_pte_get_pfn(page1, 0);
    ASSERT_TRUE(page1_pfn);
    size_t page2_pfn = ptedit_pte_get_pfn(page2, 0);
    ASSERT_TRUE(page2_pfn);
    ptedit_pte_set_pfn(accessor, 0, page1_pfn);
    ASSERT_TRUE(accessor[0] == 0);
    ptedit_pte_set_pfn(accessor, 0, page2_pfn);
    ASSERT_TRUE(accessor[0] == 1);
    ptedit_pte_set_pfn(accessor, 0, accessor_pfn);
    ASSERT_TRUE(accessor[0] == 2);
}


// =========================================================================
//                             Physical Pages
// =========================================================================

UTEST(page, read) {
    char buffer[4096];
    size_t pfn = ptedit_pte_get_pfn(page1, 0);
    ASSERT_TRUE(pfn);
    ptedit_read_physical_page(pfn, buffer);
    ASSERT_TRUE(!memcmp(buffer, page1, sizeof(buffer)));
    pfn = ptedit_pte_get_pfn(page2, 0);
    ASSERT_TRUE(pfn);
    ptedit_read_physical_page(pfn, buffer);
    ASSERT_TRUE(!memcmp(buffer, page2, sizeof(buffer)));
}

UTEST(page, write) {
    char buffer[4096];
    size_t pfn = ptedit_pte_get_pfn(scratch, 0);
    ASSERT_TRUE(pfn);
    ptedit_write_physical_page(pfn, page1);
    ptedit_read_physical_page(pfn, buffer);
    ASSERT_TRUE(!memcmp(page1, buffer, sizeof(buffer)));
    ptedit_write_physical_page(pfn, page2);
    ptedit_read_physical_page(pfn, buffer);
    ASSERT_TRUE(!memcmp(page2, buffer, sizeof(buffer)));
}

// =========================================================================
//                                Paging
// =========================================================================

UTEST(paging, get_root) {
    size_t root = ptedit_get_paging_root(0);
    ASSERT_TRUE(root);
}

UTEST(paging, get_root_deterministic) {
    size_t root = ptedit_get_paging_root(0);
    ASSERT_TRUE(root);
    size_t root_check = ptedit_get_paging_root(0);
    ASSERT_EQ(root, root_check);   
}

UTEST(paging, get_root_invalid_pid) {
    size_t root = ptedit_get_paging_root(-1);
    ASSERT_FALSE(root);
}

UTEST(paging, root_page_aligned) {
    size_t root = ptedit_get_paging_root(0);
    ASSERT_TRUE(root);
    ASSERT_FALSE(root % ptedit_get_pagesize());
}

UTEST(paging, correct_root) {
    size_t buffer[4096 / sizeof(size_t)];
    size_t root = ptedit_get_paging_root(0);
    ptedit_read_physical_page(root / ptedit_get_pagesize(), (char*)buffer);
    ptedit_entry_t vm = ptedit_resolve(0, 0);
    ASSERT_EQ(vm.pgd, buffer[0]);
}

// =========================================================================
//                               Memory Types
// =========================================================================

UTEST(memtype, get) {
    ASSERT_TRUE(ptedit_get_mts());
}

UTEST(memtype, get_deterministic) {
    ASSERT_EQ(ptedit_get_mts(), ptedit_get_mts());
}

UTEST(memtype, uncachable) {
    ASSERT_NE(ptedit_find_first_mt(PTEDIT_MT_UC), -1);
}

UTEST(memtype, writeback) {
    ASSERT_NE(ptedit_find_first_mt(PTEDIT_MT_WB), -1);
}

UTEST(memtype, find_first) {
    ASSERT_TRUE(ptedit_get_mt(ptedit_find_first_mt(PTEDIT_MT_UC)) == PTEDIT_MT_UC);
    ASSERT_TRUE(ptedit_get_mt(ptedit_find_first_mt(PTEDIT_MT_WB)) == PTEDIT_MT_WB);
}

UTEST(memtype, apply) {
    size_t entry = 0;
    ASSERT_NE(ptedit_apply_mt(entry, 1), entry);
    ASSERT_EQ(ptedit_apply_mt(entry, 0), entry);
}

UTEST(memtype, extract) {
    ASSERT_TRUE(ptedit_extract_mt(ptedit_apply_mt(0, 5)) == 5);
    ASSERT_TRUE(ptedit_extract_mt(ptedit_apply_mt(-1ull, 2)) == 2);
}

UTEST(memtype, uncachable_access_time) {
    int uc_mt = ptedit_find_first_mt(PTEDIT_MT_UC);
    ASSERT_NE(uc_mt, -1);
    int wb_mt = ptedit_find_first_mt(PTEDIT_MT_WB);
    ASSERT_NE(wb_mt, -1);
    
    int before = access_time(scratch);
    
    ptedit_entry_t entry = ptedit_resolve(scratch, 0);
    size_t pte = entry.pte;
    ASSERT_TRUE(entry.valid);
    ASSERT_TRUE(entry.pte);
    entry.pte = ptedit_apply_mt(entry.pte, uc_mt);
    entry.valid = PTEDIT_VALID_MASK_PTE;
    ptedit_update(scratch, 0, &entry);   
    
    int uc = access_time(scratch);
    
    entry.pte = pte;
    entry.valid = PTEDIT_VALID_MASK_PTE;
    ptedit_update(scratch, 0, &entry);   
    
    int after = access_time(scratch);
    ASSERT_LT(after + 5, uc);
    ASSERT_LT(before + 5, uc);
}

// =========================================================================
//                               TLB
// =========================================================================


UTEST(tlb, access_time) {
    int flushed = access_time_ext(scratch, 100, ptedit_invalidate_tlb);
    int normal = access_time_ext(scratch, 100, NULL);
    ASSERT_GT(flushed, normal);
}



int main(int argc, const char *const argv[]) {
    if(ptedit_init()) {
        printf("Could not initialize PTEditor, did you load the kernel module?\n");
        return 1;
    }
    memset(scratch, 0, sizeof(page1));
    memset(page1, 0, sizeof(page1));
    memset(page2, 1, sizeof(page2));
    memset(accessor, 2, sizeof(page2));
    
//     ptedit_use_implementation(PTEDIT_IMPL_USER_PREAD);
    
    int result = utest_main(argc, argv);
    
    ptedit_cleanup();
    return result;
}
