#ifndef KEYPOINTS_H
#define KEYPOINTS_H

#include "mipk.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Keypoints from a model, and how to draw them.
 *
 * What the model does is detect points: the decoder takes the keypoint count
 * from the manifest and hands back a detection followed by its keypoints,
 * knowing nothing about what they are.  With no more than that the sample
 * marks each point where it lies, which is all any keypoint model needs and
 * works for a model nobody here has seen before.
 *
 * A .keypoints file says what to make of them.  Only a person choosing which
 * joints to join knows that an ankle belongs to a knee, so that choice is
 * written down rather than compiled in: a new model is two files dropped next
 * to the program -- the package and its layout -- and no rebuild.
 *
 * A file is a few groups, because a subject is a few body parts and each
 * wants its own colour:
 *
 *     name dog                  # what to call the subject in the log
 *     keypoints 24              # must match the model's own count
 *
 *     group leg_left color=40a0ff
 *       point 0 front_left_paw            # index in the model, then a name
 *       point 1 front_left_knee
 *       bone front_left_paw front_left_knee
 *
 *     group head color=ff40c8 size=8
 *       point 16 nose color=ff2020 size=10
 *       point 17 chin min=0.7
 *       bone left_ear_base nose width=5
 *
 * A group runs until the next one.  Its settings are the defaults for the
 * points and bones inside it, and any of them may say something else for
 * itself; the indentation is only there to be read.  The settings are
 * color=RRGGBB, size=N for a dot, width=N for a line, and min=0.N for the
 * confidence a point needs before it is used.  A group takes all four, a
 * point takes colour, size and min, a bone takes colour and width.
 *
 * A bone names its ends, since "bone left_ear_base nose" says what it joins
 * and "bone 14 16" does not.  An index works too, and either end may be
 * declared further down the file.
 *
 * A point the file does not list is neither drawn nor counted, which is how a
 * part the model was never taught, or one nobody wants to see, is left out.
 * A point of size 0 is not drawn either, which is how a file that would
 * rather show only its bones says so.
 *
 * Bones are optional, and so is the file: with no bones the points are drawn
 * on their own, and with no file at all every point is drawn in one colour,
 * which is keypoint_layout_default() and the same thing again.  There is one
 * format and one drawing path; a file only ever changes what they are given.
 *
 * Blank lines and anything after a # are ignored, so a colour is written as
 * bare hex digits with no leading #.
 */

#define KEYPOINT_MAX_SUBJECTS 4U
/* Enough for the pose models in use; COCO has 17, the dog dataset 24. */
#define KEYPOINT_MAX_POINTS 32U
#define LAYOUT_MAX_BONES 64U
#define KEYPOINT_NAME_MAX 32U

typedef struct {
    bool has_box;
    mipk_bbox box;
    float score;
    bool has_point[KEYPOINT_MAX_POINTS];
    float x[KEYPOINT_MAX_POINTS];
    float y[KEYPOINT_MAX_POINTS];
    /* The model's confidence that the point is visible, 0 to 1. */
    float visibility[KEYPOINT_MAX_POINTS];
} keypoint_subject;

typedef struct {
    /* False for a point the file leaves out: not drawn, not counted. */
    bool declared;
    char name[KEYPOINT_NAME_MAX];
    /* Confidence the model must report before the point is used. */
    float min_visibility;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    /* Diameter of the dot, in pixels of the model's frame.  0 draws none. */
    uint8_t size;
} layout_point;

typedef struct {
    uint8_t from;
    uint8_t to;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    /* Width of the line, in pixels of the model's frame. */
    uint8_t width;
} layout_bone;

typedef struct {
    char name[KEYPOINT_NAME_MAX];
    /*
     * Keypoints the model emits, as the file declares them, so a layout
     * paired with the wrong model can be caught.  Zero when no file said.
     */
    uint8_t keypoint_count;
    uint8_t bone_count;
    layout_point points[KEYPOINT_MAX_POINTS];
    layout_bone bones[LAYOUT_MAX_BONES];
} keypoint_layout;

/*
 * The layout used when no file is given: every point the model can report,
 * drawn as a dot of one colour, and no bones.  `visibility` is the confidence
 * a point needs, and is also where a file's own defaults start.
 */
void keypoint_layout_default(keypoint_layout *layout, float visibility);

/*
 * Reads a .keypoints file over keypoint_layout_default().  On failure it
 * returns false and writes why to `error`, which is always NUL terminated;
 * the message names the line at fault.
 */
bool keypoint_layout_load(const char *path, keypoint_layout *layout, float visibility,
    char *error, size_t error_size);

/*
 * Narrows the default layout to the points the model turned out to report,
 * which nothing knows until the first subject arrives.
 */
void keypoint_layout_limit(keypoint_layout *layout, size_t keypoint_count);

/* The point's name, or its index when no file names it. */
const char *keypoints_name(const keypoint_layout *layout, size_t keypoint,
    char *buffer, size_t buffer_size);

/*
 * Groups one frame of results by subject.  The decoder reports each subject as
 * a detection followed by its keypoints, and the keypoints name the detection
 * as their parent; this undoes that.  Returns the number of subjects.
 */
size_t keypoints_collect(const mipk_output *output,
    keypoint_subject subjects[KEYPOINT_MAX_SUBJECTS]);

/*
 * Whether a point is worth using: reported by the model, listed by the
 * layout, and as confident as that point asks for.
 */
bool keypoints_point_shown(const keypoint_layout *layout,
    const keypoint_subject *subject, size_t keypoint);

/* Points on a subject that keypoints_point_shown accepts. */
size_t keypoints_visible(const keypoint_layout *layout,
    const keypoint_subject *subject);

/* Points the layout lists at all, visible or not. */
size_t keypoints_wanted(const keypoint_layout *layout);

/* Points a subject reported at all, whether visible or not. */
size_t keypoints_reported(const keypoint_subject *subject);

/*
 * The highest keypoint index the model reported, so a layout paired with the
 * wrong model can be caught rather than drawn as nonsense.
 */
size_t keypoints_highest_index(const mipk_output *output);

#endif
