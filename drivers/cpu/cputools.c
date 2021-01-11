#include "boot.h"
#include "config.h"
#include "cpu.h"
#include "lib/time/timeManagement.h"

extern void cpuid(int op, int *eax, int *ebx, int *ecx, int *edx)
{
        __asm__("pushl %%ebx\n\t"
        "cpuid\n\t"
        "movl    %%ebx, %%esi\n\t"
        "popl    %%ebx\n\t"
                : "=a" (*eax),
                  "=S" (*ebx),
                  "=c" (*ecx),
                  "=d" (*edx)
                : "a" (op)
                : "cc");
}              

extern void intel_interrupts_on()
{
    unsigned long low, high;
    
   // printk("Disabling local apic...");
    
    /* this is so interrupts work. This is very limited scope --
     * linux will do better later, we hope ...
     */
    rdmsr(0x1b, low, high);
    low &= ~0x800;
    wrmsr(0x1b, low, high);
}

extern void cache_disable(void)
{
    unsigned int tmp;

    /* Disable cache */
    //printk("Disable Cache\n");

    /* Write back the cache and flush TLB */
    asm volatile ("movl  %%cr0, %0\n\t"
              "orl  $0x40000000, %0\n\t"
              "wbinvd\n\t"
              "movl  %0, %%cr0\n\t"
              "wbinvd\n\t"
              : "=r" (tmp) : : "memory");
}

extern void cache_enable(void)
{
    unsigned int tmp;

    asm volatile ("movl  %%cr0, %0\n\t"
              "andl  $0x9fffffff, %0\n\t"
              "movl  %0, %%cr0\n\t"
              :"=r" (tmp) : : "memory");

    //printk("Enable Cache\n");
}


//Thanks to anyone involved in XBMC for CPU freq code!
double RDTSC(void){
    unsigned long a, d;

    rdtsc(a,d);
    return (((unsigned long long)a) | (((unsigned long long)d) << 32));
}

extern unsigned long getCPUFreq(void){
    double Tcpu_fsb, Tcpu_result, Fcpu;
    unsigned int Twin_fsb, Twin_result;
    unsigned long finalResult;

    Tcpu_fsb = RDTSC();
    Twin_fsb = IoInputDword(0x8008);

    wait_ms(200); //TODO: check for consistency

    Tcpu_result = RDTSC();
    Twin_result = IoInputDword(0x8008);
    

    Fcpu = (Tcpu_result - Tcpu_fsb);
    Fcpu /= (Twin_result - Twin_fsb);
    Fcpu *= 3.375;
    finalResult = Fcpu;
    
    if(finalResult < 704)
        finalResult = 700;     //Trusty half speed
    else if(finalResult < 736)
        finalResult = 733;     //Normal CPU
    else if(finalResult < 750)
        finalResult = 740;     //DreamX half sped
    else if(finalResult < 1100)
        finalResult = 1000;    //1GHz CPU
    else if(finalResult < 1410)
        finalResult = 1400;    //Trusty 1.4GHz
    else
        finalResult = 1480;    //DreamX 1.48GHz    

    return finalResult;
}
