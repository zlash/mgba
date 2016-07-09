/* Copyright (c) 2013-2016 Jeffrey Pfau
 * Copyright (c) 2016 Miguel Ángel Pérez Martínez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef UDS_DEBUGGER_H
#define UDS_DEBUGGER_H

#ifdef USE_UDS_DEBUGGER

#include "util/common.h"
#include "debugger/debugger.h"

struct UDSDebugger {
	struct mDebugger d;

	int 	socket_fd;
	int 	client_fd;	
	char 	*path;
};


void UDSDebuggerCreate(struct UDSDebugger*);

#endif

#endif
