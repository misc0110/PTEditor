#include <stdio.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

#include "../ptedit_header.h"


int main(int argc, char** argv)
{
  if (ptedit_init()) {
      printf("Error: Could not initalize PTEditor, did you load the kernel module?\n");
      return 1;
  }
  
  unsigned char* addr = (unsigned char*) mmap(NULL,2*1024*1024, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  memset(addr,0x42,2*1024*1024);
  
  ptedit_entry_t entry = ptedit_resolve(addr, 0);
  ptedit_pte_clear_bit(addr, 0, PTEDIT_PAGE_BIT_ACCESSED);
  entry = ptedit_resolve(addr, 0);
  ptedit_print_entry(entry.pte);
  ptedit_print_entry(entry.pmd);
  ptedit_print_entry(entry.pud);
  ptedit_pte_set_bit(addr, 0, PTEDIT_PAGE_BIT_ACCESSED);
  entry = ptedit_resolve(addr, 0);
  ptedit_print_entry(entry.pte);
  ptedit_print_entry(entry.pmd);
  ptedit_print_entry(entry.pud);

  //set accessed bit of pd entry
  size_t address_pfn = ptedit_get_pfn(entry.pd);
  ptedit_pte_set_bit(addr, 0, PTEDIT_PAGE_BIT_ACCESSED);
  munmap(addr,4096);

  ptedit_cleanup();

  return 0;
}
