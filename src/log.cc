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

#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <libintl.h>
#include <sys/stat.h>

#include <gtk/gtk.h>

#include "log.h"
#include "util.h"
#include "main.h"
#include "settings.h"

#define _(x) gettext (x)

static GString *log_text = NULL;
static gchar *last_save_log_dir = NULL;

enum {
  RESPONSE_SAVE = 1,
  RESPONSE_CLEAR = 2
};

static void
save_log_do_nothing (void *data)
{
}

static void
save_log_cont (bool res, void *data)
{
  char *uri = (char *)data;
  bool success = res;

  if (res)
  {
    GFile *file;
    GFileOutputStream *stream;
    gsize bytes;
    GError *error = NULL;

    file = g_file_new_for_uri (uri);
    stream = g_file_replace (file, NULL, TRUE, G_FILE_CREATE_NONE, NULL, &error);

    if (error != NULL)
    {
      annoy_user_with_g_error (error, uri, save_log_do_nothing, NULL);
      g_error_free (error);
      success = false;
    }
    else
    {
      if (log_text)
      {
        if (!g_output_stream_write_all (G_OUTPUT_STREAM(stream),
                                        log_text->str,
                                        log_text->len,
                                        &bytes,
                                        NULL,
                                        &error))
        {
          annoy_user_with_g_error (error, uri, save_log_do_nothing, NULL);
          g_error_free (error);
          success = false;
        }

        if (!g_output_stream_close (G_OUTPUT_STREAM(stream), NULL, &error))
        {
          annoy_user_with_g_error (error, uri, save_log_do_nothing, NULL);
          g_error_free (error);
          success = false;
        }
      }
    }

    g_object_unref (stream);
    g_object_unref (file);
  }

  /* Store the last path used for saving the log */
  if (last_save_log_dir != NULL)
    g_free (last_save_log_dir);

  if (success)
    {
      gchar *tmp_dir = NULL;

      /* Remove the file at the end and just get the dir path */
      tmp_dir = g_path_get_dirname (uri);
      last_save_log_dir = g_strdup (tmp_dir);
      g_free (tmp_dir);

      /* Report the successful result to the user */
      irritate_user (dgettext ("hildon-common-strings",
                               "sfil_ib_saved"));
    }
  else
    {
      /* Remove the previously stored dir if failed */
      last_save_log_dir = NULL;
    }

  g_free (uri);
}

static void
save_log (char *uri, void *data)
{
  GFileInfo *info;
  GFile *file;
  GError *error = NULL;

  /* Now users able to delete the parent dialog (save dialog is closed)... */
  g_object_set (G_OBJECT (data), "deletable", TRUE, NULL);

  if (uri == NULL)
    {
      /* Dialog was cancelled, do nothing.
       */
      return;
    }

  file = g_file_new_for_uri (uri);
  info = g_file_query_info (file, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE,
                            G_FILE_QUERY_INFO_NONE, NULL, &error);

  if (info)
    {
      ask_custom (dgettext ("hildon-fm", "docm_nc_replace_file"),
		  dgettext ("hildon-libs", "wdgt_bd_yes"),
		  dgettext ("hildon-libs", "wdgt_bd_no"),
		  save_log_cont, uri);
    }
  else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      /* Remove the previously stored dir if failed */
      if (last_save_log_dir != NULL)
        g_free (last_save_log_dir);
      last_save_log_dir = NULL;

      /* Annoy user with error */
      annoy_user_with_g_error (error, uri, save_log_do_nothing, NULL);
      g_error_free (error);
      g_free (uri);
    }
  else
    save_log_cont (true, uri);

  g_object_unref (info);
  g_object_unref (file);
}

void
clear_log ()
{
  if (log_text)
    g_string_truncate (log_text, 0);
  add_log ("%s %s\n", PACKAGE, VERSION);
  set_log_start ();
}

static void
log_response (GtkDialog *dialog, gint response, gpointer clos)
{
  GtkWidget *text_view = (GtkWidget *)clos;

  if (response == RESPONSE_CLEAR)
    {
      clear_log ();
      if (log_text && text_view)
	set_small_text_view_text (text_view, log_text->str);
    }
  else if (response == RESPONSE_SAVE)
    {
      /* We should ignore delete-event when save dialog is shown... */
      g_object_set (G_OBJECT (dialog), "deletable", FALSE, NULL);

      const char *home = getenv ("HOME");
      char *folder = NULL;
      char *name = g_strconcat (_("ai_li_save_log_default_name"), ".txt",
				NULL);

      if (last_save_log_dir)
	folder = g_strdup (last_save_log_dir);
      else
        {
          const char *dir = g_get_user_special_dir (G_USER_DIRECTORY_DOCUMENTS);
          if (dir)
            folder = g_strdup_printf ("file://%s", dir);
          else if (home)
            folder = g_strdup_printf ("file://%s/MyDocs/.documents", home);
        }

      show_file_chooser_for_save (_("ai_ti_save_log"),
				  GTK_WINDOW (dialog),
				  folder,
				  name,
				  save_log, dialog);
      g_free (folder);
      g_free (name);
    }
  else if (response == GTK_RESPONSE_CLOSE
           || response == GTK_RESPONSE_DELETE_EVENT)
    {
      gboolean deletable;
      g_object_get (G_OBJECT (dialog), "deletable", &deletable, NULL);

      if (deletable)
        {
          pop_dialog (GTK_WIDGET (dialog));
          gtk_widget_destroy (GTK_WIDGET (dialog));
          end_interaction_flow ();
        }
    }
}

static gboolean
log_dialog_delete (GtkWidget *widget, GdkEvent *event, gpointer unused)
{
  /** XXX: Why not GTK does this? */
  gboolean deletable;
  g_object_get (G_OBJECT (widget), "deletable", &deletable, NULL);

  return !deletable;
}

void
show_log_dialog_flow ()
{
  if (start_interaction_flow ())
    {
      GtkWidget *dialog, *text_view;

      dialog = gtk_dialog_new_with_buttons (_("ai_ti_log"),
					    NULL,
					    GTK_DIALOG_MODAL,
					    _("ai_bd_log_clear"),
					    RESPONSE_CLEAR,
					    _("ai_bd_log_save_as"),
					    RESPONSE_SAVE,
					    NULL);
      push_dialog (dialog);
      respond_on_escape (GTK_DIALOG (dialog), GTK_RESPONSE_CLOSE);

      gtk_dialog_set_has_separator (GTK_DIALOG (dialog), FALSE);

      text_view = make_small_text_view (log_text? log_text->str : "");

      gtk_container_add (GTK_CONTAINER (GTK_DIALOG (dialog)->vbox), text_view);

      gtk_widget_set_size_request (dialog, 600,300);

      g_object_set (G_OBJECT (dialog), "deletable", TRUE, NULL);

      g_signal_connect (dialog, "delete-event",
                        G_CALLBACK (log_dialog_delete), NULL);
      g_signal_connect (dialog, "response",
                        G_CALLBACK (log_response), text_view);

      gtk_widget_show_all (dialog);
    }
}

#if !HAVE_G_STRING_APPEND_VPRINTF
void
g_string_append_vprintf (GString *str, const gchar *fmt, va_list args)
{
  /* As found on the net.  Hackish.
   */
  va_list args2;
  va_copy (args2, args);
  gsize old_len = str->len;
  gsize fmt_len = g_printf_string_upper_bound (fmt, args) + 1;
  g_string_set_size (str, old_len + fmt_len);
  str->len = old_len + g_vsnprintf (str->str + old_len, fmt_len, fmt, args2);
  va_end (args2);
}
#endif

void
add_log (const char *text, ...)
{
  va_list args;
  va_start (args, text);

  if (log_text == NULL)
    log_text = g_string_new ("");
  g_string_append_vprintf (log_text, text, args);

  // XXX - update log dialog when it is open now

  va_end (args);
}

void
log_perror (const char *msg)
{
  add_log ("%s: %s\n", msg, strerror (errno));
}

static void
add_log_no_fmt (const gchar *str, size_t n)
{
  if (log_text == NULL)
    log_text = g_string_new ("");
  g_string_append_len (log_text, str, n);
}

static guint log_start = 0;

void
set_log_start ()
{
  if (log_text)
    log_start = log_text->len;
  else
    log_start = 0;
}

bool
scan_log (const char *str)
{
  if (log_text && log_text->len >= log_start)
    return strstr (log_text->str + log_start, str);
  else
    return false;
}

apt_proto_result_code
scan_log_for_result_code (apt_proto_result_code code)
{
  if (code == rescode_failure)
    {
      /* XXX - We should probably get the string from strerror, but
	 then we need to synchronize the locale environments between
	 dpkg and this process...
      */
      if (scan_log ("No space left on device"))
	code = rescode_out_of_space;
    }

  return code;
}

static gboolean
read_for_log (GIOChannel *channel, GIOCondition cond, gpointer data)
{
  gchar buf[256];
  gsize count;
  GIOStatus status;
  
#if 0
  /* XXX - this blocks sometime.  Maybe setting the encoding to NULL
           will work, but for now we just do it the old school way...
  */
  status = g_io_channel_read_chars (channel, buf, 256, &count, NULL);
#else
  {
    int fd = g_io_channel_unix_get_fd (channel);
    int n = read (fd, buf, 256);
    if (n > 0)
      {
	status = G_IO_STATUS_NORMAL;
	count = n;
      }
    else
      {
	status = G_IO_STATUS_EOF;
	count = 0;
      }
  }
#endif

  if (status == G_IO_STATUS_NORMAL)
    {
      add_log_no_fmt (buf, count);
      write (2, buf, count);
      return TRUE;
    }
  else
    {
      g_io_channel_shutdown (channel, 0, NULL);
      return FALSE;
    }
}

void
log_from_fd (int fd)
{
  GIOChannel *channel = g_io_channel_unix_new (fd);
  g_io_add_watch (channel, GIOCondition (G_IO_IN | G_IO_HUP | G_IO_ERR),
		  read_for_log, NULL);
  g_io_channel_unref (channel);
}
