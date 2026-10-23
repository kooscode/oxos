/* The kernel library's code in a section of its own, so the harness can tell kernel instructions from the C
   library's and its own (asynchronous interrupts are delivered only inside kernel code: host/hostcpu.cpp). */
SECTIONS
{
  .oxos_kernel_text : {
    __oxos_kernel_text_start = .;
    *liboxos_core.a:*(.text .text.*)
    __oxos_kernel_text_end = .;
  }
}
INSERT BEFORE .text;
