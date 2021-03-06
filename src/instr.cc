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

#include <ctype.h>
#include <string.h>
#include <libintl.h>

#include <glib.h>
#include <gtk/gtk.h>

#include "operations.h"
#include "settings.h"
#include "util.h"
#include "log.h"
#include "repo.h"
#include "xexp.h"
#include "confutils.h"
#include "main.h"

#define _(x) gettext (x)

/* Strings used for parsing available translations */
#define LOC_BEGIN "repo_name["
#define LOC_BEGIN_SIZE 10
#define LOC_END "]"

static void
annoy_user_with_gerror (const char *filename, GError *error,
			void (*cont) (void *), void *data)
{
  add_log ("%s: %s\n", filename, error->message);
  annoy_user (_("ai_ni_operation_failed"), cont, data);
}

static xexp *
convert_locale_string (const char *xname,
		       GKeyFile *keyfile,
		       const char *group, const char *name)
{
  gsize name_len = strlen (name);
  gchar *def = NULL;
  xexp *value = xexp_list_new (xname);

  gsize n_keys;
  gchar **keys;

  keys = g_key_file_get_keys (keyfile, group, &n_keys, NULL);
  if (keys == NULL)
    {
      xexp_free (value);
      return NULL;
    }

  for (gsize i = 0; i < n_keys; i++)
    {
      if (g_str_has_prefix (keys[i], name))
	{
	  if (keys[i][name_len] == '[')
	    {
	      gchar *locale = g_strdup (keys[i]+name_len+1);
	      gchar *locale_end = strchr (locale, ']');
	      if (locale_end)
		*locale_end = '\0';
	      gchar *val = g_key_file_get_locale_string (keyfile, group,
							 name, locale, NULL);
	      xexp_cons (value, xexp_text_new (locale, val));
	      g_free (locale);
	      g_free (val);
	    }
	  else if (def == NULL)
	    def = g_key_file_get_string (keyfile, group, name, NULL);
	}
    }
  xexp_reverse (value);

  g_strfreev (keys);

  if (def && xexp_is_empty (value))
    {
      xexp_free (value);
      value = xexp_text_new (xname, def);
      g_free (def);
      return value;
    }
  
  if (def)
    {
      xexp_cons (value, xexp_text_new ("default", def));
      g_free (def);
    }

  return value;
}

static xexp *
convert_catalogue (GKeyFile *keyfile, const char *group,
		   const char *file_uri_base)
{
  gchar *val;
  gchar *uri;

  if (!g_key_file_has_group (keyfile, group))
    {
      add_log ("Catalogue '%s' not found\n", group);
      return NULL;
    }

  val = g_key_file_get_string (keyfile, group, "filter_dist", NULL);
  bool applicable = val == NULL || !strcmp (val, default_distribution);
  g_free (val);

  if (!applicable)
    return NULL;

  xexp *cat = xexp_list_new ("catalogue");

  xexp *name = convert_locale_string ("name", keyfile, group, "name");
  if (name)
    xexp_aset (cat, name);

  if (file_uri_base == NULL)
    {
      uri = g_key_file_get_string (keyfile, group, "uri", NULL);
      gchar *id = g_key_file_get_string (keyfile, group, "id", NULL);
      gchar *file = g_key_file_get_string (keyfile, group, "file", NULL);

      if (uri == NULL && (id != NULL && file != NULL))
	{
          xexp_aset_text (cat, "id", id);
          g_free (id);
          xexp_aset_text (cat, "file", file);
          g_free (file);
        }
      else if (uri != NULL && (id == NULL && file == NULL))
        {
          xexp_aset_text (cat, "uri", uri);
        }
      else
        {
          g_free (id);
          g_free (file);
          g_free (uri);

          add_log ("Catalogue must have 'uri' key or 'id' and 'file' key: %s\n",
                   group);
          xexp_free (cat);
          return NULL;
        }
    }
  else
    {
      uri = g_key_file_get_string (keyfile, group, "file_uri", NULL);
      if (uri == NULL)
	{
	  add_log ("Catalogue must have 'file_uri' key: %s\n", group);
	  xexp_free (cat);
	  return NULL;
	}

      char *full_uri = g_strdup_printf ("file://%s/%s", file_uri_base, val);
      xexp_aset_text (cat, "uri", full_uri);
      g_free (full_uri);
    }

  if (uri)
    {
      val = g_key_file_get_string (keyfile, group, "dist", NULL);
      xexp_aset_text (cat, "dist", val);
      g_free (val);

      val = g_key_file_get_string (keyfile, group, "components", NULL);
      xexp_aset_text (cat, "components", val);
      g_free (val);

      g_free (uri);
    }

  xexp_reverse (cat);
  return cat;
}

static xexp *
convert_catalogues (GKeyFile *keyfile, const char *group, const char *key,
		    const char *file_uri_base)
{
  gchar **catalogue_names;
  gsize n_catalogue_names;

  catalogue_names = g_key_file_get_string_list (keyfile, group, key,
						&n_catalogue_names, NULL);
  if (catalogue_names == NULL)
    return NULL;

  xexp *catalogues = xexp_list_new ("catalogues");
  for (gsize i = 0; i < n_catalogue_names; i++)
    {
      g_strchug (catalogue_names[i]);
      xexp *cat = convert_catalogue (keyfile, catalogue_names[i],
				     file_uri_base);
      if (cat)
	xexp_cons (catalogues, cat);
    }
  xexp_reverse (catalogues);

  g_strfreev (catalogue_names);

  return catalogues;
}

static bool
parse_quoted_word (char **start, char **end, bool term)
{
  char *ptr = *start;

  while (isspace (*ptr))
    ptr++;

  *start = ptr;
  *end = ptr;

  if (*ptr == 0)
    return false;

  // Jump to the next word, handling double quotes and brackets.

  while (*ptr && !isspace (*ptr))
   {
     if (*ptr == '"')
      {
	for (ptr++; *ptr && *ptr != '"'; ptr++);
	if (*ptr == 0)
	  return false;
      }
     if (*ptr == '[')
      {
	for (ptr++; *ptr && *ptr != ']'; ptr++);
	if (*ptr == 0)
	  return false;
      }
     ptr++;
   }

  if (term)
    {
      if (*ptr)
	*ptr++ = '\0';
    }
  
  *end = ptr;
  return true;
}

static xexp *
convert_compatibility_catalogue (const char *deb_line, const char *name)
{
  char *start, *end;

  xexp *x = xexp_list_new ("catalogue");

  start = (char *)deb_line;
  parse_quoted_word (&start, &end, false);
  if (end - start != 3 || strncmp (start, "deb", 3))
    {
    error:
      add_log ("Unrecognized repository format: '%s'\n", deb_line);
      xexp_free (x);
      return NULL;
    }

  start = end;
  parse_quoted_word (&start, &end, false);
  if (start)
    xexp_cons (x, xexp_text_newn ("uri", start, end - start));
  else
    goto error;

  start = end;
  parse_quoted_word (&start, &end, false);
  if (start)
    xexp_cons (x, xexp_text_newn ("dist", start, end - start));
  else
    goto error;

  start = end;
  parse_quoted_word (&start, &end, false);
  if (start)
    xexp_cons (x, xexp_text_new ("components", start));
  else
    goto error;

  if (name)
    xexp_cons (x, xexp_text_new ("name", name));

  xexp_reverse (x);
  return x;
}

static xexp *
convert_compatibility_catalogues (GKeyFile *keyfile, const char *group)
{
  gchar *repo_name = g_key_file_get_string (keyfile, group, "repo_name", NULL);
  gchar *repo_deb = g_key_file_get_string (keyfile, group, "repo_deb", NULL);
  gchar *repo_deb_3 = g_key_file_get_string (keyfile, group, "repo_deb_3",
					     NULL);
  xexp *x = NULL;

  if (repo_deb || repo_deb_3)
    {
      /* We have at least one catalogue so we return a list of them.
	 When both are filtered out, the list will be empty and that
	 is the signal that the .install file is incompatible.
      */

      x = xexp_list_new ("catalogues");

      if (!strcmp (default_distribution, "bora") && repo_deb_3)
	{
	  xexp *c = convert_compatibility_catalogue (repo_deb_3, repo_name);
	  if (c)
	    xexp_cons (x, c);
	}

      if (!strcmp (default_distribution, "mistral") && repo_deb)
	{
	  xexp *c = convert_compatibility_catalogue (repo_deb, repo_name);
	  if (c)
	    xexp_cons (x, c);
	}
    }

  g_free (repo_name);
  g_free (repo_deb);
  g_free (repo_deb_3);

  return x;
}

struct add_catalogues_closure {
  xexp *catalogues;
  void (*cont) (void *);
  void *data;
};

static void
add_catalogues_done (bool res, void *data)
{
  add_catalogues_closure *c = (add_catalogues_closure *)data;

  xexp_free (c->catalogues);
  c->cont (c->data);
}

static void
execute_add_catalogues (GKeyFile *keyfile, const char *entry,
			void (*cont) (void *data), void *data)
{
  xexp *catalogues = convert_catalogues (keyfile, entry, "catalogues", NULL);
  
  if (catalogues == NULL)
    {
      annoy_user (_("ai_ni_operation_failed"), cont, data);
      return;
    }

  if (xexp_is_empty (catalogues))
    {
      /* All catalogues were filtered out (or the list was empty
	 to begin with, which we treat the same).  That means that
	 this installation script was not for us.

	 XXX - We don't have a good name to use in this case, so we
	       just use "".
      */
      char *msg = g_strdup_printf (_("ai_ni_error_install_incompatible"),
				   "");
      annoy_user (msg, cont, data);
      xexp_free (catalogues);
      g_free (msg);
      return;
    }
    
  add_catalogues_closure *c = new add_catalogues_closure;
  c->catalogues = catalogues;
  c->cont = cont;
  c->data = data;

  add_catalogues (catalogues, true, false, add_catalogues_done, c);
}

struct eip_clos {
  xexp *catalogues;
  gchar *package;
  void (*cont) (void *data);
  void *data;
};

static void eip_with_catalogues (bool res, void *data);
static void eip_unsuccessful (void *data);
static void eip_end (int n_successful, void *data);

static void
execute_install_package (GKeyFile *keyfile, const char *entry,
			 void (*cont) (void *data), void *data)
{
  gchar *package = g_key_file_get_string (keyfile, entry, "package", NULL);
  xexp *catalogues = convert_catalogues (keyfile, entry, "catalogues", NULL);
  xexp *comp_catalogues = convert_compatibility_catalogues (keyfile, entry);

  eip_clos *c = new eip_clos;
  c->catalogues = catalogues;
  c->package = g_strstrip (package);
  c->cont = cont;
  c->data = data;

  if (catalogues == NULL)
    catalogues = comp_catalogues;
  else if (comp_catalogues)
    xexp_append (catalogues, comp_catalogues);

  if (catalogues)
    {
      if (xexp_is_empty (catalogues))
	{
	  /* All catalogues were filtered out (or the list was empty
	     to begin with, which we treat the same).  That means that
	     this installation script was not for us.
	  */
	  char *msg = g_strdup_printf (_("ai_ni_error_install_incompatible"),
				       package);
	  annoy_user (msg, eip_unsuccessful, c);
	  g_free (msg);
	  return;
	}

      add_catalogues (catalogues, true, c->package != NULL,
		      eip_with_catalogues, c);
    }
  else
    eip_with_catalogues (true, c);
}

static void
eip_with_catalogues (bool res, void *data)
{
  eip_clos *c = (eip_clos *)data;

  if (res && c->package)
    install_named_package (c->package, eip_end, c);
  else
    eip_end (0, c);
}

static void
eip_unsuccessful (void *data)
{
  eip_clos *c = (eip_clos *)data;

  eip_end (0, c);
}

static void
eip_end (int n_successful, void *data)
{
  eip_clos *c = (eip_clos *)data;

  c->cont (c->data);

  if (c->catalogues)
    {
      xexp_free (c->catalogues);

      /* When repository configuration successfully installed,
       * we should update catalogues in order to get proper
       * gpgv results (*.gpg.info) */
      if (n_successful > 0)
        maybe_refresh_package_cache_without_user ();
    }
  g_free (c->package);
  delete c;
}

struct eci_clos {
  xexp *card_catalogues;
  xexp *perm_catalogues;
  gchar **packages;
  bool automatic;
  void (*cont) (void *);
  void *data;
};

static void eci_with_temp_catalogues (bool res, void *data);
static void eci_reply (int n_successful, void *data);

void
execute_card_install (GKeyFile *keyfile, const char *entry,
		      const char *filename,
		      void (*cont) (void *data), void *data)

{
  if (filename[0] != '/')
    {
      add_log ("card-install filename must be absolute\n");
      what_the_fock_p ();
      cont (data);
      return;
    }

  gchar *dirname = g_path_get_dirname (filename);
  gchar *esc_dirname = g_uri_escape_string (dirname, NULL, true);
  xexp *card_catalogues = convert_catalogues (keyfile, entry,
					      "card_catalogues", esc_dirname);
  xexp *perm_catalogues = convert_catalogues (keyfile, entry,
					      "permanent_catalogues", NULL);
  gchar **packages = g_key_file_get_string_list (keyfile, entry, "packages",
						 NULL, NULL);
  g_free (dirname);
  g_free (esc_dirname);

  if (card_catalogues == NULL
      || xexp_is_empty (card_catalogues))
    {
      add_log ("Must specify non-empty 'card_catalogues' key\n");
    error:
      xexp_free (card_catalogues);
      xexp_free (perm_catalogues);
      g_strfreev (packages);
      what_the_fock_p ();
      cont (data);
      return;
    }

  if (packages == NULL || packages[0] == NULL)
    {
      add_log ("Must specify non-empty 'packages' key\n");
      goto error;
    }

  eci_clos *c = new eci_clos;
  c->card_catalogues = card_catalogues;
  c->perm_catalogues = perm_catalogues;
  c->packages = packages;
  c->automatic = g_str_has_suffix (filename, "/.auto.install");
  c->cont = cont;
  c->data = data;

  add_temp_catalogues_and_refresh (card_catalogues,
                                   _("ai_nw_preparing_installation"),
                                   eci_with_temp_catalogues, c);
}

static void
eci_with_temp_catalogues (bool res, void *data)
{
  struct eci_clos *c = (eci_clos *)data;

#ifdef INSTALL_TYPE_MEMORY_CARD
  if (res)
    install_named_packages ((const char **)c->packages, 
                            INSTALL_TYPE_MEMORY_CARD, c->automatic,
                            NULL, NULL, eci_reply, c);
  else
#endif
    eci_reply (0, c);
}

static void
eci_end (void *data)
{
  struct eci_clos *c = (eci_clos *)data;
  
  c->cont (c->data);

  xexp_free (c->card_catalogues);
  xexp_free (c->perm_catalogues);
  g_strfreev (c->packages);
  delete c;

}

static void
eci_reply (int n_successful, void *data)
{
  rm_temp_catalogues (eci_end, data);
}


void
open_local_install_instructions (const char *filename,
				 void (*cont) (void *data),
				 void *data)
{
  GError *error = NULL;

  GKeyFile *keyfile = g_key_file_new ();
  if (!g_key_file_load_from_file (keyfile, filename,
				  GKeyFileFlags(G_KEY_FILE_KEEP_TRANSLATIONS),
				  &error))
    {
      g_key_file_free (keyfile);
      annoy_user_with_gerror (filename, error, cont, data);
      g_error_free (error);
      return;
    }

  if (g_key_file_has_group (keyfile, "install"))
    execute_install_package (keyfile, "install",
			     cont, data);
  else if (g_key_file_has_group (keyfile, "catalogues"))
    execute_add_catalogues (keyfile, "catalogues",
			    cont, data);
// FIXME: the memory card installation is dropped in Fremantle
//        but won't delete the code, if the community wants to
//        reenable it.
//  else if (g_key_file_has_group (keyfile, "card_install"))
//    execute_card_install (keyfile, "card_install", filename,
//			  cont, data);
  else
    {
      add_log ("Unrecognized .install file variant\n");
      annoy_user (_("ai_ni_operation_failed"), cont, data);
    }

  g_key_file_free (keyfile);
}
