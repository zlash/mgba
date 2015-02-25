/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef SIO_MESH_H
#define SIO_MESH_H

#include "gba/sio.h"

#include "util/socket.h"
#include "util/threading.h"

// State transitions:
// IDLE ------> PENDING (GBA thread)
// PENDING ---> GOT_START (Network thread)
// GOT_START -> SENT_DATA (GBA thread)
// SENT_DATA -> FINISHED (Network thread)
// FINISHED --> IDLE (GBA thread)
enum TransferState {
	// Set in network thread
	TRANSFER_IDLE = 0,
	TRANSFER_PENDING,
	TRANSFER_GOT_START,
	TRANSFER_SENT_DATA,
	TRANSFER_FINISHED,
	TRANSFER_DEAD = -1
};

struct GBASIOMultiMeshNode {
	struct GBASIODriver d;

	Thread networkThread;
	bool active;
	int port;
	struct Address publicAddress[MAX_GBAS];

	int id;
	int connected;
	Socket mesh[MAX_GBAS];
	uint32_t nonces[MAX_GBAS];

	union {
		struct {
			unsigned : 2;
			unsigned slave : 1;
			unsigned ready : 1;
			unsigned id : 2;
			unsigned error : 1;
			unsigned busy : 1;
			unsigned : 8;
		};
		uint16_t packed;
	} siocnt;

	int transferActive;
	uint16_t transferValues[MAX_GBAS];
	int32_t transferTime;
	int32_t transferStart;

	// These must be protected by the mutex
	int32_t nextEvent;
	int32_t linkCycles;
	enum TransferState transferState;

	Mutex lock;
	Condition dataGBACond;
	Condition dataNetworkCond;
};

bool GBASIOMultiMeshCreateNode(struct GBASIOMultiMeshNode* node, int port, const struct Address* bindAddress);
bool GBASIOMultiMeshNodeConnect(struct GBASIOMultiMeshNode* node, int port, const struct Address* masterAddress, const struct Address* publicAddress);

#endif
