/*
 * smatch/smatch_function_ptrs.c
 *
 * Copyright (C) 2013 Oracle.
 *
 * Licensed under the Open Software License version 1.1
 *
 */

/*
 * Track how functions are saved as various struct members or passed as
 * parameters.
 *
 */

#include "smatch.h"
#include "smatch_slist.h"

static int my_id;

static void match_passes_function_pointer(struct expression *expr)
{
	struct expression *arg, *tmp;
	struct symbol *type;
	char *called_name;
	char *fn_name;
	char ptr_name[256];
	int i;


	i = -1;
	FOR_EACH_PTR(expr->args, arg) {
		i++;

		tmp = strip_expr(arg);
		if (tmp->type == EXPR_PREOP && tmp->op == '&')
			tmp = strip_expr(tmp->unop);
		type = get_type(tmp);
		if (!type || type->type != SYM_FN)
			continue;

		called_name = expr_to_var(expr->fn);
		if (!called_name)
			return;
		fn_name = expr_to_var(tmp);
		if (!fn_name)
			goto free;

		snprintf(ptr_name, sizeof(ptr_name), "%s param %d", called_name, i);
		sql_insert_function_ptr(fn_name, ptr_name);
free:
		free_string(fn_name);
		free_string(called_name);
	} END_FOR_EACH_PTR(arg);

}

static void match_function_assign(struct expression *expr)
{
	struct expression *right = expr->right;
	struct symbol *sym;
	char *fn_name;
	char *ptr_name;

	if (right->type == EXPR_PREOP && right->op == '&')
		right = strip_expr(right->unop);
	if (right->type != EXPR_SYMBOL)
		return;
	sym = get_type(right);
	if (!sym || sym->type != SYM_FN)
		return;

	fn_name = expr_to_var(right);
	ptr_name = get_fnptr_name(expr->left);
	if (!fn_name || !ptr_name)
		goto free;

	sql_insert_function_ptr(fn_name, ptr_name);

free:
	free_string(fn_name);
	free_string(ptr_name);
}

void register_function_ptrs(int id)
{
	my_id = id;

	if (!option_info)
		return;

	add_hook(&match_passes_function_pointer, FUNCTION_CALL_HOOK);
	add_hook(&match_function_assign, ASSIGNMENT_HOOK);
}