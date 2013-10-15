#include "hle-bios.h"

const unsigned int hleBiosLength = 196;
const unsigned char hleBios[] = {
  0x06, 0x00, 0x00, 0xea, 0xfe, 0xff, 0xff, 0xea, 0x05, 0x00, 0x00, 0xea,
  0xfe, 0xff, 0xff, 0xea, 0xfe, 0xff, 0xff, 0xea, 0x00, 0x00, 0xa0, 0xe1,
  0x0e, 0x00, 0x00, 0xea, 0xfe, 0xff, 0xff, 0xea, 0x02, 0xf3, 0xa0, 0xe3,
  0x00, 0x00, 0x5d, 0xe3, 0x01, 0xd3, 0xa0, 0x03, 0x20, 0xd0, 0x4d, 0x02,
  0x04, 0x40, 0x2d, 0xe9, 0x02, 0x20, 0x5e, 0xe5, 0x04, 0x00, 0x52, 0xe3,
  0x0b, 0x00, 0x00, 0x0b, 0x05, 0x00, 0x52, 0xe3, 0x01, 0x00, 0xa0, 0x03,
  0x01, 0x10, 0xa0, 0x03, 0x07, 0x00, 0x00, 0x0b, 0x04, 0x40, 0xbd, 0xe8,
  0x0e, 0xf0, 0xb0, 0xe1, 0x0f, 0x50, 0x2d, 0xe9, 0x01, 0x03, 0xa0, 0xe3,
  0x00, 0xe0, 0x8f, 0xe2, 0x04, 0xf0, 0x10, 0xe5, 0x0f, 0x50, 0xbd, 0xe8,
  0x04, 0xf0, 0x5e, 0xe2, 0x10, 0x40, 0x2d, 0xe9, 0x00, 0x30, 0x0f, 0xe1,
  0x80, 0x30, 0xc3, 0xe3, 0x03, 0xf0, 0x29, 0xe1, 0x01, 0x43, 0xa0, 0xe3,
  0x00, 0x00, 0x50, 0xe3, 0x00, 0x00, 0xa0, 0xe3, 0x01, 0x20, 0xa0, 0xe3,
  0x00, 0x00, 0x00, 0x0a, 0x01, 0x03, 0xc4, 0xe5, 0x04, 0x22, 0xc4, 0xe5,
  0xb8, 0x30, 0x54, 0xe1, 0x01, 0x30, 0x13, 0xe0, 0x01, 0x30, 0x23, 0x10,
  0xb8, 0x30, 0x44, 0x11, 0x04, 0x02, 0xc4, 0xe5, 0xf7, 0xff, 0xff, 0x0a,
  0x00, 0x00, 0x0f, 0xe1, 0x80, 0x00, 0x80, 0xe3, 0x00, 0xf0, 0x29, 0xe1,
  0x10, 0x80, 0xbd, 0xe8
};
