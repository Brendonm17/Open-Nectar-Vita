## 2025-05-15 - DSP Volume Ramping Division Optimization
**Learning:** Audio DSP channels (`pc_dsp_host.cpp`) compute linear volume ramps per subframe (80 samples). Re-calculating integer volume interpolation (`start + (end - start) * t / n`) on every sample per voice performed millions of redundant 32-bit integer divisions per second during steady-state audio when volume is constant (`start == end`).
**Action:** Pre-calculate `diff = end - start` outside subframe sample loops and bypass division when `diff == 0`.
