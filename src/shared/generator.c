/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <unistd.h>

#include "alloc-util.h"
#include "cgroup-util.h"
#include "dropin.h"
#include "escape.h"
#include "fd-util.h"
#include "fileio.h"
#include "fstab-util.h"
#include "generator.h"
#include "log.h"
#include "macro.h"
#include "mkdir-label.h"
#include "path-util.h"
#include "process-util.h"
#include "special.h"
#include "specifier.h"
#include "string-util.h"
#include "time-util.h"
#include "unit-name.h"
#include "util.h"

int generator_open_unit_file(
                const char *dir,
                const char *source,
                const char *fn,
                FILE **ret) {

        _cleanup_free_ char *p = NULL;
        FILE *f;
        int r;

        assert(dir);
        assert(fn);
        assert(ret);

        p = path_join(dir, fn);
        if (!p)
                return log_oom();

        r = fopen_unlocked(p, "wxe", &f);
        if (r < 0) {
                if (source && r == -EEXIST)
                        return log_error_errno(r,
                                               "Failed to create unit file '%s', as it already exists. Duplicate entry in '%s'?",
                                               p, source);

                return log_error_errno(r, "Failed to create unit file '%s': %m", p);
        }

        fprintf(f,
                "# Automatically generated by %s\n\n",
                program_invocation_short_name);

        *ret = f;
        return 0;
}

int generator_add_symlink(const char *dir, const char *dst, const char *dep_type, const char *src) {
        /* Adds a symlink from <dst>.<dep_type>/ to <src> (if src is absolute)
         * or ../<src> (otherwise). */

        const char *from, *to;

        from = path_is_absolute(src) ? src : strjoina("../", src);
        to = strjoina(dir, "/", dst, ".", dep_type, "/", basename(src));

        (void) mkdir_parents_label(to, 0755);
        if (symlink(from, to) < 0)
                if (errno != EEXIST)
                        return log_error_errno(errno, "Failed to create symlink \"%s\": %m", to);

        return 0;
}

static int write_fsck_sysroot_service(
                const char *unit, /* Either SPECIAL_FSCK_ROOT_SERVICE or SPECIAL_FSCK_USR_SERVICE */
                const char *dir,
                const char *what,
                const char *extra_after) {

        _cleanup_free_ char *device = NULL, *escaped = NULL, *escaped2 = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        const char *fn;
        int r;

        /* Writes out special versions of systemd-root-fsck.service and systemd-usr-fsck.service for use in
         * the initrd. The regular statically shipped versions of these unit files use / and /usr for as
         * paths, which doesn't match what we need for the initrd (where the dirs are /sysroot +
         * /sysusr/usr), hence we overwrite those versions here. */

        escaped = specifier_escape(what);
        if (!escaped)
                return log_oom();

        escaped2 = cescape(escaped);
        if (!escaped2)
                return log_oom();

        fn = strjoina(dir, "/", unit);
        log_debug("Creating %s", fn);

        r = unit_name_from_path(what, ".device", &device);
        if (r < 0)
                return log_error_errno(r, "Failed to convert device \"%s\" to unit name: %m", what);

        f = fopen(fn, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m", fn);

        fprintf(f,
                "# Automatically generated by %1$s\n\n"
                "[Unit]\n"
                "Description=File System Check on %2$s\n"
                "Documentation=man:%3$s(8)\n"
                "DefaultDependencies=no\n"
                "BindsTo=%4$s\n"
                "Conflicts=shutdown.target\n"
                "After=%5$s%6$slocal-fs-pre.target %4$s\n"
                "Before=shutdown.target\n"
                "\n"
                "[Service]\n"
                "Type=oneshot\n"
                "RemainAfterExit=yes\n"
                "ExecStart=" SYSTEMD_FSCK_PATH " %7$s\n"
                "TimeoutSec=0\n",
                program_invocation_short_name,
                escaped,
                unit,
                device,
                strempty(extra_after),
                isempty(extra_after) ? "" : " ",
                escaped2);

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit file %s: %m", fn);

        return 0;
}

int generator_write_fsck_deps(
                FILE *f,
                const char *dir,
                const char *what,
                const char *where,
                const char *fstype) {

        int r;

        assert(f);
        assert(dir);
        assert(what);
        assert(where);

        /* Let's do an early exit if we are invoked for the root and /usr/ trees in the initrd, to avoid
         * generating confusing log messages */
        if (in_initrd() && PATH_IN_SET(where, "/", "/usr")) {
                log_debug("Skipping fsck for %s in initrd.", where);
                return 0;
        }

        if (!is_device_path(what)) {
                log_warning("Checking was requested for \"%s\", but it is not a device.", what);
                return 0;
        }

        if (!isempty(fstype) && !streq(fstype, "auto")) {
                r = fsck_exists_for_fstype(fstype);
                if (r < 0)
                        log_warning_errno(r, "Checking was requested for %s, but couldn't detect if fsck.%s may be used, proceeding: %m", what, fstype);
                else if (r == 0) {
                        /* treat missing check as essentially OK */
                        log_debug("Checking was requested for %s, but fsck.%s does not exist.", what, fstype);
                        return 0;
                }
        } else {
                r = fsck_exists();
                if (r < 0)
                        log_warning_errno(r, "Checking was requested for %s, but couldn't detect if the fsck command may be used, proceeding: %m", what);
                else if (r == 0) {
                        /* treat missing fsck as essentially OK */
                        log_debug("Checking was requested for %s, but the fsck command does not exist.", what);
                        return 0;
                }
        }

        if (path_equal(where, "/")) {
                const char *lnk;

                /* We support running the fsck instance for the root fs while it is already mounted, for
                 * compatibility with non-initrd boots. It's ugly, but it is how it is. Since – unlike for
                 * regular file systems – this means the ordering is reversed (i.e. mount *before* fsck) we
                 * have a separate fsck unit for this, independent of systemd-fsck@.service. */

                lnk = strjoina(dir, "/" SPECIAL_LOCAL_FS_TARGET ".wants/" SPECIAL_FSCK_ROOT_SERVICE);

                (void) mkdir_parents(lnk, 0755);
                if (symlink(SYSTEM_DATA_UNIT_DIR "/" SPECIAL_FSCK_ROOT_SERVICE, lnk) < 0)
                        return log_error_errno(errno, "Failed to create symlink %s: %m", lnk);

        } else {
                _cleanup_free_ char *_fsck = NULL;
                const char *fsck, *dep;

                if (in_initrd() && path_equal(where, "/sysroot")) {
                        r = write_fsck_sysroot_service(SPECIAL_FSCK_ROOT_SERVICE, dir, what, SPECIAL_INITRD_ROOT_DEVICE_TARGET);
                        if (r < 0)
                                return r;

                        fsck = SPECIAL_FSCK_ROOT_SERVICE;
                        dep = "Requires";

                } else if (in_initrd() && path_equal(where, "/sysusr/usr")) {
                        r = write_fsck_sysroot_service(SPECIAL_FSCK_USR_SERVICE, dir, what, NULL);
                        if (r < 0)
                                return r;

                        fsck = SPECIAL_FSCK_USR_SERVICE;
                        dep = "Requires";
                } else {
                        /* When this is /usr, then let's add a Wants= dependency, otherwise a Requires=
                         * dependency. Why? We can't possibly unmount /usr during shutdown, but if we have a
                         * Requires= from /usr onto a fsck@.service unit and that unit is shut down, then
                         * we'd have to unmount /usr too.  */

                        dep = path_equal(where, "/usr") ? "Wants" : "Requires";

                        r = unit_name_from_path_instance("systemd-fsck", what, ".service", &_fsck);
                        if (r < 0)
                                return log_error_errno(r, "Failed to create fsck service name: %m");

                        fsck = _fsck;
                }

                fprintf(f,
                        "%1$s=%2$s\n"
                        "After=%2$s\n",
                        dep, fsck);
        }

        return 0;
}

int generator_write_timeouts(
                const char *dir,
                const char *what,
                const char *where,
                const char *opts,
                char **filtered) {

        /* Configure how long we wait for a device that backs a mount point or a
         * swap partition to show up. This is useful to support endless device timeouts
         * for devices that show up only after user input, like crypto devices. */

        _cleanup_free_ char *node = NULL, *unit = NULL, *timeout = NULL;
        usec_t u;
        int r;

        r = fstab_filter_options(opts, "comment=systemd.device-timeout\0"
                                       "x-systemd.device-timeout\0",
                                 NULL, &timeout, NULL, filtered);
        if (r < 0) {
                log_warning_errno(r, "Failed to parse fstab options, ignoring: %m");
                return 0;
        }
        if (r == 0)
                return 0;

        r = parse_sec_fix_0(timeout, &u);
        if (r < 0) {
                log_warning("Failed to parse timeout for %s, ignoring: %s", where, timeout);
                return 0;
        }

        node = fstab_node_to_udev_node(what);
        if (!node)
                return log_oom();
        if (!is_device_path(node)) {
                log_warning("x-systemd.device-timeout ignored for %s", what);
                return 0;
        }

        r = unit_name_from_path(node, ".device", &unit);
        if (r < 0)
                return log_error_errno(r, "Failed to make unit name from path: %m");

        return write_drop_in_format(dir, unit, 50, "device-timeout",
                                    "# Automatically generated by %s\n"
                                    "# from supplied options \"%s\"\n\n"
                                    "[Unit]\n"
                                    "JobRunningTimeoutSec=%s",
                                    program_invocation_short_name,
                                    opts,
                                    timeout);
}

int generator_write_device_deps(
                const char *dir,
                const char *what,
                const char *where,
                const char *opts) {

        /* fstab records that specify _netdev option should apply the network
         * ordering on the actual device depending on network connection. If we
         * are not mounting real device (NFS, CIFS), we rely on _netdev effect
         * on the mount unit itself. */

        _cleanup_free_ char *node = NULL, *unit = NULL;
        int r;

        if (fstab_is_extrinsic(where, opts))
                return 0;

        if (!fstab_test_option(opts, "_netdev\0"))
                return 0;

        node = fstab_node_to_udev_node(what);
        if (!node)
                return log_oom();

        /* Nothing to apply dependencies to. */
        if (!is_device_path(node))
                return 0;

        r = unit_name_from_path(node, ".device", &unit);
        if (r < 0)
                return log_error_errno(r, "Failed to make unit name from path \"%s\": %m",
                                       node);

        /* See mount_add_default_dependencies for explanation why we create such
         * dependencies. */
        return write_drop_in_format(dir, unit, 50, "netdev-dependencies",
                                    "# Automatically generated by %s\n\n"
                                    "[Unit]\n"
                                    "After=" SPECIAL_NETWORK_ONLINE_TARGET " " SPECIAL_NETWORK_TARGET "\n"
                                    "Wants=" SPECIAL_NETWORK_ONLINE_TARGET "\n",
                                    program_invocation_short_name);
}

int generator_write_initrd_root_device_deps(const char *dir, const char *what) {
        _cleanup_free_ char *unit = NULL;
        int r;

        r = unit_name_from_path(what, ".device", &unit);
        if (r < 0)
                return log_error_errno(r, "Failed to make unit name from path \"%s\": %m",
                                       what);

        return write_drop_in_format(dir, SPECIAL_INITRD_ROOT_DEVICE_TARGET, 50, "root-device",
                                    "# Automatically generated by %s\n\n"
                                    "[Unit]\n"
                                    "Requires=%s\n"
                                    "After=%s",
                                    program_invocation_short_name,
                                    unit,
                                    unit);
}

int generator_hook_up_mkswap(
                const char *dir,
                const char *what) {

        _cleanup_free_ char *node = NULL, *unit = NULL, *escaped = NULL, *where_unit = NULL;
        _cleanup_free_ char *unit_file = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        node = fstab_node_to_udev_node(what);
        if (!node)
                return log_oom();

        /* Nothing to work on. */
        if (!is_device_path(node))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Cannot format something that is not a device node: %s",
                                       node);

        r = unit_name_from_path_instance("systemd-mkswap", node, ".service", &unit);
        if (r < 0)
                return log_error_errno(r, "Failed to make unit instance name from path \"%s\": %m",
                                       node);

        unit_file = path_join(dir, unit);
        if (!unit_file)
                return log_oom();

        log_debug("Creating %s", unit_file);

        escaped = cescape(node);
        if (!escaped)
                return log_oom();

        r = unit_name_from_path(what, ".swap", &where_unit);
        if (r < 0)
                return log_error_errno(r, "Failed to make unit name from path \"%s\": %m",
                                       what);

        f = fopen(unit_file, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m",
                                       unit_file);

        fprintf(f,
                "# Automatically generated by %s\n\n"
                "[Unit]\n"
                "Description=Make Swap on %%f\n"
                "Documentation=man:systemd-mkswap@.service(8)\n"
                "DefaultDependencies=no\n"
                "BindsTo=%%i.device\n"
                "Conflicts=shutdown.target\n"
                "After=%%i.device\n"
                "Before=shutdown.target %s\n"
                "\n"
                "[Service]\n"
                "Type=oneshot\n"
                "RemainAfterExit=yes\n"
                "ExecStart="SYSTEMD_MAKEFS_PATH " swap %s\n"
                "TimeoutSec=0\n",
                program_invocation_short_name,
                where_unit,
                escaped);

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit file %s: %m", unit_file);

        return generator_add_symlink(dir, where_unit, "requires", unit);
}

int generator_hook_up_mkfs(
                const char *dir,
                const char *what,
                const char *where,
                const char *type) {

        _cleanup_free_ char *node = NULL, *unit = NULL, *unit_file = NULL, *escaped = NULL, *where_unit = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        node = fstab_node_to_udev_node(what);
        if (!node)
                return log_oom();

        /* Nothing to work on. */
        if (!is_device_path(node))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Cannot format something that is not a device node: %s",
                                       node);

        if (!type || streq(type, "auto"))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Cannot format partition %s, filesystem type is not specified",
                                       node);

        r = unit_name_from_path_instance("systemd-makefs", node, ".service", &unit);
        if (r < 0)
                return log_error_errno(r, "Failed to make unit instance name from path \"%s\": %m",
                                       node);

        unit_file = path_join(dir, unit);
        if (!unit_file)
                return log_oom();

        log_debug("Creating %s", unit_file);

        escaped = cescape(node);
        if (!escaped)
                return log_oom();

        r = unit_name_from_path(where, ".mount", &where_unit);
        if (r < 0)
                return log_error_errno(r, "Failed to make unit name from path \"%s\": %m",
                                       where);

        f = fopen(unit_file, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m",
                                       unit_file);

        fprintf(f,
                "# Automatically generated by %s\n\n"
                "[Unit]\n"
                "Description=Make File System on %%f\n"
                "Documentation=man:systemd-makefs@.service(8)\n"
                "DefaultDependencies=no\n"
                "BindsTo=%%i.device\n"
                "Conflicts=shutdown.target\n"
                "After=%%i.device\n"
                /* fsck might or might not be used, so let's be safe and order
                 * ourselves before both systemd-fsck@.service and the mount unit. */
                "Before=shutdown.target systemd-fsck@%%i.service %s\n"
                "\n"
                "[Service]\n"
                "Type=oneshot\n"
                "RemainAfterExit=yes\n"
                "ExecStart="SYSTEMD_MAKEFS_PATH " %s %s\n"
                "TimeoutSec=0\n",
                program_invocation_short_name,
                where_unit,
                type,
                escaped);
        // XXX: what about local-fs-pre.target?

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit file %s: %m", unit_file);

        return generator_add_symlink(dir, where_unit, "requires", unit);
}

int generator_hook_up_growfs(
                const char *dir,
                const char *where,
                const char *target) {

        _cleanup_free_ char *unit = NULL, *escaped = NULL, *where_unit = NULL, *unit_file = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(dir);
        assert(where);

        escaped = cescape(where);
        if (!escaped)
                return log_oom();

        r = unit_name_from_path_instance("systemd-growfs", where, ".service", &unit);
        if (r < 0)
                return log_error_errno(r, "Failed to make unit instance name from path \"%s\": %m",
                                       where);

        r = unit_name_from_path(where, ".mount", &where_unit);
        if (r < 0)
                return log_error_errno(r, "Failed to make unit name from path \"%s\": %m",
                                       where);

        unit_file = path_join(dir, unit);
        if (!unit_file)
                return log_oom();

        log_debug("Creating %s", unit_file);

        f = fopen(unit_file, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m",
                                       unit_file);

        fprintf(f,
                "# Automatically generated by %s\n\n"
                "[Unit]\n"
                "Description=Grow File System on %%f\n"
                "Documentation=man:systemd-growfs@.service(8)\n"
                "DefaultDependencies=no\n"
                "BindsTo=%%i.mount\n"
                "Conflicts=shutdown.target\n"
                "After=systemd-repart.service %%i.mount\n"
                "Before=shutdown.target%s%s\n",
                program_invocation_short_name,
                target ? " " : "",
                strempty(target));

        if (empty_or_root(where)) /* Make sure the root fs is actually writable before we resize it */
                fprintf(f,
                        "After=systemd-remount-fs.service\n");

        fprintf(f,
                "\n"
                "[Service]\n"
                "Type=oneshot\n"
                "RemainAfterExit=yes\n"
                "ExecStart="SYSTEMD_GROWFS_PATH " %s\n"
                "TimeoutSec=0\n",
                escaped);

        return generator_add_symlink(dir, where_unit, "wants", unit);
}

int generator_enable_remount_fs_service(const char *dir) {
        /* Pull in systemd-remount-fs.service */
        return generator_add_symlink(dir, SPECIAL_LOCAL_FS_TARGET, "wants",
                                     SYSTEM_DATA_UNIT_DIR "/" SPECIAL_REMOUNT_FS_SERVICE);
}

int generator_write_blockdev_dependency(
                FILE *f,
                const char *what) {

        _cleanup_free_ char *escaped = NULL;
        int r;

        assert(f);
        assert(what);

        if (!path_startswith(what, "/dev/"))
                return 0;

        r = unit_name_path_escape(what, &escaped);
        if (r < 0)
                return log_error_errno(r, "Failed to escape device node path %s: %m", what);

        fprintf(f,
                "After=blockdev@%s.target\n",
                escaped);

        return 0;
}

int generator_write_cryptsetup_unit_section(
                FILE *f,
                const char *source) {

        assert(f);

        fprintf(f,
                "[Unit]\n"
                "Description=Cryptography Setup for %%I\n"
                "Documentation=man:crypttab(5) man:systemd-cryptsetup-generator(8) man:systemd-cryptsetup@.service(8)\n");

        if (source)
                fprintf(f, "SourcePath=%s\n", source);

        fprintf(f,
                "DefaultDependencies=no\n"
                "IgnoreOnIsolate=true\n"
                "After=cryptsetup-pre.target systemd-udevd-kernel.socket\n"
                "Before=blockdev@dev-mapper-%%i.target\n"
                "Wants=blockdev@dev-mapper-%%i.target\n");

        return 0;
}

int generator_write_cryptsetup_service_section(
                FILE *f,
                const char *name,
                const char *what,
                const char *key_file,
                const char *options) {

        _cleanup_free_ char *name_escaped = NULL, *what_escaped = NULL, *key_file_escaped = NULL, *options_escaped = NULL;

        assert(f);
        assert(name);
        assert(what);

        name_escaped = specifier_escape(name);
        if (!name_escaped)
                return log_oom();

        what_escaped = specifier_escape(what);
        if (!what_escaped)
                return log_oom();

        if (key_file) {
                key_file_escaped = specifier_escape(key_file);
                if (!key_file_escaped)
                        return log_oom();
        }

        if (options) {
                options_escaped = specifier_escape(options);
                if (!options_escaped)
                        return log_oom();
        }

        fprintf(f,
                "\n"
                "[Service]\n"
                "Type=oneshot\n"
                "RemainAfterExit=yes\n"
                "TimeoutSec=0\n"          /* The binary handles timeouts on its own */
                "KeyringMode=shared\n"    /* Make sure we can share cached keys among instances */
                "OOMScoreAdjust=500\n"    /* Unlocking can allocate a lot of memory if Argon2 is used */
                "ExecStart=" SYSTEMD_CRYPTSETUP_PATH " attach '%s' '%s' '%s' '%s'\n"
                "ExecStop=" SYSTEMD_CRYPTSETUP_PATH " detach '%s'\n",
                name_escaped, what_escaped, strempty(key_file_escaped), strempty(options_escaped),
                name_escaped);

        return 0;
}

int generator_write_veritysetup_unit_section(
                FILE *f,
                const char *source) {

        assert(f);

        fprintf(f,
                "[Unit]\n"
                "Description=Integrity Protection Setup for %%I\n"
                "Documentation=man:veritytab(5) man:systemd-veritysetup-generator(8) man:systemd-veritysetup@.service(8)\n");

        if (source)
                fprintf(f, "SourcePath=%s\n", source);

        fprintf(f,
                "DefaultDependencies=no\n"
                "IgnoreOnIsolate=true\n"
                "After=cryptsetup-pre.target systemd-udevd-kernel.socket\n"
                "Before=blockdev@dev-mapper-%%i.target\n"
                "Wants=blockdev@dev-mapper-%%i.target\n");

        return 0;
}

int generator_write_veritysetup_service_section(
                FILE *f,
                const char *name,
                const char *data_what,
                const char *hash_what,
                const char *roothash,
                const char *options) {

        _cleanup_free_ char *name_escaped = NULL, *data_what_escaped = NULL, *hash_what_escaped = NULL,
                            *roothash_escaped = NULL, *options_escaped = NULL;

        assert(f);
        assert(name);
        assert(data_what);
        assert(hash_what);

        name_escaped = specifier_escape(name);
        if (!name_escaped)
                return log_oom();

        data_what_escaped = specifier_escape(data_what);
        if (!data_what_escaped)
                return log_oom();

        hash_what_escaped = specifier_escape(hash_what);
        if (!hash_what_escaped)
                return log_oom();

        roothash_escaped = specifier_escape(roothash);
        if (!roothash_escaped)
                return log_oom();

        if (options) {
                options_escaped = specifier_escape(options);
                if (!options_escaped)
                        return log_oom();
        }

        fprintf(f,
                "\n"
                "[Service]\n"
                "Type=oneshot\n"
                "RemainAfterExit=yes\n"
                "ExecStart=" SYSTEMD_VERITYSETUP_PATH " attach '%s' '%s' '%s' '%s' '%s'\n"
                "ExecStop=" SYSTEMD_VERITYSETUP_PATH " detach '%s'\n",
                name_escaped, data_what_escaped, hash_what_escaped, roothash_escaped, strempty(options_escaped),
                name_escaped);

        return 0;
}

void log_setup_generator(void) {
        if (invoked_by_systemd()) {
                /* Disable talking to syslog/journal (i.e. the two IPC-based loggers) if we run in system context. */
                if (cg_pid_get_owner_uid(0, NULL) == -ENXIO /* not running in a per-user slice */)
                        log_set_prohibit_ipc(true);

                /* This effectively means: journal for per-user generators, kmsg otherwise */
                log_set_target(LOG_TARGET_JOURNAL_OR_KMSG);
        }

        log_parse_environment();
        (void) log_open();
}
