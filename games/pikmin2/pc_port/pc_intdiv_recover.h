#pragma once

// División entera entre cero en x86-64. En GameCube (divw) y en ARM64 da un
// valor sin más; en x86 es una excepción y el juego se cerraba. El código
// decompilado hereda divisiones que en consola nunca fallaban, así que aquí se
// emula ARM64: cociente y resto a 0 (desbordamiento INT_MIN / -1: cociente =
// dividendo, resto 0) y se sigue tras la instrucción. Cada caso se registra
// una vez con su dirección para arreglar el origen.
//
// Uso: desde el manejador de SIGFPE (Linux) o un manejador vectorizado
// (Windows), con los registros del hilo que falló.

#if defined(__x86_64__) || defined(_M_X64)

#include <cstdint>

// Longitud de la instrucción DIV/IDIV en p, o 0 si no lo es.
static inline int pc_intdiv_length(const uint8_t* p, bool* byteOp)
{
	int n = 0;
	while (n < 4 && (p[n] == 0x66 || p[n] == 0x67 || p[n] == 0xF2 || p[n] == 0xF3 || p[n] == 0x2E || p[n] == 0x3E
	                 || p[n] == 0x26 || p[n] == 0x36 || p[n] == 0x64 || p[n] == 0x65)) {
		n++;
	}
	if ((p[n] & 0xF0) == 0x40) n++; // REX
	const uint8_t op = p[n++];
	if (op != 0xF6 && op != 0xF7) return 0;
	const uint8_t modrm = p[n++];
	const int reg = (modrm >> 3) & 7;
	if (reg != 6 && reg != 7) return 0; // /6 DIV, /7 IDIV
	const int mod = modrm >> 6, rm = modrm & 7;
	if (mod != 3 && rm == 4) {
		const uint8_t sib = p[n++];
		if (mod == 0 && (sib & 7) == 5) n += 4;
	}
	if (mod == 0 && rm == 5) n += 4; // RIP + disp32
	else if (mod == 1) n += 1;
	else if (mod == 2) n += 4;
	*byteOp = op == 0xF6;
	return n;
}

// Ajusta rax/rdx/rip y devuelve true si la instrucción era una división.
static inline bool pc_intdiv_recover(uint64_t* rip, uint64_t* rax, uint64_t* rdx, bool overflow)
{
	bool byteOp = false;
	const int len = pc_intdiv_length(reinterpret_cast<const uint8_t*>(*rip), &byteOp);
	if (len == 0) return false;
	if (byteOp) {
		// AX / r/m8: AL = cociente, AH = resto.
		*rax = overflow ? (*rax & ~0xFF00ull) : (*rax & ~0xFFFFull);
	} else {
		if (!overflow) *rax = 0;
		*rdx = 0;
	}
	*rip += len;
	return true;
}

#endif
