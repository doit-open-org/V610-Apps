#define _POSIX_C_SOURCE 200809L

#include "keypoints.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * A .keypoints file is small and written by hand, so it is parsed strictly: a
 * line that is not understood stops the load and says which line it was,
 * rather than leaving a half-built layout to draw nonsense later.
 */

#define LAYOUT_LINE_MAX 256U
/* One colour for every point, until a file says otherwise. */
#define LAYOUT_POINT_RED 0x40U
#define LAYOUT_POINT_GREEN 0xE0U
#define LAYOUT_POINT_BLUE 0x60U
/* Dot and line sizes in pixels of the frame the model sees. */
#define LAYOUT_POINT_SIZE 6U
#define LAYOUT_BONE_WIDTH 2U

static void say(char *error, size_t error_size, unsigned long line,
    const char *detail)
{
    if (error == NULL || error_size == 0) return;
    (void)snprintf(error, error_size, "line %lu: %s", line, detail);
}

/* Cuts the comment and the trailing newline, leaving a bare NUL-terminated line. */
static void trim(char *text)
{
    char *comment = strchr(text, '#');
    if (comment != NULL) *comment = '\0';
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1U] == '\n' || text[length - 1U] == '\r' ||
        text[length - 1U] == ' ' || text[length - 1U] == '\t')) {
        text[--length] = '\0';
    }
}

/* Reads one non-negative integer, refusing anything that is not exactly that. */
static bool read_index(const char *text, unsigned long limit,
    unsigned long *value)
{
    if (text == NULL || text[0] == '\0' || text[0] == '-') return false;
    errno = 0;
    char *end = NULL;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || end[0] != '\0' || parsed >= limit) {
        return false;
    }
    *value = parsed;
    return true;
}

/*
 * A colour as six hex digits, RRGGBB.  Bare: a leading # would have been cut
 * as a comment long before this sees the line.
 */
static bool read_colour(const char *text, uint8_t *red, uint8_t *green,
    uint8_t *blue)
{
    if (text == NULL || strlen(text) != 6U) return false;
    unsigned long channel[3] = {0};
    for (size_t index = 0; index < 3U; ++index) {
        for (size_t digit = 0; digit < 2U; ++digit) {
            const char c = text[index * 2U + digit];
            unsigned long value;
            if (c >= '0' && c <= '9') value = (unsigned long)(c - '0');
            else if (c >= 'a' && c <= 'f') value = (unsigned long)(c - 'a') + 10UL;
            else if (c >= 'A' && c <= 'F') value = (unsigned long)(c - 'A') + 10UL;
            else return false;
            channel[index] = channel[index] * 16UL + value;
        }
    }
    *red = (uint8_t)channel[0];
    *green = (uint8_t)channel[1];
    *blue = (uint8_t)channel[2];
    return true;
}

/* A confidence, which has to be a plain fraction of one. */
static bool read_visibility(const char *text, float *value)
{
    if (text == NULL || text[0] == '\0') return false;
    errno = 0;
    char *end = NULL;
    const float parsed = strtof(text, &end);
    if (errno != 0 || end == text || end[0] != '\0' || !(parsed >= 0.0F) ||
        parsed > 1.0F) {
        return false;
    }
    *value = parsed;
    return true;
}

/* A dot or line size in model-frame pixels; 0 means "do not draw". */
static bool read_size(const char *text, uint8_t *value)
{
    unsigned long parsed = 0;
    if (!read_index(text, 256UL, &parsed)) return false;
    *value = (uint8_t)parsed;
    return true;
}

static bool copy_word(char *target, size_t target_size, const char *word)
{
    const size_t length = strlen(word);
    if (length == 0 || length >= target_size) return false;
    memcpy(target, word, length + 1U);
    return true;
}

/*
 * What a group hands its points and bones, and what either may say for
 * itself.  Carrying concrete values rather than "was it given" flags is what
 * keeps the three levels -- built in, group, element -- to one line each.
 */
typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t point_size;
    uint8_t bone_width;
    float min_visibility;
} layout_style;

#define SETTING_COLOR 0x1U
#define SETTING_SIZE 0x2U
#define SETTING_WIDTH 0x4U
#define SETTING_MIN 0x8U

static bool key_is(const char *text, size_t length, const char *key)
{
    return strlen(key) == length && memcmp(text, key, length) == 0;
}

/* One key=value, refused unless this kind of line has that setting. */
static bool read_setting(const char *token, unsigned allowed,
    layout_style *style)
{
    const char *equals = strchr(token, '=');
    if (equals == NULL || equals == token) return false;
    const size_t length = (size_t)(equals - token);
    const char *value = equals + 1U;
    if ((allowed & SETTING_COLOR) != 0U && key_is(token, length, "color")) {
        return read_colour(value, &style->red, &style->green, &style->blue);
    }
    if ((allowed & SETTING_SIZE) != 0U && key_is(token, length, "size")) {
        return read_size(value, &style->point_size);
    }
    if ((allowed & SETTING_WIDTH) != 0U && key_is(token, length, "width")) {
        return read_size(value, &style->bone_width);
    }
    if ((allowed & SETTING_MIN) != 0U && key_is(token, length, "min")) {
        return read_visibility(value, &style->min_visibility);
    }
    return false;
}

/* Every remaining word on the line, each of which must be a setting. */
static bool read_settings(char **save, unsigned allowed,
    layout_style *style)
{
    for (const char *token = strtok_r(NULL, " \t", save); token != NULL;
        token = strtok_r(NULL, " \t", save)) {
        if (!read_setting(token, allowed, style)) return false;
    }
    return true;
}

void keypoint_layout_default(keypoint_layout *layout, float visibility)
{
    if (layout == NULL) return;
    memset(layout, 0, sizeof(*layout));
    (void)copy_word(layout->name, sizeof(layout->name), "subject");
    for (size_t index = 0; index < KEYPOINT_MAX_POINTS; ++index) {
        layout->points[index] = (layout_point) {
            .declared = true,
            .min_visibility = visibility,
            .red = LAYOUT_POINT_RED,
            .green = LAYOUT_POINT_GREEN,
            .blue = LAYOUT_POINT_BLUE,
            .size = LAYOUT_POINT_SIZE,
        };
    }
}

/*
 * A bone may name an end that the file declares further down, so the ends are
 * kept as written and resolved once the whole file has been read.
 */
typedef struct {
    char from[KEYPOINT_NAME_MAX];
    char to[KEYPOINT_NAME_MAX];
    unsigned long line;
} pending_bone;

static bool resolve_end(const keypoint_layout *layout, const char *word,
    unsigned long limit, uint8_t *keypoint)
{
    unsigned long index = 0;
    if (read_index(word, limit, &index)) {
        *keypoint = (uint8_t)index;
        return layout->points[index].declared;
    }
    for (size_t point = 0; point < KEYPOINT_MAX_POINTS; ++point) {
        if (layout->points[point].declared &&
            strcmp(layout->points[point].name, word) == 0) {
            *keypoint = (uint8_t)point;
            return true;
        }
    }
    return false;
}

bool keypoint_layout_load(const char *path, keypoint_layout *layout, float visibility,
    char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (path == NULL || layout == NULL) {
        say(error, error_size, 0, "no layout given");
        return false;
    }
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        if (error != NULL && error_size != 0) {
            (void)snprintf(error, error_size, "cannot read %s", path);
        }
        return false;
    }
    /* A file names the points it wants, so start from nothing declared. */
    memset(layout, 0, sizeof(*layout));
    const layout_style built_in = {
        .red = LAYOUT_POINT_RED,
        .green = LAYOUT_POINT_GREEN,
        .blue = LAYOUT_POINT_BLUE,
        .point_size = LAYOUT_POINT_SIZE,
        .bone_width = LAYOUT_BONE_WIDTH,
        .min_visibility = visibility,
    };
    /* Anything written before the first group belongs to no group at all. */
    layout_style group = built_in;
    static pending_bone pending[LAYOUT_MAX_BONES];
    memset(pending, 0, sizeof(pending));
    unsigned long declared = 0;
    bool ok = true;
    unsigned long line_number = 0;
    char line[LAYOUT_LINE_MAX];

    while (ok && fgets(line, (int)sizeof(line), file) != NULL) {
        ++line_number;
        if (strchr(line, '\n') == NULL && !feof(file)) {
            say(error, error_size, line_number, "line is too long");
            ok = false;
            break;
        }
        trim(line);
        char *save = NULL;
        const char *keyword = strtok_r(line, " \t", &save);
        if (keyword == NULL) continue;

        if (strcmp(keyword, "name") == 0) {
            const char *word = strtok_r(NULL, " \t", &save);
            ok = word != NULL && strtok_r(NULL, " \t", &save) == NULL &&
                copy_word(layout->name, sizeof(layout->name), word);
            if (!ok) say(error, error_size, line_number, "expected: name WORD");
        } else if (strcmp(keyword, "keypoints") == 0) {
            const char *word = strtok_r(NULL, " \t", &save);
            ok = word != NULL && strtok_r(NULL, " \t", &save) == NULL &&
                read_index(word, KEYPOINT_MAX_POINTS + 1U, &declared) &&
                declared > 0;
            if (!ok) {
                say(error, error_size, line_number,
                    "expected: keypoints 1 to 32");
            } else {
                layout->keypoint_count = (uint8_t)declared;
            }
        } else if (strcmp(keyword, "group") == 0) {
            /* The name is for whoever reads the file, not for the drawing. */
            const char *word = strtok_r(NULL, " \t", &save);
            layout_style style = built_in;
            ok = word != NULL && strchr(word, '=') == NULL &&
                read_settings(&save, SETTING_COLOR | SETTING_SIZE |
                    SETTING_WIDTH | SETTING_MIN, &style);
            if (!ok) {
                say(error, error_size, line_number, "expected: group NAME "
                    "[color=RRGGBB] [size=N] [width=N] [min=0.N]");
            } else {
                group = style;
            }
        } else if (strcmp(keyword, "point") == 0) {
            unsigned long index = 0;
            layout_style style = group;
            const char *number = strtok_r(NULL, " \t", &save);
            const char *word = strtok_r(NULL, " \t", &save);
            ok = declared != 0 && number != NULL && word != NULL &&
                strchr(word, '=') == NULL &&
                read_index(number, declared, &index) &&
                !layout->points[index].declared &&
                read_settings(&save, SETTING_COLOR | SETTING_SIZE |
                    SETTING_MIN, &style) &&
                copy_word(layout->points[index].name,
                    sizeof(layout->points[index].name), word);
            if (!ok) {
                say(error, error_size, line_number, "expected: point INDEX "
                    "NAME [color=RRGGBB] [size=N] [min=0.N], after keypoints");
            } else {
                layout->points[index].declared = true;
                layout->points[index].min_visibility = style.min_visibility;
                layout->points[index].red = style.red;
                layout->points[index].green = style.green;
                layout->points[index].blue = style.blue;
                layout->points[index].size = style.point_size;
            }
        } else if (strcmp(keyword, "bone") == 0) {
            layout_style style = group;
            const char *first = strtok_r(NULL, " \t", &save);
            const char *second = strtok_r(NULL, " \t", &save);
            ok = declared != 0 && first != NULL && second != NULL &&
                strchr(first, '=') == NULL && strchr(second, '=') == NULL &&
                strcmp(first, second) != 0 &&
                layout->bone_count < LAYOUT_MAX_BONES &&
                read_settings(&save, SETTING_COLOR | SETTING_WIDTH, &style) &&
                copy_word(pending[layout->bone_count].from,
                    KEYPOINT_NAME_MAX, first) &&
                copy_word(pending[layout->bone_count].to,
                    KEYPOINT_NAME_MAX, second);
            if (!ok) {
                say(error, error_size, line_number, "expected: bone FROM TO "
                    "[color=RRGGBB] [width=N], after keypoints");
            } else {
                pending[layout->bone_count].line = line_number;
                layout->bones[layout->bone_count] = (layout_bone) {
                    .red = style.red,
                    .green = style.green,
                    .blue = style.blue,
                    .width = style.bone_width,
                };
                ++layout->bone_count;
            }
        } else {
            say(error, error_size, line_number, "unknown keyword");
            ok = false;
        }
    }
    if (ok && ferror(file)) {
        say(error, error_size, line_number, "read failed");
        ok = false;
    }
    (void)fclose(file);
    if (!ok) return false;

    if (layout->name[0] == '\0' || layout->keypoint_count == 0) {
        say(error, error_size, line_number, "name and keypoints are required");
        return false;
    }
    /* Now that every point is known, a bone can find the ones it names. */
    for (size_t index = 0; index < layout->bone_count; ++index) {
        const pending_bone *ends = &pending[index];
        if (!resolve_end(layout, ends->from, declared,
                &layout->bones[index].from) ||
            !resolve_end(layout, ends->to, declared,
                &layout->bones[index].to)) {
            say(error, error_size, ends->line,
                "a bone joins a point the file does not list");
            return false;
        }
        if (layout->bones[index].from == layout->bones[index].to) {
            say(error, error_size, ends->line, "a bone joins a point to itself");
            return false;
        }
    }
    /* A file that draws nothing at all is a mistake rather than a choice. */
    bool draws = layout->bone_count > 0;
    for (size_t index = 0; !draws && index < KEYPOINT_MAX_POINTS; ++index) {
        draws = layout->points[index].declared &&
            layout->points[index].size > 0;
    }
    if (!draws) {
        say(error, error_size, line_number, "nothing to draw: no bones, and "
            "no point with a size");
        return false;
    }
    return true;
}

void keypoint_layout_limit(keypoint_layout *layout, size_t keypoint_count)
{
    if (layout == NULL || keypoint_count > KEYPOINT_MAX_POINTS) return;
    layout->keypoint_count = (uint8_t)keypoint_count;
    for (size_t index = keypoint_count; index < KEYPOINT_MAX_POINTS; ++index) {
        layout->points[index].declared = false;
    }
}

const char *keypoints_name(const keypoint_layout *layout, size_t keypoint,
    char *buffer, size_t buffer_size)
{
    if (layout != NULL && keypoint < KEYPOINT_MAX_POINTS &&
        layout->points[keypoint].name[0] != '\0') {
        return layout->points[keypoint].name;
    }
    /* Unnamed points still need something to print, so say which one it is. */
    if (buffer == NULL || buffer_size == 0) return "?";
    (void)snprintf(buffer, buffer_size, "point_%zu", keypoint);
    return buffer;
}

/*
 * Which subject a result belongs to: the one it names as its parent, or
 * itself when it names none.
 *
 * Both shapes of pipeline in this tree land on the same answer.  A YOLO pose
 * decoder emits a detection with no parent and hangs its keypoints off that
 * detection's id.  A MediaPipe cascade runs its landmark stage once per
 * detected item, so the runtime stamps everything that stage emits -- the
 * keypoints and the box drawn around them -- with the id of the item they
 * came from, and the box's own id belongs to nothing.  Keying a detection on
 * its own id alone would split that hand into a box and a set of points.
 */
static uint64_t subject_key(const mipk_result *result)
{
    return result->parent_id != MIPK_INVALID_RESULT_ID ? result->parent_id :
        result->result_id;
}

/*
 * Keypoints are looked up rather than assumed to follow their detection,
 * since nothing in the result contract promises the order survives.
 */
static size_t find_or_add(uint64_t keys[KEYPOINT_MAX_SUBJECTS], size_t *count,
    uint64_t key)
{
    for (size_t index = 0; index < *count; ++index) {
        if (keys[index] == key) return index;
    }
    if (*count == KEYPOINT_MAX_SUBJECTS) return KEYPOINT_MAX_SUBJECTS;
    keys[*count] = key;
    return (*count)++;
}

size_t keypoints_collect(const mipk_output *output,
    keypoint_subject subjects[KEYPOINT_MAX_SUBJECTS])
{
    if (output == NULL || subjects == NULL) return 0;
    memset(subjects, 0, sizeof(*subjects) * KEYPOINT_MAX_SUBJECTS);
    uint64_t keys[KEYPOINT_MAX_SUBJECTS] = {0};
    size_t count = 0;
    const size_t result_count = mipk_output_result_count(output);
    for (size_t index = 0; index < result_count; ++index) {
        const mipk_result *result = mipk_output_result_at(output, index);
        if (result == NULL) continue;
        if (result->type == MIPK_RESULT_DETECTION) {
            const size_t slot = find_or_add(keys, &count, subject_key(result));
            if (slot == KEYPOINT_MAX_SUBJECTS) continue;
            subjects[slot].has_box = true;
            subjects[slot].box = result->value.bbox;
            subjects[slot].score = result->score;
        } else if (result->type == MIPK_RESULT_KEYPOINT) {
            const size_t slot = find_or_add(keys, &count, subject_key(result));
            if (slot == KEYPOINT_MAX_SUBJECTS) continue;
            const int32_t id = result->value.keypoint.keypoint_id;
            if (id < 0 || (size_t)id >= KEYPOINT_MAX_POINTS) continue;
            subjects[slot].has_point[id] = true;
            subjects[slot].x[id] = result->value.keypoint.x;
            subjects[slot].y[id] = result->value.keypoint.y;
            subjects[slot].visibility[id] = result->value.keypoint.visibility;
        }
    }
    return count;
}

size_t keypoints_reported(const keypoint_subject *subject)
{
    if (subject == NULL) return 0;
    size_t reported = 0;
    for (size_t point = 0; point < KEYPOINT_MAX_POINTS; ++point) {
        if (subject->has_point[point]) ++reported;
    }
    return reported;
}

bool keypoints_point_shown(const keypoint_layout *layout,
    const keypoint_subject *subject, size_t keypoint)
{
    if (layout == NULL || subject == NULL ||
        keypoint >= KEYPOINT_MAX_POINTS) {
        return false;
    }
    const layout_point *point = &layout->points[keypoint];
    return point->declared && subject->has_point[keypoint] &&
        subject->visibility[keypoint] >= point->min_visibility;
}

size_t keypoints_visible(const keypoint_layout *layout,
    const keypoint_subject *subject)
{
    size_t visible = 0;
    for (size_t point = 0; point < KEYPOINT_MAX_POINTS; ++point) {
        if (keypoints_point_shown(layout, subject, point)) ++visible;
    }
    return visible;
}

size_t keypoints_wanted(const keypoint_layout *layout)
{
    if (layout == NULL) return 0;
    size_t wanted = 0;
    for (size_t point = 0; point < KEYPOINT_MAX_POINTS; ++point) {
        if (layout->points[point].declared) ++wanted;
    }
    return wanted;
}

size_t keypoints_highest_index(const mipk_output *output)
{
    size_t highest = 0;
    const size_t result_count = mipk_output_result_count(output);
    for (size_t index = 0; index < result_count; ++index) {
        const mipk_result *result = mipk_output_result_at(output, index);
        if (result == NULL || result->type != MIPK_RESULT_KEYPOINT) continue;
        const int32_t id = result->value.keypoint.keypoint_id;
        if (id >= 0 && (size_t)id > highest) highest = (size_t)id;
    }
    return highest;
}
