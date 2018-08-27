
#include <tilck/common/utils.h>
#include <tilck/common/string_util.h>
#include <tilck/common/arch/generic_x86/cpu_features.h>

#include <tilck/kernel/hal.h>
#include <tilck/kernel/paging.h>

bool enable_mttr(void)
{
   if (!x86_cpu_features.edx1.mtrr)
      return false;

   u64 mtrr_dt = rdmsr(MSR_IA32_MTRR_DEF_TYPE);

   if (!(mtrr_dt & (1 << 11))) {
      mtrr_dt |= (1 << 11);
      wrmsr(MSR_IA32_MTRR_DEF_TYPE, mtrr_dt);
   }

   return true;
}

int get_var_mttrs_count(void)
{
   if (!x86_cpu_features.edx1.mtrr)
      return 0;

   return rdmsr(MSR_IA32_MTRRCAP) & 255;
}

int get_free_mtrr(void)
{
   u32 var_mtrr_count = get_var_mttrs_count();
   u32 selected = 0;

   for (u32 i = 0; i < var_mtrr_count; i++, selected++) {

      u64 mask_reg = rdmsr(MSR_MTRRphysBase0 + 2 * i + 1);
      bool used = !!(mask_reg & (1 << 11));

      if (!used)
         return selected;
   }

   return -1;
}

void set_mtrr(int num, u64 paddr, u32 pow2size, u8 mem_type)
{
   ASSERT(num > 0);
   ASSERT(num < get_var_mttrs_count());
   ASSERT(pow2size > 0);
   ASSERT(roundup_next_power_of_2(pow2size) == pow2size);
   ASSERT(round_up_at64(paddr, pow2size) == paddr);
   ASSERT(x86_cpu_features.edx1.mtrr);

   u64 mask64 = ~((u64)pow2size - 1);
   u64 physBaseVal = ((u64)paddr & PAGE_MASK) | mem_type;
   u64 physMaskVal;

   physMaskVal = mask64 & ((1ull << x86_cpu_features.phys_addr_bits) - 1);
   physMaskVal |= (1 << 11); // valid = 1

   printk("round up size: %u\n", pow2size);
   printk("physBaseVal: %llx\n", physBaseVal);
   printk("physMaskVal: %llx\n", physMaskVal);

   wrmsr(MSR_MTRRphysBase0 + 2 * num, physBaseVal);
   wrmsr(MSR_MTRRphysBase0 + 2 * num + 1, physMaskVal);
}