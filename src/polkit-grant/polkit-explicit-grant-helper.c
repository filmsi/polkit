/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/***************************************************************************
 *
 * polkit-explicit-grant-helper.c : setgid polkituser explicit grant
 * helper for PolicyKit
 *
 * Copyright (C) 2007 David Zeuthen, <david@fubar.dk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307	 USA
 *
 **************************************************************************/

#define _GNU_SOURCE

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <security/pam_appl.h>
#include <grp.h>
#include <pwd.h>
#include <syslog.h>
#include <errno.h>
#include <string.h>
#include <utime.h>
#include <fcntl.h>

#include <polkit-dbus/polkit-dbus.h>
#include <polkit/polkit-private.h>

static polkit_bool_t
check_pid_for_authorization (pid_t caller_pid, const char *action_id)
{
        polkit_bool_t ret;
        DBusError error;
        DBusConnection *bus;
        PolKitCaller *caller;
        PolKitAction *action;
        PolKitContext *context;
        PolKitError *pk_error;
        PolKitResult pk_result;

        ret = FALSE;

        dbus_error_init (&error);
        bus = dbus_bus_get (DBUS_BUS_SYSTEM, &error);
        if (bus == NULL) {
                fprintf (stderr, "polkit-explicit-grant-helper: cannot connect to system bus: %s: %s\n", 
                         error.name, error.message);
                dbus_error_free (&error);
                goto out;
        }

        caller = polkit_caller_new_from_pid (bus, caller_pid, &error);
        if (caller == NULL) {
                fprintf (stderr, "polkit-explicit-grant-helper: cannot get caller from pid: %s: %s\n",
                         error.name, error.message);
                goto out;
        }

        action = polkit_action_new ();
        if (action == NULL) {
                fprintf (stderr, "polkit-explicit-grant-helper: cannot allocate PolKitAction\n");
                goto out;
        }
        if (!polkit_action_set_action_id (action, action_id)) {
                fprintf (stderr, "polkit-explicit-grant-helper: cannot set action_id\n");
                goto out;
        }

        context = polkit_context_new ();
        if (context == NULL) {
                fprintf (stderr, "polkit-explicit-grant-helper: cannot allocate PolKitContext\n");
                goto out;
        }

        pk_error = NULL;
        if (!polkit_context_init (context, &pk_error)) {
                fprintf (stderr, "polkit-explicit-grant-helper: cannot initialize polkit context: %s: %s\n",
                         polkit_error_get_error_name (pk_error),
                         polkit_error_get_error_message (pk_error));
                polkit_error_free (pk_error);
                goto out;
        }

        pk_result = polkit_context_is_caller_authorized (context, action, caller, FALSE, &pk_error);
        if (polkit_error_is_set (pk_error)) {
                fprintf (stderr, "polkit-explicit-grant-helper: cannot determine if caller is authorized: %s: %s\n",
                         polkit_error_get_error_name (pk_error),
                         polkit_error_get_error_message (pk_error));
                polkit_error_free (pk_error);
                goto out;
        }

        if (pk_result != POLKIT_RESULT_YES) {
                //fprintf (stderr, 
                //         "polkit-explicit-grant-helper: uid %d (pid %d) does not have the "
                //         "org.freedesktop.policykit.read-other-authorizations authorization\n", 
                //         caller_uid, caller_pid);
                goto out;
        }

        ret = TRUE;
out:

        return ret;
}

int
main (int argc, char *argv[])
{
        int ret;
        gid_t egid;
        struct group *group;
        uid_t invoking_uid;
        char *action_id;
        char *endp;
        char grant_line[512];
        struct timeval now;

        ret = 1;

        /* clear the entire environment to avoid attacks using with libraries honoring environment variables */
        if (clearenv () != 0)
                goto out;
        /* set a minimal environment */
        setenv ("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);

        openlog ("polkit-explicit-grant-helper", LOG_CONS | LOG_PID, LOG_AUTHPRIV);

        /* check for correct invocation */
        if (argc != 5) {
                syslog (LOG_NOTICE, "inappropriate use of helper, wrong number of arguments [uid=%d]", getuid ());
                fprintf (stderr, "polkit-explicit-grant-helper: wrong number of arguments. This incident has been logged.\n");
                goto out;
        }

        /* check we're running with a non-tty stdin */
        if (isatty (STDIN_FILENO) != 0) {
                syslog (LOG_NOTICE, "inappropriate use of helper, stdin is a tty [uid=%d]", getuid ());
                fprintf (stderr, "polkit-explicit-grant-helper: inappropriate use of helper, stdin is a tty. This incident has been logged.\n");
                goto out;
        }

        invoking_uid = getuid ();

        /* check that we are setgid polkituser */
        egid = getegid ();
        group = getgrgid (egid);
        if (group == NULL) {
                fprintf (stderr, "polkit-explicit-grant-helper: cannot lookup group info for gid %d\n", egid);
                goto out;
        }
        if (strcmp (group->gr_name, POLKIT_GROUP) != 0) {
                fprintf (stderr, "polkit-explicit-grant-helper: needs to be setgid " POLKIT_GROUP "\n");
                goto out;
        }

        /*----------------------------------------------------------------------------------------------------*/

        /* check and validate incoming parameters */

        /* first one is action_id */
        action_id = argv[1];
        if (!polkit_action_validate_id (action_id)) {
                syslog (LOG_NOTICE, "action_id is malformed [uid=%d]", getuid ());
                fprintf (stderr, "polkit-explicit-grant-helper: action_id is malformed. This incident has been logged.\n");
                goto out;
        }

        char *authc_str;
        PolKitAuthorizationConstraint *authc;

        /* second is the auth constraint */
        authc_str = argv[2];
        authc = polkit_authorization_constraint_from_string (authc_str);
        if (authc == NULL) {
                syslog (LOG_NOTICE, "auth constraint is malformed [uid=%d]", getuid ());
                fprintf (stderr, "polkit-explicit-grant-helper: auth constraint is malformed. This incident has been logged.\n");
                goto out;
        }

#define TARGET_UID 0
        int target;
        uid_t target_uid = -1;

        /* (third, fourth) is one of: ("uid", uid) */
        if (strcmp (argv[3], "uid") == 0) {

                target = TARGET_UID;
                target_uid = strtol (argv[4], &endp, 10);
                if  (*endp != '\0') {
                        syslog (LOG_NOTICE, "target uid is malformed [uid=%d]", getuid ());
                        fprintf (stderr, "polkit-explicit-grant-helper: target uid is malformed. This incident has been logged.\n");
                        goto out;
                }
        } else {
                syslog (LOG_NOTICE, "target type is malformed [uid=%d]", getuid ());
                fprintf (stderr, "polkit-explicit-grant-helper: target type is malformed. This incident has been logged.\n");
                goto out;
        }


        //fprintf (stderr, "action_id=%s constraint=%s uid=%d\n", action_id, authc_str, target_uid);

        /* OK, we're done parsing ... check if the user is authorized */

        if (invoking_uid != 0) {
                /* see if calling user is authorized for
                 *
                 *  org.freedesktop.policykit.grant
                 */
                if (!check_pid_for_authorization (getppid (), "org.freedesktop.policykit.grant")) {
                        goto out;
                }
        }

        /* he is.. proceed to add the grant */

        umask (002);

        if (gettimeofday (&now, NULL) != 0) {
                fprintf (stderr, "polkit-explicit-grant-helper: error calling gettimeofday: %m");
                return FALSE;
        }

        if (snprintf (grant_line, 
                      sizeof (grant_line), 
                      "grant:%s:%Lu:%d:%s\n",
                      action_id,
                      (polkit_uint64_t) now.tv_sec,
                      invoking_uid,
                      authc_str) >= (int) sizeof (grant_line)) {
                fprintf (stderr, "polkit-explicit-grant-helper: str to add is too long!\n");
                goto out;
        }

        if (_polkit_authorization_db_auth_file_add (PACKAGE_LOCALSTATE_DIR "/lib/PolicyKit", 
                                                    FALSE, 
                                                    target_uid, 
                                                    grant_line)) {
                ret = 0;
        }

out:

        return ret;
}
