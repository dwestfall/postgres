/*-------------------------------------------------------------------------
 *
 * backendid.h--
 *    POSTGRES backend id communication definitions
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef	BACKENDID_H
#define BACKENDID_H

/* ----------------
 *	pulled out of sinval.h to temporarily reduce #include nesting.
 *	-cim 8/17/90
 * ----------------
 */
typedef int16	BackendId;	/* unique currently active backend identifier */

#define InvalidBackendId	(-1)

typedef int32	BackendTag;	/* unique backend identifier */

#define InvalidBackendTag	(-1)

extern BackendId	MyBackendId;	/* backend id of this backend */
extern BackendTag	MyBackendTag;	/* backend tag of this backend */

#endif /* BACKENDID_H */
