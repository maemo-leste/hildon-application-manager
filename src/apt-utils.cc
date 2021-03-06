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

#include <apt-pkg/debversion.h>

#include "apt-utils.h"

gint
compare_deb_versions (const gchar *a, const gchar *b)
{
  if (a == NULL)
    return 1;
  if (b == NULL)
    return -1;

  return debVS.CmpVersion (a,b);
}
