/**
 * kalu - Copyright (C) 2012-2013 Olivier Brunel
 *
 * kalu-alpm.c
 * Copyright (C) 2012-2013 Olivier Brunel <i.am.jack.mail@gmail.com>
 *
 * This file is part of kalu.
 *
 * kalu is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * kalu is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * kalu. If not, see http://www.gnu.org/licenses/
 */

#include <config.h>

/* C */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <utime.h>

/* alpm */
#include <alpm.h>
#include <alpm_list.h>

/* glib */
#include <glib-2.0/glib.h>

/* kalu */
#include "kalu.h"
#include "kalu-alpm.h"
#include "util.h"
#include "conf.h"

/* global variable */
unsigned short alpm_verbose;


static kalu_alpm_t *alpm;

static gboolean copy_file (const gchar *from, const gchar *to);
static gboolean create_local_db (const gchar *dbpath, gchar **newpath,
        GError **error);



static gboolean
copy_file (const gchar *from, const gchar *to)
{
    gchar *contents;
    gsize  length;

    debug ("copying %s to %s", from, to);

    if (!g_file_get_contents (from, &contents, &length, NULL))
    {
        debug ("cannot read %s", from);
        return FALSE;
    }

    if (!g_file_set_contents (to, contents, (gssize) length, NULL))
    {
        debug ("cannot write %s", to);
        g_free (contents);
        return FALSE;
    }

    debug ("..done");
    g_free (contents);
    return TRUE;
}

static gboolean
create_local_db (const gchar *_dbpath, gchar **newpath, GError **error)
{
    gchar    buf[MAX_PATH];
    gchar    buf2[MAX_PATH];
    gchar   *dbpath;
    size_t   l;
    gchar   *folder;
    GDir    *dir;

    debug ("creating local db");

    /* create folder in tmp dir */
    if (NULL == (folder = g_dir_make_tmp ("kalu-XXXXXX", NULL)))
    {
        g_set_error (error, KALU_ERROR, 1, _("Unable to create temp folder"));
        return FALSE;
    }
    debug ("created tmp folder %s", folder);

    /* dbpath will not be slash-terminated */
    dbpath = strdup (_dbpath);
    l = strlen (dbpath) - 1;
    if (dbpath[l] == '/')
    {
        dbpath[l] = '\0';
    }

    /* symlink local */
    snprintf (buf, MAX_PATH - 1, "%s/local", dbpath);
    snprintf (buf2, MAX_PATH - 1, "%s/local", folder);
    if (0 != symlink (buf, buf2))
    {
        g_set_error (error, KALU_ERROR, 1,
                _("Unable to create symlink %s"),
                buf2);
        goto error;
    }
    debug ("created symlink %s", buf2);

    /* copy databases in sync */
    snprintf (buf, MAX_PATH - 1, "%s/sync", folder);
    if (0 != mkdir (buf, 0700))
    {
        g_set_error (error, KALU_ERROR, 1,
                _("Unable to create folder %s"),
                buf);
        goto error;
    }
    debug ("created folder %s", buf);

    snprintf (buf, MAX_PATH - 1, "%s/sync", dbpath);
    if (NULL == (dir = g_dir_open (buf, 0, NULL)))
    {
        g_set_error (error, KALU_ERROR, 1,
                _("Unable to open folder %s"),
                buf);
        goto error;
    }

    const gchar    *file;
    struct stat     filestat;
    struct utimbuf  times;

    while ((file = g_dir_read_name (dir)))
    {
        snprintf (buf, MAX_PATH - 1, "%s/sync/%s", dbpath, file);
        /* stat so we copy files only. also, we need to preserve modified date,
         * used to determine if DBs are up to date or not by libalpm */
        if (0 == stat (buf, &filestat))
        {
            if (S_ISREG (filestat.st_mode))
            {
                snprintf (buf2, MAX_PATH - 1, "%s/sync/%s", folder, file);
                if (!copy_file (buf, buf2))
                {
                    g_set_error (error, KALU_ERROR, 1,
                            _("Copy failed for %s"),
                            buf);
                    g_dir_close (dir);
                    goto error;
                }
                /* preserve time */
                times.actime = filestat.st_atime;
                times.modtime = filestat.st_mtime;
                if (0 != utime (buf2, &times))
                {
                    /* sucks, but no fail, we'll just have to download this db */
                    debug ("Unable to change time of %s", buf2);
                }
                else
                {
                    debug ("updated time for %s", buf2);
                }
            }
            else
            {
                debug ("ignoring non-regular file: %s", buf);
            }
        }
        else
        {
            g_set_error (error, KALU_ERROR, 1, _("Unable to stat %s\n"), buf);
            g_dir_close (dir);
            goto error;
        }
    }
    g_dir_close (dir);
    free (dbpath);

    *newpath = folder;
    return TRUE;

error:
    free (dbpath);
    g_free (folder);
    return FALSE;
}

static void
log_cb (alpm_loglevel_t level, const char *fmt, va_list args)
{
    gchar *s;
    gsize l;

    if (!fmt || *fmt == '\0')
    {
        return;
    }

    if (config->is_debug == 2 && (level & (ALPM_LOG_DEBUG | ALPM_LOG_FUNCTION)))
    {
        return;
    }

    s = g_strdup_vprintf (fmt, args);
    l = strlen (s);
    if (s[--l] == '\n')
        s[l] = '\0';

    debug ("ALPM: %s", s);
    g_free (s);
}

gboolean
kalu_alpm_load (const gchar *conffile, GError **error)
{
    GError             *local_err = NULL;
    gchar              *newpath;
    enum _alpm_errno_t  err;
    pacman_config_t    *pac_conf = NULL;

    /* parse pacman.conf */
    debug ("parsing pacman.conf (%s) for options", conffile);
    if (!parse_pacman_conf (conffile, NULL, 0, 0, &pac_conf, &local_err))
    {
        g_propagate_error (error, local_err);
        free_pacman_config (pac_conf);
        return FALSE;
    }

    debug ("setting up libalpm");
    alpm = new0 (kalu_alpm_t, 1);

    /* create tmp copy of db (so we can sync w/out being root) */
    if (!create_local_db (pac_conf->dbpath, &newpath, &local_err))
    {
        g_set_error (error, KALU_ERROR, 1,
                _("Unable to create local copy of database: %s"),
                local_err->message);
        g_clear_error (&local_err);
        free_pacman_config (pac_conf);
        kalu_alpm_free ();
        return FALSE;
    }
    alpm->dbpath = newpath;

    /* init libalpm */
    alpm->handle = alpm_initialize (pac_conf->rootdir, alpm->dbpath, &err);
    if (alpm->handle == NULL)
    {
        g_set_error (error, KALU_ERROR, 1,
                _("Failed to initialize alpm library: %s"),
                alpm_strerror (err));
        free_pacman_config (pac_conf);
        kalu_alpm_free ();
        return FALSE;
    }

    /* set arch & some options (what to ignore during update) */
    alpm_option_set_arch (alpm->handle, pac_conf->arch);
    alpm_option_set_ignorepkgs (alpm->handle, pac_conf->ignorepkgs);
    alpm_option_set_ignoregroups (alpm->handle, pac_conf->ignoregroups);
    /* cachedirs are used when determining download size */
    alpm_option_set_cachedirs (alpm->handle, pac_conf->cachedirs);

    if (config->is_debug > 1)
        alpm_option_set_logcb (alpm->handle, log_cb);

    /* now we need to add dbs */
    alpm_list_t *i;
    FOR_LIST (i, pac_conf->databases)
    {
        database_t  *db_conf = i->data;
        alpm_db_t   *db;

        /* register db */
        debug ("register %s", db_conf->name);
        db = alpm_register_syncdb (alpm->handle, db_conf->name,
                db_conf->siglevel);
        if (db == NULL)
        {
            g_set_error (error, KALU_ERROR, 1,
                    _("Could not register database %s: %s"),
                    db_conf->name, alpm_strerror (alpm_errno (alpm->handle)));
            free_pacman_config (pac_conf);
            kalu_alpm_free ();
            return FALSE;
        }

        /* add servers */
        alpm_list_t *j;
        FOR_LIST (j, db_conf->servers)
        {
            char        *value  = j->data;
            const char  *dbname = alpm_db_get_name (db);
            /* let's attempt a replacement for the current repo */
            char        *temp   = strreplace (value, "$repo", dbname);
            /* let's attempt a replacement for the arch */
            const char  *arch   = pac_conf->arch;
            char        *server;

            if (arch)
            {
                server = strreplace (temp, "$arch", arch);
                free (temp);
            }
            else
            {
                if (strstr (temp, "$arch"))
                {
                    g_set_error (error, KALU_ERROR, 1,
                            _("Server %s contains the $arch variable, "
                                "but no Architecture was defined"),
                            value);
                    free (temp);
                    free (value);
                    free_pacman_config (pac_conf);
                    kalu_alpm_free ();
                    return FALSE;
                }
                server = temp;
            }

            debug ("add server %s into %s", server, dbname);
            if (alpm_db_add_server (db, server) != 0)
            {
                /* pm_errno is set by alpm_db_setserver */
                g_set_error (error, KALU_ERROR, 1,
                        _("Could not add server %s to database %s: %s"),
                        server,
                        dbname,
                        alpm_strerror (alpm_errno (alpm->handle)));
                free (server);
                free (value);
                free_pacman_config (pac_conf);
                kalu_alpm_free ();
                return FALSE;
            }

            free (server);
        }
    }

    /* set global var */
    alpm_verbose = pac_conf->verbosepkglists;

    free_pacman_config (pac_conf);
    return TRUE;
}

gboolean
kalu_alpm_syncdbs (gint *nb_dbs_synced, GError **error)
{
    alpm_list_t     *sync_dbs   = NULL;
    alpm_list_t     *i;
    GError          *local_err  = NULL;
    int              ret;

    if (!check_syncdbs (alpm, 1, 0, &local_err))
    {
        g_propagate_error (error, local_err);
        return FALSE;
    }

    sync_dbs = alpm_get_syncdbs (alpm->handle);
    *nb_dbs_synced = 0;
    FOR_LIST (i, sync_dbs)
    {
        alpm_db_t *db = i->data;

        ret = alpm_db_update (0, db);
        if (ret < 0)
        {
            g_set_error (error, KALU_ERROR, 1,
                    _("Failed to update %s: %s"),
                    alpm_db_get_name (db),
                    alpm_strerror (alpm_errno (alpm->handle)));
            return FALSE;
        }
        else if (ret == 1)
        {
            debug ("%s is up to date", alpm_db_get_name (db));
        }
        else
        {
            ++*nb_dbs_synced;
            debug ("%s was updated", alpm_db_get_name (db));
        }
    }

    return TRUE;
}

gboolean
kalu_alpm_has_updates (alpm_list_t **packages, GError **error)
{
    alpm_list_t *i;
    alpm_list_t *data       = NULL;
    GError      *local_err  = NULL;

    if (!check_syncdbs (alpm, 1, 1, &local_err))
    {
        g_propagate_error (error, local_err);
        return FALSE;
    }

    if (!trans_init (alpm, alpm->flags, 1, &local_err) == -1)
    {
        g_propagate_error (error, local_err);
        return FALSE;
    }

    if (alpm_sync_sysupgrade (alpm->handle, 0) == -1)
    {
        g_set_error (error, KALU_ERROR, 1, "%s",
                alpm_strerror (alpm_errno (alpm->handle)));
        goto cleanup;
    }

    if (alpm_trans_prepare (alpm->handle, &data) == -1)
    {
        int len = 1024;
        gchar buf[255], err[len--];
        err[0] = '\0';
        switch (alpm_errno (alpm->handle))
        {
            case ALPM_ERR_PKG_INVALID_ARCH:
                FOR_LIST (i, data)
                {
                    const char *pkg = i->data;
                    len -= snprintf (buf, 255,
                            _("- Package %s does not have a valid architecture\n"),
                            pkg);
                    if (len >= 0)
                    {
                        strncat (err, buf, (size_t) len);
                    }
                }
                break;
            case ALPM_ERR_UNSATISFIED_DEPS:
                FOR_LIST (i, data)
                {
                    alpm_depmissing_t *miss = i->data;
                    char *depstring = alpm_dep_compute_string (miss->depend);
                    len -= snprintf (buf, 255,
                            _("- %s requires %s\n"),
                            miss->target,
                            depstring);
                    if (len >= 0)
                    {
                        strncat (err, buf, (size_t) len);
                    }
                    free (depstring);
                }
                break;
            case ALPM_ERR_CONFLICTING_DEPS:
                FOR_LIST (i, data)
                {
                    alpm_conflict_t *conflict = i->data;
                    /* only print reason if it contains new information */
                    if (conflict->reason->mod == ALPM_DEP_MOD_ANY)
                    {
                        len -= snprintf (buf, 255,
                                _("- %s and %s are in conflict\n"),
                                conflict->package1,
                                conflict->package2);
                        if (len >= 0)
                        {
                            strncat (err, buf, (size_t) len);
                        }
                    }
                    else
                    {
                        char *reason;
                        reason = alpm_dep_compute_string (conflict->reason);
                        len -= snprintf (buf, 255,
                                _("- %s and %s are in conflict (%s)\n"),
                                conflict->package1,
                                conflict->package2,
                                reason);
                        if (len >= 0)
                        {
                            strncat (err, buf, (size_t) len);
                        }
                        free (reason);
                    }
                }
                break;
            default:
                break;
        }
        g_set_error (error, KALU_ERROR, 2,
                _("Failed to prepare transaction: %s\n%s"),
                alpm_strerror (alpm_errno (alpm->handle)),
                err);
        goto cleanup;
    }

    alpm_db_t  *db_local = alpm_get_localdb (alpm->handle);
    FOR_LIST (i, alpm_trans_get_add (alpm->handle))
    {
        alpm_pkg_t *pkg = i->data;
        alpm_pkg_t *old = alpm_db_get_pkg (db_local, alpm_pkg_get_name (pkg));
        kalu_package_t *package;

        package = new0 (kalu_package_t, 1);
        package->name = strdup (alpm_pkg_get_name (pkg));
        package->desc = strdup (alpm_pkg_get_desc (pkg));
        package->new_version = strdup (alpm_pkg_get_version (pkg));
        package->dl_size = (guint) alpm_pkg_download_size (pkg);
        package->new_size = (guint) alpm_pkg_get_isize (pkg);
        /* we might not have an old package, when an update requires to
         * install a new package (e.g. after a split) */
        if (old)
        {
            package->old_version = strdup (alpm_pkg_get_version (old));
            package->old_size = (guint) alpm_pkg_get_isize (old);
        }
        else
        {
            /* TRANSLATORS: no previous version */
            package->old_version = strdup (_("none"));
            package->old_size = 0;
        }

        *packages = alpm_list_add (*packages, package);
    }

cleanup:
    if (data)
    {
        FREELIST (data);
    }
    trans_release (alpm, NULL);

    return (*packages != NULL);
}

gboolean
kalu_alpm_has_updates_watched (alpm_list_t **packages, alpm_list_t *watched,
        GError **error)
{
    alpm_list_t *sync_dbs = alpm_get_syncdbs (alpm->handle);
    alpm_list_t *i, *j;
    GError *local_err = NULL;

    if (!check_syncdbs (alpm, 1, 1, &local_err))
    {
        g_propagate_error (error, local_err);
        return FALSE;
    }

    FOR_LIST (i, watched)
    {
        watched_package_t *w_pkg = i->data;
        FOR_LIST (j, sync_dbs)
        {
            alpm_pkg_t *pkg;
            pkg = alpm_db_get_pkg ((alpm_db_t *) j->data, w_pkg->name);
            if (pkg)
            {
                if (alpm_pkg_vercmp (alpm_pkg_get_version (pkg),
                            w_pkg->version) > 0)
                {
                    kalu_package_t *package;
                    package = new0 (kalu_package_t, 1);

                    package->name = strdup (alpm_pkg_get_name (pkg));
                    package->desc = strdup (alpm_pkg_get_desc (pkg));
                    package->old_version = strdup (w_pkg->version);
                    package->new_version = strdup (alpm_pkg_get_version (pkg));
                    package->dl_size = (guint) alpm_pkg_download_size (pkg);
                    package->new_size = (guint) alpm_pkg_get_isize (pkg);

                    *packages = alpm_list_add (*packages, package);
                    debug ("found watched update %s: %s -> %s", package->name,
                            package->old_version, package->new_version);
                }
                break;
            }
        }
    }

    return (*packages != NULL);
}

gboolean
kalu_alpm_has_foreign (alpm_list_t **packages, alpm_list_t *ignore,
        GError **error)
{
    alpm_db_t *dblocal;
    alpm_list_t *sync_dbs, *i, *j;
    gboolean found;
    GError *local_err = NULL;

    if (!check_syncdbs (alpm, 1, 1, &local_err))
    {
        g_propagate_error (error, local_err);
        return FALSE;
    }

    dblocal  = alpm_get_localdb (alpm->handle);
    sync_dbs = alpm_get_syncdbs (alpm->handle);

    FOR_LIST (i, alpm_db_get_pkgcache (dblocal))
    {
        alpm_pkg_t *pkg = i->data;
        const char *pkgname = alpm_pkg_get_name (pkg);
        found = FALSE;

        if (NULL != alpm_list_find_str (ignore, pkgname))
        {
            continue;
        }

        FOR_LIST (j, sync_dbs)
        {
            if (alpm_db_get_pkg ((alpm_db_t *) j->data, pkgname))
            {
                found = TRUE;
                break;
            }
        }
        if (!found)
        {
            *packages = alpm_list_add (*packages, pkg);
        }
    }

    return (*packages != NULL);
}

void
kalu_alpm_free (void)
{
    if (alpm == NULL)
    {
        return;
    }

    if (alpm->handle != NULL)
    {
        alpm_release (alpm->handle);
    }

    /* yes, we remove the dbpath. because we made a tmp copy of it */
    if (alpm->dbpath)
    {
        rmrf (alpm->dbpath);
    }
    free (alpm->dbpath);

    g_free (alpm);
    alpm = NULL;
}
