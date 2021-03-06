/*
 * This file is part of the hildon-application-manager.
 *
 * Copyright (C) 2007, 2008 Nokia Corporation.  All Rights reserved.
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

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>

#include "xexp.h"

struct xexp {
  char *tag;
  xexp *rest;
  xexp *first;
  char *text;
};

xexp *
xexp_rest (xexp *x)
{
  g_return_val_if_fail (x != NULL, NULL);

  return x->rest;
}

void
xexp_free (xexp *x)
{
  if (x == NULL)
    return;

  g_return_if_fail (x->rest == NULL);

  xexp *c = x->first;
  while (c)
    {
      xexp *r = c->rest;
      c->rest = NULL;
      xexp_free (c);
      c = r;
    }
  g_free (x->tag);
  g_free (x->text);
  g_free (x);
}

xexp *
xexp_copy (xexp *x)
{
  xexp *y, *z, **zptr;

  if (x == NULL)
    return NULL;

  y = g_new0 (xexp, 1);
  y->tag = g_strdup (x->tag);
  y->text = g_strdup (x->text);
  
  for (z = x->first, zptr = &y->first;
       z;
       z = z->rest, zptr = &(*zptr)->rest)
    *zptr = xexp_copy (z);

  return y;
}

const char *
xexp_tag (xexp *x)
{
  return x->tag;
}

int
xexp_is (xexp *x, const char *tag)
{
  return strcmp (x->tag, tag) == 0;
}

int
xexp_is_empty (xexp *x)
{
  return x->first == NULL && x->text == NULL;
}

xexp *
xexp_list_new (const char *tag)
{
  g_assert (tag);

  xexp *x = g_new0 (xexp, 1);
  x->tag = g_strdup (tag);
  return x;
}

xexp *
xexp_list_sort (xexp *x,
		int (*xexp_compare_func) (xexp *x1, xexp *x2))
{
  /* If it's not a list, return NULL */
  if (!xexp_is_list (x))
    return NULL;

  /* Build a GSList to sort it */
  GSList *x_slist = NULL;
  xexp *x_item = NULL;

  for (x_item = x->first; x_item; x_item = x_item->rest)
    x_slist = g_slist_prepend (x_slist, x_item);

  x_slist = g_slist_reverse (x_slist);

  if (x_slist)
    {
      GSList *l_item = NULL;
      xexp *x_item1 = NULL;
      xexp *x_item2 = NULL;

      /* Sort the list */
      x_slist = g_slist_sort (x_slist,
			      (gint (*)(gconstpointer, gconstpointer)) xexp_compare_func);

      /* Rebuild the xexp expression from the sorted list */

      /* Get first element and update root xexp element to point to it */
      x_item1 = (xexp *)x_slist->data;
      x->first = x_item1;

      /* Reassign 'rest' links with new order */
      for (l_item = g_slist_next (x_slist);
	   l_item;
	   l_item = g_slist_next (l_item))
	{
	  x_item2 = (xexp *)l_item->data;
	  x_item1->rest = x_item2;
	  x_item1 = x_item2;
	}

      /* Finish the list */
      x_item1->rest = NULL;

      g_slist_free (x_slist);
    }

  /* Return sorted xexp expression */
  return x;
}

xexp *
xexp_list_filter (xexp *x, int (*xexp_filter_func) (xexp *x))
{
  /* If it's not a list, return NULL */
  if (!xexp_is_list (x))
    return NULL;

  xexp *filtered_xexp = NULL;
  xexp *last_inserted_item = NULL;
  xexp *x_item = NULL;

  /* Create a new list */
  filtered_xexp = xexp_list_new (x->tag);

  /* Fill the list */
  for (x_item = x->first; x_item; x_item = x_item->rest)
    {
      if (xexp_filter_func (x_item))
	{
	  xexp *valid_item = xexp_copy (x_item);

	  /* Check if it's the first item or not */
	  if (last_inserted_item == NULL)
	    filtered_xexp->first = valid_item;
	  else
	    last_inserted_item->rest = valid_item;

	  last_inserted_item = valid_item;
	}
    }

  /* Finish the list */
  if (last_inserted_item)
    last_inserted_item->rest = NULL;

  return filtered_xexp;
}

xexp *
xexp_list_map (xexp *x, xexp * (*xexp_map_func) (xexp *x))
{
  /* If it's not a list, return NULL */
  if (!xexp_is_list (x))
    return NULL;

  xexp *mapped_xexp = NULL;
  xexp *last_inserted_item = NULL;
  xexp *x_item = NULL;

  /* Create a new list */
  mapped_xexp = xexp_list_new (x->tag);

  /* Fill the list */
  for (x_item = x->first; x_item; x_item = x_item->rest)
    {
      xexp *mapped_item = xexp_map_func (x_item);

      if (mapped_item)
	{
	  /* Check if it's the first item or not */
	  if (last_inserted_item == NULL)
	    mapped_xexp->first = mapped_item;
	  else
	    last_inserted_item->rest = mapped_item;

	  last_inserted_item = mapped_item;
	}
    }

  /* Finish the list */
  if (last_inserted_item)
    last_inserted_item->rest = NULL;

  return mapped_xexp;
}

int
xexp_is_list (xexp *x)
{
  g_return_val_if_fail (x != NULL, FALSE);

  return x->text == NULL;
}

xexp *
xexp_first (xexp *x)
{
  g_return_val_if_fail (xexp_is_list (x), NULL);
  return x->first;
}

int
xexp_length (xexp *x)
{
  int len;
  xexp *y;

  g_return_val_if_fail (xexp_is_list (x), 0);
  for (len = 0, y = xexp_first (x); y; len++, y = xexp_rest (y))
    ;
  return len;
}

void
xexp_cons (xexp *x, xexp *y)
{
  g_return_if_fail (xexp_is_list (x));
  g_return_if_fail (xexp_rest (y) == NULL);
  y->rest = x->first;
  x->first = y;
}

void
xexp_append_1 (xexp *x, xexp *y)
{
  xexp **yptr;

  g_return_if_fail (xexp_is_list (x));
  g_return_if_fail (xexp_rest (y) == NULL);

  for (yptr = &x->first; *yptr; yptr = &(*yptr)->rest)
    ;
  *yptr = y;
}

void
xexp_append (xexp *x, xexp *y)
{
  xexp **yptr;

  g_return_if_fail (xexp_is_list (x));
  g_return_if_fail (xexp_is_list (y));
  g_return_if_fail (xexp_rest (y) == NULL);

  for (yptr = &x->first; *yptr; yptr = &(*yptr)->rest)
    ;
  *yptr = y->first;
  y->first = NULL;
  xexp_free (y);
}

void
xexp_reverse (xexp *x)
{
  xexp *y, *f;

  g_return_if_fail (xexp_is_list (x));

  y = x->first;
  f = NULL;
  while (y)
    {
      xexp *r = y->rest;
      y->rest = f;
      f = y;
      y = r;
    }
  x->first = f;
}

void
xexp_del (xexp *x, xexp *z)
{
  xexp **yptr;

  g_return_if_fail (xexp_is_list (x));
  yptr = &x->first;
  while (*yptr)
    {
      xexp *y = *yptr;
      if (y == z)
	{
	  *yptr = y->rest;
	  y->rest = NULL;
	  xexp_free (y);
	  return;
	}
      yptr = &(*yptr)->rest;
    }
  g_return_if_reached ();
}

xexp *
xexp_pop (xexp *x)
{
  xexp *y;

  g_return_val_if_fail (xexp_is_list (x), NULL);

  y = x->first;
  if (y)
    {
      x->first = y->rest;
      y->rest = NULL;
    }
  return y;
}

xexp *
xexp_text_new (const char *tag, const char *text)
{
  g_assert (tag);
  g_assert (text);

  xexp *x = g_new0 (xexp, 1);
  x->tag = g_strdup (tag);
  if (*text)
    x->text = g_strdup (text);
  return x;
}

xexp *
xexp_text_newn (const char *tag, const char *text, int len)
{
  g_assert (tag);
  g_assert (text);

  xexp *x = g_new0 (xexp, 1);
  x->tag = g_strdup (tag);
  if (*text)
    x->text = g_strndup (text, len);
  return x;
}

int
xexp_is_text (xexp *x)
{
  g_return_val_if_fail (x != NULL, FALSE);

  return x->first == NULL;
}

const char *
xexp_text (xexp *x)
{
  g_return_val_if_fail (xexp_is_text (x), NULL);
  if (x->text)
    return x->text;
  else
    return "";
}

int
xexp_text_as_int (xexp *x)
{
  return atoi (xexp_text (x));
}

/* Association lists
 */

xexp *
xexp_aref (xexp *x, const char *tag)
{
  if (xexp_is_list (x))
    {
      xexp *y = xexp_first (x);
      while (y)
	{
	  if (xexp_is (y, tag))
	    return y;
	  y = xexp_rest (y);
	}
    }
  return NULL;
}

xexp *
xexp_aref_rest (xexp *x, const char *tag)
{
  xexp *y = xexp_rest (x);
  while (y)
    {
      if (xexp_is (y, tag))
	return y;
      y = xexp_rest (y);
    }
  return NULL;
}

const char *
xexp_aref_text (xexp *x, const char *tag)
{
  xexp *y = xexp_aref (x, tag);
  if (y)
    return xexp_text (y);
  else
    return NULL;
}

int
xexp_aref_bool (xexp *x, const char *tag)
{
  return xexp_aref (x, tag) != NULL;
}

int
xexp_aref_int (xexp *x, const char *tag, int def)
{
  const char *val = xexp_aref_text (x, tag);
  if (val)
    return atoi (val);
  else
    return def;
}

void
xexp_aset (xexp *x, xexp *val)
{
  xexp_adel (x, xexp_tag (val));
  xexp_cons (x, val);
}

void
xexp_aset_bool (xexp *x, const char *tag, int val)
{
  if (val)
    xexp_aset (x, xexp_list_new (tag));
  else
    xexp_adel (x, tag);
}

void
xexp_aset_text (xexp *x, const char *tag, const char *val)
{
  if (val)
    xexp_aset (x, xexp_text_new (tag, val));
  else
    xexp_adel (x, tag);
}

void
xexp_aset_int (xexp *x, const char *tag, int val)
{
  char *t = g_strdup_printf ("%d", val);
  xexp_aset_text (x, tag, t);
  g_free (t);
}

void
xexp_adel (xexp *x, const char *tag)
{
  xexp **yptr;

  g_return_if_fail (xexp_is_list (x));
  yptr = &x->first;
  while (*yptr)
    {
      xexp *y = *yptr;
      if (xexp_is (y, tag))
	{
	  *yptr = y->rest;
	  y->rest = NULL;
	  xexp_free (y);
	}
      else
	yptr = &(*yptr)->rest;
    }
}

static void
transmogrify_text_to_empty (xexp *x)
{
  g_free (x->text);
  x->text = NULL;
}

static void
transmogrify_empty_to_text (xexp *x, const char *text)
{
  g_assert (text && *text);
  x->text = g_strdup (text);
}

/** Parsing */

typedef struct {
  xexp *result;
  GSList *stack;
} xexp_parse_context;

static void
ignore_text (const char *text, GError **error)
{
  const char *p = text;
  while (*p && isspace (*p))
    p++;

  if (*p)
    g_set_error (error, 
		 G_MARKUP_ERROR,
		 G_MARKUP_ERROR_INVALID_CONTENT,
		 "Unexpected text: %s", text);
}

static void
xexp_parser_start_element (GMarkupParseContext *context,
			   const gchar         *element_name,
			   const gchar        **attribute_names,
			   const gchar        **attribute_values,
			   gpointer             user_data,
			   GError             **error)
{
  xexp_parse_context *xp = (xexp_parse_context *)user_data;

  xexp *x = xexp_list_new (element_name);
  if (xp->stack)
    {
      /* If the current node is a text, it must be all whitespace and
	 we turn it into a empty node.
      */
      xexp *y = (xexp *)xp->stack->data;
      if (xexp_is_text (y))
	{
	  ignore_text (xexp_text (y), error);
	  transmogrify_text_to_empty (y);
	}

      xexp_cons (y, x);
    }
  xp->stack = g_slist_prepend (xp->stack, x);
}

static void
xexp_parser_end_element (GMarkupParseContext *context,
			 const gchar         *element_name,
			 gpointer             user_data,
			 GError             **error)
{
  xexp_parse_context *xp = (xexp_parse_context *)user_data;

  GSList *top = xp->stack;
  xexp *x = (xexp *)top->data;
  xp->stack = top->next;
  g_slist_free_1 (top);

  if (xexp_is_list (x))
    xexp_reverse (x);

  if (xp->stack == NULL)
    xp->result = x;
}

static void
xexp_parser_text (GMarkupParseContext *context,
		  const gchar         *text,
		  gsize                text_len,
		  gpointer             user_data,
		  GError             **error)
{
  xexp_parse_context *xp = (xexp_parse_context *)user_data;

  /* If the current xexp is not empty, TEXT must be all whitespace and
     we ignore it.  Otherwise, we turn the current node into text (if
     it is not the empty string).
  */
  
  if (xp->stack)
    {
      xexp *y = (xexp *)xp->stack->data;
      if (!xexp_is_empty (y))
	ignore_text (text, error);
      else if (text && *text)
	transmogrify_empty_to_text (y, text);
    }
}

static GMarkupParser xexp_markup_parser = {
  xexp_parser_start_element,
  xexp_parser_end_element,
  xexp_parser_text,
  NULL,
  NULL
};

static xexp *
xexp_read_1 (FILE *f, GError **error, int fuzzy)
{
  xexp_parse_context xp;
  GMarkupParseContext *ctxt;
  gchar buf[1024];
  size_t buf_size = fuzzy? sizeof (buf) : 1;
  size_t n;
  gboolean parse_failed = FALSE;

  if (f == NULL)
    return NULL;

  xp.stack = NULL;
  xp.result = NULL;
  ctxt = g_markup_parse_context_new (&xexp_markup_parser, 0, &xp, NULL);

  while (xp.result == NULL && (n = fread (buf, 1, buf_size, f)) > 0)
    {
      if (!g_markup_parse_context_parse (ctxt, buf, n, error))
	{
	  parse_failed = TRUE;
	  break;
	}
    }

  if (!g_markup_parse_context_end_parse (ctxt, error))
    parse_failed = TRUE;

  g_markup_parse_context_free (ctxt);

  if (!parse_failed)
    {
      g_assert (xp.result && xp.stack == NULL);
      return xp.result;
    }

  /* An error while parsing has ocurred */
  if (xp.stack)
    {
      xexp *top = (xexp *) g_slist_last (xp.stack)->data;
      xexp_free (top);
      g_slist_free (xp.stack);
    }
  else if (xp.result)
    xexp_free (xp.result);

  return NULL;
}

xexp *
xexp_read (FILE *f, GError **error)
{
  if (f == NULL)
    return NULL;
  else
    return xexp_read_1 (f, error, FALSE);
}

xexp *
xexp_read_file (const char *filename)
{
  FILE *f = fopen (filename, "r");
  if (f != NULL)
    {
      GError *error = NULL;
      xexp *x = xexp_read_1 (f, &error, TRUE);
      fclose (f);
      if (error)
	{
	  fprintf (stderr, "%s: %s\n", filename, error->message);
	  g_error_free (error);
	}
      return x;
    }
  fprintf (stderr, "%s: %s\n", filename, strerror (errno));
  return NULL;
}

/** Writing */

static void
xexp_fprintf_escaped (FILE *f, const char *fmt, ...)
{
  va_list args;
  char *str;

  if (f == NULL) return;

  va_start (args, fmt);
  str = g_markup_vprintf_escaped (fmt, args);
  va_end (args);
  fputs (str, f);
  g_free (str);
}

static void
write_blanks (FILE *f, int level)
{
  static const char blanks[] = "                ";
  if (f == NULL) return;

  if (level > sizeof(blanks)-1)
    level = sizeof(blanks)-1;
  fputs (blanks + sizeof(blanks)-1 - level, f);
}

static void
xexp_write_1 (FILE *f, xexp *x, int level)
{
  xexp *y;

  if (f == NULL) return;

  write_blanks (f, level);

  if (xexp_is_empty (x))
    xexp_fprintf_escaped (f, "<%s/>\n", xexp_tag (x));
  else if (xexp_is_list (x))
    {
      y = xexp_first (x);
      xexp_fprintf_escaped (f, "<%s>\n", xexp_tag (x));
      for (; y; y = xexp_rest (y))
	xexp_write_1 (f, y, level + 1);
      write_blanks (f, level);
      xexp_fprintf_escaped (f, "</%s>\n", xexp_tag (x));
    }
  else
    xexp_fprintf_escaped (f, "<%s>%s</%s>\n",
			  xexp_tag (x), xexp_text (x), xexp_tag (x));
}

void
xexp_write (FILE *f, xexp *x)
{
  if (f == NULL)
    return;
  else
    xexp_write_1 (f, x, 0);
}

int
xexp_write_file (const char *filename, xexp *x)
{
  char *tmp_filename = g_strdup_printf ("%s#%d", filename, getpid());
  FILE *f = fopen (tmp_filename, "w");

  if (f == NULL)
    goto error;

  xexp_write (f, x);

  if ((ferror (f) | fflush (f) | fsync (fileno (f)) | fclose (f))
      || (rename (tmp_filename, filename) < 0))
    {
      f = NULL;
      goto error;
    }

  g_free (tmp_filename);
  return 1;

 error:
  fprintf (stderr, "%s: %s\n", filename, strerror (errno));
  if (f)
    fclose (f);
  if (tmp_filename != filename)
    g_free (tmp_filename);
  return 0;
}
