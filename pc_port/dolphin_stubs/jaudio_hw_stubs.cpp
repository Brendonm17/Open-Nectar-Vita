/**
 * @file jaudio_hw_stubs.cpp
 * @brief Hardware symbols the original audio engine references but that this
 *        port has no silicon for.
 *
 * Only compiled when PIKMIN_NATIVE_JAUDIO is on. Activating src/jaudio pulls in
 * exactly nine symbols that live in directories the port never builds:
 * src/dsp/ (the DSP register block), src/os/ (interrupt masking) and
 * src/hvqm4dec/ (the video decoder). Nothing else was missing, and no symbol
 * collided, so this file is the whole hardware boundary.
 *
 * The DSP register and interrupt stubs are inert by design, not by omission:
 * pc_dsp_host renders the voice parameter blocks on the CPU and deliberately
 * produces no mailbox traffic, so a real register file would have no reader.
 */

#include "types.h"

#if PIKI_USE_JAUDIO

#include "Dolphin/OS/OSInterrupt.h"

/**
 * DSP hardware register file. audiothread.c and dspboot.c poll and write these
 * during boot and sync. Backing them with plain memory lets that code run
 * unchanged; the values it reads back are the ones it last wrote, which is
 * enough because the host renderer never waits on the DSP to answer.
 */
vu16 __DSPRegs[32];

extern "C" {

/**
 * @brief Accepts an interrupt unmask request and reports nothing was masked.
 *
 * DSPInit2 and DspBoot unmask the DSP interrupt during boot. There is no
 * interrupt controller here and no handler to arm, so the request is a no-op.
 */
OSInterruptMask __OSUnmaskInterrupts(OSInterruptMask mask)
{
	(void)mask;
	return 0;
}

} // extern "C"

#endif // PIKI_USE_JAUDIO
