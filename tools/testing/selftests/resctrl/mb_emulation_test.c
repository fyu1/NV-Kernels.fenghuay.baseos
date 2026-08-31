// SPDX-License-Identifier: GPL-2.0
/*
 * MB (memory bandwidth) control emulation test (ARM MPAM)
 *
 * Exercises info/MB/control_mode, the nested MB_NODE control under
 * info/MB/schemata/MB/, and schemata visibility when switching between
 * legacy (MB:) and native (MB_NODE:) modes.
 */
#include <fcntl.h>
#include <limits.h>

#include "resctrl.h"

#define MB_INFO			INFO_PATH "/MB"
#define MB_MODE_PATH		MB_INFO "/control_mode"
#define MB_SCHEMATA_DIR		MB_INFO "/schemata"
#define MB_CTRL_PATH		MB_SCHEMATA_DIR "/MB"
#define MB_NODE_NESTED_PATH	MB_CTRL_PATH "/MB_NODE"
#define MB_NODE_SIBLING_PATH	MB_SCHEMATA_DIR "/MB_NODE"
#define ROOT_SCHEMATA_PATH	RESCTRL_PATH "/schemata"

static int read_file(const char *path, char *buf, size_t buflen)
{
	ssize_t ret;
	int fd, n;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -errno;

	ret = read(fd, buf, buflen - 1);
	close(fd);
	if (ret < 0)
		return -errno;

	n = (int)ret;
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		n--;
	buf[n] = '\0';

	return 0;
}

static bool path_is_dir(const char *path)
{
	struct stat st;

	if (stat(path, &st))
		return false;

	return S_ISDIR(st.st_mode);
}

static int mb_read_mode(char *buf, size_t len)
{
	return read_file(MB_MODE_PATH, buf, len);
}

static int mb_write_mode(const char *mode)
{
	char msg[32];
	int fd, ret, len;

	len = snprintf(msg, sizeof(msg), "%s\n", mode);
	if (len < 0 || len >= (int)sizeof(msg))
		return -EINVAL;

	fd = open(MB_MODE_PATH, O_WRONLY);
	if (fd < 0)
		return -errno;

	ret = write(fd, msg, len) == len ? 0 : -errno;
	close(fd);

	return ret;
}

static bool mb_mode_active_is(const char *mode_text, const char *mode)
{
	char pattern[32];

	snprintf(pattern, sizeof(pattern), "[%s]", mode);
	return strstr(mode_text, pattern);
}

static int mb_read_root_schemata(char *buf, size_t len)
{
	return read_file(ROOT_SCHEMATA_PATH, buf, len);
}

/*
 * resctrl right-aligns schemata names with leading whitespace, so a control
 * line looks like "             MB:0=100...". Treat prefix as matching when it
 * appears at the start of a line, ignoring any leading spaces or tabs.
 */
static const char *schemata_find_line(const char *schemata, const char *prefix)
{
	const char *p;

	if (!schemata || !prefix)
		return NULL;

	for (p = strstr(schemata, prefix); p; p = strstr(p + 1, prefix)) {
		const char *q = p;

		while (q > schemata && (q[-1] == ' ' || q[-1] == '\t'))
			q--;

		if (q == schemata || q[-1] == '\n')
			return p;
	}

	return NULL;
}

static bool schemata_has_prefix(const char *schemata, const char *prefix)
{
	return schemata_find_line(schemata, prefix);
}

static int mb_expect_nested_hierarchy(const char *why)
{
	if (!path_is_dir(MB_NODE_NESTED_PATH) || path_is_dir(MB_NODE_SIBLING_PATH)) {
		ksft_print_msg("%s: MB_NODE should be nested, nested=%d sibling=%d\n",
			       why, path_is_dir(MB_NODE_NESTED_PATH),
			       path_is_dir(MB_NODE_SIBLING_PATH));
		return -EINVAL;
	}

	return 0;
}

static int mb_test_mode_read(void)
{
	char mode[64];
	int ret;

	ret = mb_read_mode(mode, sizeof(mode));
	if (ret)
		return ret;

	if (!mb_mode_active_is(mode, "legacy") && !mb_mode_active_is(mode, "native")) {
		ksft_print_msg("mode file has no active mode: '%s'\n", mode);
		return -EINVAL;
	}

	ksft_print_msg("mode file: %s\n", mode);
	return 0;
}

static int mb_test_schemata_visibility(bool expect_mb_line, const char *mode)
{
	char schemata[4096];
	bool has_mb, has_mb_node;
	int ret;

	ret = mb_read_root_schemata(schemata, sizeof(schemata));
	if (ret)
		return ret;

	has_mb = schemata_has_prefix(schemata, "MB:");
	has_mb_node = schemata_has_prefix(schemata, "MB_NODE:");

	if (expect_mb_line) {
		if (!has_mb) {
			ksft_print_msg("%s: expected MB: line in schemata\n", mode);
			return -EINVAL;
		}
		if (has_mb_node) {
			ksft_print_msg("%s: MB_NODE: line should be hidden in schemata\n",
				       mode);
			return -EINVAL;
		}
	} else {
		if (has_mb) {
			ksft_print_msg("%s: MB: line should be hidden in schemata\n",
				       mode);
			return -EINVAL;
		}
		if (!has_mb_node) {
			ksft_print_msg("%s: expected MB_NODE: line in schemata\n",
				       mode);
			return -EINVAL;
		}
	}

	ksft_print_msg("switched to %s: schemata MB:%d MB_NODE:%d\n",
		       mode, has_mb, has_mb_node);
	return 0;
}

static int mb_restore_mode(const char *orig_mode)
{
	if (mb_mode_active_is(orig_mode, "legacy"))
		return mb_write_mode("legacy");
	if (mb_mode_active_is(orig_mode, "native"))
		return mb_write_mode("native");

	return 0;
}

static int mb_verify_active_mode(const char *mode)
{
	char buf[64];
	int ret;

	ret = mb_read_mode(buf, sizeof(buf));
	if (ret)
		return ret;

	if (!mb_mode_active_is(buf, mode)) {
		ksft_print_msg("mode switch to %s failed, mode file: '%s'\n",
			       mode, buf);
		return -EINVAL;
	}

	ksft_print_msg("switched to %s: mode file='%s'\n", mode, buf);
	return 0;
}

/*
 * Switch to @mode and verify the mode file, the nested info/MB/schemata
 * layout (unchanged by the switch), and root schemata visibility.
 */
static int mb_switch_and_check(const char *mode)
{
	bool legacy = !strcmp(mode, "legacy");
	int ret;

	ksft_print_msg("--- switching to %s mode ---\n", mode);

	ret = mb_write_mode(mode);
	if (ret) {
		ksft_print_msg("failed to write %s mode\n", mode);
		return ret;
	}

	ret = mb_verify_active_mode(mode);
	if (ret)
		return ret;

	ret = mb_expect_nested_hierarchy(mode);
	if (ret)
		return ret;

	return mb_test_schemata_visibility(legacy, mode);
}

static int mb_emulation_run_test(const struct resctrl_test *test,
				 const struct user_params *uparams)
{
	char orig_mode[64];
	int ret;

	(void)test;
	(void)uparams;

	ret = mb_expect_nested_hierarchy("initial");
	if (ret)
		return ret;

	ret = mb_read_mode(orig_mode, sizeof(orig_mode));
	if (ret) {
		ksft_print_msg("failed to read %s\n", MB_MODE_PATH);
		return ret;
	}

	ret = mb_test_mode_read();
	if (ret)
		goto out_restore;

	/* Cycle legacy -> native -> legacy, checking each transition. */
	ret = mb_switch_and_check("legacy");
	if (ret)
		goto out_restore;

	ret = mb_switch_and_check("native");
	if (ret)
		goto out_restore;

	ret = mb_switch_and_check("legacy");
	if (ret)
		goto out_restore;

out_restore:
	if (mb_restore_mode(orig_mode))
		ksft_print_msg("warning: failed to restore original mode\n");

	return ret;
}

static bool mb_emulation_feature_check(const struct resctrl_test *test)
{
	(void)test;

	if (!resctrl_resource_exists("MB"))
		return false;

	if (!resource_info_file_exists("MB", "control_mode"))
		return false;

	if (!path_is_dir(MB_SCHEMATA_DIR) || !path_is_dir(MB_CTRL_PATH))
		return false;

	return path_is_dir(MB_NODE_NESTED_PATH);
}

struct resctrl_test mb_emulation_test = {
	.name = "mb_emulation",
	.group = "mb",
	.resource = "MB",
	.feature_check = mb_emulation_feature_check,
	.run_test = mb_emulation_run_test,
};
