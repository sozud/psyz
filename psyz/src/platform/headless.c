// Renderer backend that draws nothing and opens no window.
#include <psyz.h>
#include <psyz/overlay.h>
#include <psyz/overlay_sdl3.h>
#include <stdbool.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "../internal.h"

#define RECT PS1_RECT
#include "libgpu.h"
#include "../draw.h"
#undef RECT

#define SEMITRANSP 0x02
#define TEXTURED 0x04
#define EXTRA_VERTEX 0x08
#define GOURAUD 0x10
#define TRIANGLE 0x20

static bool is_platform_initialized = false;
static PsyzVsyncMode vsync_mode = PSYZ_VSYNC_AUTO;
static PsyzDitherMode dither_mode = PSYZ_DITHER_AUTO;
static PsyzAspectMode aspect_mode = PSYZ_ASPECT_DISPLAY;
static unsigned internal_res = 1;
static Uint32 last_vsync = 0;
static PsyzVideoStats stats = {0};
static PsyzOverlayEventCB_SDL3 overlay_event_cb = NULL;
static PsyzOverlayFrameCB overlay_frame_cb = NULL;
static PsyzOverlayDestroyCB overlay_destroy_cb = NULL;

bool InitPlatform() {
    if (!is_platform_initialized) {
        is_platform_initialized = true;
        last_vsync = (Uint32)SDL_GetTicks();
    }
    return true;
}

void ResetPlatform(void) {}

void Psyz_SetTitle(const char* str) { (void)str; }

int Psyz_QuitRequested(void) { return 0; }

PsyzOverlayEventCB_SDL3 Psyz_OverlayEvent_SDL3(PsyzOverlayEventCB_SDL3 cb) {
    PsyzOverlayEventCB_SDL3 prev = overlay_event_cb;
    overlay_event_cb = cb;
    return prev;
}

PsyzOverlayFrameCB Psyz_OverlayFrameCB(PsyzOverlayFrameCB cb) {
    PsyzOverlayFrameCB prev = overlay_frame_cb;
    overlay_frame_cb = cb;
    return prev;
}

PsyzOverlayDestroyCB Psyz_OverlayDestroyCB(PsyzOverlayDestroyCB cb) {
    PsyzOverlayDestroyCB prev = overlay_destroy_cb;
    overlay_destroy_cb = cb;
    return prev;
}

int Psyz_VideoVSync(int mode) {
    Uint32 cur;
    unsigned short ret;
    InitPlatform();
    cur = (Uint32)SDL_GetTicks();
    if (mode >= 0) {
        ret = (unsigned short)(cur - last_vsync);
    } else {
        ret = (unsigned short)stats.total_frames;
    }
    last_vsync = cur;
    if (mode == 0) {
        stats.total_frames++;
    }
    return ret;
}

int Psyz_VideoSetVsyncMode(PsyzVsyncMode mode) {
    if (mode < 0 || mode > 3) {
        return -1;
    }
    vsync_mode = mode;
    return 0;
}

PsyzVsyncMode Psyz_VideoGetVsyncMode(void) { return vsync_mode; }

int Psyz_VideoSetDitheringMode(PsyzDitherMode mode) {
    dither_mode = mode;
    return 0;
}

PsyzDitherMode Psyz_VideoGetDitheringMode(void) { return dither_mode; }

int Psyz_VideoSetAspectMode(PsyzAspectMode mode) {
    aspect_mode = mode;
    return 0;
}

PsyzAspectMode Psyz_VideoGetAspectMode(void) { return aspect_mode; }

PsyzSize Psyz_VideoGetDisplaySize(void) {
    PsyzSize s = {0, 0};
    return s;
}

void Psyz_VideoSetDrawArea(PsyzRect rect) { (void)rect; }

int Psyz_VideoSetInternalResolution(unsigned multiplier) {
    if (multiplier < 1 || multiplier > PSYZ_INTERNAL_RES_MAX) {
        return -1;
    }
    internal_res = multiplier;
    return 0;
}

unsigned Psyz_VideoGetInternalResolution(void) { return internal_res; }

int Psyz_VideoStats(PsyzVideoStats* out) {
    if (!out) {
        return -1;
    }
    *out = stats;
    return 0;
}

unsigned char* Psyz_VideoAllocCapturedFrame(int* w, int* h) {
    *w = 0;
    *h = 0;
    return NULL;
}

unsigned char* Psyz_VideoAllocVramDump(int* w, int* h) {
    *w = 0;
    *h = 0;
    return NULL;
}

void MyPadInit(int mode) { (void)mode; }

void Psyz_PadsPoll(void) {
    char frame[PSYZ_PAD_BUF_LEN];
    memset(frame, 0xFF, sizeof(frame));
    frame[0] = 0;
    frame[1] = 0x41;
    for (int port = 0; port < 2; port++) {
        Psyz_PadsSet(port, frame, sizeof(frame));
    }
}

void Draw_Reset() {}
void Draw_DisplayEnable(unsigned int on) { (void)on; }
void Draw_DisplayArea(unsigned int x, unsigned int y) {}
void Draw_DisplayHorizontalRange(unsigned int start, unsigned int end) {}
void Draw_DisplayVerticalRange(unsigned int start, int unsigned end) {}
void Draw_SetDisplayMode(DisplayMode* mode) { (void)mode; }
int Draw_ExequeSync() { return 0; }
void Draw_SetTexpageMode(ParamDrawTexpageMode* p) { (void)p; }
void Draw_SetTextureWindow(unsigned int mask_x, unsigned int mask_y,
                           unsigned int off_x, unsigned int off_y) {}
void Draw_SetAreaStart(int x, int y) {}
void Draw_SetAreaEnd(int x, int y) {}
void Draw_SetOffset(int x, int y) {}
int Draw_SetHorizontalGrid(
    unsigned int source_width, unsigned int target_width) {
    return 0;
}
void Draw_SetMask(int bit0, int bit1) {}
void Draw_ClearImage(PS1_RECT* rect, u_char r, u_char g, u_char b) {}
void Draw_LoadImage(PS1_RECT* rect, u_long* p) {}
void Draw_StoreImage(PS1_RECT* rect, u_long* p) {}
void Draw_MoveImage(PS1_RECT* rect, unsigned int x, unsigned int y) {}
void Draw_ResetBuffer(void) {}
void Draw_FlushBuffer(void) {}

static int VertexWords(int code, int n) {
    int w;
    if (!n) {
        return 0;
    }
    n--;
    if (!n) {
        return 1;
    }
    w = 1;
    if (code & TEXTURED) {
        w++;
        n--;
        if (!n) {
            return w;
        }
    }
    if (code & GOURAUD) {
        w++;
    }
    return w;
}

int Draw_PushPrim(u_long* packets, int max_len) {
    int len = max_len;
    int code = (int)(*packets >> 24) & 0xFF;
    bool isPoly = !(code & 0x40);
    bool isLine = (code & 0x40) && !(code & 0x20);
    bool isTextured = (code & TEXTURED) != 0;
    bool isGouraud = (code & GOURAUD) != 0;

    len--;
    if (isPoly) {
        if (code & TRIANGLE) {
            int nVertices = code & EXTRA_VERTEX ? 4 : 3;
            for (int i = 0; i < nVertices; i++) {
                len -= VertexWords(code, len);
            }
            if (isGouraud) {
                len++;
            }
        }
    } else if (isLine) {
        bool padding = true;
        int nPoints = ((code >> 2) & 3) + 1;
        if (nPoints == 1) {
            padding = false;
            nPoints++;
        }
        for (int i = 0; len > 0 && i < nPoints; i++) {
            len--;
            if (len > 0 && i + 1 < nPoints && isGouraud) {
                len--;
            }
        }
        if (padding) {
            len--;
        }
    } else {
        len--;
        if (isTextured) {
            len--;
        }
        if ((code & ~3) == 0x60 || (code & ~3) == 0x64) {
            len--;
        }
    }
    return max_len - len;
}
