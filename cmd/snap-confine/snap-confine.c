/*
 * Copyright (C) 2015-2018 Canonical Ltd
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <glob.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../libsnap-confine-private/apparmor-support.h"
#include "../libsnap-confine-private/cgroup-freezer-support.h"
#include "../libsnap-confine-private/classic.h"
#include "../libsnap-confine-private/cleanup-funcs.h"
#include "../libsnap-confine-private/feature.h"
#include "../libsnap-confine-private/locking.h"
#include "../libsnap-confine-private/secure-getenv.h"
#include "../libsnap-confine-private/snap.h"
#include "../libsnap-confine-private/utils.h"
#include "cookie-support.h"
#include "mount-support.h"
#include "ns-support.h"
#include "snap-confine-args.h"
#include "udev-support.h"
#include "user-support.h"
#ifdef HAVE_SECCOMP
#include "seccomp-support.h"
#endif  // ifdef HAVE_SECCOMP

// sc_maybe_fixup_permissions fixes incorrect permissions
// inside the mount namespace for /var/lib. Before 1ccce4
// this directory was created with permissions 1777.
static void sc_maybe_fixup_permissions(void) {
    struct stat buf;
    if (stat("/var/lib", &buf) != 0) {
        die("cannot stat /var/lib");
    }
    if ((buf.st_mode & 0777) == 0777) {
        if (chmod("/var/lib", 0755) != 0) {
            die("cannot chmod /var/lib");
        }
        if (chown("/var/lib", 0, 0) != 0) {
            die("cannot chown /var/lib");
        }
    }
}

// sc_maybe_fixup_udev will remove incorrectly created udev tags
// that cause libudev on 16.04 to fail with "udev_enumerate_scan failed".
// See also:
// https://forum.snapcraft.io/t/weird-udev-enumerate-error/2360/17
static void sc_maybe_fixup_udev(void) {
    glob_t glob_res SC_CLEANUP(globfree) = {
        .gl_pathv = NULL,
        .gl_pathc = 0,
        .gl_offs = 0,
    };
    const char *glob_pattern = "/run/udev/tags/snap_*/*nvidia*";
    int err = glob(glob_pattern, 0, NULL, &glob_res);
    if (err == GLOB_NOMATCH) {
        return;
    }
    if (err != 0) {
        die("cannot search using glob pattern %s: %d", glob_pattern, err);
    }
    // kill bogus udev tags for nvidia. They confuse udev, this
    // undoes the damage from github.com/snapcore/snapd/pull/3671.
    //
    // The udev tagging of nvidia got reverted in:
    // https://github.com/snapcore/snapd/pull/4022
    // but leftover files need to get removed or apps won't start
    for (size_t i = 0; i < glob_res.gl_pathc; ++i) {
        unlink(glob_res.gl_pathv[i]);
    }
}

typedef struct sc_invocation {
    /* Things declared by the system. */
    const char *base_snap_name;
    const char *security_tag;
    const char *snap_instance;
    /* Things derived at runtime. */
    bool is_normal_mode;
} sc_invocation;

static void enter_classic_execution_environment(void);
static void enter_non_classic_execution_environment(sc_invocation *inv, struct sc_apparmor *aa, uid_t real_uid,
                                                    gid_t real_gid, gid_t saved_gid);

int main(int argc, char **argv) {
    // Use our super-defensive parser to figure out what we've been asked to do.
    struct sc_error *err = NULL;
    struct sc_args *args SC_CLEANUP(sc_cleanup_args) = NULL;
    args = sc_nonfatal_parse_args(&argc, &argv, &err);
    sc_die_on_error(err);

    // We've been asked to print the version string so let's just do that.
    if (sc_args_is_version_query(args)) {
        printf("%s %s\n", PACKAGE, PACKAGE_VERSION);
        return 0;
    }

    const char *snap_instance = getenv("SNAP_INSTANCE_NAME");
    if (snap_instance == NULL) {
        die("SNAP_INSTANCE_NAME is not set");
    }
    sc_instance_name_validate(snap_instance, NULL);

    // Collect and validate the security tag and a few other things passed on
    // command line.
    const char *security_tag = sc_args_security_tag(args);
    if (!verify_security_tag(security_tag, snap_instance)) {
        die("security tag %s not allowed", security_tag);
    }
    const char *executable = sc_args_executable(args);
    const char *base_snap_name = sc_args_base_snap(args) ?: "core";
    bool classic_confinement = sc_args_is_classic_confinement(args);

    sc_snap_name_validate(base_snap_name, NULL);

    debug("security tag: %s", security_tag);
    debug("executable:   %s", executable);
    debug("confinement:  %s", classic_confinement ? "classic" : "non-classic");
    debug("base snap:    %s", base_snap_name);

    // Who are we?
    uid_t real_uid, effective_uid, saved_uid;
    gid_t real_gid, effective_gid, saved_gid;
    getresuid(&real_uid, &effective_uid, &saved_uid);
    getresgid(&real_gid, &effective_gid, &saved_gid);
    debug("ruid: %d, euid: %d, suid: %d", real_uid, effective_uid, saved_uid);
    debug("rgid: %d, egid: %d, sgid: %d", real_gid, effective_gid, saved_gid);

    // snap-confine runs as both setuid root and setgid root.
    // Temporarily drop group privileges here and reraise later
    // as needed.
    if (effective_gid == 0 && real_gid != 0) {
        if (setegid(real_gid) != 0) {
            die("cannot set effective group id to %d", real_gid);
        }
    }
#ifndef CAPS_OVER_SETUID
    // this code always needs to run as root for the cgroup/udev setup,
    // however for the tests we allow it to run as non-root
    if (geteuid() != 0 && secure_getenv("SNAP_CONFINE_NO_ROOT") == NULL) {
        die("need to run as root or suid");
    }
#endif

    char *snap_context SC_CLEANUP(sc_cleanup_string) = NULL;
    // Do no get snap context value if running a hook (we don't want to overwrite hook's SNAP_COOKIE)
    if (!sc_is_hook_security_tag(security_tag)) {
        struct sc_error *err SC_CLEANUP(sc_cleanup_error) = NULL;
        snap_context = sc_cookie_get_from_snapd(snap_instance, &err);
        if (err != NULL) {
            error("%s\n", sc_error_msg(err));
        }
    }

    struct sc_apparmor apparmor;
    sc_init_apparmor_support(&apparmor);
    if (!apparmor.is_confined && apparmor.mode != SC_AA_NOT_APPLICABLE && getuid() != 0 && geteuid() == 0) {
        // Refuse to run when this process is running unconfined on a system
        // that supports AppArmor when the effective uid is root and the real
        // id is non-root.  This protects against, for example, unprivileged
        // users trying to leverage the snap-confine in the core snap to
        // escalate privileges.
        die("snap-confine has elevated permissions and is not confined"
            " but should be. Refusing to continue to avoid"
            " permission escalation attacks");
    }

    /* Invocation helps to pass relevant data to various parts of snap-confine. */
    sc_invocation invocation = {
        .snap_instance = snap_instance, .base_snap_name = base_snap_name, .security_tag = security_tag,
        /* is_normal_mode is not probed yet */
    };
    /* For the ease of introducing inv to the if branch below. */
    sc_invocation *inv = &invocation;
    struct sc_apparmor *aa = &apparmor;

    // TODO: check for similar situation and linux capabilities.
    if (geteuid() == 0) {
        if (classic_confinement) {
            enter_classic_execution_environment();
        } else {
            enter_non_classic_execution_environment(inv, aa, real_uid, real_gid, saved_gid);
        }
        // The rest does not so temporarily drop privs back to calling
        // user (we'll permanently drop after loading seccomp)
        if (setegid(real_gid) != 0) die("setegid failed");
        if (seteuid(real_uid) != 0) die("seteuid failed");

        if (real_gid != 0 && geteuid() == 0) die("dropping privs did not work");
        if (real_uid != 0 && getegid() == 0) die("dropping privs did not work");
    }
    // Ensure that the user data path exists.
    setup_user_data();
#if 0
	setup_user_xdg_runtime_dir();
#endif
    // https://wiki.ubuntu.com/SecurityTeam/Specifications/SnappyConfinement
    sc_maybe_aa_change_onexec(aa, security_tag);
#ifdef HAVE_SECCOMP
    if (sc_apply_seccomp_profile_for_security_tag(security_tag)) {
        /* If the process is not explicitly unconfined then load the global
         * profile as well. */
        sc_apply_global_seccomp_profile();
    }
#endif  // ifdef HAVE_SECCOMP
    if (snap_context != NULL) {
        setenv("SNAP_COOKIE", snap_context, 1);
        // for compatibility, if facing older snapd.
        setenv("SNAP_CONTEXT", snap_context, 1);
    }
    // Permanently drop if not root
    if (geteuid() == 0) {
        // Note that we do not call setgroups() here because its ok
        // that the user keeps the groups he already belongs to
        if (setgid(real_gid) != 0) die("setgid failed");
        if (setuid(real_uid) != 0) die("setuid failed");

        if (real_gid != 0 && (getuid() == 0 || geteuid() == 0)) die("permanently dropping privs did not work");
        if (real_uid != 0 && (getgid() == 0 || getegid() == 0)) die("permanently dropping privs did not work");
    }
    // and exec the new executable
    argv[0] = (char *)executable;
    debug("execv(%s, %s...)", executable, argv[0]);
    for (int i = 1; i < argc; ++i) {
        debug(" argv[%i] = %s", i, argv[i]);
    }
    execv(executable, (char *const *)&argv[0]);
    perror("execv failed");
    return 1;
}

static void enter_classic_execution_environment(void) {
    /* 'classic confinement' is designed to run without the sandbox inside the
     * shared namespace. Specifically:
     * - snap-confine skips using the snap-specific mount namespace
     * - snap-confine skips using device cgroups
     * - snapd sets up a lenient AppArmor profile for snap-confine to use
     * - snapd sets up a lenient seccomp profile for snap-confine to use
     */
    debug("skipping sandbox setup, classic confinement in use");
}

static void enter_non_classic_execution_environment(sc_invocation *inv, struct sc_apparmor *aa, uid_t real_uid,
                                                    gid_t real_gid, gid_t saved_gid) {
    /* snap-confine uses privately-shared /run/snapd/ns to store bind-mounted
     * mount namespaces of each snap. In the case that snap-confine is invoked
     * from the mount namespace it typically constructs, the said directory
     * does not contain mount entries for preserved namespaces as those are
     * only visible in the main, outer namespace.
     *
     * In order to operate in such an environment snap-confine must first
     * re-associate its own process with another namespace in which the
     * /run/snapd/ns directory is visible. The most obvious candidate is pid
     * one, which definitely doesn't run in a snap-specific namespace, has a
     * predictable PID and is long lived.
     */
    sc_reassociate_with_pid1_mount_ns();
    // Do global initialization:
    int global_lock_fd = sc_lock_global();
    // ensure that "/" or "/snap" is mounted with the
    // "shared" option, see LP:#1668659
    debug("ensuring that snap mount directory is shared");
    sc_ensure_shared_snap_mount();
    debug("unsharing snap namespace directory");
    sc_initialize_mount_ns();
    sc_unlock(global_lock_fd);

    // Find and open snap-update-ns and snap-discard-ns from the same
    // path as where we (snap-confine) were called.
    int snap_update_ns_fd SC_CLEANUP(sc_cleanup_close) = -1;
    snap_update_ns_fd = sc_open_snap_update_ns();
    int snap_discard_ns_fd SC_CLEANUP(sc_cleanup_close) = -1;
    snap_discard_ns_fd = sc_open_snap_discard_ns();

    // Do per-snap initialization.
    int snap_lock_fd = sc_lock_snap(inv->snap_instance);
    debug("initializing mount namespace: %s", inv->snap_instance);
    struct sc_mount_ns *group = NULL;
    group = sc_open_mount_ns(inv->snap_instance);

    // Check if we are running in normal mode with pivot root. Do this here
    // because once on the inside of the transformed mount namespace we can no
    // longer tell.
    inv->is_normal_mode = sc_should_use_normal_mode(sc_classify_distro(), inv->base_snap_name);

    /* Stale mount namespace discarded or no mount namespace to
       join. We need to construct a new mount namespace ourselves.
       To capture it we will need a helper process so make one. */
    sc_fork_helper(group, aa);
    int retval = sc_join_preserved_ns(group, aa, inv->base_snap_name, inv->snap_instance, snap_discard_ns_fd,
                                      inv->is_normal_mode);
    if (retval == ESRCH) {
        /* Create and populate the mount namespace. This performs all
           of the bootstrapping mounts, pivots into the new root filesystem and
           applies the per-snap mount profile using snap-update-ns. */
        debug("unsharing the mount namespace (per-snap)");
        if (unshare(CLONE_NEWNS) < 0) {
            die("cannot unshare the mount namespace");
        }
        sc_populate_mount_ns(aa, snap_update_ns_fd, inv->base_snap_name, inv->snap_instance, inv->is_normal_mode);

        /* Preserve the mount namespace. */
        sc_preserve_populated_mount_ns(group);
    }

    /* Older versions of snap-confine created incorrect 777 permissions
       for /var/lib and we need to fixup for systems that had their NS created
       with an old version. */
    sc_maybe_fixup_permissions();
    sc_maybe_fixup_udev();

    /* User mount profiles do not apply to non-root users. */
    if (real_uid != 0) {
        debug("joining preserved per-user mount namespace");
        retval = sc_join_preserved_per_user_ns(group, inv->snap_instance);
        if (retval == ESRCH) {
            debug("unsharing the mount namespace (per-user)");
            if (unshare(CLONE_NEWNS) < 0) {
                die("cannot unshare the mount namespace");
            }
            sc_setup_user_mounts(aa, snap_update_ns_fd, inv->snap_instance);
            /* Preserve the mount per-user namespace. But only if the
             * experimental feature is enabled. This way if the feature is
             * disabled user mount namespaces will still exist but will be
             * entirely ephemeral. In addition the call
             * sc_join_preserved_user_ns() will never find a preserved mount
             * namespace and will always enter this code branch. */
            if (sc_feature_enabled(SC_PER_USER_MOUNT_NAMESPACE)) {
                sc_preserve_populated_per_user_mount_ns(group);
            } else {
                debug("NOT preserving per-user mount namespace");
            }
        }
    }
    // Associate each snap process with a dedicated snap freezer control group.
    // This simplifies testing if any processes belonging to a given snap are
    // still alive.  See the documentation of the function for details.
    if (getegid() != 0 && saved_gid == 0) {
        // Temporarily raise egid so we can chown the freezer cgroup under LXD.
        if (setegid(0) != 0) {
            die("cannot set effective group id to root");
        }
    }
    sc_cgroup_freezer_join(inv->snap_instance, getpid());
    if (geteuid() == 0 && real_gid != 0) {
        if (setegid(real_gid) != 0) {
            die("cannot set effective group id to %d", real_gid);
        }
    }

    sc_unlock(snap_lock_fd);

    sc_close_mount_ns(group);

    // Reset path as we cannot rely on the path from the host OS to make sense.
    // The classic distribution may use any PATH that makes sense but we cannot
    // assume it makes sense for the core snap layout. Note that the /usr/local
    // directories are explicitly left out as they are not part of the core
    // snap.
    debug("resetting PATH to values in sync with core snap");
    setenv("PATH",
           "/usr/local/sbin:"
           "/usr/local/bin:"
           "/usr/sbin:"
           "/usr/bin:"
           "/sbin:"
           "/bin:"
           "/usr/games:"
           "/usr/local/games",
           1);
    // Ensure we set the various TMPDIRs to /tmp. One of the parts of setting
    // up the mount namespace is to create a private /tmp directory (this is
    // done in sc_populate_mount_ns() above). The host environment may point to
    // a directory not accessible by snaps so we need to reset it here.
    const char *tmpd[] = {"TMPDIR", "TEMPDIR", NULL};
    int i;
    for (i = 0; tmpd[i] != NULL; i++) {
        if (setenv(tmpd[i], "/tmp", 1) != 0) {
            die("cannot set environment variable '%s'", tmpd[i]);
        }
    }
    struct snappy_udev udev_s;
    if (snappy_udev_init(inv->security_tag, &udev_s) == 0) setup_devices_cgroup(inv->security_tag, &udev_s);
    snappy_udev_cleanup(&udev_s);
}
