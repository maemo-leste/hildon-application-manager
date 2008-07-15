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

#include <dbus/dbus.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <string.h>
#include <libintl.h>

#include "dbus.h"
#include "util.h"
#include "log.h"
#include "main.h"
#include "operations.h"

#include <mce/dbus-names.h>
#include <mce/mode-names.h>

/* For asking for battery information */
#define BME_SERVICE			"com.nokia.bme"
#define BME_REQUEST_IF			"com.nokia.bme.request"
#define BME_SIGNAL_IF			"com.nokia.bme.signal"
#define BME_REQUEST_PATH		"/com/nokia/bme/request"
#define BME_STATUS_INFO_REQ		"status_info_req"
#define BME_BATTERY_STATE_UPDATE	"battery_state_changed"
#define BME_CHARGER_CONNECTED		"charger_connected"
#define BME_CHARGER_DISCONNECTED        "charger_disconnected"
#define BME_CHARGER_CHARGING_ON         "charger_charging_on"
#define BME_CHARGER_CHARGING_OFF	"charger_charging_off"

/* For getting and tracking the Bluetooth name
 */
#define BTNAME_SERVICE                  "org.bluez"
#define BTNAME_REQUEST_IF               "org.bluez.Adapter"
#define BTNAME_SIGNAL_IF                "org.bluez.Adapter"
#define BTNAME_REQUEST_PATH             "/org/bluez/hci0"
#define BTNAME_SIGNAL_PATH              "/org/bluez/hci0"

#define BTNAME_REQ_GET                  "GetName"
#define BTNAME_SIG_CHANGED              "NameChanged"

#define BTNAME_MATCH_RULE "type='signal',interface='" BTNAME_SIGNAL_IF \
                          "',member='" BTNAME_SIG_CHANGED "'"

/* Max time to wait for the battery status */
#define BATTERY_REQUEST_TIMEOUT 5

static void
dbus_mime_open (DBusConnection *conn, DBusMessage *message)
{
  DBusError error;
  DBusMessage *reply;
  char *filename;

  dbus_error_init (&error);
  if (dbus_message_get_args (message, &error,
			     DBUS_TYPE_STRING, &filename,
			     DBUS_TYPE_INVALID))
    {
      present_main_window ();
      if (strcmp (filename, "magic:restore-packages") == 0)
	restore_packages_flow ();
      else
	install_from_file_flow (filename);

      reply = dbus_message_new_method_return (message);
      dbus_connection_send (conn, reply, NULL);
      dbus_message_unref (reply);
    }
  else
    {
      reply = dbus_message_new_error (message,
				      DBUS_ERROR_INVALID_ARGS,
				      error.message);
      dbus_connection_send (conn, reply, NULL);
      dbus_message_unref (reply);
    }
}

struct dip_clos {
  Window xid;
  char *title;
  char *desc;
  const char **packages;
  DBusConnection *conn;
  DBusMessage *message;
};

static void dbus_install_packages (DBusConnection *conn, DBusMessage *message);
static void dip_with_initialized_packages (void *data);
static void dip_install_done (int n_successful, void *data);
static void dip_end (int result, void *data);

static void
dbus_install_packages (DBusConnection *conn, DBusMessage *message)
{
  DBusError error;
  
  dbus_int32_t xid;
  const char **packages;
  int n_packages;

  dbus_connection_ref (conn);
  dbus_message_ref (message);
  
  dip_clos *c = new dip_clos;
  c->conn = conn;
  c->message = message;
  c->packages = NULL;

  dbus_error_init (&error);
  if (dbus_message_get_args (message, &error,
			     DBUS_TYPE_INT32, &xid,
			     DBUS_TYPE_STRING, &c->title,
			     DBUS_TYPE_STRING, &c->desc,
			     DBUS_TYPE_ARRAY,
			     DBUS_TYPE_STRING, &packages, &n_packages,
			     DBUS_TYPE_INVALID))
    {
      c->xid = xid;
      c->packages = packages;
      
      with_initialized_packages (dip_with_initialized_packages, c);
    }
  else
    {
      DBusMessage *reply;
      reply = dbus_message_new_error (message,
				      DBUS_ERROR_INVALID_ARGS,
				      error.message);
      dbus_connection_send (conn, reply, NULL);
      dbus_message_unref (reply);
      dip_end (-1, c);
    }
}

static void
dip_with_initialized_packages (void *data)
{
  dip_clos *c = (dip_clos *)data;

  if (c->xid)
    {
      if (start_foreign_interaction_flow (c->xid))
	install_named_packages (APTSTATE_DEFAULT, c->packages,
				INSTALL_TYPE_MULTI, false,
				c->title, c->desc,
				dip_install_done, c);
      else
	dip_end (-1, c);
    }
  else
    {
      present_main_window ();
      if (start_interaction_flow ())
	install_named_packages (APTSTATE_DEFAULT, c->packages,
				INSTALL_TYPE_MULTI, false,
				c->title, c->desc,
				dip_install_done, c);
      else
	dip_end (-1, c);
    }
}

static void
dip_install_done (int n_successful, void *data)
{
  dip_clos *c = (dip_clos *)data;

  end_interaction_flow ();

  dip_end (n_successful, c);
}

static void
dip_end (int result, void *data)
{
  dip_clos *c = (dip_clos *)data;

  DBusMessage *reply;
  dbus_int32_t dbus_result = result;
	  
  reply = dbus_message_new_method_return (c->message);
  dbus_message_append_args (reply,
			    DBUS_TYPE_INT32, &dbus_result,
			    DBUS_TYPE_INVALID);
  
  dbus_connection_send (c->conn, reply, NULL);
  dbus_message_unref (reply);

  // So that we don't lose the reply when we exit below.
  dbus_connection_flush (c->conn);

  dbus_free_string_array ((char **)c->packages);
  dbus_message_unref (c->message);
  dbus_connection_unref (c->conn);
  delete c;

  maybe_exit ();
}

struct dif_clos {
  Window xid;
  char *filename;
  DBusConnection *conn;
  DBusMessage *message;
};

static void dbus_install_file (DBusConnection *conn, DBusMessage *message);
static void dif_with_initialized_packages (void *data);
static void dif_install_done (bool success, void *data);
static void dif_end (int result, void *data);

static void
dbus_install_file (DBusConnection *conn, DBusMessage *message)
{
  DBusError error;

  dbus_int32_t xid;

  dbus_connection_ref (conn);
  dbus_message_ref (message);
  
  dif_clos *c = new dif_clos;
  c->conn = conn;
  c->message = message;
  c->filename = NULL;

  dbus_error_init (&error);
  if (dbus_message_get_args (message, &error,
			     DBUS_TYPE_INT32, &xid,
			     DBUS_TYPE_STRING, &c->filename,
			     DBUS_TYPE_INVALID))
    {
      c->xid = xid;
      with_initialized_packages (dif_with_initialized_packages, c);
    }
  else
    {
      DBusMessage *reply;
      reply = dbus_message_new_error (message,
				      DBUS_ERROR_INVALID_ARGS,
				      error.message);
      dbus_connection_send (conn, reply, NULL);
      dbus_message_unref (reply);
      dif_end (-1, c);
    }
}

static void
dif_with_initialized_packages (void *data)
{
  dif_clos *c = (dif_clos *)data;

  if (c->xid)
    {
      if (start_foreign_interaction_flow (c->xid))
	install_file (c->filename, dif_install_done, c);
      else
	dif_end (-1, c);
    }
  else
    {
      present_main_window ();
      if (start_interaction_flow ())
	install_file (c->filename, dif_install_done, c);
      else
	dif_end (-1, c);
    }
}

static void
dif_install_done (bool success, void *data)
{
  dif_clos *c = (dif_clos *)data;

  end_interaction_flow ();

  dif_end (success? 1 : 0, c);
}

static void
dif_end (int result, void *data)
{
  dif_clos *c = (dif_clos *)data;

  DBusMessage *reply;
  dbus_int32_t dbus_result = result;
	  
  reply = dbus_message_new_method_return (c->message);
  dbus_message_append_args (reply,
			    DBUS_TYPE_INT32, &dbus_result,
			    DBUS_TYPE_INVALID);
  
  dbus_connection_send (c->conn, reply, NULL);
  dbus_message_unref (reply);

  // So that we don't lose the reply when we exit below.
  dbus_connection_flush (c->conn);

  dbus_message_unref (c->message);
  dbus_connection_unref (c->conn);
  delete c;

  maybe_exit ();
}

static void icfu_end (bool ignored, void *data);

static void
idle_check_for_updates (void *unused)
{
  refresh_package_cache_without_user (NULL, APTSTATE_DEFAULT, icfu_end, NULL);
}

static void
icfu_end (bool ignored, void *data)
{
  end_interaction_flow ();
}

static DBusHandlerResult 
dbus_handler (DBusConnection *conn, DBusMessage *message, void *data)
{
  if (dbus_message_is_method_call (message,
				   "com.nokia.hildon_application_manager",
				   "mime_open"))
    {
      dbus_mime_open (conn, message);
      return DBUS_HANDLER_RESULT_HANDLED;
    }

  if (dbus_message_is_method_call (message,
				   "com.nokia.hildon_application_manager",
				   "install_packages"))
    {
      dbus_install_packages (conn, message);
      return DBUS_HANDLER_RESULT_HANDLED;
    }

  if (dbus_message_is_method_call (message,
				   "com.nokia.hildon_application_manager",
				   "install_file"))
    {
      dbus_install_file (conn, message);
      return DBUS_HANDLER_RESULT_HANDLED;
    }

  if (dbus_message_is_method_call (message,
				   "com.nokia.hildon_application_manager",
				   "top_application"))
    {
      DBusMessage *reply;

      present_main_window ();

      reply = dbus_message_new_method_return (message);
      dbus_connection_send (conn, reply, NULL);
      dbus_message_unref (reply);

      return DBUS_HANDLER_RESULT_HANDLED;
    }

  if (dbus_message_is_method_call (message,
				   "com.nokia.hildon_application_manager",
				   "show_check_for_updates_view"))
    {
      DBusMessage *reply;

      present_main_window ();
      if (is_idle ())
	show_check_for_updates_view ();
      
      reply = dbus_message_new_method_return (message);
      dbus_connection_send (conn, reply, NULL);
      dbus_message_unref (reply);

      return DBUS_HANDLER_RESULT_HANDLED;
    }

  if (dbus_message_is_method_call (message,
				   "com.nokia.hildon_application_manager",
				   "showing_check_for_updates_view"))
    {
      DBusMessage *reply;
      gboolean showing_view = FALSE;

      /* Check if 'check for updates' view is being shown */
      if (get_current_view_id () == UPGRADE_APPLICATIONS_VIEW)
	showing_view = TRUE;

      /* Build reply message with the required boolean value */
      reply = dbus_message_new_method_return (message);
      dbus_message_append_args (reply,
				DBUS_TYPE_BOOLEAN , &showing_view,
				DBUS_TYPE_INVALID);

      dbus_connection_send (conn, reply, NULL);
      dbus_message_unref (reply);

      return DBUS_HANDLER_RESULT_HANDLED;
    }

  if (dbus_message_is_method_call (message,
				   "com.nokia.hildon_application_manager",
				   "check_for_updates"))
    {
      DBusMessage *reply;

      start_interaction_flow_when_idle (idle_check_for_updates, NULL);
      
      reply = dbus_message_new_method_return (message);
      dbus_connection_send (conn, reply, NULL);
      dbus_message_unref (reply);

      return DBUS_HANDLER_RESULT_HANDLED;
    }

  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static char *btname = NULL;

const char *
device_name ()
{
  if (btname != NULL)
    return btname;
  else
    {
      const char *name = getenv ("OSSO_PRODUCT_NAME");
      if (name)
	return name;

      return "";
    }
}

static void
set_bt_name_from_message (DBusMessage *message)
{
  DBusMessageIter iter;
  const char *name = NULL;
  GtkWidget *label = NULL;

  g_return_if_fail (message != NULL);

  if (!dbus_message_iter_init (message, &iter))
    {
      add_log ("message did not have argument\n");
      return;
    }
  dbus_message_iter_get_basic (&iter, &name);

  if (btname) 
    g_free (btname);

  btname = g_strdup (name);

  label = get_device_label ();

  if (label)
    gtk_label_set_text (GTK_LABEL (label), btname);
}

static void 
btname_received (DBusPendingCall *call, void *user_data)
{
  DBusMessage *message;
  DBusError error;

  g_assert (dbus_pending_call_get_completed (call));
  message = dbus_pending_call_steal_reply (call);
  if (message == NULL)
    {
      add_log ("no reply\n");
      return;
    }

  dbus_error_init (&error);

  if (dbus_set_error_from_message (&error, message))
    {
      add_log ("get btname: %s\n", error.message);
      dbus_error_free (&error);
    }
  else   
    set_bt_name_from_message (message);

  dbus_message_unref (message);
}

static DBusHandlerResult
handle_dbus_signal (DBusConnection *conn,
		    DBusMessage *msg,
		    gpointer data)
{
  if (dbus_message_is_signal(msg, BTNAME_SIGNAL_IF, BTNAME_SIG_CHANGED))
    set_bt_name_from_message(msg);

  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void
init_dbus_or_die (bool top_existing)
{
  DBusError error;
  DBusConnection *connection;
  DBusMessage *request;
  DBusPendingCall *call = NULL;

  /* Set ourself up on the session bus.
   */

  dbus_error_init (&error);
  connection = dbus_bus_get (DBUS_BUS_SESSION, &error);
  if (connection == NULL)
    {
      fprintf (stderr, "Can't get session dbus: %s", error.message);
      exit (1);
    }

  dbus_connection_setup_with_g_main (connection, NULL);

  if (!dbus_connection_add_filter (connection, dbus_handler, NULL, NULL))
    {
      fprintf (stderr, "Can't add dbus filter");
      exit (1);
    }

  dbus_error_init (&error);
  int result = dbus_bus_request_name (connection,
				      "com.nokia.hildon_application_manager",
				      DBUS_NAME_FLAG_DO_NOT_QUEUE,
				      &error);

  if (result < 0)
    {
      fprintf (stderr, "Can't request name on dbus: %s\n", error.message);
      exit (1);
    }

  if (result == DBUS_REQUEST_NAME_REPLY_EXISTS)
    {
      /* There is already an instance of us running.  Bring it to the
	 front if requested.
      */
      if (top_existing)
	{
	  request = dbus_message_new_method_call 
	    ("com.nokia.hildon_application_manager",
	     "/com/nokia/hildon_application_manager",
	     "com.nokia.hildon_application_manager",
	     "top_application");
	  
	  if (request)
	    dbus_connection_send_with_reply_and_block (connection, request,
						       INT_MAX, NULL);
	}

      exit (0);
    }

  if (result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
    {
      /* Wierdness, but let's continue anyway.
       */
      fprintf (stderr, "Couldn't be the primary owner.\n");
    }

  /* Listen on the system bus for changes to the device name.
   */

  dbus_error_init (&error);
  connection = dbus_bus_get (DBUS_BUS_SYSTEM, &error);
  if (connection == NULL)
    {
      fprintf (stderr, "Can't get system dbus: %s", error.message);
      exit (1);
    }

  /* Listen to signals from bme-dbus-proxy */
  dbus_error_init(&error);
  dbus_bus_add_match(connection,
		     "type='signal',interface='" BME_SIGNAL_IF "'", &error);
  dbus_connection_flush(connection);
  if (dbus_error_is_set(&error))
    {
      dbus_error_free(&error);
      fprintf (stderr, "Failed to add DBus match for '" BME_SIGNAL_IF "'\n");
    }

  /* Let's query initial state.  These calls are async, so they do not
     consume too much startup time.
   */
  request = dbus_message_new_method_call (BTNAME_SERVICE,
					  BTNAME_REQUEST_PATH,
					  BTNAME_REQUEST_IF,
					  BTNAME_REQ_GET);
  if (request == NULL)
    {
      fprintf (stderr, "dbus_message_new_method_call failed\n");
      return;
    }

  dbus_message_set_auto_start (request, TRUE);

  if (dbus_connection_send_with_reply (connection, request, &call, -1))
    {
      dbus_pending_call_set_notify (call, btname_received, NULL, NULL);
      dbus_pending_call_unref (call);
    }

  dbus_message_unref (request);

  dbus_connection_setup_with_g_main (connection, NULL);
  dbus_bus_add_match (connection, BTNAME_MATCH_RULE, &error);
  if (dbus_error_is_set(&error))
    {
      fprintf (stderr, "dbus_bus_add_match failed: %s\n", error.message);
      dbus_error_free (&error);
    }

  if (!dbus_connection_add_filter(connection, handle_dbus_signal, NULL, NULL))
    fprintf (stderr, "dbus_connection_add_filter failed\n");
}


void
send_reboot_message (void)
{
  DBusConnection *conn;
  DBusMessage *msg;

  /* Helps debugging. */
  add_log ("Sending reboot message.\n");

  conn = dbus_bus_get (DBUS_BUS_SYSTEM, NULL);
  if (!conn)
    {
      add_log ("Could not get system bus.\n");
      return;
    }

  msg = dbus_message_new_method_call (MCE_SERVICE,
				      MCE_REQUEST_PATH,
				      MCE_REQUEST_IF,
				      MCE_REBOOT_REQ);

  dbus_connection_send (conn, msg, NULL);
  dbus_connection_flush (conn);

  add_log ("Reboot message sent, quit the application.\n");
}

static inline void send_battery_status_request (void)
{
  DBusError error;
  DBusConnection *conn;
  DBusMessage *msg;
  dbus_bool_t retval = 0;

  dbus_error_init (&error);
  conn = dbus_bus_get (DBUS_BUS_SYSTEM, &error);
  if (conn == NULL)
    {
      fprintf (stderr, "Can't get session dbus: %s", error.message);
      return;
    }

  msg = dbus_message_new_signal(BME_REQUEST_PATH, BME_REQUEST_IF, BME_STATUS_INFO_REQ);
  if (!msg)
    {
      fprintf (stderr, "Fail initializing Dbus signal msg\n");
      return;
    }

  dbus_message_set_no_reply(msg, TRUE);

  retval = dbus_connection_send (conn, msg, NULL);
  if (!retval)
    {
      fprintf (stderr, "Fail initializing Dbus signal msg\n");
      return;
    }
  else
    dbus_connection_flush(conn);

  dbus_message_unref(msg);
}

static battery_info *
receive_battery_status_update (void)
{
  DBusMessage* msg = NULL;
  DBusMessageIter args;
  DBusConnection* conn;
  DBusError err;
  GTimer *timer = NULL;
  gint32 b_level = -1;
  battery_info *b_info = NULL;
  gboolean checked_charging = FALSE;

  dbus_error_init(&err);
  conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
  if (dbus_error_is_set(&err)) 
    {
      fprintf (stderr, "Connection Error (%s)\n", err.message);
      dbus_error_free(&err);
      return NULL;
    }

  /* Init battery info struct */
  b_info = new battery_info ();
  b_info->level = 0;
  b_info->charging = FALSE;

  /* Init timer */
  timer = g_timer_new ();

  /* Check for interesting signals */
  g_timer_start (timer);
  while ((g_timer_elapsed (timer, NULL) < BATTERY_REQUEST_TIMEOUT) &&
    (b_info->level == -1 || !checked_charging))
    {
      dbus_connection_read_write(conn, 0);
      msg = dbus_connection_pop_message(conn);

      if (msg == NULL)
	continue;

      if (dbus_message_is_signal(msg, BME_SIGNAL_IF, BME_BATTERY_STATE_UPDATE))
	{
	  /* Read the parameters */
	  if (!dbus_message_iter_init(msg, &args))
	    {
	      fprintf (stderr, "Message Has No Parameters\n");
	      continue;
	    }

	  if (DBUS_TYPE_UINT32 != dbus_message_iter_get_arg_type(&args))
	    {
	      fprintf(stderr, "Argument is not uint32!\n");
	      continue;
	    }

	  dbus_message_iter_get_basic(&args, &b_level);
	  b_info->level = b_level;
	}
      else if (dbus_message_is_signal(msg, BME_SIGNAL_IF, BME_CHARGER_CONNECTED) ||
	       dbus_message_is_signal(msg, BME_SIGNAL_IF, BME_CHARGER_CHARGING_ON))
	{
	  b_info->charging = TRUE;
	  checked_charging = TRUE;
	}
      else if (dbus_message_is_signal(msg, BME_SIGNAL_IF, BME_CHARGER_DISCONNECTED) ||
	       dbus_message_is_signal(msg, BME_SIGNAL_IF, BME_CHARGER_CHARGING_OFF))
	{
	  b_info->charging = FALSE;
	  checked_charging = TRUE;
	}
    }

  /* Free memory */
  if (msg != NULL)
    dbus_message_unref(msg);

  g_timer_destroy (timer);

  return b_info;
}

battery_info *
check_battery_status (void)
{
  battery_info *batt_info = NULL;
  struct stat info;

  /* Do the battery check only if not in the scratchbox */
  if (stat ("/targets/links/scratchbox.config", &info))
    {
      send_battery_status_request ();
      batt_info = receive_battery_status_update ();
    }
  else
    {
      /* If working in the scratchbox, return a ad-hoc struct telling
	 that there's enough battery and that it's chargin on */
      batt_info = new battery_info ();
      batt_info->level = 4;
      batt_info->charging = TRUE;
    }
  return batt_info;
}
