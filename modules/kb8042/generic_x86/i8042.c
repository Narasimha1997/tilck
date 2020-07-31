/* SPDX-License-Identifier: BSD-2-Clause */

#include <tilck/common/basic_defs.h>
#include <tilck/common/printk.h>

#include <tilck/kernel/irq.h>
#include <tilck/kernel/sched.h>

#include "i8042.h"

#define KB_ITERS_TIMEOUT   10000


/* Hack!!! See pic_io_wait() */
static NO_INLINE void kb_io_wait(void)
{
   if (in_hypervisor())
      return;

   for (int i = 0; i < 1000; i++)
      asmVolatile("nop");
}

static bool kb_wait_cmd_fetched(void)
{
   for (int i = 0; !kb_ctrl_is_read_for_next_cmd(); i++) {

      if (i >= KB_ITERS_TIMEOUT)
         return false;

      kb_io_wait();
   }

   return true;
}

static NODISCARD bool kb_wait_for_data(void)
{
   for (int i = 0; !kb_ctrl_is_pending_data(); i++) {

      if (i >= KB_ITERS_TIMEOUT)
         return false;

      kb_io_wait();
   }

   return true;
}

void kb_drain_any_data(void)
{
   while (kb_ctrl_is_pending_data()) {
      inb(KB_DATA_PORT);
      kb_io_wait();
   }
}

void kb_drain_data_no_check(void)
{
   for (int i = 0; i < 16; i++)
      inb(KB_DATA_PORT);
}

static NODISCARD bool kb_ctrl_send_cmd(u8 cmd)
{
   if (!kb_wait_cmd_fetched())
      return false;

   outb(KB_COMMAND_PORT, cmd);

   if (!kb_wait_cmd_fetched())
      return false;

   return true;
}

static NODISCARD bool kb_ctrl_send_cmd_and_wait_response(u8 cmd)
{
   if (!kb_ctrl_send_cmd(cmd))
      return false;

   if (!kb_wait_for_data())
      return false;

   return true;
}

static NODISCARD bool kb_ctrl_full_wait(void)
{
   u8 ctrl;
   u32 iters = 0;

   do
   {
      if (iters > KB_ITERS_TIMEOUT)
         return false;

      ctrl = inb(KB_STATUS_PORT);

      if (ctrl & KB_STATUS_OUTPUT_FULL) {
         inb(KB_DATA_PORT); /* drain the KB's output */
      }

      iters++;
      kb_io_wait();

   } while (ctrl & (KB_STATUS_INPUT_FULL | KB_STATUS_OUTPUT_FULL));

   return true;
}

static NODISCARD bool kb_ctrl_disable_ports(void)
{
   irq_set_mask(X86_PC_KEYBOARD_IRQ);

   if (!kb_ctrl_full_wait())
      return false;

   if (!kb_ctrl_send_cmd(KB_CTRL_CMD_PORT1_DISABLE))
      return false;

   if (!kb_ctrl_send_cmd(KB_CTRL_CMD_PORT2_DISABLE))
      return false;

   if (!kb_ctrl_full_wait())
      return false;

   return true;
}

static NODISCARD bool kb_ctrl_enable_ports(void)
{
   if (!kb_ctrl_full_wait())
      return false;

   if (!kb_ctrl_send_cmd(KB_CTRL_CMD_PORT1_ENABLE))
      return false;

   if (!kb_ctrl_send_cmd(KB_CTRL_CMD_PORT2_ENABLE))
      return false;

   if (!kb_ctrl_full_wait())
      return false;

   kb_drain_data_no_check();
   irq_clear_mask(X86_PC_KEYBOARD_IRQ);
   return true;
}

bool kb_led_set(u8 val)
{
   if (!kb_ctrl_disable_ports()) {
      printk("KB: kb_ctrl_disable_ports() fail\n");
      return false;
   }

   if (!kb_ctrl_full_wait()) goto err;
   outb(KB_DATA_PORT, 0xED);
   if (!kb_ctrl_full_wait()) goto err;
   outb(KB_DATA_PORT, val & 7);
   if (!kb_ctrl_full_wait()) goto err;

   if (!kb_ctrl_enable_ports()) {
      printk("KB: kb_ctrl_enable_ports() fail\n");
      return false;
   }

   return true;

err:
   printk("kb_led_set() failed: timeout in kb_ctrl_full_wait()\n");
   return false;
}

/*
 * From http://wiki.osdev.org/PS/2_Keyboard
 *
 * BITS [0..4]: Repeat rate (00000b = 30 Hz, ..., 11111b = 2 Hz)
 * BITS [5..6]: Delay before keys repeat (00b = 250 ms, ..., 11b = 1000 ms)
 * BIT  [7]: Must be zero
 *
 * Note: this function sets just the repeat rate.
 */

bool kb_set_typematic_byte(u8 val)
{
   if (!kb_ctrl_disable_ports()) {
      printk("kb_set_typematic_byte() failed: kb_ctrl_disable_ports() fail\n");
      return false;
   }

   if (!kb_ctrl_full_wait()) goto err;
   outb(KB_DATA_PORT, 0xF3);
   if (!kb_ctrl_full_wait()) goto err;
   outb(KB_DATA_PORT, val & 0b11111);
   if (!kb_ctrl_full_wait()) goto err;

   if (!kb_ctrl_enable_ports()) {
      printk("kb_set_typematic_byte() failed: kb_ctrl_enable_ports() fail\n");
      return false;
   }

   return true;

err:
   printk("kb_set_typematic_byte() failed: timeout in kb_ctrl_full_wait()\n");
   return false;
}

NODISCARD bool kb_ctrl_self_test(void)
{
   u8 res, resend_count = 0;
   bool success = false;

   if (!kb_ctrl_disable_ports())
      goto out;

   do {

      if (resend_count >= 3)
         break;

      if (!kb_ctrl_send_cmd_and_wait_response(KB_CTRL_CMD_SELFTEST))
         goto out;

      res = inb(KB_DATA_PORT);
      resend_count++;

   } while (res == KB_RESPONSE_RESEND);

   if (res == KB_RESPONSE_SELF_TEST_OK)
      success = true;

out:
   if (!kb_ctrl_enable_ports())
      success = false;

   return success;
}

NODISCARD bool kb_ctrl_reset(void)
{
   u8 res;
   u8 kb_ctrl;
   u8 resend_count = 0;
   bool success = false;

   if (!kb_ctrl_disable_ports())
      goto out;

   kb_ctrl = inb(KB_STATUS_PORT);

   printk("KB: reset procedure\n");
   printk("KB: initial status: 0x%x\n", kb_ctrl);
   printk("KB: sending 0xFF (reset) to the controller\n");

   if (!kb_ctrl_send_cmd_and_wait_response(KB_CTRL_CMD_RESET))
      goto out;

   do {

      if (resend_count >= 3)
         break;

      res = inb(KB_DATA_PORT);
      printk("KB: response: 0x%x\n", res);
      resend_count++;

   } while (res == KB_RESPONSE_RESEND);

   if (res != KB_RESPONSE_ACK) {

      if (res == KB_RESPONSE_BAT_OK)
         success = true;

      goto out;
   }

   /* We got an ACK, now wait for the success/failure of the reset itself */

   if (!kb_wait_for_data())
      goto out;

   res = inb(KB_DATA_PORT);
   printk("KB: response: 0x%x\n", res);

   if (res == KB_RESPONSE_BAT_OK)
      success = true;

out:
   if (!kb_ctrl_enable_ports())
      success = false;

   printk("KB: reset success: %u\n", success);
   return success;
}

bool kb_ctrl_read_ctr_and_cto(u8 *ctr, u8 *cto)
{
   bool ok = true;
   ASSERT(are_interrupts_enabled());

   if (!kb_ctrl_disable_ports()) {
      printk("KB: disable ports failed\n");
      ok = false;
      goto out;
   }

   if (ctr) {
      if (kb_ctrl_send_cmd_and_wait_response(KB_CTRL_CMD_READ_CTR)) {
         *ctr = inb(KB_DATA_PORT);
      } else {
         printk("KB: send cmd failed\n");
         ok = false;
      }
   }

   if (cto) {
      if (kb_ctrl_send_cmd_and_wait_response(KB_CTRL_CMD_READ_CTO)) {
         *cto = inb(KB_DATA_PORT);
      } else {
         printk("KB: send cmd failed\n");
         ok = false;
      }
   }

out:
   if (!kb_ctrl_enable_ports()) {
      printk("KB: enable ports failed\n");
      ok = false;
   }

   return ok;
}

// Reboot procedure using the 8042 PS/2 controller
void x86_pc_8042_reboot(void)
{
   disable_interrupts_forced(); /* Disable the interrupts before rebooting */

   if (!kb_ctrl_send_cmd(KB_CTRL_CMD_CPU_RESET))
      panic("Unable to reboot using the 8042 controller: timeout in send cmd");

   while (true) {
      halt();
   }
}