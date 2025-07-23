/*-------------------------------------------------------------------------
 *
 * index_build_optimizer.c
 *	  Optimization for CREATE INDEX WHERE statements using existing indexes
 *
 * This module provides functionality to optimize partial index creation
 * by leveraging existing indexes that can efficiently filter tuples
 * matching the WHERE predicate, instead of always doing a full table scan.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/index_build_optimizer.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/nbtree.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_index.h"
#include "commands/defrem.h"
#include "commands/index_build_optimizer.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/plancat.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "catalog/pg_index.h"
#include "catalog/pg_am_d.h"
#include "access/hash.h"

/* GUC parameter to enable/disable optimization */
bool enable_index_build_optimization = true;


/* Function prototypes */
static List *AnalyzeExistingIndexesForPredicate(Relation heapRel, 
											   List *predicate_clauses);
static Cost EstimateIndexScanCost(Relation heapRel, Relation indexRel, 
								 List *indexQuals, double selectivity);
static Cost EstimateSequentialScanCost(Relation heapRel);
static IndexScanOption *ChooseOptimalScanMethod(List *options, Cost seqscan_cost, Relation heapRel);
static bool ExtractIndexQuals(List *predicate_clauses, Relation indexRel,
							 List **indexQuals, List **remainingQuals);
static ScanKey BuildScanKeysFromQuals(List *indexQuals, Relation indexRel, int *nkeys_built);
static double EstimateClauseSelectivity(Node *clause, Relation heapRel);

/*
 * AnalyzeIndexBuildOptimization
 *
 * Main entry point for analyzing optimization opportunities for 
 * CREATE INDEX WHERE statements.
 *
 * Returns an IndexScanOption if optimization is beneficial, NULL otherwise.
 */
IndexScanOption *
AnalyzeIndexBuildOptimization(Relation heapRel, IndexInfo *indexInfo)
{
	List	   *predicate_clauses;
	List	   *index_options;
	Cost		seqscan_cost;
	IndexScanOption *best_option;

	/* Check if optimization is enabled */
	if (!enable_index_build_optimization)
		return NULL;

	/* Must have a WHERE predicate */
	if (indexInfo->ii_Predicate == NIL)
		return NULL;

	/* Convert predicate to list of clauses */
	if (IsA(indexInfo->ii_Predicate, List))
		predicate_clauses = (List *) indexInfo->ii_Predicate;
	else
		predicate_clauses = list_make1(indexInfo->ii_Predicate);

	/* Analyze existing indexes */
	index_options = AnalyzeExistingIndexesForPredicate(heapRel, predicate_clauses);
	
	if (index_options == NIL)
		return NULL;

	/* Estimate cost of sequential scan */
	seqscan_cost = EstimateSequentialScanCost(heapRel);

	/* Choose the best option */
	best_option = ChooseOptimalScanMethod(index_options, seqscan_cost, heapRel);

	return best_option;
}

/*
 * AnalyzeExistingIndexesForPredicate
 *
 * Analyze all existing indexes on the relation to see which ones
 * can help optimize the WHERE predicate evaluation.
 */
static List *
AnalyzeExistingIndexesForPredicate(Relation heapRel, List *predicate_clauses)
{
	List	   *index_options = NIL;
	List	   *indexoidlist;
	ListCell   *lc;

	/* Get list of indexes on this relation */
	indexoidlist = RelationGetIndexList(heapRel);

	foreach(lc, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(lc);
		Relation	indexRel;
		List	   *indexQuals = NIL;
		List	   *remainingQuals = NIL;
		IndexScanOption *option;
		HeapTuple	indexTuple;
		Form_pg_index indexForm;

		/* Check if the index is valid and ready for use */
		indexTuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
		if (!HeapTupleIsValid(indexTuple))
			continue; /* Index doesn't exist anymore */
		
		indexForm = (Form_pg_index) GETSTRUCT(indexTuple);
		
		/* Skip invalid, not ready, or being built indexes */
		if (!indexForm->indisvalid || !indexForm->indisready || !indexForm->indislive)
		{
			ReleaseSysCache(indexTuple);
			continue;
		}

		/* Skip partial indexes (they have their own predicates) */
		if (!heap_attisnull(indexTuple, Anum_pg_index_indpred, NULL))
		{
			ReleaseSysCache(indexTuple);
			continue;
		}

		/* Skip expression indexes for now (more complex to handle) */
		if (!heap_attisnull(indexTuple, Anum_pg_index_indexprs, NULL))
		{
			ReleaseSysCache(indexTuple);
			continue;
		}

		ReleaseSysCache(indexTuple);

		indexRel = index_open(indexoid, AccessShareLock);

		/* Try to match predicate clauses to this index */
		if (ExtractIndexQuals(predicate_clauses, indexRel, 
							 &indexQuals, &remainingQuals))
		{
			/* Create option for this index */
			option = (IndexScanOption *) palloc0(sizeof(IndexScanOption));
			option->indexOid = indexoid;
			option->indexQuals = indexQuals;
			option->remainingQuals = remainingQuals;
			
			/* Estimate selectivity and cost */
			option->selectivity = EstimateClauseSelectivity(
				(Node *) indexQuals, heapRel);
			option->estimated_cost = EstimateIndexScanCost(
				heapRel, indexRel, indexQuals, option->selectivity);

			/* Prepare scan keys */
			option->scankeys = BuildScanKeysFromQuals(indexQuals, indexRel, &option->nkeys);

			index_options = lappend(index_options, option);
		}

		index_close(indexRel, AccessShareLock);
	}

	list_free(indexoidlist);
	return index_options;
}

/*
 * ExtractIndexQuals
 *
 * Determine which predicate clauses can be handled by the given index.
 * This is a simplified version - in a full implementation, we'd use
 * more sophisticated matching logic from the planner.
 */
static bool
ExtractIndexQuals(List *predicate_clauses, Relation indexRel,
				 List **indexQuals, List **remainingQuals)
{
	ListCell   *lc;
	bool		found_useful_qual = false;

	*indexQuals = NIL;
	*remainingQuals = NIL;

	foreach(lc, predicate_clauses)
	{
		Node	   *clause = (Node *) lfirst(lc);
		bool		can_use_index = false;

		/* Check if clause references indexed columns */
		if (IsA(clause, OpExpr))
		{
			OpExpr	   *opexpr = (OpExpr *) clause;
			Var		   *var = NULL;
			Const	   *const_val = NULL;
			int			i;

			/* Look for patterns: var op const, const op var */
			if (list_length(opexpr->args) == 2)
			{
				Node	   *leftarg = linitial(opexpr->args);
				Node	   *rightarg = lsecond(opexpr->args);

				if (IsA(leftarg, Var) && IsA(rightarg, Const))
				{
					var = (Var *) leftarg;
					const_val = (Const *) rightarg;
				}
				else if (IsA(rightarg, Var) && IsA(leftarg, Const))
				{
					var = (Var *) rightarg;
					const_val = (Const *) leftarg;
				}
			}

			if (var != NULL && const_val != NULL)
			{
				/* Check if this variable matches an indexed column */
				for (i = 0; i < indexRel->rd_index->indnatts; i++)
				{
					if (indexRel->rd_index->indkey.values[i] == var->varattno)
					{
						/* Check if the operator is supported by this index */
						Oid opfamily = indexRel->rd_opfamily[i];
						Oid opno = opexpr->opno;
						
						if (get_op_opfamily_strategy(opno, opfamily) > 0)
						{
							can_use_index = true;
							break;
						}
					}
				}
			}
		}
		else if (IsA(clause, ScalarArrayOpExpr))
		{
			/* Handle IN clauses: column IN (const1, const2, ...) */
			ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
			
			if (list_length(saop->args) == 2 && saop->useOr)
			{
				Node *leftarg = linitial(saop->args);
				Node *rightarg = lsecond(saop->args);
				
				if (IsA(leftarg, Var) && IsA(rightarg, Const))
				{
					Var *var = (Var *) leftarg;
					int i;
					
					/* Check if this variable matches an indexed column */
					for (i = 0; i < indexRel->rd_index->indnatts; i++)
					{
						if (indexRel->rd_index->indkey.values[i] == var->varattno)
						{
							/* Check if the array operator is supported */
							Oid opfamily = indexRel->rd_opfamily[i];
							Oid opno = saop->opno;
							
							if (get_op_opfamily_strategy(opno, opfamily) > 0)
							{
								can_use_index = true;
								break;
							}
						}
					}
				}
			}
		}
		else if (IsA(clause, NullTest))
		{
			/* Handle IS NULL / IS NOT NULL */
			NullTest *nulltest = (NullTest *) clause;
			
			if (IsA(nulltest->arg, Var))
			{
				Var *var = (Var *) nulltest->arg;
				int i;
				
				/* Check if this variable matches an indexed column */
				for (i = 0; i < indexRel->rd_index->indnatts; i++)
				{
					if (indexRel->rd_index->indkey.values[i] == var->varattno)
					{
						/* NULL tests can often use indexes */
						can_use_index = true;
						break;
					}
				}
			}
		}

		if (can_use_index)
		{
			*indexQuals = lappend(*indexQuals, clause);
			found_useful_qual = true;
		}
		else
		{
			*remainingQuals = lappend(*remainingQuals, clause);
		}
	}

	return found_useful_qual;
}

/*
 * BuildScanKeysFromQuals
 *
 * Convert index quals to ScanKey array for index scanning.
 * This implements basic operator function lookup for simple equality comparisons.
 */
static ScanKey
BuildScanKeysFromQuals(List *indexQuals, Relation indexRel, int *nkeys_built)
{
	int			nkeys = list_length(indexQuals);
	ScanKey		scankeys;
	ListCell   *lc;
	int			i = 0;

	*nkeys_built = 0;

	if (nkeys == 0)
		return NULL;

	scankeys = (ScanKey) palloc(nkeys * sizeof(ScanKeyData));

	foreach(lc, indexQuals)
	{
		OpExpr	   *opexpr = (OpExpr *) lfirst(lc);
		Var		   *var = NULL;
		Const	   *const_val = NULL;
		int			attno = 0;
		int			keyno = -1;

		/* Extract var and const from simple equality */
		if (IsA(opexpr, OpExpr) && list_length(opexpr->args) == 2)
		{
			Node	   *leftarg = linitial(opexpr->args);
			Node	   *rightarg = lsecond(opexpr->args);

			if (IsA(leftarg, Var) && IsA(rightarg, Const))
			{
				var = (Var *) leftarg;
				const_val = (Const *) rightarg;
			}
			else if (IsA(rightarg, Var) && IsA(leftarg, Const))
			{
				var = (Var *) rightarg;
				const_val = (Const *) leftarg;
			}
		}

		if (var && const_val)
		{
			/* Find the index column number */
			for (int j = 0; j < indexRel->rd_index->indnatts; j++)
			{
				if (indexRel->rd_index->indkey.values[j] == var->varattno)
				{
					attno = j + 1; /* ScanKey uses 1-based attribute numbers */
					keyno = j;     /* 0-based for accessing index metadata */
					break;
				}
			}

			if (attno > 0 && keyno >= 0)
			{
				Oid			opno = opexpr->opno;
				Oid			opfamily = indexRel->rd_opfamily[keyno];
				Oid			opcintype = indexRel->rd_opcintype[keyno];
				int			op_strategy;
				Oid			lefttype;
				Oid			righttype;
				Oid			proc;
				
				/* Look up the operator in the index's operator family */
				op_strategy = get_op_opfamily_strategy(opno, opfamily);
				if (op_strategy > 0)
				{
					get_op_opfamily_properties(opno, opfamily, false,
											   &op_strategy, &lefttype, &righttype);

					/* Get the procedure (function) for this operator based on access method */
					switch (indexRel->rd_rel->relam)
					{
						case BTREE_AM_OID:
							proc = get_opfamily_proc(opfamily, opcintype, opcintype,
													BTORDER_PROC);
							break;
						case HASH_AM_OID:
							/* Hash indexes use hash functions instead of comparison */
							proc = get_opfamily_proc(opfamily, opcintype, opcintype,
													HASHSTANDARD_PROC);
							break;
						case GIST_AM_OID:
						case GIN_AM_OID:
						case SPGIST_AM_OID:
						case BRIN_AM_OID:
							/* These access methods have more complex operator semantics */
							/* For now, skip them to keep the implementation simple */
							elog(DEBUG1, "Access method %u not yet supported for optimization",
								 indexRel->rd_rel->relam);
							proc = InvalidOid;
							break;
						default:
							/* Unknown access method */
							elog(DEBUG1, "Unknown access method %u", indexRel->rd_rel->relam);
							proc = InvalidOid;
							break;
					}
					
					if (!OidIsValid(proc))
					{
						elog(DEBUG1, "Could not find procedure for operator %u in family %u for access method %u",
							 opno, opfamily, indexRel->rd_rel->relam);
						/* Cannot build scan key without valid procedure */
						pfree(scankeys);
						*nkeys_built = 0;
						return NULL;
					}
					
					/* Build the scan key */
					ScanKeyInit(&scankeys[i],
							   attno,
							   op_strategy,
							   proc,
							   const_val->constvalue);
					
					/* Handle NULL values */
					if (const_val->constisnull)
						scankeys[i].sk_flags |= SK_ISNULL;
					
					i++;
				}
				else
				{
					elog(DEBUG1, "Could not find strategy for operator %u in family %u",
						 opno, opfamily);
				}
			}
		}
	}

	/* If we couldn't build any scan keys, return NULL */
	if (i == 0)
	{
		pfree(scankeys);
		*nkeys_built = 0;
		return NULL;
	}

	/* Update the actual number of keys built */
	if (i < nkeys)
	{
		/* Resize if we built fewer keys than expected */
		scankeys = (ScanKey) repalloc(scankeys, i * sizeof(ScanKeyData));
	}

	*nkeys_built = i;
	return scankeys;
}

/*
 * EstimateIndexScanCost
 *
 * Estimate the cost of scanning using the given index with the given quals.
 */
static Cost
EstimateIndexScanCost(Relation heapRel, Relation indexRel, 
					 List *indexQuals, double selectivity)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	double		index_pages;
	double		heap_tuples;
	double		index_tuples;

	/* Get basic relation statistics */
	heap_tuples = heapRel->rd_rel->reltuples;
	if (heap_tuples <= 0)
		heap_tuples = 1000; /* default estimate */

	index_pages = indexRel->rd_rel->relpages;
	if (index_pages <= 0)
		index_pages = 1;

	/* Estimate tuples returned by index scan */
	index_tuples = heap_tuples * selectivity;

	/* Index access cost */
	startup_cost += random_page_cost; /* initial index page */
	run_cost += (index_pages * selectivity) * random_page_cost;

	/* Heap access cost for tuples found */
	run_cost += index_tuples * random_page_cost;

	/* CPU cost for processing tuples */
	run_cost += index_tuples * cpu_tuple_cost;

	return startup_cost + run_cost;
}

/*
 * EstimateSequentialScanCost
 *
 * Estimate the cost of a full sequential scan of the relation.
 */
static Cost
EstimateSequentialScanCost(Relation heapRel)
{
	double		heap_pages;
	double		heap_tuples;
	Cost		run_cost;

	heap_pages = heapRel->rd_rel->relpages;
	heap_tuples = heapRel->rd_rel->reltuples;

	if (heap_pages <= 0)
		heap_pages = 1;
	if (heap_tuples <= 0)
		heap_tuples = 1000;

	/* Sequential scan cost */
	run_cost = heap_pages * seq_page_cost;
	run_cost += heap_tuples * cpu_tuple_cost;

	return run_cost;
}

/*
 * ChooseOptimalScanMethod
 *
 * Choose the best index scan option, or return NULL to use sequential scan.
 */
static IndexScanOption *
ChooseOptimalScanMethod(List *options, Cost seqscan_cost, Relation heapRel)
{
	IndexScanOption *best_option = NULL;
	Cost		best_cost = seqscan_cost;
	ListCell   *lc;

	foreach(lc, options)
	{
		IndexScanOption *option = (IndexScanOption *) lfirst(lc);

		/* Add cost of evaluating remaining predicates */
		Cost		remaining_cost = 0;
		Cost		total_cost;
		
		if (option->remainingQuals != NIL)
		{
			double		heap_tuples = heapRel->rd_rel->reltuples;
			double		filtered_tuples;

			if (heap_tuples <= 0)
				heap_tuples = 1000; /* fallback for empty relations */

			filtered_tuples = option->selectivity * heap_tuples;
			remaining_cost = filtered_tuples * cpu_operator_cost * 
							list_length(option->remainingQuals);
		}

		total_cost = option->estimated_cost + remaining_cost;

		if (total_cost < best_cost)
		{
			best_cost = total_cost;
			best_option = option;
		}
	}

	/* Only use optimization if it's significantly better */
	if (best_option && best_cost < seqscan_cost * 0.8)
		return best_option;

	return NULL;
}

/*
 * EstimateClauseSelectivity
 *
 * Estimate selectivity of a clause or list of clauses.
 * This is a very simplified implementation.
 */
static double
EstimateClauseSelectivity(Node *clause, Relation heapRel)
{
	/* Default selectivity estimate */
	if (IsA(clause, List))
	{
		/* For multiple clauses, multiply selectivities */
		List	   *clauses = (List *) clause;
		double		selectivity = 1.0;
		ListCell   *lc;

		foreach(lc, clauses)
		{
			/* Unused variable lc is needed for foreach macro */
			(void) lc;
			selectivity *= 0.1; /* Very rough estimate */
		}
		return selectivity;
	}
	else
	{
		/* Single clause - default estimate */
		return 0.1;
	}
}
