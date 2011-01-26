/*-------------------------------------------------------------------------
 *
 * basebackup.h
 *	  Exports from replication/basebackup.c.
 *
 * Portions Copyright (c) 2010-2010, PostgreSQL Global Development Group
 *
 * src/include/replication/walsender.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _BASEBACKUP_H
#define _BASEBACKUP_H

#include "replication/replnodes.h"

extern void SendBaseBackup(BaseBackupCmd *cmd);

#endif   /* _BASEBACKUP_H */
