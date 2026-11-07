// "Virtua NBA Recompiled" product layer for the standalone (SDL) build.
// - starts the game directly (no emulator menu) from roms/virnba.zip next to the executable
// - explains clearly which user files are missing
// - window title / branding
#include "vnba_product.h"
#include "types.h"
#include "stdclass.h"
#include "oslib/oslib.h"
#include "hw/sh4/aot/aot_boot.h"
#include "hw/sh4/aot/aot_dll_api.h"
#include <nowide/cstdio.hpp>
#include <string>
#include <vector>
#ifdef USE_SDL
#include <SDL.h>
#endif

namespace {

bool fileExists(const std::string& path)
{
	FILE *f = nowide::fopen(path.c_str(), "rb");
	if (f == nullptr)
		return false;
	fclose(f);
	return true;
}

void showError(const std::string& msg)
{
	ERROR_LOG(BOOT, "%s", msg.c_str());
#ifdef USE_SDL
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, VNBA_PRODUCT_NAME, msg.c_str(), nullptr);
#else
	fprintf(stderr, "%s\n", msg.c_str());
#endif
}

} // namespace

// Returns false if the game cannot start (the caller should exit).
bool vnba_product_init()
{
	if (settings.content.path.empty())
	{
		const std::vector<std::string> candidates = {
			get_writable_config_path("roms/virnba.zip"),
			get_writable_config_path("virnba.zip"),
			get_writable_data_path("virnba.zip"),
			"roms/virnba.zip",
			"virnba.zip",
		};
		for (const std::string& c : candidates)
			if (fileExists(c))
			{
				settings.content.path = c;
				break;
			}
		if (settings.content.path.empty())
		{
			showError("No se encuentra el juego / Game not found.\n\n"
				"Copia tu romset de Virtua NBA (virnba.zip) en la carpeta \"roms\" junto a este ejecutable.\n"
				"Copy your Virtua NBA romset (virnba.zip) to the \"roms\" folder next to this executable:\n\n" + get_writable_config_path("roms/virnba.zip"));
			return false;
		}
	}
	// The recompiled game code is not distributed: each user builds vnba_game.dll from their own
	// ROM with the builder (CONSTRUIR_JUEGO.bat / build_game.ps1).
	if (aot_table_count() == 0 && getenv("VNBA_GENERATE") == nullptr && getenv("VNBA_AOT") == nullptr)
	{
		showError("Falta el código del juego (" VNBA_GAME_MODULE ").\n"
			"Game code module (" VNBA_GAME_MODULE ") not found.\n\n"
			"Ejecuta CONSTRUIR_JUEGO.bat (una sola vez) para generarlo a partir de tu ROM.\n"
			"Run BUILD_GAME.bat once to build it from your own ROM.");
		return false;
	}
	// The NAOMI BIOS is only needed the first time, to create the boot snapshot.
	if (!aot_boot_snapshot_available())
	{
		const std::string bios = get_writable_data_path("naomi.zip");
		if (!fileExists(bios) && !fileExists(get_writable_config_path("roms/naomi.zip")) && !aot_boot_use_fallback())
		{
			showError("Primer arranque: falta la BIOS de NAOMI (naomi.zip). Sólo se necesita una vez, "
				"para crear la instantánea de arranque.\n"
				"First start: the NAOMI BIOS (naomi.zip) is missing. It is only needed once, to create the boot snapshot.\n\n" + bios);
			return false;
		}
	}
	NOTICE_LOG(BOOT, "%s: content %s", VNBA_PRODUCT_NAME, settings.content.path.c_str());
	return true;
}
