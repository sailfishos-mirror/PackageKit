/* pk-backend-apt.cpp
 *
 * Copyright (C) 2007-2008 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2009-2016 Daniel Nicoletti <dantti12@gmail.com>
 * Copyright (C) 2016 Harald Sitter <sitter@kde.org>
 * Copyright (C) 2018-2025 Matthias Klumpp <matthias@tenstral.net>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>

#include <config.h>
#include <pk-backend.h>
#include <packagekit-glib2/pk-debug.h>

#include <apt-pkg/aptconfiguration.h>
#include <apt-pkg/error.h>
#include <apt-pkg/fileutl.h>
#include <apt-pkg/init.h>
#include <apt-pkg/pkgsystem.h>

#include "apt-job.h"
#include "apt-cache-file.h"
#include "apt-messages.h"
#include "acqpkitstatus.h"
#include "apt-sourceslist.h"


const gchar* pk_backend_get_description(PkBackend *backend)
{
    return "APT";
}

const gchar* pk_backend_get_author(PkBackend *backend)
{
    return "Daniel Nicoletti <dantti12@gmail.com>, "
           "Matthias Klumpp <mak@debian.org>";
}

gboolean
pk_backend_supports_parallelization (PkBackend *backend)
{
    // we need to set this to TRUE as soon as the parallelization work is completed!
    return FALSE;
}

void pk_backend_initialize(GKeyFile *conf, PkBackend *backend)
{
    /* use logging */
    pk_debug_add_log_domain (G_LOG_DOMAIN);
    pk_debug_add_log_domain ("APT");

    g_debug("Using APT: %s", pkgVersion);

    // Disable apt-listbugs as it freezes PK
    g_setenv("APT_LISTBUGS_FRONTEND", "none", 1);

    // Set apt-listchanges frontend to "debconf" to make it's output visible
    // (without using the debconf frontend, PK will freeze)
    g_setenv("APT_LISTCHANGES_FRONTEND", "debconf", 1);

    // pkgInitConfig makes sure the config is ready for the
    // get-filters call which needs to know about multi-arch
    if (!pkgInitConfig(*_config)) {
        g_debug("ERROR initializing backend configuration");
    }

    // pkgInitSystem is needed to compare the changelog verstion to
    // current package using DoCmpVersion()
    if (!pkgInitSystem(*_config, _system)) {
        g_debug("ERROR initializing backend system");
    }
}

void pk_backend_destroy(PkBackend *backend)
{
    g_debug("APT backend being destroyed");
}

PkBitfield pk_backend_get_groups(PkBackend *backend)
{
    return pk_bitfield_from_enums(
                PK_GROUP_ENUM_ACCESSORIES,
                PK_GROUP_ENUM_ADMIN_TOOLS,
                PK_GROUP_ENUM_COMMUNICATION,
                PK_GROUP_ENUM_DOCUMENTATION,
                PK_GROUP_ENUM_DESKTOP_GNOME,
                PK_GROUP_ENUM_DESKTOP_KDE,
                PK_GROUP_ENUM_DESKTOP_OTHER,
                PK_GROUP_ENUM_ELECTRONICS,
                PK_GROUP_ENUM_FONTS,
                PK_GROUP_ENUM_GAMES,
                PK_GROUP_ENUM_GRAPHICS,
                PK_GROUP_ENUM_INTERNET,
                PK_GROUP_ENUM_LEGACY,
                PK_GROUP_ENUM_LOCALIZATION,
                PK_GROUP_ENUM_MULTIMEDIA,
                PK_GROUP_ENUM_NETWORK,
                PK_GROUP_ENUM_OTHER,
                PK_GROUP_ENUM_PROGRAMMING,
                PK_GROUP_ENUM_PUBLISHING,
                PK_GROUP_ENUM_SCIENCE,
                PK_GROUP_ENUM_SYSTEM,
                -1);
}

PkBitfield pk_backend_get_filters(PkBackend *backend)
{
    PkBitfield filters;
    filters = pk_bitfield_from_enums(
                PK_FILTER_ENUM_GUI,
                PK_FILTER_ENUM_INSTALLED,
                PK_FILTER_ENUM_DEVELOPMENT,
                PK_FILTER_ENUM_SUPPORTED,
                PK_FILTER_ENUM_FREE,
                PK_FILTER_ENUM_APPLICATION,
                PK_FILTER_ENUM_DOWNLOADED,
                -1);

    // if we have multiArch support we add the native filter
    if (APT::Configuration::getArchitectures(false).size() > 1) {
        pk_bitfield_add(filters, PK_FILTER_ENUM_ARCH);
    }

    return filters;
}

gchar** pk_backend_get_mime_types(PkBackend *backend)
{
    const gchar *mime_types[] = { "application/vnd.debian.binary-package",
                                  "application/x-deb",
                                  NULL };
    return g_strdupv ((gchar **) mime_types);
}

void pk_backend_start_job(PkBackend *backend, PkBackendJob *job)
{
    /* create private state for this job */
    auto apt = new AptJob(job);
    pk_backend_job_set_user_data(job, apt);
}

void pk_backend_stop_job(PkBackend *backend, PkBackendJob *job)
{
    auto apt = static_cast<AptJob*>(pk_backend_job_get_user_data(job));
    if (apt)
        delete apt;

    /* make debugging easier */
    pk_backend_job_set_user_data (job, NULL);
}

void pk_backend_cancel(PkBackend *backend, PkBackendJob *job)
{
    auto apt = static_cast<AptJob*>(pk_backend_job_get_user_data(job));
    if (apt) {
        /* try to cancel the thread */
        g_debug ("cancelling transaction");
        apt->cancel();
    }
}

static void backend_depends_on_or_requires_thread(PkBackendJob *job, GVariant *params, gpointer user_data)
{
    PkRoleEnum role;
    PkBitfield filters;
    gchar **package_ids;
    gboolean recursive;
    gchar *pi;

    g_variant_get(params, "(t^a&sb)",
                  &filters,
                  &package_ids,
                  &recursive);
    role = pk_backend_job_get_role(job);

    pk_backend_job_set_allow_cancel(job, true);

    auto apt = static_cast<AptJob*>(pk_backend_job_get_user_data(job));
    if (!apt->init()) {
        g_debug("Failed to create apt cache");
        return;
    }

    pk_backend_job_set_status(job, PK_STATUS_ENUM_QUERY);
    PkgList output;
    for (uint i = 0; i < g_strv_length(package_ids); ++i) {
        if (apt->cancelled()) {
            break;
        }
        pi = package_ids[i];
        if (pk_package_id_check(pi) == false) {
            pk_backend_job_error_code(job,
                                      PK_ERROR_ENUM_PACKAGE_ID_INVALID,
                                      "%s",
                                      pi);
            return;
        }

        const PkgInfo &pkInfo = apt->aptCacheFile()->resolvePkgID(pi);
        if (pkInfo.ver.end()) {
            pk_backend_job_error_code(job,
                                      PK_ERROR_ENUM_PACKAGE_NOT_FOUND,
                                      "Couldn't find package %s",
                                      pi);
            return;
        }

        if (role == PK_ROLE_ENUM_DEPENDS_ON) {
            apt->getDepends(output, pkInfo.ver, recursive);
        } else {
            apt->getRequires(output, pkInfo.ver, recursive);
        }
    }

    // It's faster to emit the packages here than in the matching part
    apt->emitPackages(output, filters);
}

void pk_backend_depends_on(PkBackend *backend, PkBackendJob *job, PkBitfield filters,
                           gchar **package_ids, gboolean recursive)
{
    pk_backend_job_thread_create(job, backend_depends_on_or_requires_thread, NULL, NULL);
}

void pk_backend_required_by(PkBackend *backend,
                            PkBackendJob *job,
                            PkBitfield filters,
                            gchar **package_ids,
                            gboolean recursive)
{
    pk_backend_job_thread_create(job, backend_depends_on_or_requires_thread, NULL, NULL);
}

static void backend_get_files_thread(PkBackendJob *job, GVariant *params, gpointer user_data)
{
    gchar **package_ids;
    gchar *pi;

    g_variant_get(params, "(^a&s)",
                  &package_ids);

    auto apt = static_cast<AptJob*>(pk_backend_job_get_user_data(job));
    if (!apt->init()) {
        g_debug("Failed to create apt cache");
        return;
    }

    if (package_ids == NULL) {
        pk_backend_job_error_code(job,
                                  PK_ERROR_ENUM_PACKAGE_ID_INVALID,
                                  "Invalid package id");
        return;
    }

    pk_backend_job_set_status(job, PK_STATUS_ENUM_QUERY);
    for (uint i = 0; i < g_strv_length(package_ids); ++i) {
        pi = package_ids[i];
        if (pk_package_id_check(pi) == false) {
            pk_backend_job_error_code(job,
                                      PK_ERROR_ENUM_PACKAGE_ID_INVALID,
                                      "%s",
                                      pi);
            return;
        }

        const PkgInfo &pkInfo = apt->aptCacheFile()->resolvePkgID(pi);
        if (pkInfo.ver.end()) {
            pk_backend_job_error_code(job,
                                      PK_ERROR_ENUM_PACKAGE_NOT_FOUND,
                                      "Couldn't find package %s",
                                      pi);
            return;
        }

        apt->emitPackageFiles(pi);
    }
}

void pk_backend_get_files(PkBackend *backend, PkBackendJob *job, gchar **package_ids)
{
    pk_backend_job_thread_create(job, backend_get_files_thread, NULL, NULL);
}

static void backend_get_details_thread(PkBackendJob *job, GVariant *params, gpointer user_data)
{
    gchar **package_ids = nullptr;
    gchar **files = nullptr;
    PkRoleEnum role;
    role = pk_backend_job_get_role(job);

    if (role == PK_ROLE_ENUM_GET_DETAILS_LOCAL) {
        g_variant_get(params, "(^a&s)",
                      &files);
    } else {
        g_variant_get(params, "(^a&s)",
                      &package_ids);
    }

    auto apt = static_cast<AptJob*>(pk_backend_job_get_user_data(job));
    if (!apt->init(files)) {
        g_debug ("Failed to create apt cache");
        return;
    }

    pk_backend_job_set_status(job, PK_STATUS_ENUM_QUERY);
    PkgList pkgs;
    if (role == PK_ROLE_ENUM_GET_DETAILS_LOCAL) {
        pkgs = apt->resolveLocalFiles(files);
    } else {
        pkgs = apt->resolvePackageIds(package_ids);
    }

    if (role == PK_ROLE_ENUM_GET_UPDATE_DETAIL) {
        apt->emitUpdateDetails(pkgs);
    } else {
        apt->emitDetails(pkgs);
    }
}

void pk_backend_get_update_detail(PkBackend *backend, PkBackendJob *job, gchar **package_ids)
{
    pk_backend_job_thread_create(job, backend_get_details_thread, NULL, NULL);
}

void pk_backend_get_details(PkBackend *backend, PkBackendJob *job, gchar **package_ids)
{
    pk_backend_job_thread_create(job, backend_get_details_thread, NULL, NULL);
}

void pk_backend_get_details_local(PkBackend *backend, PkBackendJob *job, gchar **files)
{
    pk_backend_job_thread_create(job, backend_get_details_thread, NULL, NULL);
}

static void backend_get_files_local_thread(PkBackendJob *job, GVariant *params, gpointer user_data)
{
    g_autofree gchar **files = nullptr;
    g_variant_get(params, "(^a&s)",
                  &files);
    auto apt = static_cast<AptJob*>(pk_backend_job_get_user_data(job));

    for (guint i = 0; files[i] != nullptr; ++i)
        apt->emitPackageFilesLocal(files[i]);
}

void pk_backend_get_files_local(PkBackend *backend, PkBackendJob *job, gchar **files)
{
    pk_backend_job_thread_create(job, backend_get_files_local_thread, NULL, NULL);
}

static void backend_get_updates_thread(PkBackendJob *job, GVariant *params, gpointer user_data)
{
    PkBitfield filters;
    g_variant_get(params, "(t)", &filters);

    pk_backend_job_set_allow_cancel(job, true);

    auto apt = static_cast<AptJob*>(pk_backend_job_get_user_data(job));
    if (!apt->init()) {
        g_debug("Failed to create APT cache");
        return;
    }

    pk_backend_job_set_status(job, PK_STATUS_ENUM_QUERY);

    PkgList updates;
    PkgList installs;
    PkgList removals;
    PkgList obsoleted;
    PkgList downgrades;
    PkgList blocked;
    updates = apt->getUpdates(blocked, downgrades, installs, removals, obsoleted);

    apt->emitUpdates(updates, filters);
    apt->emitPackages(installs, filters, PK_INFO_ENUM_INSTALL);
    apt->emitPackages(removals, filters, PK_INFO_ENUM_REMOVE);
    apt->emitPackages(obsoleted, filters, PK_INFO_ENUM_OBSOLETE);
    apt->emitPackages(downgrades, filters, PK_INFO_ENUM_DOWNGRADE);
    apt->emitPackages(blocked, filters, PK_INFO_ENUM_BLOCKED);
}

void pk_backend_get_updates(PkBackend *backend, PkBackendJob *job, PkBitfield filters)
{
    pk_backend_job_thread_create(job, backend_get_updates_thread, NULL, NULL);
}

static void backend_what_provides_thread(PkBackendJob *job, GVariant *params, gpointer user_data)
{
    PkBitfield filters;
    gchar **values;
    auto apt = static_cast<AptJob*>(pk_backend_job_get_user_data(job));

    g_variant_get(params, "(t^a&s)",
                  &filters,
                  &values);

    pk_backend_job_set_status(job, PK_STATUS_ENUM_QUERY);

    // We can handle libraries, mimetypes and codecs
    if (!apt->init()) {
        g_debug("Failed to create apt cache");
        g_strfreev(values);
        return;
    }

    pk_backend_job_set_status(job, PK_STATUS_ENUM_QUERY);

    PkgList output;
    apt->providesLibrary(output, values);
    apt->providesCodec(output, values);
    apt->providesMimeType(output, values);

    // It's faster to emit the packages here rather than in the matching part
    apt->emitPackages(output, filters);
}

/**
  * pk_backend_what_provides
  */
void pk_backend_what_provides(PkBackend *backend,
                              PkBackendJob *job,
                              PkBitfield filters,
                              gchar **values)
{
    pk_backend_job_thread_create(job, backend_what_provides_thread, NULL, NULL);
}

/**
 * pk_backend_download_packages_thread:
 */
static void pk_backend_download_packages_thread(PkBackendJob *job, GVariant *params, gpointer user_data)
{
    gchar **package_ids;
    const gchar *tmpDir;
    string directory;

    g_variant_get(params, "(^a&ss)",
                  &package_ids,
                  &tmpDir);
    directory = _config->FindDir("Dir::Cache::archives");
    pk_backend_job_set_allow_cancel(job, true);

    auto apt = static_cast<AptJob*>(pk_backend_job_get_user_data(job));
    if (!apt->init()) {
        g_debug("Failed to create apt cache");
        return;
    }

    PkBackend *backend = PK_BACKEND(pk_backend_job_get_backend(job));
    if (pk_backend_is_online(backend)) {
        pk_backend_job_set_status(job, PK_STATUS_ENUM_QUERY);
        // Create the progress
        AcqPackageKitStatus Stat(apt);

        // get a fetcher
        pkgAcquire fetcher(&Stat);
        gchar *pi;

        // TODO this might be useful when the item is in the cache
        // 	for (pkgAcquire::ItemIterator I = fetcher.ItemsBegin(); I < fetcher.ItemsEnd();)
        // 	{
        // 		if ((*I)->Local == true)
        // 		{
        // 			I++;
        // 			continue;
        // 		}
        //
        // 		// Close the item and check if it was found in cache
        // 		(*I)->Finished();
        // 		if ((*I)->Complete == false) {
        // 			Transient = true;
        // 		}
        //
        // 		// Clear it out of the fetch list
        // 		delete *I;
        // 		I = fetcher.ItemsBegin();
        // 	}

        for (uint i = 0; i < g_strv_length(package_ids); ++i) {
            pi = package_ids[i];
            if (pk_package_id_check(pi) == false) {
                pk_backend_job_error_code(job,
                                          PK_ERROR_ENUM_PACKAGE_ID_INVALID,
                                          "%s",
                                          pi);
                return;
            }

            if (apt->cancelled()) {
                break;
            }

            const PkgInfo &pkInfo = apt->aptCacheFile()->resolvePkgID(pi);
            // Ignore packages that could not be found or that exist only due to dependencies.
            if (pkInfo.ver.end()) {
                _error->Error("Can't find this package id \"%s\".", pi);
                continue;
            } else {
                if(!pkInfo.ver.Downloadable()) {
                    _error->Error("No downloadable files for %s,"
                                  "perhaps it is a local or obsolete" "package?",
                                  pi);
                    continue;
                }

                string storeFileName;
                if (!apt->getArchive(&fetcher,
                                     pkInfo.ver,
                                     directory,
                                     storeFileName)) {
                    return;
                }

                gchar **files = (gchar **) g_malloc(2 * sizeof(gchar *));
                files[0] = g_strdup_printf("%s/%s", directory.c_str(), std::string{flNotDir(storeFileName)}.c_str());
                files[1] = NULL;
                pk_backend_job_files(job, pi, files);
                g_strfreev(files);
            }
        }

        if (fetcher.Run() != pkgAcquire::Continue
                && apt->cancelled() == false) {
            // We failed and we did not cancel
            show_errors(job, PK_ERROR_ENUM_PACKAGE_DOWNLOAD_FAILED);
            return;
        }

    } else {
        pk_backend_job_error_code(job,
                                  PK_ERROR_ENUM_NO_NETWORK,
                                  "Cannot download packages whilst offline");
    }
}

void pk_backend_download_packages(PkBackend *backend,
                                  PkBackendJob *job,
                                  gchar **package_ids,
                                  const gchar *directory)
{
    pk_backend_job_thread_create(job, pk_backend_download_packages_thread, NULL, NULL);
}

static void pk_backend_refresh_cache_thread(PkBackendJob *job, GVariant *params, gpointer user_data)
{
    pk_backend_job_set_allow_cancel(job, true);

    auto apt = static_cast<AptJob*>(pk_backend_job_get_user_data(job));
    if (!apt->init()) {
        g_debug("Failed to create apt cache");
        return;
    }

    PkBackend *backend = PK_BACKEND(pk_backend_job_get_backend(job));
    if (pk_backend_is_online(backend)) {
        apt->refreshCache();

        if (_error->PendingError() == true) {
            show_errors(job, PK_ERROR_ENUM_CANNOT_FETCH_SOURCES, true);
        }
    } else {
        pk_backend_job_error_code(job,
                                  PK_ERROR_ENUM_NO_NETWORK,
                                  "Cannot refresh cache whilst offline");
    }
}

void pk_backend_refresh_cache(PkBackend *backend, PkBackendJob *job, gboolean force)
{
    pk_backend_job_thread_create(job, pk_backend_refresh_cache_thread, NULL, NULL);
}

static void pk_backend_resolve_thread(PkBackendJob *job, GVariant *params, gpointer user_data)
{
    gchar **search;
    PkBitfield filters;

    g_variant_get(params, "(t^a&s)",
                  &filters,
                  &search);
    pk_backend_job_set_allow_cancel(job, true);

    auto apt = static_cast<AptJob*>(pk_backend_job_get_user_data(job));
    if (!apt->init()) {
        g_debug("Failed to initialize APT job");
        return;
    }

    PkgList pkgs = apt->resolvePackageIds(search);

    // It's faster to emit the packages here rather than in the matching part
    apt->emitPackages(pkgs, filters, PK_INFO_ENUM_UNKNOWN, true);
}

void pk_backend_resolve(PkBackend *backend, PkBackendJob *job, PkBitfield filters, gchar **packages)
{
    pk_backend_job_thread_create(job, pk_backend_resolve_thread, NULL, NULL);
}

static void pk_backend_search_files_thread(PkBackendJob *job, GVariant *params, gpointer user_data)
{
    gchar **search;
    PkBitfield filters;
    auto apt = static_cast<AptJob*>(pk_backend_job_get_user_data(job));

    g_variant_get(params, "(t^a&s)",
                  &filters,
                  &search);

    pk_backend_job_set_allow_cancel(job, true);

    // as we can only search for installed files lets avoid the opposite
    if (!pk_bitfield_contain(filters, PK_FILTER_ENUM_NOT_INSTALLED)) {
        if (!apt->init()) {
            g_debug("Failed to create apt cache");
            return;
        }

        pk_backend_job_set_status(job, PK_STATUS_ENUM_QUERY);
        PkgList output;
        output = apt->searchPackageFiles(search);

        // It's faster to emit the packages here rather than in the matching part
        apt->emitPackages(output, filters);
    }
}

void pk_backend_search_files(PkBackend *backend, PkBackendJob *job, PkBitfield filters, gchar **values)
{
    pk_backend_job_thread_create(job, pk_backend_search_files_thread, NULL, NULL);
}

static void backend_search_groups_thread(PkBackendJob *job, GVariant *params, gpointer user_data)
{
    gchar **search;
    PkBitfield filters;

    g_variant_get(params, "(t^a&s)",
                  &filters,
                  &search);

    auto apt = static_cast<AptJob*>(pk_backend_job_get_user_data(job));
    if (!apt->init()) {
        g_debug("Failed to create apt cache");
        return;
    }

    // It's faster to emit the packages here rather than in the matching part
    PkgList output;
    output = apt->getPackagesFromGroup(search);
    apt->emitPackages(output, filters);

    pk_backend_job_set_percentage(job, 100);
}

void pk_backend_search_groups(PkBackend *backend, PkBackendJob *job, PkBitfield filters, gchar **values)
{
    pk_backend_job_thread_create(job, backend_search_groups_thread, NULL, NULL);
}

static void backend_search_package_thread(PkBackendJob *job, GVariant *params, gpointer user_data)
{
    gchar **values;
    PkBitfield filters;
    PkRoleEnum role;
    vector<string> queries;

    g_variant_get(params, "(t^a&s)",
                  &filters,
                  &values);

    if (*values) {
        for (gint i = 0; values[i] != NULL; i++) {
            queries.push_back(values[i]);
        }
    }

    auto apt = static_cast<AptJob*>(pk_backend_job_get_user_data(job));
    if (!apt->init()) {
        g_debug("Failed to create apt cache");
        return;
    }

    if (_error->PendingError() == true) {
        return;
    }

    pk_backend_job_set_status(job, PK_STATUS_ENUM_QUERY);
    pk_backend_job_set_percentage(job, PK_BACKEND_PERCENTAGE_INVALID);
    pk_backend_job_set_allow_cancel(job, true);

    PkgList output;
    role = pk_backend_job_get_role(job);
    if (role == PK_ROLE_ENUM_SEARCH_DETAILS) {
        output = apt->searchPackageDetails(queries);
    } else {
        output = apt->searchPackageName(queries);
    }

    // It's faster to emit the packages here than in the matching part
    apt->emitPackages(output, filters, PK_INFO_ENUM_UNKNOWN, true);

    pk_backend_job_set_percentage(job, 100);
}

void pk_backend_search_names(PkBackend *backend, PkBackendJob *job, PkBitfield filters, gchar **values)
{
    pk_backend_job_thread_create(job, backend_search_package_thread, NULL, NULL);
}

void pk_backend_search_details(PkBackend *backend, PkBackendJob *job, PkBitfield filters, gchar **values)
{
    pk_backend_job_thread_create(job, backend_search_package_thread, NULL, NULL);
}

static void backend_manage_packages_thread(PkBackendJob *job, GVariant *params, gpointer user_data)
{
    // Transaction flags
    PkBitfield transaction_flags = 0;
    gboolean allow_deps = false;
    gboolean autoremove = false;
    gchar **full_paths = NULL;
    gchar **package_ids = NULL;

    // Get the transaction role since this method is called by install/remove/update/repair
    PkRoleEnum role = pk_backend_job_get_role(job);
    if (role == PK_ROLE_ENUM_INSTALL_FILES) {
        g_variant_get(params, "(t^a&s)",
                      &transaction_flags,
                      &full_paths);
    } else if (role == PK_ROLE_ENUM_REMOVE_PACKAGES) {
        g_variant_get(params, "(t^a&sbb)",
                      &transaction_flags,
                      &package_ids,
                      &allow_deps,
                      &autoremove);
    } else if (role == PK_ROLE_ENUM_INSTALL_PACKAGES) {
        g_variant_get(params, "(t^a&s)",
                      &transaction_flags,
                      &package_ids);
    } else if (role == PK_ROLE_ENUM_UPDATE_PACKAGES) {
        g_variant_get(params, "(t^a&s)",
                      &transaction_flags,
                      &package_ids);
    }

    // Check if we should fix broken packages
    bool fixBroken = false;
    if (role == PK_ROLE_ENUM_REPAIR_SYSTEM) {
        // On fix broken mode no package to remove/install is allowed
        fixBroken = true;
    }

    pk_backend_job_set_allow_cancel(job, true);

    auto apt = static_cast<AptJob*>(pk_backend_job_get_user_data(job));
    if (!apt->init(full_paths)) {
        g_debug("Failed to create apt cache");
        return;
    }

    pk_backend_job_set_status(job, PK_STATUS_ENUM_QUERY);
    PkgList installPkgs, removePkgs, updatePkgs;

    if (!fixBroken) {
        // Resolve the given packages
        if (role == PK_ROLE_ENUM_REMOVE_PACKAGES) {
            removePkgs = apt->resolvePackageIds(package_ids);
        } else if (role == PK_ROLE_ENUM_INSTALL_PACKAGES) {
            installPkgs = apt->resolvePackageIds(package_ids);
        } else if (role == PK_ROLE_ENUM_UPDATE_PACKAGES) {
            updatePkgs = apt->resolvePackageIds(package_ids);
        } else if (role == PK_ROLE_ENUM_INSTALL_FILES) {
            installPkgs = apt->resolveLocalFiles(full_paths);
        } else {
            pk_backend_job_error_code(job,
                                      PK_ERROR_ENUM_PACKAGE_NOT_FOUND,
                                      "Could not figure out what to do to apply the change.");
            return;
        }

        if (removePkgs.size() == 0 && installPkgs.size() == 0 && updatePkgs.size() == 0) {
            pk_backend_job_error_code(job,
                                      PK_ERROR_ENUM_PACKAGE_NOT_FOUND,
                                      "Could not find package(s)");
            return;
        }
    }

    // Install/Update/Remove packages, or just simulate
    bool ret = apt->runTransaction(installPkgs,
                                   removePkgs,
                                   updatePkgs,
                                   fixBroken,
                                   transaction_flags,
                                   autoremove);
    if (!ret) {
        // Print transaction errors
        g_debug("AptJob::runTransaction() failed: %i", _error->PendingError());
        return;
    }
}

void pk_backend_install_packages(PkBackend *backend,
                                 PkBackendJob *job,
                                 PkBitfield transaction_flags,
                                 gchar **package_ids)
{
    pk_backend_job_thread_create(job, backend_manage_packages_thread, NULL, NULL);
}

void pk_backend_update_packages(PkBackend *backend,
                                PkBackendJob *job,
                                PkBitfield transaction_flags,
                                gchar **package_ids)
{
    pk_backend_job_thread_create(job, backend_manage_packages_thread, NULL, NULL);
}

void pk_backend_install_files(PkBackend *backend,
                              PkBackendJob *job,
                              PkBitfield transaction_flags,
                              gchar **full_paths)
{
    pk_backend_job_thread_create(job, backend_manage_packages_thread, NULL, NULL);
}

void pk_backend_remove_packages(PkBackend *backend,
                                PkBackendJob *job,
                                PkBitfield transaction_flags,
                                gchar **package_ids,
                                gboolean allow_deps,
                                gboolean autoremove)
{
    pk_backend_job_thread_create(job, backend_manage_packages_thread, NULL, NULL);
}

void pk_backend_repair_system(PkBackend *backend, PkBackendJob *job, PkBitfield transaction_flags)
{
    pk_backend_job_thread_create(job, backend_manage_packages_thread, NULL, NULL);
}

static void backend_repo_manager_thread(PkBackendJob *job, GVariant *params, gpointer user_data)
{
    // list
    PkBitfield filters;
    PkBitfield transaction_flags = 0;
    // enable
    const gchar *repo_id;
    gboolean enabled;
    gboolean autoremove;
    bool found = false;
    // generic
    PkRoleEnum role;

    role = pk_backend_job_get_role(job);
    if (role == PK_ROLE_ENUM_GET_REPO_LIST) {
        pk_backend_job_set_status(job, PK_STATUS_ENUM_QUERY);
        g_variant_get(params, "(t)",
                      &filters);
    } else if (role == PK_ROLE_ENUM_REPO_REMOVE) {
        g_variant_get(params, "(t&sb)",
                      &transaction_flags,
                      &repo_id,
                      &autoremove);
    } else {
        pk_backend_job_set_status(job, PK_STATUS_ENUM_REQUEST);
        g_variant_get (params, "(&sb)",
                       &repo_id,
                       &enabled);
    }

    SourcesList sourcesList;
    if (sourcesList.ReadSources() == false) {
        _error->
                Warning("Ignoring invalid record(s) in sources.list file!");
        //return false;
    }

    if (sourcesList.ReadVendors() == false) {
        _error->Error("Cannot read vendors.list file");
        show_errors(job, PK_ERROR_ENUM_FAILED_CONFIG_PARSING);
        return;
    }

    for (SourcesList::SourceRecord *sourceRecord : sourcesList.SourceRecords) {

        if (sourceRecord->Type & SourcesList::Comment) {
            continue;
        }

        string sections = sourceRecord->joinedSections();

        string repoId = sourceRecord->repoId();

        if (role == PK_ROLE_ENUM_GET_REPO_LIST) {
            if (pk_bitfield_contain(filters, PK_FILTER_ENUM_NOT_DEVELOPMENT) &&
                    (sourceRecord->Type & SourcesList::DebSrc)) {
                continue;
            }

            pk_backend_job_repo_detail(job,
                                       repoId.c_str(),
                                       sourceRecord->niceName().c_str(),
                                       !(sourceRecord->Type & SourcesList::Disabled));
        } else if (repoId.compare(repo_id) == 0) {
            // Found the repo to enable/disable
            found = true;

            if (role == PK_ROLE_ENUM_REPO_ENABLE) {
                if (enabled) {
                    sourceRecord->Type = sourceRecord->Type & ~SourcesList::Disabled;
                } else {
                    sourceRecord->Type |= SourcesList::Disabled;
                }

                // Commit changes
                if (!sourcesList.UpdateSources()) {
                    _error->Error("Could not update sources file");
                    show_errors(job, PK_ERROR_ENUM_CANNOT_WRITE_REPO_CONFIG);
                }
            } else if (role == PK_ROLE_ENUM_REPO_REMOVE) {
                if (autoremove) {
                    auto apt = static_cast<AptJob*>(pk_backend_job_get_user_data(job));
                    if (!apt->init()) {
                        g_debug("Failed to create apt cache");
                        return;
                    }

                    PkgList removePkgs = apt->getPackagesFromRepo(sourceRecord);
                    if (removePkgs.size() > 0) {
                        // Install/Update/Remove packages, or just simulate
                        bool ret;
                        ret = apt->runTransaction(PkgList(),
                                                  removePkgs,
                                                  PkgList(),
                                                  false,
                                                  transaction_flags,
                                                  false);
                        if (!ret) {
                            // Print transaction errors
                            g_debug("AptJob::runTransaction() failed: %i", _error->PendingError());
                            return;
                        }
                    }
                }

                // Now if we are not simulating remove the repository
                if (!pk_bitfield_contain(transaction_flags, PK_TRANSACTION_FLAG_ENUM_SIMULATE)) {
                    sourcesList.RemoveSource(sourceRecord);

                    // Commit changes
                    if (!sourcesList.UpdateSources()) {
                        _error->Error("Could not update sources file");
                        show_errors(job, PK_ERROR_ENUM_CANNOT_WRITE_REPO_CONFIG);
                    }
                }
            }

            // Leave the search loop
            break;
        }
    }

    if ((role == PK_ROLE_ENUM_REPO_ENABLE || role == PK_ROLE_ENUM_REPO_REMOVE) &&
            !found) {
        _error->Error("Could not find the repository");
        show_errors(job, PK_ERROR_ENUM_REPO_NOT_AVAILABLE);
    }
}

void pk_backend_get_repo_list(PkBackend *backend, PkBackendJob *job, PkBitfield filters)
{
    pk_backend_job_thread_create(job, backend_repo_manager_thread, NULL, NULL);
}

void pk_backend_repo_enable(PkBackend *backend, PkBackendJob *job, const gchar *repo_id, gboolean enabled)
{
    pk_backend_job_thread_create(job, backend_repo_manager_thread, NULL, NULL);
}

void
pk_backend_repo_remove (PkBackend *backend,
                        PkBackendJob *job,
                        PkBitfield transaction_flags,
                        const gchar *repo_id,
                        gboolean autoremove)
{
    pk_backend_job_thread_create(job, backend_repo_manager_thread, NULL, NULL);
}

static void backend_get_packages_thread(PkBackendJob *job, GVariant *params, gpointer user_data)
{
    PkBitfield filters;
    g_variant_get(params, "(t)",
                  &filters);
    pk_backend_job_set_allow_cancel(job, true);

    auto apt = static_cast<AptJob*>(pk_backend_job_get_user_data(job));
    if (!apt->init()) {
        g_debug("Failed to create apt cache");
        return;
    }

    PkgList output;
    output = apt->getPackages();

    // It's faster to emit the packages rather here than in the matching part
    apt->emitPackages(output, filters);
}

void pk_backend_get_packages(PkBackend *backend, PkBackendJob *job, PkBitfield filters)
{
    pk_backend_job_thread_create(job, backend_get_packages_thread, NULL, NULL);
}


/* TODO
void
pk_backend_get_categories (PkBackend *backend, PkBackendJob *job)
{
    pk_backend_job_thread_create (job, pk_backend_get_categories_thread, NULL, NULL);
}
*/

PkBitfield pk_backend_get_roles(PkBackend *backend)
{
    PkBitfield roles;
    roles = pk_bitfield_from_enums(
                PK_ROLE_ENUM_CANCEL,
                PK_ROLE_ENUM_DEPENDS_ON,
                PK_ROLE_ENUM_GET_DETAILS,
                PK_ROLE_ENUM_GET_DETAILS_LOCAL,
                PK_ROLE_ENUM_GET_FILES,
                PK_ROLE_ENUM_GET_FILES_LOCAL,
                PK_ROLE_ENUM_REQUIRED_BY,
                PK_ROLE_ENUM_GET_PACKAGES,
                PK_ROLE_ENUM_WHAT_PROVIDES,
                PK_ROLE_ENUM_GET_UPDATES,
                PK_ROLE_ENUM_GET_UPDATE_DETAIL,
                PK_ROLE_ENUM_INSTALL_PACKAGES,
                PK_ROLE_ENUM_INSTALL_SIGNATURE,
                PK_ROLE_ENUM_REFRESH_CACHE,
                PK_ROLE_ENUM_REMOVE_PACKAGES,
                PK_ROLE_ENUM_DOWNLOAD_PACKAGES,
                PK_ROLE_ENUM_RESOLVE,
                PK_ROLE_ENUM_SEARCH_DETAILS,
                PK_ROLE_ENUM_SEARCH_FILE,
                PK_ROLE_ENUM_SEARCH_GROUP,
                PK_ROLE_ENUM_SEARCH_NAME,
                PK_ROLE_ENUM_UPDATE_PACKAGES,
                PK_ROLE_ENUM_GET_REPO_LIST,
                PK_ROLE_ENUM_REPO_ENABLE,
                PK_ROLE_ENUM_REPAIR_SYSTEM,
                PK_ROLE_ENUM_REPO_REMOVE,
                PK_ROLE_ENUM_INSTALL_FILES,
                -1);

    return roles;
}
