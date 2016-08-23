/*
 * kicad/lib.c - Parse Eeschema .lib file
 *
 * Written 2016 by Werner Almesberger
 * Copyright 2016 by Werner Almesberger
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */


#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include "misc/util.h"
#include "misc/diag.h"
#include "gfx/text.h"
#include "file/file.h"
#include "kicad/lib.h"


/* ----- Text -------------------------------------------------------------- */


static enum text_style decode_style(const char *s)
{
	if (!strcmp(s, "Normal"))
		return text_normal;
	if (!strcmp(s, "Italic"))
		return text_italic;
	assert(0);
}


/* ----- Polygons ---------------------------------------------------------- */


static bool parse_poly(struct lib_poly *poly, const char *line, int points)
{
	int i, n;

	poly->points = points;
	poly->x = alloc_type_n(int, points);
	poly->y = alloc_type_n(int, points);
	for (i = 0; i != points; i++) {
		if (sscanf(line, "%d %d %n",
		    poly->x + i, poly->y + i, &n) != 2)
			return 0;
		line += n;
	}
	if (sscanf(line, "%c", &poly->fill) != 1)
		return 0;
	return 1;
}


/* ----- Definitions ------------------------------------------------------- */


static bool parse_def(struct lib *lib, const char *line)
{
	char *s, *ref;
	char draw_num, draw_name;
	unsigned name_offset;
	unsigned units;
	bool power;

	if (sscanf(line, "DEF %ms %ms %*d %u %c %c %u",
	    &s, &ref, &name_offset, &draw_num, &draw_name, &units) != 6)
		return 0;

	power = *ref == '#';
	free(ref);

	lib->curr_comp = alloc_type(struct comp);
	if (*s == '~') {
		char *tmp = alloc_size(strlen(s) + 1);

		/* we can't just s++, since that would break freeing */
		strcpy(tmp, s + 1);
		free(s);
		s = tmp;
	}
	lib->curr_comp->name = s;
	lib->curr_comp->aliases = NULL;
	lib->curr_comp->units = units;

	lib->curr_comp->visible = 0;
	lib->curr_comp->show_pin_name = draw_name == 'Y' && !power;
	lib->curr_comp->show_pin_num = draw_num == 'Y' && !power;
	lib->curr_comp->name_offset = name_offset;

	lib->curr_comp->objs = NULL;
	lib->next_obj = &lib->curr_comp->objs;

	lib->curr_comp->next = NULL;
	*lib->next_comp = lib->curr_comp;
	lib->next_comp = &lib->curr_comp->next;

	return 1;
}


/* ----- Arcs -------------------------------------------------------------- */


static bool parse_arc(struct lib_obj *obj, const char *line)
{
	struct lib_arc *arc = &obj->u.arc;
	int a1, a2;

	if (sscanf(line, "A %d %d %d %d %d %u %u %u %c",
	    &arc->x, &arc->y, &arc->r, &a1, &a2, &obj->unit, &obj->convert,
	    &arc->thick, &arc->fill) != 9)
		return 0;

	/*
	 * KiCad arcs can be clockwise or counter-clockwise. They must always
	 * be smaller than 180 degrees.
	 */

	while (a1 < 0)
		a1 += 3600;
	while (a2 < 0)
		a2 += 3600;
	a1 %= 3600;
	a2 %= 3600;
	if (a2 < a1)
		a2 += 3600;
	assert(a2 - a1 != 1800);
	if (a2 - a1 > 1800)
		swap(a1, a2);

	arc->start_a = (a1 % 3600) / 10;
	arc->end_a = (a2 % 3600) / 10;

	return 1;
}


/* ----- Aliases ----------------------------------------------------------- */


static void add_alias(struct comp *comp, const char *alias)
{
	struct comp_alias *new;

	new = alloc_type(struct comp_alias);
	new->name = alias;
	new->next = comp->aliases;
	comp->aliases = new;
}


/* ----- Library parser ---------------------------------------------------- */


static enum pin_shape decode_pin_shape(const char *s)
{
	int flags = 0;

	if (*s == 'N') {
		flags = pin_invisible;
		s++;
	}
	if (!strcmp(s, ""))
		return flags;
	if (!strcmp(s, "I"))
		return flags | pin_inverted;
	if (!strcmp(s, "C"))
		return flags | pin_clock;
	if (!strcmp(s, "CI") || !strcmp(s, "IC"))
		return flags | pin_clock | pin_inverted;
	if (!strcmp(s, "L"))
		return flags | pin_input_low;
	if (!strcmp(s, "CL"))
		return flags | pin_clock | pin_input_low;
	if (!strcmp(s, "V"))
		return flags | pin_output_low;;
	if (!strcmp(s, "F"))
		return flags | pin_falling_edge;
	if (!strcmp(s, "X"))
		return flags | pin_non_logic;
	error("unrecognized pin shape \"%s\"", s);
	return flags;
}


static bool lib_parse_line(const struct file *file,
    void *user, const char *line)
{
	struct lib *lib = user;
	int n = 0;
	unsigned points;
	struct lib_obj *obj;
	char *s, *style;
	unsigned zero1, zero2;
	char vis;

	switch (lib->state) {
	case lib_skip:
		if (parse_def(lib, line)) {
			lib->state = lib_def;
			return 1;
		}
		return 1;
	case lib_def:
		if (sscanf(line, "DRAW%n", &n) == 0 && n) {
			lib->state = lib_draw;
			return 1;
		}
		if (sscanf(line, "F%d \"\" %*d %*d %*d %*c %c", &n, &vis) == 2
		    || sscanf(line, "F%d \"%*[^\"]\" %*d %*d %*d %*c %c",
		    &n, &vis) == 2) {
			if (vis == 'V')
				lib->curr_comp->visible |= 1 << n;
			return 1;
		}
		if (sscanf(line, "ALIAS %ms", &s) == 1) {
			add_alias(lib->curr_comp, s);
			return 1;
		}
		/* @@@ explicitly ignore FPLIST */
		return 1;
	case lib_draw:
		if (sscanf(line, "ENDDRAW%n", &n) == 0 && n) {
			lib->state = lib_skip;
			return 1;
		}

		obj = alloc_type(struct lib_obj);
		obj->next = NULL;
		*lib->next_obj = obj;
		lib->next_obj = &obj->next;

		if (sscanf(line, "P %u %u %u %u %n",
		    &points, &obj->unit, &obj->convert, &obj->u.poly.thick,
		    &n) == 4) {
			obj->type = lib_obj_poly;
			if (parse_poly(&obj->u.poly, line + n, points))
				return 1;
			break;
		}
		if (sscanf(line, "S %d %d %d %d %u %u %d %c",
		    &obj->u.rect.sx, &obj->u.rect.sy, &obj->u.rect.ex,
		    &obj->u.rect.ey, &obj->unit, &obj->convert,
		    &obj->u.rect.thick, &obj->u.rect.fill) == 8) {
			obj->type = lib_obj_rect;
			return 1;
		}
		if (sscanf(line, "C %d %d %d %u %u %d %c",
		    &obj->u.circ.x, &obj->u.circ.y, &obj->u.circ.r,
		    &obj->unit, &obj->convert, &obj->u.circ.thick,
		    &obj->u.circ.fill) == 7) {
			obj->type = lib_obj_circ;
			return 1;
		}
		if (parse_arc(obj, line)) {
			obj->type = lib_obj_arc;
			return 1;
		}
		n = sscanf(line,
		    "T %d %d %d %d %u %u %u \"%m[^\"]\" %ms %u %c %c",
		    &obj->u.text.orient, &obj->u.text.x, &obj->u.text.y,
		    &obj->u.text.dim, &zero1, &obj->unit, &obj->convert,
		    &obj->u.text.s, &style, &zero2,
		    &obj->u.text.hor_align, &obj->u.text.vert_align);
		if (n != 12) {
			n = sscanf(line,
			    "T %d %d %d %d %u %u %u %ms %ms %u %c %c",
			    &obj->u.text.orient, &obj->u.text.x, &obj->u.text.y,
			    &obj->u.text.dim, &zero1, &obj->unit, &obj->convert,
			    &obj->u.text.s, &style, &zero2,
			    &obj->u.text.hor_align, &obj->u.text.vert_align);
			while (n == 12) {
				char *tilde;

				tilde = strchr(obj->u.text.s, '~');
				if (!tilde)
					break;
				*tilde = ' ';
			}
		}
		/*
		 * zero2 seems to be the font style: 0 = normal, 1 = bold ?
		 */
		if (n == 12) {
			if (zero1)
				fatal("%u: only understand 0 x x\n"
				    "\"%s\"\n", file->lineno, line);
			obj->u.text.style = decode_style(style);
			obj->type = lib_obj_text;
			return 1;
		}
		s = NULL;
		if (sscanf(line, "X %ms %ms %d %d %d %c %d %d %u %u %c %ms",
		    &obj->u.pin.name, &obj->u.pin.number,
		    &obj->u.pin.x, &obj->u.pin.y, &obj->u.pin.length,
		    &obj->u.pin.orient,
		    &obj->u.pin.number_size, &obj->u.pin.name_size,
		    &obj->unit, &obj->convert, &obj->u.pin.etype, &s) >= 11) {
			obj->type = lib_obj_pin;
			if (s) {
				obj->u.pin.shape = decode_pin_shape(s);
				free(s);
			} else {
				obj->u.pin.shape = 0;
			}
			return 1;
		}
		break;
	default:
		BUG("invalid state %d", lib->state);
	}
	fatal("%u: cannot parse\n\"%s\"\n", file->lineno, line);
}


bool lib_parse_file(struct lib *lib, struct file *file)
{
	lib->state = lib_skip;
	return file_read(file, lib_parse_line, lib);
}



bool lib_parse(struct lib *lib, const char *name, const struct file *related)
{
	struct file file;
	bool res;

	if (!file_open(&file, name, related))
		return 0;
	res = lib_parse_file(lib, &file);
	file_close(&file);
	return res;
}


void lib_init(struct lib *lib)
{
	lib->comps = NULL;
	lib->next_comp = &lib->comps;
}


static void free_objs(struct lib_obj *objs)
{
	struct lib_obj *next;

	while (objs) {
		next = objs->next;
		switch (objs->type) {
		case lib_obj_text:
			free((char *) objs->u.text.s);
			break;
		case lib_obj_pin:
			free((char *) objs->u.pin.name);
			free((char *) objs->u.pin.number);
			break;
		default:
			break;
		}
		free(objs);
		objs = next;
	}
}


static void free_comp(struct comp *comp)
{
	struct comp_alias *next;

	free((char *) comp->name);
	while (comp->aliases) {
		next = comp->aliases->next;
		free((char *) comp->aliases->name);
		comp->aliases = next;
	}
	free_objs(comp->objs);
	free(comp);
}


void lib_free(struct lib *lib)
{
	struct comp *comp, *next;

	for (comp = lib->comps; comp; comp = next) {
		next = comp->next;
		free_comp(comp);
	}
}
