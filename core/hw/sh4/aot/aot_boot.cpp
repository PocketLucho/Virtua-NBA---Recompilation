// BIOS-free boot for the Virtua NBA recompilation.
//
// The NAOMI BIOS only has to run once: when the game reaches its entry point (gamePC,
// 0x0C020500) the complete machine state is captured into a boot snapshot. Later launches
// restore that snapshot instead of running the BIOS, so naomi.zip is no longer needed.
//
// After the entry point the game never executes BIOS ROM code; it only reads a table of
// pointers to BIOS service routines (resident in RAM, so part of the snapshot) at BIOS
// offset 0x60000, plus a few identification bytes. Those windows are stored in the snapshot.
//
// Environment:
//   VNBA_BOOT_SNAPSHOT=path   snapshot file (default: <data dir>/vnba_boot.state)
//   VNBA_BIOS_BOOT=1          ignore the snapshot and boot through the BIOS
#include "aot_boot.h"
#include "types.h"
#include "serialize.h"
#include "stdclass.h"
#include <zlib.h>
#include <nowide/cstdio.hpp>
#include "hw/flashrom/nvmem.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/mem/addrspace.h"
#include "cfg/option.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <iterator>
#include <cmath>

namespace {

constexpr u32 MAGIC = 0x544F4256;	// "VBOT"
constexpr u32 FORMAT = 2;
// BIOS ROM ranges the game reads after its entry point (measured with VNBA_BIOS_WATCH):
// null-pointer reads of the header, an ID string, the service-routine table and the
// BIOS identification block at the end of the ROM.
struct Window { u32 start, size; };
constexpr Window BIOS_WINDOWS[] = {
	{ 0x000000, 0x100 },
	{ 0x010F00, 0x200 },
	{ 0x060000, 0x8000 },
	{ 0x1FFC00, 0x400 },
};
constexpr u32 windowBytes()
{
	u32 n = 0;
	for (const Window& w : BIOS_WINDOWS)
		n += w.size;
	return n;
}
constexpr u32 GAME_ENTRY = 0x0C020500;

struct Header {
	u32 magic;
	u32 format;
	u32 entry;
	u32 biosWindows;
	u32 biosWindowBytes;
	u32 stateSize;
	u32 compressedSize;
};

bool checked, available, captured, restored;
std::string path;

// config::Region for NAOMI: 0 Japan, 1 USA, 2 Export, 3 Korea
int regionIndex(int region)
{
	return region >= 0 && region <= 3 ? region : 1;
}

std::string regionSnapshotPath(int region)
{
	static const char * const names[] { "jp", "us", "ex", "kr" };
	return get_writable_data_path(std::string("vnba_boot_") + names[regionIndex(region)] + ".state");
}

bool validSnapshot(const std::string& file)
{
	FILE *f = nowide::fopen(file.c_str(), "rb");
	if (f == nullptr)
		return false;
	Header h{};
	bool ok = fread(&h, sizeof(h), 1, f) == 1 && h.magic == MAGIC && h.format == FORMAT && h.entry == GAME_ENTRY;
	fclose(f);
	return ok;
}

void init()
{
	if (checked)
		return;
	checked = true;
	if (const char *p = getenv("VNBA_BOOT_SNAPSHOT"))
		path = p;
	else
	{
		// One snapshot per BIOS region: the game shows region specific screens and texts
		path = regionSnapshotPath(config::Region);
		if (!validSnapshot(path) && regionIndex(config::Region) == 1 && validSnapshot(get_writable_data_path("vnba_boot.state")))
			path = get_writable_data_path("vnba_boot.state");	// snapshot made by earlier versions (USA BIOS)
	}
	if (getenv("VNBA_BIOS_BOOT") != nullptr)
		return;
	available = validSnapshot(path);
	INFO_LOG(BOOT, "VNBA boot snapshot %s: %s", path.c_str(), available ? "found" : "not found");
}

} // namespace

// ---- Widescreen ----
// VNBA_WS_MODE / product default:
//   "cull"    (default) keep the game's 4:3 projection (HUD untouched), let the renderer draw the
//             area outside 4:3 (Flycast widescreen), and widen the game's frustum culling planes so
//             crowd, players and baskets are not discarded at the sides. Done by a hook that runs
//             right after the game computes its camera planes (block 0x0C0C1EF6).
//   "stretch" make the game's camera 16:9 (aspect literals) and stretch the 4:3 frame to 16:9.
//   "off"     original 4:3 behaviour.
extern u32 aot_hook_pc, aot_hook_pc2;
enum class WsMode { Off, Cull, Stretch };
static WsMode wsMode()
{
	static const WsMode m = [] {
		const char *e = getenv("VNBA_WS_MODE");
		if (e == nullptr && getenv("VNBA_NATIVE_WS") != nullptr)
			e = getenv("VNBA_NATIVE_WS")[0] == '0' ? "off" : "stretch";
		if (e != nullptr)
			return strcmp(e, "stretch") == 0 ? WsMode::Stretch : strcmp(e, "cull") == 0 ? WsMode::Cull : WsMode::Off;
#ifdef VNBA_PRODUCT
		return config::Widescreen ? WsMode::Stretch : WsMode::Off;
#else
		return WsMode::Off;
#endif
	}();
	return m;
}

bool vnba_native_widescreen()
{
	return wsMode() != WsMode::Off;
}

constexpr u32 CAMERA_SLOPE_X = 0x0C18E860, CAMERA_SCALE_X = 0x0C18E898, CAMERA_BOUND = 0x0C18E868;
constexpr u32 CAMERA_PLANES = 0x0C2DD5F0;	// 4 planes (x, y, z, d): left, right, top, bottom
constexpr u32 CAMERA_EDGE_DIST = 0x0C2DD630;	// sqrt(f^2 + (W/2)^2): distance to the screen edge
constexpr u32 CAMERA_SCREEN_SCALE = 0x0C2DD64C;	// f in pixels
constexpr float WIDEN = (16.f / 9.f) / (4.f / 3.f);

static float rf(u32 a) { u32 u = addrspace::read32(a); float f; memcpy(&f, &u, 4); return f; }
static void wf(u32 a, float v) { u32 u; memcpy(&u, &v, 4); addrspace::write32(a, u); }

// Runs after the game has computed its camera frustum planes for the current camera
constexpr u32 HOOK_CAMERA = 0x0C0C1EF6;	// camera planes / view slopes computed
constexpr u32 HOOK_PLAYER_CULL = 0x0C05715E;	// fr14/fr15 = horizontal/vertical tan(fov/2) for the player visibility loop

void aot_run_hook(u32 pc, Sh4Context *ctx)
{
	if (wsMode() != WsMode::Cull)
		return;
	if (pc == HOOK_PLAYER_CULL)
	{
		static float extra = getenv("VNBA_WS_PLAYER_X") ? atof(getenv("VNBA_WS_PLAYER_X")) : 1.f;
		ctx->fr[14] *= WIDEN * extra;	// horizontal tan(fov/2); fr15 is the vertical one
		static int dbg = getenv("VNBA_WS_DEBUG") ? 20 : 0;
		if (dbg > 0) { dbg--; fprintf(stderr, "WSHOOK player fr15=%g fr13=%g fr14=%g\n", ctx->fr[15], ctx->fr[13], ctx->fr[14]); }
		return;
	}
	for (u32 plane = 0; plane < 2; plane++)
	{
		u32 a = CAMERA_PLANES + plane * 0x10;
		float c = rf(a), z = rf(a + 8);
		if (c == 0.f)
			continue;
		static float extra = getenv("VNBA_WS_PLANE_X") ? atof(getenv("VNBA_WS_PLANE_X")) : 1.f;
		float t = z / std::fabs(c) * WIDEN * extra;
		float n = 1.f / std::sqrt(1.f + t * t);
		wf(a, c > 0 ? n : -n);
		wf(a + 8, t * n);
	}
	// Horizontal projection scale used by the per-object/strip visibility tests
	// (0x0C0C22E6 / 0x0C0C24EC / 0x0C0C27D0): slopeX, slopeY, distX, distY, bound
	static float lastSx;
	float sx = rf(CAMERA_SLOPE_X);
	if (sx != lastSx)	// not yet widened since the game last computed it
	{
		static float extra = getenv("VNBA_WS_SLOPE_X") ? atof(getenv("VNBA_WS_SLOPE_X")) : 1.f;
		sx /= WIDEN * extra;	// projection scale (f / half-width): smaller = wider view
	}
	lastSx = sx;
	wf(CAMERA_SLOPE_X, sx);
	static int dbg = getenv("VNBA_WS_DEBUG") ? 20 : 0;
	if (dbg > 0) { dbg--; fprintf(stderr, "WSHOOK sx=%g base=%g\n", sx, rf(CAMERA_SCALE_X)); }
	float dx = rf(CAMERA_SCALE_X) * sx;
	float dy = rf(CAMERA_EDGE_DIST + 4);
	wf(CAMERA_EDGE_DIST, dx);
	wf(CAMERA_BOUND, dx > dy ? dx : dy);
}

static void applyGamePatches()
{
	switch (wsMode())
	{
	case WsMode::Off:
		return;
	case WsMode::Cull:
		aot_hook_pc = HOOK_CAMERA;
		aot_hook_pc2 = HOOK_PLAYER_CULL;
		config::Widescreen.override(true);
		config::ScreenStretching.override(100);
		NOTICE_LOG(BOOT, "VNBA: widescreen (game culling widened, HUD 4:3)");
		return;
	case WsMode::Stretch:
		break;
	}
	wf(0x0C04F0CC, 16.f / 9.f);	// full-screen viewport aspect literal (was 4/3)
	wf(0x0C04F0D8, 8.f / 9.f);	// split-screen viewport aspect literal (was 2/3)
	wf(0x0C13ACD8, 16.f / 9.f);	// already-initialized viewport descriptors
	wf(0x0C13AE40, 8.f / 9.f);
	wf(0x0C13AFA8, 8.f / 9.f);
	config::Widescreen.override(false);	// the game now renders the wide view itself
	config::ScreenStretching.override(133);	// 4:3 frame -> 16:9 display
	// The 2D HUD is drawn with sprites: squeeze them so they keep 4:3 proportions
	// once the frame is stretched.
	extern float vnba_sprite_squeeze, vnba_sprite_keep_z;
	vnba_sprite_squeeze = getenv("VNBA_HUD_STRETCH") ? 1.f : 0.75f;
	extern bool vnba_sprite_fill_edges;
	vnba_sprite_fill_edges = getenv("VNBA_2D_NOFILL") == nullptr;
	vnba_sprite_keep_z = getenv("VNBA_HUD_KEEP_Z") ? atof(getenv("VNBA_HUD_KEEP_Z")) : 0.f;
	NOTICE_LOG(BOOT, "VNBA: widescreen (16:9 camera, 4:3 HUD)");
}

bool aot_boot_snapshot_available()
{
	init();
	return available;
}

// No snapshot for the selected region and no BIOS to create it: boot from another region's
// snapshot instead. Returns false if there is none at all.
bool aot_boot_use_fallback()
{
	init();
	if (available || getenv("VNBA_BOOT_SNAPSHOT") != nullptr)
		return available;
	const std::string candidates[] {
		regionSnapshotPath(1), get_writable_data_path("vnba_boot.state"), regionSnapshotPath(2), regionSnapshotPath(0), regionSnapshotPath(3)
	};
	for (const std::string& c : candidates)
		if (validSnapshot(c))
		{
			WARN_LOG(BOOT, "VNBA: no boot snapshot for region %d, using %s", (int)config::Region, c.c_str());
			path = c;
			available = true;
			return true;
		}
	return false;
}

// Called before every block dispatch until the snapshot is captured or restored.
// Returns true if the machine state was replaced (the caller must re-read the context).
// After a reset the machine restarts from the BIOS entry point: boot from the snapshot again.
void aot_boot_rearm()
{
	if (!checked)
		return;
	restored = false;
	if (!available && getenv("VNBA_BIOS_BOOT") == nullptr)
		available = validSnapshot(path);	// captured during this session
}

bool aot_boot_hook(u32 pc)
{
	init();
	if (available && !restored)
	{
		restored = true;
		FILE *raw = nowide::fopen(path.c_str(), "rb");
		Header h{};
		if (raw == nullptr || fread(&h, sizeof(h), 1, raw) != 1)
		{
			if (raw) fclose(raw);
			throw FlycastException("Cannot read boot snapshot " + path);
		}
		if (h.biosWindows != std::size(BIOS_WINDOWS) || h.biosWindowBytes != windowBytes())
		{
			fclose(raw);
			throw FlycastException("Incompatible boot snapshot " + path);
		}
		std::vector<u8> window(h.biosWindowBytes);
		std::vector<u8> packed(h.compressedSize);
		std::vector<u8> state(h.stateSize);
		bool ok = fread(window.data(), 1, window.size(), raw) == window.size()
				&& fread(packed.data(), 1, packed.size(), raw) == packed.size();
		fclose(raw);
		uLongf outLen = state.size();
		if (!ok || uncompress(state.data(), &outLen, packed.data(), packed.size()) != Z_OK || outLen != state.size())
			throw FlycastException("Corrupted boot snapshot " + path);

		Deserializer deser(state.data(), state.size());
		dc_deserialize(deser);
		mmu_set_state();
		u8 *bios = nvmem::getBiosData();
		u32 off = 0;
		for (const Window& w : BIOS_WINDOWS)
		{
			memcpy(bios + w.start, window.data() + off, w.size);
			off += w.size;
		}
		NOTICE_LOG(BOOT, "VNBA: booted from snapshot (BIOS-free), pc %08x", Sh4cntx.pc);
		applyGamePatches();
		return true;
	}
	if (!available && !captured && (pc & 0x1FFFFFFF) == GAME_ENTRY)
	{
		captured = true;
		struct PatchAfterCapture { ~PatchAfterCapture() { applyGamePatches(); } } patchAfter;	// keep the snapshot pristine
		Serializer dry;
		dc_serialize(dry);
		std::vector<u8> state(dry.size());
		Serializer ser(state.data(), state.size());
		dc_serialize(ser);

		uLongf packedLen = compressBound(state.size());
		std::vector<u8> packed(packedLen);
		if (compress2(packed.data(), &packedLen, state.data(), state.size(), 6) != Z_OK)
			return false;
		Header h{ MAGIC, FORMAT, GAME_ENTRY, (u32)std::size(BIOS_WINDOWS), windowBytes(), (u32)state.size(), (u32)packedLen };
		std::vector<u8> window;
		for (const Window& w : BIOS_WINDOWS)
			window.insert(window.end(), nvmem::getBiosData() + w.start, nvmem::getBiosData() + w.start + w.size);
		FILE *raw = nowide::fopen(path.c_str(), "wb");
		if (raw == nullptr)
		{
			WARN_LOG(BOOT, "VNBA: cannot write boot snapshot %s", path.c_str());
			return false;
		}
		bool ok = fwrite(&h, sizeof(h), 1, raw) == 1
				&& fwrite(window.data(), 1, window.size(), raw) == window.size()
				&& fwrite(packed.data(), 1, packedLen, raw) == packedLen;
		fclose(raw);
		if (!ok)
		{
			WARN_LOG(BOOT, "VNBA: error writing boot snapshot %s", path.c_str());
			remove(path.c_str());
			return false;
		}
		NOTICE_LOG(BOOT, "VNBA: boot snapshot saved to %s (%zu bytes of state, %lu compressed)", path.c_str(), state.size(), (unsigned long)packedLen);
		return true;	// at the game entry point: same moment as a snapshot restore
	}
	return false;
}

bool aot_boot_pending()
{
	init();
	return available ? !restored : !captured;
}

bool aot_boot_bios_free()
{
	return restored;
}

bool aot_bios_window(u32 addr)
{
	for (const Window& w : BIOS_WINDOWS)
		if (addr >= w.start && addr < w.start + w.size)
			return true;
	return false;
}
