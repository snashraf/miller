#include "lib/mlr_globals.h"
#include "lib/mlrutil.h"
#include "containers/lrec.h"
#include "containers/sllv.h"
#include "mapping/mappers.h"
#include "mapping/lrec_evaluators.h"
#include "dsls/mlr_dsl_wrapper.h"
#include "cli/argparse.h"

// begin list of:
// main  list of:
// end   list of:
//
// * node type
// * list of:
//   o output field name
//   o evaluator
//   o is_oosvar

//typedef struct _mlr_dsl_cst_node_item_t {
//	lrec_evaluator_t* pevaluator;
//	char* output_field_name;
//	int is_oosvar;
//} mlr_dsl_cst_node_item_t;

//typedef struct _mlr_dsl_cst_node_t {
//	int ast_node_type;
//	sllv_t* pnode_items;
//} mlr_dsl_cst_node_t;

// xxx also move these outside of mapper_put & into their own CST source file. (lrec_evaluators.c is big already.)

typedef struct _mapper_put_state_t {
	ap_state_t* pargp;
	sllv_t* pasts;

	int at_begin;

	// xxx transpose these from separate arrays to arrays of structs.

	int num_begin_evaluators;
	lrec_evaluator_t** pbegin_evaluators;
	char** begin_output_field_names;
	int*   begin_node_types;
	int*   begin_is_oosvars;

	int num_main_evaluators;
	lrec_evaluator_t** pmain_evaluators;
	char** main_output_field_names;
	int*   main_node_types;
	int*   main_is_oosvars;

	int num_end_evaluators;
	lrec_evaluator_t** pend_evaluators;
	char** end_output_field_names;
	int*   end_node_types;
	int*   end_is_oosvars;

	lrec_t* poosvars;
	lhmsv_t* poosvars_typed_overlay;

} mapper_put_state_t;

static sllv_t*   mapper_put_process(lrec_t* pinrec, context_t* pctx, void* pvstate);
static void      mapper_put_free(mapper_t* pmapper);
static mapper_t* mapper_put_alloc(ap_state_t* pargp, sllv_t* pasts, int type_inferencing);
static void      mapper_put_usage(FILE* o, char* argv0, char* verb);
static mapper_t* mapper_put_parse_cli(int* pargi, int argc, char** argv);

static void foo(
	lrec_t* pinrec,
	lhmsv_t* ptyped_overlay,
	lrec_t* poosvars,
	lhmsv_t* poosvars_typed_overlay,
	string_array_t* pregex_captures,
	context_t* pctx,
	int num_evaluators,
	lrec_evaluator_t** pevaluators,
	char** output_field_names,
	int*   node_types,
	int*   is_oosvars,
	int*   pemit_rec,
	sllv_t* poutrecs
);

// ----------------------------------------------------------------
mapper_setup_t mapper_put_setup = {
	.verb = "put",
	.pusage_func = mapper_put_usage,
	.pparse_func = mapper_put_parse_cli
};

// ----------------------------------------------------------------
static void mapper_put_usage(FILE* o, char* argv0, char* verb) {
	fprintf(o, "Usage: %s %s [options] {expression}\n", argv0, verb);
	fprintf(o, "Adds/updates specified field(s). Expressions are semicolon-separated and must\n");
	fprintf(o, "either be assignments, or evaluate to boolean.  Each expression is evaluated in\n");
	fprintf(o, "turn from left to right. Assignment expressions are applied to the current\n");
	fprintf(o, "record; once a boolean expression evaluates to false, the record is emitted\n");
	fprintf(o, "with all changes up to that point and remaining expressions to the right are\n");
	fprintf(o, "not evaluated.\n");
	fprintf(o, "\n");
	fprintf(o, "Options:\n");
	fprintf(o, "-v: First prints the AST (abstract syntax tree) for the expression, which gives\n");
	fprintf(o, "    full transparency on the precedence and associativity rules of Miller's\n");
	fprintf(o, "    grammar.\n");
	fprintf(o, "-S: Keeps field values, or literals in the expression, as strings with no type \n");
	fprintf(o, "    inference to int or float.\n");
	fprintf(o, "-F: Keeps field values, or literals in the expression, as strings or floats\n");
	fprintf(o, "    with no inference to int.\n");
	fprintf(o, "\n");
	fprintf(o, "Please use a dollar sign for field names and double-quotes for string\n");
	fprintf(o, "literals. If field names have special characters such as \".\" then you might\n");
	fprintf(o, "use braces, e.g. '${field.name}'. Miller built-in variables are\n");
	fprintf(o, "NF NR FNR FILENUM FILENAME PI E.\n");
	fprintf(o, "\n");
	fprintf(o, "Examples:\n");
	fprintf(o, "  Assignment only:\n");
	fprintf(o, "  %s %s '$y = log10($x); $z = sqrt($y)'\n", argv0, verb);
	fprintf(o, "  %s %s '$filename = FILENAME'\n", argv0, verb);
	fprintf(o, "  %s %s '$colored_shape = $color . \"_\" . $shape'\n", argv0, verb);
	fprintf(o, "  %s %s '$y = cos($theta); $z = atan2($y, $x)'\n", argv0, verb);
	fprintf(o, "  %s %s '$name = sub($name, \"http.*com\"i, \"\")'\n", argv0, verb);
	fprintf(o, "  Mixed assignment/boolean:\n");
	fprintf(o, "  %s %s '$x > 0.0; $y = log10($x); $z = sqrt($y)'\n", argv0, verb);
	fprintf(o, "  %s %s '$y = log10($x); 1.1 < $y && $y < 7.0; $z = sqrt($y)'\n", argv0, verb);
	fprintf(o, "\n");
	fprintf(o, "Please see http://johnkerl.org/miller/doc/reference.html for more information\n");
	fprintf(o, "including function list. Or \"%s -f\".\n", argv0);
}

// ----------------------------------------------------------------
static mapper_t* mapper_put_parse_cli(int* pargi, int argc, char** argv) {
	char* verb = argv[(*pargi)++];
	char* mlr_dsl_expression = NULL;
	int   type_inferencing = TYPE_INFER_STRING_FLOAT_INT;
	int   print_asts = FALSE;

	ap_state_t* pstate = ap_alloc();
	ap_define_true_flag(pstate,      "-v", &print_asts);
	ap_define_int_value_flag(pstate, "-S", TYPE_INFER_STRING_ONLY,  &type_inferencing);
	ap_define_int_value_flag(pstate, "-F", TYPE_INFER_STRING_FLOAT, &type_inferencing);

	if (!ap_parse(pstate, verb, pargi, argc, argv)) {
		mapper_put_usage(stderr, argv[0], verb);
		return NULL;
	}

	if ((argc - *pargi) < 1) {
		mapper_put_usage(stderr, argv[0], verb);
		return NULL;
	}
	mlr_dsl_expression = argv[(*pargi)++];

	// Linked list of mlr_dsl_ast_node_t*.
	sllv_t* pasts = mlr_dsl_parse(mlr_dsl_expression);
	if (pasts == NULL) {
		fprintf(stderr, "%s %s: syntax error on DSL parse of '%s'\n",
			argv[0], verb, mlr_dsl_expression);
		return NULL;
	}

	// For just dev-testing the parser, you can do
	//   mlr put -v 'expression goes here' /dev/null
	if (print_asts) {
		for (sllve_t* pe = pasts->phead; pe != NULL; pe = pe->pnext)
			mlr_dsl_ast_node_print(pe->pvvalue);
	}

	return mapper_put_alloc(pstate, pasts, type_inferencing);
}

// ----------------------------------------------------------------
static mapper_t* mapper_put_alloc(ap_state_t* pargp, sllv_t* pasts, int type_inferencing) {
	mapper_put_state_t* pstate = mlr_malloc_or_die(sizeof(mapper_put_state_t));
	pstate->pargp = pargp;
	pstate->pasts = pasts;

	pstate->at_begin = TRUE;

	pstate->num_begin_evaluators = 0;
	pstate->num_main_evaluators  = 0;
	pstate->num_end_evaluators   = 0;
	for (sllve_t* pe = pasts->phead; pe != NULL; pe = pe->pnext) {
		mlr_dsl_ast_node_t* past = pe->pvvalue;
		if (past->type == MLR_DSL_AST_NODE_TYPE_BEGIN) {
			pstate->num_begin_evaluators++;
		} else if (past->type == MLR_DSL_AST_NODE_TYPE_END) {
			pstate->num_end_evaluators++;
		} else {
			pstate->num_main_evaluators++;
		}
	}

	pstate->begin_output_field_names = mlr_malloc_or_die(pstate->num_begin_evaluators * sizeof(char*));
	pstate->begin_node_types  = mlr_malloc_or_die(pstate->num_begin_evaluators * sizeof(int));
	pstate->begin_is_oosvars  = mlr_malloc_or_die(pstate->num_begin_evaluators * sizeof(int));
	pstate->pbegin_evaluators = mlr_malloc_or_die(pstate->num_begin_evaluators * sizeof(lrec_evaluator_t*));

	pstate->main_output_field_names = mlr_malloc_or_die(pstate->num_main_evaluators * sizeof(char*));
	pstate->main_node_types  = mlr_malloc_or_die(pstate->num_main_evaluators * sizeof(int));
	pstate->main_is_oosvars  = mlr_malloc_or_die(pstate->num_main_evaluators * sizeof(int));
	pstate->pmain_evaluators = mlr_malloc_or_die(pstate->num_main_evaluators * sizeof(lrec_evaluator_t*));

	pstate->end_output_field_names = mlr_malloc_or_die(pstate->num_end_evaluators * sizeof(char*));
	pstate->end_node_types  = mlr_malloc_or_die(pstate->num_end_evaluators * sizeof(int));
	pstate->end_is_oosvars  = mlr_malloc_or_die(pstate->num_end_evaluators * sizeof(int));
	pstate->pend_evaluators = mlr_malloc_or_die(pstate->num_end_evaluators * sizeof(lrec_evaluator_t*));

	int bi = 0;
	int mi = 0;
	int ei = 0;
	for (sllve_t* pe = pasts->phead; pe != NULL; pe = pe->pnext) {
		mlr_dsl_ast_node_t* past = pe->pvvalue;

		lrec_evaluator_t** pevaluators = NULL;
		char** output_field_names = NULL;
		int*   node_types = NULL;
		int*   is_oosvars = NULL;
		int    i = 0;

		int orig_type = past->type;

		if (orig_type == MLR_DSL_AST_NODE_TYPE_BEGIN) {
			pevaluators        = pstate->pbegin_evaluators;
			output_field_names = pstate->begin_output_field_names;
			node_types         = pstate->begin_node_types;
			is_oosvars         = pstate->begin_is_oosvars;
			i                  = bi;
			past = past->pchildren->phead->pvvalue;
		} else if (orig_type == MLR_DSL_AST_NODE_TYPE_END) {
			pevaluators        = pstate->pend_evaluators;
			output_field_names = pstate->end_output_field_names;
			node_types         = pstate->end_node_types;
			is_oosvars         = pstate->end_is_oosvars;
			i                  = ei;
			past = past->pchildren->phead->pvvalue;
		} else {
			pevaluators        = pstate->pmain_evaluators;
			output_field_names = pstate->main_output_field_names;
			node_types         = pstate->main_node_types;
			is_oosvars         = pstate->main_is_oosvars;
			i                  = mi;
		}

		node_types[i] = past->type;
		is_oosvars[i] = FALSE;

		if (past->type == MLR_DSL_AST_NODE_TYPE_SREC_ASSIGNMENT) {
			if ((past->pchildren == NULL) || (past->pchildren->length != 2)) {
				fprintf(stderr, "%s: internal coding error detected in file %s at line %d.\n",
					MLR_GLOBALS.argv0, __FILE__, __LINE__);
				exit(1);
			}

			mlr_dsl_ast_node_t* pleft  = past->pchildren->phead->pvvalue;
			mlr_dsl_ast_node_t* pright = past->pchildren->phead->pnext->pvvalue;

			if (pleft->type != MLR_DSL_AST_NODE_TYPE_FIELD_NAME) {
				fprintf(stderr, "%s: internal coding error detected in file %s at line %d.\n",
					MLR_GLOBALS.argv0, __FILE__, __LINE__);
				exit(1);
			} else if (pleft->pchildren != NULL) {
				fprintf(stderr, "%s: coding error detected in file %s at line %d.\n",
					MLR_GLOBALS.argv0, __FILE__, __LINE__);
				exit(1);
			}

			char* output_field_name = pleft->text;
			pevaluators[i] = lrec_evaluator_alloc_from_ast(pright, type_inferencing);
			output_field_names[i] = output_field_name;

		} else if (past->type == MLR_DSL_AST_NODE_TYPE_OOSVAR_ASSIGNMENT) {
			if ((past->pchildren == NULL) || (past->pchildren->length != 2)) {
				fprintf(stderr, "%s: internal coding error detected in file %s at line %d.\n",
					MLR_GLOBALS.argv0, __FILE__, __LINE__);
				exit(1);
			}

			mlr_dsl_ast_node_t* pleft  = past->pchildren->phead->pvvalue;
			mlr_dsl_ast_node_t* pright = past->pchildren->phead->pnext->pvvalue;

			if (pleft->type != MLR_DSL_AST_NODE_TYPE_OOSVAR_NAME) {
				fprintf(stderr, "%s: internal coding error detected in file %s at line %d.\n",
					MLR_GLOBALS.argv0, __FILE__, __LINE__);
				exit(1);
			} else if (pleft->pchildren != NULL) {
				fprintf(stderr, "%s: coding error detected in file %s at line %d.\n",
					MLR_GLOBALS.argv0, __FILE__, __LINE__);
				exit(1);
			}

			char* output_field_name = pleft->text;
			pevaluators[i] = lrec_evaluator_alloc_from_ast(pright, type_inferencing);
			output_field_names[i] = output_field_name;
			is_oosvars[i] = TRUE;

		} else if (past->type == MLR_DSL_AST_NODE_TYPE_FILTER) {
			mlr_dsl_ast_node_t* pnode = past->pchildren->phead->pvvalue;
			pevaluators[i] = lrec_evaluator_alloc_from_ast(pnode, type_inferencing);
			output_field_names[i] = NULL;

		} else if (past->type == MLR_DSL_AST_NODE_TYPE_GATE) {
			mlr_dsl_ast_node_t* pnode = past->pchildren->phead->pvvalue;
			pevaluators[i] = lrec_evaluator_alloc_from_ast(pnode, type_inferencing);
			output_field_names[i] = NULL;

		} else if (past->type == MLR_DSL_AST_NODE_TYPE_EMIT) {
			sllv_t* pchildren = past->pchildren;

			// xxx need to loop over multis in 'emit @a, @b, @c'
			mlr_dsl_ast_node_t* pnode  = pchildren->phead->pvvalue;
			pevaluators[i] = lrec_evaluator_alloc_from_ast(pnode, type_inferencing);
			output_field_names[i] = pnode->text;

		} else {
			// Bare-boolean statement
			pevaluators[i] = lrec_evaluator_alloc_from_ast(past, type_inferencing);
			output_field_names[i] = NULL;
		}

		if (orig_type == MLR_DSL_AST_NODE_TYPE_BEGIN) {
			bi++;
		} else if (orig_type == MLR_DSL_AST_NODE_TYPE_END) {
			ei++;
		} else {
			mi++;
		}
	}

	pstate->poosvars = lrec_unbacked_alloc();
	pstate->poosvars_typed_overlay = lhmsv_alloc();

	mapper_t* pmapper = mlr_malloc_or_die(sizeof(mapper_t));

	pmapper->pvstate       = (void*)pstate;
	pmapper->pprocess_func = mapper_put_process;
	pmapper->pfree_func    = mapper_put_free;

	return pmapper;
}

static void mapper_put_free(mapper_t* pmapper) {
	mapper_put_state_t* pstate = pmapper->pvstate;

	free(pstate->begin_output_field_names);
	free(pstate->begin_node_types);
	for (int i = 0; i < pstate->num_begin_evaluators; i++) {
		lrec_evaluator_t* pevaluator = pstate->pbegin_evaluators[i];
		pevaluator->pfree_func(pevaluator);
	}
	free(pstate->pbegin_evaluators);

	free(pstate->main_output_field_names);
	free(pstate->main_node_types);
	for (int i = 0; i < pstate->num_main_evaluators; i++) {
		lrec_evaluator_t* pevaluator = pstate->pmain_evaluators[i];
		pevaluator->pfree_func(pevaluator);
	}
	free(pstate->pmain_evaluators);

	free(pstate->end_output_field_names);
	free(pstate->end_node_types);
	for (int i = 0; i < pstate->num_end_evaluators; i++) {
		lrec_evaluator_t* pevaluator = pstate->pend_evaluators[i];
		pevaluator->pfree_func(pevaluator);
	}
	free(pstate->pend_evaluators);

	for (sllve_t* pe = pstate->pasts->phead; pe != NULL; pe = pe->pnext) {
		mlr_dsl_ast_node_t* past = pe->pvvalue;
		mlr_dsl_ast_node_free(past);
	}
	sllv_free(pstate->pasts);
	lrec_free(pstate->poosvars);
	for (lhmsve_t* pe = pstate->poosvars_typed_overlay->phead; pe != NULL; pe = pe->pnext)
		mv_free(pe->pvvalue);
	lhmsv_free(pstate->poosvars_typed_overlay);

	ap_free(pstate->pargp);

	free(pstate);
	free(pmapper);
}

// ----------------------------------------------------------------
// The typed-overlay holds intermediate values such as in
//
//   echo x=1 | mlr put '$y = string($x); $z = $y . $y'
//
// because otherwise
// * lrecs store insertion-ordered maps of string to string (since this is ultimately file I/O);
// * types are inferred at entry to put;
// * x=1 would be inferred to int; string($x) would be string; written back to the
//   lrec, y would be "1" which would be re-inferred to int.
//
// So the typed overlay allows us to remember that y is string "1" not integer 1.
//
// But this raises the question: why stop here? Why not have lrecs be insertion-ordered maps from
// string to mlrval? Then we could preserve types for the duration of each lrec, not just for
// the duration of the put operation. Reasons:
// * The compare_lexically operation would suffer a performance regression;
// * Worse, all lhmslv group-by operations (used by many Miller verbs) would likewise suffer a performance regression.
//
// ----------------------------------------------------------------
// The regex-capture string-array holds copies of regex matches, e.g. in
//
//   echo name=abc_def | mlr put '$name =~ "(.*)_(.*)"; $left = "\1"; $right = "\2"'
//
// produces a record with left=abc and right=def.
//
// There is an important trick here with the length of the string-array:
// * It is allocated here with length 0.
// * It is passed by reference to the lrec-evaluator tree. In particular, the matches and does-not-match functions
//   (which implement the =~ and !=~ operators) resize it and populate it.
// * For simplicity, it is a 1-up array: so \1, \2, \3 are at array indices 1, 2, 3.
// * If the matches/does-not-match functions are entered, even with no matches, the regex-captures string-array
//   will be resized to have length at least 1: length 1 for 0 matches, length 2 for 1 match, etc. since
//   the array is indexed 1-up.
// * When the lrec-evaluator's from-literal function is invoked, the interpolate_regex_captures function can quickly
//   check to see if the regex-captures array has length 0 and thereby know that a time-consuming scan for \1, \2, \3,
//   etc. does not need to be done.

static sllv_t* mapper_put_process(lrec_t* pinrec, context_t* pctx, void* pvstate) {
	mapper_put_state_t* pstate = (mapper_put_state_t*)pvstate;

	string_array_t* pregex_captures = string_array_alloc(0);
	sllv_t* poutrecs = sllv_alloc();

	if (pstate->at_begin) {
		int emit_rec = TRUE;

		foo(NULL, NULL, pstate->poosvars, pstate->poosvars_typed_overlay, pregex_captures, pctx,
			pstate->num_begin_evaluators, pstate->pbegin_evaluators, pstate->begin_output_field_names,
			pstate->begin_node_types, pstate->begin_is_oosvars,
			&emit_rec, poutrecs);

		pstate->at_begin = FALSE;
	}

	if (pinrec == NULL) { // End of input stream
		int emit_rec = TRUE;

		foo(NULL, NULL, pstate->poosvars, pstate->poosvars_typed_overlay, pregex_captures, pctx,
			pstate->num_end_evaluators, pstate->pend_evaluators, pstate->end_output_field_names,
			pstate->end_node_types, pstate->end_is_oosvars,
			&emit_rec, poutrecs);

		string_array_free(pregex_captures);
		sllv_add(poutrecs, NULL);
		return poutrecs;
	}

	lhmsv_t* ptyped_overlay = lhmsv_alloc();
	int emit_rec = TRUE;

	foo(pinrec, ptyped_overlay, pstate->poosvars, pstate->poosvars_typed_overlay, pregex_captures, pctx,
		pstate->num_main_evaluators, pstate->pmain_evaluators, pstate->main_output_field_names,
		pstate->main_node_types, pstate->main_is_oosvars,
		&emit_rec, poutrecs);

	if (emit_rec) {
		// Write the output fields from the typed overlay back to the lrec.
		for (lhmsve_t* pe = ptyped_overlay->phead; pe != NULL; pe = pe->pnext) {
			char* output_field_name = pe->key;
			mv_t* pval = pe->pvvalue;

			if (pval->type == MT_STRING) {
				// Ownership transfer from mv_t to lrec.
				lrec_put(pinrec, output_field_name, pval->u.strv, pval->free_flags);
			} else {
				char free_flags = NO_FREE;
				char* string = mv_format_val(pval, &free_flags);
				lrec_put(pinrec, output_field_name, string, free_flags);
			}
			free(pval);

		}
	}
	lhmsv_free(ptyped_overlay);
	string_array_free(pregex_captures);

	if (emit_rec) {
		sllv_add(poutrecs, pinrec);
	} else {
		lrec_free(pinrec);
	}
	return poutrecs;
}

// ----------------------------------------------------------------
static void foo(
	lrec_t* pinrec,
	lhmsv_t* ptyped_overlay,
	lrec_t* poosvars,
	lhmsv_t* poosvars_typed_overlay,
	string_array_t* pregex_captures,
	context_t* pctx,
	int num_evaluators,
	lrec_evaluator_t** pevaluators,
	char** output_field_names,
	int*   node_types,
	int*   is_oosvars,
	int*   pemit_rec,
	sllv_t* poutrecs
) {

	// Do the evaluations, writing typed mlrval output to the typed overlay rather than into the lrec (which holds only
	// string values).
	*pemit_rec = TRUE;
	for (int i = 0; i < num_evaluators; i++) {
		lrec_evaluator_t* pevaluator = pevaluators[i];
		char* output_field_name = output_field_names[i];
		int node_type = node_types[i];

		if (node_type == MLR_DSL_AST_NODE_TYPE_SREC_ASSIGNMENT) {
			mv_t val = pevaluator->pprocess_func(pinrec, ptyped_overlay,
				poosvars, poosvars_typed_overlay,
				pregex_captures, pctx, pevaluator->pvstate);
			mv_t* pval = mlr_malloc_or_die(sizeof(mv_t));
			*pval = val;
			lhmsv_put(ptyped_overlay, output_field_name, pval, NO_FREE);
			// The lrec_evaluator reads the overlay in preference to the lrec. E.g. if the input had
			// "x"=>"abc","y"=>"def" but the previous pass through this loop set "y"=>7.4 and "z"=>"ghi" then an
			// expression right-hand side referring to $y would get the floating-point value 7.4. So we don't need to do
			// lrec_put here, and moreover should not for two reasons: (1) there is a performance hit of doing throwaway
			// number-to-string formatting -- it's better to do it once at the end; (2) having the string values doubly
			// owned by the typed overlay and the lrec would result in double frees, or awkward bookkeeping. However,
			// the NR variable evaluator reads prec->field_count, so we need to put something here. And putting
			// something statically allocated minimizes copying/freeing.
			lrec_put(pinrec, output_field_name, "bug", NO_FREE);

		} else if (node_type == MLR_DSL_AST_NODE_TYPE_OOSVAR_ASSIGNMENT) {
			mv_t val = pevaluator->pprocess_func(pinrec, ptyped_overlay,
				poosvars, poosvars_typed_overlay,
				pregex_captures, pctx, pevaluator->pvstate);
			mv_t* pval = mlr_malloc_or_die(sizeof(mv_t));
			*pval = val;
			lhmsv_put(poosvars_typed_overlay, output_field_name, pval, NO_FREE);
			lrec_put(poosvars, output_field_name, "bug", NO_FREE);

		} else if (node_type == MLR_DSL_AST_NODE_TYPE_FILTER) {
			mv_t val = pevaluator->pprocess_func(pinrec, ptyped_overlay,
				poosvars, poosvars_typed_overlay,
				pregex_captures, pctx, pevaluator->pvstate);
			if (val.type != MT_NULL) {
				mv_set_boolean_strict(&val);
				if (!val.u.boolv) {
					*pemit_rec = FALSE;
					break;
				}
			}

		} else if (node_type == MLR_DSL_AST_NODE_TYPE_GATE) {
			mv_t val = pevaluator->pprocess_func(pinrec, ptyped_overlay,
				poosvars, poosvars_typed_overlay,
				pregex_captures, pctx, pevaluator->pvstate);
			if (val.type == MT_NULL)
				break;
			mv_set_boolean_strict(&val);
			if (!val.u.boolv) {
				break;
			}

		} else if (node_type == MLR_DSL_AST_NODE_TYPE_EMIT) {
			mv_t val = pevaluator->pprocess_func(pinrec, ptyped_overlay,
				poosvars, poosvars_typed_overlay,
				pregex_captures, pctx, pevaluator->pvstate);
			lrec_t* pemit_rec = lrec_unbacked_alloc();

			if (val.type == MT_STRING) {
				// Ownership transfer from mv_t to lrec.
				lrec_put(pemit_rec, output_field_name, val.u.strv, val.free_flags);
			} else {
				char free_flags = NO_FREE;
				char* string = mv_format_val(&val, &free_flags);
				lrec_put(pemit_rec, output_field_name, string, free_flags);
			}

			sllv_add(poutrecs, pemit_rec);

		} else { // Bare-boolean statement
			mv_t val = pevaluator->pprocess_func(pinrec, ptyped_overlay,
				poosvars, poosvars_typed_overlay,
				pregex_captures, pctx, pevaluator->pvstate);
			if (val.type != MT_NULL)
				mv_set_boolean_strict(&val);
		}
	}
}
