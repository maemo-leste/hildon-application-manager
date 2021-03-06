/*
 * This file is part of the hildon-application-manager.
 *
 * Copyright (C) 2008 Nokia Corporation.  All Rights reserved.
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

/* HAM-AFTER-BOOT

   This utility is called on every boot.  It's purpose is to announce
   the successful updating of the operating system, if that was the
   reason for the boot.
*/

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libintl.h>
#include <errno.h>

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <hildon/hildon.h>

#include "user_files.h"
#include "xexp.h"

#define _(x) dgettext ("hildon-application-manager", x)

/* This path is an implicit contract with apt-worker */
#define RESCUE_RESULT_FILE "/var/lib/hildon-application-manager/rescue-result"

int
main (int argc, char **argv)
{
  FILE *f;
  GtkWidget *dialog;
  xexp *rescue_xexp = NULL;
  int rescue_success = 1;

  /* Check UFILE_BOOT flag file */
  f = user_file_open_for_read (UFILE_BOOT);
  if (f == NULL)
    {
      if (errno != ENOENT)
	perror (UFILE_BOOT);
      return 0;
    }
  fclose (f);
  user_file_remove (UFILE_BOOT);

  /* Check rescue mode last result */
  rescue_xexp = xexp_read_file (RESCUE_RESULT_FILE);
  if (rescue_xexp && xexp_is_text (rescue_xexp)
      && xexp_is (rescue_xexp, "success"))
    {
      rescue_success = xexp_text_as_int (rescue_xexp);
    }
  xexp_free (rescue_xexp);

  /* Show success banner only on success. Pretty logical, isn't it? */
  if (rescue_success)
    {
      hildon_gtk_init (&argc, &argv);

      /* For the OS update, make sure the icon will blink after reboot
         by deleting user files related to tapped and seen states */
      user_file_remove (UFILE_SEEN_UPDATES);
      user_file_remove (UFILE_TAPPED_UPDATES);

      dialog = hildon_note_new_information
        (NULL, _("ai_ni_system_update_installed"));

      gtk_widget_show_all (dialog);
      gtk_dialog_run (GTK_DIALOG (dialog));
      gtk_widget_destroy (dialog);
    }

  return 0;
}
