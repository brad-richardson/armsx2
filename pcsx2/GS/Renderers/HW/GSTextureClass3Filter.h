// GE1 class-3 art filter: Lanczos-4x + mild unsharp (PIL recipe port).
//
// Replicates, in double precision, the numpy-fitted chain (REM3 P12):
// premultiply (half-up), separable Lanczos-3 4x (center-mapped,
// clamp-to-edge, per-pixel renormalized, rint-rounded planes), clip,
// unpremultiply (half-up, alpha 0 -> RGB 0), Gaussian sigma=2.0 span 3
// edge-clamped, per-channel unsharp |d|>=3 -> v + d*0.5, half-up quantize,
// alpha raw-Lanczos or clamped to a stock peak. No PCSX2 dependencies
// (pure byte buffers) so a standalone harness can verify it against the
// PIL oracle raws without linking the emulator.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace GE1Class3Filter
{

// Builds the 4x canvas from stock RGBA (w*h*4 bytes, RGBA order).
// alpha_clamp < 0 leaves the Lanczos alpha raw (buttons); otherwise the
// alpha is min(alpha, alpha_clamp) (snowflake: 128, the stock peak).
// Returns false on empty input. ms receives wall milliseconds or null.
bool BuildLanczosAtlas(const std::uint8_t* stock_rgba, int w, int h, int alpha_clamp,
	std::vector<std::uint8_t>& out_rgba, int& out_w, int& out_h, double* ms = nullptr);

} // namespace GE1Class3Filter
