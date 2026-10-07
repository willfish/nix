#include "pill.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <string.h>

static const double PI = 3.14159265358979323846;

static void rounded(cairo_t *cr, double x, double y, double width, double height, double radius) {
    double limit = width < height ? width : height;
    if (limit < 0) limit = 0;
    if (radius > limit / 2.0) radius = limit / 2.0;
    if (radius < 0) radius = 0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + width - radius, y + radius, radius, -PI / 2.0, 0);
    cairo_arc(cr, x + width - radius, y + height - radius, radius, 0, PI / 2.0);
    cairo_arc(cr, x + radius, y + height - radius, radius, PI / 2.0, PI);
    cairo_arc(cr, x + radius, y + radius, radius, PI, PI + PI / 2.0);
    cairo_close_path(cr);
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void rgb_of(const char *value, double *r, double *g, double *b) {
    char hex[7];
    int comps[3];
    *r = *g = *b = 0.7;
    if (!value) return;
    while (*value == '#') value++;
    if (strlen(value) == 3) {
        hex[0] = value[0];
        hex[1] = value[0];
        hex[2] = value[1];
        hex[3] = value[1];
        hex[4] = value[2];
        hex[5] = value[2];
        hex[6] = 0;
    } else if (strlen(value) >= 6) {
        memcpy(hex, value, 6);
        hex[6] = 0;
    } else {
        return;
    }
    for (int i = 0; i < 3; i++) {
        int hi = hex_value(hex[i * 2]);
        int lo = hex_value(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return;
        comps[i] = hi * 16 + lo;
    }
    *r = comps[0] / 255.0;
    *g = comps[1] / 255.0;
    *b = comps[2] / 255.0;
}

static const char *tone_hex(const VoiceColours *colours, const char *tone) {
    if (!colours || !tone) return "#9c9ca5";
    if (strcmp(tone, "background") == 0) return colours->background;
    if (strcmp(tone, "text") == 0) return colours->text;
    if (strcmp(tone, "border") == 0) return colours->border;
    if (strcmp(tone, "accent") == 0) return colours->accent;
    if (strcmp(tone, "muted") == 0) return colours->muted;
    if (strcmp(tone, "red") == 0) return colours->red;
    if (strcmp(tone, "yellow") == 0) return colours->yellow;
    if (strcmp(tone, "green") == 0) return colours->green;
    if (strcmp(tone, "orange") == 0) return colours->orange;
    if (strcmp(tone, "teal") == 0) return colours->teal;
    return "#9c9ca5";
}

static void show_text(cairo_t *cr, const char *value, double tx, double ty, double max_width,
    int size, int bold, double r, double g, double b) {
    PangoLayout *layout;
    PangoFontDescription *font;
    char desc[64];
    int width;
    if (!value) value = "";
    layout = pango_cairo_create_layout(cr);
    snprintf(desc, sizeof desc, "Sans %s%d", bold ? "Bold " : "", size);
    font = pango_font_description_from_string(desc);
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    pango_layout_set_text(layout, value, -1);
    width = (int)max_width;
    if (width < 1) width = 1;
    pango_layout_set_width(layout, width * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_single_paragraph_mode(layout, TRUE);
    cairo_move_to(cr, tx, ty);
    cairo_set_source_rgb(cr, r, g, b);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

void pill_paint(cairo_t *cr, const PillView *view, const PillMotion *motion,
    const VoiceColours *colours, double now, int reduced) {
    double w;
    double h;
    double x;
    double y;
    double tone_r;
    double tone_g;
    double tone_b;
    double opacity;
    double cy;
    const double spreads[] = {7, 4, 2};
    const double alphas[] = {0.025, 0.04, 0.07};
    if (!cr || !view || !motion) return;
    w = motion->width;
    h = motion->height;
    x = (PILL_SURFACE_WIDTH - w) / 2.0;
    y = 12.0 + (1.0 - motion->opacity) * -6.0;
    rgb_of(tone_hex(colours, view->tone), &tone_r, &tone_g, &tone_b);
    cairo_save(cr);
    cairo_push_group(cr);
    for (int i = 0; i < 3; i++) {
        double spread = spreads[i];
        rounded(cr, x - spread, y + 3.0 - spread, w + spread * 2.0, h + spread * 2.0, h / 2.0 + spread);
        cairo_set_source_rgba(cr, 0, 0, 0, alphas[i]);
        cairo_fill(cr);
    }
    rounded(cr, x, y, w, h, h / 2.0 < 26.0 ? h / 2.0 : 26.0);
    cairo_set_source_rgba(cr, 0.047, 0.051, 0.063, 0.97);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.13);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    cairo_save(cr);
    rounded(cr, x + 1, y + 1, w - 2, h - 2, h / 2.0 < 25.0 ? h / 2.0 : 25.0);
    cairo_clip(cr);
    cy = y + 22.0;
    cairo_set_source_rgb(cr, tone_r, tone_g, tone_b);
    if (view->working) {
        double angle = reduced ? 0 : now * 4.0;
        cairo_set_line_width(cr, 2);
        cairo_arc(cr, x + 22.0, cy, 5, angle, angle + PI * 1.45);
        cairo_stroke(cr);
    } else if (view->warning || ((strcmp(view->tone, "orange") == 0 || strcmp(view->tone, "red") == 0) && !view->recording)) {
        show_text(cr, "!", x + 18.0, cy - 10.0, 12, 12, 1, tone_r, tone_g, tone_b);
    } else if (strcmp(view->tone, "green") == 0) {
        cairo_set_line_width(cr, 2);
        cairo_move_to(cr, x + 17.0, cy);
        cairo_line_to(cr, x + 21.0, cy + 4.0);
        cairo_line_to(cr, x + 28.0, cy - 4.0);
        cairo_stroke(cr);
    } else {
        cairo_arc(cr, x + 22.0, cy, view->recording ? 4 : 3, 0, PI * 2.0);
        cairo_fill(cr);
    }
    if (view->recording && !view->warning) {
        double bar_x = x + 43.0;
        for (int index = 0; index < PILL_BARS; index++) {
            double level = motion->levels[index];
            double bar_h = 2.0 + level * 17.0;
            rounded(cr, bar_x + index * 4.5, cy - bar_h / 2.0, 2.5, bar_h, 1.25);
            cairo_set_source_rgba(cr, 0.95, 0.95, 0.97, 0.4 + 0.6 * level);
            cairo_fill(cr);
        }
        show_text(cr, view->timer, x + w - 63.0, cy - 9.0, 53, 10, 1, 0.95, 0.95, 0.97);
    } else {
        show_text(cr, view->title, x + 39.0, cy - 10.0, w - 58.0, 11, 1, 0.95, 0.95, 0.97);
    }
    if (view->expanded) {
        show_text(cr, view->detail, x + 22.0, y + 45.0, w - 44.0, 9, 0, 0.66, 0.68, 0.73);
    }
    cairo_restore(cr);
    cairo_pop_group_to_source(cr);
    opacity = motion->opacity;
    if (opacity < 0) opacity = 0;
    if (opacity > 1) opacity = 1;
    cairo_paint_with_alpha(cr, opacity);
    cairo_restore(cr);
}
