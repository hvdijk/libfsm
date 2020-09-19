#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

#include <fsm/fsm.h>
#include <fsm/options.h>
#include <fsm/pred.h>
#include <fsm/walk.h>

#include <re/re.h>

#include "ast.h"
#include "ast_analysis.h"

static int
change_to_alt(struct ast_expr **n)
{
	struct ast_expr *alt;

	assert(n);
	assert(*n);

	if ((*n)->type == AST_EXPR_ALT)
		return 1;

	alt = ast_make_expr_alt(0);
	if (!alt)
		return 0;

	if (!ast_add_expr_alt(alt, *n)) {
		ast_expr_free(alt);
		return 0;
	}

	*n = alt;
	return 1;
}

/*
 * RE state/edge
 *
 * We clone the FSM into a custom structure that allows edges to be
 * annotated by arbitrary AST expressions. The states double as the
 * self-edges. Outgoing edges to another state are stored in an
 * intrusive circular doubly linked list, ordered by state, using the
 * succprev and succnext pointers. Incoming edges from another state
 * are likewise accessible using the predprev and prednext pointers.
 *
 * expr is the expression associated with the transition. It can be null
 * only for self-edges.
 */

struct rese
{
	struct rese *from, *to;
	struct rese *predprev, *prednext;
	struct rese *succprev, *succnext;
	struct ast_expr *expr;
};

static struct rese *
build_restates(unsigned int numstates)
{
	size_t numbytes;
	struct rese *result, *cur, *end;

	if (!numstates)
		return NULL;

	numbytes = numstates * sizeof *result;
	if (numbytes / sizeof *result != numstates)
		return NULL;

	result = malloc(numbytes);
	if (!result)
		return NULL;

	for (cur = result, end = &result[numstates]; cur != end; cur++) {
		cur->from = cur;
		cur->to = cur;
		cur->succprev = cur;
		cur->succnext = cur;
		cur->predprev = cur;
		cur->prednext = cur;
		cur->expr = NULL;
	}

	return result;
}

static struct rese *
build_reedge(struct rese *from, struct rese *to)
{
	struct rese *result, *succ, *pred;

	assert(from);
	assert(to);

	if (from == to)
		return from;

	pred = to->prednext;
	for (;;) {
		assert(pred->to == to);
		if (pred->from == from)
			return pred;
		if (pred->from == to || pred->from > from)
			break;
		pred = pred->prednext;
	}

	succ = from->succnext;
	for (;;) {
		assert(succ->from == from);
		if (succ->to == to) {
			assert(!"should have caught edge in pred loop");
			return succ;
		}
		if (succ->to == from || succ->to > to)
			break;
		succ = succ->succnext;
	}

	result = malloc(sizeof *result);
	if (!result)
		return NULL;

	result->from = from;
	result->to = to;
	result->predprev = pred->predprev;
	result->prednext = pred;
	result->succprev = succ->succprev;
	result->succnext = succ;
	result->expr = NULL;
	result->predprev->prednext = result;
	result->prednext->predprev = result;
	result->succprev->succnext = result;
	result->succnext->succprev = result;
	return result;
}

static int
build_edge_expr(const struct fsm *fsm, fsm_state_t from, fsm_state_t to, struct ast_expr *expr, void *opaque)
{
	struct rese *restates, *reedge;

	assert(fsm);
	assert(expr);
	assert(opaque);

	restates = opaque;

	reedge = build_reedge(&restates[from], &restates[to]);
	if (!reedge)
		return 0;

	if (!reedge->expr) {
		reedge->expr = expr;
		return 1;
	}

	if (!change_to_alt(&reedge->expr))
		return 0;

	if (!ast_add_expr_alt(reedge->expr, expr))
		return 0;

	return 1;
}

static int
build_edge_literal(const struct fsm *fsm, fsm_state_t from, fsm_state_t to, char c, void *opaque)
{
	struct ast_expr *expr;

	assert(fsm);
	assert(opaque);

	expr = ast_make_expr_literal(0, c);
	if (!expr)
		return 0;

	if (!build_edge_expr(fsm, from, to, expr, opaque)) {
		ast_expr_free(expr);
		return 0;
	}

	return 1;
}

static int
build_edge_epsilon(const struct fsm *fsm, fsm_state_t from, fsm_state_t to, void *opaque)
{
	struct ast_expr *expr;

	assert(fsm);
	assert(opaque);

	expr = ast_make_expr_empty(0);
	if (!expr)
		return 0;

	if (!build_edge_expr(fsm, from, to, expr, opaque)) {
		ast_expr_free(expr);
		return 0;
	}

	return 1;
}

static void
free_reedge(struct rese *edge)
{
	assert(edge);
	assert(edge->from != edge && edge->to != edge);

	edge->succprev->succnext = edge->succnext;
	edge->succnext->succprev = edge->succprev;
	edge->predprev->prednext = edge->prednext;
	edge->prednext->predprev = edge->predprev;

	ast_expr_free(edge->expr);
	free(edge);
}

static void
free_restate(struct rese *state)
{
	assert(state);
	assert(state->from == state && state->to == state);

	while (state->succnext != state)
		free_reedge(state->succnext);
	while (state->prednext != state)
		free_reedge(state->prednext);

	ast_expr_free(state->expr);
	state->expr = NULL;
}

static void
free_restates(struct rese *states, unsigned int numstates)
{
	unsigned int i;

	for (i = 0; i < numstates; i++)
		free_restate(&states[i]);

	free(states);
}

static int
remove_state(struct rese *state)
{
	struct rese *p /*pred*/, *ps /*predsucc*/, *s /*succ*/;

	assert(state);
	assert(state->from == state->to);

	/* build new edges for all pred->succ combinations
	 *
	 * if pred->self is A, self->self is B, self->succ is C, we add
	 * AB*C to the pred->succ transition */
	for (p = state->prednext; p != state; p = p->prednext) {
		assert(p->from != state && p->to == state);

		ps = p->from->succnext;

		for (s = state->succnext; s != state; s = s->succnext) {
			struct rese *e;
			struct ast_expr *cat;
			struct ast_expr *tmp;

			assert(s->from == state && s->to != state);

			if (p->from == s->to) {
				e = p->from;
			} else {
				while (ps->from != ps->to && ps->to < s->to)
					ps = ps->succnext;

				if (ps->to != s->to)
					ps = build_reedge(p->from, s->to);

				e = ps;
			}

			assert(p->expr);
			assert(s->expr);

			cat = ast_make_expr_concat(0);
			if (!cat) {
				return 0;
			}

			tmp = p->expr;
			if (!ast_expr_clone(&tmp)) {
				ast_expr_free(cat);
				return 0;
			}

			if (!ast_add_expr_concat(cat, tmp)) {
				ast_expr_free(tmp);
				ast_expr_free(cat);
				return 0;
			}

			if (state->expr) {
				struct ast_expr *rep;
				tmp = state->expr;
				if (!ast_expr_clone(&tmp)) {
					ast_expr_free(cat);
					return 0;
				}
				rep = ast_make_expr_repeat(0, tmp,
					ast_make_count(0, NULL, AST_COUNT_UNBOUNDED, NULL));
				if (!rep) {
					ast_expr_free(tmp);
					ast_expr_free(cat);
					return 0;
				}
				if (!ast_add_expr_concat(cat, rep)) {
					ast_expr_free(rep);
					ast_expr_free(cat);
					return 0;
				}
			}

			tmp = s->expr;
			if (!ast_expr_clone(&tmp)) {
				ast_expr_free(cat);
				return 0;
			}
			if (!ast_add_expr_concat(cat, tmp)) {
				ast_expr_free(tmp);
				ast_expr_free(cat);
				return 0;
			}

			if (!e->expr) {
				e->expr = cat;
			} else {
				if (!change_to_alt(&e->expr)) {
					ast_expr_free(cat);
					return 0;
				}

				if (!ast_add_expr_alt(e->expr, cat)) {
					ast_expr_free(cat);
					return 0;
				}
			}
		}
	}

	free_restate(state);
	return 1;
}

static struct ast_expr *
ast_expr_new_from_fsm(const struct fsm *fsm)
{
	struct ast_expr *expr;

	unsigned int numstates;
	fsm_state_t start;
	struct rese *restates;
	struct rese *restart, *reend;

	unsigned int i;

	assert(fsm);

	numstates = fsm_countstates(fsm);

	if (!fsm_getstart(fsm, &start))
		return ast_expr_tombstone;

	/* Copy FSM states as RE states.
	 * Reserve an additional state to use as an artificial single
	 * end state. */
	restates = build_restates(numstates + 1);
	if (!restates)
		return NULL;

	restart = &restates[start];
	reend = &restates[numstates];

	/* Copy FSM edges as RE edges */
	if (!fsm_walk_edges(fsm, restates, build_edge_literal, build_edge_epsilon)) {
		free_restates(restates, numstates + 1);
		return NULL;
	}

	/* Add epsilon transitions from all FSM end states to our
	 * artificial end state. */
	for (i = 0; i < numstates; i++) {
		if (!fsm_isend(fsm, i))
			continue;

		if (!build_edge_epsilon(fsm, i, numstates, restates)) {
			free_restates(restates, numstates + 1);
			return NULL;
		}
	}

	/* Remove all the states that are not the start or end state */
	for (i = 0; i < numstates; i++) {
		if (i == start)
			continue;

		if (!remove_state(&restates[i])) {
			free_restates(restates, numstates + 1);
			return NULL;
		}
	}

	/* If we now do not have a transition out of the start state,
	 * the FSM is unsatisfiable. */
	if (restart->succnext->to != reend || !restart->succnext->expr) {
		free_restates(restates, numstates + 1);
		return ast_expr_tombstone;
	}

	/* Finally we are left with the start state, end state, a transition
	 * from start to end, and optionally also a transition from start to
	 * start. If the latter is present, handle it here. */
	expr = restart->succnext->expr;

	if (restart->expr) {
		struct ast_expr *cat, *rep;

		cat = ast_make_expr_concat(0);
		if (!cat) {
			free_restates(restates, numstates + 1);
			return NULL;
		}

		rep = ast_make_expr_repeat(
			0, restart->expr, ast_make_count(0, NULL, AST_COUNT_UNBOUNDED, NULL));
		if (!rep) {
			ast_expr_free(cat);
			free_restates(restates, numstates + 1);
			return NULL;
		}

		if (!ast_add_expr_concat(cat, rep)) {
			ast_expr_free(rep);
			ast_expr_free(cat);
			free_restates(restates, numstates + 1);
			return NULL;
		}

		if (!ast_add_expr_concat(cat, expr)) {
			ast_expr_free(cat);
			free_restates(restates, numstates + 1);
			return NULL;
		}

		expr = cat;
	}

	restart->succnext->expr = NULL;
	restart->expr = NULL;

	free_restates(restates, numstates + 1);

	return expr;
}

struct ast *
ast_new_from_fsm(const struct fsm *fsm)
{
	struct ast *ast;
	struct ast_expr *expr;

	assert(fsm);

	expr = ast_expr_new_from_fsm(fsm);
	if (!expr)
		return NULL;

	ast = ast_new();
	if (!ast) {
		ast_expr_free(expr);
		return NULL;
	}

	ast->expr = expr;

	if (!ast_rewrite(ast, 0)) {
		ast_free(ast);
		return NULL;
	}

	return ast;
}
