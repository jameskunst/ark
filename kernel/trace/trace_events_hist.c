/*
 * trace_events_hist - trace event hist triggers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Copyright (C) 2015 Tom Zanussi <tom.zanussi@linux.intel.com>
 */

#include <linux/module.h>
#include <linux/kallsyms.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/stacktrace.h>
#include <linux/rculist.h>

#include "tracing_map.h"
#include "trace.h"

struct hist_field;

typedef u64 (*hist_field_fn_t) (struct hist_field *field,
				struct tracing_map_elt *elt,
				struct ring_buffer_event *rbe,
				void *event);

#define HIST_FIELD_OPERANDS_MAX	2
#define HIST_FIELDS_MAX		(TRACING_MAP_FIELDS_MAX + TRACING_MAP_VARS_MAX)

enum field_op_id {
	FIELD_OP_NONE,
	FIELD_OP_PLUS,
	FIELD_OP_MINUS,
	FIELD_OP_UNARY_MINUS,
};

struct hist_var {
	char				*name;
	struct hist_trigger_data	*hist_data;
	unsigned int			idx;
};

struct hist_field {
	struct ftrace_event_field	*field;
	unsigned long			flags;
	hist_field_fn_t			fn;
	unsigned int			size;
	unsigned int			offset;
	unsigned int                    is_signed;
	const char			*type;
	struct hist_field		*operands[HIST_FIELD_OPERANDS_MAX];
	struct hist_trigger_data	*hist_data;
	struct hist_var			var;
	enum field_op_id		operator;
	char				*name;
};

static u64 hist_field_none(struct hist_field *field,
			   struct tracing_map_elt *elt,
			   struct ring_buffer_event *rbe,
			   void *event)
{
	return 0;
}

static u64 hist_field_counter(struct hist_field *field,
			      struct tracing_map_elt *elt,
			      struct ring_buffer_event *rbe,
			      void *event)
{
	return 1;
}

static u64 hist_field_string(struct hist_field *hist_field,
			     struct tracing_map_elt *elt,
			     struct ring_buffer_event *rbe,
			     void *event)
{
	char *addr = (char *)(event + hist_field->field->offset);

	return (u64)(unsigned long)addr;
}

static u64 hist_field_dynstring(struct hist_field *hist_field,
				struct tracing_map_elt *elt,
				struct ring_buffer_event *rbe,
				void *event)
{
	u32 str_item = *(u32 *)(event + hist_field->field->offset);
	int str_loc = str_item & 0xffff;
	char *addr = (char *)(event + str_loc);

	return (u64)(unsigned long)addr;
}

static u64 hist_field_pstring(struct hist_field *hist_field,
			      struct tracing_map_elt *elt,
			      struct ring_buffer_event *rbe,
			      void *event)
{
	char **addr = (char **)(event + hist_field->field->offset);

	return (u64)(unsigned long)*addr;
}

static u64 hist_field_log2(struct hist_field *hist_field,
			   struct tracing_map_elt *elt,
			   struct ring_buffer_event *rbe,
			   void *event)
{
	struct hist_field *operand = hist_field->operands[0];

	u64 val = operand->fn(operand, elt, rbe, event);

	return (u64) ilog2(roundup_pow_of_two(val));
}

static u64 hist_field_plus(struct hist_field *hist_field,
			   struct tracing_map_elt *elt,
			   struct ring_buffer_event *rbe,
			   void *event)
{
	struct hist_field *operand1 = hist_field->operands[0];
	struct hist_field *operand2 = hist_field->operands[1];

	u64 val1 = operand1->fn(operand1, elt, rbe, event);
	u64 val2 = operand2->fn(operand2, elt, rbe, event);

	return val1 + val2;
}

static u64 hist_field_minus(struct hist_field *hist_field,
			    struct tracing_map_elt *elt,
			    struct ring_buffer_event *rbe,
			    void *event)
{
	struct hist_field *operand1 = hist_field->operands[0];
	struct hist_field *operand2 = hist_field->operands[1];

	u64 val1 = operand1->fn(operand1, elt, rbe, event);
	u64 val2 = operand2->fn(operand2, elt, rbe, event);

	return val1 - val2;
}

static u64 hist_field_unary_minus(struct hist_field *hist_field,
				  struct tracing_map_elt *elt,
				  struct ring_buffer_event *rbe,
				  void *event)
{
	struct hist_field *operand = hist_field->operands[0];

	s64 sval = (s64)operand->fn(operand, elt, rbe, event);
	u64 val = (u64)-sval;

	return val;
}

#define DEFINE_HIST_FIELD_FN(type)					\
	static u64 hist_field_##type(struct hist_field *hist_field,	\
				     struct tracing_map_elt *elt,	\
				     struct ring_buffer_event *rbe,	\
				     void *event)			\
{									\
	type *addr = (type *)(event + hist_field->field->offset);	\
									\
	return (u64)(unsigned long)*addr;				\
}

DEFINE_HIST_FIELD_FN(s64);
DEFINE_HIST_FIELD_FN(u64);
DEFINE_HIST_FIELD_FN(s32);
DEFINE_HIST_FIELD_FN(u32);
DEFINE_HIST_FIELD_FN(s16);
DEFINE_HIST_FIELD_FN(u16);
DEFINE_HIST_FIELD_FN(s8);
DEFINE_HIST_FIELD_FN(u8);

#define for_each_hist_field(i, hist_data)	\
	for ((i) = 0; (i) < (hist_data)->n_fields; (i)++)

#define for_each_hist_val_field(i, hist_data)	\
	for ((i) = 0; (i) < (hist_data)->n_vals; (i)++)

#define for_each_hist_key_field(i, hist_data)	\
	for ((i) = (hist_data)->n_vals; (i) < (hist_data)->n_fields; (i)++)

#define HIST_STACKTRACE_DEPTH	16
#define HIST_STACKTRACE_SIZE	(HIST_STACKTRACE_DEPTH * sizeof(unsigned long))
#define HIST_STACKTRACE_SKIP	5

#define HITCOUNT_IDX		0
#define HIST_KEY_SIZE_MAX	(MAX_FILTER_STR_VAL + HIST_STACKTRACE_SIZE)

enum hist_field_flags {
	HIST_FIELD_FL_HITCOUNT		= 1 << 0,
	HIST_FIELD_FL_KEY		= 1 << 1,
	HIST_FIELD_FL_STRING		= 1 << 2,
	HIST_FIELD_FL_HEX		= 1 << 3,
	HIST_FIELD_FL_SYM		= 1 << 4,
	HIST_FIELD_FL_SYM_OFFSET	= 1 << 5,
	HIST_FIELD_FL_EXECNAME		= 1 << 6,
	HIST_FIELD_FL_SYSCALL		= 1 << 7,
	HIST_FIELD_FL_STACKTRACE	= 1 << 8,
	HIST_FIELD_FL_LOG2		= 1 << 9,
	HIST_FIELD_FL_TIMESTAMP		= 1 << 10,
	HIST_FIELD_FL_TIMESTAMP_USECS	= 1 << 11,
	HIST_FIELD_FL_VAR		= 1 << 12,
	HIST_FIELD_FL_EXPR		= 1 << 13,
};

struct var_defs {
	unsigned int	n_vars;
	char		*name[TRACING_MAP_VARS_MAX];
	char		*expr[TRACING_MAP_VARS_MAX];
};

struct hist_trigger_attrs {
	char		*keys_str;
	char		*vals_str;
	char		*sort_key_str;
	char		*name;
	bool		pause;
	bool		cont;
	bool		clear;
	bool		ts_in_usecs;
	unsigned int	map_bits;

	char		*assignment_str[TRACING_MAP_VARS_MAX];
	unsigned int	n_assignments;

	struct var_defs	var_defs;
};

struct hist_trigger_data {
	struct hist_field               *fields[HIST_FIELDS_MAX];
	unsigned int			n_vals;
	unsigned int			n_keys;
	unsigned int			n_fields;
	unsigned int			n_vars;
	unsigned int			key_size;
	struct tracing_map_sort_key	sort_keys[TRACING_MAP_SORT_KEYS_MAX];
	unsigned int			n_sort_keys;
	struct trace_event_file		*event_file;
	struct hist_trigger_attrs	*attrs;
	struct tracing_map		*map;
	bool				enable_timestamps;
	bool				remove;
};

static u64 hist_field_timestamp(struct hist_field *hist_field,
				struct tracing_map_elt *elt,
				struct ring_buffer_event *rbe,
				void *event)
{
	struct hist_trigger_data *hist_data = hist_field->hist_data;
	struct trace_array *tr = hist_data->event_file->tr;

	u64 ts = ring_buffer_event_time_stamp(rbe);

	if (hist_data->attrs->ts_in_usecs && trace_clock_in_ns(tr))
		ts = ns2usecs(ts);

	return ts;
}

static struct hist_field *find_var_field(struct hist_trigger_data *hist_data,
					 const char *var_name)
{
	struct hist_field *hist_field, *found = NULL;
	int i;

	for_each_hist_field(i, hist_data) {
		hist_field = hist_data->fields[i];
		if (hist_field && hist_field->flags & HIST_FIELD_FL_VAR &&
		    strcmp(hist_field->var.name, var_name) == 0) {
			found = hist_field;
			break;
		}
	}

	return found;
}

static struct hist_field *find_var(struct hist_trigger_data *hist_data,
				   struct trace_event_file *file,
				   const char *var_name)
{
	struct hist_trigger_data *test_data;
	struct event_trigger_data *test;
	struct hist_field *hist_field;

	hist_field = find_var_field(hist_data, var_name);
	if (hist_field)
		return hist_field;

	list_for_each_entry_rcu(test, &file->triggers, list) {
		if (test->cmd_ops->trigger_type == ETT_EVENT_HIST) {
			test_data = test->private_data;
			hist_field = find_var_field(test_data, var_name);
			if (hist_field)
				return hist_field;
		}
	}

	return NULL;
}

struct hist_elt_data {
	char *comm;
};

static const char *hist_field_name(struct hist_field *field,
				   unsigned int level)
{
	const char *field_name = "";

	if (level > 1)
		return field_name;

	if (field->field)
		field_name = field->field->name;
	else if (field->flags & HIST_FIELD_FL_LOG2)
		field_name = hist_field_name(field->operands[0], ++level);
	else if (field->flags & HIST_FIELD_FL_TIMESTAMP)
		field_name = "common_timestamp";
	else if (field->flags & HIST_FIELD_FL_EXPR)
		field_name = field->name;

	if (field_name == NULL)
		field_name = "";

	return field_name;
}

static hist_field_fn_t select_value_fn(int field_size, int field_is_signed)
{
	hist_field_fn_t fn = NULL;

	switch (field_size) {
	case 8:
		if (field_is_signed)
			fn = hist_field_s64;
		else
			fn = hist_field_u64;
		break;
	case 4:
		if (field_is_signed)
			fn = hist_field_s32;
		else
			fn = hist_field_u32;
		break;
	case 2:
		if (field_is_signed)
			fn = hist_field_s16;
		else
			fn = hist_field_u16;
		break;
	case 1:
		if (field_is_signed)
			fn = hist_field_s8;
		else
			fn = hist_field_u8;
		break;
	}

	return fn;
}

static int parse_map_size(char *str)
{
	unsigned long size, map_bits;
	int ret;

	strsep(&str, "=");
	if (!str) {
		ret = -EINVAL;
		goto out;
	}

	ret = kstrtoul(str, 0, &size);
	if (ret)
		goto out;

	map_bits = ilog2(roundup_pow_of_two(size));
	if (map_bits < TRACING_MAP_BITS_MIN ||
	    map_bits > TRACING_MAP_BITS_MAX)
		ret = -EINVAL;
	else
		ret = map_bits;
 out:
	return ret;
}

static void destroy_hist_trigger_attrs(struct hist_trigger_attrs *attrs)
{
	unsigned int i;

	if (!attrs)
		return;

	for (i = 0; i < attrs->n_assignments; i++)
		kfree(attrs->assignment_str[i]);

	kfree(attrs->name);
	kfree(attrs->sort_key_str);
	kfree(attrs->keys_str);
	kfree(attrs->vals_str);
	kfree(attrs);
}

static int parse_assignment(char *str, struct hist_trigger_attrs *attrs)
{
	int ret = 0;

	if ((strncmp(str, "key=", strlen("key=")) == 0) ||
	    (strncmp(str, "keys=", strlen("keys=")) == 0)) {
		attrs->keys_str = kstrdup(str, GFP_KERNEL);
		if (!attrs->keys_str) {
			ret = -ENOMEM;
			goto out;
		}
	} else if ((strncmp(str, "val=", strlen("val=")) == 0) ||
		 (strncmp(str, "vals=", strlen("vals=")) == 0) ||
		 (strncmp(str, "values=", strlen("values=")) == 0)) {
		attrs->vals_str = kstrdup(str, GFP_KERNEL);
		if (!attrs->vals_str) {
			ret = -ENOMEM;
			goto out;
		}
	} else if (strncmp(str, "sort=", strlen("sort=")) == 0) {
		attrs->sort_key_str = kstrdup(str, GFP_KERNEL);
		if (!attrs->sort_key_str) {
			ret = -ENOMEM;
			goto out;
		}
	} else if (strncmp(str, "name=", strlen("name=")) == 0) {
		attrs->name = kstrdup(str, GFP_KERNEL);
		if (!attrs->name) {
			ret = -ENOMEM;
			goto out;
		}
	} else if (strncmp(str, "size=", strlen("size=")) == 0) {
		int map_bits = parse_map_size(str);

		if (map_bits < 0) {
			ret = map_bits;
			goto out;
		}
		attrs->map_bits = map_bits;
	} else {
		char *assignment;

		if (attrs->n_assignments == TRACING_MAP_VARS_MAX) {
			ret = -EINVAL;
			goto out;
		}

		assignment = kstrdup(str, GFP_KERNEL);
		if (!assignment) {
			ret = -ENOMEM;
			goto out;
		}

		attrs->assignment_str[attrs->n_assignments++] = assignment;
	}
 out:
	return ret;
}

static struct hist_trigger_attrs *parse_hist_trigger_attrs(char *trigger_str)
{
	struct hist_trigger_attrs *attrs;
	int ret = 0;

	attrs = kzalloc(sizeof(*attrs), GFP_KERNEL);
	if (!attrs)
		return ERR_PTR(-ENOMEM);

	while (trigger_str) {
		char *str = strsep(&trigger_str, ":");

		if (strchr(str, '=')) {
			ret = parse_assignment(str, attrs);
			if (ret)
				goto free;
		} else if (strcmp(str, "pause") == 0)
			attrs->pause = true;
		else if ((strcmp(str, "cont") == 0) ||
			 (strcmp(str, "continue") == 0))
			attrs->cont = true;
		else if (strcmp(str, "clear") == 0)
			attrs->clear = true;
		else {
			ret = -EINVAL;
			goto free;
		}
	}

	if (!attrs->keys_str) {
		ret = -EINVAL;
		goto free;
	}

	return attrs;
 free:
	destroy_hist_trigger_attrs(attrs);

	return ERR_PTR(ret);
}

static inline void save_comm(char *comm, struct task_struct *task)
{
	if (!task->pid) {
		strcpy(comm, "<idle>");
		return;
	}

	if (WARN_ON_ONCE(task->pid < 0)) {
		strcpy(comm, "<XXX>");
		return;
	}

	memcpy(comm, task->comm, TASK_COMM_LEN);
}

static void hist_elt_data_free(struct hist_elt_data *elt_data)
{
	kfree(elt_data->comm);
	kfree(elt_data);
}

static void hist_trigger_elt_data_free(struct tracing_map_elt *elt)
{
	struct hist_elt_data *elt_data = elt->private_data;

	hist_elt_data_free(elt_data);
}

static int hist_trigger_elt_data_alloc(struct tracing_map_elt *elt)
{
	struct hist_trigger_data *hist_data = elt->map->private_data;
	unsigned int size = TASK_COMM_LEN;
	struct hist_elt_data *elt_data;
	struct hist_field *key_field;
	unsigned int i;

	elt_data = kzalloc(sizeof(*elt_data), GFP_KERNEL);
	if (!elt_data)
		return -ENOMEM;

	for_each_hist_key_field(i, hist_data) {
		key_field = hist_data->fields[i];

		if (key_field->flags & HIST_FIELD_FL_EXECNAME) {
			elt_data->comm = kzalloc(size, GFP_KERNEL);
			if (!elt_data->comm) {
				kfree(elt_data);
				return -ENOMEM;
			}
			break;
		}
	}

	elt->private_data = elt_data;

	return 0;
}

static void hist_trigger_elt_data_init(struct tracing_map_elt *elt)
{
	struct hist_elt_data *elt_data = elt->private_data;

	if (elt_data->comm)
		save_comm(elt_data->comm, current);
}

static const struct tracing_map_ops hist_trigger_elt_data_ops = {
	.elt_alloc	= hist_trigger_elt_data_alloc,
	.elt_free	= hist_trigger_elt_data_free,
	.elt_init	= hist_trigger_elt_data_init,
};

static const char *get_hist_field_flags(struct hist_field *hist_field)
{
	const char *flags_str = NULL;

	if (hist_field->flags & HIST_FIELD_FL_HEX)
		flags_str = "hex";
	else if (hist_field->flags & HIST_FIELD_FL_SYM)
		flags_str = "sym";
	else if (hist_field->flags & HIST_FIELD_FL_SYM_OFFSET)
		flags_str = "sym-offset";
	else if (hist_field->flags & HIST_FIELD_FL_EXECNAME)
		flags_str = "execname";
	else if (hist_field->flags & HIST_FIELD_FL_SYSCALL)
		flags_str = "syscall";
	else if (hist_field->flags & HIST_FIELD_FL_LOG2)
		flags_str = "log2";
	else if (hist_field->flags & HIST_FIELD_FL_TIMESTAMP_USECS)
		flags_str = "usecs";

	return flags_str;
}

static void expr_field_str(struct hist_field *field, char *expr)
{
	strcat(expr, hist_field_name(field, 0));

	if (field->flags) {
		const char *flags_str = get_hist_field_flags(field);

		if (flags_str) {
			strcat(expr, ".");
			strcat(expr, flags_str);
		}
	}
}

static char *expr_str(struct hist_field *field, unsigned int level)
{
	char *expr;

	if (level > 1)
		return NULL;

	expr = kzalloc(MAX_FILTER_STR_VAL, GFP_KERNEL);
	if (!expr)
		return NULL;

	if (!field->operands[0]) {
		expr_field_str(field, expr);
		return expr;
	}

	if (field->operator == FIELD_OP_UNARY_MINUS) {
		char *subexpr;

		strcat(expr, "-(");
		subexpr = expr_str(field->operands[0], ++level);
		if (!subexpr) {
			kfree(expr);
			return NULL;
		}
		strcat(expr, subexpr);
		strcat(expr, ")");

		kfree(subexpr);

		return expr;
	}

	expr_field_str(field->operands[0], expr);

	switch (field->operator) {
	case FIELD_OP_MINUS:
		strcat(expr, "-");
		break;
	case FIELD_OP_PLUS:
		strcat(expr, "+");
		break;
	default:
		kfree(expr);
		return NULL;
	}

	expr_field_str(field->operands[1], expr);

	return expr;
}

static int contains_operator(char *str)
{
	enum field_op_id field_op = FIELD_OP_NONE;
	char *op;

	op = strpbrk(str, "+-");
	if (!op)
		return FIELD_OP_NONE;

	switch (*op) {
	case '-':
		if (*str == '-')
			field_op = FIELD_OP_UNARY_MINUS;
		else
			field_op = FIELD_OP_MINUS;
		break;
	case '+':
		field_op = FIELD_OP_PLUS;
		break;
	default:
		break;
	}

	return field_op;
}

static void destroy_hist_field(struct hist_field *hist_field,
			       unsigned int level)
{
	unsigned int i;

	if (level > 3)
		return;

	if (!hist_field)
		return;

	for (i = 0; i < HIST_FIELD_OPERANDS_MAX; i++)
		destroy_hist_field(hist_field->operands[i], level + 1);

	kfree(hist_field->var.name);
	kfree(hist_field->name);
	kfree(hist_field->type);

	kfree(hist_field);
}

static struct hist_field *create_hist_field(struct hist_trigger_data *hist_data,
					    struct ftrace_event_field *field,
					    unsigned long flags,
					    char *var_name)
{
	struct hist_field *hist_field;

	if (field && is_function_field(field))
		return NULL;

	hist_field = kzalloc(sizeof(struct hist_field), GFP_KERNEL);
	if (!hist_field)
		return NULL;

	hist_field->hist_data = hist_data;

	if (flags & HIST_FIELD_FL_EXPR)
		goto out; /* caller will populate */

	if (flags & HIST_FIELD_FL_HITCOUNT) {
		hist_field->fn = hist_field_counter;
		hist_field->size = sizeof(u64);
		hist_field->type = kstrdup("u64", GFP_KERNEL);
		if (!hist_field->type)
			goto free;
		goto out;
	}

	if (flags & HIST_FIELD_FL_STACKTRACE) {
		hist_field->fn = hist_field_none;
		goto out;
	}

	if (flags & HIST_FIELD_FL_LOG2) {
		unsigned long fl = flags & ~HIST_FIELD_FL_LOG2;
		hist_field->fn = hist_field_log2;
		hist_field->operands[0] = create_hist_field(hist_data, field, fl, NULL);
		hist_field->size = hist_field->operands[0]->size;
		hist_field->type = kstrdup(hist_field->operands[0]->type, GFP_KERNEL);
		if (!hist_field->type)
			goto free;
		goto out;
	}

	if (flags & HIST_FIELD_FL_TIMESTAMP) {
		hist_field->fn = hist_field_timestamp;
		hist_field->size = sizeof(u64);
		hist_field->type = kstrdup("u64", GFP_KERNEL);
		if (!hist_field->type)
			goto free;
		goto out;
	}

	if (WARN_ON_ONCE(!field))
		goto out;

	/* Pointers to strings are just pointers and dangerous to dereference */
	if (is_string_field(field) &&
	    (field->filter_type != FILTER_PTR_STRING)) {
		flags |= HIST_FIELD_FL_STRING;

		hist_field->size = MAX_FILTER_STR_VAL;
		hist_field->type = kstrdup(field->type, GFP_KERNEL);
		if (!hist_field->type)
			goto free;

		if (field->filter_type == FILTER_STATIC_STRING)
			hist_field->fn = hist_field_string;
		else if (field->filter_type == FILTER_DYN_STRING)
			hist_field->fn = hist_field_dynstring;
		else
			hist_field->fn = hist_field_pstring;
	} else {
		hist_field->size = field->size;
		hist_field->is_signed = field->is_signed;
		hist_field->type = kstrdup(field->type, GFP_KERNEL);
		if (!hist_field->type)
			goto free;

		hist_field->fn = select_value_fn(field->size,
						 field->is_signed);
		if (!hist_field->fn) {
			destroy_hist_field(hist_field, 0);
			return NULL;
		}
	}
 out:
	hist_field->field = field;
	hist_field->flags = flags;

	if (var_name) {
		hist_field->var.name = kstrdup(var_name, GFP_KERNEL);
		if (!hist_field->var.name)
			goto free;
	}

	return hist_field;
 free:
	destroy_hist_field(hist_field, 0);
	return NULL;
}

static void destroy_hist_fields(struct hist_trigger_data *hist_data)
{
	unsigned int i;

	for (i = 0; i < HIST_FIELDS_MAX; i++) {
		if (hist_data->fields[i]) {
			destroy_hist_field(hist_data->fields[i], 0);
			hist_data->fields[i] = NULL;
		}
	}
}

static struct ftrace_event_field *
parse_field(struct hist_trigger_data *hist_data, struct trace_event_file *file,
	    char *field_str, unsigned long *flags)
{
	struct ftrace_event_field *field = NULL;
	char *field_name, *modifier, *str;

	modifier = str = kstrdup(field_str, GFP_KERNEL);
	if (!modifier)
		return ERR_PTR(-ENOMEM);

	field_name = strsep(&modifier, ".");
	if (modifier) {
		if (strcmp(modifier, "hex") == 0)
			*flags |= HIST_FIELD_FL_HEX;
		else if (strcmp(modifier, "sym") == 0)
			*flags |= HIST_FIELD_FL_SYM;
		else if (strcmp(modifier, "sym-offset") == 0)
			*flags |= HIST_FIELD_FL_SYM_OFFSET;
		else if ((strcmp(modifier, "execname") == 0) &&
			 (strcmp(field_name, "common_pid") == 0))
			*flags |= HIST_FIELD_FL_EXECNAME;
		else if (strcmp(modifier, "syscall") == 0)
			*flags |= HIST_FIELD_FL_SYSCALL;
		else if (strcmp(modifier, "log2") == 0)
			*flags |= HIST_FIELD_FL_LOG2;
		else if (strcmp(modifier, "usecs") == 0)
			*flags |= HIST_FIELD_FL_TIMESTAMP_USECS;
		else {
			field = ERR_PTR(-EINVAL);
			goto out;
		}
	}

	if (strcmp(field_name, "common_timestamp") == 0) {
		*flags |= HIST_FIELD_FL_TIMESTAMP;
		hist_data->enable_timestamps = true;
		if (*flags & HIST_FIELD_FL_TIMESTAMP_USECS)
			hist_data->attrs->ts_in_usecs = true;
	} else {
		field = trace_find_event_field(file->event_call, field_name);
		if (!field || !field->size) {
			field = ERR_PTR(-EINVAL);
			goto out;
		}
	}
 out:
	kfree(str);

	return field;
}

static struct hist_field *parse_atom(struct hist_trigger_data *hist_data,
				     struct trace_event_file *file, char *str,
				     unsigned long *flags, char *var_name)
{
	struct ftrace_event_field *field = NULL;
	struct hist_field *hist_field = NULL;
	int ret = 0;

	field = parse_field(hist_data, file, str, flags);
	if (IS_ERR(field)) {
		ret = PTR_ERR(field);
		goto out;
	}

	hist_field = create_hist_field(hist_data, field, *flags, var_name);
	if (!hist_field) {
		ret = -ENOMEM;
		goto out;
	}

	return hist_field;
 out:
	return ERR_PTR(ret);
}

static struct hist_field *parse_expr(struct hist_trigger_data *hist_data,
				     struct trace_event_file *file,
				     char *str, unsigned long flags,
				     char *var_name, unsigned int level);

static struct hist_field *parse_unary(struct hist_trigger_data *hist_data,
				      struct trace_event_file *file,
				      char *str, unsigned long flags,
				      char *var_name, unsigned int level)
{
	struct hist_field *operand1, *expr = NULL;
	unsigned long operand_flags;
	int ret = 0;
	char *s;

	// we support only -(xxx) i.e. explicit parens required

	if (level > 3) {
		ret = -EINVAL;
		goto free;
	}

	str++; // skip leading '-'

	s = strchr(str, '(');
	if (s)
		str++;
	else {
		ret = -EINVAL;
		goto free;
	}

	s = strrchr(str, ')');
	if (s)
		*s = '\0';
	else {
		ret = -EINVAL; // no closing ')'
		goto free;
	}

	flags |= HIST_FIELD_FL_EXPR;
	expr = create_hist_field(hist_data, NULL, flags, var_name);
	if (!expr) {
		ret = -ENOMEM;
		goto free;
	}

	operand_flags = 0;
	operand1 = parse_expr(hist_data, file, str, operand_flags, NULL, ++level);
	if (IS_ERR(operand1)) {
		ret = PTR_ERR(operand1);
		goto free;
	}

	expr->flags |= operand1->flags &
		(HIST_FIELD_FL_TIMESTAMP | HIST_FIELD_FL_TIMESTAMP_USECS);
	expr->fn = hist_field_unary_minus;
	expr->operands[0] = operand1;
	expr->operator = FIELD_OP_UNARY_MINUS;
	expr->name = expr_str(expr, 0);
	expr->type = kstrdup(operand1->type, GFP_KERNEL);
	if (!expr->type) {
		ret = -ENOMEM;
		goto free;
	}

	return expr;
 free:
	destroy_hist_field(expr, 0);
	return ERR_PTR(ret);
}

static int check_expr_operands(struct hist_field *operand1,
			       struct hist_field *operand2)
{
	unsigned long operand1_flags = operand1->flags;
	unsigned long operand2_flags = operand2->flags;

	if ((operand1_flags & HIST_FIELD_FL_TIMESTAMP_USECS) !=
	    (operand2_flags & HIST_FIELD_FL_TIMESTAMP_USECS))
		return -EINVAL;

	return 0;
}

static struct hist_field *parse_expr(struct hist_trigger_data *hist_data,
				     struct trace_event_file *file,
				     char *str, unsigned long flags,
				     char *var_name, unsigned int level)
{
	struct hist_field *operand1 = NULL, *operand2 = NULL, *expr = NULL;
	unsigned long operand_flags;
	int field_op, ret = -EINVAL;
	char *sep, *operand1_str;

	if (level > 3)
		return ERR_PTR(-EINVAL);

	field_op = contains_operator(str);

	if (field_op == FIELD_OP_NONE)
		return parse_atom(hist_data, file, str, &flags, var_name);

	if (field_op == FIELD_OP_UNARY_MINUS)
		return parse_unary(hist_data, file, str, flags, var_name, ++level);

	switch (field_op) {
	case FIELD_OP_MINUS:
		sep = "-";
		break;
	case FIELD_OP_PLUS:
		sep = "+";
		break;
	default:
		goto free;
	}

	operand1_str = strsep(&str, sep);
	if (!operand1_str || !str)
		goto free;

	operand_flags = 0;
	operand1 = parse_atom(hist_data, file, operand1_str,
			      &operand_flags, NULL);
	if (IS_ERR(operand1)) {
		ret = PTR_ERR(operand1);
		operand1 = NULL;
		goto free;
	}

	// rest of string could be another expression e.g. b+c in a+b+c
	operand_flags = 0;
	operand2 = parse_expr(hist_data, file, str, operand_flags, NULL, ++level);
	if (IS_ERR(operand2)) {
		ret = PTR_ERR(operand2);
		operand2 = NULL;
		goto free;
	}

	ret = check_expr_operands(operand1, operand2);
	if (ret)
		goto free;

	flags |= HIST_FIELD_FL_EXPR;

	flags |= operand1->flags &
		(HIST_FIELD_FL_TIMESTAMP | HIST_FIELD_FL_TIMESTAMP_USECS);

	expr = create_hist_field(hist_data, NULL, flags, var_name);
	if (!expr) {
		ret = -ENOMEM;
		goto free;
	}

	expr->operands[0] = operand1;
	expr->operands[1] = operand2;
	expr->operator = field_op;
	expr->name = expr_str(expr, 0);
	expr->type = kstrdup(operand1->type, GFP_KERNEL);
	if (!expr->type) {
		ret = -ENOMEM;
		goto free;
	}

	switch (field_op) {
	case FIELD_OP_MINUS:
		expr->fn = hist_field_minus;
		break;
	case FIELD_OP_PLUS:
		expr->fn = hist_field_plus;
		break;
	default:
		goto free;
	}

	return expr;
 free:
	destroy_hist_field(operand1, 0);
	destroy_hist_field(operand2, 0);
	destroy_hist_field(expr, 0);

	return ERR_PTR(ret);
}

static int create_hitcount_val(struct hist_trigger_data *hist_data)
{
	hist_data->fields[HITCOUNT_IDX] =
		create_hist_field(hist_data, NULL, HIST_FIELD_FL_HITCOUNT, NULL);
	if (!hist_data->fields[HITCOUNT_IDX])
		return -ENOMEM;

	hist_data->n_vals++;
	hist_data->n_fields++;

	if (WARN_ON(hist_data->n_vals > TRACING_MAP_VALS_MAX))
		return -EINVAL;

	return 0;
}

static int __create_val_field(struct hist_trigger_data *hist_data,
			      unsigned int val_idx,
			      struct trace_event_file *file,
			      char *var_name, char *field_str,
			      unsigned long flags)
{
	struct hist_field *hist_field;
	int ret = 0;

	hist_field = parse_expr(hist_data, file, field_str, flags, var_name, 0);
	if (IS_ERR(hist_field)) {
		ret = PTR_ERR(hist_field);
		goto out;
	}

	hist_data->fields[val_idx] = hist_field;

	++hist_data->n_vals;
	++hist_data->n_fields;

	if (WARN_ON(hist_data->n_vals > TRACING_MAP_VALS_MAX + TRACING_MAP_VARS_MAX))
		ret = -EINVAL;
 out:
	return ret;
}

static int create_val_field(struct hist_trigger_data *hist_data,
			    unsigned int val_idx,
			    struct trace_event_file *file,
			    char *field_str)
{
	if (WARN_ON(val_idx >= TRACING_MAP_VALS_MAX))
		return -EINVAL;

	return __create_val_field(hist_data, val_idx, file, NULL, field_str, 0);
}

static int create_var_field(struct hist_trigger_data *hist_data,
			    unsigned int val_idx,
			    struct trace_event_file *file,
			    char *var_name, char *expr_str)
{
	unsigned long flags = 0;

	if (WARN_ON(val_idx >= TRACING_MAP_VALS_MAX + TRACING_MAP_VARS_MAX))
		return -EINVAL;
	if (find_var(hist_data, file, var_name) && !hist_data->remove) {
		return -EINVAL;
	}

	flags |= HIST_FIELD_FL_VAR;
	hist_data->n_vars++;
	if (WARN_ON(hist_data->n_vars > TRACING_MAP_VARS_MAX))
		return -EINVAL;

	return __create_val_field(hist_data, val_idx, file, var_name, expr_str, flags);
}

static int create_val_fields(struct hist_trigger_data *hist_data,
			     struct trace_event_file *file)
{
	char *fields_str, *field_str;
	unsigned int i, j = 1;
	int ret;

	ret = create_hitcount_val(hist_data);
	if (ret)
		goto out;

	fields_str = hist_data->attrs->vals_str;
	if (!fields_str)
		goto out;

	strsep(&fields_str, "=");
	if (!fields_str)
		goto out;

	for (i = 0, j = 1; i < TRACING_MAP_VALS_MAX &&
		     j < TRACING_MAP_VALS_MAX; i++) {
		field_str = strsep(&fields_str, ",");
		if (!field_str)
			break;

		if (strcmp(field_str, "hitcount") == 0)
			continue;

		ret = create_val_field(hist_data, j++, file, field_str);
		if (ret)
			goto out;
	}

	if (fields_str && (strcmp(fields_str, "hitcount") != 0))
		ret = -EINVAL;
 out:
	return ret;
}

static int create_key_field(struct hist_trigger_data *hist_data,
			    unsigned int key_idx,
			    unsigned int key_offset,
			    struct trace_event_file *file,
			    char *field_str)
{
	struct hist_field *hist_field = NULL;

	unsigned long flags = 0;
	unsigned int key_size;
	int ret = 0;

	if (WARN_ON(key_idx >= HIST_FIELDS_MAX))
		return -EINVAL;

	flags |= HIST_FIELD_FL_KEY;

	if (strcmp(field_str, "stacktrace") == 0) {
		flags |= HIST_FIELD_FL_STACKTRACE;
		key_size = sizeof(unsigned long) * HIST_STACKTRACE_DEPTH;
		hist_field = create_hist_field(hist_data, NULL, flags, NULL);
	} else {
		hist_field = parse_expr(hist_data, file, field_str, flags,
					NULL, 0);
		if (IS_ERR(hist_field)) {
			ret = PTR_ERR(hist_field);
			goto out;
		}

		key_size = hist_field->size;
	}

	hist_data->fields[key_idx] = hist_field;

	key_size = ALIGN(key_size, sizeof(u64));
	hist_data->fields[key_idx]->size = key_size;
	hist_data->fields[key_idx]->offset = key_offset;

	hist_data->key_size += key_size;

	if (hist_data->key_size > HIST_KEY_SIZE_MAX) {
		ret = -EINVAL;
		goto out;
	}

	hist_data->n_keys++;
	hist_data->n_fields++;

	if (WARN_ON(hist_data->n_keys > TRACING_MAP_KEYS_MAX))
		return -EINVAL;

	ret = key_size;
 out:
	return ret;
}

static int create_key_fields(struct hist_trigger_data *hist_data,
			     struct trace_event_file *file)
{
	unsigned int i, key_offset = 0, n_vals = hist_data->n_vals;
	char *fields_str, *field_str;
	int ret = -EINVAL;

	fields_str = hist_data->attrs->keys_str;
	if (!fields_str)
		goto out;

	strsep(&fields_str, "=");
	if (!fields_str)
		goto out;

	for (i = n_vals; i < n_vals + TRACING_MAP_KEYS_MAX; i++) {
		field_str = strsep(&fields_str, ",");
		if (!field_str)
			break;
		ret = create_key_field(hist_data, i, key_offset,
				       file, field_str);
		if (ret < 0)
			goto out;
		key_offset += ret;
	}
	if (fields_str) {
		ret = -EINVAL;
		goto out;
	}
	ret = 0;
 out:
	return ret;
}

static int create_var_fields(struct hist_trigger_data *hist_data,
			     struct trace_event_file *file)
{
	unsigned int i, j = hist_data->n_vals;
	int ret = 0;

	unsigned int n_vars = hist_data->attrs->var_defs.n_vars;

	for (i = 0; i < n_vars; i++) {
		char *var_name = hist_data->attrs->var_defs.name[i];
		char *expr = hist_data->attrs->var_defs.expr[i];

		ret = create_var_field(hist_data, j++, file, var_name, expr);
		if (ret)
			goto out;
	}
 out:
	return ret;
}

static void free_var_defs(struct hist_trigger_data *hist_data)
{
	unsigned int i;

	for (i = 0; i < hist_data->attrs->var_defs.n_vars; i++) {
		kfree(hist_data->attrs->var_defs.name[i]);
		kfree(hist_data->attrs->var_defs.expr[i]);
	}

	hist_data->attrs->var_defs.n_vars = 0;
}

static int parse_var_defs(struct hist_trigger_data *hist_data)
{
	char *s, *str, *var_name, *field_str;
	unsigned int i, j, n_vars = 0;
	int ret = 0;

	for (i = 0; i < hist_data->attrs->n_assignments; i++) {
		str = hist_data->attrs->assignment_str[i];
		for (j = 0; j < TRACING_MAP_VARS_MAX; j++) {
			field_str = strsep(&str, ",");
			if (!field_str)
				break;

			var_name = strsep(&field_str, "=");
			if (!var_name || !field_str) {
				ret = -EINVAL;
				goto free;
			}

			if (n_vars == TRACING_MAP_VARS_MAX) {
				ret = -EINVAL;
				goto free;
			}

			s = kstrdup(var_name, GFP_KERNEL);
			if (!s) {
				ret = -ENOMEM;
				goto free;
			}
			hist_data->attrs->var_defs.name[n_vars] = s;

			s = kstrdup(field_str, GFP_KERNEL);
			if (!s) {
				kfree(hist_data->attrs->var_defs.name[n_vars]);
				ret = -ENOMEM;
				goto free;
			}
			hist_data->attrs->var_defs.expr[n_vars++] = s;

			hist_data->attrs->var_defs.n_vars = n_vars;
		}
	}

	return ret;
 free:
	free_var_defs(hist_data);

	return ret;
}

static int create_hist_fields(struct hist_trigger_data *hist_data,
			      struct trace_event_file *file)
{
	int ret;

	ret = parse_var_defs(hist_data);
	if (ret)
		goto out;

	ret = create_val_fields(hist_data, file);
	if (ret)
		goto out;

	ret = create_var_fields(hist_data, file);
	if (ret)
		goto out;

	ret = create_key_fields(hist_data, file);
	if (ret)
		goto out;
 out:
	free_var_defs(hist_data);

	return ret;
}

static int is_descending(const char *str)
{
	if (!str)
		return 0;

	if (strcmp(str, "descending") == 0)
		return 1;

	if (strcmp(str, "ascending") == 0)
		return 0;

	return -EINVAL;
}

static int create_sort_keys(struct hist_trigger_data *hist_data)
{
	char *fields_str = hist_data->attrs->sort_key_str;
	struct tracing_map_sort_key *sort_key;
	int descending, ret = 0;
	unsigned int i, j, k;

	hist_data->n_sort_keys = 1; /* we always have at least one, hitcount */

	if (!fields_str)
		goto out;

	strsep(&fields_str, "=");
	if (!fields_str) {
		ret = -EINVAL;
		goto out;
	}

	for (i = 0; i < TRACING_MAP_SORT_KEYS_MAX; i++) {
		struct hist_field *hist_field;
		char *field_str, *field_name;
		const char *test_name;

		sort_key = &hist_data->sort_keys[i];

		field_str = strsep(&fields_str, ",");
		if (!field_str) {
			if (i == 0)
				ret = -EINVAL;
			break;
		}

		if ((i == TRACING_MAP_SORT_KEYS_MAX - 1) && fields_str) {
			ret = -EINVAL;
			break;
		}

		field_name = strsep(&field_str, ".");
		if (!field_name) {
			ret = -EINVAL;
			break;
		}

		if (strcmp(field_name, "hitcount") == 0) {
			descending = is_descending(field_str);
			if (descending < 0) {
				ret = descending;
				break;
			}
			sort_key->descending = descending;
			continue;
		}

		for (j = 1, k = 1; j < hist_data->n_fields; j++) {
			unsigned int idx;

			hist_field = hist_data->fields[j];
			if (hist_field->flags & HIST_FIELD_FL_VAR)
				continue;

			idx = k++;

			test_name = hist_field_name(hist_field, 0);

			if (strcmp(field_name, test_name) == 0) {
				sort_key->field_idx = idx;
				descending = is_descending(field_str);
				if (descending < 0) {
					ret = descending;
					goto out;
				}
				sort_key->descending = descending;
				break;
			}
		}
		if (j == hist_data->n_fields) {
			ret = -EINVAL;
			break;
		}
	}

	hist_data->n_sort_keys = i;
 out:
	return ret;
}

static void destroy_hist_data(struct hist_trigger_data *hist_data)
{
	destroy_hist_trigger_attrs(hist_data->attrs);
	destroy_hist_fields(hist_data);
	tracing_map_destroy(hist_data->map);
	kfree(hist_data);
}

static int create_tracing_map_fields(struct hist_trigger_data *hist_data)
{
	struct tracing_map *map = hist_data->map;
	struct ftrace_event_field *field;
	struct hist_field *hist_field;
	int i, idx;

	for_each_hist_field(i, hist_data) {
		hist_field = hist_data->fields[i];
		if (hist_field->flags & HIST_FIELD_FL_KEY) {
			tracing_map_cmp_fn_t cmp_fn;

			field = hist_field->field;

			if (hist_field->flags & HIST_FIELD_FL_STACKTRACE)
				cmp_fn = tracing_map_cmp_none;
			else if (!field)
				cmp_fn = tracing_map_cmp_num(hist_field->size,
							     hist_field->is_signed);
			else if (is_string_field(field))
				cmp_fn = tracing_map_cmp_string;
			else
				cmp_fn = tracing_map_cmp_num(field->size,
							     field->is_signed);
			idx = tracing_map_add_key_field(map,
							hist_field->offset,
							cmp_fn);
		} else if (!(hist_field->flags & HIST_FIELD_FL_VAR))
			idx = tracing_map_add_sum_field(map);

		if (idx < 0)
			return idx;

		if (hist_field->flags & HIST_FIELD_FL_VAR) {
			idx = tracing_map_add_var(map);
			if (idx < 0)
				return idx;
			hist_field->var.idx = idx;
			hist_field->var.hist_data = hist_data;
		}
	}

	return 0;
}

static struct hist_trigger_data *
create_hist_data(unsigned int map_bits,
		 struct hist_trigger_attrs *attrs,
		 struct trace_event_file *file,
		 bool remove)
{
	const struct tracing_map_ops *map_ops = NULL;
	struct hist_trigger_data *hist_data;
	int ret = 0;

	hist_data = kzalloc(sizeof(*hist_data), GFP_KERNEL);
	if (!hist_data)
		return ERR_PTR(-ENOMEM);

	hist_data->attrs = attrs;
	hist_data->remove = remove;

	ret = create_hist_fields(hist_data, file);
	if (ret)
		goto free;

	ret = create_sort_keys(hist_data);
	if (ret)
		goto free;

	map_ops = &hist_trigger_elt_data_ops;

	hist_data->map = tracing_map_create(map_bits, hist_data->key_size,
					    map_ops, hist_data);
	if (IS_ERR(hist_data->map)) {
		ret = PTR_ERR(hist_data->map);
		hist_data->map = NULL;
		goto free;
	}

	ret = create_tracing_map_fields(hist_data);
	if (ret)
		goto free;

	ret = tracing_map_init(hist_data->map);
	if (ret)
		goto free;

	hist_data->event_file = file;
 out:
	return hist_data;
 free:
	hist_data->attrs = NULL;

	destroy_hist_data(hist_data);

	hist_data = ERR_PTR(ret);

	goto out;
}

static void hist_trigger_elt_update(struct hist_trigger_data *hist_data,
				    struct tracing_map_elt *elt, void *rec,
				    struct ring_buffer_event *rbe)
{
	struct hist_field *hist_field;
	unsigned int i, var_idx;
	u64 hist_val;

	for_each_hist_val_field(i, hist_data) {
		hist_field = hist_data->fields[i];
		hist_val = hist_field->fn(hist_field, elt, rbe, rec);
		if (hist_field->flags & HIST_FIELD_FL_VAR) {
			var_idx = hist_field->var.idx;
			tracing_map_set_var(elt, var_idx, hist_val);
			continue;
		}
		tracing_map_update_sum(elt, i, hist_val);
	}

	for_each_hist_key_field(i, hist_data) {
		hist_field = hist_data->fields[i];
		if (hist_field->flags & HIST_FIELD_FL_VAR) {
			hist_val = hist_field->fn(hist_field, elt, rbe, rec);
			var_idx = hist_field->var.idx;
			tracing_map_set_var(elt, var_idx, hist_val);
		}
	}
}

static inline void add_to_key(char *compound_key, void *key,
			      struct hist_field *key_field, void *rec)
{
	size_t size = key_field->size;

	if (key_field->flags & HIST_FIELD_FL_STRING) {
		struct ftrace_event_field *field;

		field = key_field->field;
		if (field->filter_type == FILTER_DYN_STRING)
			size = *(u32 *)(rec + field->offset) >> 16;
		else if (field->filter_type == FILTER_STATIC_STRING)
			size = field->size;

		/* ensure NULL-termination */
		if (size > key_field->size - 1)
			size = key_field->size - 1;

		strncpy(compound_key + key_field->offset, (char *)key, size);
	} else
		memcpy(compound_key + key_field->offset, key, size);
}

static void event_hist_trigger(struct event_trigger_data *data, void *rec,
			       struct ring_buffer_event *rbe)
{
	struct hist_trigger_data *hist_data = data->private_data;
	bool use_compound_key = (hist_data->n_keys > 1);
	unsigned long entries[HIST_STACKTRACE_DEPTH];
	char compound_key[HIST_KEY_SIZE_MAX];
	struct tracing_map_elt *elt = NULL;
	struct stack_trace stacktrace;
	struct hist_field *key_field;
	u64 field_contents;
	void *key = NULL;
	unsigned int i;

	memset(compound_key, 0, hist_data->key_size);

	for_each_hist_key_field(i, hist_data) {
		key_field = hist_data->fields[i];

		if (key_field->flags & HIST_FIELD_FL_STACKTRACE) {
			stacktrace.max_entries = HIST_STACKTRACE_DEPTH;
			stacktrace.entries = entries;
			stacktrace.nr_entries = 0;
			stacktrace.skip = HIST_STACKTRACE_SKIP;

			memset(stacktrace.entries, 0, HIST_STACKTRACE_SIZE);
			save_stack_trace(&stacktrace);

			key = entries;
		} else {
			field_contents = key_field->fn(key_field, elt, rbe, rec);
			if (key_field->flags & HIST_FIELD_FL_STRING) {
				key = (void *)(unsigned long)field_contents;
				use_compound_key = true;
			} else
				key = (void *)&field_contents;
		}

		if (use_compound_key)
			add_to_key(compound_key, key, key_field, rec);
	}

	if (use_compound_key)
		key = compound_key;

	elt = tracing_map_insert(hist_data->map, key);
	if (elt)
		hist_trigger_elt_update(hist_data, elt, rec, rbe);
}

static void hist_trigger_stacktrace_print(struct seq_file *m,
					  unsigned long *stacktrace_entries,
					  unsigned int max_entries)
{
	char str[KSYM_SYMBOL_LEN];
	unsigned int spaces = 8;
	unsigned int i;

	for (i = 0; i < max_entries; i++) {
		if (stacktrace_entries[i] == ULONG_MAX)
			return;

		seq_printf(m, "%*c", 1 + spaces, ' ');
		sprint_symbol(str, stacktrace_entries[i]);
		seq_printf(m, "%s\n", str);
	}
}

static void
hist_trigger_entry_print(struct seq_file *m,
			 struct hist_trigger_data *hist_data, void *key,
			 struct tracing_map_elt *elt)
{
	struct hist_field *key_field;
	char str[KSYM_SYMBOL_LEN];
	bool multiline = false;
	const char *field_name;
	unsigned int i;
	u64 uval;

	seq_puts(m, "{ ");

	for_each_hist_key_field(i, hist_data) {
		key_field = hist_data->fields[i];

		if (i > hist_data->n_vals)
			seq_puts(m, ", ");

		field_name = hist_field_name(key_field, 0);

		if (key_field->flags & HIST_FIELD_FL_HEX) {
			uval = *(u64 *)(key + key_field->offset);
			seq_printf(m, "%s: %llx", field_name, uval);
		} else if (key_field->flags & HIST_FIELD_FL_SYM) {
			uval = *(u64 *)(key + key_field->offset);
			sprint_symbol_no_offset(str, uval);
			seq_printf(m, "%s: [%llx] %-45s", field_name,
				   uval, str);
		} else if (key_field->flags & HIST_FIELD_FL_SYM_OFFSET) {
			uval = *(u64 *)(key + key_field->offset);
			sprint_symbol(str, uval);
			seq_printf(m, "%s: [%llx] %-55s", field_name,
				   uval, str);
		} else if (key_field->flags & HIST_FIELD_FL_EXECNAME) {
			struct hist_elt_data *elt_data = elt->private_data;
			char *comm;

			if (WARN_ON_ONCE(!elt_data))
				return;

			comm = elt_data->comm;

			uval = *(u64 *)(key + key_field->offset);
			seq_printf(m, "%s: %-16s[%10llu]", field_name,
				   comm, uval);
		} else if (key_field->flags & HIST_FIELD_FL_SYSCALL) {
			const char *syscall_name;

			uval = *(u64 *)(key + key_field->offset);
			syscall_name = get_syscall_name(uval);
			if (!syscall_name)
				syscall_name = "unknown_syscall";

			seq_printf(m, "%s: %-30s[%3llu]", field_name,
				   syscall_name, uval);
		} else if (key_field->flags & HIST_FIELD_FL_STACKTRACE) {
			seq_puts(m, "stacktrace:\n");
			hist_trigger_stacktrace_print(m,
						      key + key_field->offset,
						      HIST_STACKTRACE_DEPTH);
			multiline = true;
		} else if (key_field->flags & HIST_FIELD_FL_LOG2) {
			seq_printf(m, "%s: ~ 2^%-2llu", field_name,
				   *(u64 *)(key + key_field->offset));
		} else if (key_field->flags & HIST_FIELD_FL_STRING) {
			seq_printf(m, "%s: %-50s", field_name,
				   (char *)(key + key_field->offset));
		} else {
			uval = *(u64 *)(key + key_field->offset);
			seq_printf(m, "%s: %10llu", field_name, uval);
		}
	}

	if (!multiline)
		seq_puts(m, " ");

	seq_puts(m, "}");

	seq_printf(m, " hitcount: %10llu",
		   tracing_map_read_sum(elt, HITCOUNT_IDX));

	for (i = 1; i < hist_data->n_vals; i++) {
		field_name = hist_field_name(hist_data->fields[i], 0);

		if (hist_data->fields[i]->flags & HIST_FIELD_FL_VAR ||
		    hist_data->fields[i]->flags & HIST_FIELD_FL_EXPR)
			continue;

		if (hist_data->fields[i]->flags & HIST_FIELD_FL_HEX) {
			seq_printf(m, "  %s: %10llx", field_name,
				   tracing_map_read_sum(elt, i));
		} else {
			seq_printf(m, "  %s: %10llu", field_name,
				   tracing_map_read_sum(elt, i));
		}
	}

	seq_puts(m, "\n");
}

static int print_entries(struct seq_file *m,
			 struct hist_trigger_data *hist_data)
{
	struct tracing_map_sort_entry **sort_entries = NULL;
	struct tracing_map *map = hist_data->map;
	int i, n_entries;

	n_entries = tracing_map_sort_entries(map, hist_data->sort_keys,
					     hist_data->n_sort_keys,
					     &sort_entries);
	if (n_entries < 0)
		return n_entries;

	for (i = 0; i < n_entries; i++)
		hist_trigger_entry_print(m, hist_data,
					 sort_entries[i]->key,
					 sort_entries[i]->elt);

	tracing_map_destroy_sort_entries(sort_entries, n_entries);

	return n_entries;
}

static void hist_trigger_show(struct seq_file *m,
			      struct event_trigger_data *data, int n)
{
	struct hist_trigger_data *hist_data;
	int n_entries, ret = 0;

	if (n > 0)
		seq_puts(m, "\n\n");

	seq_puts(m, "# event histogram\n#\n# trigger info: ");
	data->ops->print(m, data->ops, data);
	seq_puts(m, "#\n\n");

	hist_data = data->private_data;
	n_entries = print_entries(m, hist_data);
	if (n_entries < 0) {
		ret = n_entries;
		n_entries = 0;
	}

	seq_printf(m, "\nTotals:\n    Hits: %llu\n    Entries: %u\n    Dropped: %llu\n",
		   (u64)atomic64_read(&hist_data->map->hits),
		   n_entries, (u64)atomic64_read(&hist_data->map->drops));
}

static int hist_show(struct seq_file *m, void *v)
{
	struct event_trigger_data *data;
	struct trace_event_file *event_file;
	int n = 0, ret = 0;

	mutex_lock(&event_mutex);

	event_file = event_file_data(m->private);
	if (unlikely(!event_file)) {
		ret = -ENODEV;
		goto out_unlock;
	}

	list_for_each_entry_rcu(data, &event_file->triggers, list) {
		if (data->cmd_ops->trigger_type == ETT_EVENT_HIST)
			hist_trigger_show(m, data, n++);
	}

 out_unlock:
	mutex_unlock(&event_mutex);

	return ret;
}

static int event_hist_open(struct inode *inode, struct file *file)
{
	return single_open(file, hist_show, file);
}

const struct file_operations event_hist_fops = {
	.open = event_hist_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static void hist_field_print(struct seq_file *m, struct hist_field *hist_field)
{
	const char *field_name = hist_field_name(hist_field, 0);

	if (hist_field->var.name)
		seq_printf(m, "%s=", hist_field->var.name);

	if (hist_field->flags & HIST_FIELD_FL_TIMESTAMP)
		seq_puts(m, "common_timestamp");
	else if (field_name)
		seq_printf(m, "%s", field_name);

	if (hist_field->flags) {
		const char *flags_str = get_hist_field_flags(hist_field);

		if (flags_str)
			seq_printf(m, ".%s", flags_str);
	}
}

static int event_hist_trigger_print(struct seq_file *m,
				    struct event_trigger_ops *ops,
				    struct event_trigger_data *data)
{
	struct hist_trigger_data *hist_data = data->private_data;
	struct hist_field *field;
	bool have_var = false;
	unsigned int i;

	seq_puts(m, "hist:");

	if (data->name)
		seq_printf(m, "%s:", data->name);

	seq_puts(m, "keys=");

	for_each_hist_key_field(i, hist_data) {
		field = hist_data->fields[i];

		if (i > hist_data->n_vals)
			seq_puts(m, ",");

		if (field->flags & HIST_FIELD_FL_STACKTRACE)
			seq_puts(m, "stacktrace");
		else
			hist_field_print(m, field);
	}

	seq_puts(m, ":vals=");

	for_each_hist_val_field(i, hist_data) {
		field = hist_data->fields[i];
		if (field->flags & HIST_FIELD_FL_VAR) {
			have_var = true;
			continue;
		}

		if (i == HITCOUNT_IDX)
			seq_puts(m, "hitcount");
		else {
			seq_puts(m, ",");
			hist_field_print(m, field);
		}
	}

	if (have_var) {
		unsigned int n = 0;

		seq_puts(m, ":");

		for_each_hist_val_field(i, hist_data) {
			field = hist_data->fields[i];

			if (field->flags & HIST_FIELD_FL_VAR) {
				if (n++)
					seq_puts(m, ",");
				hist_field_print(m, field);
			}
		}
	}

	seq_puts(m, ":sort=");

	for (i = 0; i < hist_data->n_sort_keys; i++) {
		struct tracing_map_sort_key *sort_key;
		unsigned int idx, first_key_idx;

		/* skip VAR vals */
		first_key_idx = hist_data->n_vals - hist_data->n_vars;

		sort_key = &hist_data->sort_keys[i];
		idx = sort_key->field_idx;

		if (WARN_ON(idx >= HIST_FIELDS_MAX))
			return -EINVAL;

		if (i > 0)
			seq_puts(m, ",");

		if (idx == HITCOUNT_IDX)
			seq_puts(m, "hitcount");
		else {
			if (idx >= first_key_idx)
				idx += hist_data->n_vars;
			hist_field_print(m, hist_data->fields[idx]);
		}

		if (sort_key->descending)
			seq_puts(m, ".descending");
	}
	seq_printf(m, ":size=%u", (1 << hist_data->map->map_bits));

	if (data->filter_str)
		seq_printf(m, " if %s", data->filter_str);

	if (data->paused)
		seq_puts(m, " [paused]");
	else
		seq_puts(m, " [active]");

	seq_putc(m, '\n');

	return 0;
}

static int event_hist_trigger_init(struct event_trigger_ops *ops,
				   struct event_trigger_data *data)
{
	struct hist_trigger_data *hist_data = data->private_data;

	if (!data->ref && hist_data->attrs->name)
		save_named_trigger(hist_data->attrs->name, data);

	data->ref++;

	return 0;
}

static void event_hist_trigger_free(struct event_trigger_ops *ops,
				    struct event_trigger_data *data)
{
	struct hist_trigger_data *hist_data = data->private_data;

	if (WARN_ON_ONCE(data->ref <= 0))
		return;

	data->ref--;
	if (!data->ref) {
		if (data->name)
			del_named_trigger(data);
		trigger_data_free(data);
		destroy_hist_data(hist_data);
	}
}

static struct event_trigger_ops event_hist_trigger_ops = {
	.func			= event_hist_trigger,
	.print			= event_hist_trigger_print,
	.init			= event_hist_trigger_init,
	.free			= event_hist_trigger_free,
};

static int event_hist_trigger_named_init(struct event_trigger_ops *ops,
					 struct event_trigger_data *data)
{
	data->ref++;

	save_named_trigger(data->named_data->name, data);

	event_hist_trigger_init(ops, data->named_data);

	return 0;
}

static void event_hist_trigger_named_free(struct event_trigger_ops *ops,
					  struct event_trigger_data *data)
{
	if (WARN_ON_ONCE(data->ref <= 0))
		return;

	event_hist_trigger_free(ops, data->named_data);

	data->ref--;
	if (!data->ref) {
		del_named_trigger(data);
		trigger_data_free(data);
	}
}

static struct event_trigger_ops event_hist_trigger_named_ops = {
	.func			= event_hist_trigger,
	.print			= event_hist_trigger_print,
	.init			= event_hist_trigger_named_init,
	.free			= event_hist_trigger_named_free,
};

static struct event_trigger_ops *event_hist_get_trigger_ops(char *cmd,
							    char *param)
{
	return &event_hist_trigger_ops;
}

static void hist_clear(struct event_trigger_data *data)
{
	struct hist_trigger_data *hist_data = data->private_data;

	if (data->name)
		pause_named_trigger(data);

	synchronize_sched();

	tracing_map_clear(hist_data->map);

	if (data->name)
		unpause_named_trigger(data);
}

static bool compatible_field(struct ftrace_event_field *field,
			     struct ftrace_event_field *test_field)
{
	if (field == test_field)
		return true;
	if (field == NULL || test_field == NULL)
		return false;
	if (strcmp(field->name, test_field->name) != 0)
		return false;
	if (strcmp(field->type, test_field->type) != 0)
		return false;
	if (field->size != test_field->size)
		return false;
	if (field->is_signed != test_field->is_signed)
		return false;

	return true;
}

static bool hist_trigger_match(struct event_trigger_data *data,
			       struct event_trigger_data *data_test,
			       struct event_trigger_data *named_data,
			       bool ignore_filter)
{
	struct tracing_map_sort_key *sort_key, *sort_key_test;
	struct hist_trigger_data *hist_data, *hist_data_test;
	struct hist_field *key_field, *key_field_test;
	unsigned int i;

	if (named_data && (named_data != data_test) &&
	    (named_data != data_test->named_data))
		return false;

	if (!named_data && is_named_trigger(data_test))
		return false;

	hist_data = data->private_data;
	hist_data_test = data_test->private_data;

	if (hist_data->n_vals != hist_data_test->n_vals ||
	    hist_data->n_fields != hist_data_test->n_fields ||
	    hist_data->n_sort_keys != hist_data_test->n_sort_keys)
		return false;

	if (!ignore_filter) {
		if ((data->filter_str && !data_test->filter_str) ||
		   (!data->filter_str && data_test->filter_str))
			return false;
	}

	for_each_hist_field(i, hist_data) {
		key_field = hist_data->fields[i];
		key_field_test = hist_data_test->fields[i];

		if (key_field->flags != key_field_test->flags)
			return false;
		if (!compatible_field(key_field->field, key_field_test->field))
			return false;
		if (key_field->offset != key_field_test->offset)
			return false;
		if (key_field->size != key_field_test->size)
			return false;
		if (key_field->is_signed != key_field_test->is_signed)
			return false;
		if (!!key_field->var.name != !!key_field_test->var.name)
			return false;
		if (key_field->var.name &&
		    strcmp(key_field->var.name, key_field_test->var.name) != 0)
			return false;
	}

	for (i = 0; i < hist_data->n_sort_keys; i++) {
		sort_key = &hist_data->sort_keys[i];
		sort_key_test = &hist_data_test->sort_keys[i];

		if (sort_key->field_idx != sort_key_test->field_idx ||
		    sort_key->descending != sort_key_test->descending)
			return false;
	}

	if (!ignore_filter && data->filter_str &&
	    (strcmp(data->filter_str, data_test->filter_str) != 0))
		return false;

	return true;
}

static int hist_register_trigger(char *glob, struct event_trigger_ops *ops,
				 struct event_trigger_data *data,
				 struct trace_event_file *file)
{
	struct hist_trigger_data *hist_data = data->private_data;
	struct event_trigger_data *test, *named_data = NULL;
	int ret = 0;

	if (hist_data->attrs->name) {
		named_data = find_named_trigger(hist_data->attrs->name);
		if (named_data) {
			if (!hist_trigger_match(data, named_data, named_data,
						true)) {
				ret = -EINVAL;
				goto out;
			}
		}
	}

	if (hist_data->attrs->name && !named_data)
		goto new;

	list_for_each_entry_rcu(test, &file->triggers, list) {
		if (test->cmd_ops->trigger_type == ETT_EVENT_HIST) {
			if (!hist_trigger_match(data, test, named_data, false))
				continue;
			if (hist_data->attrs->pause)
				test->paused = true;
			else if (hist_data->attrs->cont)
				test->paused = false;
			else if (hist_data->attrs->clear)
				hist_clear(test);
			else
				ret = -EEXIST;
			goto out;
		}
	}
 new:
	if (hist_data->attrs->cont || hist_data->attrs->clear) {
		ret = -ENOENT;
		goto out;
	}

	if (hist_data->attrs->pause)
		data->paused = true;

	if (named_data) {
		destroy_hist_data(data->private_data);
		data->private_data = named_data->private_data;
		set_named_trigger_data(data, named_data);
		data->ops = &event_hist_trigger_named_ops;
	}

	if (data->ops->init) {
		ret = data->ops->init(data->ops, data);
		if (ret < 0)
			goto out;
	}

	list_add_rcu(&data->list, &file->triggers);
	ret++;

	update_cond_flag(file);

	if (hist_data->enable_timestamps)
		tracing_set_time_stamp_abs(file->tr, true);

	if (trace_event_trigger_enable_disable(file, 1) < 0) {
		list_del_rcu(&data->list);
		update_cond_flag(file);
		ret--;
	}
 out:
	return ret;
}

static void hist_unregister_trigger(char *glob, struct event_trigger_ops *ops,
				    struct event_trigger_data *data,
				    struct trace_event_file *file)
{
	struct hist_trigger_data *hist_data = data->private_data;
	struct event_trigger_data *test, *named_data = NULL;
	bool unregistered = false;

	if (hist_data->attrs->name)
		named_data = find_named_trigger(hist_data->attrs->name);

	list_for_each_entry_rcu(test, &file->triggers, list) {
		if (test->cmd_ops->trigger_type == ETT_EVENT_HIST) {
			if (!hist_trigger_match(data, test, named_data, false))
				continue;
			unregistered = true;
			list_del_rcu(&test->list);
			trace_event_trigger_enable_disable(file, 0);
			update_cond_flag(file);
			break;
		}
	}

	if (unregistered && test->ops->free)
		test->ops->free(test->ops, test);

	if (hist_data->enable_timestamps) {
		if (!hist_data->remove || unregistered)
			tracing_set_time_stamp_abs(file->tr, false);
	}
}

static void hist_unreg_all(struct trace_event_file *file)
{
	struct event_trigger_data *test, *n;
	struct hist_trigger_data *hist_data;

	list_for_each_entry_safe(test, n, &file->triggers, list) {
		if (test->cmd_ops->trigger_type == ETT_EVENT_HIST) {
			hist_data = test->private_data;
			list_del_rcu(&test->list);
			trace_event_trigger_enable_disable(file, 0);
			update_cond_flag(file);
			if (hist_data->enable_timestamps)
				tracing_set_time_stamp_abs(file->tr, false);
			if (test->ops->free)
				test->ops->free(test->ops, test);
		}
	}
}

static int event_hist_trigger_func(struct event_command *cmd_ops,
				   struct trace_event_file *file,
				   char *glob, char *cmd, char *param)
{
	unsigned int hist_trigger_bits = TRACING_MAP_BITS_DEFAULT;
	struct event_trigger_data *trigger_data;
	struct hist_trigger_attrs *attrs;
	struct event_trigger_ops *trigger_ops;
	struct hist_trigger_data *hist_data;
	bool remove = false;
	char *trigger;
	int ret = 0;

	if (!param)
		return -EINVAL;

	if (glob[0] == '!')
		remove = true;

	/* separate the trigger from the filter (k:v [if filter]) */
	trigger = strsep(&param, " \t");
	if (!trigger)
		return -EINVAL;

	attrs = parse_hist_trigger_attrs(trigger);
	if (IS_ERR(attrs))
		return PTR_ERR(attrs);

	if (attrs->map_bits)
		hist_trigger_bits = attrs->map_bits;

	hist_data = create_hist_data(hist_trigger_bits, attrs, file, remove);
	if (IS_ERR(hist_data)) {
		destroy_hist_trigger_attrs(attrs);
		return PTR_ERR(hist_data);
	}

	trigger_ops = cmd_ops->get_trigger_ops(cmd, trigger);

	ret = -ENOMEM;
	trigger_data = kzalloc(sizeof(*trigger_data), GFP_KERNEL);
	if (!trigger_data)
		goto out_free;

	trigger_data->count = -1;
	trigger_data->ops = trigger_ops;
	trigger_data->cmd_ops = cmd_ops;

	INIT_LIST_HEAD(&trigger_data->list);
	RCU_INIT_POINTER(trigger_data->filter, NULL);

	trigger_data->private_data = hist_data;

	/* if param is non-empty, it's supposed to be a filter */
	if (param && cmd_ops->set_filter) {
		ret = cmd_ops->set_filter(param, trigger_data, file);
		if (ret < 0)
			goto out_free;
	}

	if (remove) {
		cmd_ops->unreg(glob+1, trigger_ops, trigger_data, file);
		ret = 0;
		goto out_free;
	}

	ret = cmd_ops->reg(glob, trigger_ops, trigger_data, file);
	/*
	 * The above returns on success the # of triggers registered,
	 * but if it didn't register any it returns zero.  Consider no
	 * triggers registered a failure too.
	 */
	if (!ret) {
		if (!(attrs->pause || attrs->cont || attrs->clear))
			ret = -ENOENT;
		goto out_free;
	} else if (ret < 0)
		goto out_free;
	/* Just return zero, not the number of registered triggers */
	ret = 0;
 out:
	return ret;
 out_free:
	if (cmd_ops->set_filter)
		cmd_ops->set_filter(NULL, trigger_data, NULL);

	kfree(trigger_data);

	destroy_hist_data(hist_data);
	goto out;
}

static struct event_command trigger_hist_cmd = {
	.name			= "hist",
	.trigger_type		= ETT_EVENT_HIST,
	.flags			= EVENT_CMD_FL_NEEDS_REC,
	.func			= event_hist_trigger_func,
	.reg			= hist_register_trigger,
	.unreg			= hist_unregister_trigger,
	.unreg_all		= hist_unreg_all,
	.get_trigger_ops	= event_hist_get_trigger_ops,
	.set_filter		= set_trigger_filter,
};

__init int register_trigger_hist_cmd(void)
{
	int ret;

	ret = register_event_command(&trigger_hist_cmd);
	WARN_ON(ret < 0);

	return ret;
}

static void
hist_enable_trigger(struct event_trigger_data *data, void *rec,
		    struct ring_buffer_event *event)
{
	struct enable_trigger_data *enable_data = data->private_data;
	struct event_trigger_data *test;

	list_for_each_entry_rcu(test, &enable_data->file->triggers, list) {
		if (test->cmd_ops->trigger_type == ETT_EVENT_HIST) {
			if (enable_data->enable)
				test->paused = false;
			else
				test->paused = true;
		}
	}
}

static void
hist_enable_count_trigger(struct event_trigger_data *data, void *rec,
			  struct ring_buffer_event *event)
{
	if (!data->count)
		return;

	if (data->count != -1)
		(data->count)--;

	hist_enable_trigger(data, rec, event);
}

static struct event_trigger_ops hist_enable_trigger_ops = {
	.func			= hist_enable_trigger,
	.print			= event_enable_trigger_print,
	.init			= event_trigger_init,
	.free			= event_enable_trigger_free,
};

static struct event_trigger_ops hist_enable_count_trigger_ops = {
	.func			= hist_enable_count_trigger,
	.print			= event_enable_trigger_print,
	.init			= event_trigger_init,
	.free			= event_enable_trigger_free,
};

static struct event_trigger_ops hist_disable_trigger_ops = {
	.func			= hist_enable_trigger,
	.print			= event_enable_trigger_print,
	.init			= event_trigger_init,
	.free			= event_enable_trigger_free,
};

static struct event_trigger_ops hist_disable_count_trigger_ops = {
	.func			= hist_enable_count_trigger,
	.print			= event_enable_trigger_print,
	.init			= event_trigger_init,
	.free			= event_enable_trigger_free,
};

static struct event_trigger_ops *
hist_enable_get_trigger_ops(char *cmd, char *param)
{
	struct event_trigger_ops *ops;
	bool enable;

	enable = (strcmp(cmd, ENABLE_HIST_STR) == 0);

	if (enable)
		ops = param ? &hist_enable_count_trigger_ops :
			&hist_enable_trigger_ops;
	else
		ops = param ? &hist_disable_count_trigger_ops :
			&hist_disable_trigger_ops;

	return ops;
}

static void hist_enable_unreg_all(struct trace_event_file *file)
{
	struct event_trigger_data *test, *n;

	list_for_each_entry_safe(test, n, &file->triggers, list) {
		if (test->cmd_ops->trigger_type == ETT_HIST_ENABLE) {
			list_del_rcu(&test->list);
			update_cond_flag(file);
			trace_event_trigger_enable_disable(file, 0);
			if (test->ops->free)
				test->ops->free(test->ops, test);
		}
	}
}

static struct event_command trigger_hist_enable_cmd = {
	.name			= ENABLE_HIST_STR,
	.trigger_type		= ETT_HIST_ENABLE,
	.func			= event_enable_trigger_func,
	.reg			= event_enable_register_trigger,
	.unreg			= event_enable_unregister_trigger,
	.unreg_all		= hist_enable_unreg_all,
	.get_trigger_ops	= hist_enable_get_trigger_ops,
	.set_filter		= set_trigger_filter,
};

static struct event_command trigger_hist_disable_cmd = {
	.name			= DISABLE_HIST_STR,
	.trigger_type		= ETT_HIST_ENABLE,
	.func			= event_enable_trigger_func,
	.reg			= event_enable_register_trigger,
	.unreg			= event_enable_unregister_trigger,
	.unreg_all		= hist_enable_unreg_all,
	.get_trigger_ops	= hist_enable_get_trigger_ops,
	.set_filter		= set_trigger_filter,
};

static __init void unregister_trigger_hist_enable_disable_cmds(void)
{
	unregister_event_command(&trigger_hist_enable_cmd);
	unregister_event_command(&trigger_hist_disable_cmd);
}

__init int register_trigger_hist_enable_disable_cmds(void)
{
	int ret;

	ret = register_event_command(&trigger_hist_enable_cmd);
	if (WARN_ON(ret < 0))
		return ret;
	ret = register_event_command(&trigger_hist_disable_cmd);
	if (WARN_ON(ret < 0))
		unregister_trigger_hist_enable_disable_cmds();

	return ret;
}
