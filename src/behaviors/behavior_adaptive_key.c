/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_adaptive_key

#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/dlist.h>
#include <zephyr/kernel.h>

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#if DT_HAS_COMPAT_STATUS_OKAY(razen_vim_adaptive_guard)
#include <dt-bindings/zmk/hid_indicators.h>
#include <zmk/events/hid_indicators_changed.h>
#endif
#include <zmk/keys.h>
#include <zmk/matrix.h>
#include <zmk/keymap.h>

#include <zmk-adaptive-key/keys.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct key_list {
    size_t size;
    struct zmk_key_param keys[];
};

struct binding_list {
    size_t size;
    struct zmk_behavior_binding bindings[CONFIG_ZMK_ADAPTIVE_KEY_MAX_BINDINGS];
};

struct trigger_cfg {
    struct binding_list bindings;
    struct zmk_key_param swap_key;
    size_t trigger_keys_len;
    const struct zmk_key_param trigger_keys[CONFIG_ZMK_ADAPTIVE_KEY_MAX_TRIGGER_CONDITIONS];
    size_t prior_trigger_keys_len;
    const struct zmk_key_param prior_trigger_keys[CONFIG_ZMK_ADAPTIVE_KEY_MAX_TRIGGER_CONDITIONS];
    size_t prior_keys_len;
    const struct zmk_key_param prior_keys[CONFIG_ZMK_ADAPTIVE_KEY_HISTORY_DEPTH];
    int min_idle_ms;
    int max_idle_ms;
    bool delete_prior;
    bool strict_modifiers;
    bool continue_swap;
    bool continue_shift;
};

struct behavior_adaptive_key_config {
    uint8_t index;
    struct zmk_key_param input_key;
    struct binding_list default_binding;
    size_t triggers_len;
    const struct trigger_cfg *triggers;
    const struct key_list *dead_keys;
    uint32_t delete_keycode;
    bool skip_magic;
};

struct behavior_adaptive_key_data {
    const struct binding_list *pressed_bindings;
    struct zmk_keycode_state_changed skip_repeat_ev;
    bool using_skip_repeat;
};

// Global state.
struct zmk_key_param last_keycode;
int64_t last_timestamp;
bool last_keycode_is_dead;

struct zmk_key_param prev_keycode;
int64_t prev_timestamp;

static struct zmk_key_param history[CONFIG_ZMK_ADAPTIVE_KEY_HISTORY_DEPTH];

static struct {
    bool active;
    bool strict_modifiers;
    bool allow_shift;
    struct zmk_key_param input_key;
    struct zmk_key_param swap_key;
    const struct binding_list *input_bindings;
    const struct binding_list *swap_bindings;
    int max_idle_ms;
    int64_t timestamp;
} active_swap;

#if DT_HAS_COMPAT_STATUS_OKAY(razen_vim_adaptive_guard)
static bool adaptives_enabled = true;
#endif

static inline int press_adaptive_key_behavior(const struct behavior_adaptive_key_data *data,
                                              struct zmk_behavior_binding_event *event) {
    const struct binding_list *list = data->pressed_bindings;
    LOG_DBG("Iterating %d adaptive key bindings", list->size);
    if (list->size > 1) {

        for (int i = 0; i < list->size - 1; i++) {
            zmk_behavior_queue_add(event, list->bindings[i], true, CONFIG_ZMK_ADAPTIVE_KEY_TAP_MS);
            zmk_behavior_queue_add(event, list->bindings[i], false,
                                   CONFIG_ZMK_ADAPTIVE_KEY_WAIT_MS);
        }

        return zmk_behavior_queue_add(event, list->bindings[list->size - 1], true,
                                      CONFIG_ZMK_ADAPTIVE_KEY_TAP_MS);
    }

    return zmk_behavior_invoke_binding(&list->bindings[0], *event, true);
}

static inline int release_adaptive_key_behavior(const struct behavior_adaptive_key_data *data,
                                                struct zmk_behavior_binding_event *event) {
    LOG_DBG("Releasing adaptive key binding");
    const struct binding_list *list = data->pressed_bindings;
    if (list->size > 1) {
        return zmk_behavior_queue_add(event, list->bindings[list->size - 1], false,
                                      CONFIG_ZMK_ADAPTIVE_KEY_WAIT_MS);
    }

    return zmk_behavior_invoke_binding(&list->bindings[0], *event, false);
}

static bool keys_are_equal(const struct zmk_key_param *key, const struct zmk_key_param *other,
                           bool strict) {

    if (strict && key->modifiers != other->modifiers) {
        return false;
    }

    // *key mods can be subset of *other mods if strict is disabled.
    if (!strict && (key->modifiers & other->modifiers) != key->modifiers) {
        return false;
    }

    return key->page == other->page && key->id == other->id;
}

static void clear_active_swap(void) { active_swap.active = false; }

static bool trigger_is_true(const struct trigger_cfg *trigger,
                            struct behavior_adaptive_key_data *data, int64_t timestamp,
                            bool skip_magic) {
    if (trigger->min_idle_ms > -1 && (timestamp - last_timestamp) < trigger->min_idle_ms) {
        return false;
    }

    if (trigger->max_idle_ms > -1 && (timestamp - last_timestamp) > trigger->max_idle_ms) {
        return false;
    }

    const struct zmk_key_param *check_key = skip_magic ? &prev_keycode : &last_keycode;

    bool last_match = false;
    for (int i = 0; i < trigger->trigger_keys_len; i++) {
        if (keys_are_equal(&trigger->trigger_keys[i], check_key, trigger->strict_modifiers)) {
            last_match = true;
            break;
        }
    }
    if (!last_match) {
        return false;
    }

    if (trigger->prior_trigger_keys_len > 0) {
        bool prior_match = false;
        for (int i = 0; i < trigger->prior_trigger_keys_len; i++) {
            if (keys_are_equal(&trigger->prior_trigger_keys[i], &prev_keycode,
                               trigger->strict_modifiers)) {
                prior_match = true;
                break;
            }
        }
        if (!prior_match) {
            return false;
        }
    }

    if (trigger->prior_keys_len > 0) {
        for (int i = 0; i < trigger->prior_keys_len; i++) {
            const struct zmk_key_param *want =
                &trigger->prior_keys[trigger->prior_keys_len - 1 - i];
            if (!keys_are_equal(want, &history[1 + i], trigger->strict_modifiers)) {
                return false;
            }
        }
    }

    data->pressed_bindings = &trigger->bindings;
    return true;
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_adaptive_key_data *data = dev->data;

    if (!last_keycode.page && data->pressed_bindings) {
        LOG_ERR("Adaptive key pressed twice or no prior key press detected");
        return ZMK_BEHAVIOR_OPAQUE;
    }

    bool match = false;
    bool skip_triggers = false;
    const struct trigger_cfg *matched_trigger = NULL;
    const struct behavior_adaptive_key_config *config = dev->config;
#if DT_HAS_COMPAT_STATUS_OKAY(razen_vim_adaptive_guard)
    if (!adaptives_enabled) {
        data->pressed_bindings = &config->default_binding;
        press_adaptive_key_behavior(data, &event);
        return ZMK_BEHAVIOR_OPAQUE;
    }
#endif
    uint8_t mods = zmk_hid_get_explicit_mods();
    bool camel_case_boundary =
        (mods & (MOD_LSFT | MOD_RSFT)) && last_keycode.page == ZMK_HID_USAGE_PAGE(A) &&
        last_keycode.id >= ZMK_HID_USAGE_ID(A) && last_keycode.id <= ZMK_HID_USAGE_ID(Z) &&
        !(last_keycode.modifiers & (MOD_LSFT | MOD_RSFT));
    LOG_DBG("Comparing adaptive key triggers to last key press: usage_page 0x%02X keycode 0x%02X "
            "implicit_mods 0x%02X",
            last_keycode.page, last_keycode.id, last_keycode.modifiers);
    if (active_swap.active) {
        uint8_t allowed_mods = active_swap.allow_shift ? MOD_LSFT | MOD_RSFT : 0;
        bool valid = (active_swap.max_idle_ms < 0 ||
                      event.timestamp - active_swap.timestamp <= active_swap.max_idle_ms) &&
                     !(active_swap.strict_modifiers && (mods & ~allowed_mods)) &&
                     !camel_case_boundary;
        if (valid && keys_are_equal(&config->input_key, &active_swap.input_key, false)) {
            data->pressed_bindings = active_swap.input_bindings;
            active_swap.timestamp = event.timestamp;
            press_adaptive_key_behavior(data, &event);
            return ZMK_BEHAVIOR_OPAQUE;
        }
        if (valid && keys_are_equal(&config->input_key, &active_swap.swap_key, false)) {
            data->pressed_bindings = active_swap.swap_bindings;
            active_swap.timestamp = event.timestamp;
            press_adaptive_key_behavior(data, &event);
            return ZMK_BEHAVIOR_OPAQUE;
        }
        clear_active_swap();
        skip_triggers = true;
    }
    if (!camel_case_boundary && !skip_triggers) {
        for (int i = 0; i < config->triggers_len; i++) {
            if (trigger_is_true(&config->triggers[i], data, event.timestamp, config->skip_magic)) {
                matched_trigger = &config->triggers[i];
                match = true;
                break;
            }
        }
    }

    if (matched_trigger && matched_trigger->continue_swap) {
        active_swap.active = true;
        active_swap.strict_modifiers = matched_trigger->strict_modifiers;
        active_swap.allow_shift = matched_trigger->continue_shift;
        active_swap.input_key = config->input_key;
        active_swap.swap_key = matched_trigger->swap_key;
        active_swap.input_bindings = &matched_trigger->bindings;
        active_swap.swap_bindings = &config->default_binding;
        active_swap.max_idle_ms = matched_trigger->max_idle_ms;
        active_swap.timestamp = event.timestamp;
    }

    if (!match) {
        if (config->skip_magic && prev_keycode.page) {
            LOG_DBG("Skip-magic fallback");
            data->skip_repeat_ev = (struct zmk_keycode_state_changed){
                .usage_page = prev_keycode.page,
                .keycode = prev_keycode.id,
                .implicit_modifiers = prev_keycode.modifiers,
                .explicit_modifiers = 0,
                .state = true,
                .timestamp = k_uptime_get(),
            };
            data->using_skip_repeat = true;
            raise_zmk_keycode_state_changed(data->skip_repeat_ev);
            return ZMK_BEHAVIOR_OPAQUE;
        }
        LOG_DBG("No adaptive key match found, invoking default behavior");
        data->pressed_bindings = &config->default_binding;
    }

    press_adaptive_key_behavior(data, &event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_adaptive_key_data *data = dev->data;

    if (data->using_skip_repeat) {
        data->skip_repeat_ev.state = false;
        data->skip_repeat_ev.timestamp = k_uptime_get();
        raise_zmk_keycode_state_changed(data->skip_repeat_ev);
        data->using_skip_repeat = false;
        return ZMK_BEHAVIOR_OPAQUE;
    }

    if (data->pressed_bindings) {
        release_adaptive_key_behavior(data, &event);
        data->pressed_bindings = NULL;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_adaptive_key_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

static int adaptive_key_keycode_state_changed_listener(const zmk_event_t *eh);

ZMK_LISTENER(behavior_adaptive_key, adaptive_key_keycode_state_changed_listener);
ZMK_SUBSCRIPTION(behavior_adaptive_key, zmk_keycode_state_changed);

static bool key_list_contains(const struct key_list *list, const struct zmk_key_param *key) {
    for (int i = 0; i < list->size; i++) {
        if (keys_are_equal(&list->keys[i], key, true)) {
            return true;
        }
    }

    return false;
}

static const struct device *devs[DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT)];

static bool is_dead(const struct zmk_key_param *key) {
    // Could merge devices during instantiation for better performance.
    for (int i = 0; i < DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT); i++) {

        const struct device *dev = devs[i];
        if (dev == NULL) {
            continue;
        }

        const struct behavior_adaptive_key_config *config = dev->config;
        if (key_list_contains(config->dead_keys, key)) {
            return true;
        }
    }

    return false;
}

static void restore_history(int64_t timestamp) {
    last_keycode = history[0];
    prev_keycode = CONFIG_ZMK_ADAPTIVE_KEY_HISTORY_DEPTH > 1
                       ? history[1]
                       : (struct zmk_key_param){0};
    last_timestamp = last_keycode.page ? timestamp : 0;
    prev_timestamp = prev_keycode.page ? timestamp : 0;
    last_keycode_is_dead = false;
}

static void rewind_history(int64_t timestamp) {
    for (int i = 0; i < CONFIG_ZMK_ADAPTIVE_KEY_HISTORY_DEPTH - 1; i++) {
        history[i] = history[i + 1];
    }
    history[CONFIG_ZMK_ADAPTIVE_KEY_HISTORY_DEPTH - 1] = (struct zmk_key_param){0};
    restore_history(timestamp);
}

static void clear_history(void) {
    clear_active_swap();
    for (int i = 0; i < CONFIG_ZMK_ADAPTIVE_KEY_HISTORY_DEPTH; i++) {
        history[i] = (struct zmk_key_param){0};
    }
    restore_history(0);
}

#if DT_HAS_COMPAT_STATUS_OKAY(razen_vim_adaptive_guard)
static int vim_adaptive_guard_listener(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    uint8_t code = !!(ev->indicators & HID_INDICATOR_COMPOSE) |
                   (!!(ev->indicators & HID_INDICATOR_KANA) << 1) |
                   (!!(ev->indicators & HID_INDICATOR_SCROLL_LOCK) << 2);
    bool enabled = code == 0 || code == 2 || code == 5;
    if (enabled != adaptives_enabled) {
        adaptives_enabled = enabled;
        clear_history();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(vim_adaptive_guard, vim_adaptive_guard_listener);
ZMK_SUBSCRIPTION(vim_adaptive_guard, zmk_hid_indicators_changed);
#endif

static int adaptive_key_keycode_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL || (!ev->state && !last_keycode_is_dead)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

#if DT_HAS_COMPAT_STATUS_OKAY(razen_vim_adaptive_guard)
    if (!adaptives_enabled) {
        clear_history();
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

    const struct zmk_key_param key = {
        .modifiers = ev->implicit_modifiers | zmk_hid_get_explicit_mods(),
        .page = ev->usage_page,
        .id = ev->keycode,
    };

    if (!ev->state && last_keycode_is_dead) {
        if (is_dead(&key)) {
            return ZMK_EV_EVENT_HANDLED;
        } else {
            return ZMK_EV_EVENT_BUBBLE;
        }
    }

    if (key.page == ZMK_HID_USAGE_PAGE(BACKSPACE) &&
        key.id == ZMK_HID_USAGE_ID(BACKSPACE)) {
        clear_active_swap();
        if (key.modifiers) {
            clear_history();
        } else {
            rewind_history(ev->timestamp);
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (key.page == ZMK_HID_USAGE_PAGE(LEFT_CONTROL) &&
        key.id >= ZMK_HID_USAGE_ID(LEFT_CONTROL) &&
        key.id <= ZMK_HID_USAGE_ID(RIGHT_GUI)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (active_swap.active && !keys_are_equal(&key, &active_swap.input_key, false) &&
        !keys_are_equal(&key, &active_swap.swap_key, false)) {
        clear_active_swap();
    }

    prev_keycode = last_keycode;
    prev_timestamp = last_timestamp;

    last_keycode = key;
    last_timestamp = ev->timestamp;

    for (int i = CONFIG_ZMK_ADAPTIVE_KEY_HISTORY_DEPTH - 1; i > 0; i--) {
        history[i] = history[i - 1];
    }
    history[0] = key;

    last_keycode_is_dead = is_dead(&key) && !last_keycode_is_dead;
    if (last_keycode_is_dead) {
        return ZMK_EV_EVENT_HANDLED;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

static int behavior_adaptive_key_init(const struct device *dev) {
    const struct behavior_adaptive_key_config *config = dev->config;
    devs[config->index] = dev;
    return 0;
}

#define KEY_TRIGGER_ITEM(i, n, prop) ZMK_KEY_PARAM_DECODE(DT_PROP_BY_IDX(n, prop, i))

#define ZMK_KEY_PARAM_DECODE(param)                                                                \
    (struct zmk_key_param) {                                                                       \
        .modifiers = SELECT_MODS(param), .page = ZMK_HID_USAGE_PAGE(param),                        \
        .id = ZMK_HID_USAGE_ID(param),                                                             \
    }

#define TRANSFORMED_BINDINGS(n)                                                                    \
    (struct binding_list) {                                                                        \
        .size = DT_PROP_LEN(n, bindings),                                                          \
        .bindings = {LISTIFY(DT_PROP_LEN(n, bindings), ZMK_KEYMAP_EXTRACT_BINDING, (, ), n)},      \
    }

#define PROP_TRIGGERS(n, prop)                                                                     \
    {                                                                                              \
        .bindings = TRANSFORMED_BINDINGS(n),                                                       \
        .swap_key = ZMK_KEY_PARAM_DECODE(DT_PROP(n, swap_key)),                                    \
        .trigger_keys_len = DT_PROP_LEN(n, prop),                                                  \
        .trigger_keys = {LISTIFY(DT_PROP_LEN(n, prop), KEY_TRIGGER_ITEM, (, ), n, prop)},          \
        .prior_trigger_keys_len = DT_PROP_LEN(n, prior_trigger_keys),                              \
        .prior_trigger_keys = {LISTIFY(DT_PROP_LEN(n, prior_trigger_keys), KEY_TRIGGER_ITEM,       \
                                       (, ), n, prior_trigger_keys)},                              \
        .prior_keys_len = DT_PROP_LEN(n, prior_keys),                                              \
        .prior_keys = {LISTIFY(DT_PROP_LEN(n, prior_keys), KEY_TRIGGER_ITEM, (, ), n, prior_keys)}, \
        .min_idle_ms = DT_PROP(n, min_prior_idle_ms),                                              \
        .max_idle_ms = DT_PROP(n, max_prior_idle_ms),                                              \
        .strict_modifiers = DT_PROP(n, strict_modifiers),                                          \
        .continue_swap = DT_PROP(n, continue_swap),                                                \
        .continue_shift = DT_PROP(n, continue_shift),                                              \
    }

#define KEY_LIST_ITEM(i, n, prop) ZMK_KEY_PARAM_DECODE(DT_INST_PROP_BY_IDX(n, prop, i))

#define PROP_KEY_LIST(n, prop)                                                                     \
    COND_CODE_1(DT_NODE_HAS_PROP(DT_DRV_INST(n), prop),                                            \
                ({                                                                                 \
                    .size = DT_INST_PROP_LEN(n, prop),                                             \
                    .keys = {LISTIFY(DT_INST_PROP_LEN(n, prop), KEY_LIST_ITEM, (, ), n, prop)},    \
                }),                                                                                \
                ({.size = 0}))

#define AK_INST(n)                                                                                 \
    static const struct trigger_cfg adaptive_key_triggers_##n[] = {                                \
        DT_INST_FOREACH_CHILD_STATUS_OKAY_SEP_VARGS(n, PROP_TRIGGERS, (, ), trigger_keys)};        \
    static const struct key_list adaptive_key_ignore_keys_##n = PROP_KEY_LIST(n, dead_keys);       \
    static struct behavior_adaptive_key_data behavior_adaptive_key_data_##n = {};                  \
    static const struct behavior_adaptive_key_config behavior_adaptive_key_config_##n = {          \
        .index = n,                                                                                \
        .input_key = ZMK_KEY_PARAM_DECODE(DT_INST_PROP(n, input_key)),                              \
        .default_binding =                                                                         \
            (struct binding_list){                                                                 \
                .size = 1,                                                                         \
                .bindings = {ZMK_KEYMAP_EXTRACT_BINDING(0, DT_DRV_INST(n))},                       \
            },                                                                                     \
        .triggers = adaptive_key_triggers_##n,                                                     \
        .triggers_len = ARRAY_SIZE(adaptive_key_triggers_##n),                                     \
        .dead_keys = &adaptive_key_ignore_keys_##n,                                                \
        .skip_magic = DT_INST_PROP(n, skip_magic),                                                 \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_adaptive_key_init, NULL, &behavior_adaptive_key_data_##n,  \
                            &behavior_adaptive_key_config_##n, POST_KERNEL,                        \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_adaptive_key_driver_api);

DT_INST_FOREACH_STATUS_OKAY(AK_INST)
