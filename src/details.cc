/*
 * This file is part of the hildon-application-manager.
 *
 * Copyright (C) 2005, 2006, 2007, 2008 Nokia Corporation.  All Rights reserved.
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

#include <stdio.h>
#include <gtk/gtk.h>
#include <assert.h>
#include <libintl.h>
#include <string.h>
#include <hildon/hildon.h>

#include "details.h"
#include "log.h"
#include "util.h"
#include "settings.h"
#include "apt-worker-client.h"
#include "apt-worker-proto.h"

#define _(x) gettext (x)

/* Utilities
 */

static bool
is_installable (package_info *pi)
{
  bool info_status = (pi->have_info
                      && pi->info.installable_status == status_able);

  /* Consider red pill mode options */
  if (red_pill_mode && red_pill_ignore_thirdparty_policy)
    return info_status;
  else
    return (pi->third_party_policy != third_party_incompatible) && info_status;
}

static const char *
deptype_name (apt_proto_deptype dep)
{
  switch (dep)
    {
    case deptype_depends:
      return "Depends";
    case deptype_conflicts:
      return "Conflicts";
    default:
      return "Unknown";
    }
}

static char *
decode_dependencies (apt_proto_decoder *dec)
{
  GString *str = g_string_new ("");
  char *chars;

  while (true)
    {
      apt_proto_deptype type = (apt_proto_deptype) dec->decode_int ();
      if (dec->corrupted () || type == deptype_end)
	break;
      
      const char *target = dec->decode_string_in_place ();
      g_string_append_printf (str, "%s: %s\n", deptype_name (type), target);
    }

  chars = str->str;
  g_string_free (str, 0);
  return chars;
}

void
decode_summary (apt_proto_decoder *dec, package_info *pi, detail_kind kind)
{
  char size_buf[20];

  g_free (pi->summary);
  pi->summary = NULL;

  for (int i = 0; i < sumtype_max; i++)
    {
      //g_free (pi->summary_packages[i]);
      pi->summary_packages[i] = NULL;
    }

  while (true)
    {
      apt_proto_sumtype type = (apt_proto_sumtype) dec->decode_int ();
      if (dec->corrupted () || type == sumtype_end)
	break;
      
      char *target = dec->decode_string_dup ();
      if (type >= 0 && type < sumtype_max)
	pi->summary_packages[type] = g_list_append (pi->summary_packages[type],
						    target);
      else
	g_free (target);
    }

  bool possible = true;
  if (kind == remove_details)
    {
      const char *name = pi->get_display_name (true);

      if (pi->info.removable_status == status_able)
	{
	  size_string_detailed (size_buf, 20,
				-pi->info.remove_user_size_delta);
	  pi->summary =
	    g_strdup_printf (_("ai_va_details_uninstall_frees"),
			     name, size_buf);
	}
      else
	{
	  pi->summary =
	    g_strdup_printf (_("ai_va_details_unable_uninstall"), name);
	  possible = false;
	}
    }
  else
    {
      const char *name = pi->get_display_name (false);

      if (pi->installed_version)
	{
	  if (is_installable (pi))
	    {
	      if (pi->info.install_user_size_delta >= 0)
		{
		  size_string_detailed (size_buf, 20,
					pi->info.install_user_size_delta);
		  pi->summary =
		    g_strdup_printf (_("ai_va_details_update_requires"),
				     name, size_buf);
		}
	      else
		{
		  size_string_detailed (size_buf, 20,
					-pi->info.install_user_size_delta);
		  pi->summary =
		    g_strdup_printf (_("ai_va_details_update_frees"),
				     name, size_buf);
		}
	    }
	  else
	    {
	      pi->summary =
		g_strdup_printf (_("ai_va_details_unable_update"), name);
	      possible = false;
	    }
	}
      else
	{
	  if (is_installable (pi))
	    {
	      if (pi->info.install_user_size_delta >= 0)
		{
		  size_string_detailed (size_buf, 20,
					pi->info.install_user_size_delta);
		  pi->summary =
		    g_strdup_printf (_("ai_va_details_install_requires"),
				     name, size_buf);
		}
	      else
		{
		  size_string_detailed (size_buf, 20,
					-pi->info.install_user_size_delta);
		  pi->summary =
		    g_strdup_printf (_("ai_va_details_install_frees"),
				     name, size_buf);
		}
	    }
	  else
	    {
	      pi->summary =
		g_strdup_printf (_("ai_va_details_unable_install"),
				 name, size_buf);
	      possible = false;
	    }
	}
    }
}

static void
add_table_field (GtkWidget *table, int row,
		 const char *field, const char *value,
		 PangoEllipsizeMode em = PANGO_ELLIPSIZE_NONE)
{
  GtkWidget *label;

  if (field)
    {
      label = make_small_label (field);
      gtk_misc_set_alignment (GTK_MISC (label), 1.0, 0.0);
      gtk_table_attach (GTK_TABLE (table), label, 0, 1, row, row+1,
			GTK_FILL, GTK_FILL, 0, 0);
    }

  if (value && !all_whitespace (value))
    {
      label = make_small_label (value);
      gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.0);
      gtk_label_set_line_wrap (GTK_LABEL (label), FALSE);
      gtk_label_set_ellipsize (GTK_LABEL (label), em);

      gtk_table_attach (GTK_TABLE (table), label, 1, 2, row, row+1,
			GtkAttachOptions (GTK_EXPAND | GTK_FILL), GTK_FILL,
			0, 0);

      if (red_pill_include_details_in_log)
	add_log ("%s %s\n", field? field : "\t", value);
    }
}

static int
add_table_list (GtkWidget *table, int row,
		const char *title,
		GList *list)
{
  while (list)
    {
      add_table_field (table, row++, title, (char *)list->data);
      title = NULL;
      list = list->next;
    }
  return row;
}

void
nicify_description_in_place (char *desc)
{
  if (desc == NULL)
    return;

  /* The nicifications are this:

     - the first space of a line is removed.

     - if after that a line consists solely of a '.', that dot is
       removed.
  */

  char *src = desc, *dst = desc;
  char *bol = src;

  while (*src)
    {
      if (src == bol && *src == ' ')
	{
	  src++;
	  continue;
	}

      if (*src == '\n')
	{
	  if (bol+2 == src && bol[0] == ' ' && bol[1] == '.')
	    dst--;
	  bol = src + 1;
	}

      *dst++ = *src++;
    }

  *dst = '\0';
}

/* Show package details
 */

struct spd_clos {
  package_info *pi;
  detail_kind kind;
  bool show_problems;

  GtkWidget *dialog;
  GtkWidget *notebook;
  GtkWidget *table;
  GtkWidget **spd_nb_widgets;
  bool showing_details;

  void (*cont) (void * data);
  void *data;
};

enum spd_nb_page_index {
  SPD_COMMON_PAGE,
  SPD_DESCRIPTION_PAGE,
  SPD_SUMMARY_PAGE,
  SPD_DEPS_PAGE,
  SPD_NUM_PAGES
};

static spd_clos *current_spd_clos = NULL;

static void spd_third_party_policy_check (package_info *pi,
                                          void *data,
                                          bool unused);
static void spd_third_party_policy_check_reply (package_info *pi, void *data);

static void spd_get_details (void *data);
static void spd_get_details_reply (int cmd, apt_proto_decoder *dec, void *data);

static GtkWidget *spd_create_common_page (void *data);
static GtkWidget *spd_create_description_page (void *data);
static GtkWidget *spd_create_summary_page (void *data);
static const gchar *spd_get_summary_label (void *data);
static GtkWidget *spd_create_deps_page (void *data);

static void spd_with_details (void *data, bool filling_details);

static void spd_response (GtkDialog *dialog, gint response, gpointer data);
static void spd_end (void *data);

void
show_package_details (package_info *pi, detail_kind kind,
                      bool show_problems, 
                      void (*cont) (void *data), void *data)
{
  spd_clos *c = new spd_clos;
  current_spd_clos = c;

  c->pi = pi;
  c->kind = kind;
  c->show_problems = show_problems;
  c->cont = cont;
  c->data = data;
  c->dialog = NULL;
  c->notebook = NULL;
  c->table = NULL;
  c->spd_nb_widgets = NULL;
  c->showing_details = false;
  pi->ref ();

  allow_updating ();

  /* Build the dialog */
  spd_with_details (c, false);

  /* Show the data */
  if (pi->have_detail_kind != c->kind)
    get_package_info (pi, false, spd_third_party_policy_check, c);
  else
    spd_with_details (c, true);
}

static void
spd_third_party_policy_check (package_info *pi, void *data, bool unused)
{
  spd_clos *c = (spd_clos *)data;

  /* Exit if no valid data at this point */
  if (current_spd_clos != data)
    return;

  /* Check current policy check */
  if (pi->third_party_policy != third_party_unknown)
    {
      /* Continue if this information was already known */
      spd_get_details (c);
    }
  else
    {
      /* Check the third party policy for the first time */
      check_third_party_policy (pi, spd_third_party_policy_check_reply, c);
    }
}

static void spd_third_party_policy_check_reply (package_info *pi, void *data)
{
  /* Continue only if valid data is stored in 'data' */
  if (current_spd_clos == data)
    spd_get_details (data);
}

void
spd_get_details (void *data)
{
  spd_clos *c = (spd_clos *)data;
  package_info *pi = c->pi;

  apt_worker_get_package_details (pi->name, (c->kind == remove_details
					     ? pi->installed_version
					     : pi->available_version),
				  c->kind, spd_get_details_reply, c);
}

static void
spd_get_details_reply (int cmd, apt_proto_decoder *dec, void *data)
{
  spd_clos *c = (spd_clos *)data;

  /* Just return when the reply is not about the current details dialog
     data, or when it's already showing the package details */
  if ((c == NULL) || (c != current_spd_clos) || c->showing_details)
    return;

  if ((dec == NULL) || (c->pi->have_detail_kind == c->kind))
    {
      spd_end (c);
      return;
    }

  g_free (c->pi->maintainer);
  g_free (c->pi->description);
  g_free (c->pi->dependencies);
  g_free (c->pi->repository);

  c->pi->maintainer = dec->decode_string_dup ();
  c->pi->description = dec->decode_string_dup ();
  nicify_description_in_place (c->pi->description);

  c->pi->dependencies = decode_dependencies (dec);
  c->pi->repository = dec->decode_string_dup ();

  if (!red_pill_mode || !red_pill_show_deps)
    {
      // Too much information can kill you.
      g_free (c->pi->dependencies);
      c->pi->dependencies = NULL;
    }

  decode_summary (dec, c->pi, c->kind);

  c->pi->have_detail_kind = c->kind;

  spd_with_details (c, true);
}

/* Creates the first page */
static GtkWidget *
spd_create_common_page (void *data)
{
  spd_clos *c = (spd_clos *)data;
  GtkWidget *table, *common, *pannable;

  /* Creates the table */
  table = gtk_table_new (9, 2, FALSE);
  gtk_table_set_col_spacings (GTK_TABLE (table), 10);
  gtk_table_set_row_spacings (GTK_TABLE (table), 0);
  c->table = table;

  common = gtk_vbox_new (TRUE, 0);
  pannable = GTK_WIDGET (
    g_object_new (HILDON_TYPE_PANNABLE_AREA,
                  "hscrollbar-policy", GTK_POLICY_AUTOMATIC,
                  "vscrollbar-policy", GTK_POLICY_AUTOMATIC,
                  "mov-mode", HILDON_MOVEMENT_MODE_BOTH,
                  NULL)
    );
  hildon_pannable_area_add_with_viewport (HILDON_PANNABLE_AREA (pannable),
                                          table);
  gtk_box_pack_start (GTK_BOX (common), pannable, TRUE, TRUE, 0);

  return common;
}

static void
spd_update_common_page (void *data)
{
  spd_clos *c = (spd_clos *)data;
  GtkWidget *table = c->table;
  package_info *pi = c->pi;
  const char *status;
  gint last_row = -1;

  if (pi->installed_version && pi->available_version)
    {
      if (pi->broken)
        {
          if (is_installable (pi))
            status = _("ai_va_details_status_broken_updateable");
          else
            status = _("ai_va_details_status_broken_not_updateable");
        }
      else
        {
          if (is_installable (pi))
            status = _("ai_va_details_status_updateable");
          else
            status = _("ai_va_details_status_not_updateable");
        }
    }
  else if (pi->installed_version)
    {
      if (pi->broken)
        status = _("ai_va_details_status_broken");
      else
        status = _("ai_va_details_status_installed");
    }
  else if (pi->available_version)
    {
      if (is_installable (pi))
        status = _("ai_va_details_status_installable");
      else
        status = _("ai_va_details_status_not_installable");
    }
  else
    status = "?";

  {
    const char *display_name =
      pi->get_display_name (pi->have_detail_kind == remove_details);

    if (red_pill_mode && strcmp (pi->name, display_name))
      {
        char *extended_name = g_strdup_printf ("%s (%s)", display_name,
                                               pi->name);
        add_table_field (table, ++last_row, _("ai_fi_details_package"),
                         extended_name);
        g_free (extended_name);
      }
    else
      add_table_field (table, ++last_row, _("ai_fi_details_package"),
                       display_name);
  }

  gchar *short_description = (pi->have_detail_kind == remove_details
      ? pi->installed_short_description
      : pi->available_short_description);
  if (short_description != NULL && strlen (short_description) > 0)
    add_table_field (table, ++last_row, "", short_description);

  add_table_field (table, ++last_row, _("ai_fi_details_maintainer"),
                   pi->maintainer);

  add_table_field (table, ++last_row, _("ai_fi_details_status"), status);

  const char *section_name =
    nicify_section_name (pi->have_detail_kind == remove_details
			 ? pi->installed_section
			 : pi->available_section);
  add_table_field (table, ++last_row, _("ai_fi_details_category"),
		   section_name? section_name : _("ai_category_other"));

  /* now, the optional fields */
  if (pi->installed_version)
    add_table_field (table, ++last_row, _("ai_va_details_installed_version"),
                     pi->installed_version);

  if (pi->installed_version && pi->installed_size > 0)
    {
      char size_buf[20];
      size_string_detailed (size_buf, 20, pi->installed_size);
      add_table_field (table, ++last_row, _("ai_va_details_size"), size_buf);
    }

  if (pi->available_version)
    {
      char size_buf[20];
      add_table_field (table, ++last_row, _("ai_va_details_available_version"),
                       pi->available_version);
      size_string_detailed (size_buf, 20, pi->info.download_size);
      add_table_field (table, ++last_row, _("ai_va_details_download_size"),
                       size_buf);
    }

  if (pi->repository)
    {
      /** id is not released yet XXX **/
      add_table_field (table, ++last_row, _("ai_va_details_catalogue"),
                       pi->repository);
    }

  gtk_widget_set_sensitive (GTK_WIDGET (table), TRUE);
}

static gint
get_screen_width (void)
{
  GdkScreen *screen = NULL;
  gint width = -1;

  screen = gdk_screen_get_default ();
  if (screen != NULL)
    width = gdk_screen_get_width (screen);

  return width;
}

static GtkWidget *
make_small_text_label (const char *text)
{
  gint scn_width = get_screen_width ();

  GtkWidget *summary_table = gtk_table_new (1, 1, FALSE);
  gtk_table_set_col_spacings (GTK_TABLE (summary_table), 10);
  gtk_table_set_row_spacings (GTK_TABLE (summary_table), 0);

  GtkWidget *desc = make_small_label (text);
  gtk_misc_set_alignment (GTK_MISC (desc), 0.0, 0.0);
  gtk_label_set_line_wrap (GTK_LABEL (desc), TRUE);
  gtk_label_set_line_wrap_mode (GTK_LABEL (desc), PANGO_WRAP_WORD);
  gtk_label_set_ellipsize (GTK_LABEL (desc), PANGO_ELLIPSIZE_NONE);

  /* Set GtkLabel width to 85% of screen width, to avoid weird wraps */
  if (scn_width != -1)
    gtk_widget_set_size_request (desc, scn_width * 0.85, -1);

  gtk_table_attach (GTK_TABLE (summary_table), desc, 0, 1, 0, 1,
		    GtkAttachOptions (GTK_EXPAND | GTK_FILL), GTK_FILL,
		    0, 0);

  GtkWidget *summary_tab = hildon_pannable_area_new ();
  hildon_pannable_area_add_with_viewport (HILDON_PANNABLE_AREA (summary_tab),
                                          summary_table);

  return summary_tab;
}

/* Creates the second page */
static GtkWidget *
spd_create_description_page (void *data)
{
  spd_clos *c = (spd_clos *)data;
  package_info *pi = c->pi;

  return make_small_text_label (pi->description);
}

/* Creates the third page */
static GtkWidget *
spd_create_summary_page (void *data)
{
  spd_clos *c = (spd_clos *)data;
  GtkWidget *summary_table, *summary_tab;
  package_info *pi = c->pi;

  bool possible = false;
  if (c->showing_details)
    {
      if (pi->have_detail_kind == remove_details)
	possible = (pi->info.removable_status == status_able);
      else
	possible = is_installable (pi);
    }

  summary_table = gtk_table_new (1, 2, FALSE);
  gtk_table_set_col_spacings (GTK_TABLE (summary_table), 10);
  gtk_table_set_row_spacings (GTK_TABLE (summary_table), 0);

  GtkWidget *header_label = make_small_label (pi->summary);
  gtk_misc_set_alignment (GTK_MISC (header_label), 0.0, 0.0);
  gtk_label_set_line_wrap (GTK_LABEL (header_label), FALSE);
  gtk_label_set_ellipsize (GTK_LABEL (header_label), PANGO_ELLIPSIZE_NONE);
  gtk_table_attach (GTK_TABLE (summary_table), header_label, 0, 2, 0, 1,
		    GtkAttachOptions (GTK_EXPAND | GTK_FILL), GTK_FILL,
		    0, 0);

  int r = 1;
  if (possible || red_pill_mode)
    {
      /* When there is exactly one package in all of the lists, we
	 show nothing for the summary since it would look stupid and
	 it is the common case.  However, we always show everything
	 when the package is broken.
      */

      int n_entries =
	(g_list_length (pi->summary_packages[sumtype_installing]) +
	 g_list_length (pi->summary_packages[sumtype_upgrading]) +
	 g_list_length (pi->summary_packages[sumtype_removing]));

      if (n_entries > 1 || pi->broken || red_pill_mode)
	{
	  r = add_table_list (summary_table, r,
			      _("ai_fi_details_packages_install"),
			      pi->summary_packages[sumtype_installing]);
	  r = add_table_list (summary_table, r,
			      _("ai_fi_details_packages_update"),
			      pi->summary_packages[sumtype_upgrading]);
	  r = add_table_list (summary_table, r,
			      _("ai_fi_details_packages_uninstall"),
			      pi->summary_packages[sumtype_removing]);
	}
    }
  if (!possible || red_pill_mode)
    {
      r = add_table_list (summary_table, r,
			  _("ai_fi_details_packages_missing"),
			  pi->summary_packages[sumtype_missing]);
      r = add_table_list (summary_table, r,
			  _("ai_fi_details_packages_conflicting"),
			  pi->summary_packages[sumtype_conflicting]);
      r = add_table_list (summary_table, r,
			  _("ai_fi_details_packages_needing"),
			  pi->summary_packages[sumtype_needed_by]);
    }

  summary_tab = hildon_pannable_area_new ();
  hildon_pannable_area_add_with_viewport (HILDON_PANNABLE_AREA (summary_tab),
                                          summary_table);

  return summary_tab;
}

/* Get the proper label for the third page */
static const gchar *
spd_get_summary_label (void *data)
{
  spd_clos *c = (spd_clos *)data;
  package_info *pi = c->pi;
  const gchar *summary_label;

  /* Don't return the 'Problems' label while the full details for the
     package have not been retrieved yet */
  if (pi->have_detail_kind == remove_details)
    summary_label = _("ai_ti_details_noteb_uninstalling");
  else if (c->showing_details && !is_installable (pi))
    summary_label = _("ai_ti_details_noteb_problems");
  else if (pi->installed_version && pi->available_version)
    summary_label = _("ai_ti_details_noteb_updating");
  else
    summary_label = _("ai_ti_details_noteb_installing");

  return summary_label;
}

/* Creates the fourth page */
static GtkWidget *
spd_create_deps_page (void *data)
{
  spd_clos *c = (spd_clos *)data;
  package_info *pi = c->pi;

  return make_small_text_label (pi->dependencies);
}

static void
spd_set_page_widget (void *data, gint page_number, GtkWidget *widget)
{
  g_return_if_fail (0 <= page_number && page_number < SPD_NUM_PAGES);

  spd_clos *c = (spd_clos *)data;
  GtkWidget **spd_nb_widgets = c->spd_nb_widgets;

  /* Just return if data is invalid */
  if (c == NULL || spd_nb_widgets == NULL)
    return;

  GList *children =
    gtk_container_get_children (GTK_CONTAINER (spd_nb_widgets[page_number]));

  guint n_children = g_list_length (children);

  if (n_children > 0)
    {
      g_assert (n_children == 1);
      gtk_container_remove (GTK_CONTAINER (spd_nb_widgets[page_number]),
			    GTK_WIDGET (children->data));
    }

  gtk_box_pack_start (GTK_BOX (spd_nb_widgets[page_number]),
		      widget, TRUE, TRUE, 0);

  g_list_free (children);
}

static bool
has_long_description (package_info *pi)
{
  bool result = false;

  if (pi && pi->description)
    {
      char *full_desc = g_strdup (pi->description);

      /* Ensure there no leading / trailing blanks */
      full_desc = g_strstrip (full_desc);

      /* Check non empty strings only */
      if (g_strcmp0 (full_desc, ""))
        {
          char **desc_lines = g_strsplit (full_desc, "\n", 2);
          if ((desc_lines[1] != NULL) && g_strcmp0 (desc_lines[1], ""))
            {
              /* At least more than one line was found */
              result = true;
            }
          g_strfreev (desc_lines);
        }
      g_free (full_desc);
    }

  return result;
}

static void
spd_with_details (void *data, bool filling_details)
{
  spd_clos *c = (spd_clos *)data;
  GtkWidget *dialog, *notebook;
  GtkWidget **spd_nb_widgets = NULL;
  package_info *pi = c->pi;

  /* Set this value to check whether the dialog is showing the full
     details for a package or not */
  c->showing_details = (pi->have_detail_kind == c->kind);

  if (!filling_details)
    {
      /* If it's not filling the details for a package it would mean
	 it's the first time this function is called, regardless the
	 full details are already available or not */
      dialog = gtk_dialog_new_with_buttons (_("ai_ti_details"),
					    NULL,
					    GTK_DIALOG_MODAL,
					    NULL);

      push_dialog (dialog);

      gtk_dialog_set_has_separator (GTK_DIALOG (dialog), FALSE);
      respond_on_escape (GTK_DIALOG (dialog), GTK_RESPONSE_OK);

      /* Create the notebook */
      notebook = gtk_notebook_new ();
      gtk_container_add (GTK_CONTAINER (GTK_DIALOG(dialog)->vbox), notebook);

      /* Init the global array of notebook pages */
      spd_nb_widgets = g_new (GtkWidget *, SPD_NUM_PAGES);

      /* Show the 'Updating' banner */
      show_updating ();

      /* Initialize the notebook pages before appending them */
      spd_nb_widgets[SPD_COMMON_PAGE] = spd_create_common_page (c);
      spd_nb_widgets[SPD_SUMMARY_PAGE] = gtk_vbox_new (TRUE, 0);

      /* Append the needed notebook pages */
      gtk_notebook_append_page (GTK_NOTEBOOK (notebook),
				spd_nb_widgets[SPD_COMMON_PAGE],
				gtk_label_new (_("ai_ti_details_noteb_common")));

      gtk_notebook_append_page (GTK_NOTEBOOK (notebook),
                                spd_nb_widgets[SPD_SUMMARY_PAGE],
                                gtk_label_new (spd_get_summary_label (c)));

      /* Save needed references */
      c->dialog = dialog;
      c->notebook = notebook;
      c->spd_nb_widgets = spd_nb_widgets;
    }
  else
    {
      /* It's the second time this function is called, so use the
	 previously stored widget references */
      dialog = c->dialog;
      notebook = c->notebook;
      spd_nb_widgets = c->spd_nb_widgets;

      if (c->showing_details)
        {
          /* Prevent the 'Updating' banner from being shown */
          prevent_updating ();

          /* Update the main common tab */
          spd_update_common_page (c);

          /* Set the content of the rest of the notebook pages */

          /* Check whether the 'description' tab should show up */
          if (has_long_description (pi))
            {
              /* Now create and insert the "Description" tab in its place */
              spd_nb_widgets[SPD_DESCRIPTION_PAGE] = spd_create_description_page (c);
              gtk_notebook_insert_page (GTK_NOTEBOOK (notebook),
                                        spd_nb_widgets[SPD_DESCRIPTION_PAGE],
                                        gtk_label_new (_("ai_ti_details_noteb_description")),
                                        SPD_DESCRIPTION_PAGE);
            }

          spd_set_page_widget (c, SPD_SUMMARY_PAGE,
                               spd_create_summary_page (c));

          /* Update 'summary' tab label */
          gtk_notebook_set_tab_label (GTK_NOTEBOOK (notebook),
                                      spd_nb_widgets[SPD_SUMMARY_PAGE],
                                      gtk_label_new (spd_get_summary_label (c)));

          if (pi->dependencies)
            {
              spd_nb_widgets[SPD_DEPS_PAGE] = spd_create_deps_page (c);
              gtk_notebook_append_page (GTK_NOTEBOOK (notebook),
                                        spd_nb_widgets[SPD_DEPS_PAGE],
                                        gtk_label_new (_("ai_ti_details_noteb_dependencies")));
            }
        }
    }

  g_signal_connect (dialog, "response",
		    G_CALLBACK (spd_response), c);

  gtk_widget_set_size_request (dialog, -1, 320);
  gtk_widget_show_all (dialog);

  if (c->show_problems && c->showing_details)
    gtk_notebook_set_current_page (GTK_NOTEBOOK (notebook),
				   SPD_SUMMARY_PAGE);
}

static void
spd_response (GtkDialog *dialog, gint response, gpointer data)
{
  spd_clos *c = (spd_clos *)data;

  pop_dialog (GTK_WIDGET (dialog));
  gtk_widget_destroy (GTK_WIDGET (dialog));

  spd_end (c);
}

static void
spd_end (void *data)
{
  spd_clos *c = (spd_clos *)data;

  c->pi->unref ();
  c->cont (c->data);
  g_free (c->spd_nb_widgets);

  current_spd_clos = NULL;
  hide_updating ();

  delete c;
}

/* Show package details as an interaction flow
 */

static void spdf_end (void *data);

void
show_package_details_flow (package_info *pi, detail_kind kind)
{
  if (start_interaction_flow ())
    show_package_details (pi, kind, false, spdf_end, NULL);
}

void
spdf_end (void *data)
{
  end_interaction_flow ();
}
