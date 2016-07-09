/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "main.h"

#ifdef USE_CLI_DEBUGGER
#include "debugger/cli-debugger.h"
#endif

#ifdef USE_UDS_DEBUGGER
#include "debugger/uds-debugger.h"
#endif

#ifdef USE_GDB_STUB
#include "debugger/gdb-stub.h"
#endif

#include "core/core.h"
#include "core/config.h"
#include "core/input.h"
#include "core/thread.h"
#include "gba/input.h"
#ifdef M_CORE_GBA
#include "gba/core.h"
#include "gba/gba.h"
#include "gba/video.h"
#endif
#ifdef M_CORE_GB
#include "gb/core.h"
#include "gb/gb.h"
#include "gb/video.h"
#endif
#include "feature/commandline.h"
#include "util/configuration.h"
#include "util/vfs.h"

#include <SDL.h>

#include <errno.h>
#include <signal.h>
#include <sys/time.h>

#define PORT "sdl"

static bool mSDLInit(struct mSDLRenderer* renderer);
static void mSDLDeinit(struct mSDLRenderer* renderer);

static int mSDLRun(struct mSDLRenderer* renderer, struct mArguments* args);

int main(int argc, char** argv) {
	struct mSDLRenderer renderer = {};

	struct mCoreOptions opts = {
		.useBios = true,
		.rewindEnable = true,
		.audioBuffers = 512,
		.videoSync = false,
		.audioSync = true,
		.volume = 0x100,
	};

	struct mArguments args;
	struct mGraphicsOpts graphicsOpts;

	struct mSubParser subparser;

	initParserForGraphics(&subparser, &graphicsOpts);
	bool parsed = parseArguments(&args, argc, argv, &subparser);
	if (!parsed || args.showHelp) {
		usage(argv[0], subparser.usage);
		freeArguments(&args);
		return !parsed;
	}
	if (args.showVersion) {
		version(argv[0]);
		freeArguments(&args);
		return 0;
	}

	renderer.core = mCoreFind(args.fname);
	if (!renderer.core) {
		printf("Could not run game. Are you sure the file exists and is a compatible game?\n");
		freeArguments(&args);
		return 1;
	}
	renderer.core->desiredVideoDimensions(renderer.core, &renderer.width, &renderer.height);
#ifdef BUILD_GL
	mSDLGLCreate(&renderer);
#elif defined(BUILD_GLES2) || defined(USE_EPOXY)
	mSDLGLES2Create(&renderer);
#else
	mSDLSWCreate(&renderer);
#endif

	renderer.ratio = graphicsOpts.multiplier;
	if (renderer.ratio == 0) {
		renderer.ratio = 1;
	}
	opts.width = renderer.width * renderer.ratio;
	opts.height = renderer.height * renderer.ratio;

	if (!renderer.core->init(renderer.core)) {
		freeArguments(&args);
		return 1;
	}

	mInputMapInit(&renderer.core->inputMap, &GBAInputInfo);
	mCoreInitConfig(renderer.core, PORT);
	applyArguments(&args, &subparser, &renderer.core->config);

	mCoreConfigLoadDefaults(&renderer.core->config, &opts);
	mCoreLoadConfig(renderer.core);

	renderer.viewportWidth = renderer.core->opts.width;
	renderer.viewportHeight = renderer.core->opts.height;
#if SDL_VERSION_ATLEAST(2, 0, 0)
	renderer.player.fullscreen = renderer.core->opts.fullscreen;
	renderer.player.windowUpdated = 0;
#else
	renderer.fullscreen = renderer.core->opts.fullscreen;
#endif

	renderer.lockAspectRatio = renderer.core->opts.lockAspectRatio;
	renderer.filter = renderer.core->opts.resampleVideo;

	if (!mSDLInit(&renderer)) {
		freeArguments(&args);
		renderer.core->deinit(renderer.core);
		return 1;
	}

	renderer.player.bindings = &renderer.core->inputMap;
	mSDLInitBindingsGBA(&renderer.core->inputMap);
	mSDLInitEvents(&renderer.events);
	mSDLEventsLoadConfig(&renderer.events, mCoreConfigGetInput(&renderer.core->config));
	mSDLAttachPlayer(&renderer.events, &renderer.player);
	mSDLPlayerLoadConfig(&renderer.player, mCoreConfigGetInput(&renderer.core->config));

	int ret;

	// TODO: Use opts and config
	ret = mSDLRun(&renderer, &args);
	mSDLDetachPlayer(&renderer.events, &renderer.player);
	mInputMapDeinit(&renderer.core->inputMap);

	mSDLDeinit(&renderer);

	freeArguments(&args);
	mCoreConfigFreeOpts(&opts);
	mCoreConfigDeinit(&renderer.core->config);
	renderer.core->deinit(renderer.core);

	return ret;
}

int mSDLRun(struct mSDLRenderer* renderer, struct mArguments* args) {
	struct mCoreThread thread = {
		.core = renderer->core
	};
	if (!mCoreLoadFile(renderer->core, args->fname)) {
		return 1;
	}
	mCoreAutoloadSave(renderer->core);
	struct mDebugger* debugger = mDebuggerCreate(args->debuggerType, renderer->core);
	if (debugger) {
#ifdef USE_UDS_DEBUGGER
		if (args->udsSocketPath) {
			((struct UDSDebugger*) debugger)->path = args->udsSocketPath;
		}
#endif
		mDebuggerAttach(debugger, renderer->core);
		mDebuggerEnter(debugger, DEBUGGER_ENTER_MANUAL, NULL);
	}

	if (args->patch) {
		struct VFile* patch = VFileOpen(args->patch, O_RDONLY);
		if (patch) {
			renderer->core->loadPatch(renderer->core, patch);
		}
	} else {
		mCoreAutoloadPatch(renderer->core);
	}

	renderer->audio.samples = renderer->core->opts.audioBuffers;
	renderer->audio.sampleRate = 44100;

	bool didFail = !mSDLInitAudio(&renderer->audio, &thread);
	if (!didFail) {
#if SDL_VERSION_ATLEAST(2, 0, 0)
		mSDLSetScreensaverSuspendable(&renderer->events, renderer->core->opts.suspendScreensaver);
		mSDLSuspendScreensaver(&renderer->events);
#endif
		if (mCoreThreadStart(&thread)) {
			renderer->runloop(renderer, &thread);
			mSDLPauseAudio(&renderer->audio);
			mCoreThreadJoin(&thread);
		} else {
			didFail = true;
			printf("Could not run game. Are you sure the file exists and is a compatible game?\n");
		}

#if SDL_VERSION_ATLEAST(2, 0, 0)
		mSDLResumeScreensaver(&renderer->events);
		mSDLSetScreensaverSuspendable(&renderer->events, false);
#endif

		if (mCoreThreadHasCrashed(&thread)) {
			didFail = true;
			printf("The game crashed!\n");
		}
	}
	renderer->core->unloadROM(renderer->core);
	return didFail;
}

static bool mSDLInit(struct mSDLRenderer* renderer) {
	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		printf("Could not initialize video: %s\n", SDL_GetError());
		return false;
	}

	return renderer->init(renderer);
}

static void mSDLDeinit(struct mSDLRenderer* renderer) {
	mSDLDeinitEvents(&renderer->events);
	mSDLDeinitAudio(&renderer->audio);
#if SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_DestroyWindow(renderer->window);
#endif

	renderer->deinit(renderer);

	SDL_Quit();
}
