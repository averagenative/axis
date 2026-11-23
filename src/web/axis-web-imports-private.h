/*
 * axis-web-imports-private.h - shared by the files of the import pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   axis-web-imports.c        the module: work off the main loop, the jobs,
 *                             the dashboard's items
 *   axis-web-imports-pages.c  the pages
 */

#ifndef AXIS_WEB_IMPORTS_PRIVATE_H
#define AXIS_WEB_IMPORTS_PRIVATE_H

#include <htmx-glib.h>

#include "web/axis-web.h"
#include "web/axis-web-imports.h"

G_BEGIN_DECLS

#define AXIS_WEB_IMPORTS_MAX_PENDING 16

struct _AxisWebImports
{
	AxisImport   *import;	/* owned reference */
	AxisSettings *settings;	/* (nullable) not owned */
	guint         pending;	/* (main loop) */
	gboolean      stopping;	/* (main loop) */
};

typedef void (*AxisWebImportsWorkFunc)(AxisWebImports *self, gpointer data);

typedef HtmxResponse *(*AxisWebImportsDoneFunc)(
	AxisWebImports *self,
	HtmxRequest    *request,
	gpointer        data
);

/* As axis_web_downloads_dispatch(). */
HtmxResponse *
axis_web_imports_dispatch(
	AxisWebImports         *self,
	HtmxRequest            *request,
	AxisWebImportsWorkFunc  work,
	AxisWebImportsDoneFunc  done,
	gpointer                data,
	GDestroyNotify          free_data
);

void
axis_web_imports_register_pages(
	AxisWebImports *self,
	AxisWeb        *web
);

/* "Assign by hand" for an import (axis-web-imports-assign.c). */
void
axis_web_imports_register_assign(
	AxisWebImports *self,
	AxisWeb        *web
);

/* "Delete this download" for an import (axis-web-imports-delete.c). */
void
axis_web_imports_register_delete(
	AxisWebImports *self,
	AxisWeb        *web
);

/* Media -> Duplicates (axis-web-imports-duplicates.c). */
void
axis_web_imports_register_duplicates(
	AxisWebImports *self,
	AxisWeb        *web
);

/* Downloads -> Leftovers (axis-web-imports-leftovers.c). */
void
axis_web_imports_register_leftovers(
	AxisWebImports *self,
	AxisWeb        *web
);

G_END_DECLS

#endif /* AXIS_WEB_IMPORTS_PRIVATE_H */
