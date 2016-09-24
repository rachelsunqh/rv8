//
//  riscv-codec.h
//

#ifndef riscv_codec_h
#define riscv_codec_h

/*
 *
 * Instruction length
 * ==================
 * Returns the instruction length, either 2, 4, 6 or 8 bytes.
 *
 *   inline size_t riscv::inst_length(uint64_t inst)
 *
 * Instruction fetch
 * =================
 * Returns the instruction and its length
 *
 *   inline size_t riscv::inst_fetch(uintptr_t addr, uintptr_t *inst_length)
 *
 * Decoding instructions
 * =====================
 * The decode functions decode the instruction passed as an argument in to
 * struct riscv_decode using: op, codec, imm, rd, rs1, rs2, etc. 
 * The encode function only depends on the fields in riscv_decode.
 *
 *   template <typename T> inline void riscv::decode_inst_rv32(T &dec, uint64_t inst)
 *   template <typename T> inline void riscv::decode_inst_rv64(T &dec, uint64_t inst)
 *
 * Encoding instructions
 * =====================
 * The encode function encodes the operands in struct riscv_decode using:
 * op, imm, rd, rs1, rs2, etc. The encode function only depends on 
 * riscv_decode fields and it is up to the caller to save the instruction.
 * Returns the encoded instruction.
 *
 *   template <typename T> inline uint64_t riscv::encode_inst(T &dec)
 *
 * Decompressing instructions
 * ==========================
 * The decompress functions work on an already decoded instruction and
 * they just set the op and codec field if the instruction is compressed.
 *
 *   template <typename T> inline void riscv::decompress_inst_rv32(T &dec)
 *   template <typename T> inline void riscv::decompress_inst_rv64(T &dec)
 *
 * Compressing instructions
 * ========================
 * The compress functions work on an already decoded instruction and
 * they just set the op and codec field if the instruction is compressed.
 * Returns false if the instruction cannot be compressed.
 *
 *   template <typename T> inline bool riscv::compress_inst_rv32(T &dec)
 *   template <typename T> inline bool riscv::compress_inst_rv64(T &dec)
 *
 */

/*
 * Decoded Instruction
 *
 * Structure that contains instruction decode information.
 */

namespace riscv
{

	struct decode
	{
		int32_t   imm;

		union {
			uint32_t inst;
			struct {
				uint32_t opcode : 7;
				uint32_t rd     : 5;
				uint32_t rm     : 3;
				uint32_t rs1    : 5;
				uint32_t rs2    : 5;
				uint32_t _pad1  : 2;
				uint32_t rs3    : 5;
			} r;
			struct {
				uint32_t _pad1  : 25;
				uint32_t rl     : 1;
				uint32_t aq     : 1;
				uint32_t _pad2  : 5;
			} amo;
			struct {
				uint32_t _pad1  : 20;
				uint32_t succ   : 4;
				uint32_t pred   : 4;
				uint32_t _pad2  : 4;
			} fence;
		} rv;

		union {
			uint16_t inst;
		} rvc;

		uint8_t  op;
		uint8_t  codec;


		decode() : imm(0), rv{ .inst = 0 }, rvc{ .inst = 0 }, op(0), codec(0) {}
	};

	#include "riscv-operands.h"
	#include "riscv-decode.h"
	#include "riscv-encode.h"
	#include "riscv-switch.h"
	#include "riscv-constraints.h"


	/* Instruction Length */

	inline size_t inst_length(uint64_t inst)
	{
		// instruction length coding

		//      aa - 16 bit aa != 11
		//   bbb11 - 32 bit bbb != 111
		//  011111 - 48 bit
		// 0111111 - 64 bit

		// NOTE: currenttly supports maximum of 64-bit
		return (inst &      0b11) != 0b11      ? 2
			 : (inst &   0b11100) != 0b11100   ? 4
			 : (inst &  0b111111) == 0b011111  ? 6
			 : (inst & 0b1111111) == 0b0111111 ? 8
			 : 0;
	}

	/* Fetch Instruction */

	inline uint64_t inst_fetch(uintptr_t addr, size_t *inst_length)
	{
		// NOTE: currently supports maximum instruction size of 64-bits

		// optimistically read 32-bit instruction
		uint64_t inst = htole32(*(uint32_t*)addr);
		if ((inst & 0b11) != 0b11) {
			inst &= 0xffff; // mask to 16-bits
			*inst_length = 2;
		} else if ((inst & 0b11100) != 0b11100) {
			*inst_length = 4;
		} else if ((inst & 0b111111) == 0b011111) {
			inst |= uint64_t(htole16(*(uint16_t*)(addr + 4))) << 32;
			*inst_length = 6;
		} else if ((inst & 0b1111111) == 0b0111111) {
			inst |= uint64_t(htole32(*(uint32_t*)(addr + 4))) << 32;
			*inst_length = 8;
		} else {
			inst = 0; /* illegal instruction */
			*inst_length = 8;
		}
		return inst;
	}

	/* Decompress Instruction */

	template <typename T>
	inline void decompress_inst_rv32(T &dec)
	{
	    int decomp_op = riscv_inst_decomp_rv32[dec.op];
	    if (decomp_op != riscv_op_illegal) {
	        dec.op = decomp_op;
	        dec.codec = riscv_inst_codec[decomp_op];
	    }
	}

	template <typename T>
	inline void decompress_inst_rv64(T &dec)
	{
	    int decomp_op = riscv_inst_decomp_rv64[dec.op];
	    if (decomp_op != riscv_op_illegal) {
	        dec.op = decomp_op;
	        dec.codec = riscv_inst_codec[decomp_op];
	    }
	}

	/* Decode Instruction */

	template <typename T, bool rv32, bool rv64, bool rvi = true, bool rvm = true, bool rva = true, bool rvs = true, bool rvf = true, bool rvd = true, bool rvc = true>
	inline void decode_inst(T &dec, uint64_t inst)
	{
		dec.rv.inst = u32(inst);
		dec.op = decode_inst_op<rv32,rv64,rvi,rvm,rva,rvs,rvf,rvd,rvc>(inst);
		decode_inst_type<T>(dec, inst);
	}

	template <typename T>
	inline void decode_inst_rv32(T &dec, uint64_t inst)
	{
		decode_inst<T,true,false>(dec, inst);
		decompress_inst_rv32<T>(dec);
	}

	template <typename T>
	inline void decode_inst_rv64(T &dec, uint64_t inst)
	{
		decode_inst<T,false,true>(dec, inst);
		decompress_inst_rv64<T>(dec);
	}


	/* Decode Pseudoinstruction */

	template <typename T>
	inline bool decode_pseudo_inst(T &dec)
	{
		const riscv_comp_data *comp_data = riscv_inst_pseudo[dec.op];
		if (!comp_data) return false;
		while (comp_data->constraints) {
			if (constraint_check(dec, comp_data->constraints)) {
				dec.op = comp_data->op;
				dec.codec = riscv_inst_codec[dec.op];
				return true;
			}
			comp_data++;
		}
		return false;
	}


	/* Compress Instruction */

	template <typename T>
	inline bool compress_inst_rv32(T &dec)
	{
		const riscv_comp_data *comp_data = riscv_inst_comp_rv32[dec.op];
		if (!comp_data) return false;
		while (comp_data->constraints) {
			if (constraint_check(dec, comp_data->constraints)) {
				dec.op = comp_data->op;
				dec.codec = riscv_inst_codec[dec.op];
				return true;
			}
			comp_data++;
		}
		return false;
	}

	template <typename T>
	inline bool compress_inst_rv64(T &dec)
	{
		const riscv_comp_data *comp_data = riscv_inst_comp_rv64[dec.op];
		if (!comp_data) return false;
		while (comp_data->constraints) {
			if (constraint_check(dec, comp_data->constraints)) {
				dec.op = comp_data->op;
				dec.codec = riscv_inst_codec[dec.op];
				return true;
			}
			comp_data++;
		}
		return false;
	}

}

#endif
