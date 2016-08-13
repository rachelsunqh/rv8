//
//  riscv-test-emulate.cc
//

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cinttypes>
#include <cstdarg>
#include <cerrno>
#include <cmath>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include <deque>
#include <map>

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "riscv-types.h"
#include "riscv-endian.h"
#include "riscv-format.h"
#include "riscv-meta.h"
#include "riscv-util.h"
#include "riscv-cmdline.h"
#include "riscv-codec.h"
#include "riscv-elf.h"
#include "riscv-elf-file.h"
#include "riscv-elf-format.h"
#include "riscv-strings.h"
#include "riscv-disasm.h"
#include "riscv-processor.h"
#include "riscv-fpu.h"
#include "riscv-bits.h"
#include "riscv-pte.h"
#include "riscv-memory.h"
#include "riscv-cache.h"
#include "riscv-mmu.h"
#include "riscv-interp.h"
#include "riscv-unknown-abi.h"

#if defined (ENABLE_GPERFTOOL)
#include "gperftools/profiler.h"
#endif

using namespace riscv;

/*
 * Flat address space user-mode logging processor decorator
 *
 * This template is parameterized with a processor from riscv-processor.h
 *
 *   riscv_processor_rv32ima
 *   riscv_processor_rv64ima
 *   riscv_processor_rv32imafd
 *   riscv_processor_rv64imafd
 */

template<typename T, typename P>
struct riscv_processor_base : P
{
	typedef T decode_type;
	typedef P processor_type;

	std::vector<std::pair<void*,size_t>> mapped_segments;

	uintptr_t heap_begin;
	uintptr_t heap_end;

	bool log_registers;
	bool log_instructions;

	riscv_processor_base() :
		P(),
		mapped_segments(),
		heap_begin(0),
		heap_end(0),
		log_registers(false),
		log_instructions(false)
	{}

	std::string format_inst(uintptr_t pc)
	{
		char buf[20];
		uintptr_t inst_length;
		uint64_t inst = riscv_inst_fetch(pc, &inst_length);
		switch (inst_length) {
			case 2:  snprintf(buf, sizeof(buf), "0x%08tx", inst); break;
			case 4:  snprintf(buf, sizeof(buf), "0x%08tx", inst); break;
			case 6:  snprintf(buf, sizeof(buf), "0x%012tx", inst); break;
			case 8:  snprintf(buf, sizeof(buf), "0x%016tx", inst); break;
			default: snprintf(buf, sizeof(buf), "(invalid)"); break;
		}
		return buf;
	}

	void print_disassembly(T &dec)
	{
		static const char *fmt_32 = "core %3zu: 0x%08tx (%s) %-30s\n";
		static const char *fmt_64 = "core %3zu: 0x%016tx (%s) %-30s\n";
		static const char *fmt_128 = "core %3zu: 0x%032tx (%s) %-30s\n";
		std::string args = riscv_disasm_inst_simple(dec);
		printf(P::xlen == 32 ? fmt_32 : P::xlen == 64 ? fmt_64 : fmt_128,
			P::hart_id, uintptr_t(P::pc), format_inst(P::pc).c_str(), args.c_str());
	}

	void print_int_regeisters()
	{
		for (size_t i = riscv_ireg_x0; i < P::ireg_count; i++) {
			char fmt[32];
			snprintf(fmt, sizeof(fmt), "%%-4s: 0x%%0%u%sx%%s",
				(P::xlen >> 2), P::xlen == 64 ? "ll" : "");
			printf(fmt, riscv_ireg_name_sym[i],
				P::ireg[i].r.xu.val, (i + 1) % 4 == 0 ? "\n" : " ");
		}
	}

	void print_f32_regeisters()
	{
		for (size_t i = riscv_freg_f0; i < P::freg_count; i++) {
			printf("%-4s: s %16.5f%s", riscv_freg_name_sym[i],
				P::freg[i].r.s.val, (i + 1) % 4 == 0 ? "\n" : " ");
		}
	}

	void print_f64_regeisters()
	{
		for (size_t i = riscv_freg_f0; i < P::freg_count; i++) {
			printf("%-4s: d %16.5f%s", riscv_freg_name_sym[i],
				P::freg[i].r.d.val, (i + 1) % 4 == 0 ? "\n" : " ");
		}
	}
};


/* Decode and Exec template parameters */

#define RV_32  /*rv32*/true,  /*rv64*/false
#define RV_64  /*rv32*/false, /*rv64*/true

#define RV_IMA    /*I*/true, /*M*/true, /*A*/true, /*S*/true, /*F*/false,/*D*/false,/*C*/false
#define RV_IMAC   /*I*/true, /*M*/true, /*A*/true, /*S*/true, /*F*/false,/*D*/false,/*C*/true
#define RV_IMAFD  /*I*/true, /*M*/true, /*A*/true, /*S*/true, /*F*/true, /*D*/true, /*C*/false
#define RV_IMAFDC /*I*/true, /*M*/true, /*A*/true, /*S*/true, /*F*/true, /*D*/true, /*C*/true


/* RV32 Partial processor specialization templates (RV32IMA, RV32IMAC, RV32IMAFD, RV32IMAFDC) */

template <typename T>
struct riscv_processor_rv32ima_unit : riscv_processor_base<T,riscv_processor_rv32ima>
{
	void decode_inst(T &dec, uint64_t inst) {
		riscv_decode_inst<T,RV_32,RV_IMA>(dec, inst);
	}

	bool exec_inst(T &dec, size_t inst_length) {
		return rv32_exec<RV_IMA>(dec, *this, inst_length);
	}
};

template <typename T>
struct riscv_processor_rv32imac_unit : riscv_processor_base<T,riscv_processor_rv32ima>
{
	void decode_inst(T &dec, uint64_t inst) {
		riscv_decode_inst<T,RV_32,RV_IMAC>(dec, inst);
		riscv_decompress_inst_rv32<T>(dec);
	}

	bool exec_inst(T &dec, size_t inst_length) {
		return rv32_exec<RV_IMAC>(dec, *this, inst_length);
	}
};

template <typename T>
struct riscv_processor_rv32imafd_unit : riscv_processor_base<T,riscv_processor_rv32imafd>
{
	void decode_inst(T &dec, uint64_t inst) {
		riscv_decode_inst<T,RV_32,RV_IMAFD>(dec, inst);
	}

	bool exec_inst(T &dec, size_t inst_length) {
		return rv32_exec<RV_IMAFD>(dec, *this, inst_length);
	}
};

template <typename T>
struct riscv_processor_rv32imafdc_unit : riscv_processor_base<T,riscv_processor_rv32imafd>
{
	void decode_inst(T &dec, uint64_t inst) {
		riscv_decode_inst<T,RV_32,RV_IMAFDC>(dec, inst);
		riscv_decompress_inst_rv32<T>(dec);
	}

	bool exec_inst(T &dec, size_t inst_length) {
		return rv32_exec<RV_IMAFDC>(dec, *this, inst_length);
	}
};


/* RV64 Partial processor specialization templates (RV64IMA, RV64IMAC, RV64IMAFD, RV64IMAFDC) */

template <typename T>
struct riscv_processor_rv64ima_unit : riscv_processor_base<T,riscv_processor_rv64ima>
{
	void decode_inst(T &dec, uint64_t inst) {
		riscv_decode_inst<T,RV_64,RV_IMA>(dec, inst);
	}

	bool exec_inst(T &dec, size_t inst_length) {
		return rv64_exec<RV_IMA>(dec, *this, inst_length);
	}
};

template <typename T>
struct riscv_processor_rv64imac_unit : riscv_processor_base<T,riscv_processor_rv64ima>
{
	void decode_inst(T &dec, uint64_t inst) {
		riscv_decode_inst<T,RV_64,RV_IMAC>(dec, inst);
		riscv_decompress_inst_rv64<T>(dec);
	}

	bool exec_inst(T &dec, size_t inst_length) {
		return rv64_exec<RV_IMAC>(dec, *this, inst_length);
	}
};

template <typename T>
struct riscv_processor_rv64imafd_unit : riscv_processor_base<T,riscv_processor_rv64imafd>
{
	void decode_inst(T &dec, uint64_t inst) {
		riscv_decode_inst<T,RV_64,RV_IMAFD>(dec, inst);
	}

	bool exec_inst(T &dec, size_t inst_length) {
		return rv64_exec<RV_IMAFD>(dec, *this, inst_length);
	}
};

template <typename T>
struct riscv_processor_rv64imafdc_unit : riscv_processor_base<T,riscv_processor_rv64imafd>
{
	void decode_inst(T &dec, uint64_t inst) {
		riscv_decode_inst<T,RV_64,RV_IMAFDC>(dec, inst);
		riscv_decompress_inst_rv64<T>(dec);
	}

	bool exec_inst(T &dec, size_t inst_length) {
		return rv64_exec<RV_IMAFDC>(dec, *this, inst_length);
	}
};


/* Simple processor stepper with instruction cache that delegates ecall to an abi proxy */

template <typename P>
struct riscv_proxy_runner : P
{
	static const size_t inst_cache_size = 8191;

	struct riscv_inst_cache_ent
	{
		uint64_t inst;
		typename P::decode_type dec;
	};

	riscv_inst_cache_ent inst_cache[inst_cache_size];

	bool step(size_t count)
	{
		typename P::decode_type dec;
		size_t i = 0, inst_length;
		uint64_t inst;
		while (i < count) {
			inst = riscv_inst_fetch(P::pc, &inst_length);
			uint64_t inst_cache_key = inst % inst_cache_size;
			if (inst_cache[inst_cache_key].inst == inst) {
				dec = inst_cache[inst_cache_key].dec;
			} else {
				P::decode_inst(dec, inst);
				inst_cache[inst_cache_key].inst = inst;
				inst_cache[inst_cache_key].dec = dec;
			}
			if (P::log_registers) P::print_int_regeisters();
			if (P::log_instructions) P::print_disassembly(dec);
			if (P::exec_inst(dec, inst_length)) continue;
			if (dec.op == riscv_op_ecall) {
				proxy_syscall(*this);
				P::pc += inst_length;
				continue;
			}
			debug("illegal instruciton: pc=0x%tx inst=%s",
				uintptr_t(P::pc), P::format_inst(P::pc).c_str());
			return false;
		}
		return true;
	}
};


/* Parameterized kernel proxy processor models */

using riscv_processor_proxy_rv32ima = riscv_proxy_runner<riscv_processor_rv32ima_unit<riscv_decode>>;
using riscv_processor_proxy_rv32imac = riscv_proxy_runner<riscv_processor_rv32imac_unit<riscv_decode>>;
using riscv_processor_proxy_rv32imafd = riscv_proxy_runner<riscv_processor_rv32imafd_unit<riscv_decode>>;
using riscv_processor_proxy_rv32imafdc = riscv_proxy_runner<riscv_processor_rv32imafdc_unit<riscv_decode>>;
using riscv_processor_proxy_rv64ima = riscv_proxy_runner<riscv_processor_rv64ima_unit<riscv_decode>>;
using riscv_processor_proxy_rv64imac = riscv_proxy_runner<riscv_processor_rv64imac_unit<riscv_decode>>;
using riscv_processor_proxy_rv64imafd = riscv_proxy_runner<riscv_processor_rv64imafd_unit<riscv_decode>>;
using riscv_processor_proxy_rv64imafdc = riscv_proxy_runner<riscv_processor_rv64imafdc_unit<riscv_decode>>;


/* Emulator */

struct riscv_emulator
{
	/*
		Simple ABI/AEE RISC-V emulator that uses a machine generated interpreter
		created by parse-meta using the C-psuedo code in meta/instructions

		Currently only a small number of syscalls are implemented

		(ABI) application binary interface
		(AEE) application execution environment
	*/

	static const size_t stack_top =  0x78000000; // 1920 MiB
	static const size_t stack_size = 0x01000000; //   16 MiB

	elf_file elf;
	std::string filename;

	bool memory_debug = false;
	bool emulator_debug = false;
	bool log_registers = false;
	bool log_instructions = false;
	bool help_or_error = false;

	enum rv_isa {
		rv_isa_none,
		rv_isa_ima,
		rv_isa_imac,
		rv_isa_imafd,
		rv_isa_imafdc,
	} ext = rv_isa_imafdc;

	static rv_isa decode_isa_ext(std::string isa_ext)
	{
		if (strncasecmp(isa_ext.c_str(), "IMA", isa_ext.size()) == 0) return rv_isa_ima;
		else if (strncasecmp(isa_ext.c_str(), "IMAC", isa_ext.size()) == 0) return rv_isa_imac;
		else if (strncasecmp(isa_ext.c_str(), "IMAFD", isa_ext.size()) == 0) return rv_isa_imafd;
		else if (strncasecmp(isa_ext.c_str(), "IMAFDC", isa_ext.size()) == 0) return rv_isa_imafdc;
		else return rv_isa_none;
	}

	static const int elf_p_flags_mmap(int v)
	{
		int prot = 0;
		if (v & PF_X) prot |= PROT_EXEC;
		if (v & PF_W) prot |= PROT_WRITE;
		if (v & PF_R) prot |= PROT_READ;
		return prot;
	}

	/* Map a single stack segment into user address space */
	template <typename P>
	void map_stack(P &proc, uintptr_t stack_top, uintptr_t stack_size)
	{
		void *addr = mmap((void*)(stack_top - stack_size), stack_size,
			PROT_READ | PROT_WRITE, MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		if (addr == MAP_FAILED) {
			panic("map_stack: error: mmap: %s", strerror(errno));
		}

		/* keep track of the mapped segment and set the stack_top */
		proc.mapped_segments.push_back(std::pair<void*,size_t>((void*)(stack_top - stack_size), stack_size));
		proc.ireg[riscv_ireg_sp] = stack_top - 0x8;

		if (emulator_debug) {
			debug("sp : mmap: 0x%016" PRIxPTR " - 0x%016" PRIxPTR " +R+W",
				(stack_top - stack_size), stack_top);
		}
	}

	/* Map ELF load segments into user address space */
	template <typename P>
	void map_load_segment(P &proc, const char* filename, Elf64_Phdr &phdr)
	{
		int fd = open(filename, O_RDONLY);
		if (fd < 0) {
			panic("map_executable: error: open: %s: %s", filename, strerror(errno));
		}
		void *addr = mmap((void*)phdr.p_vaddr, phdr.p_memsz,
			elf_p_flags_mmap(phdr.p_flags), MAP_FIXED | MAP_PRIVATE, fd, phdr.p_offset);
		close(fd);
		if (addr == MAP_FAILED) {
			panic("map_executable: error: mmap: %s: %s", filename, strerror(errno));
		}

		/* keep track of the mapped segment and set the heap_end */
		proc.mapped_segments.push_back(std::pair<void*,size_t>((void*)phdr.p_vaddr, phdr.p_memsz));
		uintptr_t seg_end = uintptr_t(phdr.p_vaddr + phdr.p_memsz);
		if (proc.heap_begin < seg_end) proc.heap_begin = proc.heap_end = seg_end;

		if (emulator_debug) {
			debug("elf: mmap: 0x%016" PRIxPTR " - 0x%016" PRIxPTR " %s",
				uintptr_t(phdr.p_vaddr), seg_end, elf_p_flags_name(phdr.p_flags).c_str());
		}
	}

	void parse_commandline(int argc, const char *argv[])
	{
		cmdline_option options[] =
		{
			{ "-m", "--memory-debug", cmdline_arg_type_none,
				"Memory debug",
				[&](std::string s) { return (memory_debug = true); } },
			{ "-d", "--emulator-debug", cmdline_arg_type_none,
				"Emulator debug",
				[&](std::string s) { return (emulator_debug = true); } },
			{ "-i", "--isa", cmdline_arg_type_string,
				"ISA Extensions (IMA, IMAC, IMAFD, IMAFDC)",
				[&](std::string s) { return (ext = decode_isa_ext(s)); } },
			{ "-r", "--log-registers", cmdline_arg_type_none,
				"Log Registers",
				[&](std::string s) { return (log_registers = true); } },
			{ "-l", "--log-instructions", cmdline_arg_type_none,
				"Log Instructions",
				[&](std::string s) { return (log_instructions = true); } },
			{ "-h", "--help", cmdline_arg_type_none,
				"Show help",
				[&](std::string s) { return (help_or_error = true); } },
			{ nullptr, nullptr, cmdline_arg_type_none,   nullptr, nullptr }
		};

		auto result = cmdline_option::process_options(options, argc, argv);
		if (!result.second) {
			help_or_error = true;
		} else if (result.first.size() != 1) {
			printf("%s: wrong number of arguments\n", argv[0]);
			help_or_error = true;
		}

		if (help_or_error) {
			printf("usage: %s [<options>] <elf_file>\n", argv[0]);
			cmdline_option::print_options(options);
			exit(9);
		}

		filename = result.first[0];

		/* print process information */
		if (memory_debug) {
			memory_info(argc, argv);
		}

		/* load ELF (headers only) */
		elf.load(filename, true);
	}

	/* print approximate location of host text, heap and stack of our user process */
	void memory_info(int argc, const char *argv[])
	{
		static const char *textptr = nullptr;
		void *heapptr = malloc(8);
		debug("text : ~0x%016" PRIxPTR, (uintptr_t)&textptr);
		debug("heap : ~0x%016" PRIxPTR, (uintptr_t)heapptr);
		debug("stack: ~0x%016" PRIxPTR, (uintptr_t)argv);
		free(heapptr);
	}

	/* Start the execuatable with the given processor template */
	template <typename P>
	void start()
	{
		/* instantiate processor, set log options and program counter to entry address */
		P proc;
		proc.flags = emulator_debug ? riscv_processor_flag_emulator_debug : 0;
		proc.log_registers = log_registers;
		proc.log_instructions = log_instructions;
		proc.pc = elf.ehdr.e_entry;

		/* Find the ELF executable PT_LOAD segments and mmap them into memory */
		for (size_t i = 0; i < elf.phdrs.size(); i++) {
			Elf64_Phdr &phdr = elf.phdrs[i];
			if (phdr.p_flags & PT_LOAD) {
				map_load_segment(proc, filename.c_str(), phdr);
			}
		}

		/* Map a stack and set the stack pointer */
		map_stack(proc, stack_top, stack_size);

#if defined (ENABLE_GPERFTOOL)
		ProfilerStart("test-emulate.out");
#endif

		/* Step the CPU until it halts */
		while(proc.step(1024));

#if defined (ENABLE_GPERFTOOL)
		ProfilerStop();
#endif

		/* Unmap memory segments */
		for (auto &seg: proc.mapped_segments) {
			munmap(seg.first, seg.second);
		}
	}

	/* Start a specific processor implementation based on ELF type and ISA extensions */
	void exec()
	{
		switch (elf.ei_class) {
			case ELFCLASS32:
				switch (ext) {
					case rv_isa_ima: start<riscv_processor_proxy_rv32ima>(); break;
					case rv_isa_imac: start<riscv_processor_proxy_rv32imac>(); break;
					case rv_isa_imafd: start<riscv_processor_proxy_rv32imafd>(); break;
					case rv_isa_imafdc: start<riscv_processor_proxy_rv32imafdc>(); break;
					case rv_isa_none: panic("unknown isa extension"); break;
				}
				break;
			case ELFCLASS64:
				switch (ext) {
					case rv_isa_ima: start<riscv_processor_proxy_rv64ima>(); break;
					case rv_isa_imac: start<riscv_processor_proxy_rv64imac>(); break;
					case rv_isa_imafd: start<riscv_processor_proxy_rv64imafd>(); break;
					case rv_isa_imafdc: start<riscv_processor_proxy_rv64imafdc>(); break;
					case rv_isa_none: panic("unknown isa extension"); break;
				}
				break;
			default: panic("unknonwn elf class");
		}
	}
};


/* program main */

int main(int argc, const char *argv[])
{
	riscv_emulator emulator;
	emulator.parse_commandline(argc, argv);
	emulator.exec();
	return 0;
}
