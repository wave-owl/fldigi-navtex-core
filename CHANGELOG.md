# Changelog

All notable changes to fldigi-navtex-core. Versions follow semantic versioning; the contract
tag `navtex-c1` changes only with an incompatible API change.

## 0.1.0 - 2026-10-18

First release (wave-owl change `add-fldigi-navtex-core`).

- NAVTEX and SITOR-B receiver after fldigi 4.2.13 `src/navtex/navtex.cxx`: mark/space mixers,
  fldigi's raised-cosine RTTY low passes (fldigi-dsp-core 0.7.0), W7AY's discriminator, the
  early/prompt/late multicorrelator, the alpha/rep phase search, the CCIR 476 FEC cascade,
  letters/figures, the ZCZC/NNNN framing with lost header, lost trailer and the 600 s
  timeout; fldigi's s/n, metric and AFC on a power spectrum computed here.
- NAVTEX and SITOR-B transmitter after fldigi (`tx=1`): encoder, B-mode interleave, phasing,
  fldigi's `ZCZC ZI<nn>` framing in NAVTEX mode.
- Verification against the fldigi 4.2.13 bench: 11 TX cases sample-identical, 38 RX decodes
  identical in printed bytes, logged messages and logic levels.
- Deviations: per-instance state, 64-bit sample clock, USB only, the power spectrum in place
  of the GUI waterfall, defined behaviour where fldigi's is undefined.
