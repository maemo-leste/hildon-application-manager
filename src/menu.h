/*
 * This file is part of the hildon-application-manager.
 *
 * Copyright (C) 2005, 2006, 2007 Nokia Corporation.  All Rights reserved.
 *
 * Contact: Marius Vollmer <marius.vollmer@nokia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#ifndef MENU_H
#define MENU_H

#include <gtk/gtk.h>
#include <hildon/hildon-window.h>

void create_menu (HildonWindow *window);
void set_details_menu_sensitive (bool);
void set_search_menu_sensitive (bool);
void set_operation_menu_label (const gchar *label, bool sensitive,
			       const gchar *insens);
void set_operation_menu_item_sensitiveness (bool sensitive);

void set_fullscreen_menu_check (bool f);

GtkWidget *create_package_menu (const char *op_label);

void menu_close ();

#endif /* !MENU_H */
