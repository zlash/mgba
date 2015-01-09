/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "sio-mesh.h"

#include "gba.h"
#include "gba-io.h"

/* GBA Mesh protocol
 * =================
 *
 * First byte of packet is the packet type. Packet types are as follows:
 * - 0x00: Reserved
 * - 0x01: Hello
 * - 0x02: Join
 * - 0x03: Leave
 * - 0x10: Transfer start
 * - 0x11: Transfer data
 */

enum PacketType {
	PACKET_HELLO = 0x01,
	PACKET_JOIN = 0x02,
	PACKET_LEAVE = 0x03,
	PACKET_TRANSFER_START = 0x10,
	PACKET_TRANSFER_DATA = 0x11
};

struct PacketHello {
	uint8_t type; // 0x01
	uint8_t id;
	uint32_t sync;
};

struct PacketJoin {
	uint8_t type; // 0x02
	uint8_t id;
	uint16_t port;
	uint32_t ipVersion;
};

struct PacketTransferStart {
	uint8_t type; // 0x10
	uint8_t reserved;
	uint32_t sync;
};

struct PacketTransferData {
	uint8_t type; // 0x11
	uint8_t id;
	uint16_t data;
};

union Packet {
	struct {
		uint8_t type;
		uint8_t data;
	};
	struct PacketHello hello;
	struct PacketJoin join;
	struct PacketTransferStart transferStart;
	struct PacketTransferData transferData;
};

static int GBASIOMultiMeshInit(struct GBASIODriver* driver);
static void GBASIOMultiMeshDeinit(struct GBASIODriver* driver);
static int GBASIOMultiMeshLoad(struct GBASIODriver* driver);
static int GBASIOMultiMeshWriteRegister(struct GBASIODriver* driver, uint32_t address, uint16_t value);
static int32_t GBASIOMultiMeshProcessEvents(struct GBASIODriver* driver, int32_t cycles);

static THREAD_ENTRY _networkThread(void*);
static void _startTransfer(struct GBASIOMultiMeshNode* node);
static void _setupTransfer(struct GBASIOMultiMeshNode* node);
static void _doTransfer(struct GBASIOMultiMeshNode* node);
static void _finishTransfer(struct GBASIOMultiMeshNode* node);
static void _siocntSync(struct GBASIOMultiMeshNode* node);

int GBASIOMultiMeshCreateNode(struct GBASIOMultiMeshNode* node, int port, const struct Address* bindAddress) {
	MutexInit(&node->lock);
	ConditionInit(&node->dataGBACond);
	ConditionInit(&node->dataNetworkCond);
	node->id = 0;
	node->connected = 1;
	node->nextEvent = INT_MAX;
	node->linkCycles = 0;
	node->mesh[1] = INVALID_SOCKET;
	node->mesh[2] = INVALID_SOCKET;
	node->mesh[3] = INVALID_SOCKET;
	node->siocnt.packed = 0;

	node->port = port;
	node->publicAddress[0] = *bindAddress;

	node->d.p = 0;
	node->d.init = GBASIOMultiMeshInit;
	node->d.deinit = GBASIOMultiMeshDeinit;
	node->d.load = GBASIOMultiMeshLoad;
	node->d.unload = 0;
	node->d.writeRegister = GBASIOMultiMeshWriteRegister;
	node->d.processEvents = GBASIOMultiMeshProcessEvents;

	node->mesh[0] = SocketOpenTCP(port, bindAddress);
	if (SOCKET_FAILED(node->mesh[0])) {
		return 0;
	}
	if (SocketListen(node->mesh[0], 2)) {
		SocketClose(node->mesh[0]);
		return 0;
	}
	return 1;
}

int GBASIOMultiMeshNodeConnect(struct GBASIOMultiMeshNode* node, int port, const struct Address* masterAddress, const struct Address* publicAddress) {
	Socket thisSocket = node->mesh[0];
	if (publicAddress) {
		node->publicAddress[0] = *publicAddress;
	}
	node->mesh[0] = SocketConnectTCP(port, masterAddress);
	if (SOCKET_FAILED(node->mesh[0])) {
		node->mesh[0] = thisSocket;
		return 0;
	}
	node->mesh[1] = thisSocket;
	node->id = -1;
	return 1;
}

int GBASIOMultiMeshInit(struct GBASIODriver* driver) {
	struct GBASIOMultiMeshNode* node = (struct GBASIOMultiMeshNode*) driver;
	node->active = 1;
	node->transferActive = 0;
	node->transferState = TRANSFER_IDLE;
	node->transferValues[0] = 0xFFFF;
	node->transferValues[1] = 0xFFFF;
	node->transferValues[2] = 0xFFFF;
	node->transferValues[3] = 0xFFFF;
	node->transferTime = 0;
	return !ThreadCreate(&node->networkThread, _networkThread, node);
}

void GBASIOMultiMeshDeinit(struct GBASIODriver* driver) {
	struct GBASIOMultiMeshNode* node = (struct GBASIOMultiMeshNode*) driver;
	node->active = 0;
	ThreadJoin(node->networkThread);
}

int GBASIOMultiMeshLoad(struct GBASIODriver* driver) {
	struct GBASIOMultiMeshNode* node = (struct GBASIOMultiMeshNode*) driver;
	_siocntSync(node);
	return 1;
}

int GBASIOMultiMeshWriteRegister(struct GBASIODriver* driver, uint32_t address, uint16_t value) {
	struct GBASIOMultiMeshNode* node = (struct GBASIOMultiMeshNode*) driver;
	if (address == REG_SIOCNT) {
		if (value & 0x0080) {
			if (!node->id) {
				MutexLock(&node->lock);
				if (node->transferState != TRANSFER_IDLE) {
					GBALog(node->d.p->p, GBA_LOG_ERROR, "Transfer backed up");
				}
				_setupTransfer(node);
				node->transferState = TRANSFER_PENDING;
				ConditionWake(&node->dataGBACond);
				MutexUnlock(&node->lock);
			} else {
				GBALog(node->d.p->p, GBA_LOG_ERROR, "Slave attempting to commence transfer");
				value &= ~0x0080;
				value |= driver->p->siocnt & 0x0080;
			}
		}
		value &= 0xFF03;
		value |= driver->p->siocnt & 0x00F8;
	}
	return value;
}

int32_t GBASIOMultiMeshProcessEvents(struct GBASIODriver* driver, int32_t cycles) {
	struct GBASIOMultiMeshNode* node = (struct GBASIOMultiMeshNode*) driver;
	MutexLock(&node->lock);
	node->linkCycles += cycles;
	if (node->nextEvent != INT_MAX) {
		node->nextEvent -= cycles;
		if (node->nextEvent <= 0) {
			while (node->transferState == TRANSFER_PENDING) {
				ConditionWait(&node->dataNetworkCond, &node->lock);
			}
			if (node->transferState == TRANSFER_GOT_START) {
				_setupTransfer(node);
				node->nextEvent -= node->linkCycles;
				node->transferState = TRANSFER_SENT_DATA;
				ConditionWake(&node->dataGBACond);
			} else {
				while (node->transferState == TRANSFER_SENT_DATA) {
					ConditionWait(&node->dataNetworkCond, &node->lock);
				}
				if (node->transferState != TRANSFER_FINISHED) {
					GBALog(node->d.p->p, GBA_LOG_ERROR, "SIO entered bad state");
					node->nextEvent = 32;
				}
			}
			if (node->transferState == TRANSFER_FINISHED) {
				_finishTransfer(node);
				node->nextEvent = INT_MAX;
			}
		}
	}
	MutexUnlock(&node->lock);
	return node->nextEvent;
}

static void _setupTransfer(struct GBASIOMultiMeshNode* node) {
	node->transferActive = (1 << node->connected) - 1;
	node->transferActive &= ~(1 << node->id);
	node->transferValues[0] = 0xFFFF;
	node->transferValues[1] = 0xFFFF;
	node->transferValues[2] = 0xFFFF;
	node->transferValues[3] = 0xFFFF;
	node->transferTime = GBASIOCyclesPerTransfer[node->d.p->multiplayerControl.baud][node->connected - 1];
	node->nextEvent = node->transferTime;
}

static void _startTransfer(struct GBASIOMultiMeshNode* node) {
	int i;
	struct PacketTransferStart transfer = {
		.type = PACKET_TRANSFER_START,
		.sync = node->linkCycles
	};
	for (i = 1; i < node->connected; ++i) {
		if (SOCKET_FAILED(node->mesh[i])) {
			continue;
		}
		SocketSend(node->mesh[i], &transfer, sizeof(transfer));
	}
	MutexLock(&node->lock);
	node->linkCycles = 0;
	node->transferState = TRANSFER_SENT_DATA;
	ConditionWake(&node->dataNetworkCond);
	MutexUnlock(&node->lock);

	_doTransfer(node);
}

static void _doTransfer(struct GBASIOMultiMeshNode* node) {
	struct PacketTransferData data = {
		.type = PACKET_TRANSFER_DATA,
		.id = node->id,
		.data = node->d.p->p->memory.io[REG_SIOMLT_SEND >> 1]
	};
	node->transferValues[node->id] = data.data;
	int i;
	for (i = 0; i < node->connected; ++i) {
		if (node->id == i || SOCKET_FAILED(node->mesh[i])) {
			continue;
		}
		SocketSend(node->mesh[i], &data, sizeof(data));
	}
}

static void _finishTransfer(struct GBASIOMultiMeshNode* node) {
	node->d.p->p->memory.io[REG_SIOMULTI0 >> 1] = node->transferValues[0];
	node->d.p->p->memory.io[REG_SIOMULTI1 >> 1] = node->transferValues[1];
	node->d.p->p->memory.io[REG_SIOMULTI2 >> 1] = node->transferValues[2];
	node->d.p->p->memory.io[REG_SIOMULTI3 >> 1] = node->transferValues[3];
	node->transferState = TRANSFER_IDLE;
	node->nextEvent = INT_MAX;
	GBALog(node->d.p->p, GBA_LOG_DEBUG, "Final values: %04X %04X %04X %04X", node->d.p->p->memory.io[REG_SIOMULTI0 >> 1], node->d.p->p->memory.io[REG_SIOMULTI1 >> 1], node->d.p->p->memory.io[REG_SIOMULTI2 >> 1], node->d.p->p->memory.io[REG_SIOMULTI3 >> 1]);
	if (node->d.p->multiplayerControl.irq) {
		GBARaiseIRQ(node->d.p->p, IRQ_SIO);
	}
}

static void _siocntSync(struct GBASIOMultiMeshNode* node) {
	if (node->d.p->activeDriver == &node->d && node->d.p->mode == SIO_MULTI) {
		node->d.p->siocnt &= 0xFF03;
		node->d.p->siocnt |= node->siocnt.packed;
	}
}

static Socket _greet(struct GBASIOMultiMeshNode* node, int port, const struct Address* ipAddress) {
	Socket socket = SocketConnectTCP(port, ipAddress);
	if (SOCKET_FAILED(socket)) {
		return -1;
	}
	struct PacketHello hello = {
		.type = PACKET_HELLO,
		.id = node->id
	};
	if (SocketSend(socket, &hello, sizeof(hello)) != sizeof(hello)) {
		SocketClose(socket);
		return -1;
	}
	return socket;
}

static int  _readIPAddress(Socket socket, int ipVersion, struct Address* ipAddress, int ipAddressSize) {
	int read;
	int toRead;
	int excessRead = 0;
	switch (ipVersion) {
	case 4:
	case 16:
		toRead = ipVersion;
		break;
	default:
		return -1;
	}
	if (toRead > ipAddressSize) {
		excessRead = toRead - ipAddressSize;
		toRead = ipAddressSize;
	}
	read = SocketRecv(socket, ipAddress->ipv6, toRead);
	while (excessRead > 0) {
		uint32_t buffer;
		toRead = sizeof(buffer);
		if (excessRead < toRead) {
			toRead = excessRead;
		}
		int thisRead = SocketRecv(socket, &buffer, toRead);
		if (thisRead > 0) {
			read += thisRead;
			excessRead -= toRead;
		} else {
			return -1;
		}
	}
	return read;
}

static Socket _select(struct GBASIOMultiMeshNode* node, int* id) {
	// TODO: abstract from BSD sockets
	fd_set set;
	fd_set errorSet;
	FD_ZERO(&set);
	FD_ZERO(&errorSet);
	int i;
	Socket maxFd = -1;
	for (i = 0; i < MAX_GBAS; ++i) {
		if (SOCKET_FAILED(node->mesh[i])) {
			continue;
		}
		FD_SET(node->mesh[i], &set);
		FD_SET(node->mesh[i], &errorSet);
		if (node->mesh[i] > maxFd) {
			maxFd = node->mesh[i];
		}
	}
	int ready = select(maxFd + 1, &set, 0, &errorSet, 0);
	if (!ready) {
		return -1;
	}
	for (i = 0; i < MAX_GBAS; ++i) {
		if (SOCKET_FAILED(node->mesh[i])) {
			continue;
		}
		if (FD_ISSET(node->mesh[i], &set) || FD_ISSET(node->mesh[i], &errorSet)) {
			*id = i;
			return node->mesh[i];
		}
	}
	return -1;
}

static void _processTransferStart(struct GBASIOMultiMeshNode* node, struct PacketTransferStart* start) {
	// FIXME: Don't do this only when a transfer starts
	node->siocnt.slave = 1;
	node->siocnt.ready = 1;
	_siocntSync(node);
	GBALog(node->d.p->p, GBA_LOG_DEBUG, "Sync packet: %i -> %i (%i)", node->linkCycles, start->sync, node->linkCycles - start->sync);

	MutexLock(&node->lock);
	node->linkCycles -= start->sync;
	node->transferTime += node->linkCycles;
	node->transferState = TRANSFER_GOT_START;
	node->nextEvent = 0;
	node->d.p->p->cpu->nextEvent = 0;
	while (node->transferState == TRANSFER_GOT_START) {
		// Wait for the GBA thread to set up our state
		ConditionWait(&node->dataGBACond, &node->lock);
	}
	MutexUnlock(&node->lock);

	_doTransfer(node);
}

static void _processTransferData(struct GBASIOMultiMeshNode* node, struct PacketTransferData* data) {
	node->transferValues[data->id] = data->data;
	node->transferActive &= ~(1 << data->id);
	GBALog(node->d.p->p, GBA_LOG_DEBUG, "Data received: %04X %04X %04X %04X (from %i)", node->transferValues[0], node->transferValues[1], node->transferValues[2], node->transferValues[3], data->id);
	if (!node->transferActive) {
		node->siocnt.id = node->id;
		node->siocnt.busy = 0;
		_siocntSync(node);

		MutexLock(&node->lock);
		node->transferState = TRANSFER_FINISHED;
		node->nextEvent = node->transferTime - node->linkCycles;
		node->d.p->p->cpu->nextEvent = 0;
		ConditionWake(&node->dataNetworkCond);
		MutexUnlock(&node->lock);

		GBALog(node->d.p->p, GBA_LOG_DEBUG, "Transfer ended, %i cycles remaining", node->transferTime - node->linkCycles);
	}
}

static THREAD_ENTRY _networkThread(void* context) {
	struct GBASIOMultiMeshNode* node = context;
	while (node->active) {
		int id;
		if (node->connected > 1) {
			if (!node->id) {
				MutexLock(&node->lock);
				while (node->transferState == TRANSFER_IDLE || node->transferState == TRANSFER_FINISHED) {
					ConditionWait(&node->dataGBACond, &node->lock);
				}
				MutexUnlock(&node->lock);
				if (node->transferState == TRANSFER_PENDING) {
					_startTransfer(node);
				}
			}
		}
		if (node->id == -1) {
			// Read Hello packet
			union Packet hello;
			int read = SocketRecv(node->mesh[0], &hello, 1);
			if (read == 1 && hello.type != PACKET_HELLO) {
				GBALog(node->d.p->p, GBA_LOG_ERROR, "Received non-Hello packet from master");
				continue;
			}
			read += SocketRecv(node->mesh[0], &hello.data, sizeof(hello.hello) - 1);
			if (read < (int) sizeof(hello.hello) || hello.hello.id >= MAX_GBAS) {
				GBALog(node->d.p->p, GBA_LOG_ERROR, "Invalid Hello packet from master: %02X%02X (size %u vs %u)", hello.type, hello.hello.id, read, (int) sizeof(hello));
				SocketClose(node->mesh[0]);
				node->mesh[0] = INVALID_SOCKET;
				return 0;
			}
			GBALog(node->d.p->p, GBA_LOG_DEBUG, "Sync (hello) packet: %i -> %i", node->linkCycles, hello.hello.sync);
			MutexLock(&node->lock);
			node->id = hello.hello.id;
			node->mesh[node->id] = node->mesh[1];
			node->linkCycles = hello.hello.sync;
			node->connected = node->id + 1;
			MutexUnlock(&node->lock);

			// Tell the server about me
			struct PacketJoin join = {
				.type = PACKET_JOIN,
				.id = hello.hello.id,
				.port = node->port,
				.ipVersion = 4 // TODO: Support IPv6
			};
			SocketSend(node->mesh[0], &join, sizeof(join));
			SocketSend(node->mesh[0], node->publicAddress, 4);
		}
		Socket socket = _select(node, &id);
		union Packet packet;
		if (SOCKET_FAILED(socket)) {
			continue;
		}
		if (id == node->id) {
			// Process incoming connections
			Socket stranger = SocketAccept(socket, 0);
			if (SOCKET_FAILED(stranger)) {
				GBALog(node->d.p->p, GBA_LOG_ERROR, "Failed connection");
				continue;
			}
			struct PacketHello hello;
			if (node->id) {
				SocketRecv(stranger, &hello, sizeof(hello));
				if (hello.type != PACKET_HELLO || hello.id >= MAX_GBAS || node->mesh[hello.id] != -1) {
					GBALog(node->d.p->p, GBA_LOG_ERROR, "Invalid Hello packet");
					SocketClose(stranger);
					continue;
				}
			} else {
				hello.type = PACKET_HELLO;
				hello.id = node->connected;
				hello.sync = node->linkCycles;
				SocketSend(stranger, &hello, sizeof(hello));
				node->siocnt.slave = 0;
				node->siocnt.ready = 1;
				_siocntSync(node);
				++node->connected;
			}
			node->mesh[hello.id] = stranger;
		} else if (node->connected > 1) {
			if (SocketRecv(socket, &packet, 1) < 1) {
				SocketClose(node->mesh[id]);
				node->mesh[id] = -1;
				// TODO: Reconfigure mesh
				continue;
			}
			GBALog(node->d.p->p, GBA_LOG_DEBUG, "Received packet of type %02X", packet.type);
			switch(packet.type) {
			case PACKET_JOIN: {
				struct Address ipAddress;
				// TODO: Check the return values of these
				SocketRecv(socket, &packet.data, sizeof(struct PacketJoin) - 1);
				_readIPAddress(socket, packet.join.ipVersion, &ipAddress, sizeof(ipAddress));
				if (node->id) {
					if (id != 0) {
						// Ignore Join packets from sources other than master
						GBALog(node->d.p->p, GBA_LOG_ERROR, "Invalid Join packet sender");
						break;
					}
					if (packet.join.id >= MAX_GBAS) {
						GBALog(node->d.p->p, GBA_LOG_ERROR, "Invalid Join packet");
						break;
					}
					if (node->mesh[packet.join.id] >= 0) {
						GBALog(node->d.p->p, GBA_LOG_ERROR, "Redundant Join packet");
						break;
					}
					node->mesh[packet.join.id] = _greet(node, packet.join.port, &ipAddress);
				} else {
					// Broadcast the join packet we get from the client
					if (id != packet.join.id) {
						GBALog(node->d.p->p, GBA_LOG_ERROR, "Invalid Join packet");
						break;
					}
					int i;
					for (i = 1; i < node->connected; ++i) {
						if (i == id) {
							continue;
						}
						SocketSend(node->mesh[i], &packet, sizeof(struct PacketJoin));
						SocketSend(node->mesh[i], &ipAddress, sizeof(ipAddress));
					}
				}
				GBALog(node->d.p->p, GBA_LOG_INFO, "Welcomed player %u", packet.join.id);
				break;
			}
			case PACKET_TRANSFER_START:
				SocketRecv(socket, &packet.data, sizeof(struct PacketTransferStart) - 1);
				if (id) {
					GBALog(node->d.p->p, GBA_LOG_ERROR, "Invalid transfer start");
					continue;
				}
				_processTransferStart(node, &packet.transferStart);
				break;
			case PACKET_TRANSFER_DATA:
				SocketRecv(socket, &packet.data, sizeof(struct PacketTransferData) - 1);
				if (packet.transferData.id != id) {
					GBALog(node->d.p->p, GBA_LOG_ERROR, "Invalid transfer sender");
					continue;
				}
				_processTransferData(node, &packet.transferData);
				break;
			default:
				GBALog(node->d.p->p, GBA_LOG_ERROR, "Invalid packet type: %x", packet.type);
				break;
			}
		}
	}
	return 0;
}
