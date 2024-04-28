/* Copyright (C) 2024 Nunuhara Cabbage <nunuhara@haniwa.technology>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://gnu.org/licenses/>.
 */

#include "nulib.h"
#include "ai5/anim.h"
#include "ai5/mes.h"

#include "anim.h"
#include "audio.h"
#include "cursor.h"
#include "game.h"
#include "gfx_private.h"
#include "input.h"
#include "savedata.h"
#include "sys.h"
#include "util.h"
#include "vm_private.h"

#define VAR4_SIZE 2048
#define MEM16_SIZE 4096

static void ai_shimai_mem_restore(void)
{
	mem_set_sysvar16_ptr(MEMORY_MES_NAME_SIZE + VAR4_SIZE + 56);
	mem_set_sysvar32(mes_sysvar32_memory, offsetof(struct memory, mem16));
	mem_set_sysvar32(mes_sysvar32_file_data, offsetof(struct memory, file_data));
	mem_set_sysvar32(mes_sysvar32_menu_entry_addresses,
			offsetof(struct memory, menu_entry_addresses));
	mem_set_sysvar32(mes_sysvar32_menu_entry_numbers,
			offsetof(struct memory, menu_entry_numbers));
	mem_set_sysvar32(mes_sysvar32_map_offset, 0);

	uint16_t flags = mem_get_sysvar16(mes_sysvar16_flags);
	mem_set_sysvar16(mes_sysvar16_flags, (flags & 0xffbf) | 0x21);
	mem_set_sysvar16(0, 2632);
}

static void ai_shimai_mem_init(void)
{
	// set up pointer table for memory access
	// (needed because var4 size changes per game)
	uint32_t off = MEMORY_MES_NAME_SIZE + VAR4_SIZE;
	memory_ptr.system_var16_ptr = memory_raw + off;
	memory_ptr.var16 = memory_raw + off + 4;
	memory_ptr.system_var16 = memory_raw + off + 56;
	memory_ptr.var32 = memory_raw + off + 104;
	memory_ptr.system_var32 = memory_raw + off + 208;

	mem_set_sysvar16(mes_sysvar16_flags, 0x60f);
	mem_set_sysvar16(mes_sysvar16_text_start_x, 0);
	mem_set_sysvar16(mes_sysvar16_text_start_y, 0);
	mem_set_sysvar16(mes_sysvar16_text_end_x, 640);
	mem_set_sysvar16(mes_sysvar16_text_end_y, 480);
	mem_set_sysvar16(mes_sysvar16_font_width, 16);
	mem_set_sysvar16(mes_sysvar16_font_height, 16);
	mem_set_sysvar16(mes_sysvar16_char_space, 16);
	mem_set_sysvar16(mes_sysvar16_line_space, 16);
	mem_set_sysvar16(mes_sysvar16_mask_color, 0);

	mem_set_sysvar32(mes_sysvar32_cg_offset, 0x20000);
	mem_set_sysvar32(11, 0);
	ai_shimai_mem_restore();
}

// Text Variables:
// ---------------
//
// var4[2001] controls whether "separate"-rendered text is merged in
// System.function[22].function[1]
//   * 1 -> text is merged
//   * !1 -> text is not merged
//
// var4[2002] selects the font.
//   * 0 -> FONT.FNT
//   * 1 -> SELECT1.FNT
//   * 2 -> SELECT2.FNT
//   * 3 -> SELECT3.FNT
//
// (Note that SELECT fonts always use the "merged" rendering mode.)
//
// var4[2017] controls whether the "merged" or "separate" rendering mode is used.
//   * 0 -> use "separate" rendering mode to surface 7
//   * !0 -> use "merged" rendering mode to System.dst_surface
//
// var4[2018] controls whether text is greyscale or redscale
//   * 0 -> greyscale
//   * !0 -> redscale

// XXX: many functions below assume pixel format
_Static_assert(GFX_DIRECT_FORMAT == SDL_PIXELFORMAT_RGB24);

/*
 * Get character index from table.
 */
static int get_char_index(uint16_t ch, uint8_t *table)
{
	uint16_t size = le_get16(table, 0);
	for (unsigned i = 0; i < size; i++) {
		if (le_get16(table, (i + 1) * 2) == ch)
			return i;
	}
	return -1;
}

/*
 * Blend monochrome color data with an RGB24 pixel at a given alpha level.
 */
static void alpha_blend_rgb_mono(uint8_t *bg, uint8_t fg, uint8_t alpha)
{
	uint32_t a = (uint32_t)alpha + 1;
	uint32_t inv_a = 256 - (uint32_t)alpha;
	bg[0] = (uint8_t)((a * fg + inv_a * bg[0]) >> 8);
	bg[1] = (uint8_t)((a * fg + inv_a * bg[1]) >> 8);
	bg[2] = (uint8_t)((a * fg + inv_a * bg[2]) >> 8);
}

/*
 * Blend a BGR24 pixel with an RGB24 pixel at a given alpha level.
 */
static void alpha_blend_rgb_bgr(uint8_t *bg, uint8_t *fg, uint8_t alpha)
{
	uint32_t a = (uint32_t)alpha + 1;
	uint32_t inv_a = 256 - (uint32_t)alpha;
	bg[0] = (uint8_t)((a * fg[2] + inv_a * bg[0]) >> 8);
	bg[1] = (uint8_t)((a * fg[1] + inv_a * bg[1]) >> 8);
	bg[2] = (uint8_t)((a * fg[0] + inv_a * bg[2]) >> 8);
}

/*
 * This is the simple rendering mode, in which the mask and greyscale color data are
 * merged and written directly to a surface.
 */
static void render_char_merged(uint8_t *dst_in, uint8_t *fnt_in, uint8_t *msk_in,
		uint8_t *pal, int char_w, int char_h, int stride)
{
	for (int row = 0; row < char_h; row++) {
		uint8_t *fnt = fnt_in + char_w * row;
		uint8_t *msk = msk_in + char_w * row;
		uint8_t *dst = dst_in + row * stride;
		for (int col = 0; col < char_w; col++, fnt++, msk++, dst += 3) {
			if (*msk == 0)
				continue;
			if (pal) {
				uint8_t alpha = (min(*msk, 15) * 16) - 8;
				uint8_t *c = pal + *fnt * 3;
				alpha_blend_rgb_bgr(dst, c, alpha);
			} else if (*msk > 15) {
				dst[0] = *fnt;
				dst[1] = *fnt;
				dst[2] = *fnt;
			} else {
				uint8_t alpha = (min(*msk, 15) * 16) - 8;
				alpha_blend_rgb_mono(dst, *fnt, alpha);
			}
		}
	}
}

/*
 * "Redscale" rendering mode. This mode is like the "merged" mode, except that only the
 * red channel is blended. The green and blue channels are set to zero whenever the
 * mask is non-zero.
 */
static void render_char_redscale(uint8_t *dst_in, uint8_t *fnt_in, uint8_t *msk_in,
		uint8_t *pal, int char_w, int char_h, int stride)
{
	for (int row = 0; row < char_h; row++) {
		uint8_t *fnt = fnt_in + char_w * row;
		uint8_t *msk = msk_in + char_w * row;
		uint8_t *dst = dst_in + row * stride;
		for (int col = 0; col < char_w; col++, fnt++, msk++, dst += 3) {
			if (*msk == 0)
				continue;
			if (*msk > 15) {
				dst[0] = *fnt;
			} else {
				uint8_t alpha = (min(*msk, 15) * 16) - 8;
				alpha_blend_rgb_mono(dst, *fnt, alpha);
			}
			dst[1] = 0;
			dst[2] = 0;
		}
	}
}

/*
 * In this rendering mode, the greyscale color data is written at the text cursor, and
 * the mask data is written 256 lines below the cursor. Merging the two is a separate
 * operation.
 */
static void render_char_separate(uint8_t *dst_in, uint8_t *fnt_in, uint8_t *msk_in,
		uint8_t *pal, int char_w, int char_h, int stride)
{
	for (int row = 0; row < char_h; row++) {
		uint8_t *fnt = fnt_in + char_w * row;
		uint8_t *msk = msk_in + char_w * row;
		uint8_t *fnt_dst = dst_in + row * stride;
		uint8_t *msk_dst = dst_in + (row + 256) * stride;
		for (int col = 0; col < char_w; col++, fnt++, msk++, fnt_dst += 3, msk_dst += 3) {
			if (*fnt) {
				fnt_dst[0] = *fnt;
				fnt_dst[1] = *fnt;
				fnt_dst[2] = *fnt;
			}
			if (*msk) {
				msk_dst[0] = *msk;
				msk_dst[1] = *msk;
				msk_dst[2] = *msk;
			}
		}
	}
}

struct render_text_params {
	int char_w, char_h;
	unsigned surface;
	void (*render_char)(uint8_t*,uint8_t*,uint8_t*,uint8_t*,int,int,int);
	uint8_t *font_tbl;
	uint8_t *font_msk;
	uint8_t *font_fnt;
	uint8_t *font_pal;
};

/*
 * Render a string according to the given parameters.
 */
static void render_text(const char *txt, struct render_text_params *p)
{
	const uint16_t start_x = mem_get_sysvar16(mes_sysvar16_text_start_x);
	const uint16_t end_x = mem_get_sysvar16(mes_sysvar16_text_end_x);
	const uint16_t char_space = mem_get_sysvar16(mes_sysvar16_char_space);
	const uint16_t line_space = mem_get_sysvar16(mes_sysvar16_line_space);
	uint16_t x = mem_get_sysvar16(mes_sysvar16_text_cursor_x);
	uint16_t y = mem_get_sysvar16(mes_sysvar16_text_cursor_y);

	SDL_Surface *surf = gfx_get_surface(p->surface);
	if (SDL_MUSTLOCK(surf))
		SDL_CALL(SDL_LockSurface, surf);

	for (; *txt; txt += 2) {
		// get index of char in font data
		uint16_t char_code = le_get16((uint8_t*)txt, 0);
		int char_i = get_char_index(char_code, p->font_tbl);
		if (char_i < 0) {
			WARNING("Invalid character: %04x", char_code);
			continue;
		}

		uint8_t *char_msk = p->font_msk + (char_i * p->char_w * p->char_h);
		uint8_t *char_fnt = p->font_fnt + (char_i * p->char_w * p->char_h);
		uint8_t *dst = surf->pixels + y * surf->pitch + x * 3;
		p->render_char(dst, char_fnt, char_msk, p->font_pal, p->char_w, p->char_h, surf->pitch);

		x += char_space;
		if (x + char_space > end_x) {
			y += line_space;
			x = start_x;
		}
	}

	mem_set_sysvar16(mes_sysvar16_text_cursor_x, x);
	mem_set_sysvar16(mes_sysvar16_text_cursor_y, y);

	if (SDL_MUSTLOCK(surf))
		SDL_UnlockSurface(surf);

	gfx_dirty(p->surface);
}

/*
 * Render a string using one of the SELECT fonts.
 */
static void render_text_select(const char *txt)
{
	int sel = mem_get_var4(2002);
	if (sel < 1 || sel > 3) {
		WARNING("Invalid SELECT font index: %d", sel);
		return;
	}
	struct render_text_params p = {
		.char_w = sel == 2 ? 49 : 47,
		.char_h = sel == 2 ? 49 : 47,
		.surface = mem_get_sysvar16(mes_sysvar16_dst_surface),
		.render_char = render_char_merged,
		.font_tbl = memory.file_data + mem_get_var32(3),
		.font_msk = memory.file_data + mem_get_var32(5 + (sel - 1) * 3),
		.font_fnt = memory.file_data + mem_get_var32(6 + (sel - 1) * 3),
		.font_pal = memory.file_data + mem_get_var32(4 + (sel - 1) * 3)
	};
	render_text(txt, &p);
}

/*
 * Custom TXT function.
 */
static void ai_shimai_TXT(const char *txt)
{
	if (mem_get_var4(2002) != 0) {
		render_text_select(txt);
		return;
	}

	bool render_merged = mem_get_var4(2017) != 0;
	bool render_redscale = mem_get_var4(2018) != 0;
	struct render_text_params p = {
		.char_w = 28,
		.char_h = 28,
		.surface = render_merged ? mem_get_sysvar16(mes_sysvar16_dst_surface) : 7,
		.render_char = render_redscale ? render_char_redscale :
				render_merged ? render_char_merged :
				render_char_separate,
		.font_tbl = memory.file_data + mem_get_var32(0),
		.font_msk = memory.file_data + mem_get_var32(1),
		.font_fnt = memory.file_data + mem_get_var32(2),
		.font_pal = NULL,
	};
	render_text(txt, &p);
}

static void ai_shimai_sys_cursor(struct param_list *params)
{
	static uint32_t uk = 0;
	switch (vm_expr_param(params, 0)) {
	case 0: cursor_show(); break;
	case 1: cursor_hide(); break;
	case 2: sys_cursor_save_pos(params); break;
	case 3: cursor_set_pos(vm_expr_param(params, 1), vm_expr_param(params, 2)); break;
	case 4: cursor_load(vm_expr_param(params, 1) + 15); break;
	case 5: uk = 0; break;
	case 6: mem_set_var16(18, 0); break;
	case 7: mem_set_var32(18, uk); break;
	case 8: uk = vm_expr_param(params, 1); break;
	default: VM_ERROR("System.Cursor.function[%u] not implemented",
				 params->params[0].val);
	}
}

static unsigned vm_anim_param(struct param_list *params, unsigned i)
{
	unsigned a = vm_expr_param(params, i);
	unsigned b = vm_expr_param(params, i+1);
	unsigned stream = a * 10 + b;
	if (a >= ANIM_MAX_STREAMS)
		VM_ERROR("Invalid animation stream index: %u:%u", a, b);
	return stream;
}

static void ai_shimai_sys_anim(struct param_list *params)
{
	switch (vm_expr_param(params, 0)) {
	case 0: anim_init_stream(vm_anim_param(params, 1), vm_anim_param(params, 1)); break;
	case 1: anim_start(vm_anim_param(params, 1)); break;
	case 2: anim_stop(vm_anim_param(params, 1)); break;
	case 3: anim_halt(vm_anim_param(params, 1)); break;
	case 4: anim_wait(vm_anim_param(params, 1)); break;
	case 5: anim_stop_all(); break;
	case 6: anim_halt_all(); break;
	case 7: anim_reset_all(); break;
	case 8: anim_exec_copy_call(vm_anim_param(params, 1)); break;
	default: VM_ERROR("System.Anim.function[%u] not implemented",
				 params->params[0].val);
	}
}

static void ai_shimai_sys_savedata(struct param_list *params)
{
	char save_name[7];
	uint32_t save_no = vm_expr_param(params, 1);
	if (save_no > 99)
		VM_ERROR("Invalid save number: %u", save_no);
	sprintf(save_name, "FLAG%02u", save_no);

	switch (vm_expr_param(params, 0)) {
	case 0: savedata_resume_load(save_name); break;
	case 1: savedata_resume_save(save_name); break;
	case 2: savedata_load_var4(save_name); break;
	case 3: savedata_save_union_var4(save_name); break;
	//case 4: savedata_load_extra_var32(save_name); break;
	//case 5: savedata_save_extra_var32(save_name); break;
	//case 6: savedata_clear_var4(save_name); break;
	//case 7: savedata_load_heap(save_name); break;
	//case 8: savedata_save_heap(save_name); break;
	default: VM_ERROR("System.SaveData.function[%u] not implemented",
				 params->params[0].val);
	}
}

static void ai_shimai_sys_audio(struct param_list *params)
{
	switch (vm_expr_param(params, 0)) {
	case 0: audio_bgm_play(vm_string_param(params, 1), true); break;
	case 1: audio_bgm_stop(); break;
	case 2: audio_bgm_fade(0, 2000, true, false); break;
	case 6: audio_aux_play(vm_string_param(params, 1), vm_expr_param(params, 2)); break;
	case 7: audio_aux_stop(vm_expr_param(params, 1)); break;
	default: VM_ERROR("System.Audio.function[%u] not implemented",
				 params->params[0].val);
	}
}

static void ai_shimai_sys_voice(struct param_list *params)
{
	if (!vm_flag_is_on(FLAG_VOICE_ENABLE))
		return;
	switch (vm_expr_param(params, 0)) {
	case 0: audio_voice_play(vm_string_param(params, 1)); break;
	case 1: audio_voice_stop(); break;
	default: WARNING("System.Voice.function[%u] not implemented",
				 params->params[0].val);
	}
}

static void ai_shimai_sys_load_image(struct param_list *params)
{
	anim_halt_all();
	sys_load_image(params);
}

static void ai_shimai_sys_display(struct param_list *params)
{
	switch (vm_expr_param(params, 0)) {
	case 0:
		if (params->nr_params > 1) {
			// FIXME: use fill color
			gfx_display_hide();
		} else {
			gfx_display_unhide();
		}
		break;
	case 1:
		if (params->nr_params > 1) {
			gfx_display_fade_out(vm_expr_param(params, 1));
		} else {
			gfx_display_fade_in();
		}
		break;
	default: VM_ERROR("System.Display.function[%u] not implemented",
				 params->params[0].val);
	}
}

static void ai_shimai_sys_graphics(struct param_list *params)
{
	switch (vm_expr_param(params, 0)) {
	case 0: sys_graphics_copy(params); break;
	case 1: sys_graphics_copy_masked24(params); break;
	case 2: sys_graphics_fill_bg(params); break;
	case 4: sys_graphics_swap_bg_fg(params); break;
	case 6: sys_graphics_blend(params); break;
	case 7: sys_graphics_blend_masked(params); break;
	default: VM_ERROR("System.Graphics.function[%u] not implemented",
				 params->params[0].val);
	}
}

static void sys_19(struct param_list *params)
{
	WARNING("System.function[19] not implemented");
}

static void update_text(struct param_list *params)
{
	if (mem_get_var4(2001) != 1)
		return;

	SDL_Surface *src = gfx_get_surface(7);
	SDL_Surface *dst = gfx_get_overlay();
	if (SDL_MUSTLOCK(src))
		SDL_CALL(SDL_LockSurface, src);
	if (SDL_MUSTLOCK(dst))
		SDL_CALL(SDL_LockSurface, dst);

	// clear overlay
	SDL_Rect r = { 0, 336, 640, 128 };
	SDL_CALL(SDL_FillRect, dst, &r, SDL_MapRGBA(dst->format, 0, 0, 0, 0));

	// merge color/mask from surface 7 and write to overlay surface
	// color data is at (0,   0) -> (640, 128) on surface 7
	// mask data is at  (0, 256) -> (640, 384) on surface 7
	// destination is   (0, 336) -> (640, 464) on overlay
	for (int row = 0; row < 128; row++) {
		uint8_t *fnt = src->pixels + row * src->pitch;
		uint8_t *msk = src->pixels + (row + 256) * src->pitch;
		uint8_t *p = dst->pixels + (row + 336) * dst->pitch;
		for (int col = 0; col < 640; col++, fnt += 3, msk += 3, p += 4) {
			// XXX: only blue channel matters for mask
			if (msk[2] == 0)
				continue;
			// ???: do all channels get copied here?
			p[0] = fnt[0];
			p[1] = fnt[1];
			p[2] = fnt[2];
			if (msk[2] > 15) {
				p[3] = 255;
			} else {
				p[3] = *msk * 16 - 8;
			}
		}
	}

	if (SDL_MUSTLOCK(src))
		SDL_UnlockSurface(src);
	if (SDL_MUSTLOCK(dst))
		SDL_UnlockSurface(dst);

	// TODO: mark (x,y,w,h) as dirty
	//int x = vm_expr_param(params, 2);
	//int y = vm_expr_param(params, 3);
	//int w = vm_expr_param(params, 4);
	//int h = vm_expr_param(params, 5);
	gfx_screen_dirty();
}

static void sys_22(struct param_list *params)
{
	switch (vm_expr_param(params, 0)) {
	case 1:  update_text(params); break;
	default: WARNING("System.function[22].function[%u] not implemented",
				 params->params[0].val);
	}
}

static void util_7(struct param_list *params)
{
	WARNING("Util.function[7] not implemented");
}

static void util_11(struct param_list *params)
{
	mem_set_var32(18, 0);
}

static void util_12(struct param_list *params)
{
	WARNING("Util.function[12] not implemented");
}

static void util_15(struct param_list *params)
{
	WARNING("Util.function[15] not implemented");
}

static void util_16(struct param_list *params)
{
	mem_set_var32(18, 1);
}

struct game game_ai_shimai = {
	.surface_sizes = {
		{ 640, 480 },
		{ 640, 1280 },
		{ 640, 480 },
		{ 640, 480 },
		{ 640, 480 },
		{ 640, 480 },
		{ 640, 480 },
		{ 640, 512 },
		{ 864, 468 },
		{ 720, 680 },
		{ 640, 480 },
		{ 0, 0 }
	},
	.bpp = 24,
	.x_mult = 1,
	.use_effect_arc = false,
	.persistent_volume = false,
	.call_saves_procedures = false,
	.proc_clears_flag = true,
	.var4_size = VAR4_SIZE,
	.mem16_size = MEM16_SIZE,
	.handle_event = NULL,
	.mem_init = ai_shimai_mem_init,
	.mem_restore = ai_shimai_mem_restore,
	.custom_TXT = ai_shimai_TXT,
	.sys = {
		[0]  = sys_set_font_size,
		[1]  = sys_display_number,
		[2]  = ai_shimai_sys_cursor,
		[3]  = ai_shimai_sys_anim,
		[4]  = ai_shimai_sys_savedata,
		[5]  = ai_shimai_sys_audio,
		[6]  = ai_shimai_sys_voice,
		[7]  = sys_file,
		[8]  = ai_shimai_sys_load_image,
		[9]  = ai_shimai_sys_display,
		[10] = ai_shimai_sys_graphics,
		[11] = sys_wait,
		[12] = sys_set_text_colors_direct,
		[13] = sys_farcall,
		[14] = sys_get_cursor_segment,
		[15] = sys_menu_get_no,
		//[16] = sys_get_time,
		[17] = NULL,
		[18] = sys_check_input,
		[19] = sys_19,
		[20] = NULL,
		[21] = sys_strlen,
		[22] = sys_22,
		//[23] = TODO
	},
	.util = {
		[7] = util_7,
		[11] = util_11,
		[12] = util_12,
		[15] = util_15,
		[16] = util_16,
	},
	.flags = {
		[FLAG_ANIM_ENABLE]  = 0x0004,
		[FLAG_MENU_RETURN]  = 0x0008,
		[FLAG_RETURN]       = 0x0010,
		[FLAG_VOICE_ENABLE] = 0x0100,
	}
};
