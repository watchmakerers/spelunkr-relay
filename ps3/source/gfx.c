#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>

#include <rsx/rsx.h>
#include <sysutil/video.h>

#include "gfx.h"

#include "quad_vp_vpo.h"
#include "yuv_fp_fpo.h"
#include "rgba_fp_fpo.h"

#define CB_SIZE        0x100000      // command buffer
#define HOST_SIZE      (8*1024*1024)  // host memory mapped for the RSX (holds the command buffer)
#define FB_COUNT       2
#define MAX_VW         1024
#define MAX_VH         576

u32 gfx_width, gfx_height;
float gfx_aspect = 16.0f / 9.0f;

static gcmContextData *ctx;
static videoResolution res;
static u32 color_pitch, color_offset[FB_COUNT], depth_pitch, depth_offset;
static u32 *color_buffer[FB_COUNT], *depth_buffer;
static u32 cur_fb = 0;
static int first_flip = 1;

typedef struct {
	rsxVertexProgram *vp; void *vp_ucode;
	rsxFragmentProgram *fp; u32 fp_offset;
	rsxProgramAttrib *pos, *tex;
} program_t;

static program_t yuv_prog, rgba_prog;
static rsxProgramAttrib *u_y, *u_cb, *u_cr, *u_tex;

// Video: two sets of three planes, so the CPU writes one while the GPU reads the other.
typedef struct { u8 *y, *cb, *cr; u32 oy, ocb, ocr; } vset_t;
static vset_t vsets[2];
static int vset_cur = -1;         // set the GPU should draw, -1 = none
static u32 vw, vh;                // current frame size
#define VPITCH_Y  MAX_VW
#define VPITCH_C  (MAX_VW / 2)

// Overlay: canvas in main memory, two ARGB textures in RSX memory.
static u32 *canvas;
static u32 *otex[2];
static u32 ooff[2];
static int oset_cur = -1;

static void load_program(program_t *p, const void *vpo, const void *fpo)
{
	void *ucode; u32 size;
	p->vp = (rsxVertexProgram *)vpo;
	p->fp = (rsxFragmentProgram *)fpo;
	rsxVertexProgramGetUCode(p->vp, &p->vp_ucode, &size);
	rsxFragmentProgramGetUCode(p->fp, &ucode, &size);
	void *fp_mem = rsxMemalign(64, size);
	memcpy(fp_mem, ucode, size);
	rsxAddressToOffset(fp_mem, &p->fp_offset);
	p->pos = rsxVertexProgramGetAttrib(p->vp, "position");
	p->tex = rsxVertexProgramGetAttrib(p->vp, "texcoord");
}

static void set_render_target(u32 i)
{
	gcmSurface sf;
	memset(&sf, 0, sizeof(sf));
	sf.colorFormat = GCM_SURFACE_X8R8G8B8;
	sf.colorTarget = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0] = color_offset[i];
	sf.colorPitch[0] = color_pitch;
	sf.colorLocation[1] = sf.colorLocation[2] = sf.colorLocation[3] = GCM_LOCATION_RSX;
	sf.colorPitch[1] = sf.colorPitch[2] = sf.colorPitch[3] = 64;
	sf.depthFormat = GCM_SURFACE_ZETA_Z24S8;
	sf.depthLocation = GCM_LOCATION_RSX;
	sf.depthOffset = depth_offset;
	sf.depthPitch = depth_pitch;
	sf.type = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias = GCM_SURFACE_CENTER_1;
	sf.width = gfx_width;
	sf.height = gfx_height;
	rsxSetSurface(ctx, &sf);
}

static int configure_video(void)
{
	static const u32 ids[] = { VIDEO_RESOLUTION_1080, VIDEO_RESOLUTION_720, VIDEO_RESOLUTION_480, VIDEO_RESOLUTION_576 };
	s32 rid = -1;
	// Prefer 720p: the guide is designed for it and the video is SD anyway.
	static const u32 order[] = { 1, 0, 2, 3 };
	for (u32 k = 0; k < 4; k++) {
		u32 id = ids[order[k]];
		if (videoGetResolutionAvailability(VIDEO_PRIMARY, id, VIDEO_ASPECT_AUTO, 0) != 1) continue;
		if (videoGetResolution(id, &res) == 0) { rid = id; break; }
	}
	if (rid < 0) return -1;

	videoConfiguration cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.resolution = (u8)rid;
	cfg.format = VIDEO_BUFFER_FORMAT_XRGB;
	cfg.aspect = VIDEO_ASPECT_AUTO;
	cfg.pitch = res.width * 4;
	if (videoConfigure(VIDEO_PRIMARY, &cfg, NULL, 0) != 0) return -1;

	videoState st;
	if (videoGetState(VIDEO_PRIMARY, 0, &st) == 0 && st.displayMode.aspect == VIDEO_ASPECT_4_3) gfx_aspect = 4.0f / 3.0f;
	gfx_width = res.width;
	gfx_height = res.height;
	return 0;
}

static void wait_idle(void)
{
	static u32 label = 1;
	rsxSetWriteBackendLabel(ctx, 255, label);
	rsxFlushBuffer(ctx);
	while (*(vu32 *)gcmGetLabelAddress(255) != label) usleep(30);
	label++;
}

int gfx_init(void)
{
	void *host = memalign(1024 * 1024, HOST_SIZE);
	if (!host) return -1;
	if (rsxInit(&ctx, CB_SIZE, HOST_SIZE, host) != 0) return -1;
	if (configure_video() != 0) return -1;
	wait_idle();
	gcmSetFlipMode(GCM_FLIP_VSYNC);

	color_pitch = gfx_width * 4;
	depth_pitch = gfx_width * 4;
	for (u32 i = 0; i < FB_COUNT; i++) {
		color_buffer[i] = (u32 *)rsxMemalign(64, gfx_height * color_pitch);
		rsxAddressToOffset(color_buffer[i], &color_offset[i]);
		gcmSetDisplayBuffer(i, color_offset[i], color_pitch, gfx_width, gfx_height);
	}
	depth_buffer = (u32 *)rsxMemalign(64, gfx_height * depth_pitch);
	rsxAddressToOffset(depth_buffer, &depth_offset);

	load_program(&yuv_prog, quad_vp_vpo, yuv_fp_fpo);
	load_program(&rgba_prog, quad_vp_vpo, rgba_fp_fpo);
	u_y = rsxFragmentProgramGetAttrib(yuv_prog.fp, "texY");
	u_cb = rsxFragmentProgramGetAttrib(yuv_prog.fp, "texCb");
	u_cr = rsxFragmentProgramGetAttrib(yuv_prog.fp, "texCr");
	u_tex = rsxFragmentProgramGetAttrib(rgba_prog.fp, "tex");

	for (int i = 0; i < 2; i++) {
		vsets[i].y = rsxMemalign(128, VPITCH_Y * MAX_VH);
		vsets[i].cb = rsxMemalign(128, VPITCH_C * (MAX_VH / 2));
		vsets[i].cr = rsxMemalign(128, VPITCH_C * (MAX_VH / 2));
		rsxAddressToOffset(vsets[i].y, &vsets[i].oy);
		rsxAddressToOffset(vsets[i].cb, &vsets[i].ocb);
		rsxAddressToOffset(vsets[i].cr, &vsets[i].ocr);
		otex[i] = rsxMemalign(128, UI_W * UI_H * 4);
		rsxAddressToOffset(otex[i], &ooff[i]);
	}
	canvas = memalign(128, UI_W * UI_H * 4);
	memset(canvas, 0, UI_W * UI_H * 4);
	if (!canvas || !vsets[1].cr || !otex[1]) return -1;

	set_render_target(cur_fb);
	return 0;
}

void gfx_shutdown(void)
{
	gcmSetWaitFlip(ctx);
	wait_idle();
}

void gfx_video_upload(const u8 *y, u32 ystride, const u8 *cb, const u8 *cr, u32 cstride, u32 w, u32 h)
{
	if (w > MAX_VW) w = MAX_VW;
	if (h > MAX_VH) h = MAX_VH;
	int next = vset_cur < 0 ? 0 : vset_cur ^ 1;
	vset_t *s = &vsets[next];
	for (u32 r = 0; r < h; r++) memcpy(s->y + r * VPITCH_Y, y + r * ystride, w);
	for (u32 r = 0; r < h / 2; r++) {
		memcpy(s->cb + r * VPITCH_C, cb + r * cstride, w / 2);
		memcpy(s->cr + r * VPITCH_C, cr + r * cstride, w / 2);
	}
	vw = w; vh = h;
	vset_cur = next;
}

void gfx_video_clear(void) { vset_cur = -1; }

u32 *gfx_overlay_canvas(void) { return canvas; }

void gfx_overlay_commit(void)
{
	int next = oset_cur < 0 ? 0 : oset_cur ^ 1;
	memcpy(otex[next], canvas, UI_W * UI_H * 4);
	oset_cur = next;
}

static void bind_texture(rsxProgramAttrib *unit, u32 offset, u32 w, u32 h, u32 pitch, int single_channel)
{
	gcmTexture t;
	memset(&t, 0, sizeof(t));
	if (single_channel) {
		// 8-bit plane: replicate the one channel (B) into all four.
		t.format = GCM_TEXTURE_FORMAT_B8 | GCM_TEXTURE_FORMAT_LIN;
		t.remap = GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT |
		          GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT |
		          GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT |
		          GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT |
		          GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT |
		          GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_G_SHIFT |
		          GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_R_SHIFT |
		          GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_A_SHIFT;
	} else {
		t.format = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN;
		t.remap = GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT |
		          GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT |
		          GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT |
		          GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT |
		          GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT |
		          GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT |
		          GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT |
		          GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT;
	}
	t.mipmap = 1;
	t.dimension = GCM_TEXTURE_DIMS_2D;
	t.cubemap = GCM_FALSE;
	t.width = w;
	t.height = h;
	t.depth = 1;
	t.location = GCM_LOCATION_RSX;
	t.pitch = pitch;
	t.offset = offset;
	rsxLoadTexture(ctx, unit->index, &t);
	rsxTextureControl(ctx, unit->index, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
	rsxTextureFilter(ctx, unit->index, 0, GCM_TEXTURE_LINEAR, GCM_TEXTURE_LINEAR, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	rsxTextureWrapMode(ctx, unit->index, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE,
	                   GCM_TEXTURE_UNSIGNED_REMAP_NORMAL, GCM_TEXTURE_ZFUNC_LESS, 0);
}

// Draws a quad covering UI rect (x,y,w,h) with texture coords (0,0)-(u1,v1).
static void quad(program_t *p, float x, float y, float w, float h, float u1, float v1)
{
	float x0 = x / UI_W * 2.0f - 1.0f, x1 = (x + w) / UI_W * 2.0f - 1.0f;
	float y0 = 1.0f - y / UI_H * 2.0f, y1 = 1.0f - (y + h) / UI_H * 2.0f;
	const float pos[4][4] = { { x0, y0, 0, 1 }, { x1, y0, 0, 1 }, { x1, y1, 0, 1 }, { x0, y1, 0, 1 } };
	const float uv[4][2] = { { 0, 0 }, { u1, 0 }, { u1, v1 }, { 0, v1 } };
	rsxDrawVertexBegin(ctx, GCM_TYPE_QUADS);
	for (int i = 0; i < 4; i++) {
		rsxDrawVertex2f(ctx, p->tex->index, uv[i]);
		rsxDrawVertex4f(ctx, p->pos->index, pos[i]);
	}
	rsxDrawVertexEnd(ctx);
}

static void use_program(program_t *p)
{
	rsxLoadVertexProgram(ctx, p->vp, p->vp_ucode);
	rsxLoadFragmentProgramLocation(ctx, p->fp, p->fp_offset, GCM_LOCATION_RSX);
}

static void flip(void)
{
	if (!first_flip) {
		while (gcmGetFlipStatus() != 0) usleep(200);
	}
	gcmResetFlipStatus();
	gcmSetFlip(ctx, cur_fb);
	rsxFlushBuffer(ctx);
	gcmSetWaitFlip(ctx);
	cur_fb ^= 1;
	set_render_target(cur_fb);
	first_flip = 0;
}

void gfx_frame(float vx, float vy, float vw_ui, float vh_ui, int show_video)
{
	float scale[4] = { gfx_width * 0.5f, gfx_height * -0.5f, 0.5f, 0.0f };
	float offset[4] = { gfx_width * 0.5f, gfx_height * 0.5f, 0.5f, 0.0f };

	rsxSetColorMask(ctx, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
	rsxSetColorMaskMrt(ctx, 0);
	rsxSetViewport(ctx, 0, 0, gfx_width, gfx_height, 0.0f, 1.0f, scale, offset);
	rsxSetScissor(ctx, 0, 0, gfx_width, gfx_height);
	rsxSetDepthTestEnable(ctx, GCM_FALSE);
	rsxSetCullFaceEnable(ctx, GCM_FALSE);
	rsxSetClearColor(ctx, 0xff000000);
	rsxClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
	rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);

	if (show_video && vset_cur >= 0 && vw && vh) {
		// Fit the frame into the rect keeping its shape. UI units are square pixels on a
		// 16:9 screen; correct for 4:3 displays.
		float frame_aspect = (float)vw / (float)vh;
		float rect_aspect = (vw_ui / vh_ui) * (gfx_aspect / (16.0f / 9.0f));
		float w = vw_ui, h = vh_ui;
		if (frame_aspect > rect_aspect) h = vh_ui * rect_aspect / frame_aspect;
		else w = vw_ui * frame_aspect / rect_aspect;
		vset_t *s = &vsets[vset_cur];
		rsxSetBlendEnable(ctx, GCM_FALSE);
		use_program(&yuv_prog);
		bind_texture(u_y, s->oy, VPITCH_Y, MAX_VH, VPITCH_Y, 1);
		bind_texture(u_cb, s->ocb, VPITCH_C, MAX_VH / 2, VPITCH_C, 1);
		bind_texture(u_cr, s->ocr, VPITCH_C, MAX_VH / 2, VPITCH_C, 1);
		quad(&yuv_prog, vx + (vw_ui - w) / 2, vy + (vh_ui - h) / 2, w, h,
		     (float)vw / VPITCH_Y, (float)vh / MAX_VH);
	}

	if (oset_cur >= 0) {
		rsxSetBlendFunc(ctx, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
		rsxSetBlendEquation(ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
		rsxSetBlendEnable(ctx, GCM_TRUE);
		use_program(&rgba_prog);
		bind_texture(u_tex, ooff[oset_cur], UI_W, UI_H, UI_W * 4, 0);
		quad(&rgba_prog, 0, 0, UI_W, UI_H, 1.0f, 1.0f);
		rsxSetBlendEnable(ctx, GCM_FALSE);
	}
	flip();
}
