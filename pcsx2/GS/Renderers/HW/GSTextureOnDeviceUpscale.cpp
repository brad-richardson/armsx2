// GE1 on-device generated replacements (see GSTextureOnDeviceUpscale.h).
#include "GS/Renderers/HW/GSTextureOnDeviceUpscale.h"

#include "GS/Renderers/HW/GSTextureFontFilter.h"
#include "GS/Renderers/HW/GSTextureClass3Filter.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/SHA256.h"
#include "pcsx2/Config.h"
#include "GS/GS.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern bool FMVstarted;

namespace GE1OnDeviceUpscale
{

// Defined below LookupGenerated's helpers (provider scope, like the rest of
// the entry points); declared here so ServeImage can build mip chains.
int AppendMipChain(const std::uint8_t* base_rgba, int w, int h,
	std::vector<GSTextureReplacements::ReplacementTexture::MipData>& mips);

namespace
{

// One registry row per full replacement identity (ODP1 §3). Suffix packs
// the TextureName bits (PSM:6 TW:4 TH:4 TA0:8 AEM:1 TA1:8); region and
// miplevel are always base/full for these rows.
struct RegistryRow
{
	std::uint64_t tex0hash, cluthash;
	std::uint32_t suffix;
	int atlas;
	std::uint32_t expect_w, expect_h;
	const char* sfn_file; // atlases 2/72, else nullptr (baked words / art)
	const char* recipe; // filter + cache namespace (ODP1 keying)
	const char* cache_group; // cache dir for rows without sfn_file
	int alpha_clamp; // lanczos rows: -1 raw resized alpha, else fixed clamp (128 = PS2 peak)
	bool mip_chain; // build the RMT9 DDS mip chain on top of the 4x base (class 5)
};

constexpr char kRecipeWords[] = "raster-edge-v1";
// Prescale cache namespace: the 4x bytes resampled to the render scale.
// Keyed per (texture, target dims), so a scale change misses and rebuilds
// from the shared 4x bytes instead of serving stale-scale pixels.
constexpr char kRecipeWordsPrescale[] = "raster-edge-prescale-v1";
constexpr char kPrescaleGroup[] = "words-prescale-v1";
// v2: faithful Pillow port (premult round-half-up, Resample.c fixed-point,
// BoxBlur.c UnsharpMask). v1 cache dirs go stale and are LRU-trimmed.
constexpr char kRecipeArt[] = "lanczos-a-v2";
constexpr int kAtlasButtons = 90;
constexpr int kAtlasFlake = 91;

constexpr RegistryRow kRegistry[] = {
	{0x3be92b30fd7ab98aULL, 0x372720106ade32f4ULL, 0x00002254, 2, 512, 256, "DATA/FONTS/FEFONT.SFN", kRecipeWords, "words-v1", -1, false},
	{0xb7e032dcd63ad8dbULL, 0x372720106ade32f4ULL, 0x00001e14, 72, 256, 128, "DATA/FONTS/HUDFONT.SFN", kRecipeWords, "words-v1", -1, false},
	{0x51446d7052987c01ULL, 0xe5708d0189ccd302ULL, 0x0000221b, 70, 256, 256, nullptr, kRecipeWords, "words-v1", -1, false},
	{0xfb36e14323437580ULL, 0x2f302f4bc33356f5ULL, 0x0000221b, 80, 256, 256, nullptr, kRecipeWords, "words-v1", -1, false},
	// REM3 P12 class 3: action buttons ((a)-content: full-atlas Lanczos +
	// unsharp, alpha clamped to the PS2 peak 128 — raw overshoot above
	// 0x80 is over 100% opacity, so the P12 raw-alpha choice was wrong;
	// the staged composite's v5c background and feather ring remain
	// documented tolerance, not reproduced) and the 2779 loading snowflake
	// x2 CLUT variants (Lanczos + unsharp, alpha clamped to the stock
	// peak 128; v5f recipe, within tolerance of the shipped x4plus file
	// since the ML original cannot run on device).
	{0x2eacf2633330915cULL, 0xdcec7fcad512bfe3ULL, 0x0000221b, kAtlasButtons, 256, 256, nullptr, kRecipeArt, "art-v1", 128, false},
	{0x2779a08db7a8aeffULL, 0x1cd1ab68062c5339ULL, 0x0000155b, kAtlasFlake, 32, 32, nullptr, kRecipeArt, "art-v1", 128, false},
	{0x2779a08db7a8aeffULL, 0xae8c15bdb2b097cfULL, 0x0000155b, kAtlasFlake, 32, 32, nullptr, kRecipeArt, "art-v1", 128, false},
	// REM3 P13 classes 4-5: 11 menu-key textures (atlas 100+) + 47 Tricky
	// sign textures in UPX1 inventory order (atlas 200+). All: Lanczos4 +
	// Pillow UnsharpMask r2/p50/t3, alpha clamped to the PS2 peak 128
	// (alpha_top: every stock peaks <= 128). Class-5 rows (mip_chain)
	// additionally carry the pack DDS mip chain. The 12th menu key
	// (2eacf2633330915c buttons layout) stays on its accepted P12
	// atlas-90 row (raw alpha); see REPORT P13 for the open clamp question.
	{0x41c08928f3d5346dULL, 0x99dfdd6a5d43452ULL, 0x0000221b, 100, 256, 256, nullptr, kRecipeArt, "art-v1", 128, false}, // 41c08928f3d5346d-99dfdd6a5d43452-0000221b.png
	{0x4fd5b112a303ee29ULL, 0x5db1ffbc628260a9ULL, 0x0000265b, 101, 512, 512, nullptr, kRecipeArt, "art-v1", 128, false}, // 4fd5b112a303ee29-5db1ffbc628260a9-0000265b.png
	{0x6fdb04ca03f74699ULL, 0xd905ebdca518f156ULL, 0x0000221b, 102, 256, 256, nullptr, kRecipeArt, "art-v1", 128, false}, // 6fdb04ca03f74699-d905ebdca518f156-0000221b.png
	{0x82dcf3f8180af0b5ULL, 0xfb3f3b131a5dc0b8ULL, 0x0000221b, 103, 256, 256, nullptr, kRecipeArt, "art-v1", 128, false}, // 82dcf3f8180af0b5-fb3f3b131a5dc0b8-0000221b.png
	{0x8e09767abd52165eULL, 0x667f54aa78758c0fULL, 0x0000221b, 104, 256, 256, nullptr, kRecipeArt, "art-v1", 128, false}, // 8e09767abd52165e-667f54aa78758c0f-0000221b.png
	{0x9d8611b846a20c6dULL, 0xdd77c7aaef8ebba9ULL, 0x0000221b, 105, 256, 256, nullptr, kRecipeArt, "art-v1", 128, false}, // 9d8611b846a20c6d-dd77c7aaef8ebba9-0000221b.png
	{0x9ed7034f038263c2ULL, 0xac0cf80fd058aff9ULL, 0x0000221b, 106, 256, 256, nullptr, kRecipeArt, "art-v1", 128, false}, // 9ed7034f038263c2-ac0cf80fd058aff9-0000221b.png
	{0xb4f779439f3683e5ULL, 0x3c2a14691dc42323ULL, 0x0000221b, 107, 256, 256, nullptr, kRecipeArt, "art-v1", 128, false}, // b4f779439f3683e5-3c2a14691dc42323-0000221b.png
	{0xbf06df6b73301066ULL, 0x8534e13fe26d2fULL, 0x0000221b, 108, 256, 256, nullptr, kRecipeArt, "art-v1", 128, false}, // bf06df6b73301066-8534e13fe26d2f-0000221b.png
	{0xda03aa8e6326312dULL, 0x7a7fb9e402163862ULL, 0x0000221b, 109, 256, 256, nullptr, kRecipeArt, "art-v1", 128, false}, // da03aa8e6326312d-7a7fb9e402163862-0000221b.png
	{0xea247a3102e9bc92ULL, 0x8df23378d89026f4ULL, 0x00001993, 110, 64, 64, nullptr, kRecipeArt, "art-v1", 128, false}, // ea247a3102e9bc92-8df23378d89026f4-00001993.png
	{0x10da55cb1ceee898ULL, 0xf93775881c308fd4ULL, 0x00001994, 200, 64, 64, nullptr, kRecipeArt, "art-v1", 128, true}, // 10da55cb1ceee898-f93775881c308fd4-00001994.png
	{0x24f6ad738370775dULL, 0x5d79a9c2e778f9d9ULL, 0x000019d4, 201, 128, 64, nullptr, kRecipeArt, "art-v1", 128, true}, // 24f6ad738370775d-5d79a9c2e778f9d9-000019d4.png
	{0x2f53aff7afe1a355ULL, 0x5d79a9c2e778f9d9ULL, 0x00001594, 202, 64, 32, nullptr, kRecipeArt, "art-v1", 128, true}, // 2f53aff7afe1a355-5d79a9c2e778f9d9-00001594.png
	{0x4050550b74d0bfdULL, 0x29c9e8dc429afa8fULL, 0x00001153, 203, 32, 16, nullptr, kRecipeArt, "art-v1", 128, true}, // 4050550b74d0bfd-29c9e8dc429afa8f-00001153.png
	{0x45701b64b27ffc6fULL, 0xb87a4ce864a1d02ULL, 0x00001dd4, 204, 128, 128, nullptr, kRecipeArt, "art-v1", 128, true}, // 45701b64b27ffc6f-b87a4ce864a1d02-00001dd4.png
	{0x4b77658ea4cd309dULL, 0xc9ecd8a5dfd1e7a8ULL, 0x000019d3, 205, 128, 64, nullptr, kRecipeArt, "art-v1", 128, true}, // 4b77658ea4cd309d-c9ecd8a5dfd1e7a8-000019d3.png
	{0x4c703e729424e468ULL, 0xc98e9546eb285c55ULL, 0x00001153, 206, 32, 16, nullptr, kRecipeArt, "art-v1", 128, true}, // 4c703e729424e468-c98e9546eb285c55-00001153.png
	{0x4e0fc80870508387ULL, 0xc9ecd8a5dfd1e7a8ULL, 0x000019d3, 207, 128, 64, nullptr, kRecipeArt, "art-v1", 128, true}, // 4e0fc80870508387-c9ecd8a5dfd1e7a8-000019d3.png
	{0x4e9bf5a7d0b82a80ULL, 0x7ff1dab8612c207dULL, 0x00001994, 208, 64, 64, nullptr, kRecipeArt, "art-v1", 128, true}, // 4e9bf5a7d0b82a80-7ff1dab8612c207d-00001994.png
	{0x4f836d4a92113710ULL, 0x8b5d6e706c8d0cb6ULL, 0x00001dd4, 209, 128, 128, nullptr, kRecipeArt, "art-v1", 128, true}, // 4f836d4a92113710-8b5d6e706c8d0cb6-00001dd4.png
	{0x5190a544e0902bf9ULL, 0x29c9e8dc429afa8fULL, 0x000019d3, 210, 128, 64, nullptr, kRecipeArt, "art-v1", 128, true}, // 5190a544e0902bf9-29c9e8dc429afa8f-000019d3.png
	{0x584915239d7a01b7ULL, 0x320dd2a586da9097ULL, 0x00001dd4, 211, 128, 128, nullptr, kRecipeArt, "art-v1", 128, true}, // 584915239d7a01b7-320dd2a586da9097-00001dd4.png
	{0x61e8ffb3cb751c37ULL, 0x29c9e8dc429afa8fULL, 0x00001593, 212, 64, 32, nullptr, kRecipeArt, "art-v1", 128, true}, // 61e8ffb3cb751c37-29c9e8dc429afa8f-00001593.png
	{0x631cb75f3d315fc2ULL, 0x7ff1dab8612c207dULL, 0x00001554, 213, 32, 32, nullptr, kRecipeArt, "art-v1", 128, true}, // 631cb75f3d315fc2-7ff1dab8612c207d-00001554.png
	{0x79ed74cfa83ffb1aULL, 0x4074dbb876e22b8bULL, 0x00001993, 214, 64, 64, nullptr, kRecipeArt, "art-v1", 128, true}, // 79ed74cfa83ffb1a-4074dbb876e22b8b-00001993.png
	{0x7a1a49dca441d27bULL, 0xa6cafdc35438ebf4ULL, 0x000019d3, 215, 128, 64, nullptr, kRecipeArt, "art-v1", 128, true}, // 7a1a49dca441d27b-a6cafdc35438ebf4-000019d3.png
	{0x7b673d0cee8a3a1dULL, 0x29c9e8dc429afa8fULL, 0x000019d3, 216, 128, 64, nullptr, kRecipeArt, "art-v1", 128, true}, // 7b673d0cee8a3a1d-29c9e8dc429afa8f-000019d3.png
	{0x7dff09450b20086bULL, 0x5d79a9c2e778f9d9ULL, 0x000019d4, 217, 128, 64, nullptr, kRecipeArt, "art-v1", 128, true}, // 7dff09450b20086b-5d79a9c2e778f9d9-000019d4.png
	{0x81588f9b545b44aULL, 0x4074dbb876e22b8bULL, 0x00001dd3, 218, 128, 128, nullptr, kRecipeArt, "art-v1", 128, true}, // 81588f9b545b44a-4074dbb876e22b8b-00001dd3.png
	{0x8a71180ef49579f1ULL, 0xe0c5351a9c903c40ULL, 0x00001113, 219, 16, 16, nullptr, kRecipeArt, "art-v1", 128, true}, // 8a71180ef49579f1-e0c5351a9c903c40-00001113.png
	{0x8d9734af8b202f06ULL, 0xc98e9546eb285c55ULL, 0x000019d3, 220, 128, 64, nullptr, kRecipeArt, "art-v1", 128, true}, // 8d9734af8b202f06-c98e9546eb285c55-000019d3.png
	{0x93c5e52b5a18be27ULL, 0xc98e9546eb285c55ULL, 0x000019d3, 221, 128, 64, nullptr, kRecipeArt, "art-v1", 128, true}, // 93c5e52b5a18be27-c98e9546eb285c55-000019d3.png
	{0x9d0064f200df6904ULL, 0xc9ecd8a5dfd1e7a8ULL, 0x00001153, 222, 32, 16, nullptr, kRecipeArt, "art-v1", 128, true}, // 9d0064f200df6904-c9ecd8a5dfd1e7a8-00001153.png
	{0xa7380a9b9cc49fd2ULL, 0x3d996273c385cb41ULL, 0x00001593, 223, 64, 32, nullptr, kRecipeArt, "art-v1", 128, true}, // a7380a9b9cc49fd2-3d996273c385cb41-00001593.png
	{0xb0203c9771b8d575ULL, 0x5e593210380b307fULL, 0x00001594, 224, 64, 32, nullptr, kRecipeArt, "art-v1", 128, true}, // b0203c9771b8d575-5e593210380b307f-00001594.png
	{0xba507b1e6a9d951aULL, 0xd17a0d4c56b4390ULL, 0x00001dd3, 225, 128, 128, nullptr, kRecipeArt, "art-v1", 128, true}, // ba507b1e6a9d951a-d17a0d4c56b4390-00001dd3.png
	{0xbdb8dec323ffceb6ULL, 0xe086120cdc70c6acULL, 0x00001594, 226, 64, 32, nullptr, kRecipeArt, "art-v1", 128, true}, // bdb8dec323ffceb6-e086120cdc70c6ac-00001594.png
	{0xbe2e50b9c2061e9aULL, 0x1fadd690f6405318ULL, 0x00001993, 227, 64, 64, nullptr, kRecipeArt, "art-v1", 128, true}, // be2e50b9c2061e9a-1fadd690f6405318-00001993.png
	{0xbfb64f75e74d15f1ULL, 0x8499a0e6593ead1fULL, 0x00001dd4, 228, 128, 128, nullptr, kRecipeArt, "art-v1", 128, true}, // bfb64f75e74d15f1-8499a0e6593ead1f-00001dd4.png
	{0xc058f935cfd59646ULL, 0xe0c5351a9c903c40ULL, 0x00001dd3, 229, 128, 128, nullptr, kRecipeArt, "art-v1", 128, true}, // c058f935cfd59646-e0c5351a9c903c40-00001dd3.png
	{0xc573a9862880355aULL, 0xd17a0d4c56b4390ULL, 0x00001dd3, 230, 128, 128, nullptr, kRecipeArt, "art-v1", 128, true}, // c573a9862880355a-d17a0d4c56b4390-00001dd3.png
	{0xc61cdda42d9fc279ULL, 0xa6cafdc35438ebf4ULL, 0x00001593, 231, 64, 32, nullptr, kRecipeArt, "art-v1", 128, true}, // c61cdda42d9fc279-a6cafdc35438ebf4-00001593.png
	{0xca224b4b8120dbe5ULL, 0xa6cbc1789806c372ULL, 0x00001993, 232, 64, 64, nullptr, kRecipeArt, "art-v1", 128, true}, // ca224b4b8120dbe5-a6cbc1789806c372-00001993.png
	{0xcd5e43dc4fcdc12fULL, 0x7ff1dab8612c207dULL, 0x00001dd4, 233, 128, 128, nullptr, kRecipeArt, "art-v1", 128, true}, // cd5e43dc4fcdc12f-7ff1dab8612c207d-00001dd4.png
	{0xd1b6976bd983764dULL, 0x5a5378a4b166b9e7ULL, 0x000019d4, 234, 128, 64, nullptr, kRecipeArt, "art-v1", 128, true}, // d1b6976bd983764d-5a5378a4b166b9e7-000019d4.png
	{0xd458416277bb78e2ULL, 0xa6cafdc35438ebf4ULL, 0x000019d3, 235, 128, 64, nullptr, kRecipeArt, "art-v1", 128, true}, // d458416277bb78e2-a6cafdc35438ebf4-000019d3.png
	{0xdb14788b63960cedULL, 0xc98e9546eb285c55ULL, 0x00001593, 236, 64, 32, nullptr, kRecipeArt, "art-v1", 128, true}, // db14788b63960ced-c98e9546eb285c55-00001593.png
	{0xdf469ff5ea999966ULL, 0x950297a0060f26beULL, 0x00001594, 237, 64, 32, nullptr, kRecipeArt, "art-v1", 128, true}, // df469ff5ea999966-950297a0060f26be-00001594.png
	{0xe2ed6e6e2f341d76ULL, 0x30363ad3627f1766ULL, 0x000019d4, 238, 128, 64, nullptr, kRecipeArt, "art-v1", 128, true}, // e2ed6e6e2f341d76-30363ad3627f1766-000019d4.png
	{0xe7946bf3ea24cd63ULL, 0x77fb31f6bb6ae6c4ULL, 0x00001dd4, 239, 128, 128, nullptr, kRecipeArt, "art-v1", 128, true}, // e7946bf3ea24cd63-77fb31f6bb6ae6c4-00001dd4.png
	{0xe9020064096f9b70ULL, 0x5e593210380b307fULL, 0x000019d4, 240, 128, 64, nullptr, kRecipeArt, "art-v1", 128, true}, // e9020064096f9b70-5e593210380b307f-000019d4.png
	{0xe92f35ff6412b0fcULL, 0xd10cd2ef6c61f88cULL, 0x00001dd4, 241, 128, 128, nullptr, kRecipeArt, "art-v1", 128, true}, // e92f35ff6412b0fc-d10cd2ef6c61f88c-00001dd4.png
	{0xee1c70cc2d051c7ULL, 0xf99728affcc0c87eULL, 0x00001554, 242, 32, 32, nullptr, kRecipeArt, "art-v1", 128, true}, // ee1c70cc2d051c7-f99728affcc0c87e-00001554.png
	{0xf27309316a765362ULL, 0xc9ecd8a5dfd1e7a8ULL, 0x00001593, 243, 64, 32, nullptr, kRecipeArt, "art-v1", 128, true}, // f27309316a765362-c9ecd8a5dfd1e7a8-00001593.png
	{0xf63e59bcf0de1b62ULL, 0x3d996273c385cb41ULL, 0x000019d3, 244, 128, 64, nullptr, kRecipeArt, "art-v1", 128, true}, // f63e59bcf0de1b62-3d996273c385cb41-000019d3.png
	{0xf69a26d49dcec805ULL, 0x3d996273c385cb41ULL, 0x000019d3, 245, 128, 64, nullptr, kRecipeArt, "art-v1", 128, true}, // f69a26d49dcec805-3d996273c385cb41-000019d3.png
	{0xfcd3d70cc5ab83ccULL, 0xa1ade0be8b8a3e2dULL, 0x00001994, 246, 64, 64, nullptr, kRecipeArt, "art-v1", 128, true}, // fcd3d70cc5ab83cc-a1ade0be8b8a3e2d-00001994.png
};
constexpr char kRecipe[] = "raster-edge-v1";

struct SfnStore
{
	bool present = false;
	std::string digest16 = "missing";
	std::vector<GE1FontFilter::SfnRow> rows;
};

std::mutex s_lock;
std::once_flag s_init_once;
bool s_initialized = false;
bool s_enabled = false;
bool s_prescale = false;
unsigned s_generation = 0;
unsigned s_applied_seq = 0;
SfnStore s_sfn[2]; // [0] atlas 2, [1] atlas 72
std::string s_cache_root_override;
Stats s_stats;

int SfnIndex(int atlas)
{
	return atlas == 2 ? 0 : (atlas == 72 ? 1 : -1);
}

std::string Hex8(std::uint64_t v)
{
	char buf[17];
	std::snprintf(buf, sizeof(buf), "%08llx", static_cast<unsigned long long>(v & 0xffffffffULL));
	return std::string(buf);
}

std::string Hex16(std::uint64_t v)
{
	char buf[17];
	std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
	return std::string(buf);
}

std::string CacheRootLocked()
{
	if (!s_cache_root_override.empty())
		return s_cache_root_override;
	// EmuFolders::Cache is only populated through the settings UI path,
	// which GE1 never runs; fall back to the platform data root, else run
	// uncached (generate every time — correct, just slower).
	std::string base = EmuFolders::Cache;
	if (base.empty())
	{
		const char* d = std::getenv("GE1_GS_DATA_DIR");
		if (d && *d)
			base = d;
	}
	if (base.empty())
		base = EmuFolders::DataRoot;
	if (base.empty())
		return std::string();
	return Path::Combine(Path::Combine(base, "ondevice-upscale"), "v1");
}

bool ReadFile(const std::string& path, std::vector<std::uint8_t>& out)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	std::fseek(f, 0, SEEK_END);
	const long len = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	if (len < 0)
	{
		std::fclose(f);
		return false;
	}
	out.resize(static_cast<std::size_t>(len));
	const std::size_t got = std::fread(out.data(), 1, out.size(), f);
	std::fclose(f);
	return got == out.size();
}

bool WriteFileAtomic(const std::string& path, const std::uint8_t* data, std::size_t len)
{
	const std::string tmp = path + ".tmp";
	FILE* f = std::fopen(tmp.c_str(), "wb");
	if (!f)
		return false;
	const std::size_t wrote = std::fwrite(data, 1, len, f);
	std::fclose(f);
	if (wrote != len)
		return false;
	return FileSystem::RenamePath(tmp.c_str(), path.c_str(), nullptr);
}

// Superseded recipe dirs (explicit group + recipe pairs). Removed once at
// provider start so stale bytes can never warm-hit: only the provider's own
// cache root is touched, only provider-written extensions are deleted, and
// the dir itself is removed only when empty. Every removal is logged.
constexpr const char* kStaleRecipeDirs[][2] = {
	{"art-v1", "lanczos-a-v1"},
};

void RemoveStaleRecipeDirsLocked(const std::string& root)
{
	if (root.empty())
		return;
	// One pattern per provider-written extension, WITH the FILES flag:
	// flags 0 lists nothing (neither FILES nor FOLDERS bit set), which is
	// why the first version of this cleanup removed 0. TrimCacheLocked has
	// the same latent flags-0 shape (dormant: its budget never fires).
	constexpr const char* kOurs[] = {"*.png", "*.manifest", "*.tmp"};
	for (const auto& stale : kStaleRecipeDirs)
	{
		const std::string dir = Path::Combine(Path::Combine(root, stale[0]), stale[1]);
		int removed = 0;
		bool present = false;
		for (const char* pat : kOurs)
		{
			FileSystem::FindResultsArray files;
			if (!FileSystem::FindFiles(dir.c_str(), pat, FILESYSTEM_FIND_FILES, &files, nullptr))
				continue; // absent (or unreadable): nothing to do
			present = true;
			for (const auto& f : files)
			{
				if (f.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY)
					continue;
				// NOTE: FileName is already the full path (no RELATIVE_PATHS
				// flag); combining with dir again breaks the delete. The
				// same latent shape exists in TrimCacheLocked (dormant:
				// the 256MB budget never fires), left untouched.
				if (FileSystem::DeleteFilePath(f.FileName.c_str(), nullptr))
					removed++;
			}
		}
		if (!present)
			continue;
		const bool rmdir = FileSystem::DeleteDirectory(dir.c_str());
		Console.WriteLn("ODU: stale recipe dir %s/%s: removed %d files, dir %s.", stale[0], stale[1],
			removed, rmdir ? "removed" : "kept");
	}
}

// Deletes oldest PNGs (with manifests) while the recipe dir exceeds budget.
void TrimCacheLocked(const std::string& dir)
{
	constexpr std::int64_t kBudget = 256LL * 1024 * 1024;
	FileSystem::FindResultsArray files;
	if (!FileSystem::FindFiles(dir.c_str(), "*.png", 0, &files, nullptr))
		return;
	std::int64_t total = 0;
	for (const auto& f : files)
		total += f.Size;
	if (total <= kBudget)
		return;
	std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
		return a.ModificationTime < b.ModificationTime;
	});
	for (const auto& f : files)
	{
		if (total <= kBudget)
			break;
		const std::string png = Path::Combine(dir, f.FileName);
		std::string stem = f.FileName;
		if (stem.size() > 4)
			stem.resize(stem.size() - 4);
		FileSystem::DeleteFilePath(png.c_str(), nullptr);
		FileSystem::DeleteFilePath(Path::Combine(dir, stem + ".manifest").c_str(), nullptr);
		total -= f.Size;
	}
}

std::string CacheKey(const RegistryRow& row, const std::string& sfn_digest)
{
	return "f" + std::to_string(row.atlas) + "-" + Hex16(row.tex0hash) + "-" + Hex16(row.cluthash) + "-" +
	       Hex8(row.suffix) + "-" + sfn_digest + "-" + row.recipe;
}

struct BaseImage
{
	std::vector<std::uint8_t> rgba; // tight rows, w*h*4
	int w = 0, h = 0;
};

// Validated PNG load into tight rows. Same admission as the old warm path
// (Color format, exact dims, non-empty); the pitch guard keeps the tighten
// memcpy in-bounds if a loader ever pads rows.
bool LoadCachedImage(const std::string& png_path, int expect_w, int expect_h, BaseImage& base)
{
	GSTextureReplacements::ReplacementTextureLoader loader =
		GSTextureReplacements::GetLoader(png_path);
	GSTextureReplacements::ReplacementTexture cached;
	if (!loader || !loader(png_path, &cached, true) ||
		cached.format != GSTexture::Format::Color ||
		cached.width != static_cast<std::uint32_t>(expect_w) ||
		cached.height != static_cast<std::uint32_t>(expect_h) || cached.data.empty() ||
		cached.pitch < cached.width * 4)
		return false;
	base.rgba.resize(static_cast<std::size_t>(expect_w) * expect_h * 4);
	for (int yy = 0; yy < expect_h; yy++)
	{
		std::memcpy(base.rgba.data() + static_cast<std::size_t>(yy) * expect_w * 4,
			cached.data.data() + static_cast<std::size_t>(yy) * cached.pitch,
			static_cast<std::size_t>(expect_w) * 4);
	}
	base.w = expect_w;
	base.h = expect_h;
	return true;
}

// Cold 4x generation for one registry row: shared decode primitive, recipe
// filter, private 4x cache write. Art and words rows both land here, so the
// shipped 4x path and the prescale base share bytes. reason: 0 ok, 1 decode
// fail, 2 geometry fail (callers map to skipped_decode/skipped_geometry).
bool GenerateBase4x(const RegistryRow& row, const SfnStore* sfn, const GIFRegTEX0& TEX0,
	const GIFRegTEXA& TEXA, GSTextureCache::SourceRegion region, GSLocalMemory& mem,
	const std::string& dir, const std::string& ckey, const std::string& sfn_digest, bool cache_on,
	BaseImage& base, GE1FontFilter::Stats& fstat, int& reason)
{
	reason = 1;
	bool decoded = false;
	std::vector<std::uint8_t> tight;
	std::uint32_t dw = 0, dh = 0;
	const bool ok = GSTextureReplacements::WithDecodedSourceLevel(
		TEX0, TEXA, region, mem,
		[&](const std::uint8_t* pixels, std::uint32_t w, std::uint32_t h, std::uint32_t pitch) {
			if (w != row.expect_w || h != row.expect_h)
				return false;
			tight.resize(static_cast<std::size_t>(w) * h * 4);
			for (std::uint32_t yy = 0; yy < h; yy++)
			{
				std::memcpy(tight.data() + static_cast<std::size_t>(yy) * w * 4,
					pixels + static_cast<std::size_t>(yy) * pitch, static_cast<std::size_t>(w) * 4);
			}
			dw = w;
			dh = h;
			decoded = true;
			return true;
		});
	if (!ok || !decoded)
		return false;
	if (dw != row.expect_w || dh != row.expect_h)
	{
		reason = 2;
		return false;
	}

	std::vector<std::uint8_t> out_rgba;
	int ow = 0, oh = 0;
	bool built = false;
	if (std::strcmp(row.recipe, kRecipeArt) == 0)
	{
		double art_ms = 0.0;
		built = GE1Class3Filter::BuildLanczosAtlas(tight.data(), static_cast<int>(dw), static_cast<int>(dh),
			row.alpha_clamp, out_rgba, ow, oh, &art_ms);
		fstat.ms = art_ms;
	}
	else
	{
		const GE1FontFilter::SfnRow* sfn_rows = sfn ? sfn->rows.data() : nullptr;
		const int sfn_n = sfn ? static_cast<int>(sfn->rows.size()) : 0;
		built = GE1FontFilter::BuildFontAtlas(row.atlas, tight.data(), static_cast<int>(dw), static_cast<int>(dh),
			sfn_rows, sfn_n, out_rgba, ow, oh, &fstat);
	}
	if (!built)
	{
		reason = 2;
		return false;
	}

	// Persist privately (temp + rename, manifest last); loss is normal.
	// Written with the platform zlib: decode-equal to the oracle's PNGs
	// everywhere, byte-equal where the compressor matches (Mac harness
	// proves byte-equality against zlib-ng 1.3.1).
	if (cache_on)
	{
		const std::string png_path = Path::Combine(dir, ckey + ".png");
		const std::string man_path = Path::Combine(dir, ckey + ".manifest");
		Error err;
		FileSystem::CreateDirectoryPath(dir.c_str(), true, &err);
		const std::string tmp_png = png_path + ".tmp";
		if (GE1FontFilter::WriteFontPNG(tmp_png, out_rgba.data(), ow, oh) &&
			FileSystem::RenamePath(tmp_png.c_str(), png_path.c_str(), nullptr))
		{
			char man[256];
			std::snprintf(man, sizeof(man),
				"ondevice-upscale-manifest-v1\nrecipe=%s sharp=1.0 thresh=64 mode=smoothstep\n"
				"tex0=%s clut=%s suffix=%s\nsfn=%s\ndims=%ux%u\n",
				row.recipe, Hex16(row.tex0hash).c_str(), Hex16(row.cluthash).c_str(),
				Hex8(row.suffix).c_str(), sfn_digest.c_str(), static_cast<unsigned>(ow),
				static_cast<unsigned>(oh));
			const std::string tmp_man = man_path + ".tmp";
			WriteFileAtomic(tmp_man, reinterpret_cast<const std::uint8_t*>(man), std::strlen(man));
			FileSystem::RenamePath(tmp_man.c_str(), man_path.c_str(), nullptr);
			TrimCacheLocked(dir);
		}
	}

	reason = 0;
	base.rgba = std::move(out_rgba);
	base.w = ow;
	base.h = oh;
	return true;
}

// Upload-shape fill from tight rows: alpha bounds from the actual pixels
// (same policy as packs), Color (RGBA8) like the pack PNG loader produces,
// RMT9 chain only for mip_chain rows. Returns the chain length (0 none).
int ServeImage(BaseImage& img, bool mip_chain,
	GSTextureReplacements::ReplacementTexture& out, std::pair<std::uint8_t, std::uint8_t>* alpha_minmax)
{
	unsigned lo = 255, hi = 0;
	for (std::size_t i = 3; i < img.rgba.size(); i += 4)
	{
		lo = img.rgba[i] < lo ? img.rgba[i] : lo;
		hi = img.rgba[i] > hi ? img.rgba[i] : hi;
	}
	*alpha_minmax = {static_cast<std::uint8_t>(lo), static_cast<std::uint8_t>(hi)};
	out.width = static_cast<std::uint32_t>(img.w);
	out.height = static_cast<std::uint32_t>(img.h);
	// Color (RGBA8) like the pack PNG loader produces, not UNorm8 (A8).
	out.format = GSTexture::Format::Color;
	out.alpha_minmax = *alpha_minmax;
	out.pitch = static_cast<std::uint32_t>(img.w) * 4;
	out.data.assign(img.rgba.begin(), img.rgba.end());
	if (mip_chain)
	{
		const int levels = AppendMipChain(img.rgba.data(), img.w, img.h, out.mips);
		out.generated = true;
		return levels;
	}
	return 0;
}

} // namespace

void EnsureInitialized()
{
	std::call_once(s_init_once, InitializeFromEnvironment);
}

void InitializeFromEnvironment()
{
	std::unique_lock<std::mutex> lock(s_lock);
	if (s_initialized)
		return;
	s_initialized = true;
	const char* e = std::getenv("GE1_ONDEVICE_UPSCALE");
	s_enabled = (e && std::strcmp(e, "1") == 0);
	// Prescale is init-time only (default off): the lookup path never polls
	// getenv, and GPU entries are per-boot, so no live purge is needed.
	const char* pre = std::getenv("GE1_ONDEVICE_UPSCALE_PRESCALE");
	s_prescale = (pre && std::strcmp(pre, "1") == 0);
	if (s_enabled)
		s_generation = 1;
	const char* sfn_dir = std::getenv("GE1_ONDEVICE_SFN_DIR");
	int loaded = 0;
	if (sfn_dir && *sfn_dir)
	{
		for (const RegistryRow& row : kRegistry)
		{
			if (!row.sfn_file)
				continue;
			const char* base = std::strrchr(row.sfn_file, '/');
			const std::string path = Path::Combine(sfn_dir, base ? base + 1 : row.sfn_file);
			std::vector<std::uint8_t> bytes;
			if (ReadFile(path, bytes))
			{
				lock.unlock();
				const bool ok = SetSfnData(row.atlas, bytes.data(), bytes.size());
				lock.lock();
				loaded += ok ? 1 : 0;
			}
		}
	}
	RemoveStaleRecipeDirsLocked(CacheRootLocked());
	Console.WriteLn("ODU: on-device upscale %s prescale %s (sfn %d/2, cache %s).", s_enabled ? "ON" : "off",
		s_prescale ? "ON" : "off", loaded, CacheRootLocked().c_str());
}

void ApplyOnDeviceUpscale(bool enabled, unsigned sequence)
{
	std::unique_lock<std::mutex> lock(s_lock);
	if (sequence <= s_applied_seq)
		return; // stale command
	s_applied_seq = sequence;
	s_enabled = enabled;
	s_generation++;
	Console.WriteLn("ODU: applied %s (seq %u, gen %u).", enabled ? "ON" : "off", sequence, s_generation);
}

bool IsEnabled()
{
	// The lookup gate asks before any miss runs: initialize here so the
	// open-time environment is honored on the very first lookup.
	EnsureInitialized();
	std::unique_lock<std::mutex> lock(s_lock);
	return s_enabled;
}

unsigned Generation()
{
	std::unique_lock<std::mutex> lock(s_lock);
	return s_generation;
}

bool PrescaleEnabled()
{
	EnsureInitialized();
	std::unique_lock<std::mutex> lock(s_lock);
	return s_prescale;
}

// Render-scale target for a words row (stock dims x live UpscaleMultiplier,
// rounded like the Part 2 PRE arm: 617 px @2.41 for 256 px, 512 px @2x).
// False when prescale is off, the row is not a words row, the scale is
// unusable, or the scale is an integer >= 4 (the 4x base minifies cleanly).
// Call with s_lock held (reads s_prescale); GSConfig is read lock-free.
bool PrescaleTargetLocked(const RegistryRow& row, int& W, int& H, float& scale)
{
	if (!s_prescale || std::strcmp(row.recipe, kRecipeWords) != 0)
		return false;
	const float s = GSConfig.UpscaleMultiplier;
	if (!(s > 0.0f))
		return false;
	const float r = std::round(s);
	if (r == s && r >= 4.0f)
		return false;
	W = static_cast<int>(std::lrint(static_cast<double>(row.expect_w) * s));
	H = static_cast<int>(std::lrint(static_cast<double>(row.expect_h) * s));
	if (W <= 0 || H <= 0)
		return false;
	scale = s;
	return true;
}

bool SetSfnData(int atlas, const std::uint8_t* data, std::size_t len)
{
	const int idx = SfnIndex(atlas);
	if (idx < 0 || !data || len == 0)
		return false;
	std::vector<GE1FontFilter::SfnRow> rows;
	if (!GE1FontFilter::ParseSfn(data, len, rows) || rows.empty())
		return false;
	std::unique_lock<std::mutex> lock(s_lock);
	s_sfn[idx].present = true;
	s_sfn[idx].digest16 = SHA256Digest::HashHex(data, len).substr(0, 16);
	s_sfn[idx].rows = std::move(rows);
	return true;
}

bool HaveSfnData(int atlas)
{
	const int idx = SfnIndex(atlas);
	if (idx < 0)
		return false;
	std::unique_lock<std::mutex> lock(s_lock);
	return s_sfn[idx].present;
}

// RMT9-chain mip levels on top of a 4x base: each level a Lanczos halve
// of the previous (BuildMipLevel), sizes floor-halved to 1x1, mirroring
// the pack DDS bytes. Warm and cold paths share it, so a cache hit
// regenerates the identical chain instead of serving base-only.
int AppendMipChain(const std::uint8_t* base_rgba, int w, int h,
	std::vector<GSTextureReplacements::ReplacementTexture::MipData>& mips)
{
	std::vector<std::uint8_t> prev(base_rgba, base_rgba + static_cast<std::size_t>(w) * h * 4);
	int pw = w, ph = h, levels = 0;
	for (int guard = 0; guard < 16; guard++)
	{
		std::vector<std::uint8_t> next;
		int nw = 0, nh = 0;
		if (!GE1Class3Filter::BuildMipLevel(prev.data(), pw, ph, next, nw, nh, nullptr))
			break;
		GSTextureReplacements::ReplacementTexture::MipData md;
		md.width = static_cast<std::uint32_t>(nw);
		md.height = static_cast<std::uint32_t>(nh);
		md.pitch = static_cast<std::uint32_t>(nw) * 4;
		md.data = std::move(next);
		mips.push_back(std::move(md));
		prev.assign(mips.back().data.begin(), mips.back().data.end());
		pw = nw;
		ph = nh;
		levels++;
	}
	return levels;
}

bool LookupGenerated(const GSTextureCache::HashCacheKey& key, const GIFRegTEX0& TEX0,
	const GIFRegTEXA& TEXA, GSTextureCache::SourceRegion region, GSLocalMemory& mem, bool mipmap,
	GSTextureReplacements::ReplacementTexture& out, std::pair<std::uint8_t, std::uint8_t>* alpha_minmax)
{
	EnsureInitialized();

	std::unique_lock<std::mutex> lock(s_lock);
	s_stats.lookups++;
	if (!s_enabled)
	{
		s_stats.skipped_disabled++;
		return false;
	}
	if (FMVstarted)
	{
		s_stats.skipped_fmv++;
		return false;
	}
	// LOD ranges are served from the generated base exactly like a pack
	// texture without mip files (the device regenerates mips when asked),
	// except mip_chain rows, which carry the pack's own chain.
	// Only sub-region lookups stay stock; the registry rows are full-texture.
	(void)mipmap;
	if (key.region_width != 0 || key.region_height != 0)
	{
		s_stats.skipped_region++;
		return false;
	}
	// Same guard as the generic upscaler: the CPU CLUT copy must be live.
	if (GSLocalMemory::m_psm[TEX0.PSM].pal > 0 && mem.m_clut.GetGPUTexture())
	{
		s_stats.skipped_gpu_clut++;
		return false;
	}
	// Suffix bits use the key's masked TEXA (zero for paletted formats),
	// exactly like CreateTextureName — never the raw guest TEXA.
	const std::uint32_t suffix = static_cast<std::uint32_t>(TEX0.PSM) |
		(static_cast<std::uint32_t>(TEX0.TW) << 6) | (static_cast<std::uint32_t>(TEX0.TH) << 10) |
		(static_cast<std::uint32_t>(key.TEXA.TA0) << 15) | (static_cast<std::uint32_t>(key.TEXA.AEM) << 23) |
		(static_cast<std::uint32_t>(key.TEXA.TA1) << 24);
	const RegistryRow* row = nullptr;
	for (const RegistryRow& r : kRegistry)
	{
		if (r.tex0hash == key.TEX0Hash && r.cluthash == key.CLUTHash && r.suffix == suffix)
		{
			row = &r;
			break;
		}
	}
	if (!row)
		return false;
	s_stats.registry_hits++;
	Console.WriteLn("ODU: registry hit atlas %d (tex0=%s clut=%s suffix=%s).",
		row->atlas, Hex16(row->tex0hash).c_str(), Hex16(row->cluthash).c_str(),
		Hex8(row->suffix).c_str());
	const SfnStore* sfn = nullptr;
	std::string sfn_digest = row->cache_group;
	if (row->sfn_file)
	{
		const int idx = SfnIndex(row->atlas);
		if (!s_sfn[idx].present)
		{
			s_stats.skipped_no_sfn++;
			return false;
		}
		sfn = &s_sfn[idx];
		sfn_digest = sfn->digest16;
	}
	const std::string root = CacheRootLocked();
	const bool cache_on = !root.empty();
	const std::string dir =
		cache_on ? Path::Combine(Path::Combine(root, sfn_digest), row->recipe) : std::string();
	const std::string ckey = CacheKey(*row, sfn_digest);
	const std::string png_path = cache_on ? Path::Combine(dir, ckey + ".png") : std::string();
	const std::string man_path = cache_on ? Path::Combine(dir, ckey + ".manifest") : std::string();
	int pre_w = 0, pre_h = 0;
	float pre_scale = 0.0f;
	const bool want_prescale = PrescaleTargetLocked(*row, pre_w, pre_h, pre_scale);
	lock.unlock();

	// Prescale path (words rows only): serve the 4x bytes Lanczos-resampled
	// to the live render scale, cached per (texture, scale). A scale change
	// misses the prescale file and rebuilds from the shared 4x bytes, so
	// stale-scale pixels are never served.
	if (want_prescale)
	{
		const std::string pdir =
			cache_on ? Path::Combine(Path::Combine(root, sfn_digest), kPrescaleGroup) : std::string();
		const std::string pkey =
			ckey + "-pre" + std::to_string(pre_w) + "x" + std::to_string(pre_h);
		const std::string pre_png = cache_on ? Path::Combine(pdir, pkey + ".png") : std::string();
		const std::string pre_man = cache_on ? Path::Combine(pdir, pkey + ".manifest") : std::string();
		BaseImage pre;
		if (cache_on && FileSystem::FileExists(pre_png.c_str()) && FileSystem::FileExists(pre_man.c_str()) &&
			LoadCachedImage(pre_png, pre_w, pre_h, pre))
		{
			ServeImage(pre, false, out, alpha_minmax);
			lock.lock();
			s_stats.prescale_hits++;
			lock.unlock();
			Console.WriteLn("ODU: prescale hit atlas %d (%dx%d, scale %.4f).", row->atlas, pre_w, pre_h,
				static_cast<double>(pre_scale));
			return true;
		}
		if (cache_on && (FileSystem::FileExists(pre_png.c_str()) || FileSystem::FileExists(pre_man.c_str())))
		{
			lock.lock();
			s_stats.cache_corrupt++;
			lock.unlock();
		}
		// Base 4x: shared file first, else generate (persisting the 4x).
		BaseImage base;
		const int bw = static_cast<int>(row->expect_w) * 4, bh = static_cast<int>(row->expect_h) * 4;
		bool have_base = cache_on && LoadCachedImage(png_path, bw, bh, base);
		GE1FontFilter::Stats fstat;
		if (!have_base)
		{
			int reason = 1;
			have_base = GenerateBase4x(*row, sfn, TEX0, TEXA, region, mem, dir, ckey, sfn_digest,
				cache_on, base, fstat, reason);
			if (!have_base)
			{
				lock.lock();
				if (reason == 2)
					s_stats.skipped_geometry++;
				else
					s_stats.skipped_decode++;
				lock.unlock();
				return false;
			}
		}
		double rms = 0.0;
		BaseImage scaled;
		if (!GE1Class3Filter::PillowResizeRGBA(base.rgba.data(), base.w, base.h, scaled.rgba, pre_w,
				pre_h, &rms))
			return false;
		scaled.w = pre_w;
		scaled.h = pre_h;
		std::uint64_t pre_bytes = 0;
		if (cache_on)
		{
			Error err;
			FileSystem::CreateDirectoryPath(pdir.c_str(), true, &err);
			const std::string tmp_png = pre_png + ".tmp";
			if (GE1FontFilter::WriteFontPNG(tmp_png, scaled.rgba.data(), pre_w, pre_h) &&
				FileSystem::RenamePath(tmp_png.c_str(), pre_png.c_str(), nullptr))
			{
				char man[384];
				std::snprintf(man, sizeof(man),
					"ondevice-upscale-manifest-v1\nrecipe=%s sharp=1.0 thresh=64 mode=smoothstep\n"
					"tex0=%s clut=%s suffix=%s\nsfn=%s\nscale=%.4f\ndims=%dx%d\n",
					kRecipeWordsPrescale, Hex16(row->tex0hash).c_str(), Hex16(row->cluthash).c_str(),
					Hex8(row->suffix).c_str(), sfn_digest.c_str(), static_cast<double>(pre_scale),
					pre_w, pre_h);
				const std::string tmp_man = pre_man + ".tmp";
				WriteFileAtomic(tmp_man, reinterpret_cast<const std::uint8_t*>(man), std::strlen(man));
				FileSystem::RenamePath(tmp_man.c_str(), pre_man.c_str(), nullptr);
				TrimCacheLocked(pdir);
				std::vector<std::uint8_t> probe;
				if (ReadFile(pre_png, probe))
					pre_bytes = probe.size();
			}
		}
		ServeImage(scaled, false, out, alpha_minmax);
		lock.lock();
		s_stats.prescale_generated++;
		lock.unlock();
		Console.WriteLn("ODU: prescaled atlas %d (%dx%d -> %dx%d, scale %.4f, %.1f ms, %llu bytes).",
			row->atlas, base.w, base.h, pre_w, pre_h, static_cast<double>(pre_scale), rms,
			static_cast<unsigned long long>(pre_bytes));
		return true;
	}

	// 4x path: validated disk cache first (no recomputation, no reboot).
	BaseImage base;
	bool warm = false;
	if (cache_on && FileSystem::FileExists(png_path.c_str()) && FileSystem::FileExists(man_path.c_str()))
	{
		const int bw = static_cast<int>(row->expect_w) * 4, bh = static_cast<int>(row->expect_h) * 4;
		if (LoadCachedImage(png_path, bw, bh, base))
			warm = true;
		else
		{
			lock.lock();
			s_stats.cache_corrupt++;
			lock.unlock();
		}
	}
	if (warm)
	{
		const int warm_levels = ServeImage(base, row->mip_chain, out, alpha_minmax);
		lock.lock();
		s_stats.cache_hits++;
		lock.unlock();
		Console.WriteLn("ODU: cache hit atlas %d (%ux%u, mips=%d).", row->atlas, out.width, out.height,
			warm_levels);
		return true;
	}

	// Cold path: decode the guest level with the shared primitive, filter.
	GE1FontFilter::Stats fstat;
	int reason = 1;
	if (!GenerateBase4x(*row, sfn, TEX0, TEXA, region, mem, dir, ckey, sfn_digest, cache_on, base,
			fstat, reason))
	{
		lock.lock();
		if (reason == 2)
			s_stats.skipped_geometry++;
		else
			s_stats.skipped_decode++;
		lock.unlock();
		return false;
	}
	const int cold_levels = ServeImage(base, row->mip_chain, out, alpha_minmax);
	lock.lock();
	s_stats.generated++;
	lock.unlock();
	Console.WriteLn("ODU: generated atlas %d (%dx%d, cells=%d regions=%d mips=%d %.1f ms).", row->atlas,
		base.w, base.h, fstat.cells, fstat.regions, cold_levels, fstat.ms);
	return true;
}

Stats GetStats()
{
	std::unique_lock<std::mutex> lock(s_lock);
	return s_stats;
}

void SetCacheRootForTests(const std::string& dir)
{
	std::unique_lock<std::mutex> lock(s_lock);
	s_cache_root_override = dir;
}

void ResetForTests()
{
	std::unique_lock<std::mutex> lock(s_lock);
	s_initialized = false;
	s_enabled = false;
	s_prescale = false;
	s_generation = 0;
	s_applied_seq = 0;
	s_sfn[0] = SfnStore();
	s_sfn[1] = SfnStore();
	s_cache_root_override.clear();
	s_stats = Stats();
}

} // namespace GE1OnDeviceUpscale
