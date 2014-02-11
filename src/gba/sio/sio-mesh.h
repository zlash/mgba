#ifndef SIO_MESH_H
#define SIO_MESH_H

#include "gba-sio.h"

#include "util/socket.h"

#define MAX_GBAS 4

struct GBASIOMultiMeshNode {
	struct GBASIODriver d;

	struct GBAThread* threadContext;

	int id;
	int connected;
	Socket mesh[MAX_GBAS];

	int transferActive;
	uint16_t transferValues[MAX_GBAS];
};

// TODO: IPv6 support
int GBASIOMultiMeshCreateNode(struct GBASIOMultiMeshNode* node, int port, uint32_t bindAddress);
int GBASIOMultiMeshNodeConnect(struct GBASIOMultiMeshNode* node, int port, uint32_t masterAddress);
void GBASIOMultiMeshBindThread(struct GBASIOMultiMeshNode* node, struct GBAThread* threadContext);

#endif
