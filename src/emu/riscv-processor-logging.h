//
//  riscv-processor-logging.h
//

#ifndef riscv_processor_logging_h
#define riscv_processor_logging_h

namespace riscv {

	/* Processor logging flags */

	enum {
		proc_log_inst =            1<<0,       /* Log instructions */
		proc_log_operands =        1<<1,       /* Log instruction operands */
		proc_log_memory =          1<<2,       /* Log memory mapping information */
		proc_log_mmio =            1<<3,       /* Log memory mapped IO */
		proc_log_csr_mmode =       1<<4,       /* Log machine status and control registers */
		proc_log_csr_hmode =       1<<5,       /* Log hypervisor status and control registers */
		proc_log_csr_smode =       1<<6,       /* Log supervisor status and control registers */
		proc_log_csr_umode =       1<<7,       /* Log user status and control registers */
		proc_log_int_reg =         1<<8,       /* Log integer registers */
		proc_log_trap =            1<<9,       /* Log processor traps */
		proc_log_pagewalk =        1<<10,      /* Log virtual memory page walks */
		proc_log_no_pseudo =       1<<11       /* Don't decode pseudoinstructions */
	};

}

#endif