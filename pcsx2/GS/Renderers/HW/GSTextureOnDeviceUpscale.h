// GE1 on-device generated replacements: general filter registry under one
// live toggle (ODP1 §§1-3, REM3 P10 addendum).
//
// A targeted provider at the texture-cache lookup: on eligible hash-cache
// misses, after private-pack precedence, a registry row matching the FULL
// identity (content hash, palette hash, format bits, region) selects a
// versioned recipe. P10 ships one recipe, the font raster-edge port
// (GE1FontFilter, byte-equal to tools/ssx3/remaster_font.py); ODU1's ML
// recipes register here later. Lookup misses never start a general image
// upscaler.
//
// Sync-only in P10: fonts produce a Ready result in the lookup thread
// (measured ~106 ms for the largest atlas on arm64 Mac, one-time per
// atlas; warm disk-cache hits decode in ms). No async jobs, no pending
// marks, no completion injection: toggling re-resolves by purge +
// re-lookup. A running generation owns its buffers; the generation counter
// below is the contract Phase 2's async recipes use to reject stale work.
//
// Threading: lookups run on the texture cache's thread. Apply* and SetSfn*
// run outside it (open / host GS worker); a mutex guards the flag, the
// SFN store and stats. Disk I/O (cache read/write) happens in the lookup
// thread, bounded: 4 font files, ~1 MB total, well under the 256 MiB LRU.
#pragma once

#include "GS/Renderers/HW/GSTextureReplacements.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace GE1OnDeviceUpscale
{

// Live toggle contract (ODP1 §2), vendor side. The runtime's settings
// command (Phase 2) calls ApplyOnDeviceUpscale on the host GS worker with
// the vendor back queue drained, then purges generated entries through
// GSUpdateConfig's OnDeviceUpscale comparison. Enablement at open comes
// from InitializeFromEnvironment (GE1_ONDEVICE_UPSCALE=1); after that the
// applied flag is authoritative (no getenv polling on the lookup path).
void InitializeFromEnvironment();
void ApplyOnDeviceUpscale(bool enabled, unsigned sequence);
bool IsEnabled();
unsigned Generation();

// Prescale switch (default on since ge1-upscale-land, Brad 10-17;
// GE1_ONDEVICE_UPSCALE_PRESCALE=0 is the off switch): font/word rows are
// Lanczos-resampled from 4x to the current render scale before upload,
// cached per (texture, scale). Integer render scales >= 4 skip it.
bool PrescaleEnabled();

// SFN metrics for atlases 2/72, parsed from the user's own disc files
// (FEFONT.SFN/HUDFONT.SFN) outside the GS thread. Vendor reads
// GE1_ONDEVICE_SFN_DIR at open; SetSfnData also serves tests and the
// Phase 2 runtime loader. Unknown/absent metrics leave fonts stock.
bool SetSfnData(int atlas, const std::uint8_t* data, std::size_t len);
bool HaveSfnData(int atlas);

// Provider entry, called on hash-cache misses after pack precedence.
// Returns true with `out` populated when a registry row matches and the
// pixels are ready (freshly generated or disk-cache decoded). The caller
// uploads exactly like a pack texture (same provenance and mip policy).
// Never starts async work; on any skip or failure returns false (stock).
bool LookupGenerated(const GSTextureCache::HashCacheKey& key, const GIFRegTEX0& TEX0,
	const GIFRegTEXA& TEXA, GSTextureCache::SourceRegion region, GSLocalMemory& mem, bool mipmap,
	GSTextureReplacements::ReplacementTexture& out, std::pair<std::uint8_t, std::uint8_t>* alpha_minmax);

struct Stats
{
	unsigned long long lookups = 0;
	unsigned long long registry_hits = 0;
	unsigned long long generated = 0;
	unsigned long long cache_hits = 0;
	unsigned long long prescale_hits = 0;
	unsigned long long prescale_generated = 0;
	unsigned long long cache_corrupt = 0;
	unsigned long long skipped_disabled = 0;
	unsigned long long skipped_fmv = 0;
	unsigned long long skipped_gpu_clut = 0;
	unsigned long long skipped_region = 0;
	unsigned long long skipped_mipmap = 0;
	unsigned long long skipped_no_sfn = 0;
	unsigned long long skipped_decode = 0;
	unsigned long long skipped_geometry = 0;
};
Stats GetStats();

// Test/bring-up hooks (not a settings protocol).
void SetCacheRootForTests(const std::string& dir);
void ResetForTests();

} // namespace GE1OnDeviceUpscale
