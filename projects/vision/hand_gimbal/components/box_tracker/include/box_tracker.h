#ifndef BOX_TRACKER_H
#define BOX_TRACKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Keeps detections in the same slot from one frame to the next.
 *
 * A detector reports what it sees in each frame and nothing about what it saw
 * in the last one, so two objects visible at once can arrive in either order
 * and swap places.  Anything the caller accumulates per object -- a gesture
 * state machine, a motion history, a servo target -- needs them to stop
 * swapping, which is all this does: it hands back a slot index that stays with
 * the same physical object while it remains visible.
 *
 * It carries no appearance model and issues no identities.  An object that
 * leaves and comes back later is a new object, and two objects that overlap
 * long enough may come back the other way round.  For a handful of objects at
 * video rate that is rarely visible, and the alternative costs a descriptor
 * per object per frame.
 *
 * Matching runs on the predicted position rather than the last seen one.  Each
 * slot carries a smoothed velocity, so a slot is compared against where its
 * object should be now.  Two objects crossing are the case that needs it: at
 * the crossing each one lands nearer to where the other was than to where it
 * was itself, and matching on the last seen position swaps them there.
 */

#define BOX_TRACKER_MAX_SLOTS 4U
/* Returned for an observation that could not be placed at all. */
#define BOX_TRACKER_NO_SLOT SIZE_MAX

typedef struct {
    float x1;
    float y1;
    float x2;
    float y2;
} box_tracker_box;

typedef struct {
    bool has_box;
    box_tracker_box box;
    /* Used when there is no box; ignored otherwise. */
    float center_x;
    float center_y;
} box_tracker_observation;

/*
 * Ratios are held in percent so they can come straight from Kconfig, which has
 * no float type.
 */
typedef struct {
    /* Slots to use, at most BOX_TRACKER_MAX_SLOTS. */
    size_t slot_count;
    /* Frames an object may go unseen before its slot is released. */
    uint32_t missing_frames;
    /*
     * How far an object may travel between frames and still count as the same
     * one, as a percentage of its own width.  Scaling by the object's own size
     * keeps the gate honest at any distance from the camera.
     */
    uint32_t reach_percent;
    /* Overlap with the predicted box that identifies an object outright. */
    uint32_t overlap_percent;
    /* Weight given to the newest step in the velocity estimate. */
    uint32_t smoothing_percent;
} box_tracker_config;

typedef struct {
    box_tracker_config config;
    bool tracked[BOX_TRACKER_MAX_SLOTS];
    uint32_t missing[BOX_TRACKER_MAX_SLOTS];
    float last_x[BOX_TRACKER_MAX_SLOTS];
    float last_y[BOX_TRACKER_MAX_SLOTS];
    float velocity_x[BOX_TRACKER_MAX_SLOTS];
    float velocity_y[BOX_TRACKER_MAX_SLOTS];
    box_tracker_box last_box[BOX_TRACKER_MAX_SLOTS];
    bool has_last_box[BOX_TRACKER_MAX_SLOTS];
} box_tracker;

void box_tracker_config_init(box_tracker_config *config);
void box_tracker_init(box_tracker *tracker, const box_tracker_config *config);

/*
 * Places one frame of observations.
 *
 * assignment[i] receives the slot holding observation i.  released[slot] is
 * set for each slot whose object has now been unseen for long enough to give
 * up on, which is the caller's cue to clear whatever it keeps for that slot.
 * Both arrays are written in full; released may be NULL if it is not wanted.
 */
void box_tracker_update(box_tracker *tracker,
    const box_tracker_observation *observations, size_t count,
    size_t assignment[], bool released[]);

/* Whether a slot currently holds an object, seen this frame or not. */
bool box_tracker_active(const box_tracker *tracker, size_t slot);

#endif
