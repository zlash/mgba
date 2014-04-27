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
static void _siocntSync(struct GBASIOMultiMeshNode* node);
static void _postUpdate(struct GBASIOMultiMeshNode* node);

int GBASIOMultiMeshCreateNode(struct GBASIOMultiMeshNode* node, int port, uint32_t bindAddress) {
	MutexInit(&node->lock);
	node->threadContext = 0;
	node->id = 0;
	node->connected = 1;
	node->mesh[1] = -1;
	node->mesh[2] = -1;
	node->mesh[3] = -1;
	node->transferActive = 0;
	node->transferValues[0] = 0xFFFF;
	node->transferValues[1] = 0xFFFF;
	node->transferValues[2] = 0xFFFF;
	node->transferValues[3] = 0xFFFF;
	node->siocnt.packed = 0;

	node->port = port;
	node->publicAddress[0] = bindAddress;

	node->d.p = 0;
	node->d.init = GBASIOMultiMeshInit;
	node->d.deinit = GBASIOMultiMeshDeinit;
	node->d.load = GBASIOMultiMeshLoad;
	node->d.unload = 0;
	node->d.writeRegister = GBASIOMultiMeshWriteRegister;
	node->d.processEvents = GBASIOMultiMeshProcessEvents;

	node->mesh[0] = SocketOpenTCP(port, bindAddress);
	if (node->mesh[0] < 0) {
		return 0;
	}
	if (SocketListen(node->mesh[0], 2)) {
		SocketClose(node->mesh[0]);
		return 0;
	}
	return 1;
}

int GBASIOMultiMeshNodeConnect(struct GBASIOMultiMeshNode* node, int port, uint32_t masterAddress, uint32_t publicAddress) {
	Socket thisSocket = node->mesh[0];
	if (publicAddress) {
		node->publicAddress[0] = publicAddress;
	}
	node->mesh[0] = SocketConnectTCP(port, masterAddress);
	if (node->mesh[0] < 0) {
		node->mesh[0] = thisSocket;
		return 0;
	}

	// Read Hello packet
	struct PacketHello hello;
	int read = SocketRecv(node->mesh[0], &hello, sizeof(hello));
	if (read < (int) sizeof(hello) || hello.type != PACKET_HELLO || hello.id >= MAX_GBAS) {
		GBALog(node->d.p ? node->d.p->p : 0, GBA_LOG_ERROR, "Invalid Hello packet from master");
		SocketClose(node->mesh[0]);
		node->mesh[0] = thisSocket;
		return 0;
	}
	node->id = hello.id;
	node->mesh[node->id] = thisSocket;

	// Tell the server about me
	struct PacketJoin join = {
		.type = PACKET_JOIN,
		.id = hello.id,
		.port = node->port,
		.ipVersion = 4 // TODO: Support IPv6
	};
	SocketSend(node->mesh[0], &join, sizeof(join));
	SocketSend(node->mesh[0], node->publicAddress, 4);

	return 1;
}

void GBASIOMultiMeshBindThread(struct GBASIOMultiMeshNode* node, struct GBAThread* threadContext) {
	node->threadContext = threadContext;
}

int GBASIOMultiMeshInit(struct GBASIODriver* driver) {
	struct GBASIOMultiMeshNode* node = (struct GBASIOMultiMeshNode*) driver;
	node->active = 1;
	return !ThreadCreate(&node->networkThread, _networkThread, node);
}

void GBASIOMultiMeshDeinit(struct GBASIODriver* driver) {
	struct GBASIOMultiMeshNode* node = (struct GBASIOMultiMeshNode*) driver;
	node->active = 0;
	ThreadJoin(node->networkThread);
	MutexDeinit(&node->lock);
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
				_startTransfer(node);
			} else {
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
	if (node->transferFinished) {
		MutexLock(&node->lock);
		node->d.p->p->memory.io[REG_SIOMULTI0 >> 1] = node->transferValues[0];
		node->d.p->p->memory.io[REG_SIOMULTI1 >> 1] = node->transferValues[1];
		node->d.p->p->memory.io[REG_SIOMULTI2 >> 1] = node->transferValues[2];
		node->d.p->p->memory.io[REG_SIOMULTI3 >> 1] = node->transferValues[3];
		if (node->d.p->multiplayerControl.irq) {
			GBARaiseIRQ(node->d.p->p, IRQ_SIO);
		}
		node->transferFinished = 0;
		MutexUnlock(&node->lock);
	}
	return INT_MAX;
}

static void _setupTransfer(struct GBASIOMultiMeshNode* node) {
	node->transferActive = (1 << node->connected) - 1;
	node->transferValues[0] = 0xFFFF;
	node->transferValues[1] = 0xFFFF;
	node->transferValues[2] = 0xFFFF;
	node->transferValues[3] = 0xFFFF;
	node->transferValues[node->id] = node->d.p->p->memory.io[REG_SIOMLT_SEND >> 1];
	node->d.p->p->memory.io[REG_SIOMULTI0 >> 1] = 0xFFFF;
	node->d.p->p->memory.io[REG_SIOMULTI1 >> 1] = 0xFFFF;
	node->d.p->p->memory.io[REG_SIOMULTI2 >> 1] = 0xFFFF;
	node->d.p->p->memory.io[REG_SIOMULTI3 >> 1] = 0xFFFF;
}

static void _startTransfer(struct GBASIOMultiMeshNode* node) {
	if (!node || node->id) {
		GBALog(node->d.p->p, GBA_LOG_ERROR, "Slave attempting to commence transfer");
		return;
	}
	int i;
	struct PacketTransferStart transfer = { .type = PACKET_TRANSFER_START };
	struct PacketTransferData data = {
		.type = PACKET_TRANSFER_DATA,
		.id = 0,
		.data = node->d.p->p->memory.io[REG_SIOMLT_SEND >> 1]
	};
	_setupTransfer(node);
	for (i = 1; i < node->connected; ++i) {
		if (node->mesh[i] < 0) {
			continue;
		}
		SocketSend(node->mesh[i], &transfer, sizeof(transfer));
		SocketSend(node->mesh[i], &data, sizeof(data));
	}
	node->transferActive &= ~1;
}

static void _siocntSync(struct GBASIOMultiMeshNode* node) {
	if (node->d.p->activeDriver == &node->d && node->d.p->mode == SIO_MULTI) {
		node->d.p->siocnt &= 0xFF03;
		node->d.p->siocnt |= node->siocnt.packed;
	}
}

static Socket _greet(struct GBASIOMultiMeshNode* node, int port, uint32_t ipAddress) {
	Socket socket = SocketConnectTCP(port, ipAddress);
	if (socket < 0) {
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

static int  _readIPAddress(Socket socket, int ipVersion, void* ipAddress, int ipAddressSize) {
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
	read = SocketRecv(socket, ipAddress, toRead);
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
		if (node->mesh[i] < 0) {
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
		if (node->mesh[i] < 0) {
			continue;
		}
		if (FD_ISSET(node->mesh[i], &set) || FD_ISSET(node->mesh[i], &errorSet)) {
			*id = i;
			return node->mesh[i];
		}
	}
	return -1;
}

static THREAD_ENTRY _networkThread(void* context) {
	struct GBASIOMultiMeshNode* node = context;
	while (node->active) {
		int id;
		Socket socket = _select(node, &id);
		union Packet packet;
		if (socket < 0) {
			break;
		}
		if (id == node->id) {
			// Process incoming connections
			Socket stranger = SocketAccept(socket, 0, 0);
			if (stranger < 0) {
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
				SocketSend(stranger, &hello, sizeof(hello));
				node->siocnt.slave = 0;
				node->siocnt.ready = 1;
				_siocntSync(node);
			}
			++node->connected;
			node->mesh[hello.id] = stranger;
		} else {
			if (SocketRecv(socket, &packet, 1) < 1) {
				SocketClose(node->mesh[id]);
				node->mesh[id] = -1;
				// TODO: Reconfigure mesh
				continue;
			}
			switch(packet.type) {
			case PACKET_JOIN: {
				uint32_t ipAddress;
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
					node->mesh[packet.join.id] = _greet(node, packet.join.port, ipAddress);
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
				break;
			}
			case PACKET_TRANSFER_START: {
				SocketRecv(socket, &packet.data, sizeof(struct PacketTransferStart) - 1);
				if (id) {
					GBALog(node->d.p->p, GBA_LOG_ERROR, "Invalid transfer start");
					continue;
				}
				// FIXME: Don't do this only when a transfer starts
				node->siocnt.slave = 1;
				node->siocnt.ready = 1;
				_siocntSync(node);
				_setupTransfer(node);
				int i;
				struct PacketTransferData data = {
					.type = PACKET_TRANSFER_DATA,
					.id = node->id,
					.data =  node->d.p->p->memory.io[REG_SIOMLT_SEND >> 1]
				};
				for (i = 0; i < node->connected; ++i) {
					if (node->id == i || node->mesh[i] < 0) {
						continue;
					}
					SocketSend(node->mesh[i], &data, sizeof(data));
				}
				node->transferActive &= ~(1 << node->id);
				break;
			}
			case PACKET_TRANSFER_DATA: {
				SocketRecv(socket, &packet.data, sizeof(struct PacketTransferData) - 1);
				if (packet.transferData.id != id) {
					GBALog(node->d.p->p, GBA_LOG_ERROR, "Invalid transfer sender");
					continue;
				}
				node->transferValues[id] = packet.transferData.data;
				node->transferActive &= ~(1 << id);
				// TODO: Increase out-of-order robustness
				if (!node->transferActive) {
					node->siocnt.id = node->id;
					node->siocnt.busy = 0;
					_siocntSync(node);
					_postUpdate(node);
				}
				break;
			}
			default:
				GBALog(node->d.p->p, GBA_LOG_ERROR, "Invalid packet type: %x", packet.type);
				break;
			}
		}
	}
	return 0;
}

void _postUpdate(struct GBASIOMultiMeshNode* node) {
	MutexLock(&node->lock);
	node->transferFinished = 1;
	MutexUnlock(&node->lock);
}
