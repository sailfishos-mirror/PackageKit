/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 Ali Sabil <ali.sabil@gmail.com>
 * Copyright (C) 2007 Tom Parker <palfrey@tevp.net>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <pk-backend.h>
#include <pk-backend-dbus.h>

static PkBackendDbus *dbus;

#define PK_DBUS_BACKEND_SERVICE_APT   "org.freedesktop.PackageKitAptBackend"

/**
 * backend_initialize:
 * This should only be run once per backend load, i.e. not every transaction
 */
static void
backend_initialize (PkBackend *backend)
{
	pk_debug ("FILTER: initialize");
	dbus = pk_backend_dbus_new ();
	pk_backend_dbus_set_name (dbus, PK_DBUS_BACKEND_SERVICE_APT);
}

/**
 * backend_destroy:
 * This should only be run once per backend load, i.e. not every transaction
 */
static void
backend_destroy (PkBackend *backend)
{
	pk_debug ("FILTER: destroy");
	pk_backend_dbus_kill (dbus);
	g_object_unref (dbus);
}

/**
 * backend_get_groups:
 */
static PkGroupEnum
backend_get_groups (PkBackend *backend)
{
	return (PK_GROUP_ENUM_ACCESSORIES |
		PK_GROUP_ENUM_GAMES |
		PK_GROUP_ENUM_GRAPHICS |
		PK_GROUP_ENUM_INTERNET |
		PK_GROUP_ENUM_OFFICE |
		PK_GROUP_ENUM_OTHER |
		PK_GROUP_ENUM_PROGRAMMING |
		PK_GROUP_ENUM_MULTIMEDIA |
		PK_GROUP_ENUM_SYSTEM);
}

/**
 * backend_get_filters:
 */
static PkFilterEnum
backend_get_filters (PkBackend *backend)
{
	return (PK_FILTER_ENUM_GUI |
		PK_FILTER_ENUM_INSTALLED |
		PK_FILTER_ENUM_DEVELOPMENT);
}

/**
 * backend_get_updates:
 */
static void
backend_get_updates (PkBackend *backend, PkFilterEnum filters)
{
	pk_backend_dbus_get_updates (dbus, filters);
}

/**
 * backend_refresh_cache:
 * */
static void
backend_refresh_cache (PkBackend *backend, gboolean force)
{
	// check network state
	if (!pk_backend_is_online (backend)) {
		pk_backend_error_code (backend, PK_ERROR_ENUM_NO_NETWORK, "Cannot refresh cache whilst offline");
		pk_backend_finished (backend);
		return;
	}

	pk_backend_dbus_refresh_cache(dbus, force);
}

/**
 * pk_backend_update_system:
 * */
static void
backend_update_system (PkBackend *backend)
{
	pk_backend_dbus_update_system (dbus);
}

/**
 * backend_install_package
 *  */
static void
backend_install_package (PkBackend *backend, const gchar *package_id)
{
	pk_backend_dbus_install_package (dbus, package_id);
}

/**
 * backend_remove_package
 *  */
static void
backend_remove_package (PkBackend *backend, const gchar *package_id, gboolean allow_deps, gboolean autoremove)
{
	pk_backend_dbus_remove_package (dbus, package_id, allow_deps, autoremove);
}

/**
 * backend_get_details:
 *  */
static void
backend_get_details (PkBackend *backend, const gchar *package_id)
{
	pk_backend_dbus_get_details (dbus, package_id);
}

/**
 *  * pk_backend_search_details:
 *   */
static void
backend_search_details (PkBackend *backend, PkFilterEnum filters, const gchar *search)
{
	pk_backend_dbus_search_details (dbus, filters, search);
}

/**
 *  * pk_backend_search_name:
 *   */
static void
backend_search_name (PkBackend *backend, PkFilterEnum filters, const gchar *search)
{
	pk_backend_dbus_search_name (dbus, filters, search);
}

/**
 *  * pk_backend_cancel:
 *   */
static void
backend_cancel (PkBackend *backend)
{
	pk_backend_dbus_cancel (dbus);
}

/**
 *  * pk_backend_resolve:
 *   */
static void
backend_resolve (PkBackend *backend, PkFilterEnum filters, const gchar *package_id)
{
	        pk_backend_dbus_resolve (dbus, filters, package_id);
}

/**
 *  * pk_backend_get_packages:
 *   */
static void
backend_get_packages (PkBackend *backend, PkFilterEnum filters)
{
	        pk_backend_dbus_get_packages (dbus, filters);
}



PK_BACKEND_OPTIONS (
	"Apt",					/* description */
	"Ali Sabil <ali.sabil@gmail.com>; Tom Parker <palfrey@tevp.net>; Sebastian Heinlein <glatzor@ubuntu.com>",	/* author */
	backend_initialize,			/* initalize */
	backend_destroy,			/* destroy */
	backend_get_groups,			/* get_groups */
	backend_get_filters,			/* get_filters */
	backend_cancel,				/* cancel */
	NULL,					/* get_depends */
	backend_get_details,			/* get_details */
	NULL,					/* get_files */
	backend_get_packages,			/* get_packages */
	NULL,					/* get_repo_list */
	NULL,					/* get_requires */
	NULL,					/* get_update_detail */
	backend_get_updates,			/* get_updates */
	NULL,					/* install_file */
	backend_install_package,		/* install_package */
	NULL,					/* install_signature */
	backend_refresh_cache,			/* refresh_cache */
	backend_remove_package,			/* remove_package */
	NULL,					/* repo_enable */
	NULL,					/* repo_set_data */
	backend_resolve,			/* resolve */
	NULL,					/* rollback */
	backend_search_details,			/* search_details */
	NULL,					/* search_file */
	NULL,					/* search_group */
	backend_search_name,			/* search_name */
	NULL,					/* service_pack */
	NULL,					/* update_packages */
	backend_update_system,			/* update_system */
	NULL					/* what_provides */
);
