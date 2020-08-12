/* SPDX-License-Identifier: BSD-2-Clause */

#include <tilck/common/basic_defs.h>
#include <tilck/common/printk.h>
#include <tilck/common/utils.h>

#include <tilck/kernel/sched.h>
#include <tilck/kernel/paging.h>
#include <tilck/kernel/system_mmap.h>
#include <tilck/kernel/hal.h>

#include <3rd_party/acpi/acpi.h>
#include <3rd_party/acpi/accommon.h>

#define _COMPONENT      ACPI_OS_SERVICES
ACPI_MODULE_NAME("osl_mm")

void *
AcpiOsMapMemory(
    ACPI_PHYSICAL_ADDRESS   Where,
    ACPI_SIZE               Length)
{
   ACPI_PHYSICAL_ADDRESS paddr = Where & PAGE_MASK;
   void *va;
   size_t pg_count;
   size_t cnt;

   ACPI_FUNCTION_TRACE(__FUNC__);

   if (Where + Length <= LINEAR_MAPPING_SIZE)
      return_PTR(KERNEL_PA_TO_VA(Where));

   if (!(va = hi_vmem_reserve(Length)))
      return_PTR(NULL);

   Length = Where + Length - paddr;
   pg_count = pow2_round_up_at(Length, PAGE_SIZE) >> PAGE_SHIFT;
   cnt = map_pages(get_kernel_pdir(), va, paddr, pg_count, PAGING_FL_RW);

   if (cnt < pg_count) {
      unmap_pages_permissive(get_kernel_pdir(), va, cnt, false);
      hi_vmem_release(va, Length);
      return_PTR(NULL);
   }

   printk("ACPI: mmap %zu pages %p -> %p\n", pg_count, TO_PTR(Where), va);
   return_PTR(va);
}

void
AcpiOsUnmapMemory(
    void                    *LogicalAddr,
    ACPI_SIZE               Size)
{
   ulong vaddr = (ulong)LogicalAddr;
   ulong aligned_vaddr = vaddr & PAGE_MASK;
   size_t pg_count;

   ACPI_FUNCTION_TRACE(__FUNC__);

   if (vaddr + Size <= LINEAR_MAPPING_END)
      return_VOID;

   Size = vaddr + Size - aligned_vaddr;
   pg_count = pow2_round_up_at(Size, PAGE_SIZE) >> PAGE_SHIFT;
   printk("ACPI: release %zu pages mapped at %p\n", pg_count, LogicalAddr);
   unmap_pages(get_kernel_pdir(), TO_PTR(aligned_vaddr), pg_count, false);
   hi_vmem_release(LogicalAddr, Size);
}

ACPI_STATUS
AcpiOsGetPhysicalAddress(
    void                    *LogicalAddress,
    ACPI_PHYSICAL_ADDRESS   *PhysicalAddress)
{
   ulong paddr;
   ACPI_FUNCTION_TRACE(__FUNC__);

   if (!LogicalAddress || !PhysicalAddress)
      return_ACPI_STATUS(AE_BAD_PARAMETER);

   if (get_mapping2(get_kernel_pdir(), LogicalAddress, &paddr) < 0)
      return_ACPI_STATUS(AE_ERROR);

   *PhysicalAddress = paddr;
   return_ACPI_STATUS(AE_OK);
}

BOOLEAN
AcpiOsReadable(
    void                    *Pointer,
    ACPI_SIZE               Length)
{
   ulong va = (ulong)Pointer;
   ulong va_end = va + Length;
   ACPI_FUNCTION_TRACE(__FUNC__);

   if (va < KERNEL_BASE_VA)
      return_UINT8(false);

   if (va_end <= LINEAR_MAPPING_END)
      return_UINT8(true);

   while (va < va_end) {

      if (!is_mapped(get_kernel_pdir(), TO_PTR(va)))
         return_UINT8(false);

      va += PAGE_SIZE;
   }

   return_UINT8(true);
}

BOOLEAN
AcpiOsWritable(
    void                    *Pointer,
    ACPI_SIZE               Length)
{
   ulong va = (ulong)Pointer;
   ulong va_end = va + Length;
   struct mem_region m;
   int reg_count = get_mem_regions_count();
   ACPI_FUNCTION_TRACE(__FUNC__);

   if (va < KERNEL_BASE_VA)
      return_UINT8(false);

   for (int i = 0; i < reg_count; i++) {

      get_mem_region(i, &m);

      if (m.type != MULTIBOOT_MEMORY_AVAILABLE)
         continue;

      if (~m.extra & MEM_REG_EXTRA_KERNEL)
         continue;

      if (~m.extra & MEM_REG_EXTRA_RAMDISK)
         continue;

      /* OK, now `m` points to a kernel/ramdisk region */
      if (IN_RANGE(va, m.addr, m.addr + m.len)) {

         /*
          * The address falls inside a read/write protected region.
          * We cannot allow ACPICA to believe it's writable.
          */

         return_UINT8(false);
      }
   }

   if (va_end <= LINEAR_MAPPING_END)
      return_UINT8(true);

   while (va < va_end) {

      if (!is_rw_mapped(get_kernel_pdir(), TO_PTR(va)))
         return_UINT8(false);

      va += PAGE_SIZE;
   }

   return_UINT8(true);
}

ACPI_STATUS
AcpiOsReadMemory(
    ACPI_PHYSICAL_ADDRESS   Address,
    UINT64                  *Value,
    UINT32                  Width)
{
   void *va;
   ACPI_FUNCTION_TRACE(__FUNC__);

   if ((Address + (Width >> 3)) > LINEAR_MAPPING_SIZE) {

      /*
       * In order to support this, we'll need to implement some sort of
       * memory mapping cache. Mapping and un-mapping a page for a single
       * read/write is definitively unaccetable.
       */

      NOT_IMPLEMENTED();

   } else {
      va = KERNEL_PA_TO_VA(Address);
   }

   switch (Width) {
      case 8:
         *Value = *(volatile u8 *)va;
         break;
      case 16:
         *Value = *(volatile u16 *)va;
         break;
      case 32:
         *Value = *(volatile u32 *)va;
         break;
      case 64:
         *Value = *(volatile u64 *)va;
         break;
      default:
         return_ACPI_STATUS(AE_BAD_PARAMETER);
   }

   return_ACPI_STATUS(AE_OK);
}

ACPI_STATUS
AcpiOsWriteMemory(
    ACPI_PHYSICAL_ADDRESS   Address,
    UINT64                  Value,
    UINT32                  Width)
{
   void *va;
   ACPI_FUNCTION_TRACE(__FUNC__);

   if ((Address + (Width >> 3)) > LINEAR_MAPPING_SIZE) {

      /* See the comment in AcpiOsReadMemory() */
      NOT_IMPLEMENTED();

   } else {
      va = KERNEL_PA_TO_VA(Address);
   }

   switch (Width) {
      case 8:
         *(volatile u8 *)va = Value;
         break;
      case 16:
         *(volatile u16 *)va = Value;
         break;
      case 32:
         *(volatile u32 *)va = Value;
         break;
      case 64:
         *(volatile u64 *)va = Value;
         break;
      default:
         return_ACPI_STATUS(AE_BAD_PARAMETER);
   }

   return_ACPI_STATUS(AE_OK);
}

ACPI_STATUS
AcpiOsReadPort(
    ACPI_IO_ADDRESS         Address,
    UINT32                  *Value,
    UINT32                  Width)
{
   u16 ioport = (u16)Address;
   ACPI_FUNCTION_TRACE(__FUNC__);

   if (Address > 0xffff)
      return_ACPI_STATUS(AE_NOT_EXIST);

   switch (Width) {
      case 8:
         *Value = (u32)inb(ioport);
         break;
      case 16:
         *Value = (u32)inw(ioport);
         break;
      case 32:
         *Value = (u32)inl(ioport);
         break;
      default:
         return_ACPI_STATUS(AE_BAD_PARAMETER);
   }

   return_ACPI_STATUS(AE_OK);
}

ACPI_STATUS
AcpiOsWritePort(
    ACPI_IO_ADDRESS         Address,
    UINT32                  Value,
    UINT32                  Width)
{
   u16 ioport = (u16)Address;
   ACPI_FUNCTION_TRACE(__FUNC__);

   if (Address > 0xffff)
      return_ACPI_STATUS(AE_NOT_EXIST);

   switch (Width) {
      case 8:
         outb(ioport, (u8)Value);
         break;
      case 16:
         outw(ioport, (u16)Value);
         break;
      case 32:
         outl(ioport, (u32)Value);
         break;
      default:
         return_ACPI_STATUS(AE_BAD_PARAMETER);
   }

   return_ACPI_STATUS(AE_OK);
}
