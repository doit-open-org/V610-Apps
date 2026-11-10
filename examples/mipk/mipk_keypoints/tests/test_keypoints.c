/*
 * Reading a .keypoints file.
 *
 * The file is what makes a new keypoint model a drop-in rather than a
 * rebuild, so it is worth being strict about: a typo has to be reported with
 * its line number, not silently produce a layout missing a limb.  The shipped
 * files are checked against what their models actually emit.
 */

#include "keypoints.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * The grouping functions read a pipeline output, which the board owns.  These
 * stand in for it and serve whatever a test has laid out.
 */
static const mipk_result *g_results;
static size_t g_result_count;

size_t mipk_output_result_count(const mipk_output *output)
{
    (void)output;
    return g_result_count;
}

const mipk_result *mipk_output_result_at(const mipk_output *output,
    size_t index)
{
    (void)output;
    return index < g_result_count ? &g_results[index] : NULL;
}

/* Any non-null output will do: the stubs above answer from g_results. */
static const mipk_output *frame(void)
{
    return (const mipk_output *)&g_result_count;
}

static mipk_result detection(uint64_t result_id, uint64_t parent_id,
    float score)
{
    mipk_result result = {0};
    result.type = MIPK_RESULT_DETECTION;
    result.result_id = result_id;
    result.parent_id = parent_id;
    result.score = score;
    result.value.bbox = (mipk_bbox) {.x1 = 1.0F, .y1 = 2.0F, .x2 = 3.0F,
        .y2 = 4.0F};
    return result;
}

static mipk_result keypoint(uint64_t result_id, uint64_t parent_id,
    int32_t keypoint_id)
{
    mipk_result result = {0};
    result.type = MIPK_RESULT_KEYPOINT;
    result.result_id = result_id;
    result.parent_id = parent_id;
    result.value.keypoint.keypoint_id = keypoint_id;
    result.value.keypoint.visibility = 0.9F;
    return result;
}

static char g_directory[256];

static const char *write_file(const char *name, const char *text)
{
    static char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s", g_directory, name);
    FILE *file = fopen(path, "w");
    assert(file != NULL);
    (void)fputs(text, file);
    (void)fclose(file);
    return path;
}

/* A file that should not load, and the word its complaint must contain. */
static void refuses(const char *text, const char *expected, const char *what)
{
    keypoint_layout layout;
    char reason[160] = {0};
    const char *path = write_file("bad.keypoints", text);
    assert(!keypoint_layout_load(path, &layout, 0.5F, reason, sizeof(reason)));
    assert(reason[0] != '\0');
    assert(strstr(reason, expected) != NULL);
    printf("  refused %-34s (%s)\n", what, reason);
}

static const char *const GOOD =
    "# a comment\n"
    "name thing\n"
    "keypoints 4\n"
    "\n"
    "group limb color=112233 size=5 width=2 min=0.4\n"
    "  point 0 head   # trailing comment\n"
    "  point 1 middle min=0.8\n"
    "  bone head middle\n"
    "\n"
    "group tail color=445566\n"
    "  point 2 tip color=ff0000 size=12\n"
    "  bone middle tip width=7\n";

static void test_reads_a_file(void)
{
    keypoint_layout layout;
    char reason[160] = {0};
    assert(keypoint_layout_load(write_file("good.keypoints", GOOD), &layout, 0.5F,
        reason, sizeof(reason)));
    assert(strcmp(layout.name, "thing") == 0);
    assert(layout.keypoint_count == 4);
    assert(layout.bone_count == 2);
    char name[KEYPOINT_NAME_MAX];
    assert(strcmp(keypoints_name(&layout, 0, name, sizeof(name)),
        "head") == 0);
    /* Point 3 is declared by "keypoints" but listed by no group. */
    assert(!layout.points[3].declared);
    assert(strcmp(keypoints_name(&layout, 3, name, sizeof(name)),
        "point_3") == 0);
    assert(keypoints_wanted(&layout) == 3);

    /* A group reaches its own points; another group does not. */
    assert(layout.points[0].red == 0x11 && layout.points[0].green == 0x22 &&
        layout.points[0].blue == 0x33);
    assert(layout.points[0].size == 5);
    assert(layout.points[0].min_visibility == 0.4F);
    assert(layout.points[1].min_visibility == 0.8F);
    assert(layout.points[1].size == 5);
    /* The second group takes the built-in size again, not the first group's. */
    assert(layout.points[2].red == 0xFF && layout.points[2].size == 12);
    assert(layout.points[2].min_visibility == 0.5F);

    assert(layout.bones[0].from == 0 && layout.bones[0].to == 1);
    assert(layout.bones[0].red == 0x11 && layout.bones[0].width == 2);
    assert(layout.bones[1].from == 1 && layout.bones[1].to == 2);
    assert(layout.bones[1].red == 0x44 && layout.bones[1].width == 7);
    printf("  read a layout: %u keypoints, %zu listed, %u bones\n",
        layout.keypoint_count, keypoints_wanted(&layout),
        layout.bone_count);
}

/*
 * A bone names its ends so the line says what it joins.  An index still works,
 * and an end may be declared in a group further down the file.
 */
static void test_bone_ends(void)
{
    keypoint_layout layout;
    char reason[160] = {0};
    assert(keypoint_layout_load(write_file("ends.keypoints",
        "name e\nkeypoints 3\n"
        "group a color=010101\n  point 0 first\n  bone first last\n"
        "group b color=020202\n  point 2 last\n  bone 0 2\n"),
        &layout, 0.5F, reason, sizeof(reason)));
    assert(layout.bone_count == 2);
    /* Named forwards, and the same pair by index. */
    assert(layout.bones[0].from == 0 && layout.bones[0].to == 2);
    assert(layout.bones[1].from == 0 && layout.bones[1].to == 2);
    /* Each kept the colour of the group it was written in. */
    assert(layout.bones[0].red == 0x01 && layout.bones[1].red == 0x02);
    printf("  bones name their ends, forwards or by index\n");
}

/* A file may draw points alone; bones are what is optional, not the file. */
static void test_points_without_bones(void)
{
    keypoint_layout layout;
    char reason[160] = {0};
    assert(keypoint_layout_load(write_file("dots.keypoints",
        "name dots\nkeypoints 2\ngroup all\n point 0 a\n point 1 b\n"),
        &layout, 0.5F, reason, sizeof(reason)));
    assert(layout.bone_count == 0);
    assert(layout.points[0].size > 0);
    printf("  a file with no bones draws its points\n");
}

/* Lines written before any group take the built-in settings. */
static void test_groupless_lines(void)
{
    keypoint_layout layout;
    char reason[160] = {0};
    assert(keypoint_layout_load(write_file("plain.keypoints",
        "name p\nkeypoints 2\npoint 0 a\npoint 1 b\nbone a b\n"),
        &layout, 0.25F, reason, sizeof(reason)));
    assert(layout.points[0].min_visibility == 0.25F);
    assert(layout.points[0].size > 0);
    assert(layout.bones[0].width > 0);
    printf("  lines outside any group still work\n");
}

static void test_refuses_broken_files(void)
{
    refuses("name thing\nkeypoints 2\ngroup g size=0\n point 0 a\n point 1 b\n",
        "nothing to draw", "a file that draws nothing");
    refuses("keypoints 2\npoint 0 a\npoint 1 b\nbone a b\n",
        "required", "no name line");
    refuses("name thing\npoint 0 a\n", "point", "point before keypoints");
    refuses("name thing\nkeypoints 2\npoint 2 a\n", "point",
        "point index past the count");
    refuses("name thing\nkeypoints 2\npoint 0 a\npoint 0 b\n", "point",
        "the same point listed twice");
    refuses("name thing\nkeypoints 2\npoint 0 a min=1.5\n", "point",
        "a visibility above one");
    refuses("name thing\nkeypoints 2\npoint 0 a color=ff00\n", "point",
        "a colour that is not six digits");
    refuses("name thing\nkeypoints 2\npoint 0 a color=gggggg\n", "point",
        "a colour that is not hex");
    refuses("name thing\nkeypoints 2\npoint 0 a width=3\n", "point",
        "a bone setting on a point");
    refuses("name thing\nkeypoints 2\npoint 0 a\npoint 1 b\nbone a b size=3\n",
        "bone", "a point setting on a bone");
    refuses("name thing\nkeypoints 2\npoint 0 a\npoint 1 b\nbone a b hue=3\n",
        "bone", "an unknown setting");
    refuses("name thing\nkeypoints 2\npoint 0 a\npoint 1 b\nbone a nowhere\n",
        "does not list", "a bone naming a point that is not there");
    refuses("name thing\nkeypoints 2\npoint 0 a\npoint 1 b\nbone a a\n",
        "bone", "a bone joining a point to itself");
    refuses("name thing\nkeypoints 3\npoint 0 a\npoint 1 b\nbone a 2\n",
        "does not list", "a bone reaching an unlisted index");
    refuses("name thing\nkeypoints 2\ngroup\n", "group", "a group with no name");
    refuses("name thing\nkeypoints 99\n", "keypoints", "more keypoints than fit");
    refuses("name thing\nkeypoints 2\nwobble 1\n", "unknown", "an unknown keyword");
}

/*
 * No file is a working configuration, and it is the same layout: every point,
 * one colour, no bones.  The drawing code has one path, so this has to stand
 * on its own without a file.
 */
static void test_default_layout(void)
{
    keypoint_layout layout;
    keypoint_layout_default(&layout, 0.5F);
    assert(layout.bone_count == 0);
    assert(keypoints_wanted(&layout) == KEYPOINT_MAX_POINTS);
    /* One colour and one size for all of them. */
    for (size_t point = 1; point < KEYPOINT_MAX_POINTS; ++point) {
        assert(layout.points[point].red == layout.points[0].red);
        assert(layout.points[point].green == layout.points[0].green);
        assert(layout.points[point].blue == layout.points[0].blue);
        assert(layout.points[point].size == layout.points[0].size);
    }
    assert(layout.points[0].size > 0);

    char name[KEYPOINT_NAME_MAX];
    assert(strcmp(keypoints_name(&layout, 7, name, sizeof(name)),
        "point_7") == 0);

    keypoint_subject subject = {0};
    subject.has_point[0] = true;
    subject.visibility[0] = 0.9F;
    subject.has_point[5] = true;
    subject.visibility[5] = 0.2F;
    assert(keypoints_reported(&subject) == 2);
    assert(keypoints_visible(&layout, &subject) == 1);

    /* The model's own count settles how many there are. */
    keypoint_layout_limit(&layout, 6);
    assert(keypoints_wanted(&layout) == 6);
    assert(!keypoints_point_shown(&layout, &subject, 7));
    printf("  the default layout: every point, one colour, no bones\n");
}

/*
 * Grouping a frame back into subjects, for both shapes of pipeline in this
 * tree.  They number their results differently, and the rule -- a result
 * belongs to its parent, or to itself when it has none -- has to hold for
 * both or one of them comes apart into loose boxes and loose points.
 */
static void test_grouping_a_yolo_pose_frame(void)
{
    /* The decoder gives each detection an id and hangs its points off it. */
    static const mipk_result results[] = {
        {.type = MIPK_RESULT_DETECTION, .result_id = 1, .score = 0.9F},
        {.type = MIPK_RESULT_KEYPOINT, .result_id = 2, .parent_id = 1,
            .value.keypoint = {.keypoint_id = 0, .visibility = 0.9F}},
        {.type = MIPK_RESULT_KEYPOINT, .result_id = 3, .parent_id = 1,
            .value.keypoint = {.keypoint_id = 1, .visibility = 0.9F}},
        {.type = MIPK_RESULT_DETECTION, .result_id = 4, .score = 0.7F},
        {.type = MIPK_RESULT_KEYPOINT, .result_id = 5, .parent_id = 4,
            .value.keypoint = {.keypoint_id = 0, .visibility = 0.9F}},
    };
    g_results = results;
    g_result_count = sizeof(results) / sizeof(results[0]);
    keypoint_subject subjects[KEYPOINT_MAX_SUBJECTS];
    assert(keypoints_collect(frame(), subjects) == 2);
    assert(subjects[0].has_box && subjects[0].has_point[0] &&
        subjects[0].has_point[1]);
    assert(subjects[1].has_box && subjects[1].has_point[0]);
    printf("  a yolo pose frame: 2 subjects, each with its own points\n");
}

static void test_grouping_a_mediapipe_cascade_frame(void)
{
    /*
     * The landmark stage runs per item, so the runtime stamps everything it
     * emits -- points and the box around them -- with the id of the palm
     * detection they came from, and gives the box an unrelated id of its own.
     */
    static mipk_result results[6];
    results[0] = detection(1, 0, 0.8F);           /* the palm detection */
    results[1] = keypoint(10, 1, 0);
    results[2] = keypoint(11, 1, 1);
    results[3] = detection(12, 1, 0.8F);          /* the landmark box */
    results[4] = detection(2, 0, 0.6F);           /* a second palm */
    results[5] = keypoint(13, 2, 0);
    g_results = results;
    g_result_count = sizeof(results) / sizeof(results[0]);
    keypoint_subject subjects[KEYPOINT_MAX_SUBJECTS];
    /* Two hands, not two boxes and two clouds of points. */
    assert(keypoints_collect(frame(), subjects) == 2);
    assert(subjects[0].has_box && subjects[0].has_point[0] &&
        subjects[0].has_point[1]);
    assert(subjects[1].has_box && subjects[1].has_point[0]);
    g_results = NULL;
    g_result_count = 0;
    printf("  a mediapipe cascade frame: 2 subjects, box and points together\n");
}

static void test_missing_file_is_reported(void)
{
    keypoint_layout layout;
    char reason[160] = {0};
    assert(!keypoint_layout_load("/nonexistent/layout.keypoints", &layout, 0.5F,
        reason, sizeof(reason)));
    assert(strstr(reason, "cannot read") != NULL);
    printf("  refused a missing file           (%s)\n", reason);
}

/* The files shipped with the sample must match the models they belong to. */
static void test_shipped_files(const char *directory)
{
    const struct {
        const char *file;
        const char *name;
        uint8_t keypoints;
        size_t listed;
        uint8_t bones;
    } expected[] = {
        {"coco.keypoints", "person", 17, 17, 19},
        /* A second, quite different model: a MediaPipe cascade. */
        {"hand.keypoints", "hand", 21, 21, 21},
    };
    for (size_t index = 0; index < sizeof(expected) / sizeof(expected[0]);
        ++index) {
        char path[512];
        (void)snprintf(path, sizeof(path), "%s/%s", directory,
            expected[index].file);
        keypoint_layout layout;
        char reason[160] = {0};
        assert(keypoint_layout_load(path, &layout, 0.5F, reason, sizeof(reason)));
        assert(strcmp(layout.name, expected[index].name) == 0);
        assert(layout.keypoint_count == expected[index].keypoints);
        assert(keypoints_wanted(&layout) == expected[index].listed);
        assert(layout.bone_count == expected[index].bones);
        /* Every bone joins points the file lists, and carries a colour. */
        for (size_t bone = 0; bone < layout.bone_count; ++bone) {
            assert(layout.points[layout.bones[bone].from].declared);
            assert(layout.points[layout.bones[bone].to].declared);
            assert(layout.bones[bone].width > 0);
        }
        printf("  %-10s %-7s %2u keypoints, %2zu listed, %2u bones\n",
            expected[index].file, layout.name, layout.keypoint_count,
            keypoints_wanted(&layout), layout.bone_count);
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <keypoints directory>\n", argv[0]);
        return 2;
    }
    const char *temp = getenv("TMPDIR");
    (void)snprintf(g_directory, sizeof(g_directory), "%s/keypoints_%d",
        temp != NULL ? temp : "/tmp", (int)getpid());
    char command[600];
    (void)snprintf(command, sizeof(command), "mkdir -p '%s'", g_directory);
    assert(system(command) == 0);

    puts("keypoint layout");
    test_reads_a_file();
    test_bone_ends();
    test_points_without_bones();
    test_groupless_lines();
    test_refuses_broken_files();
    test_default_layout();
    test_grouping_a_yolo_pose_frame();
    test_grouping_a_mediapipe_cascade_frame();
    test_missing_file_is_reported();
    test_shipped_files(argv[1]);

    (void)snprintf(command, sizeof(command), "rm -rf '%s'", g_directory);
    assert(system(command) == 0);
    puts("keypoint layout: all passed");
    return 0;
}
