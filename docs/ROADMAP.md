# d9mt — roadmap

*This fork (Fireblossom/d9mt) on top of [neo773/d9mt](https://github.com/neo773/d9mt).*

## What d9mt is

A **DXVK D3D9 frontend + a hand-written Metal backend** — no Vulkan, no
MoltenVK. Shader path: `DXSO → SPIR-V → spirv-cross → MSL → xcrun metal`.
Goal: run vanilla-era D3D9 games (e.g. WoW 1.12) on Apple Silicon without
MoltenVK's shader-compile freezes.

**Status: a working research MVP.** Renders WoW 1.12 with MSAA on an M3, at the
windowed display cap.

### This fork's additions so far

| commit | what |
|--------|------|
| `098ec14` | GPU-hang recovery (Plan A) + triple-buffering / Phase B (Plan B) |
| `c714afb` | startup-hang fix (don't kill the watcher from a probe instance) |
| `2bc09e4` | docs: macOS process-exit hang & cleanup daemon |
| `e8808f6` | present MSAA backbuffers whose swizzle only remaps alpha |

## How to read this

Three buckets by **tractability**, because not everything is a "bug you fix":

- 🟢 **Fixable** — finite, understandable, patchable. The MVP is full of these; several are already cleared.
- 🟡 **Mitigable** — can't eliminate; can make it hurt less / keep it managed.
- 🔴 **Inherent / not ours** — a structural cost of the architecture, or Apple's bug. "Fixing" means re-architecting, or waiting on Apple.

---

## Horizon 1 — polish the MVP 🟢

Make the existing renderer reliable and complete for daily use. Mostly
mechanical, all within the current design.

- [ ] **Persistent shader cache / warm start.** Shaders recompile every launch today (`d3d9fe.log` is wall-to-wall `metallib COMPILED`). Persist the compiled metallib so the 2nd launch is fast — and, crucially, hits Apple's shader compiler far less (see the cross-cutting section).
- [ ] **High-refresh / fullscreen present.** Windowed is pinned to 60 by the macOS compositor even on a 120 Hz ProMotion panel. Add a fullscreen present path and/or opt the `CAMetalLayer` into ProMotion so the GPU headroom (already there) is actually usable.
- [ ] **Software cursor composition.** Currently logged "not implemented — ignored"; some games need it.
- [ ] **Wider format / swizzle coverage.** MSAA present now handles alpha-only (X8) swizzles; handle real colour-reorder swizzles (swizzled view of the resolve target) and any remaining unmapped formats instead of skipping.
- [ ] **Gamma / colour-space correctness pass.** Validate the gamma-ramp + colorspace path against a reference image.
- [ ] **In-source exit-hang investigation.** Stopping the watcher at teardown did **not** cure the winemac `-[NSApplication _shouldTerminate]` hang. Confirm whether anything d9mt-side helps (drawable-release ordering, Metal teardown) before leaning on the external reaper permanently.

## Horizon 2 — de-risk the architecture 🟡

Can't remove the architectural cost; can stop it bleeding. This is the answer
to the "tracks DXVK internals forever / unmaintainable" critique.

- [ ] **Pin DXVK + keep a thin patch set.** Vendor a fixed DXVK revision; keep frontend changes as a minimal, documented diff so re-basing onto upstream is a finite job, not a rewrite.
- [ ] **Write down the backend-surface contract.** The seam between the DXVK frontend and the Metal backend (the `BACKEND-SURFACE §X` references already in the code) is only maintainable if the contract is explicit. Document it.
- [ ] **Shader-hang blast-radius reduction.** Per-shader compile timeout + fallback so one pathological shader can't wedge the pipeline; log timed-out shader hashes so they can be reported / worked around.
- [ ] **Regression harness.** Scene-capture + frame-hash checks so the format / swizzle / MSAA / present paths don't silently regress when DXVK is re-based.

## Horizon 3 — the deep critique 🔴

These are the *real* architectural criticisms. Honest framing: within d9mt's
design they are not "fixed", they are **traded away by changing the design**.
Listed so the cost is explicit — not so it's promised.

- **The shader chain** `DXSO → SPIR-V → spirv-cross → MSL`. Four translation layers; the SPIR-V→MSL hop (spirv-cross) is the rough one, and the MSL it emits is what trips Apple's compiler. A real fix means a more direct `DXSO → MSL` compiler — i.e. a from-scratch shader backend. That is a different project (and is roughly what DXMT / Apple's D3DMetal already do).
- **The reimplemented DXVK backend.** Inherent to "DXVK frontend + custom Metal backend". Reducible only by depending on *less* DXVK internal — which trades the frontend's maturity for less surface.
- **Strategic option: don't carry this alone.** The honest long game for D3D9-on-Metal is probably *upstreaming* — D3D9 into DXMT (DX11/12→Metal, more direct), or Apple's Game Porting Toolkit growing D3D9 coverage — rather than rearchitecting d9mt. Worth tracking both.

## Cross-cutting — the Apple Metal compiler hang 🔴

The Orgrimmar / spell-effect **permanent freeze**: Apple's AGXMetal/llvm shader
compiler infinite-loops on certain generated MSL. **Not d9mt's bug to fix.**
d9mt can only:

- **Survive it** — Plan A: bounded watcher wait → device-lost recovery (done). No more unkillable freeze.
- **Reduce exposure** — persistent cache (compile once), pre-warm known shaders.
- **Localize + report** — per-shader timeout + hash logging → an Apple bug report with repros.
- **Avoid it** — if a triggering MSL pattern is identified, special-case the chain not to emit it (whack-a-mole).

A hard ceiling until Apple fixes their compiler, or the shader chain changes
(Horizon 3).

---

## Priority

1. **Shader cache** (H1) — biggest single win: faster launches *and* less compiler exposure.
2. **Fullscreen / high-refresh** (H1) — unlock the GPU headroom that's already there.
3. **Pin DXVK + document the seam** (H2) — make the fork re-baseable.
4. Rest of H1/H2 as needed. **Horizon 3 is a decision, not a task** — pursue only if d9mt is meant to be a long-term foundation rather than a working hack.

## The one-line verdict it's built on

> The rough edges are patchable and we've patched several. The architecture
> isn't — those costs come with choosing "DXVK frontend + reimplemented Metal
> backend + a four-stage shader chain". Polishing makes a *better d9mt*; the
> deep critique is only answered by *not being d9mt*. For daily WoW-on-M3 that
> trade is fine; as a serious project foundation, weigh Horizon 3 honestly.
