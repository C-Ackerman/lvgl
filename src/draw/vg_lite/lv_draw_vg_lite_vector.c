/**
 * @file lv_draw_vg_lite_vector.c
 *
 */

/*********************
 *      INCLUDES
 *********************/

#include "../lv_image_decoder_private.h"
#include "../lv_draw_vector_private.h"
#include "lv_draw_vg_lite.h"

#if LV_USE_DRAW_VG_LITE && LV_USE_VECTOR_GRAPHIC

#include "lv_draw_vg_lite_type.h"
#include "lv_vg_lite_path.h"
#include "lv_vg_lite_pending.h"
#include "lv_vg_lite_utils.h"
#include "lv_vg_lite_grad.h"
#include "lv_vg_lite_stroke.h"
#include <float.h>
#include <math.h>

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

typedef void * path_drop_data_t;
typedef void (*path_drop_func_t)(struct _lv_draw_vg_lite_unit_t *, path_drop_data_t);

/**********************
 *  STATIC PROTOTYPES
 **********************/

static void task_draw_cb(void * ctx, const lv_vector_path_t * path, const lv_vector_draw_dsc_t * dsc);
static void lv_path_to_vg(lv_vg_lite_path_t * dest, const lv_vector_path_t * src);
static vg_lite_path_type_t lv_path_opa_to_path_type(const lv_vector_draw_dsc_t * dsc);
static vg_lite_blend_t lv_blend_to_vg(lv_vector_blend_t blend);
static vg_lite_fill_t lv_fill_to_vg(lv_vector_fill_t fill_rule);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
void lv_draw_vg_lite_vector(lv_draw_task_t * t, const lv_draw_vector_task_dsc_t * dsc)
{
    if(dsc->task_list == NULL)
        return;

    lv_layer_t * layer = dsc->base.layer;
    if(layer->draw_buf == NULL)
        return;

    lv_draw_vg_lite_unit_t * u = (lv_draw_vg_lite_unit_t *)t->draw_unit;

    LV_PROFILER_DRAW_BEGIN;
    lv_vector_for_each_destroy_tasks(dsc->task_list, task_draw_cb, u);
    LV_PROFILER_DRAW_END;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static vg_lite_color_t lv_color32_to_vg(lv_color32_t color, lv_opa_t opa)
{
    uint8_t a = LV_OPA_MIX2(color.alpha, opa);
    if(a < LV_OPA_COVER) {
        color.red = LV_UDIV255(color.red * a);
        color.green = LV_UDIV255(color.green * a);
        color.blue = LV_UDIV255(color.blue * a);
    }
    return (uint32_t)a << 24 | (uint32_t)color.blue << 16 | (uint32_t)color.green << 8 | color.red;
}

static void task_draw_cb(void * ctx, const lv_vector_path_t * path, const lv_vector_draw_dsc_t * dsc)
{
    LV_PROFILER_DRAW_BEGIN;
    lv_draw_vg_lite_unit_t * u = ctx;
    LV_VG_LITE_ASSERT_DEST_BUFFER(&u->target_buffer);

    /* clear area */
    if(!path) {
        /* clear color needs to ignore fill_dsc.opa */
        vg_lite_color_t c = lv_color32_to_vg(dsc->fill_dsc.color, LV_OPA_COVER);
        vg_lite_rectangle_t rect;
        lv_vg_lite_rect(&rect, &dsc->scissor_area);
        LV_PROFILER_DRAW_BEGIN_TAG("vg_lite_clear");
        LV_VG_LITE_CHECK_ERROR(vg_lite_clear(&u->target_buffer, &rect, c), {
            lv_vg_lite_buffer_dump_info(&u->target_buffer);
            LV_LOG_ERROR("rect: X%d Y%d W%d H%d", rect.x, rect.y, rect.width, rect.height);
            lv_vg_lite_color_dump_info(c);
        });
        LV_PROFILER_DRAW_END_TAG("vg_lite_clear");
        LV_PROFILER_DRAW_END;
        return;
    }

    /* convert color */
    vg_lite_color_t vg_color = lv_color32_to_vg(dsc->fill_dsc.color, dsc->fill_dsc.opa);

    /* transform matrix */
    vg_lite_matrix_t matrix;
    lv_vg_lite_matrix(&matrix, &dsc->matrix);
    LV_VG_LITE_ASSERT_MATRIX(&matrix);

    /* convert path */
    lv_vg_lite_path_t * lv_vg_path = lv_vg_lite_path_get(u, VG_LITE_FP32);
    lv_path_to_vg(lv_vg_path, path);

    /* get path bounds */
    float min_x, min_y, max_x, max_y;
    lv_vg_lite_path_get_bounding_box(lv_vg_path, &min_x, &min_y, &max_x, &max_y);

    /* convert path type */
    vg_lite_path_type_t path_type = lv_path_opa_to_path_type(dsc);

    /* convert blend mode and fill rule */
    vg_lite_blend_t blend = lv_blend_to_vg(dsc->blend_mode);
    vg_lite_fill_t fill = lv_fill_to_vg(dsc->fill_dsc.fill_rule);

    /* set default path drop function and data */
    path_drop_func_t path_drop_func = (path_drop_func_t)lv_vg_lite_path_drop;
    path_drop_data_t path_drop_data = lv_vg_path;

    /* If it is fill mode, the end op code should be added */
    if(path_type == VG_LITE_DRAW_ZERO
       || path_type == VG_LITE_DRAW_FILL_PATH
       || path_type == VG_LITE_DRAW_FILL_STROKE_PATH) {
        lv_vg_lite_path_end(lv_vg_path);
    }

    /* convert stroke style */
    if(path_type == VG_LITE_DRAW_STROKE_PATH
       || path_type == VG_LITE_DRAW_FILL_STROKE_PATH) {
        lv_cache_entry_t * stroke_cache_entey = lv_vg_lite_stroke_get(u, lv_vg_path, &dsc->stroke_dsc);

        if(!stroke_cache_entey) {
            LV_LOG_ERROR("convert stroke failed");

            /* drop original path */
            lv_vg_lite_path_drop(u, lv_vg_path);
            LV_PROFILER_DRAW_END;
            return;
        }

        lv_vg_lite_path_t * ori_path = lv_vg_path;
        const vg_lite_path_t * ori_vg_path = lv_vg_lite_path_get_path(ori_path);

        lv_vg_lite_path_t * stroke_path = lv_vg_lite_stroke_get_path(stroke_cache_entey);
        vg_lite_path_t * vg_path = lv_vg_lite_path_get_path(stroke_path);

        /* set stroke params */
        LV_VG_LITE_CHECK_ERROR(vg_lite_set_path_type(vg_path, path_type), {});
        vg_path->stroke_color = lv_color32_to_vg(dsc->stroke_dsc.color, dsc->stroke_dsc.opa);
        vg_path->quality = ori_vg_path->quality;
        lv_memcpy(vg_path->bounding_box, ori_vg_path->bounding_box, sizeof(ori_vg_path->bounding_box));

        /* change path to stroke path */
        LV_LOG_TRACE("change path to stroke path: %p -> %p", (void *)lv_vg_path, (void *)stroke_path);
        lv_vg_path = stroke_path;
        path_drop_func = (path_drop_func_t)lv_vg_lite_stroke_drop;
        path_drop_data = stroke_cache_entey;

        /* drop original path */
        lv_vg_lite_path_drop(u, ori_path);
    }

    vg_lite_path_t * vg_path = lv_vg_lite_path_get_path(lv_vg_path);
    LV_VG_LITE_ASSERT_PATH(vg_path);

    if(vg_lite_query_feature(gcFEATURE_BIT_VG_SCISSOR)) {
        /* set scissor area */
        lv_vg_lite_set_scissor_area(&dsc->scissor_area);

        /* no bounding box */
        lv_vg_lite_path_set_bounding_box(lv_vg_path,
                                         (float)PATH_COORD_MIN, (float)PATH_COORD_MIN,
                                         (float)PATH_COORD_MAX, (float)PATH_COORD_MAX);
    }
    else {
        /* calc inverse matrix */
        vg_lite_matrix_t result;
        if(!lv_vg_lite_matrix_inverse(&result, &matrix)) {
            LV_LOG_ERROR("no inverse matrix");
            path_drop_func(u, path_drop_data);
            LV_PROFILER_DRAW_END;
            return;
        }

        /* Reverse the clip area on the source */
        lv_point_precise_t p1 = { dsc->scissor_area.x1, dsc->scissor_area.y1 };
        lv_point_precise_t p1_res = lv_vg_lite_matrix_transform_point(&result, &p1);

        /* vg-lite bounding_box will crop the pixels on the edge, so +1px is needed here */
        lv_point_precise_t p2 = { dsc->scissor_area.x2 + 1, dsc->scissor_area.y2 + 1 };
        lv_point_precise_t p2_res = lv_vg_lite_matrix_transform_point(&result, &p2);

        lv_vg_lite_path_set_bounding_box(lv_vg_path, p1_res.x, p1_res.y, p2_res.x, p2_res.y);
    }

    switch(dsc->fill_dsc.style) {
        case LV_VECTOR_DRAW_STYLE_SOLID: {
                /* normal draw shape */
                lv_vg_lite_draw(
                    &u->target_buffer,
                    vg_path,
                    fill,
                    &matrix,
                    blend,
                    vg_color);
            }
            break;
        case LV_VECTOR_DRAW_STYLE_PATTERN: {
                /* draw image */
                vg_lite_buffer_t image_buffer;
                lv_image_decoder_dsc_t decoder_dsc;
                if(lv_vg_lite_buffer_open_image(&image_buffer, &decoder_dsc, dsc->fill_dsc.img_dsc.src, false, true)) {
                    /* Calculate pattern matrix. Should start from path bond box, and also apply fill matrix. */
                    lv_matrix_t m = dsc->matrix;

                    if(dsc->fill_dsc.fill_units == LV_VECTOR_FILL_UNITS_OBJECT_BOUNDING_BOX) {
                        /* Convert to object bounding box coordinates */
                        lv_matrix_translate(&m, min_x, min_y);
                    }

                    lv_matrix_multiply(&m, &dsc->fill_dsc.matrix);

                    vg_lite_matrix_t pattern_matrix;
                    lv_vg_lite_matrix(&pattern_matrix, &m);

                    vg_lite_color_t recolor = lv_vg_lite_image_recolor(&image_buffer, &dsc->fill_dsc.img_dsc);

                    lv_vg_lite_draw_pattern(
                        &u->target_buffer,
                        vg_path,
                        fill,
                        &matrix,
                        &image_buffer,
                        &pattern_matrix,
                        blend,
                        VG_LITE_PATTERN_COLOR,
                        0,
                        recolor,
                        VG_LITE_FILTER_BI_LINEAR);

                    lv_vg_lite_pending_add(u->image_dsc_pending, &decoder_dsc);
                }
            }
            break;
        case LV_VECTOR_DRAW_STYLE_GRADIENT: {
                vg_lite_matrix_t grad_matrix = matrix;

#if LV_USE_VG_LITE_THORVG
                /* Workaround inconsistent radial gradient matrix behavior between device and ThorVG */
                if(dsc->fill_dsc.gradient.style == LV_VECTOR_GRADIENT_STYLE_RADIAL) {
                    /* Restore matrix to identity */
                    vg_lite_identity(&grad_matrix);
                }
#endif
                vg_lite_matrix_t fill_matrix;
                lv_vg_lite_matrix(&fill_matrix, &dsc->fill_dsc.matrix);
                lv_vg_lite_matrix_multiply(&grad_matrix, &fill_matrix);

                lv_vg_lite_draw_grad(
                    u,
                    &u->target_buffer,
                    vg_path,
                    &dsc->fill_dsc.gradient,
                    &grad_matrix,
                    &matrix,
                    fill,
                    blend);
            }
            break;
        default:
            LV_LOG_WARN("unknown style: %d", dsc->fill_dsc.style);
            break;
    }

    /* Flush in time to avoid accumulation of drawing commands */
    lv_vg_lite_flush(u);

    /* drop path */
    path_drop_func(u, path_drop_data);

    if(vg_lite_query_feature(gcFEATURE_BIT_VG_SCISSOR)) {
        /* disable scissor */
        lv_vg_lite_disable_scissor();
    }

    LV_PROFILER_DRAW_END;
}

static vg_lite_quality_t lv_quality_to_vg(lv_vector_path_quality_t quality)
{
    switch(quality) {
        case LV_VECTOR_PATH_QUALITY_LOW:
            return VG_LITE_LOW;
        case LV_VECTOR_PATH_QUALITY_MEDIUM:
            return VG_LITE_MEDIUM;
        case LV_VECTOR_PATH_QUALITY_HIGH:
            return VG_LITE_HIGH;
        default:
            return VG_LITE_MEDIUM;
    }
}

static void lv_path_to_vg(lv_vg_lite_path_t * dest, const lv_vector_path_t * src)
{
    LV_PROFILER_DRAW_BEGIN;
    lv_vg_lite_path_set_quality(dest, lv_quality_to_vg(src->quality));

    /* init bounds */
    float min_x = FLT_MAX;
    float min_y = FLT_MAX;
    float max_x = FLT_MIN;
    float max_y = FLT_MIN;

#define CMP_BOUNDS(point)                           \
    do {                                            \
        if((point)->x < min_x) min_x = (point)->x;  \
        if((point)->y < min_y) min_y = (point)->y;  \
        if((point)->x > max_x) max_x = (point)->x;  \
        if((point)->y > max_y) max_y = (point)->y;  \
    } while(0)

#define COPY_POINT_NEXT()        \
    do {                         \
        CMP_BOUNDS(point);       \
        *path_data++ = point->x; \
        *path_data++ = point->y; \
        point++;                 \
    } while(0)

    const lv_vector_path_op_t * ops = lv_array_front(&src->ops);
    const lv_fpoint_t * point = lv_array_front(&src->points);
    const uint32_t op_size = lv_array_size(&src->ops);
    const uint32_t point_size = lv_array_size(&src->points);
    const uint32_t path_length = (op_size + point_size * 2) * sizeof(float);

    /* Reserved memory for path data */
    lv_vg_lite_path_reserve_space(dest, path_length);
    vg_lite_path_t * vg_path = lv_vg_lite_path_get_path(dest);
    vg_path->path_length = path_length;
    float * path_data = vg_path->path;

    for(uint32_t i = 0; i < op_size; i++) {
        switch(ops[i]) {
            case LV_VECTOR_PATH_OP_MOVE_TO: {
                    LV_VG_LITE_PATH_SET_OP_CODE(path_data++, uint32_t, VLC_OP_MOVE);
                    COPY_POINT_NEXT();
                }
                break;
            case LV_VECTOR_PATH_OP_LINE_TO: {
                    LV_VG_LITE_PATH_SET_OP_CODE(path_data++, uint32_t, VLC_OP_LINE);
                    COPY_POINT_NEXT();
                }
                break;
            case LV_VECTOR_PATH_OP_QUAD_TO: {
                    LV_VG_LITE_PATH_SET_OP_CODE(path_data++, uint32_t, VLC_OP_QUAD);
                    COPY_POINT_NEXT();
                    COPY_POINT_NEXT();
                }
                break;
            case LV_VECTOR_PATH_OP_CUBIC_TO: {
                    LV_VG_LITE_PATH_SET_OP_CODE(path_data++, uint32_t, VLC_OP_CUBIC);
                    COPY_POINT_NEXT();
                    COPY_POINT_NEXT();
                    COPY_POINT_NEXT();
                }
                break;
            case LV_VECTOR_PATH_OP_CLOSE: {
                    LV_VG_LITE_PATH_SET_OP_CODE(path_data++, uint32_t, VLC_OP_CLOSE);
                }
                break;
            default:
                LV_LOG_WARN("unknown op: %d", ops[i]);
                break;
        }
    }

    LV_ASSERT_MSG((lv_uintptr_t)path_data - (lv_uintptr_t)vg_path->path == path_length, "path length overflow");

    lv_vg_lite_path_set_bounding_box(dest, lroundf(min_x), lroundf(min_y),
                                     lroundf(max_x), lroundf(max_y));
    LV_PROFILER_DRAW_END;
}

static vg_lite_path_type_t lv_path_opa_to_path_type(const lv_vector_draw_dsc_t * dsc)
{
    lv_opa_t fill_opa = dsc->fill_dsc.opa;
    lv_opa_t stroke_opa = dsc->stroke_dsc.opa;

    if(fill_opa > LV_OPA_0 && stroke_opa > LV_OPA_0) {
        return VG_LITE_DRAW_FILL_STROKE_PATH;
    }

    if(fill_opa == LV_OPA_0 && stroke_opa > LV_OPA_0) {
        return VG_LITE_DRAW_STROKE_PATH;
    }

    if(fill_opa > LV_OPA_0) {
        return VG_LITE_DRAW_FILL_PATH;
    }

    return VG_LITE_DRAW_ZERO;
}

static vg_lite_blend_t lv_blend_to_vg(lv_vector_blend_t blend)
{
    switch(blend) {
        case LV_VECTOR_BLEND_SRC_OVER:
            return VG_LITE_BLEND_SRC_OVER;
        case LV_VECTOR_BLEND_SCREEN:
            return VG_LITE_BLEND_SCREEN;
        case LV_VECTOR_BLEND_MULTIPLY:
            return VG_LITE_BLEND_MULTIPLY;
        case LV_VECTOR_BLEND_NONE:
            return VG_LITE_BLEND_NONE;
        case LV_VECTOR_BLEND_ADDITIVE:
            return VG_LITE_BLEND_ADDITIVE;
        case LV_VECTOR_BLEND_SRC_IN:
            return VG_LITE_BLEND_SRC_IN;
        case LV_VECTOR_BLEND_DST_OVER:
            return VG_LITE_BLEND_DST_OVER;
        case LV_VECTOR_BLEND_DST_IN:
            return VG_LITE_BLEND_DST_IN;
        case LV_VECTOR_BLEND_SUBTRACTIVE:
            return VG_LITE_BLEND_SUBTRACT;
        default:
            return VG_LITE_BLEND_SRC_OVER;
    }
}

static vg_lite_fill_t lv_fill_to_vg(lv_vector_fill_t fill_rule)
{
    switch(fill_rule) {
        case LV_VECTOR_FILL_NONZERO:
            return VG_LITE_FILL_NON_ZERO;
        case LV_VECTOR_FILL_EVENODD:
            return VG_LITE_FILL_EVEN_ODD;
        default:
            return VG_LITE_FILL_NON_ZERO;
    }
}

#endif /*LV_USE_DRAW_VG_LITE && LV_USE_VECTOR_GRAPHIC*/
