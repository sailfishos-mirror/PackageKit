/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2008-2011 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

/**
 * SECTION:pk-control
 * @short_description: For querying data about PackageKit
 *
 * A GObject to use for accessing PackageKit asynchronously.
 */

#include "config.h"

#include <string.h>
#include <glib-object.h>
#include <gio/gio.h>

#include <packagekit-glib2/pk-bitfield.h>
#include <packagekit-glib2/pk-common.h>
#include <packagekit-glib2/pk-control.h>
#include <packagekit-glib2/pk-version.h>
#include <packagekit-glib2/pk-enum-types.h>

static void     pk_control_finalize	(GObject     *object);

#define PK_CONTROL_DBUS_METHOD_TIMEOUT		1500 /* ms */

/**
 * PkControlPrivate:
 *
 * Private #PkControl data
 **/
struct _PkControlPrivate
{
	GCancellable		*cancellable;
	GPtrArray		*calls;
	GDBusProxy		*proxy;
	guint			 version_major;
	guint			 version_minor;
	guint			 version_micro;
	gchar			*backend_name;
	gchar			*backend_description;
	gchar			*backend_author;
	PkBitfield		 roles;
	PkBitfield		 provides;
	PkBitfield		 groups;
	PkBitfield		 filters;
	gchar			**mime_types;
	gboolean		 connected;
	gboolean		 locked;
	PkNetworkEnum		 network_state;
	gchar			*distro_id;
	guint			 watch_id;
};

enum {
	SIGNAL_TRANSACTION_LIST_CHANGED,
	SIGNAL_RESTART_SCHEDULE,
	SIGNAL_INSTALLED_CHANGED,
	SIGNAL_UPDATES_CHANGED,
	SIGNAL_REPO_LIST_CHANGED,
	SIGNAL_LAST
};

enum {
	PROP_0,
	PROP_VERSION_MAJOR,
	PROP_VERSION_MINOR,
	PROP_VERSION_MICRO,
	PROP_BACKEND_NAME,
	PROP_BACKEND_DESCRIPTION,
	PROP_BACKEND_AUTHOR,
	PROP_ROLES,
	PROP_GROUPS,
	PROP_FILTERS,
	PROP_PROVIDES,
	PROP_MIME_TYPES,
	PROP_LOCKED,
	PROP_NETWORK_STATE,
	PROP_CONNECTED,
	PROP_DISTRO_ID,
	PROP_LAST
};

static guint signals[SIGNAL_LAST] = { 0 };
static gpointer pk_control_object = NULL;

static GParamSpec *obj_properties[PROP_LAST] = { NULL, };

G_DEFINE_TYPE_WITH_PRIVATE (PkControl, pk_control, G_TYPE_OBJECT)

/**
 * pk_control_error_quark:
 *
 * We are a GObject that sets errors
 *
 * Returns: Our personal error quark.
 *
 * Since: 0.5.2
 **/
G_DEFINE_QUARK (pk-control-error-quark, pk_control_error)

/*
 * pk_control_fixup_dbus_error:
 **/
static void
pk_control_fixup_dbus_error (GError *error)
{
	g_return_if_fail (error != NULL);

	/* hardcode domain */
	error->domain = PK_CONTROL_ERROR;

	/* find a better failure code */
	if (error->code == 0)
		error->code = PK_CONTROL_ERROR_CANNOT_START_DAEMON;
	else
		error->code = PK_CONTROL_ERROR_FAILED;
}

static gboolean
_g_strvcmp0 (gchar **one, gchar **two)
{
	guint i;
	if (one == two)
		return TRUE;
	if (one == NULL && two != NULL)
		return FALSE;
	if (one != NULL && two == NULL)
		return FALSE;
	if (g_strv_length (one) != g_strv_length (two))
		return FALSE;
	for (i = 0; one[i] != NULL; i++) {
		if (g_strcmp0 (one[i], two[i]) != 0)
			return FALSE;
	}
	return TRUE;
}

/*
 * pk_control_set_property_value:
 **/
static void
pk_control_set_property_value (PkControl *control,
			       const gchar *key,
			       GVariant *value)
{
	PkControlPrivate *priv = pk_control_get_instance_private (control);
	const gchar *tmp_str;
	gboolean tmp_bool;
	guint tmp_uint;
	PkBitfield tmp_bitfield;

	if (g_strcmp0 (key, "VersionMajor") == 0) {
		tmp_uint = g_variant_get_uint32 (value);
		if (priv->version_major == tmp_uint)
			return;
		priv->version_major = tmp_uint;
		g_object_notify_by_pspec (G_OBJECT(control), obj_properties[PROP_VERSION_MAJOR]);
		return;
	}
	if (g_strcmp0 (key, "VersionMinor") == 0) {
		tmp_uint = g_variant_get_uint32 (value);
		if (priv->version_minor == tmp_uint)
			return;
		priv->version_minor = tmp_uint;
		g_object_notify_by_pspec (G_OBJECT(control), obj_properties[PROP_VERSION_MINOR]);
		return;
	}
	if (g_strcmp0 (key, "VersionMicro") == 0) {
		tmp_uint = g_variant_get_uint32 (value);
		if (priv->version_micro == tmp_uint)
			return;
		priv->version_micro = tmp_uint;
		g_object_notify_by_pspec (G_OBJECT(control), obj_properties[PROP_VERSION_MICRO]);
		return;
	}
	if (g_strcmp0 (key, "BackendName") == 0) {
		tmp_str = g_variant_get_string (value, NULL);
		if (g_strcmp0 (priv->backend_name, tmp_str) == 0)
			return;
		g_free (priv->backend_name);
		priv->backend_name = g_strdup (tmp_str);
		g_object_notify_by_pspec (G_OBJECT(control), obj_properties[PROP_BACKEND_NAME]);
		return;
	}
	if (g_strcmp0 (key, "BackendDescription") == 0) {
		tmp_str = g_variant_get_string (value, NULL);
		if (g_strcmp0 (priv->backend_description, tmp_str) == 0)
			return;
		g_free (priv->backend_description);
		priv->backend_description = g_strdup (tmp_str);
		g_object_notify_by_pspec (G_OBJECT(control), obj_properties[PROP_BACKEND_DESCRIPTION]);
		return;
	}
	if (g_strcmp0 (key, "BackendAuthor") == 0) {
		tmp_str = g_variant_get_string (value, NULL);
		if (g_strcmp0 (priv->backend_author, tmp_str) == 0)
			return;
		g_free (priv->backend_author);
		priv->backend_author = g_strdup (tmp_str);
		g_object_notify_by_pspec (G_OBJECT(control), obj_properties[PROP_BACKEND_AUTHOR]);
		return;
	}
	if (g_strcmp0 (key, "MimeTypes") == 0) {
		g_autofree gchar **tmp_strv = NULL;
		tmp_strv = (gchar **) g_variant_get_strv (value, NULL);
		if (_g_strvcmp0 (priv->mime_types, tmp_strv))
			return;
		g_strfreev (priv->mime_types);
		priv->mime_types = g_strdupv (tmp_strv);
		g_object_notify_by_pspec (G_OBJECT(control), obj_properties[PROP_MIME_TYPES]);
		return;
	}
	if (g_strcmp0 (key, "Roles") == 0) {
		tmp_bitfield = g_variant_get_uint64 (value);
		if (priv->roles == tmp_bitfield)
			return;
		priv->roles = tmp_bitfield;
		g_object_notify_by_pspec (G_OBJECT(control), obj_properties[PROP_ROLES]);
		return;
	}
	if (g_strcmp0 (key, "Provides") == 0) {
		tmp_bitfield = g_variant_get_uint64 (value);
		if (priv->provides == tmp_bitfield)
			return;
		priv->provides = tmp_bitfield;
		g_object_notify_by_pspec (G_OBJECT(control), obj_properties[PROP_PROVIDES]);
		return;
	}
	if (g_strcmp0 (key, "Groups") == 0) {
		tmp_bitfield = g_variant_get_uint64 (value);
		if (priv->groups == tmp_bitfield)
			return;
		priv->groups = tmp_bitfield;
		g_object_notify_by_pspec (G_OBJECT(control), obj_properties[PROP_GROUPS]);
		return;
	}
	if (g_strcmp0 (key, "Filters") == 0) {
		tmp_bitfield = g_variant_get_uint64 (value);
		if (priv->filters == tmp_bitfield)
			return;
		priv->filters = tmp_bitfield;
		g_object_notify_by_pspec (G_OBJECT(control), obj_properties[PROP_FILTERS]);
		return;
	}
	if (g_strcmp0 (key, "Locked") == 0) {
		tmp_bool = g_variant_get_boolean (value);
		if (priv->locked == tmp_bool)
			return;
		priv->locked = tmp_bool;
		g_object_notify_by_pspec (G_OBJECT(control), obj_properties[PROP_LOCKED]);
		return;
	}
	if (g_strcmp0 (key, "NetworkState") == 0) {
		tmp_uint = g_variant_get_uint32 (value);
		if (priv->network_state == tmp_uint)
			return;
		priv->network_state = tmp_uint;
		g_object_notify_by_pspec (G_OBJECT(control), obj_properties[PROP_NETWORK_STATE]);
		return;
	}
	if (g_strcmp0 (key, "DistroId") == 0) {
		tmp_str = g_variant_get_string (value, NULL);
		/* we don't want distro specific results in 'make check' */
		if (g_getenv ("PK_SELF_TEST") != NULL)
			tmp_str = "selftest;11.91;i686";
		if (g_strcmp0 (priv->distro_id, tmp_str) == 0)
			return;
		g_free (priv->distro_id);
		priv->distro_id = g_strdup (tmp_str);
		g_object_notify_by_pspec (G_OBJECT(control), obj_properties[PROP_DISTRO_ID]);
		return;
	}
	g_warning ("unhandled property '%s'", key);
}

/*
 * pk_control_properties_changed_cb:
 **/
static void
pk_control_properties_changed_cb (GDBusProxy *proxy,
				  GVariant *changed_properties,
				  const gchar* const  *invalidated_properties,
				  gpointer user_data)
{
	const gchar *key;
	GVariantIter *iter;
	GVariant *value;
	PkControl *control = PK_CONTROL (user_data);

	if (g_variant_n_children (changed_properties) > 0) {
		g_variant_get (changed_properties,
				"a{sv}",
				&iter);
		while (g_variant_iter_loop (iter, "{&sv}", &key, &value))
			pk_control_set_property_value (control, key, value);
		g_variant_iter_free (iter);
	}
}

/*
 * pk_control_signal_cb:
 **/
static void
pk_control_signal_cb (GDBusProxy *proxy,
		      const gchar *sender_name,
		      const gchar *signal_name,
		      GVariant *parameters,
		      gpointer user_data)
{
	PkControl *control = PK_CONTROL (user_data);
	g_auto(GStrv) ids = NULL;

	if (g_strcmp0 (signal_name, "TransactionListChanged") == 0) {
		g_autofree gchar **ids_tmp = NULL;
		g_variant_get (parameters, "(^a&s)", &ids_tmp);
		if (ids_tmp == NULL) {
			ids = g_new0 (gchar *, 1);
		} else {
			ids = g_strdupv ((gchar **) ids_tmp);
		}
		g_debug ("emit transaction-list-changed");
		g_signal_emit (control,
			       signals[SIGNAL_TRANSACTION_LIST_CHANGED], 0,
			       ids);
	}
	if (g_strcmp0 (signal_name, "InstalledChanged") == 0) {
		g_debug ("emit installed-changed");
		g_signal_emit (control, signals[SIGNAL_INSTALLED_CHANGED], 0);
		return;
	}
	if (g_strcmp0 (signal_name, "UpdatesChanged") == 0) {
		g_debug ("emit updates-changed");
		g_signal_emit (control, signals[SIGNAL_UPDATES_CHANGED], 0);
		return;
	}
	if (g_strcmp0 (signal_name, "RepoListChanged") == 0) {
		g_debug ("emit repo-list-changed");
		g_signal_emit (control, signals[SIGNAL_REPO_LIST_CHANGED], 0);
		return;
	}
	if (g_strcmp0 (signal_name, "RestartSchedule") == 0) {
		g_debug ("emit restart-schedule");
		g_signal_emit (control, signals[SIGNAL_RESTART_SCHEDULE], 0);
		return;
	}
}

/*
 * pk_control_proxy_connect:
 **/
static void
pk_control_proxy_connect (PkControl *control,
			  GDBusProxy *proxy)
{
	PkControlPrivate *priv = pk_control_get_instance_private (control);
	guint i;
	g_auto(GStrv) props = NULL;

	/* coldplug properties */
	props = g_dbus_proxy_get_cached_property_names (proxy);
	for (i = 0; props != NULL && props[i] != NULL; i++) {
		g_autoptr(GVariant) value_tmp = NULL;
		value_tmp = g_dbus_proxy_get_cached_property (proxy, props[i]);
		pk_control_set_property_value (control,
					       props[i],
					       value_tmp);
	}

	/* connect up signals */
	g_signal_connect (proxy, "g-properties-changed",
			  G_CALLBACK (pk_control_properties_changed_cb),
			  control);
	g_signal_connect (proxy, "g-signal",
			  G_CALLBACK (pk_control_signal_cb),
			  control);

	/* if we have no generic system wide proxy, then use this */
	if (priv->proxy == NULL)
		priv->proxy = g_object_ref (proxy);
}

/**********************************************************************/

/*
 * pk_control_get_tid_cb:
 **/
static void
pk_control_get_tid_cb (GObject *source_object,
		       GAsyncResult *res,
		       gpointer user_data)
{
	GDBusProxy *proxy = G_DBUS_PROXY (source_object);
	g_autoptr(GTask) task = G_TASK (user_data);
	g_autoptr(GError) error = NULL;
	g_autoptr(GVariant) value = NULL;
	gchar *tid = NULL;

	/* get the result */
	value = g_dbus_proxy_call_finish (proxy, res, &error);
	if (value == NULL) {
		/* fix up the D-Bus error */
		pk_control_fixup_dbus_error (error);
		g_task_return_error (task, g_steal_pointer (&error));
		return;
	}

	/* save results */
	g_variant_get (value, "(o)", &tid);

	/* we're done */
	g_task_return_pointer (task, g_steal_pointer (&tid), g_free);
}

/*
 * pk_control_get_tid_internal:
 **/
static void
pk_control_get_tid_internal (PkControl *control,
			     GTask *task)
{
	PkControlPrivate *priv = pk_control_get_instance_private (control);
	GCancellable *cancellable;

	g_assert (PK_IS_CONTROL (control));
	g_assert (G_IS_TASK (task));

	cancellable = g_task_get_cancellable (task);
	g_dbus_proxy_call (priv->proxy,
			   "CreateTransaction",
			   NULL,
			   G_DBUS_CALL_FLAGS_NONE,
			   PK_CONTROL_DBUS_METHOD_TIMEOUT,
			   cancellable,
			   pk_control_get_tid_cb,
			   g_steal_pointer (&task));
}

/*
 * pk_control_get_tid_proxy_cb:
 **/
static void
pk_control_get_tid_proxy_cb (GObject *source_object,
			     GAsyncResult *res,
			     gpointer user_data)
{
	g_autoptr(GTask) task = G_TASK (user_data);
	g_autoptr(GDBusProxy) proxy = NULL;
	g_autoptr(GError) error = NULL;
	PkControl *control = g_task_get_source_object (task);

	proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
	if (proxy == NULL) {
		g_task_return_error (task, g_steal_pointer (&error));
		return;
	}

	pk_control_proxy_connect (control, proxy);
	pk_control_get_tid_internal (control, g_steal_pointer (&task));
}

/**
 * pk_control_get_tid_async:
 * @control: a valid #PkControl instance
 * @cancellable: a #GCancellable or %NULL
 * @callback: the function to run on completion
 * @user_data: the data to pass to @callback
 *
 * Gets a transacton ID from the daemon.
 *
 * Since: 0.5.2
 **/
void
pk_control_get_tid_async (PkControl *control,
			  GCancellable *cancellable,
			  GAsyncReadyCallback callback,
			  gpointer user_data)
{
	PkControlPrivate *priv = pk_control_get_instance_private (control);
	g_autoptr(GError) error = NULL;
	g_autoptr(GTask) task = NULL;

	g_return_if_fail (PK_IS_CONTROL (control));
	g_return_if_fail (callback != NULL);
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

	/* check not already cancelled */
	if (cancellable != NULL &&
	    g_cancellable_set_error_if_cancelled (cancellable, &error)) {
		g_task_report_error (control, callback, user_data,
				     pk_control_get_tid_async,
				     g_steal_pointer (&error));
		return;
	}

	task = g_task_new (control, cancellable, callback, user_data);
	g_task_set_source_tag (task, pk_control_get_tid_async);

	/* skip straight to the D-Bus method if already connection */
	if (priv->proxy != NULL) {
		pk_control_get_tid_internal (control, g_steal_pointer (&task));
	} else {
		g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
					  G_DBUS_PROXY_FLAGS_NONE,
					  NULL,
					  PK_DBUS_SERVICE,
					  PK_DBUS_PATH,
					  PK_DBUS_INTERFACE,
					  priv->cancellable,
					  pk_control_get_tid_proxy_cb,
					  g_steal_pointer (&task));
	}
}

/**
 * pk_control_get_tid_finish:
 * @control: a valid #PkControl instance
 * @res: the #GAsyncResult
 * @error: A #GError or %NULL
 *
 * Gets the result from the asynchronous function.
 *
 * Returns: (nullable): the ID, or %NULL if unset, free with g_free()
 *
 * Since: 0.5.2
 **/
gchar *
pk_control_get_tid_finish (PkControl *control,
			   GAsyncResult *res,
			   GError **error)
{
	g_return_val_if_fail (PK_IS_CONTROL (control), NULL);
	g_return_val_if_fail (g_task_is_valid (res, control), NULL);
	g_return_val_if_fail (g_async_result_is_tagged (res, pk_control_get_tid_async), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	return g_task_propagate_pointer (G_TASK (res), error);
}

/**********************************************************************/

/*
 * pk_control_suggest_daemon_quit_cb:
 **/
static void
pk_control_suggest_daemon_quit_cb (GObject *source_object,
				   GAsyncResult *res,
				   gpointer user_data)
{
	GDBusProxy *proxy = G_DBUS_PROXY (source_object);
	g_autoptr(GTask) task = G_TASK (user_data);
	g_autoptr(GError) error = NULL;
	g_autoptr(GVariant) value = NULL;

	/* get the result */
	value = g_dbus_proxy_call_finish (proxy, res, &error);
	if (value == NULL) {
		g_task_return_error (task, g_steal_pointer (&error));
		return;
	}

	/* we're done */
	g_task_return_boolean (task, TRUE);
}

/*
 * pk_control_suggest_daemon_quit_internal:
 **/
static void
pk_control_suggest_daemon_quit_internal (PkControl *control,
					 GTask *task)
{
	PkControlPrivate *priv = pk_control_get_instance_private (control);
	GCancellable *cancellable;

	g_assert (PK_IS_CONTROL (control));
	g_assert (G_IS_TASK (task));

	cancellable = g_task_get_cancellable (task);
	g_dbus_proxy_call (priv->proxy,
			   "SuggestDaemonQuit",
			   NULL,
			   G_DBUS_CALL_FLAGS_NONE,
			   PK_CONTROL_DBUS_METHOD_TIMEOUT,
			   cancellable,
			   pk_control_suggest_daemon_quit_cb,
			   g_steal_pointer (&task));
}

/*
 * pk_control_suggest_daemon_quit_proxy_cb:
 **/
static void
pk_control_suggest_daemon_quit_proxy_cb (GObject *source_object,
					 GAsyncResult *res,
					 gpointer user_data)
{
	g_autoptr(GTask) task = G_TASK (user_data);
	g_autoptr(GDBusProxy) proxy = NULL;
	g_autoptr(GError) error = NULL;
	PkControl *control = g_task_get_source_object (task);

	proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
	if (proxy == NULL) {
		g_task_return_error (task, g_steal_pointer (&error));
		return;
	}

	pk_control_proxy_connect (control, proxy);
	pk_control_suggest_daemon_quit_internal (control, g_steal_pointer (&task));
}

/**
 * pk_control_suggest_daemon_quit_async:
 * @control: a valid #PkControl instance
 * @cancellable: a #GCancellable or %NULL
 * @callback: the function to run on completion
 * @user_data: the data to pass to @callback
 *
 * Suggests to the daemon that it should quit as soon as possible.
 *
 * Since: 0.6.2
 **/
void
pk_control_suggest_daemon_quit_async (PkControl *control,
				      GCancellable *cancellable,
				      GAsyncReadyCallback callback,
				      gpointer user_data)
{
	PkControlPrivate *priv = pk_control_get_instance_private (control);
	g_autoptr(GTask) task = NULL;
	g_autoptr(GError) error = NULL;

	g_return_if_fail (PK_IS_CONTROL (control));
	g_return_if_fail (callback != NULL);
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

	/* check not already cancelled */
	if (cancellable != NULL &&
	    g_cancellable_set_error_if_cancelled (cancellable, &error)) {
		g_task_report_error (control, callback, user_data,
				     pk_control_suggest_daemon_quit_async,
				     g_steal_pointer (&error));
		return;
	}

	task = g_task_new (control, cancellable, callback, user_data);
	g_task_set_source_tag (task, pk_control_suggest_daemon_quit_async);

	/* skip straight to the D-Bus method if already connection */
	if (priv->proxy != NULL) {
		pk_control_suggest_daemon_quit_internal (control, g_steal_pointer (&task));
	} else {
		g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
					  G_DBUS_PROXY_FLAGS_NONE,
					  NULL,
					  PK_DBUS_SERVICE,
					  PK_DBUS_PATH,
					  PK_DBUS_INTERFACE,
					  priv->cancellable,
					  pk_control_suggest_daemon_quit_proxy_cb,
					  g_steal_pointer (&task));
	}
}

/**
 * pk_control_suggest_daemon_quit_finish:
 * @control: a valid #PkControl instance
 * @res: the #GAsyncResult
 * @error: A #GError or %NULL
 *
 * Gets the result from the asynchronous function.
 *
 * Returns: %TRUE if the suggestion was sent
 *
 * Since: 0.6.2
 **/
gboolean
pk_control_suggest_daemon_quit_finish (PkControl *control, GAsyncResult *res, GError **error)
{
	g_return_val_if_fail (PK_IS_CONTROL (control), FALSE);
	g_return_val_if_fail (g_task_is_valid (res, control), FALSE);
	g_return_val_if_fail (g_async_result_is_tagged (res, pk_control_suggest_daemon_quit_async), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	return g_task_propagate_boolean (G_TASK (res), error);
}

/**********************************************************************/

/*
 * pk_control_get_daemon_state_cb:
 **/
static void
pk_control_get_daemon_state_cb (GObject *source_object,
				   GAsyncResult *res,
				   gpointer user_data)
{
	GDBusProxy *proxy = G_DBUS_PROXY (source_object);
	g_autoptr (GTask) task = G_TASK (user_data);
	g_autoptr(GError) error = NULL;
	g_autoptr(GVariant) value = NULL;
	gchar *daemon_state = NULL;

	/* get the result */
	value = g_dbus_proxy_call_finish (proxy, res, &error);
	if (value == NULL) {
		/* fix up the D-Bus error */
		pk_control_fixup_dbus_error (error);
		g_task_return_error (task, g_steal_pointer (&error));
		return;
	}

	/* save results */
	g_variant_get (value, "(s)", &daemon_state);

	/* we're done */
	g_task_return_pointer (task, g_steal_pointer (&daemon_state), g_free);
}

/*
 * pk_control_get_daemon_state_internal:
 **/
static void
pk_control_get_daemon_state_internal (PkControl *control,
				      GTask *task)
{
	PkControlPrivate *priv = pk_control_get_instance_private (control);
	GCancellable *cancellable;

	g_assert (PK_IS_CONTROL (control));
	g_assert (G_IS_TASK (task));

	cancellable = g_task_get_cancellable (task);
	g_dbus_proxy_call (priv->proxy,
			   "GetDaemonState",
			   NULL,
			   G_DBUS_CALL_FLAGS_NONE,
			   PK_CONTROL_DBUS_METHOD_TIMEOUT,
			   cancellable,
			   pk_control_get_daemon_state_cb,
			   g_steal_pointer (&task));
}

/*
 * pk_control_get_daemon_state_proxy_cb:
 **/
static void
pk_control_get_daemon_state_proxy_cb (GObject *source_object,
			     GAsyncResult *res,
			     gpointer user_data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GTask) task = G_TASK (user_data);
	g_autoptr(GDBusProxy) proxy = NULL;
	PkControl *control = g_task_get_source_object (task);

	proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
	if (proxy == NULL) {
		g_task_return_error (task, g_steal_pointer (&error));
		return;
	}

	pk_control_proxy_connect (control, proxy);
	pk_control_get_daemon_state_internal (control, g_steal_pointer (&task));
}

/**
 * pk_control_get_daemon_state_async:
 * @control: a valid #PkControl instance
 * @cancellable: a #GCancellable or %NULL
 * @callback: the function to run on completion
 * @user_data: the data to pass to @callback
 *
 * Gets the debugging state from the daemon.
 *
 * Since: 0.5.2
 **/
void
pk_control_get_daemon_state_async (PkControl *control,
				   GCancellable *cancellable,
				   GAsyncReadyCallback callback,
				   gpointer user_data)
{
	PkControlPrivate *priv = pk_control_get_instance_private (control);
	g_autoptr(GTask) task = NULL;
	g_autoptr(GError) error = NULL;

	g_return_if_fail (PK_IS_CONTROL (control));
	g_return_if_fail (callback != NULL);
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

	/* check not already cancelled */
	if (cancellable != NULL &&
	    g_cancellable_set_error_if_cancelled (cancellable, &error)) {
		g_task_report_error (control, callback, user_data,
				     pk_control_get_daemon_state_async,
				     g_steal_pointer (&error));
		return;
	}

	task = g_task_new (control, cancellable, callback, user_data);
	g_task_set_source_tag (task, pk_control_get_daemon_state_async);

	/* skip straight to the D-Bus method if already connection */
	if (priv->proxy != NULL) {
		pk_control_get_daemon_state_internal (control, g_steal_pointer (&task));
	} else {
		g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
					  G_DBUS_PROXY_FLAGS_NONE,
					  NULL,
					  PK_DBUS_SERVICE,
					  PK_DBUS_PATH,
					  PK_DBUS_INTERFACE,
					  priv->cancellable,
					  pk_control_get_daemon_state_proxy_cb,
					  g_steal_pointer (&task));
	}
}

/**
 * pk_control_get_daemon_state_finish:
 * @control: a valid #PkControl instance
 * @res: the #GAsyncResult
 * @error: A #GError or %NULL
 *
 * Gets the result from the asynchronous function.
 *
 * Returns: (nullable): the ID, or %NULL if unset, free with g_free()
 *
 * Since: 0.5.2
 **/
gchar *
pk_control_get_daemon_state_finish (PkControl *control,
				    GAsyncResult *res,
				    GError **error)
{
	g_return_val_if_fail (PK_IS_CONTROL (control), NULL);
	g_return_val_if_fail (g_task_is_valid (res, control), NULL);
	g_return_val_if_fail (g_async_result_is_tagged (res, pk_control_get_daemon_state_async), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	return g_task_propagate_pointer (G_TASK (res), error);
}

/**********************************************************************/

/*
 * pk_control_set_proxy_cb:
 **/
static void
pk_control_set_proxy_cb (GObject *source_object,
				   GAsyncResult *res,
				   gpointer user_data)
{
	GDBusProxy *proxy = G_DBUS_PROXY (source_object);
	g_autoptr(GTask) task = G_TASK (user_data);
	g_autoptr(GError) error = NULL;
	g_autoptr(GVariant) value = NULL;

	/* get the result */
	value = g_dbus_proxy_call_finish (proxy, res, &error);
	if (value == NULL) {
		g_debug ("Failed to set proxy: %s", error->message);
		g_task_return_error (task, g_steal_pointer (&error));
		return;
	}

	/* we're done */
	g_task_return_boolean (task, TRUE);
}

/*
 * pk_control_set_proxy_internal:
 **/
static void
pk_control_set_proxy_internal (PkControl *control,
			       GTask *task)
{
	PkControlPrivate *priv = pk_control_get_instance_private (control);
	GCancellable *cancellable;
	GVariant *parameters;

	g_assert (PK_IS_CONTROL (control));
	g_assert (G_IS_TASK (task));

	cancellable = g_task_get_cancellable (task);
	parameters = g_task_get_task_data (task);
	g_dbus_proxy_call (priv->proxy,
			   "SetProxy",
			   parameters,
			   G_DBUS_CALL_FLAGS_NONE,
			   PK_CONTROL_DBUS_METHOD_TIMEOUT,
			   cancellable,
			   pk_control_set_proxy_cb,
			   g_steal_pointer (&task));
}

/*
 * pk_control_set_proxy_proxy_cb:
 **/
static void
pk_control_set_proxy_proxy_cb (GObject *source_object,
			     GAsyncResult *res,
			     gpointer user_data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GTask) task = G_TASK (user_data);
	g_autoptr(GDBusProxy) proxy = NULL;
	PkControl *control = g_task_get_source_object (task);

	proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
	if (proxy == NULL) {
		g_task_return_error (task, g_steal_pointer (&error));
		return;
	}

	pk_control_proxy_connect (control, proxy);
	pk_control_set_proxy_internal (control, g_steal_pointer (&task));
}

/**
 * pk_control_set_proxy2_async: (finish-func pk_control_set_proxy_finish):
 * @control: a valid #PkControl instance
 * @proxy_http: a HTTP proxy string such as "username:password@server.lan:8080", or %NULL
 * @proxy_https: a HTTPS proxy string such as "username:password@server.lan:8080", or %NULL
 * @proxy_ftp: a FTP proxy string such as "server.lan:8080", or %NULL
 * @proxy_socks: a SOCKS proxy string such as "server.lan:8080", or %NULL
 * @no_proxy: a list of download IPs that shouldn't go through the proxy, or %NULL
 * @pac: a PAC string, or %NULL
 * @cancellable: a #GCancellable or %NULL
 * @callback: the function to run on completion
 * @user_data: the data to pass to @callback
 *
 * Set a proxy on the PK daemon
 *
 * Since: 0.6.13
 **/
void
pk_control_set_proxy2_async (PkControl *control,
			     const gchar *proxy_http,
			     const gchar *proxy_https,
			     const gchar *proxy_ftp,
			     const gchar *proxy_socks,
			     const gchar *no_proxy,
			     const gchar *pac,
			     GCancellable *cancellable,
			     GAsyncReadyCallback callback,
			     gpointer user_data)
{
	PkControlPrivate *priv = pk_control_get_instance_private (control);
	g_autoptr(GTask) task = NULL;
	g_autoptr(GError) error = NULL;
	GVariant *parameters = NULL;

	g_return_if_fail (PK_IS_CONTROL (control));
	g_return_if_fail (callback != NULL);
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

	/* check not already cancelled */
	if (cancellable != NULL &&
	    g_cancellable_set_error_if_cancelled (cancellable, &error)) {
		g_task_report_error (control, callback, user_data,
				     pk_control_set_proxy_async,
				     g_steal_pointer (&error));
		return;
	}

	/* save state */
	parameters = g_variant_new ("(ssssss)",
				    proxy_http ? proxy_http : "",
				    proxy_https ? proxy_https : "",
				    proxy_ftp ? proxy_ftp : "",
				    proxy_socks ? proxy_socks : "",
				    no_proxy ? no_proxy : "",
				    pac ? pac : "");

	task = g_task_new (control, cancellable, callback, user_data);
	g_task_set_source_tag (task, pk_control_set_proxy_async);
	g_task_set_task_data (task, g_variant_ref_sink (parameters), (GDestroyNotify) g_variant_unref);

	/* skip straight to the D-Bus method if already connection */
	if (priv->proxy != NULL) {
		pk_control_set_proxy_internal (control, g_steal_pointer (&task));
	} else {
		g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
					  G_DBUS_PROXY_FLAGS_NONE,
					  NULL,
					  PK_DBUS_SERVICE,
					  PK_DBUS_PATH,
					  PK_DBUS_INTERFACE,
					  priv->cancellable,
					  pk_control_set_proxy_proxy_cb,
					  g_steal_pointer (&task));
	}
}

/**
 * pk_control_set_proxy_async:
 * @control: a valid #PkControl instance
 * @proxy_http: a HTTP proxy string such as "username:password@server.lan:8080"
 * @proxy_ftp: a FTP proxy string such as "server.lan:8080"
 * @cancellable: a #GCancellable or %NULL
 * @callback: the function to run on completion
 * @user_data: the data to pass to @callback
 *
 * Set a proxy on the PK daemon
 *
 * NOTE: This is just provided for backwards compatibility.
 * Clients should really be using pk_control_set_proxy2_async().
 *
 * Since: 0.5.2
 **/
void
pk_control_set_proxy_async (PkControl *control,
			    const gchar *proxy_http,
			    const gchar *proxy_ftp,
			    GCancellable *cancellable,
			    GAsyncReadyCallback callback,
			    gpointer user_data)
{
	pk_control_set_proxy2_async (control,
				     proxy_http,
				     NULL,
				     proxy_ftp,
				     NULL,
				     NULL,
				     NULL,
				     cancellable,
				     callback,
				     user_data);
}

/**
 * pk_control_set_proxy_finish:
 * @control: a valid #PkControl instance
 * @res: the #GAsyncResult
 * @error: A #GError or %NULL
 *
 * Gets the result from the asynchronous function.
 *
 * Returns: %TRUE if we set the proxy successfully
 *
 * Since: 0.5.2
 **/
gboolean
pk_control_set_proxy_finish (PkControl *control,
			     GAsyncResult *res,
			     GError **error)
{
	g_return_val_if_fail (PK_IS_CONTROL (control), FALSE);
	g_return_val_if_fail (g_task_is_valid (res, control), FALSE);
	g_return_val_if_fail (g_async_result_is_tagged (res, pk_control_set_proxy_async), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	return g_task_propagate_boolean (G_TASK (res), error);
}

/**********************************************************************/

/*
 * pk_control_get_transaction_list_cb:
 **/
static void
pk_control_get_transaction_list_cb (GObject *source_object,
				    GAsyncResult *res,
				    gpointer user_data)
{
	const gchar **tlist_tmp = NULL;
	GDBusProxy *proxy = G_DBUS_PROXY (source_object);
	g_autoptr(GTask) task = G_TASK (user_data);
	g_autoptr(GError) error = NULL;
	g_autoptr(GVariant) value = NULL;

	/* get the result */
	value = g_dbus_proxy_call_finish (proxy, res, &error);
	if (value == NULL) {
		/* fix up the D-Bus error */
		pk_control_fixup_dbus_error (error);
		g_task_return_error (task, g_steal_pointer (&error));
		return;
	}

	/* unwrap data */
	g_variant_get (value, "(^a&o)", &tlist_tmp);

	/* we're done */
	if (tlist_tmp == NULL) {
		g_task_return_pointer (task, g_new0 (gchar *, 1), g_free);
	} else {
		g_task_return_pointer (task, g_strdupv ((gchar **)tlist_tmp), (GDestroyNotify) g_strfreev);
	}
}

/*
 * pk_control_get_transaction_list_internal:
 **/
static void
pk_control_get_transaction_list_internal (PkControl *control,
					  GTask *task)
{
	PkControlPrivate *priv = pk_control_get_instance_private (control);
	GCancellable *cancellable;

	g_assert (PK_IS_CONTROL (control));
	g_assert (G_IS_TASK (task));

	cancellable = g_task_get_cancellable (task);
	g_dbus_proxy_call (priv->proxy,
			   "GetTransactionList",
			   NULL,
			   G_DBUS_CALL_FLAGS_NONE,
			   PK_CONTROL_DBUS_METHOD_TIMEOUT,
			   cancellable,
			   pk_control_get_transaction_list_cb,
			   g_steal_pointer (&task));
}

/*
 * pk_control_get_transaction_list_proxy_cb:
 **/
static void
pk_control_get_transaction_list_proxy_cb (GObject *source_object,
					  GAsyncResult *res,
					  gpointer user_data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GTask) task = G_TASK (user_data);
	g_autoptr(GDBusProxy) proxy = NULL;
	PkControl *control = g_task_get_source_object (task);

	proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
	if (proxy == NULL) {
		g_task_return_error (task, g_steal_pointer (&error));
		return;
	}

	pk_control_proxy_connect (control, proxy);
	pk_control_get_transaction_list_internal (control, g_steal_pointer (&task));
}

/**
 * pk_control_get_transaction_list_async:
 * @control: a valid #PkControl instance
 * @cancellable: a #GCancellable or %NULL
 * @callback: the function to run on completion
 * @user_data: the data to pass to @callback
 *
 * Gets the transactions currently running in the daemon.
 *
 * Since: 0.5.2
 **/
void
pk_control_get_transaction_list_async (PkControl *control,
				       GCancellable *cancellable,
				       GAsyncReadyCallback callback,
				       gpointer user_data)
{
	PkControlPrivate *priv = pk_control_get_instance_private (control);
	g_autoptr(GTask) task = NULL;
	g_autoptr(GError) error = NULL;

	g_return_if_fail (PK_IS_CONTROL (control));
	g_return_if_fail (callback != NULL);
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

	/* check not already cancelled */
	if (cancellable != NULL &&
	    g_cancellable_set_error_if_cancelled (cancellable, &error)) {
		g_task_report_error (control, callback, user_data,
				     pk_control_get_transaction_list_async,
				     g_steal_pointer (&error));
		return;
	}

	task = g_task_new (control, cancellable, callback, user_data);
	g_task_set_source_tag (task, pk_control_get_transaction_list_async);

	/* skip straight to the D-Bus method if already connection */
	if (priv->proxy != NULL) {
		pk_control_get_transaction_list_internal (control, g_steal_pointer (&task));
	} else {
		g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
					  G_DBUS_PROXY_FLAGS_NONE,
					  NULL,
					  PK_DBUS_SERVICE,
					  PK_DBUS_PATH,
					  PK_DBUS_INTERFACE,
					  priv->cancellable,
					  pk_control_get_transaction_list_proxy_cb,
					  g_steal_pointer (&task));
	}
}

/**
 * pk_control_get_transaction_list_finish:
 * @control: a valid #PkControl instance
 * @res: the #GAsyncResult
 * @error: A #GError or %NULL
 *
 * Gets the result from the asynchronous function.
 *
 * Returns: (transfer full): A GStrv list of transaction ID's, free with g_strfreev()
 *
 * Since: 0.5.2
 **/
gchar **
pk_control_get_transaction_list_finish (PkControl *control,
					GAsyncResult *res,
					GError **error)
{
	g_return_val_if_fail (PK_IS_CONTROL (control), NULL);
	g_return_val_if_fail (g_task_is_valid (res, control), NULL);
	g_return_val_if_fail (g_async_result_is_tagged (res, pk_control_get_transaction_list_async), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	return g_task_propagate_pointer (G_TASK (res), error);
}

/**********************************************************************/

/*
 * pk_control_get_time_since_action_cb:
 **/
static void
pk_control_get_time_since_action_cb (GObject *source_object,
				     GAsyncResult *res,
				     gpointer user_data)
{
	GDBusProxy *proxy = G_DBUS_PROXY (source_object);
	g_autoptr(GTask) task = G_TASK (user_data);
	g_autoptr(GVariant) value = NULL;
	g_autoptr(GError) error = NULL;
	guint time = 0;

	/* get the result */
	value = g_dbus_proxy_call_finish (proxy, res, &error);
	if (value == NULL) {
		/* fix up the D-Bus error */
		pk_control_fixup_dbus_error (error);
		g_task_return_error (task, g_steal_pointer (&error));
		return;
	}

	/* save data */
	g_variant_get (value, "(u)", &time);
	if (time == 0) {
		g_task_return_new_error (task,
					 PK_CONTROL_ERROR, PK_CONTROL_ERROR_FAILED,
					 "could not get time");
		return;
	}

	/* we're done */
	g_task_return_pointer (task, GUINT_TO_POINTER (time), NULL);
}

/*
 * pk_control_get_time_since_action_internal:
 **/
static void
pk_control_get_time_since_action_internal (PkControl *control,
					   GTask *task)
{
	PkControlPrivate *priv = pk_control_get_instance_private (control);
	GCancellable *cancellable;
	guint role;

	g_assert (PK_IS_CONTROL (control));
	g_assert (G_IS_TASK (task));

	cancellable = g_task_get_cancellable (task);
	role = GPOINTER_TO_UINT (g_task_get_task_data (task));
	g_dbus_proxy_call (priv->proxy,
			   "GetTimeSinceAction",
			   g_variant_new ("(u)", role),
			   G_DBUS_CALL_FLAGS_NONE,
			   PK_CONTROL_DBUS_METHOD_TIMEOUT,
			   cancellable,
			   pk_control_get_time_since_action_cb,
			   g_steal_pointer (&task));
}

/*
 * pk_control_get_time_since_action_proxy_cb:
 **/
static void
pk_control_get_time_since_action_proxy_cb (GObject *source_object,
					   GAsyncResult *res,
					   gpointer user_data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GTask) task = G_TASK (user_data);
	g_autoptr(GDBusProxy) proxy = NULL;
	PkControl *control = g_task_get_source_object (task);

	proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
	if (proxy == NULL) {
		g_task_return_error (task, g_steal_pointer (&error));
		return;
	}

	pk_control_proxy_connect (control, proxy);
	pk_control_get_time_since_action_internal (control, g_steal_pointer (&task));
}

/**
 * pk_control_get_time_since_action_async:
 * @control: a valid #PkControl instance
 * @role: the role enum, e.g. %PK_ROLE_ENUM_GET_UPDATES
 * @cancellable: a #GCancellable or %NULL
 * @callback: the function to run on completion
 * @user_data: the data to pass to @callback
 *
 * We may want to know how long it has been since we refreshed the cache or
 * retrieved the update list.
 *
 * Since: 0.5.2
 **/
void
pk_control_get_time_since_action_async (PkControl *control,
					PkRoleEnum role,
					GCancellable *cancellable,
					GAsyncReadyCallback callback,
					gpointer user_data)
{
	PkControlPrivate *priv = pk_control_get_instance_private (control);
	g_autoptr(GTask) task = NULL;
	g_autoptr(GError) error = NULL;

	g_return_if_fail (PK_IS_CONTROL (control));
	g_return_if_fail (callback != NULL);
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

	/* check not already cancelled */
	if (cancellable != NULL &&
	    g_cancellable_set_error_if_cancelled (cancellable, &error)) {
		g_task_report_error (control, callback, user_data,
				     pk_control_get_time_since_action_async,
				     g_steal_pointer (&error));
		return;
	}

	task = g_task_new (control, cancellable, callback, user_data);
	g_task_set_source_tag (task, pk_control_get_time_since_action_async);
	g_task_set_task_data (task, GUINT_TO_POINTER ((guint) role), NULL);

	/* skip straight to the D-Bus method if already connection */
	if (priv->proxy != NULL) {
		pk_control_get_time_since_action_internal (control, g_steal_pointer (&task));
	} else {
		g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
					  G_DBUS_PROXY_FLAGS_NONE,
					  NULL,
					  PK_DBUS_SERVICE,
					  PK_DBUS_PATH,
					  PK_DBUS_INTERFACE,
					  priv->cancellable,
					  pk_control_get_time_since_action_proxy_cb,
					  g_steal_pointer (&task));
	}
}

/**
 * pk_control_get_time_since_action_finish:
 * @control: a valid #PkControl instance
 * @res: the #GAsyncResult
 * @error: A #GError or %NULL
 *
 * Gets the result from the asynchronous function.
 *
 * Returns: %TRUE if the daemon serviced the request
 *
 * Since: 0.5.2
 **/
guint
pk_control_get_time_since_action_finish (PkControl *control,
					 GAsyncResult *res,
					 GError **error)
{
	g_return_val_if_fail (PK_IS_CONTROL (control), 0);
	g_return_val_if_fail (g_task_is_valid (res, control), 0);
	g_return_val_if_fail (g_async_result_is_tagged (res, pk_control_get_time_since_action_async), 0);
	g_return_val_if_fail (error == NULL || *error == NULL, 0);

	return GPOINTER_TO_UINT(g_task_propagate_pointer (G_TASK (res), error));
}

/**********************************************************************/

/*
 * pk_control_can_authorize_cb:
 **/
static void
pk_control_can_authorize_cb (GObject *source_object,
			     GAsyncResult *res,
			     gpointer user_data)
{
	GDBusProxy *proxy = G_DBUS_PROXY (source_object);
	g_autoptr(GTask) task = G_TASK (user_data);
	g_autoptr(GError) error = NULL;
	g_autoptr(GVariant) value = NULL;
	guint authorize = PK_AUTHORIZE_ENUM_UNKNOWN;

	/* get the result */
	value = g_dbus_proxy_call_finish (proxy, res, &error);
	if (value == NULL) {
		/* fix up the D-Bus error */
		pk_control_fixup_dbus_error (error);
		g_task_return_error (task, g_steal_pointer (&error));
		return;
	}

	/* save data */
	g_variant_get (value, "(u)", &authorize);
	if (authorize == PK_AUTHORIZE_ENUM_UNKNOWN) {
		g_task_return_new_error (task,
					 PK_CONTROL_ERROR, PK_CONTROL_ERROR_FAILED,
					 "could not get state");
		return;
	}

	/* we're done */
	g_task_return_pointer (task, GUINT_TO_POINTER (authorize), NULL);
}

/*
 * pk_control_can_authorize_internal:
 **/
static void
pk_control_can_authorize_internal (PkControl *control,
				   GTask *task)
{
	PkControlPrivate *priv = pk_control_get_instance_private (control);
	GCancellable *cancellable;
	const gchar *action_id;

	g_assert (PK_IS_CONTROL (control));
	g_assert (G_IS_TASK (task));

	cancellable = g_task_get_cancellable (task);
	action_id = g_task_get_task_data (task);
	g_dbus_proxy_call (priv->proxy,
			   "CanAuthorize",
			   g_variant_new ("(s)", action_id),
			   G_DBUS_CALL_FLAGS_NONE,
			   PK_CONTROL_DBUS_METHOD_TIMEOUT,
			   cancellable,
			   pk_control_can_authorize_cb,
			   g_steal_pointer (&task));
}

/*
 * pk_control_can_authorize_proxy_cb:
 **/
static void
pk_control_can_authorize_proxy_cb (GObject *source_object,
				   GAsyncResult *res,
				   gpointer user_data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GTask) task = G_TASK (user_data);
	g_autoptr(GDBusProxy) proxy = NULL;
	PkControl *control = g_task_get_source_object (task);

	proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
	if (proxy == NULL) {
		g_task_return_error (task, g_steal_pointer (&error));
		return;
	}

	pk_control_proxy_connect (control, proxy);
	pk_control_can_authorize_internal (control, g_steal_pointer (&task));
}

/**
 * pk_control_can_authorize_async:
 * @control: a valid #PkControl instance
 * @action_id: The action ID, for instance "org.freedesktop.PackageKit.install-untrusted"
 * @cancellable: a #GCancellable or %NULL
 * @callback: the function to run on completion
 * @user_data: the data to pass to @callback
 *
 * We may want to know before we run a method if we are going to be denied,
 * accepted or challenged for authentication.
 *
 * Since: 0.5.2
 **/
void
pk_control_can_authorize_async (PkControl *control,
				const gchar *action_id,
				GCancellable *cancellable,
				GAsyncReadyCallback callback,
				gpointer user_data)
{
	PkControlPrivate *priv = pk_control_get_instance_private (control);
	g_autoptr(GTask) task = NULL;
	g_autoptr(GError) error = NULL;

	g_return_if_fail (PK_IS_CONTROL (control));
	g_return_if_fail (callback != NULL);
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

	/* check not already cancelled */
	if (cancellable != NULL &&
	    g_cancellable_set_error_if_cancelled (cancellable, &error)) {
		g_task_report_error (control, callback, user_data,
				     pk_control_can_authorize_async,
				     g_steal_pointer (&error));
		return;
	}

	task = g_task_new (control, cancellable, callback, user_data);
	g_task_set_source_tag (task, pk_control_can_authorize_async);
	g_task_set_task_data (task, g_strdup (action_id), g_free);

	/* skip straight to the D-Bus method if already connection */
	if (priv->proxy != NULL) {
		pk_control_can_authorize_internal (control, g_steal_pointer (&task));
	} else {
		g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
					  G_DBUS_PROXY_FLAGS_NONE,
					  NULL,
					  PK_DBUS_SERVICE,
					  PK_DBUS_PATH,
					  PK_DBUS_INTERFACE,
					  priv->cancellable,
					  pk_control_can_authorize_proxy_cb,
					  g_steal_pointer (&task));
	}
}

/**
 * pk_control_can_authorize_finish:
 * @control: a valid #PkControl instance
 * @res: the #GAsyncResult
 * @error: A #GError or %NULL
 *
 * Gets the result from the asynchronous function.
 *
 * Returns: the #PkAuthorizeEnum or %PK_AUTHORIZE_ENUM_UNKNOWN if the method failed
 *
 * Since: 0.5.2
 **/
PkAuthorizeEnum
pk_control_can_authorize_finish (PkControl *control, GAsyncResult *res, GError **error)
{
	gpointer result;

	g_return_val_if_fail (PK_IS_CONTROL (control), PK_AUTHORIZE_ENUM_UNKNOWN);
	g_return_val_if_fail (g_task_is_valid (res, control), PK_AUTHORIZE_ENUM_UNKNOWN);
	g_return_val_if_fail (g_async_result_is_tagged (res, pk_control_can_authorize_async), PK_AUTHORIZE_ENUM_UNKNOWN);

	result = g_task_propagate_pointer (G_TASK (res), error);
	if (!result)
		return PK_AUTHORIZE_ENUM_UNKNOWN;

	return (PkAuthorizeEnum) GPOINTER_TO_UINT(result);
}

/**********************************************************************/

/*
 * pk_control_get_properties_cb:
 **/
static void
pk_control_get_properties_cb (GObject *source_object,
			      GAsyncResult *res,
			      gpointer user_data)
{
	g_autoptr(GTask) task = G_TASK (user_data);
	g_autoptr(GError) error = NULL;
	g_autoptr(GDBusProxy) proxy = NULL;
	PkControl *control = g_task_get_source_object (task);

	proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
	if (proxy == NULL) {
		g_task_return_error (task, g_steal_pointer (&error));
		return;
	}

	pk_control_proxy_connect (control, proxy);

	/* we're done */
	g_task_return_boolean (task, TRUE);
}

/**
 * pk_control_get_properties_async:
 * @control: a valid #PkControl instance
 * @cancellable: a #GCancellable or %NULL
 * @callback: the function to run on completion
 * @user_data: the data to pass to @callback
 *
 * Gets global properties from the daemon.
 *
 * Since: 0.5.2
 **/
void
pk_control_get_properties_async (PkControl *control,
				 GCancellable *cancellable,
				 GAsyncReadyCallback callback,
				 gpointer user_data)
{
	PkControlPrivate *priv = pk_control_get_instance_private (control);
	g_autoptr(GTask) task = NULL;
	g_autoptr(GError) error = NULL;

	g_return_if_fail (PK_IS_CONTROL (control));
	g_return_if_fail (callback != NULL);
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

	/* check not already cancelled */
	if (cancellable != NULL &&
	    g_cancellable_set_error_if_cancelled (cancellable, &error)) {
		g_task_report_error (control, callback, user_data,
				     pk_control_get_properties_async,
				     g_steal_pointer (&error));
		return;
	}

	task = g_task_new (control, cancellable, callback, user_data);
	g_task_set_source_tag (task, pk_control_get_properties_async);

	/* already done */
	if (priv->proxy != NULL) {
		g_task_return_boolean (task, TRUE);
		return;
	}

	/* get a connection to the main interface */
	g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
				  G_DBUS_PROXY_FLAGS_NONE,
				  NULL,
				  PK_DBUS_SERVICE,
				  PK_DBUS_PATH,
				  PK_DBUS_INTERFACE,
				  priv->cancellable,
				  pk_control_get_properties_cb,
				  g_steal_pointer (&task));
}

/**
 * pk_control_get_properties_finish:
 * @control: a valid #PkControl instance
 * @res: the #GAsyncResult
 * @error: A #GError or %NULL
 *
 * Gets the result from the asynchronous function.
 *
 * Returns: %TRUE if we set the proxy successfully
 *
 * Since: 0.5.2
 **/
gboolean
pk_control_get_properties_finish (PkControl *control, GAsyncResult *res, GError **error)
{
	g_return_val_if_fail (PK_IS_CONTROL (control), FALSE);
	g_return_val_if_fail (g_task_is_valid (res, control), FALSE);
	g_return_val_if_fail (g_async_result_is_tagged (res, pk_control_get_properties_async), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	return g_task_propagate_boolean (G_TASK (res), error);
}

/**********************************************************************/

/*
 * pk_control_get_property:
 **/
static void
pk_control_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	PkControl *control = PK_CONTROL (object);
	PkControlPrivate *priv = pk_control_get_instance_private (control);

	switch (prop_id) {
	case PROP_VERSION_MAJOR:
		g_value_set_uint (value, priv->version_major);
		break;
	case PROP_VERSION_MINOR:
		g_value_set_uint (value, priv->version_minor);
		break;
	case PROP_VERSION_MICRO:
		g_value_set_uint (value, priv->version_micro);
		break;
	case PROP_BACKEND_NAME:
		g_value_set_string (value, priv->backend_name);
		break;
	case PROP_BACKEND_DESCRIPTION:
		g_value_set_string (value, priv->backend_description);
		break;
	case PROP_BACKEND_AUTHOR:
		g_value_set_string (value, priv->backend_author);
		break;
	case PROP_ROLES:
		g_value_set_uint64 (value, priv->roles);
		break;
	case PROP_GROUPS:
		g_value_set_uint64 (value, priv->groups);
		break;
	case PROP_FILTERS:
		g_value_set_uint64 (value, priv->filters);
		break;
	case PROP_PROVIDES:
		g_value_set_uint64 (value, priv->provides);
		break;
	case PROP_MIME_TYPES:
		g_value_set_boxed (value, priv->mime_types);
		break;
	case PROP_LOCKED:
		g_value_set_boolean (value, priv->locked);
		break;
	case PROP_NETWORK_STATE:
		g_value_set_enum (value, priv->network_state);
		break;
	case PROP_DISTRO_ID:
		g_value_set_string (value, priv->distro_id);
		break;
	case PROP_CONNECTED:
		g_value_set_boolean (value, priv->connected);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

/*
 * pk_control_set_property:
 **/
static void
pk_control_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	switch (prop_id) {
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

/*
 * pk_control_class_init:
 **/
static void
pk_control_class_init (PkControlClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->get_property = pk_control_get_property;
	object_class->set_property = pk_control_set_property;
	object_class->finalize = pk_control_finalize;

	/**
	 * PkControl:version-major:
	 *
	 * Since: 0.5.2
	 */
	obj_properties[PROP_VERSION_MAJOR] =
		g_param_spec_uint ("version-major", NULL, NULL,
				   0, G_MAXUINT, 0,
				   G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

	/**
	 * PkControl:version-minor:
	 *
	 * Since: 0.5.2
	 */
	obj_properties[PROP_VERSION_MINOR] =
		g_param_spec_uint ("version-minor", NULL, NULL,
				   0, G_MAXUINT, 0,
				   G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

	/**
	 * PkControl:version-micro:
	 *
	 * Since: 0.5.2
	 */
	obj_properties[PROP_VERSION_MICRO] =
		g_param_spec_uint ("version-micro", NULL, NULL,
				   0, G_MAXUINT, 0,
				   G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

	/**
	 * PkControl:backend-name:
	 *
	 * Since: 0.5.2
	 */
	obj_properties[PROP_BACKEND_NAME] =
		g_param_spec_string ("backend-name", NULL, NULL,
				     NULL,
				     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * PkControl:backend-description:
	 *
	 * Since: 0.5.2
	 */
	obj_properties[PROP_BACKEND_DESCRIPTION] =
		g_param_spec_string ("backend-description", NULL, NULL,
				     NULL,
				     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * PkControl:backend-author:
	 *
	 * Since: 0.5.2
	 */
	obj_properties[PROP_BACKEND_AUTHOR] =
		g_param_spec_string ("backend-author", NULL, NULL,
				     NULL,
				     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * PkControl:roles:
	 *
	 * Since: 0.5.2
	 */
	obj_properties[PROP_ROLES] =
		g_param_spec_uint64 ("roles", NULL, NULL,
				     0, G_MAXUINT64, 0,
				     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * PkControl:groups:
	 *
	 * Since: 0.5.2
	 */
	obj_properties[PROP_GROUPS] =
		g_param_spec_uint64 ("groups", NULL, NULL,
				     0, G_MAXUINT64, 0,
				     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * PkControl:filters:
	 *
	 * Since: 0.5.2
	 */
	obj_properties[PROP_FILTERS] =
		g_param_spec_uint64 ("filters", NULL, NULL,
				     0, G_MAXUINT64, 0,
				     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * PkControl:provides:
	 *
	 * Since: 0.8.8
	 */
	obj_properties[PROP_PROVIDES] =
		g_param_spec_uint64 ("provides", NULL, NULL,
				     0, G_MAXUINT64, 0,
				     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * PkControl:mime-types:
	 *
	 * Since: 0.8.1
	 */
	obj_properties[PROP_MIME_TYPES] =
		g_param_spec_boxed ("mime-types", NULL, NULL,
				    G_TYPE_STRV,
				    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * PkControl:locked:
	 *
	 * Since: 0.5.3
	 */
	obj_properties[PROP_LOCKED] =
		g_param_spec_boolean ("locked", NULL, NULL,
				      FALSE,
				      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * PkControl:network-state:
	 *
	 * Since: 0.5.3
	 */
	obj_properties[PROP_NETWORK_STATE] =
		g_param_spec_enum ("network-state", NULL, NULL,
				   PK_TYPE_NETWORK_ENUM, PK_NETWORK_ENUM_LAST,
				   G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * PkControl:distro-id:
	 *
	 * Since: 0.5.5
	 */
	obj_properties[PROP_DISTRO_ID] =
		g_param_spec_string ("distro-id", NULL, NULL,
				     NULL,
				     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * PkControl:connected:
	 *
	 * Since: 0.5.3
	 */
	obj_properties[PROP_CONNECTED] =
		g_param_spec_boolean ("connected", NULL, NULL,
				      FALSE,
				      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties (object_class, PROP_LAST, obj_properties);

	/**
	 * PkControl::installed-changed:
	 * @control: the #PkControl instance that emitted the signal
	 *
	 * The ::installed-changed signal is emitted when the list of installed apps may have
	 * changed and the control program may have to update some UI.
	 *
	 * Since: 1.2.9
	 **/
	signals[SIGNAL_INSTALLED_CHANGED] =
		g_signal_new ("installed-changed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (PkControlClass, installed_changed),
			      NULL, NULL, g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);
	/**
	 * PkControl::updates-changed:
	 * @control: the #PkControl instance that emitted the signal
	 *
	 * The ::updates-changed signal is emitted when the update list may have
	 * changed and the control program may have to update some UI.
	 **/
	signals[SIGNAL_UPDATES_CHANGED] =
		g_signal_new ("updates-changed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (PkControlClass, updates_changed),
			      NULL, NULL, g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);
	/**
	 * PkControl::repo-list-changed:
	 * @control: the #PkControl instance that emitted the signal
	 *
	 * The ::repo-list-changed signal is emitted when the repo list may have
	 * changed and the control program may have to update some UI.
	 **/
	signals[SIGNAL_REPO_LIST_CHANGED] =
		g_signal_new ("repo-list-changed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (PkControlClass, repo_list_changed),
			      NULL, NULL, g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);
	/**
	 * PkControl::restart-schedule:
	 * @control: the #PkControl instance that emitted the signal
	 *
	 * The ::restart_schedule signal is emitted when the packagekitd service
	 * has been restarted because it has been upgraded.
	 * Client programs should reload themselves when it is convenient to
	 * do so, as old client tools may not be compatible with the new daemon.
	 **/
	signals[SIGNAL_RESTART_SCHEDULE] =
		g_signal_new ("restart-schedule",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (PkControlClass, restart_schedule),
			      NULL, NULL, g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);
	/**
	 * PkControl::transaction-list-changed:
	 * @control: the #PkControl instance that emitted the signal
	 * @transaction_ids: an #GStrv array of transaction ID's
	 *
	 * The ::transaction-list-changed signal is emitted when the list
	 * of transactions handled by the daemon is changed.
	 **/
	signals[SIGNAL_TRANSACTION_LIST_CHANGED] =
		g_signal_new ("transaction-list-changed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (PkControlClass, transaction_list_changed),
			      NULL, NULL, g_cclosure_marshal_VOID__BOXED,
			      G_TYPE_NONE, 1, G_TYPE_STRV);
}

/*
 * pk_control_name_appeared_cb:
 **/
static void
pk_control_name_appeared_cb (GDBusConnection *connection,
			     const gchar *name,
			     const gchar *name_owner,
			     gpointer user_data)
{
	PkControl *control = PK_CONTROL (user_data);
	PkControlPrivate *priv = pk_control_get_instance_private (control);
	priv->connected = TRUE;
	g_debug ("notify::connected");
	g_object_notify_by_pspec (G_OBJECT(control), obj_properties[PROP_CONNECTED]);
}

/*
 * pk_control_proxy_destroy:
 **/
static void
pk_control_proxy_destroy (PkControl *control)
{
	PkControlPrivate *priv = pk_control_get_instance_private (control);

	if (priv->proxy == NULL)
		return;

	g_signal_handlers_disconnect_by_func (priv->proxy,
					      G_CALLBACK (pk_control_properties_changed_cb),
					      control);
	g_signal_handlers_disconnect_by_func (priv->proxy,
					      G_CALLBACK (pk_control_signal_cb),
					      control);
	g_clear_object (&priv->proxy);
}

/*
 * pk_control_name_vanished_cb:
 **/
static void
pk_control_name_vanished_cb (GDBusConnection *connection,
			     const gchar *name,
			     gpointer user_data)
{
	PkControl *control = PK_CONTROL (user_data);
	PkControlPrivate *priv = pk_control_get_instance_private (control);

	priv->connected = FALSE;
	g_debug ("notify::connected");
	g_object_notify_by_pspec (G_OBJECT(control), obj_properties[PROP_CONNECTED]);

	/* destroy the proxy, as even though it's "well known" we get a
	 * GDBus.Error:org.freedesktop.DBus.Error.ServiceUnknown if we try to
	 * use this after the server has restarted */
	pk_control_proxy_destroy (control);
}

/*
 * pk_control_init:
 * @control: This class instance
 **/
static void
pk_control_init (PkControl *control)
{
	PkControlPrivate *priv = pk_control_get_instance_private (control);
	priv->network_state = PK_NETWORK_ENUM_UNKNOWN;
	priv->version_major = G_MAXUINT;
	priv->version_minor = G_MAXUINT;
	priv->version_micro = G_MAXUINT;
	priv->cancellable = g_cancellable_new ();
	priv->watch_id = g_bus_watch_name (G_BUS_TYPE_SYSTEM,
					   PK_DBUS_SERVICE,
					   G_BUS_NAME_WATCHER_FLAGS_NONE,
					   pk_control_name_appeared_cb,
					   pk_control_name_vanished_cb,
					   control,
					   NULL);
	control->priv = priv;
}

/*
 * pk_control_finalize:
 * @object: The object to finalize
 **/
static void
pk_control_finalize (GObject *object)
{
	PkControl *control = PK_CONTROL (object);
	PkControlPrivate *priv = pk_control_get_instance_private (control);

	/* ensure we cancel any in-flight DBus calls */
	g_cancellable_cancel (priv->cancellable);
	g_bus_unwatch_name (priv->watch_id);

	/* disconnect proxy and destroy it */
	pk_control_proxy_destroy (control);

	g_clear_pointer (&priv->backend_name, g_free);
	g_clear_pointer (&priv->backend_description, g_free);
	g_clear_pointer (&priv->backend_author, g_free);
	g_clear_pointer (&priv->mime_types, g_strfreev);
	g_clear_pointer (&priv->distro_id, g_free);
	g_clear_object (&priv->cancellable);

	G_OBJECT_CLASS (pk_control_parent_class)->finalize (object);
}

/**
 * pk_control_new:
 *
 * Returns: a new #PkControl object.
 *
 * Since: 0.5.2
 **/
PkControl *
pk_control_new (void)
{
	if (pk_control_object != NULL) {
		g_object_ref (pk_control_object);
	} else {
		pk_control_object = g_object_new (PK_TYPE_CONTROL, NULL);
		g_object_add_weak_pointer (pk_control_object, &pk_control_object);
	}
	return PK_CONTROL (pk_control_object);
}
