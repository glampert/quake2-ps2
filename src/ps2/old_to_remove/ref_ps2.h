
/* ================================================================================================
 * -*- C -*-
 * File: ref_ps2.h
 * Author: Guilherme R. Lampert
 * Created on: 17/10/15
 * Brief: Implementation of Quake2 "refresh" module (renderer module) for the PS2.
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

#ifndef PS2_REFRESH_H
#define PS2_REFRESH_H

#include "client/client.h"
#include "ps2/defs_ps2.h"

// PS2DEV SDK:
#include <tamtypes.h>
#include <draw_buffers.h>
#include <draw_types.h>

/*
==============================================================

PS2 renderer API types:

==============================================================
*/

// Defined by model_load.h, but we only need a forward decl here.
typedef const struct ps2_mdl_surface_s * ps2_mdl_surface_ptr;

// Miscellaneous renderer constants.
enum
{
    // 640x448 is the standard NTSC resolution.
    DEFAULT_VID_WIDTH  = 640,
    DEFAULT_VID_HEIGHT = 448,

    // Upper texture image limits:
    MAX_TEXIMAGES      = 1024,
    MAX_TEXIMAGE_SIZE  = 256,

    // ps2_gs_packet_t constants:
    GS_PACKET_QWC_MAX  = 65535, // Maximum number of qwords allowed, but each channel has its own limitations.
    GS_PACKET_NORMAL   = 0x00,  // Normal EE RAM.
    GS_PACKET_UCAB     = 0x01,  // Uncached accelerated memory.
    GS_PACKET_SPR      = 0x02   // Scratch Pad memory.
};

// Basically the same structure of lib packet, but I needed an API
// that didn't allocate heap memory to store this structure.
typedef struct
{
    int type;
    int qwords;
    qword_t * data PS2_ALIGN(64);
} ps2_gs_packet_t;

// Type tag for textures/images (used internally Quake2).
// These can be ORed for image search criteria.
typedef enum
{
    IT_NULL    = 0,                              // Uninitialized image. Used internally.
    IT_SKIN    = (1 << 1),                       // Usually PCX.
    IT_SPRITE  = (1 << 2),                       // Usually PCX.
    IT_WALL    = (1 << 3),                       // Custom WALL format (miptex_t).
    IT_SKY     = (1 << 4),                       // PCX or TGA.
    IT_PIC     = (1 << 5),                       // Usually PCX.
    IT_BUILTIN = (1 << 6)                        // Our hardcoded built-ins. Points to static memory.
} ps2_imagetype_t;

// A texture or 2D image:
typedef struct
{
    byte *              pic;                     // Pointer to heap memory, must be freed with PS2_TexImageFree.
    ps2_imagetype_t     type;                    // Types of textures used by Quake. See ps2_imagetype_t.
    u16                 width;                   // Width in pixels;  Must be > 0 && <= MAX_TEXIMAGE_SIZE.
    u16                 height;                  // Height in pixels; Must be > 0 && <= MAX_TEXIMAGE_SIZE.
    u16                 mag_filter;              // One of the LOD_MAG_* from libdraw.
    u16                 min_filter;              // One of the LOD_MIN_* from libdraw.
    u16                 u0, v0;                  // Offsets into the scrap if this is allocate from the scrap, zero otherwise.
    u16                 u1, v1;                  // If not zero, this is a scrap image. In such case, use these instead of w&h.
    texbuffer_t         texbuf;                  // GS texture buffer info from libdraw.
    ps2_mdl_surface_ptr texture_chain;           // For sort-by-texture world drawing.
    u32                 registration_sequence;   // Registration num, so we know if it is currently referenced by the level being played.
    u32                 hash;                    // Hash of the following string, for faster lookup.
    char                name[MAX_QPATH];         // Name or id. If from a file, game path including extension.
} ps2_teximage_t;

// Common renderer state structure (AKA the "refresh" module):
typedef struct
{
    qboolean          initialized;               // Flag to keep track of initialization of our global ps2ref.
    qboolean          show_fps_count;            // Draws a frames per sec counter in the top-right corner of the screen.
    qboolean          show_mem_tags;             // Draws a debug overlay with the current values of the memory tags.
    qboolean          show_render_stats;         // Display a debug overlay with other miscellaneous renderer stats.
    qboolean          frame_started;             // Set by BeginFrame, cleared at EndFrame. Some calls must be in between.
    qboolean          registration_started;      // Set when between BeginRegistration/EndRegistration.
    u32               registration_sequence;     // Bumped each BeginRegistration. Any loaded model/image not matching it is freed on EndRegistration.
    zbuffer_t         z_buffer;                  // PS2 depth buffer information.
    framebuffer_t     frame_buffers[2];          // PS2 color framebuffers (double buffered).
    ps2_gs_packet_t   frame_packets[2];          // Big packets for all the drawings in a frame.
    ps2_gs_packet_t   tex_upload_packet[2];      // Scratch packets used only for texture uploads to VRam.
    ps2_gs_packet_t   flip_fb_packet;            // Small UCAB packet used to flip the frame buffers.
    ps2_gs_packet_t * current_frame_packet;      // One of the frame_packets[] pointers for current frame.
    qword_t *         current_frame_qwptr;       // Pointer to the qword_ts of current_frame_packet.
    qword_t *         dmatag_draw2d;             // GIF tag used by the 2D mode.
    color_t           screen_color;              // Color used to wipe the screen on BeginFrame.
    u32               ui_brightness;             // From 0 to 255, defines the color added to the 2D UI textures, such as text.
    u32               fade_scr_alpha;            // How dark to fade the screen: 255=totally black, 0=no fade.
    u32               frame_index;               // Index of the current frame buffer.
    u32               vram_used_bytes;           // Bytes of VRam currently committed.
    u32               vram_texture_start;        // Start of VRam after screen buffers where we can alloc textures.
    ps2_teximage_t *  current_tex;               // Pointer to the current game texture in VRam (points to teximages[]).
    ps2_teximage_t    teximages[MAX_TEXIMAGES];  // All the textures used by a game level + UI must fit in here!
} ps2_refresh_t;

// Renderer globals:
extern int vidref_val;
extern viddef_t viddef;
extern ps2_refresh_t ps2ref;

// Palette used to expand the 8bits textures to RGBA32.
// Imported from the colormap.pcx file.
extern const u32 ps2_global_palette[256];

/*
==============================================================

PS2 renderer API functions (ref_ps2.c):

==============================================================
*/

/*
 * Init/shutdown:
 */

qboolean PS2_RendererInit(void * unused1, void * unused2);
void PS2_RendererShutdown(void);

/*
 * Level loading / resource allocation:
 */

void PS2_BeginRegistration(const char * map_name);
void PS2_EndRegistration(void);
void PS2_SetSky(const char * name, float rotate, vec3_t axis);

struct model_s * PS2_RegisterModel(const char * name);
struct image_s * PS2_RegisterSkin(const char * name);
struct image_s * PS2_RegisterPic(const char * name);

/*
 * Frame rendering (3D stuff):
 */

qboolean PS2_IsFrameStarted(void);
void PS2_BeginFrame(float camera_separation);
void PS2_EndFrame(void);
void PS2_RenderFrame(refdef_t * view_def);
void PS2_DrawFrameSetup(const refdef_t * view_def);
void PS2_DrawWorldModel(refdef_t * view_def);
void PS2_DrawViewEntities(refdef_t * view_def);
void PS2_SetClearColor(byte r, byte g, byte b);

/*
 * 2D overlay rendering (origin at the top-left corner of the screen):
 */

void PS2_DrawGetPicSize(int * w, int * h, const char * name);
void PS2_DrawPic(int x, int y, const char * name);
void PS2_DrawStretchPic(int x, int y, int w, int h, const char * name);
void PS2_DrawStretchRaw(int x, int y, int w, int h, int cols, int rows, const byte * data);
void PS2_DrawTileClear(int x, int y, int w, int h, const char * name);

void PS2_DrawTexImage(int x, int y, ps2_teximage_t * teximage);
void PS2_DrawStretchTexImage(int x, int y, int w, int h, ps2_teximage_t * teximage);

void PS2_DrawChar(int x, int y, int c);
void PS2_DrawString(int x, int y, const char * s);
void PS2_DrawAltString(int x, int y, const char * s);

void PS2_DrawFill(int x, int y, int w, int h, int c);
void PS2_DrawFadeScreen(void);

/*
 * refexport_t miscellaneous:
 */

void PS2_CinematicSetPalette(const byte * restrict palette);
void PS2_AppActivate(qboolean activate);

/*
 * GS render packet helpers:
 */

void PS2_PacketAlloc(ps2_gs_packet_t * packet, int qwords, int type);
void PS2_PacketFree(ps2_gs_packet_t * packet);
void PS2_PacketReset(ps2_gs_packet_t * packet);
void PS2_WaitGSDrawFinish(void);

/*
==============================================================

Textures and image loading from file (tex_image.c):

==============================================================
*/

/*
 * PS2 Texture image setup/management:
 */

void PS2_TexImageInit(void);
void PS2_TexImageShutdown(void);
void PS2_TexImageFreeUnused(void);

ps2_teximage_t * PS2_TexImageAlloc(void); // Sys_Error if depleted.
ps2_teximage_t * PS2_TexImageFindOrLoad(const char * name, int flags);

void PS2_TexImageVRamUpload(ps2_teximage_t * teximage);
void PS2_TexImageBindCurrent(void);

void PS2_TexImageSetup(ps2_teximage_t * teximage, const char * name, int w, int h, int components,
                       int func, int psm, int mag_filter, int min_filter, ps2_imagetype_t type, byte * pic);
void PS2_TexImageFree(ps2_teximage_t * teximage);

/*
 * Image loading for textures/sprites:
 */

qboolean PCX_LoadFromMemory(const char * filename, const byte * data, int data_len,
                            byte ** pic, u32 * palette, int * width, int * height);
qboolean PCX_LoadFromFile(const char * filename, byte ** pic,
                          u32 * palette, int * width, int * height);
qboolean TGA_LoadFromFile(const char * filename, byte ** pic,
                          int * width, int * height);

/*
 * Convert from 8bit palettized to RGB[A]:
 */

// Palettized 8bits to RGBA 32bits.
void Img_UnPalettize32(int width, int height, const byte * restrict pic8in,
                       const u32 * restrict palette, byte * restrict pic32out);

// Palettized 8bits to RGB 24bits.
void Img_UnPalettize24(int width, int height, const byte * restrict pic8in,
                       const u32 * restrict palette, byte * restrict pic24out);

// Palettized 8bits to RGBA 16bits (5-5-5-1).
void Img_UnPalettize16(int width, int height, const byte * restrict pic8in,
                       const u32 * restrict palette, byte * restrict pic16out);

/*
 * Other image utilities:
 */

// Scrap texture atlas allocation. This is based on the same atlas used by ref_gl.
// Returns null if the atlas is full, a unique ps2_teximage_t handle referencing the atlas otherwise.
ps2_teximage_t * Img_ScrapAlloc(const byte * pic8in, int w, int h, const char * pic_name);

// Resize the input RGBA image using a box filter. Output and input must point to different buffers.
void Img_Resample32(const u32 * restrict in_img, int in_width, int in_height,
                    u32 * restrict out_img, int out_width, int out_height);

#endif // PS2_REFRESH_H
