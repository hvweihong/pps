/*
 * Parameter Shell Commands
 *
 * Shell interface for runtime parameter configuration.
 */

#include "param_config.h"
#include "bridge_config.h"
#include <zephyr/shell/shell.h>
#include <zephyr/kernel.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static void format_bytes_hex(const uint8_t *bytes, size_t len, char *text,
			     size_t text_size)
{
	static const char digits[] = "0123456789abcdef";

	if (text_size < len * 2u + 1u) {
		if (text_size != 0u) {
			text[0] = '\0';
		}
		return;
	}
	for (size_t i = 0u; i < len; i++) {
		text[i * 2u] = digits[bytes[i] >> 4];
		text[i * 2u + 1u] = digits[bytes[i] & 0x0fu];
	}
	text[len * 2u] = '\0';
}

/* Helper: find parameter ID by name */
static int find_param_by_name(const char *name, enum rb_param_id *id)
{
	enum rb_param_id i;
	const struct rb_param_descriptor *desc;

	for (i = 0; i < RB_PARAM_COUNT; i++) {
		desc = rb_param_get_descriptor(i);
		if (desc != NULL && strcmp(desc->name, name) == 0) {
			*id = i;
			return 0;
		}
	}

	return -ENOENT;
}

/* Helper: format parameter flags */
static void format_flags(uint32_t flags, char *buf, size_t size)
{
	if (flags & RB_PARAM_FLAG_REBOOT_REQUIRED) {
		snprintf(buf, size, "reboot");
	} else if (flags & RB_PARAM_FLAG_RUNTIME_UPDATE) {
		snprintf(buf, size, "runtime");
	} else {
		snprintf(buf, size, "none");
	}
}

/* param list - Display all parameters */
static int cmd_param_list(const struct shell *sh, size_t argc, char **argv)
{
	enum rb_param_id id;
	const struct rb_param_descriptor *desc;
	uint32_t value;
	uint32_t default_val;
	uint8_t bytes[32];
	size_t bytes_len;
	bool persisted;
	char flags_str[16];
	char value_str[80];
	char default_str[80];

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "%-30s %-20s %-20s %-10s %-10s",
		    "NAME", "VALUE", "DEFAULT", "PERSISTED", "FLAGS");
	shell_print(sh, "%-30s %-20s %-20s %-10s %-10s",
		    "-----------------------------", "--------------------", "--------------------",
		    "----------", "----------");

	for (id = 0; id < RB_PARAM_COUNT; id++) {
		desc = rb_param_get_descriptor(id);
		if (desc == NULL) {
			continue;
		}

		persisted = rb_param_is_persisted(id);
		format_flags(desc->flags, flags_str, sizeof(flags_str));

		if (desc->type == RB_PARAM_BYTES) {
			bytes_len = sizeof(bytes);
			if (rb_param_get_bytes(id, bytes, &bytes_len) == 0) {
				format_bytes_hex(bytes, bytes_len, value_str,
						 sizeof(value_str));
			} else {
				snprintf(value_str, sizeof(value_str), "(error)");
			}
			format_bytes_hex(desc->config.bytes.default_value,
					 desc->config.bytes.length, default_str,
					 sizeof(default_str));
		} else {
			if (rb_param_get_uint32(id, &value) != 0) {
				continue;
			}

			switch (desc->type) {
			case RB_PARAM_UINT32:
				default_val = desc->config.u32.default_value;
				break;
			case RB_PARAM_UINT16:
				default_val = desc->config.u16.default_value;
				break;
			case RB_PARAM_UINT8:
				default_val = desc->config.u8.default_value;
				break;
			case RB_PARAM_BOOL:
				default_val = desc->config.boolean.default_value ? 1 : 0;
				break;
			default:
				default_val = 0;
				break;
			}

			snprintf(value_str, sizeof(value_str), "%u", value);
			snprintf(default_str, sizeof(default_str), "%u", default_val);
		}

		shell_print(sh, "%-30s %-20s %-20s %-10s %-10s",
			    desc->name, value_str, default_str,
			    persisted ? "yes" : "no", flags_str);
	}

	return 0;
}

/* param get <name> - Query single parameter */
static int cmd_param_get(const struct shell *sh, size_t argc, char **argv)
{
	enum rb_param_id id;
	const struct rb_param_descriptor *desc;
	uint32_t value;
	uint8_t bytes[32];
	size_t bytes_len = sizeof(bytes);
	bool persisted;
	char flags_str[16];
	char value_str[65];
	int ret;

	if (argc < 2) {
		shell_error(sh, "Usage: param get <name>");
		return -EINVAL;
	}

	ret = find_param_by_name(argv[1], &id);
	if (ret != 0) {
		shell_error(sh, "Unknown parameter: %s", argv[1]);
		return ret;
	}

	desc = rb_param_get_descriptor(id);
	persisted = rb_param_is_persisted(id);
	format_flags(desc->flags, flags_str, sizeof(flags_str));
	if (desc->type == RB_PARAM_BYTES) {
		ret = rb_param_get_bytes(id, bytes, &bytes_len);
		if (ret != 0) {
			shell_error(sh, "Failed to read parameter: %d", ret);
			return ret;
		}
		format_bytes_hex(bytes, bytes_len, value_str, sizeof(value_str));
		shell_print(sh, "%s = %s (%s, %s)", desc->name, value_str,
			    persisted ? "persisted" : "default", flags_str);
	} else {
		ret = rb_param_get_uint32(id, &value);
		if (ret != 0) {
			shell_error(sh, "Failed to read parameter: %d", ret);
			return ret;
		}
		shell_print(sh, "%s = %u (%s, %s)", desc->name, value,
			    persisted ? "persisted" : "default", flags_str);
	}

	if (rb_param_requires_reboot(id)) {
		shell_print(sh, "Note: Reboot required for changes to take effect");
	}

	return 0;
}

/* param set <name> <value> - Set and persist parameter */
static int cmd_param_set(const struct shell *sh, size_t argc, char **argv)
{
	enum rb_param_id id;
	const struct rb_param_descriptor *desc;
	uint32_t value;
	uint8_t bytes[RB_GROUP_KEY_BYTES];
	char *end;
	int ret;

	if (argc != 3u) {
		shell_error(sh, "Usage: param set <name> <value>");
		return -EINVAL;
	}

	ret = find_param_by_name(argv[1], &id);
	if (ret != 0) {
		shell_error(sh, "Unknown parameter: %s", argv[1]);
		return ret;
	}

	desc = rb_param_get_descriptor(id);

	/* Parse value */
	if (desc->type == RB_PARAM_BYTES) {
		ret = rb_group_key_parse(argv[2], bytes);
		if (ret != 0) {
			shell_error(sh, "Byte parameter requires exactly 32 hexadecimal characters");
			return -EINVAL;
		}
		ret = rb_param_set_bytes(id, bytes, sizeof(bytes));
	} else if (desc->type == RB_PARAM_BOOL) {
		if (strcmp(argv[2], "true") == 0 || strcmp(argv[2], "1") == 0) {
			ret = rb_param_set_bool(id, true);
		} else if (strcmp(argv[2], "false") == 0 || strcmp(argv[2], "0") == 0) {
			ret = rb_param_set_bool(id, false);
		} else {
			shell_error(sh, "Boolean parameter requires true/false or 0/1");
			return -EINVAL;
		}
	} else {
		errno = 0;
		value = (uint32_t)strtoul(argv[2], &end, 0);
		if (errno != 0 || end == argv[2] || *end != '\0') {
			shell_error(sh, "Invalid numeric value");
			return -EINVAL;
		}
		ret = rb_param_set_uint32(id, value);
	}

	if (ret == -EINVAL) {
		/* Show valid range */
		switch (desc->type) {
		case RB_PARAM_UINT32:
			shell_error(sh, "Value out of range [%u, %u]",
				    desc->config.u32.min, desc->config.u32.max);
			break;
		case RB_PARAM_UINT16:
			shell_error(sh, "Value out of range [%u, %u]",
				    desc->config.u16.min, desc->config.u16.max);
			break;
		case RB_PARAM_UINT8:
			shell_error(sh, "Value out of range [%u, %u]",
				    desc->config.u8.min, desc->config.u8.max);
			break;
		default:
			shell_error(sh, "Invalid value");
			break;
		}
		return ret;
	}

	if (ret != 0) {
		shell_error(sh, "Failed to set parameter: %d", ret);
		return ret;
	}

	if (rb_param_requires_reboot(id)) {
		shell_print(sh, "%s set to %s. Reboot required to apply.",
			    desc->name, argv[2]);
	} else {
		shell_print(sh, "%s set to %s. Updated at runtime.",
			    desc->name, argv[2]);
	}

	return 0;
}

/* param clear <name> - Clear persisted value */
static int cmd_param_clear(const struct shell *sh, size_t argc, char **argv)
{
	enum rb_param_id id;
	const struct rb_param_descriptor *desc;
	uint32_t default_val;
	uint8_t bytes[32];
	size_t bytes_len = sizeof(bytes);
	char value_str[65];
	int ret;

	if (argc < 2) {
		shell_error(sh, "Usage: param clear <name>");
		return -EINVAL;
	}

	ret = find_param_by_name(argv[1], &id);
	if (ret != 0) {
		shell_error(sh, "Unknown parameter: %s", argv[1]);
		return ret;
	}

	desc = rb_param_get_descriptor(id);
	ret = rb_param_clear(id);
	if (ret != 0) {
		shell_error(sh, "Failed to clear parameter: %d", ret);
		return ret;
	}

	if (desc->type == RB_PARAM_BYTES) {
		ret = rb_param_get_bytes(id, bytes, &bytes_len);
		if (ret != 0) {
			return ret;
		}
		format_bytes_hex(bytes, bytes_len, value_str, sizeof(value_str));
		shell_print(sh, "%s cleared, using default (%s)", desc->name,
			    value_str);
	} else {
		rb_param_get_uint32(id, &default_val);
		shell_print(sh, "%s cleared, using default (%u)", desc->name,
			    default_val);
	}

	return 0;
}

/* param reset - Factory reset all parameters */
static int cmd_param_reset(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Resetting all parameters to defaults...");

	ret = rb_param_reset_all();
	if (ret != 0) {
		shell_error(sh, "Failed to reset parameters: %d", ret);
		return ret;
	}

	shell_print(sh, "All parameters cleared. Reboot to restore defaults.");

	return 0;
}

/* Define param subcommands */
SHELL_STATIC_SUBCMD_SET_CREATE(sub_param,
	SHELL_CMD(list, NULL, "List all parameters", cmd_param_list),
	SHELL_CMD(get, NULL, "Get parameter value: param get <name>", cmd_param_get),
	SHELL_CMD(set, NULL, "Set parameter value: param set <name> <value>", cmd_param_set),
	SHELL_CMD(clear, NULL, "Clear parameter: param clear <name>", cmd_param_clear),
	SHELL_CMD(reset, NULL, "Reset all parameters to defaults", cmd_param_reset),
	SHELL_SUBCMD_SET_END
);

/* Register param command */
SHELL_CMD_REGISTER(param, &sub_param, "Parameter configuration", NULL);
