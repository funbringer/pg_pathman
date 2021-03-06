/* ------------------------------------------------------------------------
 *
 * hooks.c
 *		definitions of rel_pathlist and join_pathlist hooks
 *
 * Copyright (c) 2016, Postgres Professional
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * ------------------------------------------------------------------------
 */

#include "compat/pg_compat.h"
#include "compat/rowmarks_fix.h"

#include "hooks.h"
#include "init.h"
#include "partition_filter.h"
#include "pathman_workers.h"
#include "planner_tree_modification.h"
#include "runtimeappend.h"
#include "runtime_merge_append.h"
#include "utility_stmt_hooking.h"
#include "utils.h"
#include "xact_handling.h"

#include "access/transam.h"
#include "catalog/pg_authid.h"
#include "miscadmin.h"
#include "optimizer/cost.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "rewrite/rewriteManip.h"
#include "utils/typcache.h"
#include "utils/lsyscache.h"


#ifdef USE_ASSERT_CHECKING
#define USE_RELCACHE_LOGGING
#endif


/* Borrowed from joinpath.c */
#define PATH_PARAM_BY_REL(path, rel)  \
	((path)->param_info && bms_overlap(PATH_REQ_OUTER(path), (rel)->relids))


static inline bool
allow_star_schema_join(PlannerInfo *root,
					   Path *outer_path,
					   Path *inner_path)
{
	Relids		innerparams = PATH_REQ_OUTER(inner_path);
	Relids		outerrelids = outer_path->parent->relids;

	/*
	 * It's a star-schema case if the outer rel provides some but not all of
	 * the inner rel's parameterization.
	 */
	return (bms_overlap(innerparams, outerrelids) &&
			bms_nonempty_difference(innerparams, outerrelids));
}


set_join_pathlist_hook_type		pathman_set_join_pathlist_next		= NULL;
set_rel_pathlist_hook_type		pathman_set_rel_pathlist_hook_next	= NULL;
planner_hook_type				pathman_planner_hook_next			= NULL;
post_parse_analyze_hook_type	pathman_post_parse_analyze_hook_next = NULL;
shmem_startup_hook_type			pathman_shmem_startup_hook_next		= NULL;
ProcessUtility_hook_type		pathman_process_utility_hook_next	= NULL;


/* Take care of joins */
void
pathman_join_pathlist_hook(PlannerInfo *root,
						   RelOptInfo *joinrel,
						   RelOptInfo *outerrel,
						   RelOptInfo *innerrel,
						   JoinType jointype,
						   JoinPathExtraData *extra)
{
	JoinCostWorkspace		workspace;
	JoinType				saved_jointype = jointype;
	RangeTblEntry		   *inner_rte = root->simple_rte_array[innerrel->relid];
	const PartRelationInfo *inner_prel;
	List				   *joinclauses,
						   *otherclauses;
	WalkerContext			context;
	double					paramsel;
	Node				   *part_expr;
	ListCell			   *lc;

	/* Call hooks set by other extensions */
	if (pathman_set_join_pathlist_next)
		pathman_set_join_pathlist_next(root, joinrel, outerrel,
									   innerrel, jointype, extra);

	/* Check that both pg_pathman & RuntimeAppend nodes are enabled */
	if (!IsPathmanReady() || !pg_pathman_enable_runtimeappend)
		return;

	/* We should only consider base relations */
	if (innerrel->reloptkind != RELOPT_BASEREL)
		return;

	/* We shouldn't process tables with active children */
	if (inner_rte->inh)
		return;

	/* We can't handle full or right outer joins */
	if (jointype == JOIN_FULL || jointype == JOIN_RIGHT)
		return;

	/* Check that innerrel is a BASEREL with PartRelationInfo */
	if (innerrel->reloptkind != RELOPT_BASEREL ||
		!(inner_prel = get_pathman_relation_info(inner_rte->relid)))
		return;

	/*
	 * Check if query is:
	 *		1) UPDATE part_table SET = .. FROM part_table.
	 *		2) DELETE FROM part_table USING part_table.
	 *
	 * Either outerrel or innerrel may be a result relation.
	 */
	if ((root->parse->resultRelation == outerrel->relid ||
		 root->parse->resultRelation == innerrel->relid) &&
			(root->parse->commandType == CMD_UPDATE ||
			 root->parse->commandType == CMD_DELETE))
	{
		int		rti = -1,
				count = 0;

		/* Inner relation must be partitioned */
		Assert(inner_prel);

		/* Check each base rel of outer relation */
		while ((rti = bms_next_member(outerrel->relids, rti)) >= 0)
		{
			Oid outer_baserel = root->simple_rte_array[rti]->relid;

			/* Is it partitioned? */
			if (get_pathman_relation_info(outer_baserel))
				count++;
		}

		if (count > 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("DELETE and UPDATE queries with a join "
							"of partitioned tables are not supported")));
	}

	/* Skip if inner table is not allowed to act as parent (e.g. FROM ONLY) */
	if (PARENTHOOD_DISALLOWED == get_rel_parenthood_status(inner_rte))
		return;

	/*
	 * These codes are used internally in the planner, but are not supported
	 * by the executor (nor, indeed, by most of the planner).
	 */
	if (jointype == JOIN_UNIQUE_OUTER || jointype == JOIN_UNIQUE_INNER)
		jointype = JOIN_INNER; /* replace with a proper value */

	/* Extract join clauses which will separate partitions */
	if (IS_OUTER_JOIN(extra->sjinfo->jointype))
	{
		extract_actual_join_clauses_compat(extra->restrictlist,
										   joinrel->relids,
										   &joinclauses,
										   &otherclauses);
	}
	else
	{
		/* We can treat all clauses alike for an inner join */
		joinclauses = extract_actual_clauses(extra->restrictlist, false);
		otherclauses = NIL;
	}

	/* Make copy of partitioning expression and fix Var's  varno attributes */
	part_expr = PrelExpressionForRelid(inner_prel, innerrel->relid);

	paramsel = 1.0;
	foreach (lc, joinclauses)
	{
		WrapperNode *wrap;

		InitWalkerContext(&context, part_expr, inner_prel, NULL);
		wrap = walk_expr_tree((Expr *) lfirst(lc), &context);
		paramsel *= wrap->paramsel;
	}

	foreach (lc, innerrel->pathlist)
	{
		AppendPath	   *cur_inner_path = (AppendPath *) lfirst(lc);
		Path		   *outer,
					   *inner;
		NestPath	   *nest_path;		/* NestLoop we're creating */
		ParamPathInfo  *ppi;			/* parameterization info */
		Relids			required_nestloop,
						required_inner;
		List		   *filtered_joinclauses = NIL,
					   *saved_ppi_list,
					   *pathkeys;
		ListCell	   *rinfo_lc;

		if (!IsA(cur_inner_path, AppendPath))
			continue;

		/* Select cheapest path for outerrel */
		outer = outerrel->cheapest_total_path;

		/* We cannot use an outer path that is parameterized by the inner rel */
		if (PATH_PARAM_BY_REL(outer, innerrel))
			continue;

		/* Wrap 'outer' in unique path if needed */
		if (saved_jointype == JOIN_UNIQUE_OUTER)
		{
			outer = (Path *) create_unique_path(root, outerrel,
												outer, extra->sjinfo);
			Assert(outer);
		}

		 /* No way to do this in a parameterized inner path */
		if (saved_jointype == JOIN_UNIQUE_INNER)
			return;


		/* Make inner path depend on outerrel's columns */
		required_inner = bms_union(PATH_REQ_OUTER((Path *) cur_inner_path),
								   outerrel->relids);

		/* Preserve existing ppis built by get_appendrel_parampathinfo() */
		saved_ppi_list = innerrel->ppilist;

		/* Get the ParamPathInfo for a parameterized path */
		innerrel->ppilist = NIL;
		ppi = get_baserel_parampathinfo(root, innerrel, required_inner);
		innerrel->ppilist = saved_ppi_list;

		/* Skip ppi->ppi_clauses don't reference partition attribute */
		if (!(ppi && get_partitioning_clauses(ppi->ppi_clauses,
											  inner_prel,
											  innerrel->relid)))
			continue;

		inner = create_runtimeappend_path(root, cur_inner_path, ppi, paramsel);
		if (!inner)
			return; /* could not build it, retreat! */


		required_nestloop = calc_nestloop_required_outer(outer, inner);

		/*
		 * Check to see if proposed path is still parameterized, and reject if the
		 * parameterization wouldn't be sensible --- unless allow_star_schema_join
		 * says to allow it anyway.  Also, we must reject if have_dangerous_phv
		 * doesn't like the look of it, which could only happen if the nestloop is
		 * still parameterized.
		 */
		if (required_nestloop &&
			((!bms_overlap(required_nestloop, extra->param_source_rels) &&
			  !allow_star_schema_join(root, outer, inner)) ||
			 have_dangerous_phv(root, outer->parent->relids, required_inner)))
			return;

		initial_cost_nestloop_compat(root, &workspace, jointype, outer, inner, extra);

		pathkeys = build_join_pathkeys(root, joinrel, jointype, outer->pathkeys);

		/* Discard all clauses that are to be evaluated by 'inner' */
		foreach (rinfo_lc, extra->restrictlist)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(rinfo_lc);

			Assert(IsA(rinfo, RestrictInfo));
			if (!join_clause_is_movable_to(rinfo, inner->parent))
				filtered_joinclauses = lappend(filtered_joinclauses, rinfo);
		}

		nest_path =
			create_nestloop_path_compat(root, joinrel, jointype,
										&workspace, extra, outer, inner,
										filtered_joinclauses, pathkeys,
										calc_nestloop_required_outer(outer, inner));

		/*
		 * NOTE: Override 'rows' value produced by standard estimator.
		 * Currently we use get_parameterized_joinrel_size() since
		 * it works just fine, but this might change some day.
		 */
		nest_path->path.rows =
				get_parameterized_joinrel_size_compat(root, joinrel,
													  outer, inner,
													  extra->sjinfo,
													  filtered_joinclauses);

		/* Finally we can add the new NestLoop path */
		add_path(joinrel, (Path *) nest_path);
	}
}

/* Cope with simple relations */
void
pathman_rel_pathlist_hook(PlannerInfo *root,
						  RelOptInfo *rel,
						  Index rti,
						  RangeTblEntry *rte)
{
	const PartRelationInfo *prel;
	int						irange_len;

	/* Invoke original hook if needed */
	if (pathman_set_rel_pathlist_hook_next)
		pathman_set_rel_pathlist_hook_next(root, rel, rti, rte);

	/* Make sure that pg_pathman is ready */
	if (!IsPathmanReady())
		return;

	/* We shouldn't process tables with active children */
	if (rte->inh)
		return;

	/*
	 * Skip if it's a result relation (UPDATE | DELETE | INSERT),
	 * or not a (partitioned) physical relation at all.
	 */
	if (rte->rtekind != RTE_RELATION ||
		rte->relkind != RELKIND_RELATION ||
		root->parse->resultRelation == rti)
		return;

#ifdef LEGACY_ROWMARKS_95
	/* It's better to exit, since RowMarks might be broken */
	if (root->parse->commandType != CMD_SELECT &&
		root->parse->commandType != CMD_INSERT)
		return;
#endif

	/* Skip if this table is not allowed to act as parent (e.g. FROM ONLY) */
	if (PARENTHOOD_DISALLOWED == get_rel_parenthood_status(rte))
		return;

	/* Proceed iff relation 'rel' is partitioned */
	if ((prel = get_pathman_relation_info(rte->relid)) != NULL)
	{
		Relation		parent_rel;				/* parent's relation (heap) */
		PlanRowMark	   *parent_rowmark;			/* parent's rowmark */
		Oid			   *children;				/* selected children oids */
		List		   *ranges,					/* a list of IndexRanges */
					   *wrappers;				/* a list of WrapperNodes */
		PathKey		   *pathkeyAsc = NULL,
					   *pathkeyDesc = NULL;
		double			paramsel = 1.0;			/* default part selectivity */
		WalkerContext	context;
		Node		   *part_expr;
		List		   *part_clauses;
		ListCell	   *lc;
		int				i;

		/*
		 * Check that this child is not the parent table itself.
		 * This is exactly how standard inheritance works.
		 *
		 * Helps with queries like this one:
		 *
		 *		UPDATE test.tmp t SET value = 2
		 *		WHERE t.id IN (SELECT id
		 *					   FROM test.tmp2 t2
		 *					   WHERE id = t.id);
		 *
		 * or unions, multilevel partitioning, etc.
		 *
		 * Since we disable optimizations on 9.5, we
		 * have to skip parent table that has already
		 * been expanded by standard inheritance.
		 */
		if (rel->reloptkind == RELOPT_OTHER_MEMBER_REL)
		{
			foreach (lc, root->append_rel_list)
			{
				AppendRelInfo  *appinfo = (AppendRelInfo *) lfirst(lc);

				/*
				 * If there's an 'appinfo' with Oid, it means that somebody
				 * (PG?) has already processed this partitioned table
				 * and added its children to the plan.
				 *
				 * NOTE: there's no Oid iff it's UNION.
				 */
				if (appinfo->child_relid == rti &&
					OidIsValid(appinfo->parent_reloid))
					return;
			}
		}

		/* Make copy of partitioning expression and fix Var's varno attributes */
		part_expr = PrelExpressionForRelid(prel, rti);

		/* Get partitioning-related clauses (do this before append_child_relation()) */
		part_clauses = get_partitioning_clauses(rel->baserestrictinfo, prel, rti);

		if (prel->parttype == PT_RANGE)
		{
			/*
			 * Get pathkeys for ascending and descending sort by partitioned column.
			 */
			List		   *pathkeys;
			TypeCacheEntry *tce;

			/* Determine operator type */
			tce = lookup_type_cache(prel->ev_type, TYPECACHE_LT_OPR | TYPECACHE_GT_OPR);

			/* Make pathkeys */
			pathkeys = build_expression_pathkey(root, (Expr *) part_expr, NULL,
												tce->lt_opr, NULL, false);
			if (pathkeys)
				pathkeyAsc = (PathKey *) linitial(pathkeys);
			pathkeys = build_expression_pathkey(root, (Expr *) part_expr, NULL,
												tce->gt_opr, NULL, false);
			if (pathkeys)
				pathkeyDesc = (PathKey *) linitial(pathkeys);
		}

		children = PrelGetChildrenArray(prel);
		ranges = list_make1_irange_full(prel, IR_COMPLETE);

		/* Make wrappers over restrictions and collect final rangeset */
		InitWalkerContext(&context, part_expr, prel, NULL);
		wrappers = NIL;
		foreach(lc, rel->baserestrictinfo)
		{
			WrapperNode	   *wrap;
			RestrictInfo   *rinfo = (RestrictInfo *) lfirst(lc);

			wrap = walk_expr_tree(rinfo->clause, &context);

			paramsel *= wrap->paramsel;
			wrappers = lappend(wrappers, wrap);
			ranges = irange_list_intersection(ranges, wrap->rangeset);
		}

		/* Get number of selected partitions */
		irange_len = irange_list_length(ranges);
		if (prel->enable_parent)
			irange_len++; /* also add parent */

		/* Expand simple_rte_array and simple_rel_array */
		if (irange_len > 0)
		{
			int current_len	= root->simple_rel_array_size,
				new_len		= current_len + irange_len;

			/* Expand simple_rel_array */
			root->simple_rel_array = (RelOptInfo **)
					repalloc(root->simple_rel_array,
							 new_len * sizeof(RelOptInfo *));

			memset((void *) &root->simple_rel_array[current_len], 0,
				   irange_len * sizeof(RelOptInfo *));

			/* Expand simple_rte_array */
			root->simple_rte_array = (RangeTblEntry **)
					repalloc(root->simple_rte_array,
							 new_len * sizeof(RangeTblEntry *));

			memset((void *) &root->simple_rte_array[current_len], 0,
				   irange_len * sizeof(RangeTblEntry *));

			/* Don't forget to update array size! */
			root->simple_rel_array_size = new_len;
		}

		/* Parent has already been locked by rewriter */
		parent_rel = heap_open(rte->relid, NoLock);

		parent_rowmark = get_plan_rowmark(root->rowMarks, rti);

		/*
		 * WARNING: 'prel' might become invalid after append_child_relation().
		 */

		/* Add parent if asked to */
		if (prel->enable_parent)
			append_child_relation(root, parent_rel, parent_rowmark,
								  rti, 0, rte->relid, NULL);

		/* Iterate all indexes in rangeset and append child relations */
		foreach(lc, ranges)
		{
			IndexRange irange = lfirst_irange(lc);

			for (i = irange_lower(irange); i <= irange_upper(irange); i++)
				append_child_relation(root, parent_rel, parent_rowmark,
									  rti, i, children[i], wrappers);
		}

		/* Now close parent relation */
		heap_close(parent_rel, NoLock);

		/* Clear path list and make it point to NIL */
		list_free_deep(rel->pathlist);
		rel->pathlist = NIL;

#if PG_VERSION_NUM >= 90600
		/* Clear old partial path list */
		list_free(rel->partial_pathlist);
		rel->partial_pathlist = NIL;
#endif

		/* Generate new paths using the rels we've just added */
		set_append_rel_pathlist(root, rel, rti, pathkeyAsc, pathkeyDesc);
		set_append_rel_size_compat(root, rel, rti);

#if PG_VERSION_NUM >= 90600
		/* consider gathering partial paths for the parent appendrel */
		generate_gather_paths(root, rel);
#endif

		/* No need to go further (both nodes are disabled), return */
		if (!(pg_pathman_enable_runtimeappend ||
			  pg_pathman_enable_runtime_merge_append))
			return;

		/* Skip if there's no PARAMs in partitioning-related clauses */
		if (!clause_contains_params((Node *) part_clauses))
			return;

		/* Generate Runtime[Merge]Append paths if needed */
		foreach (lc, rel->pathlist)
		{
			AppendPath	   *cur_path = (AppendPath *) lfirst(lc);
			Relids			inner_required = PATH_REQ_OUTER((Path *) cur_path);
			Path		   *inner_path = NULL;
			ParamPathInfo  *ppi;

			/* Skip if rel contains some join-related stuff or path type mismatched */
			if (!(IsA(cur_path, AppendPath) || IsA(cur_path, MergeAppendPath)) ||
				rel->has_eclass_joins || rel->joininfo)
			{
				continue;
			}

			/* Get existing parameterization */
			ppi = get_appendrel_parampathinfo(rel, inner_required);

			if (IsA(cur_path, AppendPath) && pg_pathman_enable_runtimeappend)
				inner_path = create_runtimeappend_path(root, cur_path,
													   ppi, paramsel);
			else if (IsA(cur_path, MergeAppendPath) &&
					 pg_pathman_enable_runtime_merge_append)
			{
				/* Check struct layout compatibility */
				if (offsetof(AppendPath, subpaths) !=
						offsetof(MergeAppendPath, subpaths))
					elog(FATAL, "Struct layouts of AppendPath and "
								"MergeAppendPath differ");

				inner_path = create_runtimemergeappend_path(root, cur_path,
															ppi, paramsel);
			}

			if (inner_path)
				add_path(rel, inner_path);
		}
	}
}

/*
 * Intercept 'pg_pathman.enable' GUC assignments.
 */
void
pathman_enable_assign_hook(bool newval, void *extra)
{
	elog(DEBUG2, "pg_pathman_enable_assign_hook() [newval = %s] triggered",
		  newval ? "true" : "false");

	/* Return quickly if nothing has changed */
	if (newval == (pathman_init_state.pg_pathman_enable &&
				   pathman_init_state.auto_partition &&
				   pathman_init_state.override_copy &&
				   pg_pathman_enable_runtimeappend &&
				   pg_pathman_enable_runtime_merge_append &&
				   pg_pathman_enable_partition_filter &&
				   pg_pathman_enable_bounds_cache))
		return;

	pathman_init_state.auto_partition		= newval;
	pathman_init_state.override_copy		= newval;
	pg_pathman_enable_runtimeappend			= newval;
	pg_pathman_enable_runtime_merge_append	= newval;
	pg_pathman_enable_partition_filter		= newval;
	pg_pathman_enable_bounds_cache			= newval;

	elog(NOTICE,
		 "RuntimeAppend, RuntimeMergeAppend and PartitionFilter nodes "
		 "and some other options have been %s",
		 newval ? "enabled" : "disabled");
}

/*
 * Planner hook. It disables inheritance for tables that have been partitioned
 * by pathman to prevent standard PostgreSQL partitioning mechanism from
 * handling that tables.
 */
PlannedStmt *
pathman_planner_hook(Query *parse, int cursorOptions, ParamListInfo boundParams)
{
#define ExecuteForPlanTree(planned_stmt, proc) \
	do { \
		ListCell *lc; \
		proc((planned_stmt)->rtable, (planned_stmt)->planTree); \
		foreach (lc, (planned_stmt)->subplans) \
			proc((planned_stmt)->rtable, (Plan *) lfirst(lc)); \
	} while (0)

	PlannedStmt	   *result;
	uint32			query_id = parse->queryId;

	/* Save the result in case it changes */
	bool			pathman_ready = IsPathmanReady();

	PG_TRY();
	{
		if (pathman_ready)
		{
			/* Increase planner() calls count */
			incr_planner_calls_count();

			/* Modify query tree if needed */
			pathman_transform_query(parse, boundParams);
		}

		/* Invoke original hook if needed */
		if (pathman_planner_hook_next)
			result = pathman_planner_hook_next(parse, cursorOptions, boundParams);
		else
			result = standard_planner(parse, cursorOptions, boundParams);

		if (pathman_ready)
		{
			/* Add PartitionFilter node for INSERT queries */
			ExecuteForPlanTree(result, add_partition_filters);

			/* Decrement planner() calls count */
			decr_planner_calls_count();

			/* HACK: restore queryId set by pg_stat_statements */
			result->queryId = query_id;
		}
	}
	/* We must decrease parenthood statuses refcount on ERROR */
	PG_CATCH();
	{
		if (pathman_ready)
		{
			/* Caught an ERROR, decrease count */
			decr_planner_calls_count();
		}

		/* Rethrow ERROR further */
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* Finally return the Plan */
	return result;
}

/*
 * Post parse analysis hook. It makes sure the config is loaded before executing
 * any statement, including utility commands
 */
void
pathman_post_parse_analyze_hook(ParseState *pstate, Query *query)
{
	/* Invoke original hook if needed */
	if (pathman_post_parse_analyze_hook_next)
		pathman_post_parse_analyze_hook_next(pstate, query);

	/* See cook_partitioning_expression() */
	if (!pathman_hooks_enabled)
		return;

	/* We shouldn't proceed on: ... */
	if (query->commandType == CMD_UTILITY)
	{
		/* ... BEGIN */
		if (xact_is_transaction_stmt(query->utilityStmt))
			return;

		/* ... SET pg_pathman.enable */
		if (xact_is_set_stmt(query->utilityStmt, PATHMAN_ENABLE))
		{
			/* Accept all events in case it's "enable = OFF" */
			if (IsPathmanReady())
				finish_delayed_invalidation();

			return;
		}

		/* ... SET [TRANSACTION] */
		if (xact_is_set_stmt(query->utilityStmt, NULL))
			return;

		/* ... ALTER EXTENSION pg_pathman */
		if (xact_is_alter_pathman_stmt(query->utilityStmt))
		{
			/* Leave no delayed events before ALTER EXTENSION */
			if (IsPathmanReady())
				finish_delayed_invalidation();

			/* Disable pg_pathman to perform a painless update */
			(void) set_config_option(PATHMAN_ENABLE, "off",
									 PGC_SUSET, PGC_S_SESSION,
									 GUC_ACTION_SAVE, true, 0, false);

			return;
		}
	}

	/* Finish all delayed invalidation jobs */
	if (IsPathmanReady())
		finish_delayed_invalidation();

	/* Load config if pg_pathman exists & it's still necessary */
	if (IsPathmanEnabled() &&
		!IsPathmanInitialized() &&
		/* Now evaluate the most expensive clause */
		get_pathman_schema() != InvalidOid)
	{
		load_config(); /* perform main cache initialization */
	}

	/* Process inlined SQL functions (we've already entered planning stage) */
	if (IsPathmanReady() && get_planner_calls_count() > 0)
	{
		/* Check that pg_pathman is the last extension loaded */
		if (post_parse_analyze_hook != pathman_post_parse_analyze_hook)
		{
			Oid		save_userid;
			int		save_sec_context;
			bool	need_priv_escalation = !superuser(); /* we might be a SU */
			char   *spl_value; /* value of "shared_preload_libraries" GUC */

			/* Do we have to escalate privileges? */
			if (need_priv_escalation)
			{
				/* Get current user's Oid and security context */
				GetUserIdAndSecContext(&save_userid, &save_sec_context);

				/* Become superuser in order to bypass sequence ACL checks */
				SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID,
									   save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
			}

			/* TODO: add a test for this case (non-privileged user etc) */

			/* Only SU can read this GUC */
#if PG_VERSION_NUM >= 90600
			spl_value = GetConfigOptionByName("shared_preload_libraries", NULL, false);
#else
			spl_value = GetConfigOptionByName("shared_preload_libraries", NULL);
#endif

			/* Restore user's privileges */
			if (need_priv_escalation)
				SetUserIdAndSecContext(save_userid, save_sec_context);

			ereport(ERROR,
					(errmsg("extension conflict has been detected"),
					 errdetail("shared_preload_libraries = \"%s\"", spl_value),
					 errhint("pg_pathman should be the last extension listed in "
							 "\"shared_preload_libraries\" GUC in order to "
							 "prevent possible conflicts with other extensions")));
		}

		/* Modify query tree if needed */
		pathman_transform_query(query, NULL);
	}
}

/*
 * Initialize dsm_config & shmem_config.
 */
void
pathman_shmem_startup_hook(void)
{
	/* Invoke original hook if needed */
	if (pathman_shmem_startup_hook_next)
		pathman_shmem_startup_hook_next();

	/* Allocate shared memory objects */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	init_concurrent_part_task_slots();
	LWLockRelease(AddinShmemInitLock);
}

/*
 * Invalidate PartRelationInfo cache entry if needed.
 */
void
pathman_relcache_hook(Datum arg, Oid relid)
{
	Oid parent_relid;

	/* See cook_partitioning_expression() */
	if (!pathman_hooks_enabled)
		return;

	if (!IsPathmanReady())
		return;

	/* Special case: flush whole relcache */
	if (relid == InvalidOid)
	{
		delay_invalidation_whole_cache();

#ifdef USE_RELCACHE_LOGGING
		elog(DEBUG2, "Invalidation message for all relations [%u]", MyProcPid);
#endif

		return;
	}

	/* We shouldn't even consider special OIDs */
	if (relid < FirstNormalObjectId)
		return;

	/* Invalidation event for PATHMAN_CONFIG table (probably DROP) */
	if (relid == get_pathman_config_relid(false))
		delay_pathman_shutdown();

	/* Invalidate PartBoundInfo cache if needed */
	forget_bounds_of_partition(relid);

	/* Invalidate PartParentInfo cache if needed */
	parent_relid = forget_parent_of_partition(relid, NULL);

	/* It *might have been a partition*, invalidate parent */
	if (OidIsValid(parent_relid))
	{
		delay_invalidation_parent_rel(parent_relid);

#ifdef USE_RELCACHE_LOGGING
		elog(DEBUG2, "Invalidation message for partition %u [%u]",
			 relid, MyProcPid);
#endif
	}
	/* We can't say, perform full invalidation procedure */
	else
	{
		delay_invalidation_vague_rel(relid);

#ifdef USE_RELCACHE_LOGGING
		elog(DEBUG2, "Invalidation message for vague rel %u [%u]",
			 relid, MyProcPid);
#endif
	}
}

/*
 * Utility function invoker hook.
 * NOTE: 'first_arg' is (PlannedStmt *) in PG 10, or (Node *) in PG <= 9.6.
 */
void
#if PG_VERSION_NUM >= 100000
pathman_process_utility_hook(PlannedStmt *first_arg,
							 const char *queryString,
							 ProcessUtilityContext context,
							 ParamListInfo params,
							 QueryEnvironment *queryEnv,
							 DestReceiver *dest, char *completionTag)
{
	Node   *parsetree		= first_arg->utilityStmt;
	int		stmt_location	= first_arg->stmt_location,
			stmt_len		= first_arg->stmt_len;
#else
pathman_process_utility_hook(Node *first_arg,
							 const char *queryString,
							 ProcessUtilityContext context,
							 ParamListInfo params,
							 DestReceiver *dest,
							 char *completionTag)
{
	Node   *parsetree		= first_arg;
	int		stmt_location	= -1,
			stmt_len		= 0;
#endif

	if (IsPathmanReady())
	{
		Oid			relation_oid;
		PartType	part_type;
		AttrNumber	attr_number;
		bool		is_parent;

		/* Override standard COPY statement if needed */
		if (is_pathman_related_copy(parsetree))
		{
			uint64	processed;

			/* Handle our COPY case (and show a special cmd name) */
			PathmanDoCopy((CopyStmt *) parsetree, queryString,
						  stmt_location, stmt_len, &processed);
			if (completionTag)
				snprintf(completionTag, COMPLETION_TAG_BUFSIZE,
						 "COPY " UINT64_FORMAT, processed);

			return; /* don't call standard_ProcessUtility() or hooks */
		}

		/* Override standard RENAME statement if needed */
		else if (is_pathman_related_table_rename(parsetree,
												 &relation_oid,
												 &is_parent))
		{
			const RenameStmt *rename_stmt = (const RenameStmt *) parsetree;

			if (is_parent)
				PathmanRenameSequence(relation_oid, rename_stmt);
			else
				PathmanRenameConstraint(relation_oid, rename_stmt);
		}

		/* Override standard ALTER COLUMN TYPE statement if needed */
		else if (is_pathman_related_alter_column_type(parsetree,
													  &relation_oid,
													  &attr_number,
													  &part_type))
		{
			if (part_type == PT_HASH)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot change type of column \"%s\""
								" of table \"%s\" partitioned by HASH",
								get_attname(relation_oid, attr_number),
								get_rel_name(relation_oid))));

			/* Don't forget to invalidate parsed partitioning expression */
			pathman_config_invalidate_parsed_expression(relation_oid);
		}
	}

	/* Finally call process_utility_hook_next or standard_ProcessUtility */
	call_process_utility_compat((pathman_process_utility_hook_next ?
										pathman_process_utility_hook_next :
										standard_ProcessUtility),
								first_arg, queryString,
								context, params, queryEnv,
								dest, completionTag);
}
