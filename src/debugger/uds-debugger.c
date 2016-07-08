/* Copyright (c) 2013-2016 Jeffrey Pfau
 * Copyright (c) 2016 Miguel Ángel Pérez Martínez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef USE_UDS_DEBUGGER

#include "uds-debugger.h"
#include "core/core.h"

static void _udsdInit(struct mDebugger* debugger) {
    struct UDSDebugger* udsd = ((struct UDSDebugger*) debugger;
    struct sockaddr_un addr;

    if ( (udsd->socket_fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		mLOG(DEBUGGER, ERROR, "Couldn't open Unix Domain Socket.");
		return;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (*udsd->path == '\0') {
        *addr.sun_path = '\0';
        strncpy(addr.sun_path+1, udsd->path+1, sizeof(addr.sun_path)-2);
    } else {
        strncpy(addr.sun_path, udsd->path, sizeof(addr.sun_path)-1);
    }    

    if (bind(udsd->socket_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
		mLOG(DEBUGGER, ERROR, "Couldn't bind to Unix Domain Socket.");
		return;
    }

    if (listen(udsd->socket_fd, 1) == -1) {
        mLOG("Listen call on Unix Domain Socket failed.");
        return;
    }

}

static void _udsdPaused(struct mDebugger* debugger) {
    struct UDSDebugger* udsd = ((struct UDSDebugger*) debugger;

    if ( (udsd->client_fd = accept(udsd->socket_fd, NULL, NULL)) == -1) {
      mLOG("Accept call on Unix Domain Socket failed.");      
    }

    write(udsd->client_fd,"hola",5);

    while(1) {

    }

}

void UDSDebuggerCreate(struct UDSDebugger* udsd) { 
	udsd->d.init    = _udsdInit;
	udsd->d.deinit  = 0;//_udsdDeinit;
	udsd->d.paused  = _udsdPaused;
	udsd->d.entered = 0;//_udsdEntered;
	udsd->d.custom  = 0;//_udsdPoll;	

    udsd->socket_fd = -1;
    udsd->client_fd = -1;
    udsd->path      = "\0mgba_uds_debugger";
}

#endif
