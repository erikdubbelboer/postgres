/*-------------------------------------------------------------------------
 *
 * index_build_optimizer.h
 *	  Optimization for CREATE INDEX WHERE statements using existing indexes
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/index_build_optimizer.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef INDEX_BUILD_OPTIMIZER_H
#define INDEX_BUILD_OPTIMIZER_H

#include "access/genam.h"
#include "catalog/index.h"
#include "nodes/execnodes.h"
#include "utils/rel.h"

/* Constants for index build optimization */
#define DEFAULT_HEAP_TUPLES_ESTIMATE		1000	/* Default estimate for
													 * empty relations */
#define OPTIMIZATION_COST_THRESHOLD			0.8 /* Cost threshold for
												 * optimization */
#define DEFAULT_SELECTIVITY_ESTIMATE		0.1 /* Default selectivity for
												 * clauses */
#define MEMORY_RESET_FREQUENCY				1000	/* How often to reset
													 * expression context */

/* Forward declaration */
typedef struct IndexScanOption IndexScanOption;

/* Structure to represent a potential optimization using an existing index */
struct IndexScanOption
{
	Oid			indexOid;		/* OID of the existing index to use */
	List	   *indexQuals;		/* Quals this index can handle */
	List	   *remainingQuals; /* Quals that still need checking */
	Cost		estimated_cost; /* Cost estimate for this approach */
	double		selectivity;	/* Expected selectivity */
	int			nkeys;			/* Number of scan keys */
	ScanKey		scankeys;		/* Prepared scan keys */
};

/* GUC parameters */
extern bool enable_index_build_optimization;
extern bool debug_index_build_optimization;

/* Main analysis function */
extern IndexScanOption * AnalyzeIndexBuildOptimization(Relation heapRel,
													   IndexInfo *indexInfo);

#endif							/* INDEX_BUILD_OPTIMIZER_H */
