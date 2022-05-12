#ifndef LWINDEX_VERSION_H
#define LWINDEX_VERSION_H

#include <stdio.h>

/* lwindex version
 * Major.Minor.Micro.Build */
#define LWINDEX_VERSION ((0 << 24) | (0 << 16) | (3 << 8) | 1)

/* index file version
 * This version is bumped when its structure changed so that the lwindex invokes
 * reindexing opened file immediately. */
#define LWINDEX_INDEX_FILE_VERSION 16

const char *lwindex_version_header();

#endif
