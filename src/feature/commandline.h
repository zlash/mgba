/* Copyright (c) 2013-2014 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef COMMAND_LINE_H
#define COMMAND_LINE_H

#include "util/common.h"

#include "debugger/debugger.h"

struct mArguments {
	char* fname;
	char* patch;
	char* cheatsFile;
	char* movie;
	char* bios;
	int logLevel;
	int frameskip;

	enum mDebuggerType debuggerType;
	bool debugAtStart;
	bool showHelp;
	bool showVersion;
	
#ifdef USE_UDS_DEBUGGER
	char *udsSocketPath;
#endif
};

struct mCoreConfig;
struct mSubParser {
	const char* usage;
	bool (*parse)(struct mSubParser* parser, int option, const char* arg);
	void (*apply)(struct mSubParser* parser, struct mCoreConfig* config);
	const char* extraOptions;
	void* opts;
};

struct mGraphicsOpts {
	int multiplier;
	bool fullscreen;
};

bool parseArguments(struct mArguments* args, int argc, char* const* argv,
                    struct mSubParser* subparser);
void applyArguments(const struct mArguments* args, struct mSubParser* subparser, struct mCoreConfig* config);
void freeArguments(struct mArguments* args);

void usage(const char* arg0, const char* extraOptions);
void version(const char* arg0);

void initParserForGraphics(struct mSubParser* parser, struct mGraphicsOpts* opts);

#endif
