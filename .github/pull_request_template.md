<!-- Title: Conventional Commits, e.g. "fix(wps): large JPEG covers load". Checked by pr-title.yml. -->

## What and why


## Evidence

<!-- Tick only what this PR actually has; CLAUDE.md "Claims". An agent never ticks the last box. -->
- [ ] **built** - CI `build` green; device version: `...` (run summary)
- [ ] **tested** - stick tests / repro output pasted below
- [ ] **sim-checked** - simulator frames attached (before/after if behaviour changes)
- [ ] **device-verified** - flashed and checked by the human; PR labelled `verified: device`

Still unverified on the device:

## Device checklist

<!-- One action and its expected result per line. Item 0 ties the results to this build. -->
0. System > Rockbox Info shows the device version above - else stop, wrong build
1. 

## Notes

<!-- Delete what does not apply. -->
- Touches `bootloader/` or `tools/r1_patcher/`: `r1.upt` must be reflashed.

<!-- "Closes #n" for an issue labelled "needs: device test" fails device-gate.yml
     until the PR is labelled "verified: device"; use "Refs #n" until then. -->
Closes #
