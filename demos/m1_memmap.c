#include <stdio.h>

#include "../ptedit_header.h"

int is_present(size_t entry) {
  return (entry & 3) == 3;
}

void dump(int do_dump, size_t entry, char *type) {
  if (do_dump) {
    for (int i = 0; i < 4; i++) {
      printf("%s", type);
      ptedit_print_entry_line(entry, i);
    }
  }
}

int main(int argc, char *argv[]) {
  if (ptedit_init()) {
    printf("Error: Could not initalize PTEditor, did you load the kernel module?\n");
    return 1;
  }

  int dump_entry = 1;
  size_t pid = 0;
  if (argc >= 2) {
    pid = atoi(argv[1]);
  }

  printf("Dumping PID %zd\n", pid);

  size_t root = ptedit_get_paging_root(pid);
  size_t pagesize = ptedit_get_pagesize();
  size_t pml4[pagesize / sizeof(size_t)], pdpt[pagesize / sizeof(size_t)],
      pd[pagesize / sizeof(size_t)], pt[pagesize / sizeof(size_t)];

  ptedit_read_physical_page((root / pagesize)*4, (char *)pml4);

  int pml4i, pdpti, pdi, pti;
  size_t mem_usage = 0;

  /* Iterate through PML4 entries */
  for (pml4i = 0; pml4i < 2048; pml4i++) {
    size_t pml4_entry = pml4[pml4i];
    if (!is_present(pml4_entry))
      continue;
    dump(dump_entry, pml4_entry, "");

    /* Iterate through PDPT entries */
    ptedit_read_physical_page(ptedit_get_pfn(pml4_entry), (char *)pdpt);
    for (pdpti = 0; pdpti < 2048; pdpti++) {
      size_t pdpt_entry = pdpt[pdpti];
      if (!is_present(pdpt_entry))
        continue;
      dump(dump_entry, pdpt_entry, "PDPT");

      /* Iterate through PD entries */
      ptedit_read_physical_page(ptedit_get_pfn(pdpt_entry), (char *)pd);
      for (pdi = 0; pdi < 2048; pdi++) {
        size_t pd_entry = pd[pdi];
        if (!is_present(pd_entry))
          continue;
        dump(dump_entry, pd_entry, "    PD  ");

        /* Iterate through PT entries */
        ptedit_read_physical_page(ptedit_get_pfn(pd_entry), (char *)pt);
        for (pti = 0; pti < 2048; pti++) {
          size_t pt_entry = pt[pti];
          if (!is_present(pt_entry))
            continue;
          dump(dump_entry, pt_entry, "        PT  ");
          // only certain entries are addressable on m1 and it depends on the page directory root
          mem_usage += 16384;
        }
      }
    }
  }

  printf("Used memory: %zd KB\n", mem_usage / 1024);

  ptedit_cleanup();
}
