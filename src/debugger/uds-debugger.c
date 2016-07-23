/* Copyright (c) 2013-2016 Jeffrey Pfau
 * Copyright (c) 2016 Miguel Ángel Pérez Martínez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef USE_UDS_DEBUGGER

#include "uds-debugger.h"
#include "core/core.h"
#include <sys/socket.h>
#include <sys/un.h>

static void _udsdInit(struct mDebugger* debugger) {
    struct UDSDebugger* udsd = (struct UDSDebugger*) debugger;
    struct sockaddr_un addr;

    if ( (udsd->socket_fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		mLOG(DEBUGGER, ERROR, "Couldn't open Unix Domain Socket.");
		return;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;    
    strncpy(addr.sun_path, udsd->path, sizeof(addr.sun_path)-1); 

    unlink(udsd->path);
    if (bind(udsd->socket_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
		mLOG(DEBUGGER, ERROR, "Couldn't bind to Unix Domain Socket.");
		return;
    }

    if (listen(udsd->socket_fd, 1) == -1) {
        mLOG(DEBUGGER, ERROR,"Listen call on Unix Domain Socket failed.");
        return;
    }

}

static void _udsdDeinit(struct mDebugger* debugger) {
    struct UDSDebugger* udsd = (struct UDSDebugger*) debugger;

    unlink(udsd->path);	    
}

static void _awaitForConnectionIfNeeded(struct UDSDebugger* udsd) {

    if (udsd->client_fd < 0) {
        if ( (udsd->client_fd = accept(udsd->socket_fd, NULL, NULL)) == -1) {
            mLOG(DEBUGGER, ERROR,"Accept call on Unix Domain Socket failed.");            
        }
    }    
}

static bool _clientRead(struct UDSDebugger* udsd, uint8_t *buffer, size_t count) {
    while(1) {        
        ssize_t nRead = read(udsd->client_fd, buffer, count);
        if (nRead < 0) {
            udsd->client_fd = -1;
            return false;
        } else {
            buffer = &buffer[nRead];
            count -= nRead;
            if (count == 0) {
                return true;
            }
        }
    }
}

static bool _clientWrite(struct UDSDebugger* udsd, uint8_t *buffer, size_t count) {
    while(1) {        
        ssize_t nWrote = write(udsd->client_fd, buffer, count);
        if (nWrote < 0) {
            udsd->client_fd = -1;
            return false;
        } else {
            buffer = &buffer[nWrote];
            count -= nWrote;
            if (count == 0) {
                return true;
            }
        }
    }
}


/*

COMMANDS (LITTLE ENDIAN):

C - Continue execution
R u32 u32 - Read $2 bytes from address $1
r u32 - Read register $1 
W u32 u32 ...u8 - Write $2 ammount of bytes (Appendded after the command) to address $1
w u32 u32 - Set register $1 to value $2   
B u32 - Set breakpoint on address $1
b u32 - Remove breakpoint on address $1
T u32 - Set watchpoint on address $1
t u32 - Remove watchpoint on address $1
X u32 - Set execution watchpoint on address $1
x u32 - Remove execution watchpoint on address $1

SERVER MESSAGES:

H BT u32 - A halt ocurred in the program at address $2, caused by (B)reakpoint or wa(T)chpoint
K - Command Ok (Implicit return for commands that don't ask explicitly for data [most of them])
! - Command Not Ok
D u32 ... u8 - A bulk of $1 bytes (Appended after the message) requested by any of the commands. 

*/

static void _udsdPaused(struct mDebugger* debugger) {
    static uint8_t stackBuffer[0xFF];
    struct UDSDebugger* udsd = (struct UDSDebugger*) debugger;

    while(1) {
        _awaitForConnectionIfNeeded(udsd);
        if (_clientRead(udsd,stackBuffer,1)) {
            switch (stackBuffer[0]) {
            case 'C': // - Continue execution
                udsd->d.state = DEBUGGER_RUNNING;
                _clientWrite(udsd,"K",1);
            return;
            }
        }
    }
}


static void _udsdEntered(struct mDebugger* debugger, enum mDebuggerEntryReason reason, struct mDebuggerEntryInfo* info) {
	struct UDSDebugger* udsd = (struct UDSDebugger*) debugger;

    _awaitForConnectionIfNeeded(udsd);
	
    switch (reason) {
	case DEBUGGER_ENTER_MANUAL:
	case DEBUGGER_ENTER_ATTACHED:
    case DEBUGGER_ENTER_BREAKPOINT:
    case DEBUGGER_ENTER_WATCHPOINT:
    case DEBUGGER_ENTER_ILLEGAL_OP:
		break;
    }

}
     
void UDSDebuggerCreate(struct UDSDebugger* udsd) { 
	udsd->d.init    = _udsdInit;
	udsd->d.deinit  = _udsdDeinit;
	udsd->d.paused  = _udsdPaused;
	udsd->d.entered = _udsdEntered;	

    udsd->socket_fd = -1;
    udsd->client_fd = -1;
    udsd->path      = "/tmp/mgba_uds_debugger";    
}

#endif
