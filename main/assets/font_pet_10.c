/*******************************************************************************
 * Size: 10 px
 * Bpp: 4
 * Opts: --font /tmp/NotoSansSC-Bold.otf -r 0x30-0x39 --size 10 --bpp 4 --format lvgl --no-compress --lv-font-name font_pet_10 -o /workspace/passport/fw/main/assets/font_pet_10.c
 ******************************************************************************/

#include "lvgl.h"

#ifndef FONT_PET_10
#define FONT_PET_10 1
#endif

#if FONT_PET_10

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0030 "0" */
    0x5, 0xed, 0x40, 0x1f, 0x78, 0xe0, 0x6f, 0x1,
    0xf4, 0x8d, 0x0, 0xf6, 0x8d, 0x0, 0xf6, 0x6f,
    0x1, 0xf4, 0x1f, 0x78, 0xe0, 0x5, 0xed, 0x40,

    /* U+0031 "1" */
    0x6, 0xdd, 0x0, 0xa, 0xdd, 0x0, 0x0, 0x9d,
    0x0, 0x0, 0x9d, 0x0, 0x0, 0x9d, 0x0, 0x0,
    0x9d, 0x0, 0x3, 0xae, 0x31, 0x2f, 0xff, 0xf4,

    /* U+0032 "2" */
    0x1a, 0xed, 0x30, 0x5b, 0x4c, 0xd0, 0x0, 0x7,
    0xf0, 0x0, 0xa, 0xd0, 0x0, 0x3f, 0x50, 0x1,
    0xe9, 0x0, 0x1d, 0xe4, 0x41, 0x8f, 0xff, 0xf6,

    /* U+0033 "3" */
    0x1a, 0xec, 0x50, 0x29, 0x4b, 0xf0, 0x0, 0xa,
    0xe0, 0x2, 0xff, 0x30, 0x0, 0x3a, 0xe0, 0x0,
    0x3, 0xf4, 0x7b, 0x5b, 0xf2, 0x2b, 0xfd, 0x50,

    /* U+0034 "4" */
    0x0, 0x4f, 0xb0, 0x0, 0xde, 0xb0, 0x6, 0xea,
    0xb0, 0xe, 0x79, 0xb0, 0x7e, 0x9, 0xb0, 0xcf,
    0xff, 0xf9, 0x22, 0x2b, 0xc1, 0x0, 0x9, 0xb0,

    /* U+0035 "5" */
    0xf, 0xff, 0xf0, 0xf, 0x84, 0x40, 0x1f, 0x30,
    0x0, 0x2f, 0xdd, 0x60, 0x6, 0x39, 0xf2, 0x0,
    0x3, 0xf4, 0x69, 0x5b, 0xf1, 0x2b, 0xed, 0x50,

    /* U+0036 "6" */
    0x2, 0xcf, 0xb1, 0xd, 0xb4, 0x90, 0x4f, 0x10,
    0x0, 0x6e, 0xae, 0x90, 0x7f, 0x65, 0xf4, 0x5f,
    0x0, 0xe7, 0xe, 0x86, 0xf3, 0x4, 0xde, 0x70,

    /* U+0037 "7" */
    0x7f, 0xff, 0xf6, 0x24, 0x48, 0xf1, 0x0, 0xd,
    0x70, 0x0, 0x4f, 0x10, 0x0, 0xab, 0x0, 0x0,
    0xd8, 0x0, 0x0, 0xf6, 0x0, 0x1, 0xf5, 0x0,

    /* U+0038 "8" */
    0x7, 0xee, 0x60, 0x1f, 0x56, 0xf0, 0x1f, 0x42,
    0xf0, 0x9, 0xfc, 0x70, 0x1c, 0x8e, 0xc0, 0x6d,
    0x2, 0xf4, 0x6f, 0x45, 0xf4, 0x9, 0xee, 0x80,

    /* U+0039 "9" */
    0x9, 0xec, 0x30, 0x5f, 0x59, 0xd0, 0x8d, 0x1,
    0xf3, 0x7f, 0x14, 0xf5, 0x1d, 0xfb, 0xf4, 0x0,
    0x12, 0xf2, 0x19, 0x5c, 0xc0, 0x2b, 0xfb, 0x10
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 94, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 24, .adv_w = 94, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 48, .adv_w = 94, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 72, .adv_w = 94, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 96, .adv_w = 94, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 120, .adv_w = 94, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 144, .adv_w = 94, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 168, .adv_w = 94, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 192, .adv_w = 94, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 216, .adv_w = 94, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 48, .range_length = 10, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t font_pet_10 = {
#else
lv_font_t font_pet_10 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 8,          /*The maximum line height required by the font*/
    .base_line = 0,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if FONT_PET_10*/

