/*
 * Copyright (C) 2010 Dan Carpenter.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see http://www.gnu.org/copyleft/gpl.txt
 */

#include <string.h>
#include <errno.h>
#include <sqlite3.h>
#include <unistd.h>
#include <ctype.h>
#include "smatch.h"
#include "smatch_slist.h"
#include "smatch_extra.h"

static sqlite3 *db;
static sqlite3 *mem_db;

static int return_id;

#define sql_insert_helper(table, ignore, values...)				\
do {										\
	if (__inline_fn) {							\
		char buf[1024];							\
		char *err, *p = buf;						\
		int rc;								\
										\
		if (!mem_db)							\
			break;							\
										\
		p += snprintf(p, buf + sizeof(buf) - p,				\
			      "insert %sinto %s values (",			\
			      ignore ? "or ignore " : "", #table);		\
		p += snprintf(p, buf + sizeof(buf) - p, values);		\
		p += snprintf(p, buf + sizeof(buf) - p, ");");			\
		sm_debug("in-mem: %s\n", buf);					\
		rc = sqlite3_exec(mem_db, buf, NULL, NULL, &err);		\
		if (rc != SQLITE_OK) {						\
			fprintf(stderr, "SQL error #2: %s\n", err);		\
			fprintf(stderr, "SQL: '%s'\n", buf);			\
			parse_error = 1;					\
		}								\
		break;								\
	}									\
	if (option_info) {							\
		FILE *tmp_fd = sm_outfd;					\
		sm_outfd = sql_outfd;						\
		sm_prefix();							\
	        sm_printf("SQL: insert %sinto " #table " values(",		\
			  ignore ? "or ignore " : "");				\
	        sm_printf(values);						\
	        sm_printf(");\n");						\
		sm_outfd = tmp_fd;						\
	}									\
} while (0)

#define sql_insert(table, values...) sql_insert_helper(table, 0, values);
#define sql_insert_or_ignore(table, values...) sql_insert_helper(table, 1, values);

struct def_callback {
	int hook_type;
	void (*callback)(const char *name, struct symbol *sym, char *key, char *value);
};
ALLOCATOR(def_callback, "definition db hook callbacks");
DECLARE_PTR_LIST(callback_list, struct def_callback);
static struct callback_list *select_caller_info_callbacks;

struct member_info_callback {
	int owner;
	void (*callback)(struct expression *call, int param, char *printed_name, struct sm_state *sm);
};
ALLOCATOR(member_info_callback, "caller_info callbacks");
DECLARE_PTR_LIST(member_info_cb_list, struct member_info_callback);
static struct member_info_cb_list *member_callbacks;

struct returned_state_callback {
	void (*callback)(int return_id, char *return_ranges, struct expression *return_expr);
};
ALLOCATOR(returned_state_callback, "returned state callbacks");
DECLARE_PTR_LIST(returned_state_cb_list, struct returned_state_callback);
static struct returned_state_cb_list *returned_state_callbacks;

struct returned_member_callback {
	int owner;
	void (*callback)(int return_id, char *return_ranges, struct expression *expr, char *printed_name, struct smatch_state *state);
};
ALLOCATOR(returned_member_callback, "returned member callbacks");
DECLARE_PTR_LIST(returned_member_cb_list, struct returned_member_callback);
static struct returned_member_cb_list *returned_member_callbacks;

struct call_implies_callback {
	int type;
	void (*callback)(struct expression *call, struct expression *arg, char *key, char *value);
};
ALLOCATOR(call_implies_callback, "call_implies callbacks");
DECLARE_PTR_LIST(call_implies_cb_list, struct call_implies_callback);
static struct call_implies_cb_list *call_implies_cb_list;

static int print_sql_output(void *unused, int argc, char **argv, char **azColName)
{
	int i;

	for (i = 0; i < argc; i++) {
		if (i != 0)
			printf(", ");
		sm_printf("%s", argv[i]);
	}
	sm_printf("\n");
	return 0;
}

void debug_sql(const char *sql)
{
	if (!option_debug)
		return;
	sm_msg("%s", sql);
	sql_exec(print_sql_output, NULL, sql);
}

void debug_mem_sql(const char *sql)
{
	if (!option_debug)
		return;
	sm_msg("%s", sql);
	sql_mem_exec(print_sql_output, NULL, sql);
}

void sql_exec(int (*callback)(void*, int, char**, char**), void *data, const char *sql)
{
	char *err = NULL;
	int rc;

	if (option_no_db || !db)
		return;

	rc = sqlite3_exec(db, sql, callback, data, &err);
	if (rc != SQLITE_OK && !parse_error) {
		fprintf(stderr, "SQL error #2: %s\n", err);
		fprintf(stderr, "SQL: '%s'\n", sql);
		parse_error = 1;
	}
}

void sql_mem_exec(int (*callback)(void*, int, char**, char**), void *data, const char *sql)
{
	char *err = NULL;
	int rc;

	if (!mem_db)
		return;

	rc = sqlite3_exec(mem_db, sql, callback, data, &err);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "SQL error #2: %s\n", err);
		fprintf(stderr, "SQL: '%s'\n", sql);
		parse_error = 1;
	}
}

static int replace_count;
static char **replace_table;
static const char *replace_return_ranges(const char *return_ranges)
{
	int i;

	if (!get_function()) {
		/* I have no idea why EXPORT_SYMBOL() is here */
		return return_ranges;
	}
	for (i = 0; i < replace_count; i += 3) {
		if (strcmp(replace_table[i + 0], get_function()) == 0) {
			if (strcmp(replace_table[i + 1], return_ranges) == 0)
				return replace_table[i + 2];
		}
	}
	return return_ranges;
}

void sql_insert_return_states(int return_id, const char *return_ranges,
		int type, int param, const char *key, const char *value)
{
	if (key && strlen(key) >= 80)
		return;
	return_ranges = replace_return_ranges(return_ranges);
	sql_insert(return_states, "'%s', '%s', %lu, %d, '%s', %d, %d, %d, '%s', '%s'",
		   get_base_file(), get_function(), (unsigned long)__inline_fn,
		   return_id, return_ranges, fn_static(), type, param, key, value);
}

static struct string_list *common_funcs;
static int is_common_function(const char *fn)
{
	char *tmp;

	if (!fn)
		return 0;

	if (strncmp(fn, "__builtin_", 10) == 0)
		return 1;

	FOR_EACH_PTR(common_funcs, tmp) {
		if (strcmp(tmp, fn) == 0)
			return 1;
	} END_FOR_EACH_PTR(tmp);

	return 0;
}

static char *function_signature(void)
{
	return type_to_str(get_real_base_type(cur_func_sym));
}

void sql_insert_caller_info(struct expression *call, int type,
		int param, const char *key, const char *value)
{
	FILE *tmp_fd = sm_outfd;
	char *fn;

	if (!option_info && !__inline_call)
		return;

	if (key && strlen(key) >= 80)
		return;

	fn = get_fnptr_name(call->fn);
	if (!fn)
		return;

	if (__inline_call) {
		mem_sql(NULL, NULL,
			"insert into caller_info values ('%s', '%s', '%s', %lu, %d, %d, %d, '%s', '%s');",
			get_base_file(), get_function(), fn, (unsigned long)call,
			is_static(call->fn), type, param, key, value);
	}

	if (!option_info)
		return;

	if (strncmp(fn, "__builtin_", 10) == 0)
		return;
	if (type != INTERNAL && is_common_function(fn))
		return;

	sm_outfd = caller_info_fd;
	sm_msg("SQL_caller_info: insert into caller_info values ("
	       "'%s', '%s', '%s', %%CALL_ID%%, %d, %d, %d, '%s', '%s');",
	       get_base_file(), get_function(), fn, is_static(call->fn),
	       type, param, key, value);
	sm_outfd = tmp_fd;

	free_string(fn);
}

void sql_insert_function_ptr(const char *fn, const char *struct_name)
{
	sql_insert(function_ptr, "'%s', '%s', '%s', 0", get_base_file(), fn,
		   struct_name);
}

void sql_insert_call_implies(int type, int param, const char *key, const char *value)
{
	sql_insert(call_implies, "'%s', '%s', %lu, %d, %d, %d, '%s', '%s'", get_base_file(),
	           get_function(), (unsigned long)__inline_fn, fn_static(),
		   type, param, key, value);
}

void sql_insert_function_type_size(const char *member, const char *ranges)
{
	sql_insert(function_type_size, "'%s', '%s', '%s', '%s'", get_base_file(), get_function(), member, ranges);
}

void sql_insert_local_values(const char *name, const char *value)
{
	sql_insert(local_values, "'%s', '%s', '%s'", get_base_file(), name, value);
}

void sql_insert_function_type_value(const char *type, const char *value)
{
	sql_insert(function_type_value, "'%s', '%s', '%s', '%s'", get_base_file(), get_function(), type, value);
}

void sql_insert_function_type(int param, const char *value)
{
	sql_insert(function_type, "'%s', '%s', %d, %d, '%s'",
		   get_base_file(), get_function(), fn_static(), param, value);
}

void sql_insert_parameter_name(int param, const char *value)
{
	sql_insert(parameter_name, "'%s', '%s', %d, %d, '%s'",
		   get_base_file(), get_function(), fn_static(), param, value);
}

void sql_insert_data_info(struct expression *data, int type, const char *value)
{
	char *data_name;

	data_name = get_data_info_name(data);
	if (!data_name)
		return;
	sql_insert(data_info, "'%s', '%s', %d, '%s'",
		   is_static(data) ? get_base_file() : "extern",
		   data_name, type, value);
}

void sql_insert_data_info_var_sym(const char *var, struct symbol *sym, int type, const char *value)
{
	sql_insert(data_info, "'%s', '%s', %d, '%s'",
		   (sym->ctype.modifiers & MOD_STATIC) ? get_base_file() : "extern",
		   var, type, value);
}

void sql_save_constraint(const char *con)
{
	if (!option_info)
		return;

        sm_msg("SQL: insert or ignore into constraints (str) values('%s');", con);
}

void sql_save_constraint_required(const char *data, int op, const char *limit)
{
	sql_insert_or_ignore(constraints_required, "'%s', '%s', '%s'", data, show_special(op), limit);
}

void sql_insert_fn_ptr_data_link(const char *ptr, const char *data)
{
	sql_insert(fn_ptr_data_link, "'%s', '%s'", ptr, data);
}

void sql_insert_fn_data_link(struct expression *fn, int type, int param, const char *key, const char *value)
{
	if (fn->type != EXPR_SYMBOL || !fn->symbol->ident)
		return;

	sql_insert(fn_data_link, "'%s', '%s', %d, %d, %d, '%s', '%s'",
		   (fn->symbol->ctype.modifiers & MOD_STATIC) ? get_base_file() : "extern",
		   fn->symbol->ident->name,
		   !!(fn->symbol->ctype.modifiers & MOD_STATIC),
		   type, param, key, value);
}

char *get_static_filter(struct symbol *sym)
{
	static char sql_filter[1024];

	/* This can only happen on buggy code.  Return invalid SQL. */
	if (!sym) {
		sql_filter[0] = '\0';
		return sql_filter;
	}

	if (sym->ctype.modifiers & MOD_STATIC) {
		snprintf(sql_filter, sizeof(sql_filter),
			 "file = '%s' and function = '%s' and static = '1'",
			 get_base_file(), sym->ident->name);
	} else {
		snprintf(sql_filter, sizeof(sql_filter),
			 "function = '%s' and static = '0'", sym->ident->name);
	}

	return sql_filter;
}

static int get_row_count(void *_row_count, int argc, char **argv, char **azColName)
{
	int *row_count = _row_count;

	*row_count = 0;
	if (argc != 1)
		return 0;
	*row_count = atoi(argv[0]);
	return 0;
}

static void mark_params_untracked(struct expression *call)
{
	struct expression *arg;
	int i = 0;

	FOR_EACH_PTR(call->args, arg) {
		mark_untracked(call, i++, "$", NULL);
	} END_FOR_EACH_PTR(arg);
}

static void sql_select_return_states_pointer(const char *cols,
	struct expression *call, int (*callback)(void*, int, char**, char**), void *info)
{
	char *ptr;
	int return_count = 0;

	ptr = get_fnptr_name(call->fn);
	if (!ptr)
		return;

	run_sql(get_row_count, &return_count,
		"select count(*) from return_states join function_ptr "
		"where return_states.function == function_ptr.function and "
		"ptr = '%s' and searchable = 1 and type = %d;", ptr, INTERNAL);
	/* The magic number 100 is just from testing on the kernel. */
	if (return_count > 100) {
		mark_params_untracked(call);
		return;
	}

	run_sql(callback, info,
		"select %s from return_states join function_ptr where "
		"return_states.function == function_ptr.function and ptr = '%s' "
		"and searchable = 1 "
		"order by function_ptr.file, return_states.file, return_id, type;",
		cols, ptr);
}

static int is_local_symbol(struct expression *expr)
{
	if (expr->type != EXPR_SYMBOL)
		return 0;
	if (expr->symbol->ctype.modifiers & (MOD_NONLOCAL | MOD_STATIC | MOD_ADDRESSABLE))
		return 0;
	return 1;
}

void sql_select_return_states(const char *cols, struct expression *call,
	int (*callback)(void*, int, char**, char**), void *info)
{
	int row_count = 0;

	if (is_fake_call(call))
		return;

	if (call->fn->type != EXPR_SYMBOL || !call->fn->symbol || is_local_symbol(call->fn)) {
		sql_select_return_states_pointer(cols, call, callback, info);
		return;
	}

	if (inlinable(call->fn)) {
		mem_sql(callback, info,
			"select %s from return_states where call_id = '%lu' order by return_id, type;",
			cols, (unsigned long)call);
		return;
	}

	run_sql(get_row_count, &row_count, "select count(*) from return_states where %s;",
		get_static_filter(call->fn->symbol));
	if (row_count > 3000)
		return;

	run_sql(callback, info, "select %s from return_states where %s order by file, return_id, type;",
		cols, get_static_filter(call->fn->symbol));
}

void sql_select_call_implies(const char *cols, struct expression *call,
	int (*callback)(void*, int, char**, char**))
{
	if (call->fn->type != EXPR_SYMBOL || !call->fn->symbol)
		return;

	if (inlinable(call->fn)) {
		mem_sql(callback, call,
			"select %s from call_implies where call_id = '%lu';",
			cols, (unsigned long)call);
		return;
	}

	run_sql(callback, call, "select %s from call_implies where %s;",
		cols, get_static_filter(call->fn->symbol));
}

struct select_caller_info_data {
	struct stree *final_states;
	int prev_func_id;
	int ignore;
	int results;
};

static void sql_select_caller_info(struct select_caller_info_data *data,
	const char *cols, struct symbol *sym,
	int (*callback)(void*, int, char**, char**))
{
	if (__inline_fn) {
		mem_sql(callback, data,
			"select %s from caller_info where call_id = %lu;",
			cols, (unsigned long)__inline_fn);
		return;
	}

	if (sym->ident->name && is_common_function(sym->ident->name))
		return;
	run_sql(callback, data,
		"select %s from common_caller_info where %s order by call_id;",
		cols, get_static_filter(sym));
	if (data->results)
		return;

	run_sql(callback, data,
		"select %s from caller_info where %s order by call_id;",
		cols, get_static_filter(sym));
}

void select_caller_info_hook(void (*callback)(const char *name, struct symbol *sym, char *key, char *value), int type)
{
	struct def_callback *def_callback = __alloc_def_callback(0);

	def_callback->hook_type = type;
	def_callback->callback = callback;
	add_ptr_list(&select_caller_info_callbacks, def_callback);
}

/*
 * These call backs are used when the --info option is turned on to print struct
 * member information.  For example foo->bar could have a state in
 * smatch_extra.c and also check_user.c.
 */
void add_member_info_callback(int owner, void (*callback)(struct expression *call, int param, char *printed_name, struct sm_state *sm))
{
	struct member_info_callback *member_callback = __alloc_member_info_callback(0);

	member_callback->owner = owner;
	member_callback->callback = callback;
	add_ptr_list(&member_callbacks, member_callback);
}

void add_split_return_callback(void (*fn)(int return_id, char *return_ranges, struct expression *returned_expr))
{
	struct returned_state_callback *callback = __alloc_returned_state_callback(0);

	callback->callback = fn;
	add_ptr_list(&returned_state_callbacks, callback);
}

void add_returned_member_callback(int owner, void (*callback)(int return_id, char *return_ranges, struct expression *expr, char *printed_name, struct smatch_state *state))
{
	struct returned_member_callback *member_callback = __alloc_returned_member_callback(0);

	member_callback->owner = owner;
	member_callback->callback = callback;
	add_ptr_list(&returned_member_callbacks, member_callback);
}

void select_call_implies_hook(int type, void (*callback)(struct expression *call, struct expression *arg, char *key, char *value))
{
	struct call_implies_callback *cb = __alloc_call_implies_callback(0);

	cb->type = type;
	cb->callback = callback;
	add_ptr_list(&call_implies_cb_list, cb);
}

struct return_info {
	struct expression *static_returns_call;
	struct symbol *return_type;
	struct range_list *return_range_list;
};

static int db_return_callback(void *_ret_info, int argc, char **argv, char **azColName)
{
	struct return_info *ret_info = _ret_info;
	struct range_list *rl;
	struct expression *call_expr = ret_info->static_returns_call;

	if (argc != 1)
		return 0;
	call_results_to_rl(call_expr, ret_info->return_type, argv[0], &rl);
	ret_info->return_range_list = rl_union(ret_info->return_range_list, rl);
	return 0;
}

struct range_list *db_return_vals(struct expression *expr)
{
	struct return_info ret_info = {};
	char buf[64];
	struct sm_state *sm;

	if (is_fake_call(expr))
		return NULL;

	snprintf(buf, sizeof(buf), "return %p", expr);
	sm = get_sm_state(SMATCH_EXTRA, buf, NULL);
	if (sm)
		return clone_rl(estate_rl(sm->state));
	ret_info.static_returns_call = expr;
	ret_info.return_type = get_type(expr);
	if (!ret_info.return_type)
		return NULL;

	if (expr->fn->type != EXPR_SYMBOL || !expr->fn->symbol)
		return NULL;

	ret_info.return_range_list = NULL;
	if (inlinable(expr->fn)) {
		mem_sql(db_return_callback, &ret_info,
			"select distinct return from return_states where call_id = '%lu';",
			(unsigned long)expr);
	} else {
		run_sql(db_return_callback, &ret_info,
			"select distinct return from return_states where %s;",
			get_static_filter(expr->fn->symbol));
	}
	return ret_info.return_range_list;
}

struct range_list *db_return_vals_from_str(const char *fn_name)
{
	struct return_info ret_info;

	ret_info.static_returns_call = NULL;
	ret_info.return_type = &llong_ctype;
	ret_info.return_range_list = NULL;

	run_sql(db_return_callback, &ret_info,
		"select distinct return from return_states where function = '%s';",
		fn_name);
	return ret_info.return_range_list;
}

static void match_call_marker(struct expression *expr)
{
	struct symbol *type;

	type = get_type(expr->fn);
	if (type && type->type == SYM_PTR)
		type = get_real_base_type(type);

	/*
	 * we just want to record something in the database so that if we have
	 * two calls like:  frob(4); frob(some_unkown); then on the receiving
	 * side we know that sometimes frob is called with unknown parameters.
	 */

	sql_insert_caller_info(expr, INTERNAL, -1, "%call_marker%", type_to_str(type));
}

static char *show_offset(int offset)
{
	static char buf[64];

	buf[0] = '\0';
	if (offset != -1)
		snprintf(buf, sizeof(buf), "(-%d)", offset);
	return buf;
}

static void print_struct_members(struct expression *call, struct expression *expr, int param, int offset, struct stree *stree,
	void (*callback)(struct expression *call, int param, char *printed_name, struct sm_state *sm))
{
	struct sm_state *sm;
	char *name;
	struct symbol *sym;
	int len;
	char printed_name[256];
	int is_address = 0;

	expr = strip_expr(expr);
	if (expr->type == EXPR_PREOP && expr->op == '&') {
		expr = strip_expr(expr->unop);
		is_address = 1;
	}

	name = expr_to_var_sym(expr, &sym);
	if (!name || !sym)
		goto free;

	len = strlen(name);
	FOR_EACH_SM(stree, sm) {
		if (sm->sym != sym)
			continue;
		if (strcmp(name, sm->name) == 0) {
			if (is_address)
				snprintf(printed_name, sizeof(printed_name), "*$%s", show_offset(offset));
			else /* these are already handled. fixme: handle them here */
				continue;
		} else if (sm->name[0] == '*' && strcmp(name, sm->name + 1) == 0) {
			snprintf(printed_name, sizeof(printed_name), "*$%s", show_offset(offset));
		} else if (strncmp(name, sm->name, len) == 0) {
			if (isalnum(sm->name[len]))
				continue;
			if (is_address)
				snprintf(printed_name, sizeof(printed_name), "$%s->%s", show_offset(offset), sm->name + len + 1);
			else
				snprintf(printed_name, sizeof(printed_name), "$%s%s", show_offset(offset), sm->name + len);
		} else {
			continue;
		}
		callback(call, param, printed_name, sm);
	} END_FOR_EACH_SM(sm);
free:
	free_string(name);
}

static int param_used_callback(void *_container, int argc, char **argv, char **azColName)
{
	char **container = _container;
	static char buf[256];

	snprintf(buf, sizeof(buf), "%s", argv[0]);
	*container = buf;
	return 0;
}

static void print_container_struct_members(struct expression *call, struct expression *expr, int param, struct stree *stree,
	void (*callback)(struct expression *call, int param, char *printed_name, struct sm_state *sm))
{
	struct expression *tmp;
	char *container = NULL;
	int offset;
	int holder_offset;
	char *p;

	if (!call->fn || !call->fn->symbol || call->fn->type != EXPR_SYMBOL)
		return;

	/*
	 * We can't use the in-mem DB because we have to parse the function
	 * first, then we know if it takes a container, then we know to pass it
	 * the container data.
	 *
	 */
	run_sql(&param_used_callback, &container,
		"select key from call_implies where %s and type = %d and key like '%%$(%%' and parameter = %d limit 1;",
		get_static_filter(call->fn->symbol), CONTAINER, param);
	if (!container)
		return;

	p = strchr(container, '-');
	if (!p)
		return;
	offset = atoi(p);
	p = strchr(p, ')');
	if (!p)
		return;
	p++;

	tmp = get_assigned_expr(expr);
	if (tmp)
		expr = tmp;

	if (expr->type != EXPR_PREOP || expr->op != '&')
		return;
	expr = strip_expr(expr->unop);
	holder_offset = get_member_offset_from_deref(expr);
	if (-holder_offset != offset)
		return;

	expr = strip_expr(expr->deref);
	if (expr->type == EXPR_PREOP && expr->op == '*')
		expr = strip_expr(expr->unop);

	print_struct_members(call, expr, param, holder_offset, stree, callback);
}

static void match_call_info(struct expression *call)
{
	struct member_info_callback *cb;
	struct expression *arg;
	struct stree *stree;
	char *name;
	int i;

	name = get_fnptr_name(call->fn);
	if (!name)
		return;

	FOR_EACH_PTR(member_callbacks, cb) {
		stree = get_all_states_stree(cb->owner);
		i = 0;
		FOR_EACH_PTR(call->args, arg) {
			print_struct_members(call, arg, i, -1, stree, cb->callback);
			print_container_struct_members(call, arg, i, stree, cb->callback);
			i++;
		} END_FOR_EACH_PTR(arg);
		free_stree(&stree);
	} END_FOR_EACH_PTR(cb);

	free_string(name);
}

static int get_param(int param, char **name, struct symbol **sym)
{
	struct symbol *arg;
	int i;

	i = 0;
	FOR_EACH_PTR(cur_func_sym->ctype.base_type->arguments, arg) {
		/*
		 * this is a temporary hack to work around a bug (I think in sparse?)
		 * 2.6.37-rc1:fs/reiserfs/journal.o
		 * If there is a function definition without parameter name found
		 * after a function implementation then it causes a crash.
		 * int foo() {}
		 * int bar(char *);
		 */
		if (arg->ident->name < (char *)100)
			continue;
		if (i == param) {
			*name = arg->ident->name;
			*sym = arg;
			return TRUE;
		}
		i++;
	} END_FOR_EACH_PTR(arg);

	return FALSE;
}

static int function_signature_matches(const char *sig)
{
	char *my_sig;

	my_sig = function_signature();
	if (!sig || !my_sig)
		return 1;  /* default to matching */
	if (strcmp(my_sig, sig) == 0)
		  return 1;
	return 0;
}

static int caller_info_callback(void *_data, int argc, char **argv, char **azColName)
{
	struct select_caller_info_data *data = _data;
	int func_id;
	long type;
	long param;
	char *key;
	char *value;
	char *name = NULL;
	struct symbol *sym = NULL;
	struct def_callback *def_callback;
	struct stree *stree;

	data->results = 1;

	if (argc != 5)
		return 0;

	func_id = atoi(argv[0]);
	errno = 0;
	type = strtol(argv[1], NULL, 10);
	param = strtol(argv[2], NULL, 10);
	if (errno)
		return 0;
	key = argv[3];
	value = argv[4];

	if (data->prev_func_id == -1)
		data->prev_func_id = func_id;
	if (func_id != data->prev_func_id) {
		stree = __pop_fake_cur_stree();
		if (!data->ignore)
			merge_stree(&data->final_states, stree);
		free_stree(&stree);
		__push_fake_cur_stree();
		__unnullify_path();
		data->prev_func_id = func_id;
		data->ignore = 0;
	}

	if (data->ignore)
		return 0;
	if (type == INTERNAL &&
	    !function_signature_matches(value)) {
		data->ignore = 1;
		return 0;
	}

	if (param >= 0 && !get_param(param, &name, &sym))
		return 0;

	FOR_EACH_PTR(select_caller_info_callbacks, def_callback) {
		if (def_callback->hook_type == type)
			def_callback->callback(name, sym, key, value);
	} END_FOR_EACH_PTR(def_callback);

	return 0;
}

static void get_direct_callers(struct select_caller_info_data *data, struct symbol *sym)
{
	sql_select_caller_info(data,
			       "call_id, type, parameter, key, value", sym,
			       caller_info_callback);
}

static struct string_list *ptr_names_done;
static struct string_list *ptr_names;

static int get_ptr_name(void *unused, int argc, char **argv, char **azColName)
{
	insert_string(&ptr_names, alloc_string(argv[0]));
	return 0;
}

static char *get_next_ptr_name(void)
{
	char *ptr;

	FOR_EACH_PTR(ptr_names, ptr) {
		if (list_has_string(ptr_names_done, ptr))
			continue;
		insert_string(&ptr_names_done, ptr);
		return ptr;
	} END_FOR_EACH_PTR(ptr);
	return NULL;
}

static void get_ptr_names(const char *file, const char *name)
{
	char sql_filter[1024];
	int before, after;

	if (file) {
		snprintf(sql_filter, 1024, "file = '%s' and function = '%s';",
			 file, name);
	} else {
		snprintf(sql_filter, 1024, "function = '%s';", name);
	}

	before = ptr_list_size((struct ptr_list *)ptr_names);

	run_sql(get_ptr_name, NULL,
		"select distinct ptr from function_ptr where %s",
		sql_filter);

	after = ptr_list_size((struct ptr_list *)ptr_names);
	if (before == after)
		return;

	while ((name = get_next_ptr_name()))
		get_ptr_names(NULL, name);
}

static void match_data_from_db(struct symbol *sym)
{
	struct select_caller_info_data data = { .prev_func_id = -1 };
	struct sm_state *sm;
	struct stree *stree;

	if (!sym || !sym->ident)
		return;

	__push_fake_cur_stree();
	__unnullify_path();

	if (!__inline_fn) {
		char *ptr;

		if (sym->ctype.modifiers & MOD_STATIC)
			get_ptr_names(get_base_file(), sym->ident->name);
		else
			get_ptr_names(NULL, sym->ident->name);

		if (ptr_list_size((struct ptr_list *)ptr_names) > 20) {
			__free_ptr_list((struct ptr_list **)&ptr_names);
			__free_ptr_list((struct ptr_list **)&ptr_names_done);
			stree = __pop_fake_cur_stree();
			free_stree(&stree);
			return;
		}

		get_direct_callers(&data, sym);

		stree = __pop_fake_cur_stree();
		if (!data.ignore)
			merge_stree(&data.final_states, stree);
		free_stree(&stree);
		__push_fake_cur_stree();
		__unnullify_path();
		data.prev_func_id = -1;
		data.ignore = 0;

		FOR_EACH_PTR(ptr_names, ptr) {
			run_sql(caller_info_callback, &data,
				"select call_id, type, parameter, key, value"
				" from common_caller_info where function = '%s' order by call_id",
				ptr);
		} END_FOR_EACH_PTR(ptr);

		if (data.results) {
			FOR_EACH_PTR(ptr_names, ptr) {
				free_string(ptr);
			} END_FOR_EACH_PTR(ptr);
			goto free_ptr_names;
		}

		FOR_EACH_PTR(ptr_names, ptr) {
			run_sql(caller_info_callback, &data,
				"select call_id, type, parameter, key, value"
				" from caller_info where function = '%s' order by call_id",
				ptr);
			free_string(ptr);
		} END_FOR_EACH_PTR(ptr);

free_ptr_names:
		__free_ptr_list((struct ptr_list **)&ptr_names);
		__free_ptr_list((struct ptr_list **)&ptr_names_done);
	} else {
		get_direct_callers(&data, sym);
	}

	stree = __pop_fake_cur_stree();
	if (!data.ignore)
		merge_stree(&data.final_states, stree);
	free_stree(&stree);

	FOR_EACH_SM(data.final_states, sm) {
		__set_sm(sm);
	} END_FOR_EACH_SM(sm);

	free_stree(&data.final_states);
}

static int call_implies_callbacks(void *_call, int argc, char **argv, char **azColName)
{
	struct expression *call_expr = _call;
	struct call_implies_callback *cb;
	struct expression *arg = NULL;
	int type;
	int param;

	if (argc != 5)
		return 0;

	type = atoi(argv[1]);
	param = atoi(argv[2]);

	FOR_EACH_PTR(call_implies_cb_list, cb) {
		if (cb->type != type)
			continue;
		if (param != -1) {
			arg = get_argument_from_call_expr(call_expr->args, param);
			if (!arg)
				continue;
		}
		cb->callback(call_expr, arg, argv[3], argv[4]);
	} END_FOR_EACH_PTR(cb);

	return 0;
}

static void match_call_implies(struct expression *expr)
{
	sql_select_call_implies("function, type, parameter, key, value", expr,
				call_implies_callbacks);
}

static void print_initializer_list(struct expression_list *expr_list,
		struct symbol *struct_type)
{
	struct expression *expr;
	struct symbol *base_type;
	char struct_name[256];

	FOR_EACH_PTR(expr_list, expr) {
		if (expr->type == EXPR_INDEX && expr->idx_expression && expr->idx_expression->type == EXPR_INITIALIZER) {
			print_initializer_list(expr->idx_expression->expr_list, struct_type);
			continue;
		}
		if (expr->type != EXPR_IDENTIFIER)
			continue;
		if (!expr->expr_ident)
			continue;
		if (!expr->ident_expression || !expr->ident_expression->symbol_name)
			continue;
		base_type = get_type(expr->ident_expression);
		if (!base_type || base_type->type != SYM_FN)
			continue;
		snprintf(struct_name, sizeof(struct_name), "(struct %s)->%s",
			 struct_type->ident->name, expr->expr_ident->name);
		sql_insert_function_ptr(expr->ident_expression->symbol_name->name,
				        struct_name);
	} END_FOR_EACH_PTR(expr);
}

static void global_variable(struct symbol *sym)
{
	struct symbol *struct_type;

	if (!sym->ident)
		return;
	if (!sym->initializer || sym->initializer->type != EXPR_INITIALIZER)
		return;
	struct_type = get_base_type(sym);
	if (!struct_type)
		return;
	if (struct_type->type == SYM_ARRAY) {
		struct_type = get_base_type(struct_type);
		if (!struct_type)
			return;
	}
	if (struct_type->type != SYM_STRUCT || !struct_type->ident)
		return;
	print_initializer_list(sym->initializer->expr_list, struct_type);
}

static void match_return_info(int return_id, char *return_ranges, struct expression *expr)
{
	sql_insert_return_states(return_id, return_ranges, INTERNAL, -1, "", function_signature());
}

static void call_return_state_hooks_conditional(struct expression *expr)
{
	struct returned_state_callback *cb;
	struct range_list *rl;
	char *return_ranges;
	int final_pass_orig = final_pass;

	__push_fake_cur_stree();

	final_pass = 0;
	__split_whole_condition(expr->conditional);
	final_pass = final_pass_orig;

	if (get_implied_rl(expr->cond_true, &rl))
		rl = cast_rl(cur_func_return_type(), rl);
	else
		rl = cast_rl(cur_func_return_type(), alloc_whole_rl(get_type(expr->cond_true)));
	return_ranges = show_rl(rl);
	set_state(RETURN_ID, "return_ranges", NULL, alloc_estate_rl(rl));

	return_id++;
	FOR_EACH_PTR(returned_state_callbacks, cb) {
		cb->callback(return_id, return_ranges, expr->cond_true);
	} END_FOR_EACH_PTR(cb);

	__push_true_states();
	__use_false_states();

	if (get_implied_rl(expr->cond_false, &rl))
		rl = cast_rl(cur_func_return_type(), rl);
	else
		rl = cast_rl(cur_func_return_type(), alloc_whole_rl(get_type(expr->cond_false)));
	return_ranges = show_rl(rl);
	set_state(RETURN_ID, "return_ranges", NULL, alloc_estate_rl(rl));

	return_id++;
	FOR_EACH_PTR(returned_state_callbacks, cb) {
		cb->callback(return_id, return_ranges, expr->cond_false);
	} END_FOR_EACH_PTR(cb);

	__merge_true_states();
	__free_fake_cur_stree();
}

static void call_return_state_hooks_compare(struct expression *expr)
{
	struct returned_state_callback *cb;
	char *return_ranges;
	int final_pass_orig = final_pass;
	sval_t sval = { .type = &int_ctype };
	sval_t ret;

	if (!get_implied_value(expr, &ret))
		ret.value = -1;

	__push_fake_cur_stree();

	final_pass = 0;
	__split_whole_condition(expr);
	final_pass = final_pass_orig;

	if (ret.value != 0) {
		return_ranges = alloc_sname("1");
		sval.value = 1;
		set_state(RETURN_ID, "return_ranges", NULL, alloc_estate_sval(sval));

		return_id++;
		FOR_EACH_PTR(returned_state_callbacks, cb) {
			cb->callback(return_id, return_ranges, expr);
		} END_FOR_EACH_PTR(cb);
	}

	__push_true_states();
	__use_false_states();

	if (ret.value != 1) {
		return_ranges = alloc_sname("0");
		sval.value = 0;
		set_state(RETURN_ID, "return_ranges", NULL, alloc_estate_sval(sval));

		return_id++;
		FOR_EACH_PTR(returned_state_callbacks, cb) {
			cb->callback(return_id, return_ranges, expr);
		} END_FOR_EACH_PTR(cb);
	}

	__merge_true_states();
	__free_fake_cur_stree();
}

static int ptr_in_list(struct sm_state *sm, struct state_list *slist)
{
	struct sm_state *tmp;

	FOR_EACH_PTR(slist, tmp) {
		if (strcmp(tmp->state->name, sm->state->name) == 0)
			return 1;
	} END_FOR_EACH_PTR(tmp);

	return 0;
}

static char *get_return_compare_str(struct expression *expr)
{
	char *compare_str;
	char *var;
	char buf[256];
	int comparison;
	int param;

	compare_str = expr_lte_to_param(expr, -1);
	if (compare_str)
		return compare_str;
	param = get_param_num(expr);
	if (param < 0)
		return NULL;

	var = expr_to_var(expr);
	if (!var)
		return NULL;
	snprintf(buf, sizeof(buf), "%s orig", var);
	comparison = get_comparison_strings(var, buf);
	free_string(var);

	if (!comparison)
		return NULL;

	snprintf(buf, sizeof(buf), "[%s$%d]", show_special(comparison), param);
	return alloc_sname(buf);
}

static int split_possible_helper(struct sm_state *sm, struct expression *expr)
{
	struct returned_state_callback *cb;
	struct range_list *rl;
	char *return_ranges;
	struct sm_state *tmp;
	int ret = 0;
	int nr_possible, nr_states;
	char *compare_str = NULL;
	char buf[128];
	struct state_list *already_handled = NULL;

	if (!sm || !sm->merged)
		return 0;

	if (too_many_possible(sm))
		return 0;

	/* bail if it gets too complicated */
	nr_possible = ptr_list_size((struct ptr_list *)sm->possible);
	nr_states = stree_count(__get_cur_stree());
	if (nr_states * nr_possible >= 2000)
		return 0;

	FOR_EACH_PTR(sm->possible, tmp) {
		if (tmp->merged)
			continue;
		if (ptr_in_list(tmp, already_handled))
			continue;
		add_ptr_list(&already_handled, tmp);

		ret = 1;
		__push_fake_cur_stree();

		overwrite_states_using_pool(sm, tmp);

		rl = cast_rl(cur_func_return_type(), estate_rl(tmp->state));
		return_ranges = show_rl(rl);
		set_state(RETURN_ID, "return_ranges", NULL, alloc_estate_rl(clone_rl(rl)));
		compare_str = get_return_compare_str(expr);
		if (compare_str) {
			snprintf(buf, sizeof(buf), "%s%s", return_ranges, compare_str);
			return_ranges = alloc_sname(buf);
		}

		return_id++;
		FOR_EACH_PTR(returned_state_callbacks, cb) {
			cb->callback(return_id, return_ranges, expr);
		} END_FOR_EACH_PTR(cb);

		__free_fake_cur_stree();
	} END_FOR_EACH_PTR(tmp);

	free_slist(&already_handled);

	return ret;
}

static int call_return_state_hooks_split_possible(struct expression *expr)
{
	struct sm_state *sm;

	if (!expr || expr_equal_to_param(expr, -1))
		return 0;

	sm = get_sm_state_expr(SMATCH_EXTRA, expr);
	return split_possible_helper(sm, expr);
}

static const char *get_return_ranges_str(struct expression *expr, struct range_list **rl_p)
{
	struct range_list *rl;
	char *return_ranges;
	sval_t sval;
	char *compare_str;
	char *math_str;
	char buf[128];

	*rl_p = NULL;

	if (!expr)
		return alloc_sname("");

	if (get_implied_value(expr, &sval)) {
		sval = sval_cast(cur_func_return_type(), sval);
		*rl_p = alloc_rl(sval, sval);
		return sval_to_str(sval);
	}

	compare_str = expr_equal_to_param(expr, -1);
	math_str = get_value_in_terms_of_parameter_math(expr);

	if (get_implied_rl(expr, &rl)) {
		rl = cast_rl(cur_func_return_type(), rl);
		return_ranges = show_rl(rl);
	} else if (get_imaginary_absolute(expr, &rl)){
		rl = cast_rl(cur_func_return_type(), rl);
		return alloc_sname(show_rl(rl));
	} else {
		rl = cast_rl(cur_func_return_type(), alloc_whole_rl(get_type(expr)));
		return_ranges = show_rl(rl);
	}
	*rl_p = rl;

	if (compare_str) {
		snprintf(buf, sizeof(buf), "%s%s", return_ranges, compare_str);
		return alloc_sname(buf);
	}
	if (math_str) {
		snprintf(buf, sizeof(buf), "%s[%s]", return_ranges, math_str);
		return alloc_sname(buf);
	}
	compare_str = get_return_compare_str(expr);
	if (compare_str) {
		snprintf(buf, sizeof(buf), "%s%s", return_ranges, compare_str);
		return alloc_sname(buf);
	}

	return return_ranges;
}

static int split_positive_from_negative(struct expression *expr)
{
	struct returned_state_callback *cb;
	struct range_list *rl;
	const char *return_ranges;
	struct range_list *ret_rl;
	int undo;

	/* We're going to print the states 3 times */
	if (stree_count(__get_cur_stree()) > 10000 / 3)
		return 0;

	if (!get_implied_rl(expr, &rl) || !rl)
		return 0;
	if (is_whole_rl(rl) || is_whole_rl_non_zero(rl))
		return 0;
	/* Forget about INT_MAX and larger */
	if (rl_max(rl).value <= 0)
		return 0;
	if (!sval_is_negative(rl_min(rl)))
		return 0;

	if (!assume(compare_expression(expr, '>', zero_expr())))
		return 0;

	return_id++;
	return_ranges = get_return_ranges_str(expr, &ret_rl);
	set_state(RETURN_ID, "return_ranges", NULL, alloc_estate_rl(ret_rl));
	FOR_EACH_PTR(returned_state_callbacks, cb) {
		cb->callback(return_id, (char *)return_ranges, expr);
	} END_FOR_EACH_PTR(cb);

	end_assume();

	if (rl_has_sval(rl, sval_type_val(rl_type(rl), 0))) {
		undo = assume(compare_expression(expr, SPECIAL_EQUAL, zero_expr()));

		return_id++;
		return_ranges = get_return_ranges_str(expr, &ret_rl);
		set_state(RETURN_ID, "return_ranges", NULL, alloc_estate_rl(ret_rl));
		FOR_EACH_PTR(returned_state_callbacks, cb) {
			cb->callback(return_id, (char *)return_ranges, expr);
		} END_FOR_EACH_PTR(cb);

		if (undo)
			end_assume();
	}

	undo = assume(compare_expression(expr, '<', zero_expr()));

	return_id++;
	return_ranges = get_return_ranges_str(expr, &ret_rl);
	set_state(RETURN_ID, "return_ranges", NULL, alloc_estate_rl(ret_rl));
	FOR_EACH_PTR(returned_state_callbacks, cb) {
		cb->callback(return_id, (char *)return_ranges, expr);
	} END_FOR_EACH_PTR(cb);

	if (undo)
		end_assume();

	return 1;
}

static int call_return_state_hooks_split_null_non_null(struct expression *expr)
{
	struct returned_state_callback *cb;
	struct range_list *rl;
	struct range_list *nonnull_rl;
	sval_t null_sval;
	struct range_list *null_rl = NULL;
	char *return_ranges;
	struct sm_state *sm;
	struct smatch_state *state;
	int nr_states;
	int final_pass_orig = final_pass;

	if (!expr || expr_equal_to_param(expr, -1))
		return 0;
	if (expr->type == EXPR_CALL)
		return 0;
	if (!is_pointer(expr))
		return 0;

	sm = get_sm_state_expr(SMATCH_EXTRA, expr);
	if (!sm)
		return 0;
	if (ptr_list_size((struct ptr_list *)sm->possible) == 1)
		return 0;
	state = sm->state;
	if (!estate_rl(state))
		return 0;
	if (estate_min(state).value == 0 && estate_max(state).value == 0)
		return 0;
	if (!rl_has_sval(estate_rl(state), sval_type_val(estate_type(state), 0)))
		return 0;

	nr_states = stree_count(__get_cur_stree());
	if (option_info && nr_states >= 1500)
		return 0;

	rl = estate_rl(state);

	__push_fake_cur_stree();

	final_pass = 0;
	__split_whole_condition(expr);
	final_pass = final_pass_orig;

	nonnull_rl = rl_filter(rl, rl_zero());
	return_ranges = show_rl(nonnull_rl);
	set_state(RETURN_ID, "return_ranges", NULL, alloc_estate_rl(nonnull_rl));

	return_id++;
	FOR_EACH_PTR(returned_state_callbacks, cb) {
		cb->callback(return_id, return_ranges, expr);
	} END_FOR_EACH_PTR(cb);

	__push_true_states();
	__use_false_states();

	return_ranges = alloc_sname("0");
	null_sval = sval_type_val(rl_type(rl), 0);
	add_range(&null_rl, null_sval, null_sval);
	set_state(RETURN_ID, "return_ranges", NULL, alloc_estate_rl(null_rl));
	return_id++;
	FOR_EACH_PTR(returned_state_callbacks, cb) {
		cb->callback(return_id, return_ranges, expr);
	} END_FOR_EACH_PTR(cb);

	__merge_true_states();
	__free_fake_cur_stree();

	return 1;
}

static int call_return_state_hooks_split_success_fail(struct expression *expr)
{
	struct range_list *rl;
	struct range_list *nonzero_rl;
	sval_t zero_sval;
	struct range_list *zero_rl = NULL;
	int nr_states;
	struct returned_state_callback *cb;
	char *return_ranges;
	int final_pass_orig = final_pass;
	sval_t val;

	if (option_project != PROJ_KERNEL)
		return 0;

	nr_states = stree_count(__get_cur_stree());
	if (nr_states > 1500)
		return 0;

	if (get_value(expr, &val))
		return 0;
	if (!get_implied_rl(expr, &rl))
		return 0;
	if (rl_min(rl).value < -4095 || rl_min(rl).value >= 0)
		return 0;
	if (rl_max(rl).value != 0)
		return 0;

	__push_fake_cur_stree();

	final_pass = 0;
	__split_whole_condition(expr);
	final_pass = final_pass_orig;

	nonzero_rl = rl_filter(rl, rl_zero());
	return_ranges = show_rl(nonzero_rl);
	set_state(RETURN_ID, "return_ranges", NULL, alloc_estate_rl(nonzero_rl));

	return_id++;
	FOR_EACH_PTR(returned_state_callbacks, cb) {
		cb->callback(return_id, return_ranges, expr);
	} END_FOR_EACH_PTR(cb);

	__push_true_states();
	__use_false_states();

	return_ranges = alloc_sname("0");
	zero_sval = sval_type_val(rl_type(rl), 0);
	add_range(&zero_rl, zero_sval, zero_sval);
	set_state(RETURN_ID, "return_ranges", NULL, alloc_estate_rl(zero_rl));
	return_id++;
	FOR_EACH_PTR(returned_state_callbacks, cb) {
		cb->callback(return_id, return_ranges, expr);
	} END_FOR_EACH_PTR(cb);

	__merge_true_states();
	__free_fake_cur_stree();

	return 1;
}

static int is_boolean(struct expression *expr)
{
	struct range_list *rl;

	if (!get_implied_rl(expr, &rl))
		return 0;
	if (rl_min(rl).value == 0 && rl_max(rl).value == 1)
		return 1;
	return 0;
}

static int is_conditional(struct expression *expr)
{
	if (!expr)
		return 0;
	if (expr->type == EXPR_CONDITIONAL || expr->type == EXPR_SELECT)
		return 1;
	return 0;
}

static int splitable_function_call(struct expression *expr)
{
	struct sm_state *sm;
	char buf[64];

	if (!expr || expr->type != EXPR_CALL)
		return 0;
	snprintf(buf, sizeof(buf), "return %p", expr);
	sm = get_sm_state(SMATCH_EXTRA, buf, NULL);
	return split_possible_helper(sm, expr);
}

static struct sm_state *find_bool_param(void)
{
	struct stree *start_states;
	struct symbol *arg;
	struct sm_state *sm, *tmp;
	sval_t sval;

	start_states = get_start_states();

	FOR_EACH_PTR_REVERSE(cur_func_sym->ctype.base_type->arguments, arg) {
		if (!arg->ident)
			continue;
		sm = get_sm_state_stree(start_states, SMATCH_EXTRA, arg->ident->name, arg);
		if (!sm)
			continue;
		if (rl_min(estate_rl(sm->state)).value != 0 ||
		    rl_max(estate_rl(sm->state)).value != 1)
			continue;
		goto found;
	} END_FOR_EACH_PTR_REVERSE(arg);

	return NULL;

found:
	/*
	 * Check if it's splitable.  If not, then splitting it up is likely not
	 * useful for the callers.
	 */
	FOR_EACH_PTR(sm->possible, tmp) {
		if (is_merged(tmp))
			continue;
		if (!estate_get_single_value(tmp->state, &sval))
			return NULL;
	} END_FOR_EACH_PTR(tmp);

	return sm;
}

static int split_on_bool_sm(struct sm_state *sm, struct expression *expr)
{
	struct returned_state_callback *cb;
	struct range_list *ret_rl;
	const char *return_ranges;
	struct sm_state *tmp;
	int ret = 0;
	int nr_possible, nr_states;
	char *compare_str = NULL;
	char buf[128];
	struct state_list *already_handled = NULL;

	if (!sm || !sm->merged)
		return 0;

	if (too_many_possible(sm))
		return 0;

	/* bail if it gets too complicated */
	nr_possible = ptr_list_size((struct ptr_list *)sm->possible);
	nr_states = stree_count(__get_cur_stree());
	if (nr_states * nr_possible >= 2000)
		return 0;

	FOR_EACH_PTR(sm->possible, tmp) {
		if (tmp->merged)
			continue;
		if (ptr_in_list(tmp, already_handled))
			continue;
		add_ptr_list(&already_handled, tmp);

		ret = 1;
		__push_fake_cur_stree();

		overwrite_states_using_pool(sm, tmp);

		return_ranges = get_return_ranges_str(expr, &ret_rl);
		set_state(RETURN_ID, "return_ranges", NULL, alloc_estate_rl(ret_rl));
		compare_str = get_return_compare_str(expr);
		if (compare_str) {
			snprintf(buf, sizeof(buf), "%s%s", return_ranges, compare_str);
			return_ranges = alloc_sname(buf);
		}

		return_id++;
		FOR_EACH_PTR(returned_state_callbacks, cb) {
			cb->callback(return_id, (char *)return_ranges, expr);
		} END_FOR_EACH_PTR(cb);

		__free_fake_cur_stree();
	} END_FOR_EACH_PTR(tmp);

	free_slist(&already_handled);

	return ret;
}

static int split_by_bool_param(struct expression *expr)
{
	struct sm_state *start_sm, *sm;
	sval_t sval;

	start_sm = find_bool_param();
	if (!start_sm)
		return 0;
	sm = get_sm_state(SMATCH_EXTRA, start_sm->name, start_sm->sym);
	if (!sm || estate_get_single_value(sm->state, &sval))
		return 0;
	return split_on_bool_sm(sm, expr);
}

static int split_by_null_nonnull_param(struct expression *expr)
{
	struct symbol *arg;
	struct sm_state *sm;

	/* function must only take one pointer */
	if (ptr_list_size((struct ptr_list *)cur_func_sym->ctype.base_type->arguments) != 1)
		return 0;
	arg = first_ptr_list((struct ptr_list *)cur_func_sym->ctype.base_type->arguments);
	if (!arg->ident)
		return 0;
	if (get_real_base_type(arg)->type != SYM_PTR)
		return 0;

	if (param_was_set_var_sym(arg->ident->name, arg))
		return 0;
	sm = get_sm_state(SMATCH_EXTRA, arg->ident->name, arg);
	if (!sm)
		return 0;

	return split_on_bool_sm(sm, expr);
}

struct expression *strip_expr_statement(struct expression *expr)
{
	struct expression *orig = expr;
	struct statement *stmt, *last_stmt;

	if (!expr)
		return NULL;
	if (expr->type == EXPR_PREOP && expr->op == '(')
		expr = expr->unop;
	if (expr->type != EXPR_STATEMENT)
		return orig;
	stmt = expr->statement;
	if (!stmt || stmt->type != STMT_COMPOUND)
		return orig;

	last_stmt = last_ptr_list((struct ptr_list *)stmt->stmts);
	if (!last_stmt || last_stmt->type == STMT_LABEL)
		last_stmt = last_stmt->label_statement;
	if (!last_stmt || last_stmt->type != STMT_EXPRESSION)
		return orig;
	return strip_expr(last_stmt->expression);
}

static void call_return_state_hooks(struct expression *expr)
{
	struct returned_state_callback *cb;
	struct range_list *ret_rl;
	const char *return_ranges;
	int nr_states;
	sval_t sval;

	if (__path_is_null())
		return;

	expr = strip_expr(expr);
	expr = strip_expr_statement(expr);

	if (is_impossible_path())
		goto vanilla;

	if (expr && (expr->type == EXPR_COMPARE ||
		     !get_implied_value(expr, &sval)) &&
	    (is_condition(expr) || is_boolean(expr))) {
		call_return_state_hooks_compare(expr);
		return;
	} else if (is_conditional(expr)) {
		call_return_state_hooks_conditional(expr);
		return;
	} else if (call_return_state_hooks_split_possible(expr)) {
		return;
	} else if (call_return_state_hooks_split_null_non_null(expr)) {
		return;
	} else if (call_return_state_hooks_split_success_fail(expr)) {
		return;
	} else if (splitable_function_call(expr)) {
		return;
	} else if (split_positive_from_negative(expr)) {
		return;
	} else if (split_by_bool_param(expr)) {
	} else if (split_by_null_nonnull_param(expr)) {
		return;
	}

vanilla:
	return_ranges = get_return_ranges_str(expr, &ret_rl);
	set_state(RETURN_ID, "return_ranges", NULL, alloc_estate_rl(ret_rl));

	return_id++;
	nr_states = stree_count(__get_cur_stree());
	if (nr_states >= 10000) {
		match_return_info(return_id, (char *)return_ranges, expr);
		return;
	}
	FOR_EACH_PTR(returned_state_callbacks, cb) {
		cb->callback(return_id, (char *)return_ranges, expr);
	} END_FOR_EACH_PTR(cb);
}

static void print_returned_struct_members(int return_id, char *return_ranges, struct expression *expr)
{
	struct returned_member_callback *cb;
	struct stree *stree;
	struct sm_state *sm;
	struct symbol *type;
	char *name;
	char member_name[256];
	int len;

	type = get_type(expr);
	if (!type || type->type != SYM_PTR)
		return;
	name = expr_to_var(expr);
	if (!name)
		return;

	member_name[sizeof(member_name) - 1] = '\0';
	strcpy(member_name, "$");

	len = strlen(name);
	FOR_EACH_PTR(returned_member_callbacks, cb) {
		stree = __get_cur_stree();
		FOR_EACH_MY_SM(cb->owner, stree, sm) {
			if (sm->name[0] == '*' && strcmp(sm->name + 1, name) == 0) {
				strcpy(member_name, "*$");
				cb->callback(return_id, return_ranges, expr, member_name, sm->state);
				continue;
			}
			if (strncmp(sm->name, name, len) != 0)
				continue;
			if (strncmp(sm->name + len, "->", 2) != 0)
				continue;
			snprintf(member_name, sizeof(member_name), "$%s", sm->name + len);
			cb->callback(return_id, return_ranges, expr, member_name, sm->state);
		} END_FOR_EACH_SM(sm);
	} END_FOR_EACH_PTR(cb);

	free_string(name);
}

static void reset_memdb(struct symbol *sym)
{
	mem_sql(NULL, NULL, "delete from caller_info;");
	mem_sql(NULL, NULL, "delete from return_states;");
	mem_sql(NULL, NULL, "delete from call_implies;");
}

static void match_end_func_info(struct symbol *sym)
{
	if (__path_is_null())
		return;
	call_return_state_hooks(NULL);
}

static void match_after_func(struct symbol *sym)
{
	if (!__inline_fn)
		reset_memdb(sym);
}

static void init_memdb(void)
{
	char *err = NULL;
	int rc;
	const char *schema_files[] = {
		"db/db.schema",
		"db/caller_info.schema",
		"db/return_states.schema",
		"db/function_type_size.schema",
		"db/type_size.schema",
		"db/call_implies.schema",
		"db/function_ptr.schema",
		"db/local_values.schema",
		"db/function_type_value.schema",
		"db/type_value.schema",
		"db/function_type.schema",
		"db/data_info.schema",
		"db/parameter_name.schema",
		"db/constraints.schema",
		"db/constraints_required.schema",
		"db/fn_ptr_data_link.schema",
		"db/fn_data_link.schema",
		"db/common_caller_info.schema",
	};
	static char buf[4096];
	int fd;
	int ret;
	int i;

	rc = sqlite3_open(":memory:", &mem_db);
	if (rc != SQLITE_OK) {
		printf("Error starting In-Memory database.");
		return;
	}

	for (i = 0; i < ARRAY_SIZE(schema_files); i++) {
		fd = open_data_file(schema_files[i]);
		if (fd < 0) {
			printf("failed to open: %s\n", schema_files[i]);
			continue;
		}
		ret = read(fd, buf, sizeof(buf));
		if (ret < 0) {
			printf("failed to read: %s\n", schema_files[i]);
			continue;
		}
		close(fd);
		if (ret == sizeof(buf)) {
			printf("Schema file too large:  %s (limit %zd bytes)",
			       schema_files[i], sizeof(buf));
			continue;
		}
		buf[ret] = '\0';
		rc = sqlite3_exec(mem_db, buf, NULL, NULL, &err);
		if (rc != SQLITE_OK) {
			fprintf(stderr, "SQL error #2: %s\n", err);
			fprintf(stderr, "%s\n", buf);
		}
	}
}

void open_smatch_db(void)
{
	int rc;

	if (option_no_db)
		return;

	init_memdb();

	rc = sqlite3_open_v2("smatch_db.sqlite", &db, SQLITE_OPEN_READONLY, NULL);
	if (rc != SQLITE_OK) {
		option_no_db = 1;
		return;
	}
	return;
}

static void register_common_funcs(void)
{
	struct token *token;
	char *func;
	char filename[256];

	if (option_project == PROJ_NONE)
		strcpy(filename, "common_functions");
	else
		snprintf(filename, 256, "%s.common_functions", option_project_str);

	token = get_tokens_file(filename);
	if (!token)
		return;
	if (token_type(token) != TOKEN_STREAMBEGIN)
		return;
	token = token->next;
	while (token_type(token) != TOKEN_STREAMEND) {
		if (token_type(token) != TOKEN_IDENT)
			return;
		func = alloc_string(show_ident(token->ident));
		add_ptr_list(&common_funcs, func);
		token = token->next;
	}
	clear_token_alloc();
}

static char *get_next_string(char **str)
{
	static char string[256];
	char *start;
	char *p = *str;
	int len;

	if (*p == '\0')
		return NULL;
	start = p;

	while (*p != '\0' && *p != ' ' && *p != '\n')
		p++;

	len = p - start;
	if (len > 256) {
		memcpy(string, start, 255);
		string[255] = '\0';
		printf("return_fix: '%s' too long", string);
		**str = '\0';
		return NULL;
	}
	memcpy(string, start, len);
	string[len] = '\0';
	if (*p != '\0')
		p++;
	*str = p;
	return string;
}

static void register_return_replacements(void)
{
	char *func, *orig, *new;
	char filename[256];
	char buf[4096];
	int fd, ret, i;
	char *p;

	snprintf(filename, 256, "db/%s.return_fixes", option_project_str);
	fd = open_data_file(filename);
	if (fd < 0)
		return;
	ret = read(fd, buf, sizeof(buf));
	close(fd);
	if (ret < 0)
		return;
	if (ret == sizeof(buf)) {
		printf("file too large:  %s (limit %zd bytes)",
		       filename, sizeof(buf));
		return;
	}
	buf[ret] = '\0';

	p = buf;
	while (*p) {
		get_next_string(&p);
		replace_count++;
	}
	if (replace_count == 0 || replace_count % 3 != 0) {
		replace_count = 0;
		return;
	}
	replace_table = malloc(replace_count * sizeof(char *));

	p = buf;
	i = 0;
	while (*p) {
		func = alloc_string(get_next_string(&p));
		orig = alloc_string(get_next_string(&p));
		new  = alloc_string(get_next_string(&p));

		replace_table[i++] = func;
		replace_table[i++] = orig;
		replace_table[i++] = new;
	}
}

void register_definition_db_callbacks(int id)
{
	add_hook(&match_call_info, FUNCTION_CALL_HOOK);
	add_hook(&global_variable, BASE_HOOK);
	add_hook(&global_variable, DECLARATION_HOOK);
	add_split_return_callback(match_return_info);
	add_split_return_callback(print_returned_struct_members);
	add_hook(&call_return_state_hooks, RETURN_HOOK);
	add_hook(&match_end_func_info, END_FUNC_HOOK);
	add_hook(&match_after_func, AFTER_FUNC_HOOK);

	add_hook(&match_data_from_db, FUNC_DEF_HOOK);
	add_hook(&match_call_implies, CALL_HOOK_AFTER_INLINE);

	register_common_funcs();
	register_return_replacements();
}

void register_db_call_marker(int id)
{
	add_hook(&match_call_marker, FUNCTION_CALL_HOOK);
}

char *return_state_to_var_sym(struct expression *expr, int param, const char *key, struct symbol **sym)
{
	struct expression *arg;
	char *name = NULL;
	char member_name[256];

	*sym = NULL;

	if (param == -1) {
		const char *star = "";

		if (expr->type != EXPR_ASSIGNMENT)
			return NULL;
		name = expr_to_var_sym(expr->left, sym);
		if (!name)
			return NULL;
		if (key[0] == '*') {
			star = "*";
			key++;
		}
		if (strncmp(key, "$", 1) != 0)
			return name;
		snprintf(member_name, sizeof(member_name), "%s%s%s", star, name, key + 1);
		free_string(name);
		return alloc_string(member_name);
	}

	while (expr->type == EXPR_ASSIGNMENT)
		expr = strip_expr(expr->right);
	if (expr->type != EXPR_CALL)
		return NULL;

	arg = get_argument_from_call_expr(expr->args, param);
	if (!arg)
		return NULL;

	return get_variable_from_key(arg, key, sym);
}

char *get_variable_from_key(struct expression *arg, const char *key, struct symbol **sym)
{
	char buf[256];
	char *tmp;

	if (!arg)
		return NULL;

	arg = strip_expr(arg);

	if (strcmp(key, "$") == 0)
		return expr_to_var_sym(arg, sym);

	if (strcmp(key, "*$") == 0) {
		if (arg->type == EXPR_PREOP && arg->op == '&') {
			arg = strip_expr(arg->unop);
			return expr_to_var_sym(arg, sym);
		} else {
			tmp = expr_to_var_sym(arg, sym);
			if (!tmp)
				return NULL;
			snprintf(buf, sizeof(buf), "*%s", tmp);
			free_string(tmp);
			return alloc_string(buf);
		}
	}

	if (arg->type == EXPR_PREOP && arg->op == '&') {
		arg = strip_expr(arg->unop);
		tmp = expr_to_var_sym(arg, sym);
		if (!tmp)
			return NULL;
		snprintf(buf, sizeof(buf), "%s.%s", tmp, key + 3);
		return alloc_string(buf);
	}

	tmp = expr_to_var_sym(arg, sym);
	if (!tmp)
		return NULL;
	snprintf(buf, sizeof(buf), "%s%s", tmp, key + 1);
	free_string(tmp);
	return alloc_string(buf);
}

char *get_chunk_from_key(struct expression *arg, char *key, struct symbol **sym, struct var_sym_list **vsl)
{
	*vsl = NULL;

	if (strcmp("$", key) == 0)
		return expr_to_chunk_sym_vsl(arg, sym, vsl);
	return get_variable_from_key(arg, key, sym);
}

const char *state_name_to_param_name(const char *state_name, const char *param_name)
{
	int name_len;
	static char buf[256];

	name_len = strlen(param_name);

	if (strcmp(state_name, param_name) == 0) {
		return "$";
	} else if (state_name[name_len] == '-' && /* check for '-' from "->" */
	    strncmp(state_name, param_name, name_len) == 0) {
		snprintf(buf, sizeof(buf), "$%s", state_name + name_len);
		return buf;
	} else if (state_name[0] == '*' && strcmp(state_name + 1, param_name) == 0) {
		return "*$";
	}
	return NULL;
}

const char *get_param_name_var_sym(const char *name, struct symbol *sym)
{
	if (!sym || !sym->ident)
		return NULL;

	return state_name_to_param_name(name, sym->ident->name);
}

const char *get_param_name(struct sm_state *sm)
{
	return get_param_name_var_sym(sm->name, sm->sym);
}

char *get_data_info_name(struct expression *expr)
{
	struct symbol *sym;
	char *name;
	char buf[256];
	char *ret = NULL;

	expr = strip_expr(expr);
	name = get_member_name(expr);
	if (name)
		return name;
	name = expr_to_var_sym(expr, &sym);
	if (!name || !sym)
		goto free;
	if (!(sym->ctype.modifiers & MOD_TOPLEVEL))
		goto free;
	if (sym->ctype.modifiers & MOD_STATIC)
		snprintf(buf, sizeof(buf), "static %s", name);
	else
		snprintf(buf, sizeof(buf), "global %s", name);
	ret = alloc_sname(buf);
free:
	free_string(name);
	return ret;
}
