/*
 * kicad/ext.c - Identify file by their extension
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
#include <string.h>

#include "misc/diag.h"
#include "kicad/ext.h"


enum ext identify(const char *path)
{
	const char *dot, *slash;

	dot = strrchr(path, '.');
	if (!dot)
		return ext_unknown;
	slash = strchr(dot + 1, '/');
	if (slash)
		return ext_unknown;

	if (!strcmp(dot, ".pro"))
		return ext_project;
	if (!strcmp(dot, ".sch"))
		return ext_sch;
	if (!strcmp(dot, ".lib"))
		return ext_lib;
	if (!strcmp(dot, ".kicad_wks"))
		return ext_pl;

	return ext_unknown;
}


void classify_files(struct file_names *fn, char *const *args,
    unsigned n_args)
{
	unsigned i;

	fn->pro = fn->sch = fn->pl = NULL;
	fn->libs = NULL;
	fn->n_libs = 0;

	for (i = 0; i != n_args; i++) {
		switch (identify(args[i])) {
		case ext_unknown:
			fatal("%s: unknown file type", args[i]);
		case ext_project:
			if (fn->pro)
				fatal("%s: there can only be one project",
				    args[i]);
			fn->pro = args[i];
			break;
		case ext_sch:
			if (fn->sch)
				fatal("%s: there can only be one top sheet",
				    args[i]);
			fn->sch = args[i];
			break;
		case ext_lib:
			fn->n_libs++;
			fn->libs = realloc(fn->libs,
			    fn->n_libs * sizeof(const char *));
			if (!fn->libs)
				diag_pfatal("realloc");
			fn->libs[fn->n_libs - 1] = args[i];
			break;
		case ext_pl:
			if (fn->pl)
				fatal("%s: there can only be one page layout",
				    args[i]);
			fn->pl = args[i];
			break;
		default:
			abort();
		}
	}
}