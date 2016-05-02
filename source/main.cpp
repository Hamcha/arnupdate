#include <string>
#include <fstream>

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <climits>

#include <3ds.h>

#include "libs.h"

#define WAIT_START while (aptMainLoop() && !(hidKeysDown() & KEY_START)) { gspWaitForVBlank(); hidScanInput(); }

bool redraw = false;
PrintConsole con;
Config config;

/* States */

enum UpdateState {
	UpdateConfirmationScreen,
	Updating,
	UpdateComplete,
	UpdateFailed,
	UpdateAborted,
	Restoring,
	RestoreComplete,
	RestoreFailed
};

enum ChoiceType {
	NoChoice,
	UpdatePayload,
	RestoreBackup
};

struct UpdateChoice {
	ChoiceType type;
	ReleaseVer chosenVersion;
	bool       isHourly;
};

struct UpdateArgs {
	// Detected options
	std::string  currentVersion, backupVersion;
	bool         migrateARN;
	bool         backupExists;

	// Configuration options
	std::string  payloadPath;
	bool         backupExisting;

	// Available data
	ReleaseInfo* stable = nullptr;
	ReleaseInfo* hourly = nullptr;

	// Chosen settings
	UpdateChoice choice;
};

UpdateChoice drawConfirmationScreen(const UpdateArgs args, const bool usingConfig) {
	static bool partialredraw = false;
	static int  selected = 0;
	static int  hourlyOptionStart = INT_MAX, extraOptionStart = INT_MAX;

	bool haveLatest = args.currentVersion == args.stable->name;
	bool backupVersionDetected = args.backupExists && args.backupVersion != "";

	u32 keydown = hidKeysDown();
	if (keydown & (KEY_UP | KEY_DOWN)) {
		partialredraw = true;
		if (keydown & KEY_UP) {
			--selected;
		}
		if (keydown & KEY_DOWN) {
			++selected;
		}
	}
	if (keydown & KEY_A) {
		if (selected < hourlyOptionStart) {
			return UpdateChoice{ UpdatePayload, args.stable->versions[selected], false };
		}
		if (selected < extraOptionStart) {
			return UpdateChoice{ UpdatePayload, args.hourly->versions[selected - hourlyOptionStart], true };
		}
		int extraOptionID = selected - extraOptionStart;

		// Skip restoring backups if they don't exist
		if (!args.backupExists) {
			extraOptionID++;
		}

		switch (extraOptionID) {
		case 0:
			// Restore backup
			return UpdateChoice{ RestoreBackup };
		default:
			// Panic!
			printf("Unknown option selected (??)\n");
			WAIT_START;
			redraw = true;
			selected = 0;
			return UpdateChoice{ NoChoice };
		}
	}

	if (!redraw && !partialredraw) {
		return UpdateChoice{ NoChoice };
	}

	if (redraw) {
		consoleClear();
		menuPrintHeader(&con);

		if (!usingConfig) {
			std::printf("  %sConfiguration not found, using default values%s\n\n", CONSOLE_MAGENTA, CONSOLE_RESET);
		}

		std::printf("  Payload path:   %s%s%s\n", CONSOLE_WHITE, args.payloadPath.c_str(), CONSOLE_RESET);
		std::printf("  Backup payload: %s%s%s\n\n",
			(args.backupExisting ? CONSOLE_GREEN : CONSOLE_RED),
			(args.backupExisting ? "Yes" : "No"),
			CONSOLE_RESET
		);

		if (args.currentVersion != "") {
			std::printf("  Current installed version:    %s%s%s\n", (haveLatest ? CONSOLE_GREEN : CONSOLE_RED), args.currentVersion.c_str(), CONSOLE_RESET);
		} else {
			std::printf("  %sCould not detect current version%s\n\n", CONSOLE_MAGENTA, CONSOLE_RESET);
		}
		if (backupVersionDetected) {
			bool backupIsLatest = args.backupVersion == args.stable->name;
			std::printf("  Current backup version:       %s%s%s\n", (backupIsLatest ? CONSOLE_GREEN : CONSOLE_RED), args.backupVersion.c_str(), CONSOLE_RESET);
		}
		std::printf("  Latest version (from Github): %s%s%s\n", CONSOLE_GREEN, args.stable->name.c_str(), CONSOLE_RESET);

		if (args.hourly != nullptr) {
			std::printf("  Latest hourly build:          %s%s%s\n", CONSOLE_GREEN, args.hourly->name.c_str(), CONSOLE_RESET);
		}

		if (haveLatest) {
			std::printf("\n  You seem to have the latest version already.\n");
		} else {
			std::printf("\n  A new version of Luma3DS is available.\n");
		}

		std::printf("\n  Choose action:\n");

		menuPrintFooter(&con);
	}

	con.cursorX = 0;
	con.cursorY = 11 + (usingConfig ? 0 : 3) + (args.hourly != nullptr ? 1 : 0) + (backupVersionDetected ? 1 : 0);

	int optionCount = args.stable->versions.size() + (args.hourly != nullptr ? args.hourly->versions.size() : 0) + (args.backupExists ? 1 : 0);

	// Wrap around cursor
	while (selected < 0) selected += optionCount;
	selected = selected % optionCount;

	int curOption = 0;
	for (ReleaseVer r : args.stable->versions) {
		printf(curOption == selected ? "   * " : "     ");
		printf("Install %s\n", r.friendlyName.c_str());
		++curOption;
	}

	hourlyOptionStart = curOption;
	if (args.hourly != nullptr) {
		for (ReleaseVer h : args.hourly->versions) {
			printf(curOption == selected ? "   * " : "     ");
			printf("Install %s\n", h.friendlyName.c_str());
			++curOption;
		}
	}

	extraOptionStart = curOption;

	// Extra #0: Restore backup
	if (args.backupExists) {
		printf(curOption == selected ? "   * " : "     ");
		printf("Restore backup\n");
		++curOption;
	}

	redraw = false;
	partialredraw = false;
	return UpdateChoice{ NoChoice };
}

bool backupA9LH(const std::string payloadName) {
	std::ifstream original(payloadName, std::ifstream::binary);
	if (!original.good()) {
		std::printf("Could not open %s\n", payloadName.c_str());
		return false;
	}

	std::string backupName = payloadName + ".bak";
	std::ofstream target(backupName, std::ofstream::binary);
	if (!target.good()) {
		std::printf("Could not open %s\n", backupName.c_str());
		original.close();
		return false;
	}

	target << original.rdbuf();

	original.close();
	target.close();
	return true;
}

bool update(const UpdateArgs args) {
	consoleClear();

	// Back up local file if it exists
	if (!args.backupExisting) {
		std::printf("Payload backup is disabled in config, skipping...\n");
	} else if (!fileExists(args.payloadPath)) {
		std::printf("Original payload not found, skipping backup...\n");
	} else {
		std::printf("Copying %s to %s.bak...\n", args.payloadPath.c_str(), args.payloadPath.c_str());
		gfxFlushBuffers();
		if (!backupA9LH(args.payloadPath)) {
			std::printf("\nCould not backup %s (!!), aborting...\n", args.payloadPath.c_str());
			return false;
		}
	}

	std::printf("Downloading %s\n", args.choice.chosenVersion.url.c_str());
	gfxFlushBuffers();

	u8* payloadData = nullptr;
	size_t offset = 0;
	size_t payloadSize = 0;
	bool ret = releaseGetPayload(args.choice.chosenVersion, args.choice.isHourly, &payloadData, &offset, &payloadSize);
	if (!ret) {
		std::printf("FATAL\nCould not get A9LH payload...\n");
		std::free(payloadData);
		return false;
	}

	if (args.payloadPath != std::string("/") + PAYLOADPATH) {
		std::printf("Requested payload path is not %s, applying path patch...\n", PAYLOADPATH);
		bool res = pathchange(payloadData + offset, payloadSize, args.payloadPath);
		if (!res) {
			std::free(payloadData);
			return false;
		}
	}

	if (args.migrateARN) {
		std::printf("Migrating AuReiNand install to Luma3DS...\n");
		if (!arnMigrate()) {
			std::printf("FATAL\nCould not migrate AuReiNand install (?)\n");
			return false;
		}
	}

	if (!lumaMigratePayloads()) {
		std::printf("WARN\nCould not migrate payloads\n\n");
	}

	std::printf("Saving %s to SD (as %s)...\n", PAYLOADPATH, args.payloadPath.c_str());
	std::ofstream a9lhfile("/" + args.payloadPath, std::ofstream::binary);
	a9lhfile.write((const char*)(payloadData + offset), payloadSize);
	a9lhfile.close();

	std::printf("All done, freeing resources and exiting...\n");
	std::free(payloadData);
	return true;
}

bool restore(const UpdateArgs args) {
	// Rename current payload to .broken
	if (std::rename(args.payloadPath.c_str(), (args.payloadPath + ".broken").c_str()) != 0) {
		std::perror("Can't rename current version");
		return false;
	}
	// Rename .bak to current
	if (std::rename((args.payloadPath + ".bak").c_str(), args.payloadPath.c_str()) != 0) {
		std::perror("Can't rename backup to current payload name");
		return false;
	}
	// Remove .broken
	if (std::remove((args.payloadPath + ".broken").c_str()) != 0) {
		std::perror("WARN: Could not remove current payload, please remove it manually");
	}
	return true;
}

int main() {
	const static char* cfgPaths[] = {
		"/lumaupdater.cfg",
		"/3DS/lumaupdater.cfg",
		"/luma/lumaupdater.cfg",
	};
	const static size_t cfgPathsLen = sizeof(cfgPaths) / sizeof(cfgPaths[0]);

	UpdateState state = UpdateConfirmationScreen;
	ReleaseInfo release, hourly;
	UpdateArgs updateArgs;
	bool nohourly = false; // Used if fetching hourlies fails

	gfxInitDefault();
	httpcInit(0);

	consoleInit(GFX_TOP, &con);
	consoleDebugInit(debugDevice_CONSOLE);

	// Read config file
	bool usingConfig = false;
	for (size_t cfgIndex = 0; !usingConfig && (cfgIndex < cfgPathsLen); ++cfgIndex) {
		LoadConfigError confStatus = config.LoadFile(cfgPaths[cfgIndex]);
		switch (confStatus) {
		case CFGE_NOTEXISTS:
			break;
		case CFGE_UNREADABLE:
			std::printf("FATAL\nConfiguration file is unreadable!\n\nPress START to quit.\n");
			gfxFlushBuffers();
			WAIT_START
			goto cleanup;
		case CFGE_MALFORMED:
			std::printf("FATAL\nConfiguration file is malformed!\n\nPress START to quit.\n");
			gfxFlushBuffers();
			WAIT_START
			goto cleanup;
		case CFGE_NONE:
			std::printf("Configuration file loaded successfully.\n");
			usingConfig = true;
			break;
		}
	}

	// Check required values in config, if existing
	if (usingConfig && !config.Has("payload path")) {
		std::printf("Missing required config value: payload path\n");
		gfxFlushBuffers();
		WAIT_START
		goto cleanup;
	}
	
	if (!usingConfig)
	{
		std::printf("The configuration file could not be found, skipping...\n");
	}

	// Load config values
	updateArgs.payloadPath = config.Get("payload path", PAYLOADPATH);
	updateArgs.backupExisting = tolower(config.Get("backup", "y")[0]) == 'y';

	// Add initial slash to payload path, if missing
	if (updateArgs.payloadPath[0] != '/') {
		updateArgs.payloadPath = "/" + updateArgs.payloadPath;
	}

	// Check that the payload path is valid
	if (updateArgs.payloadPath.length() > MAXPATHLEN) {
		std::printf("\nFATAL\nPayload path is too long!\nIt can contain at most %d characters!\n\nPress START to quit.\n", MAXPATHLEN);
		gfxFlushBuffers();
		WAIT_START
		goto cleanup;
	}

	// Try to detect current version
	std::printf("Trying detection of current payload version...\n");
	updateArgs.currentVersion = versionMemsearch(updateArgs.payloadPath);

	// Detect bak version, if exists
	if (fileExists(updateArgs.payloadPath + ".bak")) {
		updateArgs.backupExists = true;
		updateArgs.backupVersion = versionMemsearch(updateArgs.payloadPath + ".bak");
	}

	// Check for eventual migration from ARN to Luma
	updateArgs.migrateARN = arnVersionCheck(updateArgs.currentVersion);

	try {
		release = releaseGetLatestStable();
	}
	catch (std::string& e) {
		std::printf("%s\n", e.c_str());
		std::printf("\nFATAL ERROR\nFailed to obtain required data.\n\nPress START to exit.\n");
		gfxFlushBuffers();
		WAIT_START
		goto cleanup;
	}

	updateArgs.stable = &release;

	try {
		hourly = releaseGetLatestHourly();
	} catch (std::string& e) {
		std::printf("%s\n", e.c_str());
		std::printf("\nWARN\nCould not obtain latest hourly, skipping...\n");
		nohourly = true;
		gfxFlushBuffers();
	}

	if (!nohourly) {
		updateArgs.hourly = &hourly;
	}

	redraw = true;

	// Main loop
	while (aptMainLoop())
	{
		gspWaitForVBlank();
		hidScanInput();
		u32 kDown = hidKeysDown();

		switch (state) {
		case UpdateConfirmationScreen:
			// Handle aborting updates
			if (kDown & KEY_START) {
				state = UpdateAborted;
				redraw = true;
				break;
			}

			updateArgs.choice = drawConfirmationScreen(updateArgs, usingConfig);
			switch (updateArgs.choice.type) {
			case UpdatePayload:
				state = Updating;
				redraw = true;
				break;
			case RestoreBackup:
				state = Restoring;
				redraw = true;
				break;
			case NoChoice:
			default:
				break;
			}
			break;
		case Updating:
			if (update(updateArgs)) {
				state = UpdateComplete;
			} else {
				state = UpdateFailed;
			}
			redraw = true;
			break;
		case UpdateFailed:
			if (redraw) {
				std::printf("\n  %sUpdate failed%s. Press START to exit.\n", CONSOLE_RED, CONSOLE_RESET);
				redraw = false;
			}
			break;
		case UpdateComplete:
			if (redraw) {
				consoleClear();
				menuPrintHeader(&con);
				std::printf("\n  %sUpdate complete.%s\n", CONSOLE_GREEN, CONSOLE_RESET);
				if (updateArgs.backupExisting) {
					std::printf("\n  In case something goes wrong you can restore\n  the old payload from %s.bak\n", updateArgs.payloadPath.c_str());
				}
				std::printf("\n  Press START to reboot.");
				redraw = false;
			}
			if ((kDown & KEY_START) != 0) {
				// Reboot!
				aptOpenSession();
				APT_HardwareResetAsync();
				aptCloseSession();
			}
			break;
		case Restoring:
			if (restore(updateArgs)) {
				state = RestoreComplete;
			} else {
				state = RestoreFailed;
			}
			redraw = true;
			break;
		case RestoreComplete:
			if (redraw) {
				consoleClear();
				menuPrintHeader(&con);
				std::printf("\n  %sRestore complete.%s\n", CONSOLE_GREEN, CONSOLE_RESET);
				std::printf("\n  Press START to reboot.");
				redraw = false;
			}
			if ((kDown & KEY_START) != 0) {
				// Reboot!
				aptOpenSession();
				APT_HardwareResetAsync();
				aptCloseSession();
			}
			break;
		case RestoreFailed:
			if (redraw) {
				std::printf("\n  %sRestore failed%s. Press START to exit.\n", CONSOLE_RED, CONSOLE_RESET);
				redraw = false;
			}
			break;
		case UpdateAborted:
			if (redraw) {
				std::printf("\n\n  Update aborted. Press START to exit.");
				redraw = false;
			}
			break;
		}


		if (kDown & KEY_START) {
			break; // Exit to HBMenu
		}

		// Flush and swap framebuffers
		gfxFlushBuffers();
		gfxSwapBuffers();
	}

cleanup:
	// Exit services
	httpcExit();
	gfxExit();
	return 0;
}
