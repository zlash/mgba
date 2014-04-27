#ifndef SIO_MESH_H
#define SIO_MESH_H

#include "gba-sio.h"

#include "util/socket.h"
#include "util/threading.h"

#define MAX_GBAS 4

enum TransferState {
	// Set in network thread
	TRANSFER_IDLE = 0,
	TRANSFER_FINISHED,

	// Handled in network thread
	TRANSFER_PENDING,
	TRANSFER_ACTIVE
};

struct GBASIOMultiMeshNode {
	struct GBASIODriver d;

	struct GBAThread* threadContext;
	Thread networkThread;
	int active;
	int port;
	uint32_t publicAddress[4];

	int id;
	int connected;
	int32_t nextEvent;
	Socket mesh[MAX_GBAS];

	union {
		struct {
			unsigned baud : 2;
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
	enum TransferState transferState;

	Mutex lock;
	Condition dataSendCond;
	Condition dataRecvCond;
};

// TODO: IPv6 support
int GBASIOMultiMeshCreateNode(struct GBASIOMultiMeshNode* node, int port, uint32_t bindAddress);
int GBASIOMultiMeshNodeConnect(struct GBASIOMultiMeshNode* node, int port, uint32_t masterAddress, uint32_t publicAddress);
void GBASIOMultiMeshBindThread(struct GBASIOMultiMeshNode* node, struct GBAThread* threadContext);

#endif
