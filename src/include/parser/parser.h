/*-------------------------------------------------------------------------
 *
 * parser.h
 *		Definitions for the "raw" parser (lex and yacc phases only)
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSER_H
#define PARSER_H

extern List *raw_parser(const char *str);

#endif   /* PARSER_H */
