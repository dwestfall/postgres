/*-------------------------------------------------------------------------
 *
 * execFlatten.h--
 *    prototypes for execFlatten.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECFLATTEN_H
#define EXECFLATTEN_H

extern Datum ExecEvalIter(Iter *iterNode, ExprContext *econtext, bool *resultIsNull, bool *iterIsDone);

extern void ExecEvalFjoin(TargetEntry *tlist, ExprContext *econtext, bool *isNullVect, bool *fj_isDone);

extern bool FjoinBumpOuterNodes(TargetEntry *tlist, ExprContext *econtext, DatumPtr results, char *nulls);


#endif /* EXECFLATTEN_H */



