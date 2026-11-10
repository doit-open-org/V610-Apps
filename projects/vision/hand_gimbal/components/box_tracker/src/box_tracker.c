#include "box_tracker.h"

#include <math.h>
#include <string.h>

void box_tracker_config_init(box_tracker_config *config)
{
    if (config == NULL) return;
    *config = (box_tracker_config) {
        .slot_count = 2,
        .missing_frames = 4,
        .reach_percent = 150,
        .overlap_percent = 30,
        .smoothing_percent = 50,
    };
}

void box_tracker_init(box_tracker *tracker, const box_tracker_config *config)
{
    if (tracker == NULL) return;
    memset(tracker, 0, sizeof(*tracker));
    if (config != NULL) {
        tracker->config = *config;
    } else {
        box_tracker_config_init(&tracker->config);
    }
    if (tracker->config.slot_count == 0 ||
        tracker->config.slot_count > BOX_TRACKER_MAX_SLOTS) {
        tracker->config.slot_count = BOX_TRACKER_MAX_SLOTS;
    }
    if (tracker->config.missing_frames == 0) {
        tracker->config.missing_frames = 1;
    }
}

bool box_tracker_active(const box_tracker *tracker, size_t slot)
{
    if (tracker == NULL || slot >= BOX_TRACKER_MAX_SLOTS) return false;
    return tracker->tracked[slot];
}

static void observation_center(const box_tracker_observation *observation,
    float *x, float *y)
{
    if (observation->has_box) {
        *x = (observation->box.x1 + observation->box.x2) * 0.5F;
        *y = (observation->box.y1 + observation->box.y2) * 0.5F;
        return;
    }
    *x = observation->center_x;
    *y = observation->center_y;
}

static float box_overlap(const box_tracker_box *a, const box_tracker_box *b)
{
    const float x1 = fmaxf(a->x1, b->x1);
    const float y1 = fmaxf(a->y1, b->y1);
    const float x2 = fminf(a->x2, b->x2);
    const float y2 = fminf(a->y2, b->y2);
    if (!(x2 > x1) || !(y2 > y1)) return 0.0F;
    const float shared = (x2 - x1) * (y2 - y1);
    const float area_a = (a->x2 - a->x1) * (a->y2 - a->y1);
    const float area_b = (b->x2 - b->x1) * (b->y2 - b->y1);
    const float total = area_a + area_b - shared;
    return total > 0.0F ? shared / total : 0.0F;
}

/* Shifts a slot's last box by where its object was heading. */
static box_tracker_box predict_box(const box_tracker *tracker, size_t slot)
{
    const box_tracker_box *last = &tracker->last_box[slot];
    const float dx = tracker->velocity_x[slot];
    const float dy = tracker->velocity_y[slot];
    return (box_tracker_box) {
        .x1 = last->x1 + dx, .y1 = last->y1 + dy,
        .x2 = last->x2 + dx, .y2 = last->y2 + dy,
    };
}

/*
 * How well an observation continues a slot, as a number that is zero when it
 * cannot and larger the better it does.
 *
 * Overlap with the predicted box decides it whenever there is enough of it,
 * because two boxes that overlap that far are the same object however far
 * their centres have moved.  Otherwise the score falls back to distance, kept
 * strictly below the overlap band so any real overlap wins against it.
 */
static float match_score(const box_tracker *tracker, size_t slot,
    const box_tracker_observation *observation)
{
    const float overlap_limit =
        (float)tracker->config.overlap_percent / 100.0F;
    if (observation->has_box && tracker->has_last_box[slot]) {
        const box_tracker_box predicted = predict_box(tracker, slot);
        const float overlap = box_overlap(&observation->box, &predicted);
        if (overlap >= overlap_limit) return overlap;
    }

    float x = 0.0F;
    float y = 0.0F;
    observation_center(observation, &x, &y);
    const float predicted_x = tracker->last_x[slot] +
        tracker->velocity_x[slot];
    const float predicted_y = tracker->last_y[slot] +
        tracker->velocity_y[slot];
    const float width = observation->has_box ?
        observation->box.x2 - observation->box.x1 : 0.0F;
    const float reach = (width > 1.0F ? width : 1.0F) *
        ((float)tracker->config.reach_percent / 100.0F);
    const float dx = x - predicted_x;
    const float dy = y - predicted_y;
    const float gap = sqrtf(dx * dx + dy * dy);
    if (!(reach > 0.0F) || gap > reach) return 0.0F;
    return (1.0F - gap / reach) * overlap_limit;
}

/*
 * Matching is greedy over the strongest pairing available: every observation
 * is scored against every live slot, and the best pair is committed until
 * nothing scores at all.  Greedy on the global best, rather than first come
 * first served, is what stops one object claiming the slot that suits another
 * much better.
 */
static void assign_slots(const box_tracker *tracker,
    const box_tracker_observation *observations, size_t count,
    size_t assignment[])
{
    const size_t slots = tracker->config.slot_count;
    bool taken[BOX_TRACKER_MAX_SLOTS] = {false};
    for (size_t index = 0; index < count; ++index) {
        assignment[index] = BOX_TRACKER_NO_SLOT;
    }

    for (;;) {
        float best_score = 0.0F;
        size_t best_observation = BOX_TRACKER_NO_SLOT;
        size_t best_slot = BOX_TRACKER_NO_SLOT;
        for (size_t index = 0; index < count; ++index) {
            if (assignment[index] != BOX_TRACKER_NO_SLOT) continue;
            for (size_t slot = 0; slot < slots; ++slot) {
                if (taken[slot] || !tracker->tracked[slot]) continue;
                const float score = match_score(tracker, slot,
                    &observations[index]);
                if (score > best_score) {
                    best_score = score;
                    best_observation = index;
                    best_slot = slot;
                }
            }
        }
        if (best_observation == BOX_TRACKER_NO_SLOT) break;
        taken[best_slot] = true;
        assignment[best_observation] = best_slot;
    }

    /* Whatever is left is an object the tracker has not seen before. */
    for (size_t index = 0; index < count; ++index) {
        if (assignment[index] != BOX_TRACKER_NO_SLOT) continue;
        for (size_t slot = 0; slot < slots; ++slot) {
            if (taken[slot]) continue;
            taken[slot] = true;
            assignment[index] = slot;
            break;
        }
    }
}

void box_tracker_update(box_tracker *tracker,
    const box_tracker_observation *observations, size_t count,
    size_t assignment[], bool released[])
{
    if (tracker == NULL || assignment == NULL ||
        (observations == NULL && count > 0)) return;
    const size_t slots = tracker->config.slot_count;
    if (released != NULL) {
        for (size_t slot = 0; slot < BOX_TRACKER_MAX_SLOTS; ++slot) {
            released[slot] = false;
        }
    }
    assign_slots(tracker, observations, count, assignment);

    const float smoothing =
        (float)tracker->config.smoothing_percent / 100.0F;
    bool filled[BOX_TRACKER_MAX_SLOTS] = {false};
    for (size_t index = 0; index < count; ++index) {
        const size_t slot = assignment[index];
        if (slot >= slots) continue;
        filled[slot] = true;
        float x = 0.0F;
        float y = 0.0F;
        observation_center(&observations[index], &x, &y);
        if (tracker->tracked[slot]) {
            /*
             * Blend the step just taken into the running estimate, so the
             * prediction follows a sustained movement without chasing the
             * jitter of a single frame.
             */
            tracker->velocity_x[slot] +=
                (x - tracker->last_x[slot] - tracker->velocity_x[slot]) *
                smoothing;
            tracker->velocity_y[slot] +=
                (y - tracker->last_y[slot] - tracker->velocity_y[slot]) *
                smoothing;
        } else {
            tracker->velocity_x[slot] = 0.0F;
            tracker->velocity_y[slot] = 0.0F;
        }
        tracker->last_x[slot] = x;
        tracker->last_y[slot] = y;
        tracker->last_box[slot] = observations[index].box;
        tracker->has_last_box[slot] = observations[index].has_box;
        tracker->tracked[slot] = true;
        tracker->missing[slot] = 0;
    }

    for (size_t slot = 0; slot < slots; ++slot) {
        if (filled[slot] || !tracker->tracked[slot]) continue;
        /* Tolerate a few dropped frames before giving up on the object. */
        if (++tracker->missing[slot] < tracker->config.missing_frames) continue;
        tracker->tracked[slot] = false;
        tracker->has_last_box[slot] = false;
        tracker->velocity_x[slot] = 0.0F;
        tracker->velocity_y[slot] = 0.0F;
        if (released != NULL) released[slot] = true;
    }
}
