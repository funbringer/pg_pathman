/* ------------------------------------------------------------------------
 *
 * hooks.h
 *		prototypes of rel_pathlist and join_pathlist hooks
 *
 * Copyright (c) 2016, Postgres Professional
 *
 * ------------------------------------------------------------------------
 */

#ifndef PATHMAN_HOOKS_H
#define PATHMAN_HOOKS_H


#include "postgres.h"
#include "optimizer/planner.h"
#include "optimizer/paths.h"
#include "parser/analyze.h"
#include "storage/ipc.h"
#include "tcop/utility.h"


extern set_join_pathlist_hook_type		pathman_set_join_pathlist_next;
extern set_rel_pathlist_hook_type		pathman_set_rel_pathlist_hook_next;
extern planner_hook_type				pathman_planner_hook_next;
extern post_parse_analyze_hook_type		pathman_post_parse_analyze_hook_next;
extern shmem_startup_hook_type			pathman_shmem_startup_hook_next;
extern ProcessUtility_hook_type			pathman_process_utility_hook_next;


void pathman_join_pathlist_hook(PlannerInfo *root,
								RelOptInfo *joinrel,
								RelOptInfo *outerrel,
								RelOptInfo *innerrel,
								JoinType jointype,
								JoinPathExtraData *extra);

void pathman_rel_pathlist_hook(PlannerInfo *root,
							   RelOptInfo *rel,
							   Index rti,
							   RangeTblEntry *rte);

void pathman_enable_assign_hook(char newval, void *extra);

PlannedStmt * pathman_planner_hook(Query *parse,
								   int cursorOptions,
								   ParamListInfo boundParams);

void pathman_post_parse_analyze_hook(ParseState *pstate,
									  Query *query);

void pathman_shmem_startup_hook(void);

void pathman_relcache_hook(Datum arg, Oid relid);

#if PG_VERSION_NUM >= 100000
void pathman_process_utility_hook(PlannedStmt *pstmt,
								  const char *queryString,
								  ProcessUtilityContext context,
								  ParamListInfo params,
								  QueryEnvironment *queryEnv,
								  DestReceiver *dest,
								  char *completionTag);
#else
void pathman_process_utility_hook(Node *parsetree,
								  const char *queryString,
								  ProcessUtilityContext context,
								  ParamListInfo params,
								  DestReceiver *dest,
								  char *completionTag);
#endif


#endif /* PATHMAN_HOOKS_H */
