/*
 * ddcube -- DirectDraw and Direct3D 7 test case for qemu-3dfx.
 *
 * The counterpart to d3dcube (Direct3D 8 over WineD3D) and glidecube (Glide).
 * This one covers the route that games of the DirectX 6 and 7 era take:
 * DirectDraw creates the surfaces, Direct3D draws into them. Drakan is such a
 * game, and finding out what its device enumeration saw cost a whole evening of
 * guessing -- this program answers that in one run.
 *
 * Two things it does:
 *
 *   1. It reports. Which DirectDraw drivers exist, which Direct3D devices they
 *      offer, whether any of them is a HAL, what the adapter calls itself, how
 *      much video memory there is and which display modes are on offer. That is
 *      the part that replaces reading someone else's setup dialog.
 *   2. It draws. A rotating cube in a full screen 640x480 mode at 16 bit -- the
 *      depth games of that era ask for -- and measures the frame rate.
 *
 * Everything also goes to C:\DDOUT.TXT, so the result can be fetched from the
 * host with guestfish instead of being read off a screenshot.
 *
 * Guest only: DirectDraw exists on Windows, there is no host build.
 */

#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define REPORT_FILE_NAME    "C:\\DDOUT.TXT"
#define WINDOW_CLASS_NAME   "ddcube"

#define SCREEN_WIDTH        640
#define SCREEN_HEIGHT       480
#define SCREEN_DEPTH        16

static FILE *report_file = NULL;

static void report(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    vprintf(format, arguments);
    va_end(arguments);
    fflush(stdout);

    if (report_file != NULL) {
        va_start(arguments, format);
        vfprintf(report_file, format, arguments);
        va_end(arguments);
        fflush(report_file);
    }
}

/* --------------------------------------------------------- enumeration -- */

static const char *device_kind(const GUID *device_guid)
{
    if (IsEqualGUID(device_guid, &IID_IDirect3DTnLHalDevice)) {
        return "HAL mit Hardware-T&L";
    }
    if (IsEqualGUID(device_guid, &IID_IDirect3DHALDevice)) {
        return "HAL";
    }
    if (IsEqualGUID(device_guid, &IID_IDirect3DRGBDevice)) {
        return "Software (RGB)";
    }
    if (IsEqualGUID(device_guid, &IID_IDirect3DRefDevice)) {
        return "Software (Referenz)";
    }
    if (IsEqualGUID(device_guid, &IID_IDirect3DMMXDevice)) {
        return "Software (MMX)";
    }
    return "unbekannt";
}

static int hal_device_found = 0;

static HRESULT WINAPI enum_device_callback(LPSTR description, LPSTR name,
                                           LPD3DDEVICEDESC7 device_description,
                                           LPVOID context)
{
    const GUID *device_guid = &device_description->deviceGUID;
    int is_hardware = IsEqualGUID(device_guid, &IID_IDirect3DHALDevice) ||
                      IsEqualGUID(device_guid, &IID_IDirect3DTnLHalDevice);

    (void)context;

    if (is_hardware) {
        hal_device_found = 1;
    }

    report("  Geraet: %s\n", (name != NULL) ? name : "(ohne Namen)");
    report("    Beschreibung : %s\n", (description != NULL) ? description : "(keine)");
    report("    Art          : %s\n", device_kind(device_guid));
    report("    Texturgroesse: %lu x %lu\n",
           (unsigned long)device_description->dwMaxTextureWidth,
           (unsigned long)device_description->dwMaxTextureHeight);
    report("    Texturstufen : %u\n", (unsigned)device_description->wMaxSimultaneousTextures);
    report("    DevCaps      : 0x%08lx\n", (unsigned long)device_description->dwDevCaps);
    report("    Dreieck-Caps : 0x%08lx\n",
           (unsigned long)device_description->dpcTriCaps.dwTextureCaps);
    report("\n");

    return D3DENUMRET_OK;
}

static BOOL WINAPI enum_driver_callback(GUID *driver_guid, LPSTR description,
                                        LPSTR name, LPVOID context)
{
    (void)context;

    report("  Treiber: %s -- %s%s\n",
           (name != NULL) ? name : "(ohne Namen)",
           (description != NULL) ? description : "(keine Beschreibung)",
           (driver_guid == NULL) ? "   [Hauptbildschirm]" : "");

    return DDENUMRET_OK;
}

static HRESULT WINAPI enum_mode_callback(LPDDSURFACEDESC2 surface_description,
                                         LPVOID context)
{
    int *mode_count = (int *)context;

    /* Only the depths that matter here, and only a few lines of them -- the
     * full list would be dozens of entries of the same widths. */
    if (surface_description->ddpfPixelFormat.dwRGBBitCount == 16 ||
        surface_description->ddpfPixelFormat.dwRGBBitCount == 32) {
        if (*mode_count < 12) {
            report("    %lu x %lu x %lu\n",
                   (unsigned long)surface_description->dwWidth,
                   (unsigned long)surface_description->dwHeight,
                   (unsigned long)surface_description->ddpfPixelFormat.dwRGBBitCount);
        }
        (*mode_count)++;
    }

    return DDENUMRET_OK;
}

static void report_adapter(IDirectDraw7 *directdraw)
{
    DDDEVICEIDENTIFIER2 identifier;
    DDCAPS driver_caps;
    HRESULT result;

    memset(&identifier, 0, sizeof(identifier));
    result = IDirectDraw7_GetDeviceIdentifier(directdraw, &identifier, 0);
    if (result == DD_OK) {
        report("  Adapter      : %s\n", identifier.szDescription);
        report("  Treiberdatei : %s\n", identifier.szDriver);
        report("  Hersteller   : 0x%04lx  Geraet: 0x%04lx\n",
               (unsigned long)identifier.dwVendorId,
               (unsigned long)identifier.dwDeviceId);
    } else {
        report("  GetDeviceIdentifier gescheitert: 0x%08lx\n", (unsigned long)result);
    }

    memset(&driver_caps, 0, sizeof(driver_caps));
    driver_caps.dwSize = sizeof(driver_caps);
    result = IDirectDraw7_GetCaps(directdraw, &driver_caps, NULL);
    if (result == DD_OK) {
        report("  Bildspeicher : %lu MB gesamt\n",
               (unsigned long)(driver_caps.dwVidMemTotal >> 20));
        report("  DDraw-Caps   : 0x%08lx  (0x%08lx = BLT in Hardware)\n",
               (unsigned long)driver_caps.dwCaps, (unsigned long)DDCAPS_BLT);
    }
}

/* --------------------------------------------------------------- cube --- */

static void matrix_identity(D3DMATRIX *matrix)
{
    memset(matrix, 0, sizeof(*matrix));
    matrix->_11 = 1.0f;
    matrix->_22 = 1.0f;
    matrix->_33 = 1.0f;
    matrix->_44 = 1.0f;
}

static void matrix_rotation(D3DMATRIX *matrix, float angle_x, float angle_y)
{
    float sin_x = (float)sin(angle_x);
    float cos_x = (float)cos(angle_x);
    float sin_y = (float)sin(angle_y);
    float cos_y = (float)cos(angle_y);

    matrix_identity(matrix);

    matrix->_11 =  cos_y;
    matrix->_13 =  sin_y;
    matrix->_21 =  sin_x * sin_y;
    matrix->_22 =  cos_x;
    matrix->_23 = -sin_x * cos_y;
    matrix->_31 = -cos_x * sin_y;
    matrix->_32 =  sin_x;
    matrix->_33 =  cos_x * cos_y;
}

static void matrix_projection(D3DMATRIX *matrix)
{
    const float field_of_view = 1.4f;
    const float near_plane    = 1.0f;
    const float far_plane     = 100.0f;
    const float aspect        = (float)SCREEN_WIDTH / (float)SCREEN_HEIGHT;

    float scale_y = (float)(1.0 / tan(field_of_view * 0.5));
    float scale_x = scale_y / aspect;

    memset(matrix, 0, sizeof(*matrix));
    matrix->_11 = scale_x;
    matrix->_22 = scale_y;
    matrix->_33 = far_plane / (far_plane - near_plane);
    matrix->_34 = 1.0f;
    matrix->_43 = -near_plane * far_plane / (far_plane - near_plane);
}

typedef struct {
    float x, y, z;
    DWORD colour;
} CubeVertex;

#define CUBE_VERTEX_FORMAT (D3DFVF_XYZ | D3DFVF_DIFFUSE)
#define CUBE_VERTEX_COUNT  36

static CubeVertex cube_vertices[CUBE_VERTEX_COUNT];

static void build_cube(void)
{
    static const float corner[8][3] = {
        { -1.0f, -1.0f, -1.0f }, {  1.0f, -1.0f, -1.0f },
        {  1.0f,  1.0f, -1.0f }, { -1.0f,  1.0f, -1.0f },
        { -1.0f, -1.0f,  1.0f }, {  1.0f, -1.0f,  1.0f },
        {  1.0f,  1.0f,  1.0f }, { -1.0f,  1.0f,  1.0f }
    };
    static const int face[6][4] = {
        { 0, 1, 2, 3 }, { 5, 4, 7, 6 }, { 4, 0, 3, 7 },
        { 1, 5, 6, 2 }, { 4, 5, 1, 0 }, { 3, 2, 6, 7 }
    };
    static const DWORD face_colour[6] = {
        0x00FF4040, 0x0040FF40, 0x004080FF,
        0x00FFFF40, 0x00FF40FF, 0x0040FFFF
    };

    int face_index, corner_index, vertex_index = 0;

    for (face_index = 0; face_index < 6; face_index++) {
        static const int triangle[6] = { 0, 1, 2, 0, 2, 3 };

        for (corner_index = 0; corner_index < 6; corner_index++) {
            const float *point = corner[face[face_index][triangle[corner_index]]];

            cube_vertices[vertex_index].x      = point[0];
            cube_vertices[vertex_index].y      = point[1];
            cube_vertices[vertex_index].z      = point[2];
            cube_vertices[vertex_index].colour = face_colour[face_index];
            vertex_index++;
        }
    }
}

/* ---------------------------------------------------------------- main -- */

static LRESULT CALLBACK window_procedure(HWND window, UINT message,
                                         WPARAM word_parameter, LPARAM long_parameter)
{
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    if (message == WM_KEYDOWN && word_parameter == VK_ESCAPE) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(window, message, word_parameter, long_parameter);
}

static HWND create_window(HINSTANCE instance)
{
    WNDCLASS window_class;

    memset(&window_class, 0, sizeof(window_class));
    window_class.lpfnWndProc   = window_procedure;
    window_class.hInstance     = instance;
    window_class.hCursor       = LoadCursor(NULL, IDC_ARROW);
    window_class.lpszClassName = WINDOW_CLASS_NAME;
    RegisterClass(&window_class);

    return CreateWindowEx(0, WINDOW_CLASS_NAME, "ddcube", WS_POPUP | WS_VISIBLE,
                          0, 0, SCREEN_WIDTH, SCREEN_HEIGHT,
                          NULL, NULL, instance, NULL);
}

int main(int argc, char **argv)
{
    const double default_run_seconds = 15.0;

    IDirectDraw7        *directdraw   = NULL;
    IDirect3D7          *direct3d     = NULL;
    IDirectDrawSurface7 *primary      = NULL;
    IDirectDrawSurface7 *back_buffer  = NULL;
    IDirectDrawSurface7 *depth_buffer = NULL;
    IDirect3DDevice7    *device       = NULL;
    DDSURFACEDESC2       surface_description;
    DDSCAPS2             surface_caps;
    D3DVIEWPORT7         viewport;
    D3DMATRIX            world_matrix, view_matrix, projection_matrix;
    HWND                 window;
    HRESULT              result;
    double               run_seconds = default_run_seconds;
    int                  info_only = 0;
    int                  mode_count = 0;
    int                  argument;
    DWORD                start_ticks, now_ticks;
    long                 frames = 0;

    for (argument = 1; argument < argc; argument++) {
        if (strcmp(argv[argument], "-info") == 0) {
            info_only = 1;
        } else {
            run_seconds = atof(argv[argument]);
        }
    }

    report_file = fopen(REPORT_FILE_NAME, "w");

    report("ddcube -- DirectDraw und Direct3D 7 ueber qemu-3dfx\n");
    report("---------------------------------------------------\n\n");

    report("DirectDraw-Treiber:\n");
    DirectDrawEnumerateA(enum_driver_callback, NULL);
    report("\n");

    result = DirectDrawCreateEx(NULL, (LPVOID *)&directdraw, &IID_IDirectDraw7, NULL);
    if (result != DD_OK) {
        report("FEHLER: DirectDrawCreateEx gescheitert: 0x%08lx\n", (unsigned long)result);
        return 1;
    }

    report("Hauptbildschirm:\n");
    report_adapter(directdraw);
    report("\n");

    window = create_window(GetModuleHandle(NULL));

    result = IDirectDraw7_SetCooperativeLevel(directdraw, window, DDSCL_NORMAL);
    if (result != DD_OK) {
        report("FEHLER: SetCooperativeLevel(NORMAL) gescheitert: 0x%08lx\n",
               (unsigned long)result);
    }

    report("Bildschirmmodi (16 und 32 Bit, gekuerzt):\n");
    IDirectDraw7_EnumDisplayModes(directdraw, 0, NULL, &mode_count, enum_mode_callback);
    report("  ... insgesamt %d Modi in 16 oder 32 Bit\n\n", mode_count);

    report("Direct3D-Geraete:\n");
    result = IDirectDraw7_QueryInterface(directdraw, &IID_IDirect3D7, (LPVOID *)&direct3d);
    if (result != DD_OK) {
        report("FEHLER: kein IDirect3D7 -- 0x%08lx\n", (unsigned long)result);
        report("\nUrteil: **kein Direct3D**. Genau das melden Spiele als\n");
        report("        \"no 3D accelerator installed\".\n");
        IDirectDraw7_Release(directdraw);
        if (report_file != NULL) fclose(report_file);
        return 1;
    }
    IDirect3D7_EnumDevices(direct3d, enum_device_callback, NULL);

    report("Urteil: %s\n\n", hal_device_found ?
           "**ein HAL ist vorhanden**" : "**kein HAL** -- nur Software-Geraete");

    if (info_only || !hal_device_found) {
        if (direct3d != NULL)   IDirect3D7_Release(direct3d);
        if (directdraw != NULL) IDirectDraw7_Release(directdraw);
        if (report_file != NULL) fclose(report_file);
        return hal_device_found ? 0 : 1;
    }

    /* --- from here on the cube, in a full screen mode at 16 bit ---------- */

    result = IDirectDraw7_SetCooperativeLevel(directdraw, window,
                 DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_ALLOWREBOOT);
    if (result != DD_OK) {
        report("FEHLER: SetCooperativeLevel(EXCLUSIVE) gescheitert: 0x%08lx\n",
               (unsigned long)result);
        return 1;
    }

    result = IDirectDraw7_SetDisplayMode(directdraw, SCREEN_WIDTH, SCREEN_HEIGHT,
                                         SCREEN_DEPTH, 0, 0);
    if (result != DD_OK) {
        report("FEHLER: SetDisplayMode %dx%dx%d gescheitert: 0x%08lx\n",
               SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_DEPTH, (unsigned long)result);
        return 1;
    }

    memset(&surface_description, 0, sizeof(surface_description));
    surface_description.dwSize  = sizeof(surface_description);
    surface_description.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    surface_description.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP |
                                         DDSCAPS_COMPLEX | DDSCAPS_3DDEVICE;
    surface_description.dwBackBufferCount = 1;
    result = IDirectDraw7_CreateSurface(directdraw, &surface_description, &primary, NULL);
    if (result != DD_OK) {
        report("FEHLER: primaere Oberflaeche gescheitert: 0x%08lx\n", (unsigned long)result);
        return 1;
    }

    memset(&surface_caps, 0, sizeof(surface_caps));
    surface_caps.dwCaps = DDSCAPS_BACKBUFFER;
    result = IDirectDrawSurface7_GetAttachedSurface(primary, &surface_caps, &back_buffer);
    if (result != DD_OK) {
        report("FEHLER: kein Hintergrundpuffer: 0x%08lx\n", (unsigned long)result);
        return 1;
    }

    memset(&surface_description, 0, sizeof(surface_description));
    surface_description.dwSize   = sizeof(surface_description);
    surface_description.dwFlags  = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    surface_description.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
    surface_description.dwWidth  = SCREEN_WIDTH;
    surface_description.dwHeight = SCREEN_HEIGHT;
    surface_description.ddpfPixelFormat.dwSize        = sizeof(DDPIXELFORMAT);
    surface_description.ddpfPixelFormat.dwFlags       = DDPF_ZBUFFER;
    surface_description.ddpfPixelFormat.dwZBufferBitDepth = 16;
    surface_description.ddpfPixelFormat.dwZBitMask    = 0x0000FFFF;
    result = IDirectDraw7_CreateSurface(directdraw, &surface_description, &depth_buffer, NULL);
    if (result == DD_OK) {
        IDirectDrawSurface7_AddAttachedSurface(back_buffer, depth_buffer);
    } else {
        report("Hinweis: kein Z-Puffer (0x%08lx), es wird ohne gezeichnet\n",
               (unsigned long)result);
    }

    result = IDirect3D7_CreateDevice(direct3d, &IID_IDirect3DHALDevice, back_buffer, &device);
    if (result != DD_OK) {
        report("FEHLER: CreateDevice(HAL) gescheitert: 0x%08lx\n", (unsigned long)result);
        return 1;
    }

    memset(&viewport, 0, sizeof(viewport));
    viewport.dwWidth  = SCREEN_WIDTH;
    viewport.dwHeight = SCREEN_HEIGHT;
    viewport.dvMaxZ   = 1.0f;
    IDirect3DDevice7_SetViewport(device, &viewport);

    matrix_identity(&view_matrix);
    view_matrix._43 = 5.0f;          /* camera five units in front of the cube */
    matrix_projection(&projection_matrix);
    IDirect3DDevice7_SetTransform(device, D3DTRANSFORMSTATE_VIEW, &view_matrix);
    IDirect3DDevice7_SetTransform(device, D3DTRANSFORMSTATE_PROJECTION, &projection_matrix);
    IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_LIGHTING, FALSE);
    IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ZENABLE, TRUE);
    IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);

    build_cube();

    start_ticks = GetTickCount();
    for (;;) {
        MSG message;
        double elapsed_seconds;
        float angle;

        while (PeekMessage(&message, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessage(&message);
            if (message.message == WM_QUIT) {
                run_seconds = 0.0;
            }
        }

        now_ticks = GetTickCount();
        elapsed_seconds = (double)(now_ticks - start_ticks) / 1000.0;
        angle = (float)(elapsed_seconds * 0.8);

        matrix_rotation(&world_matrix, angle * 0.7f, angle);
        IDirect3DDevice7_SetTransform(device, D3DTRANSFORMSTATE_WORLD, &world_matrix);

        IDirect3DDevice7_Clear(device, 0, NULL,
                               D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0x00202020, 1.0f, 0);
        if (IDirect3DDevice7_BeginScene(device) == D3D_OK) {
            IDirect3DDevice7_DrawPrimitive(device, D3DPT_TRIANGLELIST,
                                           CUBE_VERTEX_FORMAT, cube_vertices,
                                           CUBE_VERTEX_COUNT, 0);
            IDirect3DDevice7_EndScene(device);
        }
        IDirectDrawSurface7_Flip(primary, NULL, DDFLIP_WAIT);
        frames++;

        if (run_seconds <= 0.0 || elapsed_seconds >= run_seconds) {
            break;
        }
    }

    now_ticks = GetTickCount();
    {
        double total_seconds = (double)(now_ticks - start_ticks) / 1000.0;

        IDirectDraw7_SetCooperativeLevel(directdraw, window, DDSCL_NORMAL);
        IDirectDraw7_RestoreDisplayMode(directdraw);

        report("%ld Bilder in %.1f Sekunden, %.1f FPS\n",
               frames, total_seconds,
               (total_seconds > 0.0) ? (frames / total_seconds) : 0.0);
    }

    if (device != NULL)       IDirect3DDevice7_Release(device);
    if (depth_buffer != NULL) IDirectDrawSurface7_Release(depth_buffer);
    if (primary != NULL)      IDirectDrawSurface7_Release(primary);
    if (direct3d != NULL)     IDirect3D7_Release(direct3d);
    if (directdraw != NULL)   IDirectDraw7_Release(directdraw);
    if (report_file != NULL)  fclose(report_file);

    DestroyWindow(window);
    return 0;
}
