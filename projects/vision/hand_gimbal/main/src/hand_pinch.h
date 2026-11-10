#ifndef HAND_PINCH_H
#define HAND_PINCH_H

#include "box_tracker.h"
#include "mipk.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HAND_GESTURE_MAX_HANDS 2U
#define HAND_GESTURE_LANDMARK_COUNT 21U

/* MediaPipe hand landmark indices used by the pinch measure. */
#define HAND_LANDMARK_WRIST 0U
#define HAND_LANDMARK_THUMB_TIP 4U
#define HAND_LANDMARK_INDEX_MCP 5U
#define HAND_LANDMARK_INDEX_TIP 8U
#define HAND_LANDMARK_MIDDLE_MCP 9U
#define HAND_LANDMARK_MIDDLE_TIP 12U
#define HAND_LANDMARK_RING_MCP 13U
#define HAND_LANDMARK_RING_TIP 16U
#define HAND_LANDMARK_PINKY_MCP 17U
#define HAND_LANDMARK_PINKY_TIP 20U
/*
 * Each finger is four consecutive landmarks from its knuckle outwards, so a
 * finger can be walked from its MCP index alone.
 */
#define HAND_FINGER_JOINTS 4U

typedef enum {
    HAND_EVENT_NONE = 0,
    HAND_EVENT_PINCH_DOWN,
    HAND_EVENT_PINCH_UP,
} hand_event;

typedef struct {
    bool present;
    bool has_bbox;
    mipk_bbox bbox;
    float score;
    bool has_point[HAND_GESTURE_LANDMARK_COUNT];
    float x[HAND_GESTURE_LANDMARK_COUNT];
    float y[HAND_GESTURE_LANDMARK_COUNT];
    /* Thumb-to-index distance over palm width; scale invariant. */
    float pinch_ratio;
    /* Shortest of the middle, ring and pinky extensions over palm length. */
    float extension;
    /*
     * How straight the middle, ring and pinky are, taking the least straight
     * of the three: knuckle to tip over the length of the finger walked joint
     * by joint.  One is a straight finger.
     */
    float straightness;
    bool measurable;
    bool pinching;
    hand_event event;
} hand_slot;

typedef struct {
    /* Ratios are expressed in percent so they can come from Kconfig. */
    uint32_t close_percent;
    uint32_t open_percent;
    uint32_t extension_percent;
    uint32_t straight_percent;
    uint32_t confirm_frames;
    uint32_t release_frames;
    uint32_t missing_frames;
} hand_gesture_config;

typedef struct {
    hand_gesture_config config;
    hand_slot slots[HAND_GESTURE_MAX_HANDS];
    uint32_t close_count[HAND_GESTURE_MAX_HANDS];
    uint32_t open_count[HAND_GESTURE_MAX_HANDS];
    /* Keeps each hand in the slot it occupied last frame. */
    box_tracker boxes;
} hand_gesture_tracker;

void hand_gesture_config_init(hand_gesture_config *config);
void hand_gesture_init(hand_gesture_tracker *tracker,
    const hand_gesture_config *config);

/*
 * Groups one frame of pipeline results into hands, keeps each hand in the
 * slot it occupied last frame, and advances the pinch state machine.
 */
void hand_gesture_update(hand_gesture_tracker *tracker,
    const mipk_output *output);

const char *hand_event_name(hand_event event);

#endif
