#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <drm.h>
#include <stdlib.h>
#include <libudev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>
#include "backend/drm/drm.h"
#include "backend/drm/util.h"

int32_t calculate_refresh_rate(const drmModeModeInfo *mode) {
	int32_t refresh = (mode->clock * 1000000LL / mode->htotal +
		mode->vtotal / 2) / mode->vtotal;

	if (mode->flags & DRM_MODE_FLAG_INTERLACE) {
		refresh *= 2;
	}

	if (mode->flags & DRM_MODE_FLAG_DBLSCAN) {
		refresh /= 2;
	}

	if (mode->vscan > 1) {
		refresh /= mode->vscan;
	}

	return refresh;
}

static const char *get_manufacturer(struct udev_hwdb *hwdb, uint16_t code) {
	static char pnp_id[4];

	// The ASCII 3-letter manufacturer PnP ID is encoded in 5-bit codes
	pnp_id[0] = ((code >> 10) & 0x1F) + '@';
	pnp_id[1] = ((code >> 5) & 0x1F) + '@';
	pnp_id[2] = ((code >> 0) & 0x1F) + '@';
	pnp_id[3] = '\0';

	if (hwdb == NULL) {
		return pnp_id;
	}

	char query[32];
	snprintf(query, sizeof(query), "acpi:%s:", pnp_id);
	struct udev_list_entry *acpi_entry =
		udev_hwdb_get_properties_list_entry(hwdb, query, 0);
	if (acpi_entry == NULL) {
		return pnp_id;
	}

	struct udev_list_entry *vendor_entry =
		udev_list_entry_get_by_name(acpi_entry, "ID_VENDOR_FROM_DATABASE");
	if (vendor_entry == NULL) {
		return pnp_id;
	}

	return udev_list_entry_get_value(vendor_entry);
}

/* See https://en.wikipedia.org/wiki/Extended_Display_Identification_Data for layout of EDID data.
 * We don't parse the EDID properly. We just expect to receive valid data.
 */
void parse_edid(struct wlr_drm_connector *conn, size_t len, const uint8_t *data) {
	struct wlr_output *output = &conn->output;

	free(output->make);
	free(output->model);
	free(output->serial);
	output->make = NULL;
	output->model = NULL;
	output->serial = NULL;

	if (!data || len < 128) {
		return;
	}

	uint16_t id = (data[8] << 8) | data[9];
	output->make = strdup(get_manufacturer(conn->backend->hwdb, id));

	uint16_t model = data[10] | (data[11] << 8);
	char model_str[32];
	snprintf(model_str, sizeof(model_str), "0x%04" PRIX16, model);

	uint32_t serial = data[12] | (data[13] << 8) | (data[14] << 8) | (data[15] << 8);
	char serial_str[32];
	if (serial != 0) {
		snprintf(serial_str, sizeof(serial_str), "0x%08" PRIX32, serial);
	} else {
		serial_str[0] = '\0';
	}

	for (size_t i = 72; i <= 108; i += 18) {
		uint16_t flag = (data[i] << 8) | data[i + 1];
		if (flag == 0 && data[i + 3] == 0xFC) {
			snprintf(model_str, sizeof(model_str), "%.13s", &data[i + 5]);

			// Monitor names are terminated by newline if they're too short
			char *nl = strchr(model_str, '\n');
			if (nl) {
				*nl = '\0';
			}
		} else if (flag == 0 && data[i + 3] == 0xFF) {
			snprintf(serial_str, sizeof(serial_str), "%.13s", &data[i + 5]);

			// Monitor serial numbers are terminated by newline if they're too
			// short
			char* nl = strchr(serial_str, '\n');

			if (nl) {
				*nl = '\0';
			}
		}
	}

	output->model = strdup(model_str);
	if (serial_str[0] != '\0') {
		output->serial = strdup(serial_str);
	}
}

const char *conn_get_name(uint32_t type_id) {
	switch (type_id) {
	case DRM_MODE_CONNECTOR_Unknown:     return "Unknown";
	case DRM_MODE_CONNECTOR_VGA:         return "VGA";
	case DRM_MODE_CONNECTOR_DVII:        return "DVI-I";
	case DRM_MODE_CONNECTOR_DVID:        return "DVI-D";
	case DRM_MODE_CONNECTOR_DVIA:        return "DVI-A";
	case DRM_MODE_CONNECTOR_Composite:   return "Composite";
	case DRM_MODE_CONNECTOR_SVIDEO:      return "SVIDEO";
	case DRM_MODE_CONNECTOR_LVDS:        return "LVDS";
	case DRM_MODE_CONNECTOR_Component:   return "Component";
	case DRM_MODE_CONNECTOR_9PinDIN:     return "DIN";
	case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
	case DRM_MODE_CONNECTOR_HDMIA:       return "HDMI-A";
	case DRM_MODE_CONNECTOR_HDMIB:       return "HDMI-B";
	case DRM_MODE_CONNECTOR_TV:          return "TV";
	case DRM_MODE_CONNECTOR_eDP:         return "eDP";
	case DRM_MODE_CONNECTOR_VIRTUAL:     return "Virtual";
	case DRM_MODE_CONNECTOR_DSI:         return "DSI";
	case DRM_MODE_CONNECTOR_DPI:         return "DPI";
	case DRM_MODE_CONNECTOR_WRITEBACK:   return "Writeback";
#ifdef DRM_MODE_CONNECTOR_SPI
	case DRM_MODE_CONNECTOR_SPI:         return "SPI";
#endif
#ifdef DRM_MODE_CONNECTOR_USB
	case DRM_MODE_CONNECTOR_USB:         return "USB";
#endif
	default:                             return "Unknown";
	}
}

static bool is_taken(size_t n, const uint32_t arr[static n], uint32_t key) {
	for (size_t i = 0; i < n; ++i) {
		if (arr[i] == key) {
			return true;
		}
	}
	return false;
}

/*
 * Store all of the non-recursive state in a struct, so we aren't literally
 * passing 12 arguments to a function.
 */
struct match_state {
	const size_t num_objs;
	const uint32_t *restrict objs;
	const size_t num_res;
	size_t score;
	size_t replaced;
	uint32_t *restrict res;
	uint32_t *restrict best;
	const uint32_t *restrict orig;
	bool exit_early;
};

/*
 * skips: The number of SKIP elements encountered so far.
 * score: The number of resources we've matched so far.
 * replaced: The number of changes from the original solution.
 * i: The index of the current element.
 *
 * This tries to match a solution as close to st->orig as it can.
 *
 * Returns whether we've set a new best element with this solution.
 */
static bool match_obj_(struct match_state *st, size_t skips, size_t score, size_t replaced, size_t i) {
	// Finished
	if (i >= st->num_res) {
		if (score > st->score ||
				(score == st->score && replaced < st->replaced)) {
			st->score = score;
			st->replaced = replaced;
			memcpy(st->best, st->res, sizeof(st->best[0]) * st->num_res);

			st->exit_early = (st->score == st->num_res - skips
					|| st->score == st->num_objs)
					&& st->replaced == 0;

			return true;
		} else {
			return false;
		}
	}

	if (st->orig[i] == SKIP) {
		st->res[i] = SKIP;
		return match_obj_(st, skips + 1, score, replaced, i + 1);
	}

	bool has_best = false;

	/*
	 * Attempt to use the current solution first, to try and avoid
	 * recalculating everything
	 */
	if (st->orig[i] != UNMATCHED && !is_taken(i, st->res, st->orig[i])) {
		st->res[i] = st->orig[i];
		size_t obj_score = st->objs[st->res[i]] != 0 ? 1 : 0;
		if (match_obj_(st, skips, score + obj_score, replaced, i + 1)) {
			has_best = true;
		}
	}
	if (st->orig[i] == UNMATCHED) {
		st->res[i] = UNMATCHED;
		if (match_obj_(st, skips, score, replaced, i + 1)) {
			has_best = true;
		}
	}
	if (st->exit_early) {
		return true;
	}

	if (st->orig[i] != UNMATCHED) {
		++replaced;
	}

	for (size_t candidate = 0; candidate < st->num_objs; ++candidate) {
		// We tried this earlier
		if (candidate == st->orig[i]) {
			continue;
		}

		// Not compatible
		if (!(st->objs[candidate] & (1 << i))) {
			continue;
		}

		// Already taken
		if (is_taken(i, st->res, candidate)) {
			continue;
		}

		st->res[i] = candidate;
		size_t obj_score = st->objs[candidate] != 0 ? 1 : 0;
		if (match_obj_(st, skips, score + obj_score, replaced, i + 1)) {
			has_best = true;
		}

		if (st->exit_early) {
			return true;
		}
	}

	if (has_best) {
		return true;
	}

	// Maybe this resource can't be matched
	st->res[i] = UNMATCHED;
	return match_obj_(st, skips, score, replaced, i + 1);
}

size_t match_obj(size_t num_objs, const uint32_t objs[static restrict num_objs],
		size_t num_res, const uint32_t res[static restrict num_res],
		uint32_t out[static restrict num_res]) {
	uint32_t solution[num_res];
	for (size_t i = 0; i < num_res; ++i) {
		solution[i] = UNMATCHED;
	}

	struct match_state st = {
		.num_objs = num_objs,
		.num_res = num_res,
		.score = 0,
		.replaced = SIZE_MAX,
		.objs = objs,
		.res = solution,
		.best = out,
		.orig = res,
		.exit_early = false,
	};

	match_obj_(&st, 0, 0, 0, 0);
	return st.score;
}
