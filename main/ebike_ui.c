#include "ebike_ui.h"

#include <math.h>
#include <stdio.h>

#define EBIKE_SCREEN_WIDTH       320
#define EBIKE_SCREEN_HEIGHT      240

#define EBIKE_TEMP_MIN_C         0
#define EBIKE_TEMP_MAX_C         120
#define EBIKE_TEMP_WARM_C        50
#define EBIKE_TEMP_HOT_C         90

#define EBIKE_POWER_MIN_W        0
#define EBIKE_POWER_MAX_W        2000
#define EBIKE_POWER_MEDIUM_W     700
#define EBIKE_POWER_HIGH_W       1300

#define COLOR_BG_TOP             lv_color_hex(0x061217)
#define COLOR_BG_BOTTOM          lv_color_hex(0x010608)
#define COLOR_TRACK              lv_color_hex(0x123038)
#define COLOR_DIVIDER            lv_color_hex(0x17363D)
#define COLOR_PRIMARY            lv_color_hex(0xF6FBFC)
#define COLOR_SECONDARY          lv_color_hex(0xBED0D4)
#define COLOR_MUTED              lv_color_hex(0x789198)
#define COLOR_CYAN               lv_color_hex(0x16DDF5)
#define COLOR_GREEN              lv_color_hex(0x39D98A)
#define COLOR_AMBER              lv_color_hex(0xFFB83D)
#define COLOR_RED                lv_color_hex(0xFF5263)
#define COLOR_PILL               lv_color_hex(0x10272D)
#define COLOR_PILL_OFF           lv_color_hex(0x0A1A1F)

#if LV_FONT_MONTSERRAT_48
#define FONT_SPEED               (&lv_font_montserrat_48)
#else
#define FONT_SPEED               LV_FONT_DEFAULT
#endif

#if LV_FONT_MONTSERRAT_20
#define FONT_GAUGE_VALUE         (&lv_font_montserrat_20)
#else
#define FONT_GAUGE_VALUE         LV_FONT_DEFAULT
#endif

#if LV_FONT_MONTSERRAT_16
#define FONT_UNIT                (&lv_font_montserrat_16)
#else
#define FONT_UNIT                LV_FONT_DEFAULT
#endif

#if LV_FONT_MONTSERRAT_14
#define FONT_DATA                (&lv_font_montserrat_14)
#else
#define FONT_DATA                LV_FONT_DEFAULT
#endif

#if LV_FONT_MONTSERRAT_12
#define FONT_SMALL               (&lv_font_montserrat_12)
#else
#define FONT_SMALL               LV_FONT_DEFAULT
#endif

typedef struct {
    lv_obj_t *root;

    lv_obj_t *mode_buttons[EBIKE_MODE_COUNT];
    lv_obj_t *mode_labels[EBIKE_MODE_COUNT];

    lv_obj_t *temperature_arc;
    lv_obj_t *power_arc;
    lv_obj_t *temperature_value;
    lv_obj_t *temperature_degree_dot;
    lv_obj_t *temperature_caption;
    lv_obj_t *power_value;

    lv_obj_t *speed_value;
    lv_obj_t *efficiency_value;
    lv_obj_t *pas_value;

    lv_obj_t *battery_fill;
    lv_obj_t *battery_percent;
    lv_obj_t *battery_voltage;
    lv_obj_t *link_dot;
    lv_obj_t *link_text;
    lv_obj_t *current_value;

    lv_obj_t *trip_caption;
    lv_obj_t *trip_value;
    lv_obj_t *acceleration_value;
    lv_obj_t *acceleration_fill;
    lv_obj_t *range_value;

    lv_obj_t *ota_overlay;
    lv_obj_t *ota_title;
    lv_obj_t *ota_detail;
    lv_obj_t *ota_progress;
    lv_obj_t *ota_progress_text;
    lv_obj_t *ota_install_button;
    lv_obj_t *ota_later_button;
} ebike_ui_objects_t;

static ebike_ui_objects_t s_ui;
static ebike_ride_mode_t s_mode = EBIKE_MODE_NORMAL;
static ebike_mode_changed_cb_t s_mode_callback;
static void *s_mode_callback_user_data;
typedef enum {
    TEMP_VIEW_MOTOR = 0,
    TEMP_VIEW_FET,
} temperature_view_t;

static temperature_view_t s_temperature_view = TEMP_VIEW_MOTOR;
static float s_motor_temp_c;
static float s_fet_temp_c;
static bool s_motor_temp_valid;
static bool s_fet_temp_valid;

static ebike_trip_view_t s_trip_view = EBIKE_TRIP_LOCAL_1;
static float s_trip_km[EBIKE_TRIP_VIEW_COUNT];
static bool s_odometer_valid;
static ebike_trip_reset_cb_t s_trip_reset_callback;
static void *s_trip_reset_callback_user_data;
static ebike_ota_install_cb_t s_ota_install_callback;
static void *s_ota_install_callback_user_data;

static int32_t clamp_i32(int32_t value, int32_t minimum, int32_t maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static float clamp_float(float value, float minimum, float maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static void format_positive_1dp(char *buffer, size_t buffer_size, float value)
{
    if (value < 0.0f) value = 0.0f;
    int32_t scaled = (int32_t)(value * 10.0f + 0.5f);
    snprintf(buffer, buffer_size, "%ld.%ld", (long)(scaled / 10), (long)(scaled % 10));
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, int16_t x, int16_t y,
                              int16_t width, const lv_font_t *font, lv_color_t color,
                              lv_text_align_t alignment)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, alignment, LV_PART_MAIN);
    return label;
}

static lv_obj_t *create_plain_object(lv_obj_t *parent, int16_t x, int16_t y,
                                     int16_t width, int16_t height)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    return obj;
}

static lv_color_t mode_color(ebike_ride_mode_t mode)
{
    if (mode == EBIKE_MODE_ECO) return COLOR_GREEN;
    if (mode == EBIKE_MODE_SPORT) return COLOR_AMBER;
    return COLOR_CYAN;
}

static lv_obj_t *create_gauge_arc(lv_obj_t *parent, bool reverse)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, 154, 154);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, 11, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 11, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, COLOR_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, COLOR_CYAN, LV_PART_INDICATOR);

    if (reverse) {
        lv_arc_set_bg_angles(arc, 316, 44);
        lv_arc_set_mode(arc, LV_ARC_MODE_REVERSE);
    } else {
        lv_arc_set_bg_angles(arc, 136, 224);
        lv_arc_set_mode(arc, LV_ARC_MODE_NORMAL);
    }
    return arc;
}

static void create_battery_indicator(lv_obj_t *parent)
{
    lv_obj_t *outline = create_plain_object(parent, 8, 12, 22, 11);
    lv_obj_set_style_radius(outline, 3, LV_PART_MAIN);
    lv_obj_set_style_border_width(outline, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(outline, COLOR_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_border_opa(outline, LV_OPA_COVER, LV_PART_MAIN);

    s_ui.battery_fill = create_plain_object(outline, 3, 3, 14, 5);
    lv_obj_set_style_radius(s_ui.battery_fill, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.battery_fill, COLOR_GREEN, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.battery_fill, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *tip = create_plain_object(parent, 31, 15, 3, 5);
    lv_obj_set_style_radius(tip, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(tip, COLOR_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tip, LV_OPA_COVER, LV_PART_MAIN);

    s_ui.battery_percent = create_label(parent, "82%", 38, 7, 40, FONT_GAUGE_VALUE,
                                        COLOR_PRIMARY, LV_TEXT_ALIGN_LEFT);
    s_ui.battery_voltage = create_label(parent, "52.4V", 38, 27, 48, FONT_SMALL,
                                        COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
}

static void create_mode_selector(lv_obj_t *parent)
{
    static const char *names[EBIKE_MODE_COUNT] = {"ECO", "NORM", "SPORT"};
    lv_obj_t *track = create_plain_object(parent, 88, 3, 157, 35);
    lv_obj_set_style_radius(track, 18, LV_PART_MAIN);
    lv_obj_set_style_bg_color(track, COLOR_PILL_OFF, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(track, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(track, COLOR_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_border_opa(track, LV_OPA_COVER, LV_PART_MAIN);

    for (uint32_t i = 0; i < EBIKE_MODE_COUNT; ++i) {
        lv_obj_t *button = create_plain_object(track, (int16_t)(2 + i * 51), 2, 51, 31);
        lv_obj_set_style_radius(button, 16, LV_PART_MAIN);
        s_ui.mode_buttons[i] = button;
        s_ui.mode_labels[i] = create_label(button, names[i], 0, 9, 51, FONT_SMALL,
                                           COLOR_MUTED, LV_TEXT_ALIGN_CENTER);
        lv_obj_clear_flag(s_ui.mode_labels[i], LV_OBJ_FLAG_CLICKABLE);
    }
}

static void create_link_indicator(lv_obj_t *parent)
{
    s_ui.current_value = create_label(parent, "--.- A", 258, 4, 58, FONT_DATA,
                                      COLOR_PRIMARY, LV_TEXT_ALIGN_CENTER);
    s_ui.link_dot = create_plain_object(parent, 258, 28, 7, 7);
    lv_obj_set_style_radius(s_ui.link_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.link_dot, COLOR_RED, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.link_dot, LV_OPA_COVER, LV_PART_MAIN);
    s_ui.link_text = create_label(parent, "NO VESC", 268, 24, 50, FONT_SMALL,
                                  COLOR_RED, LV_TEXT_ALIGN_LEFT);
}

static void refresh_temperature_display(void)
{
    if (s_ui.root == NULL) return;

    bool show_fet = s_temperature_view == TEMP_VIEW_FET;
    if (show_fet && !s_fet_temp_valid && s_motor_temp_valid) show_fet = false;
    if (!show_fet && !s_motor_temp_valid && s_fet_temp_valid) show_fet = true;

    const bool valid = show_fet ? s_fet_temp_valid : s_motor_temp_valid;
    const float temperature = show_fet ? s_fet_temp_c : s_motor_temp_c;
    lv_label_set_text(s_ui.temperature_caption, show_fet ? "FET  >" : "MOTOR  >");

    if (!valid) {
        lv_arc_set_value(s_ui.temperature_arc, 0);
        lv_obj_set_style_arc_color(s_ui.temperature_arc, COLOR_MUTED, LV_PART_INDICATOR);
        lv_obj_set_style_text_color(s_ui.temperature_value, COLOR_MUTED, LV_PART_MAIN);
        lv_obj_set_style_border_color(s_ui.temperature_degree_dot, COLOR_MUTED, LV_PART_MAIN);
        lv_label_set_text(s_ui.temperature_value, "--");
        return;
    }

    const int32_t rounded = (int32_t)(temperature + (temperature >= 0.0f ? 0.5f : -0.5f));
    const int16_t display_temp = (int16_t)clamp_i32(rounded, -99, 199);
    const int16_t arc_temp = (int16_t)clamp_i32(rounded, EBIKE_TEMP_MIN_C, EBIKE_TEMP_MAX_C);
    lv_color_t color = COLOR_CYAN;
    if (temperature >= EBIKE_TEMP_HOT_C) color = COLOR_RED;
    else if (temperature >= EBIKE_TEMP_WARM_C) color = COLOR_AMBER;
    lv_arc_set_value(s_ui.temperature_arc, arc_temp);
    lv_obj_set_style_arc_color(s_ui.temperature_arc, color, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(s_ui.temperature_value, color, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_ui.temperature_degree_dot, color, LV_PART_MAIN);
    lv_label_set_text_fmt(s_ui.temperature_value, "%d", display_temp);
}

static void temperature_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_RELEASED) return;
    s_temperature_view = s_temperature_view == TEMP_VIEW_MOTOR ? TEMP_VIEW_FET : TEMP_VIEW_MOTOR;
    refresh_temperature_display();
}

static void refresh_trip_display(void)
{
    static const char *captions[EBIKE_TRIP_VIEW_COUNT] = {"TRIP 1  >", "TRIP 2  >", "ODO  >"};
    if (s_ui.root == NULL) return;
    lv_label_set_text(s_ui.trip_caption, captions[s_trip_view]);
    if (s_trip_view == EBIKE_TRIP_ODOMETER && !s_odometer_valid) {
        lv_label_set_text(s_ui.trip_value, "-- km");
        return;
    }
    char value[16];
    format_positive_1dp(value, sizeof(value), s_trip_km[s_trip_view]);
    lv_label_set_text_fmt(s_ui.trip_value, "%s km", value);
}

static void create_center_status(lv_obj_t *parent)
{
    lv_obj_t *pill = create_plain_object(parent, 112, 137, 96, 28);
    lv_obj_set_style_radius(pill, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(pill, COLOR_PILL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(pill, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(pill, COLOR_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_border_opa(pill, LV_OPA_COVER, LV_PART_MAIN);

    s_ui.pas_value = create_label(pill, "P3", 7, 7, 26, FONT_SMALL,
                                  COLOR_CYAN, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *divider = create_plain_object(pill, 36, 7, 1, 14);
    lv_obj_set_style_bg_color(divider, COLOR_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    s_ui.efficiency_value = create_label(pill, "24 W/km", 41, 7, 50, FONT_SMALL,
                                         COLOR_SECONDARY, LV_TEXT_ALIGN_CENTER);
}

static void create_bottom_data(lv_obj_t *parent)
{
    lv_obj_t *divider = create_plain_object(parent, 16, 190, 288, 1);
    lv_obj_set_style_bg_color(divider, COLOR_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);

    s_ui.trip_caption = create_label(parent, "TRIP 1  >", 10, 196, 90, FONT_SMALL,
                                     COLOR_MUTED, LV_TEXT_ALIGN_CENTER);
    create_label(parent, "ACCEL  m/s2", 115, 196, 90, FONT_SMALL, COLOR_MUTED, LV_TEXT_ALIGN_CENTER);
    create_label(parent, "RANGE", 220, 196, 90, FONT_SMALL, COLOR_MUTED, LV_TEXT_ALIGN_CENTER);

    s_ui.trip_value = create_label(parent, "18.6 km", 10, 214, 90, FONT_DATA,
                                   COLOR_PRIMARY, LV_TEXT_ALIGN_CENTER);
    s_ui.acceleration_value = create_label(parent, "+0.00", 115, 212, 90, FONT_GAUGE_VALUE,
                                           COLOR_CYAN, LV_TEXT_ALIGN_CENTER);
    s_ui.range_value = create_label(parent, "31 km", 220, 214, 90, FONT_DATA,
                                    COLOR_PRIMARY, LV_TEXT_ALIGN_CENTER);

    lv_obj_t *accel_track = create_plain_object(parent, 126, 235, 68, 3);
    lv_obj_set_style_radius(accel_track, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(accel_track, COLOR_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(accel_track, LV_OPA_COVER, LV_PART_MAIN);
    s_ui.acceleration_fill = create_plain_object(parent, 160, 235, 1, 3);
    lv_obj_set_style_radius(s_ui.acceleration_fill, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.acceleration_fill, COLOR_CYAN, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.acceleration_fill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_t *zero = create_plain_object(parent, 159, 233, 2, 7);
    lv_obj_set_style_bg_color(zero, COLOR_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(zero, LV_OPA_COVER, LV_PART_MAIN);

}

static void ota_install_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_RELEASED) return;
    lv_obj_add_flag(s_ui.ota_install_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ui.ota_later_button, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_ui.ota_title, "STARTING UPDATE");
    lv_label_set_text(s_ui.ota_detail, "Checking that the bicycle is stopped...");
    if (s_ota_install_callback != NULL) {
        s_ota_install_callback(s_ota_install_callback_user_data);
    }
}

static void ota_later_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_RELEASED) return;
    ebike_ui_hide_ota();
}

static lv_obj_t *create_ota_button(lv_obj_t *parent, int16_t x, int16_t width,
                                   const char *text, lv_color_t background,
                                   lv_event_cb_t callback)
{
    lv_obj_t *button = create_plain_object(parent, x, 182, width, 38);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(button, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, background, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, COLOR_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_border_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_t *label = create_label(button, text, 0, 11, width, FONT_DATA,
                                   COLOR_PRIMARY, LV_TEXT_ALIGN_CENTER);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(button, callback, LV_EVENT_RELEASED, NULL);
    return button;
}

static void create_ota_overlay(lv_obj_t *parent)
{
    s_ui.ota_overlay = create_plain_object(parent, 0, 0, EBIKE_SCREEN_WIDTH,
                                           EBIKE_SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(s_ui.ota_overlay, COLOR_BG_TOP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.ota_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(s_ui.ota_overlay, LV_OBJ_FLAG_HIDDEN);

    s_ui.ota_title = create_label(s_ui.ota_overlay, "UPDATE AVAILABLE", 18, 25, 284,
                                  FONT_GAUGE_VALUE, COLOR_CYAN, LV_TEXT_ALIGN_CENTER);
    s_ui.ota_detail = create_label(s_ui.ota_overlay, "", 24, 66, 272, FONT_DATA,
                                   COLOR_SECONDARY, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(s_ui.ota_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(s_ui.ota_detail, 92);

    s_ui.ota_progress = lv_bar_create(s_ui.ota_overlay);
    lv_obj_set_pos(s_ui.ota_progress, 30, 145);
    lv_obj_set_size(s_ui.ota_progress, 260, 16);
    lv_bar_set_range(s_ui.ota_progress, 0, 100);
    lv_bar_set_value(s_ui.ota_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_ui.ota_progress, COLOR_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.ota_progress, COLOR_CYAN, LV_PART_INDICATOR);
    lv_obj_add_flag(s_ui.ota_progress, LV_OBJ_FLAG_HIDDEN);
    s_ui.ota_progress_text = create_label(s_ui.ota_overlay, "0%", 130, 164, 60,
                                          FONT_SMALL, COLOR_MUTED, LV_TEXT_ALIGN_CENTER);
    lv_obj_add_flag(s_ui.ota_progress_text, LV_OBJ_FLAG_HIDDEN);

    s_ui.ota_install_button = create_ota_button(s_ui.ota_overlay, 30, 160, "INSTALL",
                                                COLOR_PILL, ota_install_event_cb);
    s_ui.ota_later_button = create_ota_button(s_ui.ota_overlay, 200, 90, "LATER",
                                              COLOR_PILL_OFF, ota_later_event_cb);
}

void ebike_ui_create(lv_obj_t *parent)
{
    if (s_ui.root != NULL) ebike_ui_destroy();
    if (parent == NULL) parent = lv_scr_act();

    s_ui.root = create_plain_object(parent, 0, 0, EBIKE_SCREEN_WIDTH, EBIKE_SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(s_ui.root, COLOR_BG_TOP, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(s_ui.root, COLOR_BG_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(s_ui.root, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.root, LV_OPA_COVER, LV_PART_MAIN);

    create_battery_indicator(s_ui.root);
    create_mode_selector(s_ui.root);
    create_link_indicator(s_ui.root);

    s_ui.temperature_arc = create_gauge_arc(s_ui.root, false);
    lv_obj_set_pos(s_ui.temperature_arc, 31, 37);
    lv_arc_set_range(s_ui.temperature_arc, EBIKE_TEMP_MIN_C, EBIKE_TEMP_MAX_C);
    s_ui.power_arc = create_gauge_arc(s_ui.root, true);
    lv_obj_set_pos(s_ui.power_arc, 135, 37);
    lv_arc_set_range(s_ui.power_arc, EBIKE_POWER_MIN_W, EBIKE_POWER_MAX_W);

    s_ui.temperature_value = create_label(s_ui.root, "55", 36, 91, 68, FONT_GAUGE_VALUE,
                                          COLOR_AMBER, LV_TEXT_ALIGN_CENTER);
    s_ui.temperature_degree_dot = create_plain_object(s_ui.root, 90, 94, 5, 5);
    lv_obj_set_style_radius(s_ui.temperature_degree_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.temperature_degree_dot, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_ui.temperature_degree_dot, COLOR_AMBER, LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_ui.temperature_degree_dot, LV_OPA_COVER, LV_PART_MAIN);
    s_ui.temperature_caption = create_label(s_ui.root, "MOTOR  >", 37, 116, 66, FONT_SMALL,
                                            COLOR_MUTED, LV_TEXT_ALIGN_CENTER);

    s_ui.power_value = create_label(s_ui.root, "1000", 216, 91, 70, FONT_GAUGE_VALUE,
                                    COLOR_CYAN, LV_TEXT_ALIGN_CENTER);
    create_label(s_ui.root, "POWER W", 216, 116, 70, FONT_SMALL, COLOR_MUTED, LV_TEXT_ALIGN_CENTER);

    /* Integer speed makes the 48 px font visibly larger and readable at a glance. */
    s_ui.speed_value = create_label(s_ui.root, "36", 103, 45, 114, FONT_SPEED,
                                    COLOR_PRIMARY, LV_TEXT_ALIGN_CENTER);
    create_label(s_ui.root, "km/h", 124, 103, 72, FONT_UNIT, COLOR_SECONDARY, LV_TEXT_ALIGN_CENTER);
    create_center_status(s_ui.root);
    create_bottom_data(s_ui.root);
    create_ota_overlay(s_ui.root);

    lv_obj_t *temperature_touch = create_plain_object(s_ui.root, 20, 55, 88, 112);
    lv_obj_add_flag(temperature_touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(temperature_touch, temperature_event_cb, LV_EVENT_RELEASED, NULL);

    ebike_ui_set_mode(s_mode);

    const ebike_ui_data_t initial = {
        .speed_kph = 0.0f,
        .efficiency_w_per_km = -1.0f,
        .motor_temp_c = 0.0f,
        .fet_temp_c = 0.0f,
        .motor_temp_valid = false,
        .fet_temp_valid = false,
        .power_w = 0,
        .input_current_a = 0.0f,
        .battery_percent = 0,
        .battery_voltage_v = 0.0f,
        .pas_level = 3,
        .controller_linked = false,
        .local_trip_1_km = 0.0f,
        .local_trip_2_km = 0.0f,
        .odometer_km = 0.0f,
        .odometer_valid = false,
        .ride_time_s = 0U,
        .estimated_range_km = -1.0f,
    };
    ebike_ui_update(&initial);
    ebike_ui_set_acceleration_from_sensor(0.0f);
}

void ebike_ui_destroy(void)
{
    if (s_ui.root != NULL) lv_obj_del(s_ui.root);
    s_ui = (ebike_ui_objects_t){0};
}

lv_obj_t *ebike_ui_get_root(void)
{
    return s_ui.root;
}

void ebike_ui_update(const ebike_ui_data_t *data)
{
    if (data == NULL || s_ui.root == NULL) return;
    ebike_ui_set_speed(data->speed_kph);
    ebike_ui_set_efficiency(data->efficiency_w_per_km);
    ebike_ui_set_temperatures(data->motor_temp_c, data->motor_temp_valid,
                              data->fet_temp_c, data->fet_temp_valid);
    ebike_ui_set_power(data->power_w);
    ebike_ui_set_input_current(data->input_current_a);
    ebike_ui_set_battery(data->battery_percent, data->battery_voltage_v);
    ebike_ui_set_pas(data->pas_level);
    ebike_ui_set_controller_link(data->controller_linked);
    ebike_ui_set_trip_distances(data->local_trip_1_km, data->local_trip_2_km,
                                data->odometer_km, data->odometer_valid);
    ebike_ui_set_ride_time(data->ride_time_s);
    ebike_ui_set_range(data->estimated_range_km);
}

void ebike_ui_set_speed(float speed_kph)
{
    if (s_ui.root == NULL) return;
    speed_kph = clamp_float(speed_kph, 0.0f, 199.0f);
    lv_label_set_text_fmt(s_ui.speed_value, "%d", (int)(speed_kph + 0.5f));
}

void ebike_ui_set_efficiency(float w_per_km)
{
    if (s_ui.root == NULL) return;
    if (w_per_km < 0.0f) {
        lv_label_set_text(s_ui.efficiency_value, "-- W/km");
    } else if (w_per_km < 100.0f) {
        lv_label_set_text_fmt(s_ui.efficiency_value, "%d W/km", (int)(w_per_km + 0.5f));
    } else {
        lv_label_set_text(s_ui.efficiency_value, "99+ W/km");
    }
}

void ebike_ui_set_motor_temperature(int16_t temp_c)
{
    ebike_ui_set_temperatures((float)temp_c, true, s_fet_temp_c, s_fet_temp_valid);
}

void ebike_ui_set_temperatures(float motor_temp_c, bool motor_valid,
                               float fet_temp_c, bool fet_valid)
{
    s_motor_temp_c = motor_temp_c;
    s_fet_temp_c = fet_temp_c;
    s_motor_temp_valid = motor_valid;
    s_fet_temp_valid = fet_valid;
    refresh_temperature_display();
}

void ebike_ui_set_power(int16_t power_w)
{
    if (s_ui.root == NULL) return;
    int16_t display_power = (int16_t)clamp_i32(power_w, -999, 9999);
    int16_t arc_power = (int16_t)clamp_i32(power_w, EBIKE_POWER_MIN_W, EBIKE_POWER_MAX_W);
    lv_color_t color = COLOR_CYAN;
    if (power_w >= EBIKE_POWER_HIGH_W) color = COLOR_RED;
    else if (power_w >= EBIKE_POWER_MEDIUM_W) color = COLOR_AMBER;
    else if (power_w < 0) color = COLOR_GREEN;
    lv_arc_set_value(s_ui.power_arc, arc_power);
    lv_obj_set_style_arc_color(s_ui.power_arc, color, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(s_ui.power_value, color, LV_PART_MAIN);
    lv_label_set_text_fmt(s_ui.power_value, "%d", display_power);
}

void ebike_ui_set_input_current(float current_a)
{
    if (s_ui.root == NULL) return;
    current_a = clamp_float(current_a, -999.9f, 999.9f);
    int32_t scaled = (int32_t)(current_a * 10.0f + (current_a >= 0.0f ? 0.5f : -0.5f));
    int32_t absolute = scaled < 0 ? -scaled : scaled;
    lv_label_set_text_fmt(s_ui.current_value, "%s%ld.%ld A", scaled < 0 ? "-" : "",
                          (long)(absolute / 10), (long)(absolute % 10));
    lv_obj_set_style_text_color(s_ui.current_value,
                                scaled < 0 ? COLOR_GREEN : COLOR_PRIMARY, LV_PART_MAIN);
}

void ebike_ui_set_battery(uint8_t percent, float voltage_v)
{
    if (s_ui.root == NULL) return;
    percent = (uint8_t)clamp_i32(percent, 0, 100);
    int16_t fill_width = (int16_t)((percent * 14U + 50U) / 100U);
    lv_color_t color = percent <= 10U ? COLOR_RED : (percent <= 20U ? COLOR_AMBER : COLOR_GREEN);
    lv_obj_set_width(s_ui.battery_fill, fill_width > 0 ? fill_width : 1);
    lv_obj_set_style_bg_opa(s_ui.battery_fill, percent > 0 ? LV_OPA_COVER : LV_OPA_TRANSP,
                            LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.battery_fill, color, LV_PART_MAIN);
    lv_label_set_text_fmt(s_ui.battery_percent, "%u%%", (unsigned)percent);
    char voltage[16];
    format_positive_1dp(voltage, sizeof(voltage), voltage_v);
    lv_label_set_text_fmt(s_ui.battery_voltage, "%sV", voltage);
}

void ebike_ui_set_pas(uint8_t level)
{
    if (s_ui.root == NULL) return;
    lv_label_set_text_fmt(s_ui.pas_value, "P%u", (unsigned)level);
}

void ebike_ui_set_controller_link(bool linked)
{
    if (s_ui.root == NULL) return;
    lv_obj_set_style_bg_color(s_ui.link_dot, linked ? COLOR_GREEN : COLOR_RED, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.link_text, linked ? COLOR_MUTED : COLOR_RED, LV_PART_MAIN);
    lv_label_set_text(s_ui.link_text, linked ? "VESC" : "NO VESC");
    if (!linked) {
        lv_label_set_text(s_ui.current_value, "--.- A");
        lv_obj_set_style_text_color(s_ui.current_value, COLOR_MUTED, LV_PART_MAIN);
    }
}

void ebike_ui_set_trip(float trip_km)
{
    s_trip_km[EBIKE_TRIP_LOCAL_1] = trip_km;
    refresh_trip_display();
}

void ebike_ui_set_trip_distances(float local_1_km, float local_2_km,
                                 float odometer_km, bool odometer_valid)
{
    s_trip_km[EBIKE_TRIP_LOCAL_1] = local_1_km;
    s_trip_km[EBIKE_TRIP_LOCAL_2] = local_2_km;
    s_trip_km[EBIKE_TRIP_ODOMETER] = odometer_km;
    s_odometer_valid = odometer_valid;
    refresh_trip_display();
}

ebike_trip_view_t ebike_ui_get_trip_view(void)
{
    return s_trip_view;
}

void ebike_ui_next_trip_view(void)
{
    s_trip_view = (ebike_trip_view_t)(((uint32_t)s_trip_view + 1U) %
                                      EBIKE_TRIP_VIEW_COUNT);
    refresh_trip_display();
}

void ebike_ui_set_trip_reset_callback(ebike_trip_reset_cb_t callback, void *user_data)
{
    s_trip_reset_callback = callback;
    s_trip_reset_callback_user_data = user_data;
}

void ebike_ui_set_ride_time(uint32_t seconds)
{
    (void)seconds; /* Kept for source compatibility; the polished layout prioritizes acceleration. */
}

void ebike_ui_set_range(float range_km)
{
    if (s_ui.root == NULL) return;
    if (range_km < 0.0f) lv_label_set_text(s_ui.range_value, "-- km");
    else lv_label_set_text_fmt(s_ui.range_value, "%d km", (int)(range_km + 0.5f));
}

void ebike_ui_set_mode(ebike_ride_mode_t mode)
{
    if (mode < EBIKE_MODE_ECO || mode >= EBIKE_MODE_COUNT) return;
    bool changed = mode != s_mode;
    s_mode = mode;
    if (s_ui.root != NULL) {
        for (uint32_t i = 0; i < EBIKE_MODE_COUNT; ++i) {
            bool active = i == (uint32_t)mode;
            lv_obj_set_style_bg_opa(s_ui.mode_buttons[i], active ? LV_OPA_COVER : LV_OPA_TRANSP,
                                    LV_PART_MAIN);
            lv_obj_set_style_bg_color(s_ui.mode_buttons[i], mode_color((ebike_ride_mode_t)i),
                                      LV_PART_MAIN);
            lv_obj_set_style_text_color(s_ui.mode_labels[i], active ? COLOR_BG_BOTTOM : COLOR_MUTED,
                                        LV_PART_MAIN);
        }
        lv_obj_set_style_text_color(s_ui.pas_value, mode_color(mode), LV_PART_MAIN);
    }
    if (changed && s_mode_callback != NULL) s_mode_callback(mode, s_mode_callback_user_data);
}

ebike_ride_mode_t ebike_ui_get_mode(void)
{
    return s_mode;
}

void ebike_ui_set_mode_change_callback(ebike_mode_changed_cb_t callback, void *user_data)
{
    s_mode_callback = callback;
    s_mode_callback_user_data = user_data;
}

void ebike_ui_next_mode(void)
{
    ebike_ui_set_mode((ebike_ride_mode_t)(((uint32_t)s_mode + 1U) %
                                          EBIKE_MODE_COUNT));
}

static void set_acceleration_display(float acceleration_mps2)
{
    if (s_ui.root == NULL) return;
    acceleration_mps2 = clamp_float(acceleration_mps2, -9.9f, 9.9f);
    lv_color_t color = acceleration_mps2 < -0.12f ? COLOR_AMBER :
                       (acceleration_mps2 > 0.12f ? COLOR_CYAN : COLOR_SECONDARY);
    int32_t scaled = (int32_t)(acceleration_mps2 * 100.0f +
                               (acceleration_mps2 >= 0.0f ? 0.5f : -0.5f));
    int32_t absolute = scaled < 0 ? -scaled : scaled;
    lv_label_set_text_fmt(s_ui.acceleration_value, "%c%ld.%02ld",
                          scaled < 0 ? '-' : '+', (long)(absolute / 100),
                          (long)(absolute % 100));
    lv_obj_set_style_text_color(s_ui.acceleration_value, color, LV_PART_MAIN);
    int16_t pixels = (int16_t)(clamp_float(fabsf(acceleration_mps2) / 5.0f, 0.0f, 1.0f) * 34.0f + 0.5f);
    if (pixels < 1) pixels = 1;
    lv_obj_set_width(s_ui.acceleration_fill, pixels);
    lv_obj_set_x(s_ui.acceleration_fill, acceleration_mps2 < 0.0f ? 160 - pixels : 160);
    lv_obj_set_style_bg_color(s_ui.acceleration_fill, color, LV_PART_MAIN);
}

void ebike_ui_set_acceleration_from_sensor(float acceleration_mps2)
{
    set_acceleration_display(acceleration_mps2);
}

void ebike_ui_set_ota_install_callback(ebike_ota_install_cb_t callback, void *user_data)
{
    s_ota_install_callback = callback;
    s_ota_install_callback_user_data = user_data;
}

void ebike_ui_show_ota_available(const char *installed_version, const char *available_version,
                                 const char *release_notes)
{
    if (s_ui.ota_overlay == NULL) return;
    char detail[256];
    snprintf(detail, sizeof(detail), "Installed: %s\nAvailable: %s%s%s",
             installed_version != NULL ? installed_version : "?",
             available_version != NULL ? available_version : "?",
             release_notes != NULL && release_notes[0] != '\0' ? "\n\n" : "",
             release_notes != NULL ? release_notes : "");
    lv_label_set_text(s_ui.ota_title, "UPDATE AVAILABLE");
    lv_obj_set_style_text_color(s_ui.ota_title, COLOR_CYAN, LV_PART_MAIN);
    lv_label_set_text(s_ui.ota_detail, detail);
    lv_obj_add_flag(s_ui.ota_progress, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ui.ota_progress_text, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ui.ota_install_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ui.ota_later_button, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(lv_obj_get_child(s_ui.ota_later_button, 0), "LATER");
    lv_obj_clear_flag(s_ui.ota_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui.ota_overlay);
}

void ebike_ui_show_ota_progress(uint8_t percent, const char *message)
{
    if (s_ui.ota_overlay == NULL) return;
    if (percent > 100U) percent = 100U;
    lv_label_set_text(s_ui.ota_title, percent >= 100U ? "VERIFYING UPDATE" : "UPDATING");
    lv_obj_set_style_text_color(s_ui.ota_title, COLOR_CYAN, LV_PART_MAIN);
    lv_label_set_text(s_ui.ota_detail, message != NULL ? message : "Downloading firmware");
    lv_bar_set_value(s_ui.ota_progress, percent, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_ui.ota_progress_text, "%u%%", (unsigned)percent);
    lv_obj_clear_flag(s_ui.ota_progress, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ui.ota_progress_text, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ui.ota_install_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ui.ota_later_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ui.ota_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui.ota_overlay);
}

void ebike_ui_show_ota_message(const char *title, const char *message, bool dismissable)
{
    if (s_ui.ota_overlay == NULL) return;
    lv_label_set_text(s_ui.ota_title, title != NULL ? title : "UPDATE");
    lv_obj_set_style_text_color(s_ui.ota_title,
                                dismissable ? COLOR_AMBER : COLOR_CYAN, LV_PART_MAIN);
    lv_label_set_text(s_ui.ota_detail, message != NULL ? message : "");
    lv_obj_add_flag(s_ui.ota_progress, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ui.ota_progress_text, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ui.ota_install_button, LV_OBJ_FLAG_HIDDEN);
    if (dismissable) {
        lv_obj_clear_flag(s_ui.ota_later_button, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lv_obj_get_child(s_ui.ota_later_button, 0), "CLOSE");
    } else {
        lv_obj_add_flag(s_ui.ota_later_button, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(s_ui.ota_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui.ota_overlay);
}

void ebike_ui_hide_ota(void)
{
    if (s_ui.ota_overlay != NULL) lv_obj_add_flag(s_ui.ota_overlay, LV_OBJ_FLAG_HIDDEN);
}

bool ebike_ui_ota_overlay_active(void)
{
    return s_ui.ota_overlay != NULL &&
           !lv_obj_has_flag(s_ui.ota_overlay, LV_OBJ_FLAG_HIDDEN);
}
