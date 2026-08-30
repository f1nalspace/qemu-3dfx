/*
 * glidemin.h -- the smallest subset of the Glide 2.x interface a simple test
 * case needs.
 *
 * Why a private header instead of the 3Dfx SDK or the headers shipped with
 * OpenGLide: OpenGLide's SDK copy pulls in a configure-generated sdk2_unix.h
 * that is not in the source tree at all, and for MinGW it defines FX_ENTRY as
 * extern "C" -- that is C++ and breaks in plain C. This holds only what is
 * needed, and compiles with gcc, MinGW and Visual C++ 6.0.
 *
 * Every value is checked against sdk2_glide.h and sdk2_sst1vid.h from
 * OpenGLide. The vertex layout is the classic Glide 2.x one -- in that header
 * the Glide 3 layout only applies when GLIDE3 is defined.
 */

#ifndef GLIDEMIN_H
#define GLIDEMIN_H

/* Glide is stdcall on Windows, plain on Linux. */
#if defined(_WIN32) || defined(__MINGW32__) || defined(_MSC_VER)
#  define GLIDE_CALL __stdcall
#else
#  define GLIDE_CALL
#endif

typedef unsigned char  FxU8;
typedef signed short   FxI16;
typedef unsigned short FxU16;
typedef signed int     FxI32;
typedef unsigned int   FxU32;
typedef int            FxBool;
typedef FxU32          FxU;

#define FXTRUE  1
#define FXFALSE 0

typedef FxU32 GrColor_t;
typedef FxU8  GrAlpha_t;
typedef FxU32 GrBuffer_t;
typedef FxU32 GrScreenResolution_t;
typedef FxU32 GrScreenRefresh_t;
typedef FxU32 GrColorFormat_t;
typedef FxU32 GrOriginLocation_t;
typedef FxU32 GrCombineFunction_t;
typedef FxU32 GrCombineFactor_t;
typedef FxU32 GrCombineLocal_t;
typedef FxU32 GrCombineOther_t;
typedef FxU32 GrCullMode_t;
typedef FxU32 GrDepthBufferMode_t;
typedef FxU32 GrCmpFnc_t;

#define GR_WDEPTHVALUE_FARTHEST 0xFFFF
#define GR_BUFFER_BACKBUFFER    0x1
#define GR_CMP_LESS             0x1
#define GR_COLORFORMAT_ARGB     0x0
#define GR_COLORFORMAT_ABGR     0x1
#define GR_CULL_DISABLE         0x0
#define GR_DEPTHBUFFER_WBUFFER  0x2
#define GR_ORIGIN_UPPER_LEFT    0x0

#define GR_COMBINE_FUNCTION_LOCAL 0x1
#define GR_COMBINE_FACTOR_NONE    0x0
#define GR_COMBINE_LOCAL_ITERATED 0x0
#define GR_COMBINE_LOCAL_CONSTANT 0x1
#define GR_COMBINE_OTHER_NONE     0x2

#define GR_REFRESH_60Hz         0x0
#define GR_RESOLUTION_320x240   0x1
#define GR_RESOLUTION_512x384   0x3
#define GR_RESOLUTION_640x480   0x7
#define GR_RESOLUTION_800x600   0x8

#define GR_SSTTYPE_VOODOO    0
#define GR_SSTTYPE_SST96     1
#define GR_SSTTYPE_AT3D      2
#define GR_SSTTYPE_Voodoo2   3

#define GLIDE_NUM_TMU 2
#define MAX_NUM_SST   4

typedef struct {
    float sow;
    float tow;
    float oow;
} GrTmuVertex;

/* Classic Glide 2.x layout. Order and size have to match exactly, otherwise
 * the rasterizer reads garbage. */
typedef struct {
    float x, y, z;      /* screen coordinates, z is ignored */
    float r, g, b;      /* 0 .. 255 */
    float ooz;          /* 65535/z, for the z buffer */
    float a;            /* 0 .. 255 */
    float oow;          /* 1/w, for the w buffer and texturing */
    GrTmuVertex tmuvtx[GLIDE_NUM_TMU];
} GrVertex;

typedef int GrSstType;

typedef struct {
    int tmuRev;
    int tmuRam;
} GrTMUConfig_t;

typedef struct {
    int    fbRam;
    int    fbiRev;
    int    nTexelfx;
    FxBool sliDetect;
    GrTMUConfig_t tmuConfig[GLIDE_NUM_TMU];
} GrVoodooConfig_t;

typedef struct {
    int fbRam;
    int nTexelfx;
    GrTMUConfig_t tmuConfig;
} GrSst96Config_t;

typedef struct {
    int rev;
} GrAT3DConfig_t;

typedef struct {
    int num_sst;
    struct {
        GrSstType type;
        union {
            GrVoodooConfig_t VoodooConfig;
            GrSst96Config_t  SST96Config;
            GrAT3DConfig_t   AT3DConfig;
            GrVoodooConfig_t Voodoo2Config;
        } sstBoard;
    } SSTs[MAX_NUM_SST];
} GrHwConfiguration;

#if defined(__cplusplus)
extern "C" {
#endif

extern void   GLIDE_CALL grGlideInit(void);
extern void   GLIDE_CALL grGlideShutdown(void);
extern FxBool GLIDE_CALL grSstQueryHardware(GrHwConfiguration *hwconfig);
extern void   GLIDE_CALL grSstSelect(int which_sst);
extern FxBool GLIDE_CALL grSstWinOpen(FxU hWnd,
                                      GrScreenResolution_t resolution,
                                      GrScreenRefresh_t    refresh,
                                      GrColorFormat_t      format,
                                      GrOriginLocation_t   origin,
                                      int nColBuffers, int nAuxBuffers);
extern void   GLIDE_CALL grSstWinClose(void);
extern void   GLIDE_CALL grBufferClear(GrColor_t color, GrAlpha_t alpha, FxU16 depth);
extern void   GLIDE_CALL grBufferSwap(int swap_interval);
extern void   GLIDE_CALL grRenderBuffer(GrBuffer_t buffer);
extern void   GLIDE_CALL grDrawTriangle(const GrVertex *a, const GrVertex *b, const GrVertex *c);
extern void   GLIDE_CALL grColorCombine(GrCombineFunction_t func, GrCombineFactor_t factor,
                                        GrCombineLocal_t local, GrCombineOther_t other,
                                        FxBool invert);
extern void   GLIDE_CALL grAlphaCombine(GrCombineFunction_t func, GrCombineFactor_t factor,
                                        GrCombineLocal_t local, GrCombineOther_t other,
                                        FxBool invert);
extern void   GLIDE_CALL grColorMask(FxBool rgb, FxBool alpha);
extern void   GLIDE_CALL grCullMode(GrCullMode_t mode);
extern void   GLIDE_CALL grConstantColorValue(GrColor_t value);
extern void   GLIDE_CALL grDepthBufferMode(GrDepthBufferMode_t mode);
extern void   GLIDE_CALL grDepthBufferFunction(GrCmpFnc_t function);
extern void   GLIDE_CALL grDepthMask(FxBool enable);

#if defined(__cplusplus)
}
#endif

#endif /* GLIDEMIN_H */
