#include "hand_pinch.h"

#include <math.h>
#include <string.h>

/*
 * Pinch measure
 * -------------
 * The pipeline reports 21 landmarks per hand in source pixels.  Two scale
 * invariant ratios decide the gesture:
 *
 *   pinch_ratio = |thumb tip - index tip| / |index MCP - pinky MCP|
 *   extension   = min over middle/ring/pinky of
 *                 |finger tip - wrist| / |middle MCP - wrist|
 *
 * pinch_ratio alone cannot separate a pinch from a closed fist, because a
 * fist also brings the thumb and index tips together.  Measured over 234
 * hands from HaGRID, fists reach an extension of at most 0.80 while genuine
 * pinches stay above 1.35, so requiring the other fingers to be extended
 * removes the fist entirely.  pinch_ratio itself is bimodal: pinches sit
 * below 0.6 and open hands above 0.8, so the hysteresis band lives in the
 * empty gap between them.
 */

/*
 * Guards against degenerate landmark sets.  Live testing produced occasional
 * false palm detections on background clutter: the landmarks collapse into a
 * few pixels, which drives the palm length towards zero and makes the
 * extension ratio explode past any sensible threshold.  A hand small enough to
 * hit either bound cannot be measured reliably in the first place.
 */
#define HAND_MIN_PALM_PIXELS 12.0F
#define HAND_MAX_EXTENSION 4.0F

void hand_gesture_config_init(hand_gesture_config *config)
{
    if (config == NULL) return;
    *config = (hand_gesture_config) {
        .close_percent = 45,
        .open_percent = 70,
        .extension_percent = 110,
        .straight_percent = 85,
        .confirm_frames = 2,
        .release_frames = 2,
        .missing_frames = 4,
    };
}

void hand_gesture_init(hand_gesture_tracker *tracker,
    const hand_gesture_config *config)
{
    if (tracker == NULL) return;
    memset(tracker, 0, sizeof(*tracker));
    if (config != NULL) {
        tracker->config = *config;
    } else {
        hand_gesture_config_init(&tracker->config);
    }
    if (tracker->config.confirm_frames == 0) tracker->config.confirm_frames = 1;
    if (tracker->config.release_frames == 0) tracker->config.release_frames = 1;

    box_tracker_config boxes;
    box_tracker_config_init(&boxes);
    boxes.slot_count = HAND_GESTURE_MAX_HANDS;
    boxes.missing_frames = tracker->config.missing_frames;
    box_tracker_init(&tracker->boxes, &boxes);
}

const char *hand_event_name(hand_event event)
{
    switch (event) {
    case HAND_EVENT_PINCH_DOWN:
        return "pinch-down";
    case HAND_EVENT_PINCH_UP:
        return "pinch-up";
    case HAND_EVENT_NONE:
    default:
        return "none";
    }
}

static float distance(float x1, float y1, float x2, float y2)
{
    const float dx = x2 - x1;
    const float dy = y2 - y1;
    return sqrtf(dx * dx + dy * dy);
}

static bool landmark_pair(const hand_slot *hand, size_t first, size_t second,
    float *value)
{
    if (!hand->has_point[first] || !hand->has_point[second]) return false;
    *value = distance(hand->x[first], hand->y[first], hand->x[second],
        hand->y[second]);
    return true;
}

/*
 * How straight one finger is: the distance from its knuckle to its tip over
 * the distance walked joint by joint.  A straight finger is 1, a curled one
 * much less.
 *
 * This is what separates a pinch made with the other fingers held out from one
 * made with them curled.  `extension` cannot: it measures how far the tips sit
 * from the wrist, and a finger curling towards the camera keeps its tip far
 * from the wrist in the image even though the finger is plainly bent.
 */
static bool finger_straightness(const hand_slot *hand, size_t knuckle,
    float *value)
{
    float span = 0.0F;
    if (!landmark_pair(hand, knuckle, knuckle + HAND_FINGER_JOINTS - 1U,
        &span)) return false;
    float walked = 0.0F;
    for (size_t joint = 0; joint + 1U < HAND_FINGER_JOINTS; ++joint) {
        float segment = 0.0F;
        if (!landmark_pair(hand, knuckle + joint, knuckle + joint + 1U,
            &segment)) return false;
        walked += segment;
    }
    if (!(walked > 0.0F)) return false;
    *value = span / walked;
    return true;
}

static void measure_hand(hand_slot *hand)
{
    hand->measurable = false;
    hand->pinch_ratio = 0.0F;
    hand->extension = 0.0F;
    hand->straightness = 0.0F;
    float palm_width = 0.0F;
    float palm_length = 0.0F;
    float tip_gap = 0.0F;
    if (!landmark_pair(hand, HAND_LANDMARK_INDEX_MCP, HAND_LANDMARK_PINKY_MCP,
            &palm_width) ||
        !landmark_pair(hand, HAND_LANDMARK_WRIST, HAND_LANDMARK_MIDDLE_MCP,
            &palm_length) ||
        !landmark_pair(hand, HAND_LANDMARK_THUMB_TIP, HAND_LANDMARK_INDEX_TIP,
            &tip_gap)) return;
    if (palm_width < HAND_MIN_PALM_PIXELS ||
        palm_length < HAND_MIN_PALM_PIXELS) return;

    static const size_t knuckles[] = {
        HAND_LANDMARK_MIDDLE_MCP, HAND_LANDMARK_RING_MCP,
        HAND_LANDMARK_PINKY_MCP,
    };
    float shortest = INFINITY;
    float least_straight = INFINITY;
    for (size_t index = 0; index < sizeof(knuckles) / sizeof(knuckles[0]);
        ++index) {
        const size_t tip = knuckles[index] + HAND_FINGER_JOINTS - 1U;
        float reach = 0.0F;
        if (!landmark_pair(hand, HAND_LANDMARK_WRIST, tip, &reach)) return;
        const float extension = reach / palm_length;
        if (extension < shortest) shortest = extension;

        float straight = 0.0F;
        if (!finger_straightness(hand, knuckles[index], &straight)) return;
        if (straight < least_straight) least_straight = straight;
    }
    if (!isfinite(shortest) || shortest > HAND_MAX_EXTENSION) return;
    if (!isfinite(least_straight)) return;
    hand->pinch_ratio = tip_gap / palm_width;
    hand->extension = shortest;
    hand->straightness = least_straight;
    hand->measurable = isfinite(hand->pinch_ratio);
}

/* Collect one frame of results into hands, keyed by the runtime parent id. */
static size_t collect_hands(const mipk_output *output,
    hand_slot observed[HAND_GESTURE_MAX_HANDS])
{
    uint64_t identities[HAND_GESTURE_MAX_HANDS] = {0};
    size_t count = 0;
    const size_t result_count = mipk_output_result_count(output);
    for (size_t index = 0; index < result_count; ++index) {
        const mipk_result *result = mipk_output_result_at(output, index);
        if (result == NULL) continue;
        if (result->type != MIPK_RESULT_KEYPOINT &&
            result->type != MIPK_RESULT_DETECTION) continue;
        size_t slot = count;
        for (size_t search = 0; search < count; ++search) {
            if (identities[search] == result->parent_id) {
                slot = search;
                break;
            }
        }
        if (slot == count) {
            if (count == HAND_GESTURE_MAX_HANDS) continue;
            identities[count] = result->parent_id;
            ++count;
            observed[slot].present = true;
        }
        hand_slot *hand = &observed[slot];
        if (result->type == MIPK_RESULT_DETECTION) {
            hand->bbox = result->value.bbox;
            hand->has_bbox = true;
            hand->score = result->score;
            continue;
        }
        const int32_t id = result->value.keypoint.keypoint_id;
        if (id < 0 || (size_t)id >= HAND_GESTURE_LANDMARK_COUNT) continue;
        hand->has_point[id] = true;
        hand->x[id] = result->value.keypoint.x;
        hand->y[id] = result->value.keypoint.y;
        if (result->score > hand->score) hand->score = result->score;
    }
    for (size_t index = 0; index < count; ++index) {
        measure_hand(&observed[index]);
    }
    return count;
}

static void hand_center(const hand_slot *hand, float *x, float *y)
{
    if (hand->has_bbox) {
        *x = (hand->bbox.x1 + hand->bbox.x2) * 0.5F;
        *y = (hand->bbox.y1 + hand->bbox.y2) * 0.5F;
        return;
    }
    *x = hand->x[HAND_LANDMARK_WRIST];
    *y = hand->y[HAND_LANDMARK_WRIST];
}

/*
 * Advances the pinch state machine for one slot.  Tracking is not this file's
 * concern: by the time this runs, box_tracker has already decided which hand
 * belongs here.  `released` means that decision has just expired, so whatever
 * the slot was holding has to be given up.
 */
static void advance_slot(hand_gesture_tracker *tracker, size_t slot,
    bool present, bool released)
{
    hand_slot *hand = &tracker->slots[slot];
    const hand_gesture_config *config = &tracker->config;
    hand->event = HAND_EVENT_NONE;
    if (!present) {
        if (!released) return;
        if (hand->pinching) {
            hand->pinching = false;
            hand->event = HAND_EVENT_PINCH_UP;
        }
        tracker->close_count[slot] = 0;
        tracker->open_count[slot] = 0;
        return;
    }
    if (!hand->measurable) return;

    const float close_limit = (float)config->close_percent / 100.0F;
    const float open_limit = (float)config->open_percent / 100.0F;
    const float extension_limit = (float)config->extension_percent / 100.0F;
    const float straight_limit = (float)config->straight_percent / 100.0F;
    /*
     * Both finger measures have to agree.  Extension rules out a fist, whose
     * tips come in close to the wrist; straightness rules out the fingers
     * being curled while their tips stay far from it, which extension alone
     * reads as an open hand.
     */
    const bool closed = hand->pinch_ratio < close_limit &&
        hand->extension > extension_limit &&
        hand->straightness > straight_limit;
    /* Release on any signal so an opening hand always ends the pinch. */
    const bool opened = hand->pinch_ratio > open_limit ||
        hand->extension < extension_limit * 0.85F ||
        hand->straightness < straight_limit * 0.9F;

    if (closed) {
        tracker->open_count[slot] = 0;
        if (!hand->pinching &&
            ++tracker->close_count[slot] >= config->confirm_frames) {
            hand->pinching = true;
            hand->event = HAND_EVENT_PINCH_DOWN;
            tracker->close_count[slot] = 0;
        }
        return;
    }
    if (opened) {
        tracker->close_count[slot] = 0;
        if (hand->pinching &&
            ++tracker->open_count[slot] >= config->release_frames) {
            hand->pinching = false;
            hand->event = HAND_EVENT_PINCH_UP;
            tracker->open_count[slot] = 0;
        }
        return;
    }
    /* Inside the hysteresis band: hold the current state. */
    tracker->close_count[slot] = 0;
    tracker->open_count[slot] = 0;
}

void hand_gesture_update(hand_gesture_tracker *tracker,
    const mipk_output *output)
{
    if (tracker == NULL || output == NULL) return;
    hand_slot observed[HAND_GESTURE_MAX_HANDS] = {{0}};
    const size_t count = collect_hands(output, observed);

    box_tracker_observation boxes[HAND_GESTURE_MAX_HANDS] = {{0}};
    for (size_t index = 0; index < count; ++index) {
        const hand_slot *hand = &observed[index];
        boxes[index].has_box = hand->has_bbox;
        boxes[index].box = (box_tracker_box) {
            .x1 = hand->bbox.x1, .y1 = hand->bbox.y1,
            .x2 = hand->bbox.x2, .y2 = hand->bbox.y2,
        };
        hand_center(hand, &boxes[index].center_x, &boxes[index].center_y);
    }
    size_t assignment[HAND_GESTURE_MAX_HANDS];
    bool released[BOX_TRACKER_MAX_SLOTS];
    box_tracker_update(&tracker->boxes, boxes, count, assignment, released);

    bool filled[HAND_GESTURE_MAX_HANDS] = {false};
    for (size_t index = 0; index < count; ++index) {
        const size_t slot = assignment[index];
        if (slot >= HAND_GESTURE_MAX_HANDS) continue;
        hand_slot *hand = &tracker->slots[slot];
        /* The observation carries no gesture state, so keep the slot's own. */
        const bool pinching = hand->pinching;
        *hand = observed[index];
        hand->pinching = pinching;
        filled[slot] = true;
    }
    for (size_t slot = 0; slot < HAND_GESTURE_MAX_HANDS; ++slot) {
        if (!filled[slot]) {
            tracker->slots[slot].present = false;
            tracker->slots[slot].measurable = false;
            tracker->slots[slot].has_bbox = false;
        }
        advance_slot(tracker, slot, filled[slot], released[slot]);
    }
}
