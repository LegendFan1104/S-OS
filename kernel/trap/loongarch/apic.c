#include "types.h"
#include "param.h"
#include "mem/memlayout.h"
#include "platform.h"
#include "trap/apic.h"

#include <dev/pci/pci.h>
//
// the loongarch 7A1000 I/O Interrupt Controller Registers.
//

void
apic_init(void)
{
  *(volatile uint64*)(LS7A_INT_MASK_REG) = ~((0x1UL << UART0_IRQ) | (0x1UL << PCIE_IRQ));

  *(volatile uint64*)(LS7A_INT_EDGE_REG) = (0x1UL << UART0_IRQ) | (0x1UL << PCIE_IRQ);

  *(volatile uint8*)(LS7A_INT_HTMSI_VEC_REG + UART0_IRQ) = UART0_IRQ;
  *(volatile uint8*)(LS7A_INT_HTMSI_VEC_REG + PCIE_IRQ) = PCIE_IRQ;

  *(volatile uint64*)(LS7A_INT_POL_REG) = 0x0UL;

}

// tell the apic we've served this IRQ.
void
apic_complete(uint64 irq)

{
  *(volatile uint64*)(LS7A_INT_CLEAR_REG) = (irq);
}
