/*
 * d3dcube -- a test case for the Direct3D path of qemu-3dfx.
 *
 * The counterpart to glidecube, one interface higher up: the same rotating,
 * Gouraud-shaded cube with a depth buffer, this time through Direct3D 8. Plus
 * the frame rate and whatever GetAdapterIdentifier reports.
 *
 * The point: the chain
 *
 *     d3dcube -> D3D8 -> WineD3D -> OPENGL32.DLL (qemu-3dfx) -> MESAPT -> host GPU
 *
 * can only be proven by a program that walks it end to end and names something
 * that exists only on the far side of the device boundary. WineD3D puts the
 * GL_RENDERER string into the Description field -- if the host's card shows up
 * there, the boundary was crossed.
 *
 * Direct3DCreate8 is deliberately fetched with LoadLibrary instead of being
 * linked, so the DLL providing the entry point can be chosen:
 *
 *   -dll d3d8.dll      the ordinary route. On the host under Wine that is
 *                      Wine's own d3d8; in the guest it is Microsoft's, or the
 *                      dispatcher from wine9x.
 *   -dll wined8.dll    Wine's implementation from wine9x directly. The guest
 *                      then needs neither a DirectX redistributable nor the
 *                      dispatcher -- the shortest route to the proof.
 *
 * Deliberately plain C89, without d3dx and without C++.
 *
 * Usage:  d3dcube [seconds] [-dll NAME] [-info] [-fs] [-vsync]
 *         seconds    run time, default 15. 0 means forever.
 *         -dll NAME  DLL providing Direct3DCreate8. Default d3d8.dll.
 *         -info      print the adapter details only, draw nothing.
 *         -fs        full screen 640x480 instead of a window.
 *         -vsync     wait for vertical blank. Without this switch the frame
 *                    rate measures throughput rather than the refresh rate of
 *                    the display.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <windows.h>

#define CINTERFACE
#define COBJMACROS
#include <d3d8.h>

/* ------------------------------------------------------------------ time -- */

static double seconds_now(void)
{
    DWORD milliseconds = GetTickCount();
    return (double)milliseconds / 1000.0;
}

/* -------------------------------------------------------------- matrices -- */
/*
 * Direct3D works with row vectors: v * M. The matrices are stored row by row,
 * m[row][column]. d3dx is avoided so the source also compiles under Visual C++
 * 6.0 inside the guest, without the DirectX SDK having to be there.
 */

static void matrix_set_identity(D3DMATRIX *matrix)
{
    int row, column;

    for (row = 0; row < 4; row++) {
        for (column = 0; column < 4; column++) {
            matrix->m[row][column] = (row == column) ? 1.0f : 0.0f;
        }
    }
}

static void matrix_multiply(D3DMATRIX *result, const D3DMATRIX *left, const D3DMATRIX *right)
{
    D3DMATRIX product;
    int row, column, index;

    for (row = 0; row < 4; row++) {
        for (column = 0; column < 4; column++) {
            float sum = 0.0f;
            for (index = 0; index < 4; index++) {
                sum += left->m[row][index] * right->m[index][column];
            }
            product.m[row][column] = sum;
        }
    }
    memcpy(result, &product, sizeof(D3DMATRIX));
}

static void matrix_set_rotation_x(D3DMATRIX *matrix, float angleRadians)
{
    float cosine = (float)cos(angleRadians);
    float sine   = (float)sin(angleRadians);

    matrix_set_identity(matrix);
    matrix->m[1][1] =  cosine;
    matrix->m[1][2] =  sine;
    matrix->m[2][1] = -sine;
    matrix->m[2][2] =  cosine;
}

static void matrix_set_rotation_y(D3DMATRIX *matrix, float angleRadians)
{
    float cosine = (float)cos(angleRadians);
    float sine   = (float)sin(angleRadians);

    matrix_set_identity(matrix);
    matrix->m[0][0] =  cosine;
    matrix->m[0][2] = -sine;
    matrix->m[2][0] =  sine;
    matrix->m[2][2] =  cosine;
}

static void matrix_set_translation(D3DMATRIX *matrix, float x, float y, float z)
{
    matrix_set_identity(matrix);
    matrix->m[3][0] = x;
    matrix->m[3][1] = y;
    matrix->m[3][2] = z;
}

/* Left-handed perspective, built the way D3DXMatrixPerspectiveFovLH does. */
static void matrix_set_perspective(D3DMATRIX *matrix, float fieldOfViewRadians, float aspectRatio, float nearPlane, float farPlane)
{
    float halfFieldOfView = fieldOfViewRadians * 0.5f;
    float tangent         = (float)tan(halfFieldOfView);
    float verticalScale   = 1.0f / tangent;
    float horizontalScale = verticalScale / aspectRatio;
    float depthRange      = farPlane - nearPlane;

    memset(matrix, 0, sizeof(D3DMATRIX));
    matrix->m[0][0] = horizontalScale;
    matrix->m[1][1] = verticalScale;
    matrix->m[2][2] = farPlane / depthRange;
    matrix->m[2][3] = 1.0f;
    matrix->m[3][2] = -nearPlane * farPlane / depthRange;
}

/* -------------------------------------------------------------- geometry -- */

#define CUBE_VERTEX_FORMAT   (D3DFVF_XYZ | D3DFVF_DIFFUSE)
#define CUBE_VERTEX_COUNT    8
#define CUBE_INDEX_COUNT     36
#define CUBE_TRIANGLE_COUNT  12

typedef struct CubeVertex {
    float x;
    float y;
    float z;
    DWORD diffuseColor;
} CubeVertex;

static void fill_cube_vertices(CubeVertex *vertices)
{
    /* The eight corners of a cube around the origin. The colour follows the
     * sign of the coordinate, so every corner gets its own and the Gouraud
     * shading has something to interpolate, exactly as in glidecube. */
    const float halfExtent      = 1.0f;
    const int   brightComponent = 255;
    const int   darkComponent   = 40;

    int corner;

    for (corner = 0; corner < CUBE_VERTEX_COUNT; corner++) {
        int signX = (corner & 1) ? 1 : -1;
        int signY = (corner & 2) ? 1 : -1;
        int signZ = (corner & 4) ? 1 : -1;

        int red   = (signX > 0) ? brightComponent : darkComponent;
        int green = (signY > 0) ? brightComponent : darkComponent;
        int blue  = (signZ > 0) ? brightComponent : darkComponent;

        vertices[corner].x = halfExtent * (float)signX;
        vertices[corner].y = halfExtent * (float)signY;
        vertices[corner].z = halfExtent * (float)signZ;
        vertices[corner].diffuseColor = D3DCOLOR_XRGB(red, green, blue);
    }
}

static void fill_cube_indices(WORD *indices)
{
    /* Twelve triangles. The corner number is a bit mask from
     * fill_cube_vertices: bit 0 is the sign of x, bit 1 that of y, bit 2 that
     * of z.
     *
     *     0 (-,-,-)   1 (+,-,-)   2 (-,+,-)   3 (+,+,-)
     *     4 (-,-,+)   5 (+,-,+)   6 (-,+,+)   7 (+,+,+)
     *
     * Each face is the four corners with one axis held fixed. Back-face culling
     * is off, so the winding order does not matter -- the same way glidecube
     * works with GR_CULL_DISABLE. */
    static const WORD cubeIndices[CUBE_INDEX_COUNT] = {
        0, 1, 3,   0, 3, 2,     /* z = -1 */
        4, 5, 7,   4, 7, 6,     /* z = +1 */
        0, 2, 6,   0, 6, 4,     /* x = -1 */
        1, 3, 7,   1, 7, 5,     /* x = +1 */
        0, 1, 5,   0, 5, 4,     /* y = -1 */
        2, 3, 7,   2, 7, 6      /* y = +1 */
    };

    memcpy(indices, cubeIndices, sizeof(cubeIndices));
}

/* ----------------------------------------------------------------- window -- */

static const char windowClassName[] = "d3dcube";

static LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_CLOSE || message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    if (message == WM_KEYDOWN && wParam == VK_ESCAPE) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(window, message, wParam, lParam);
}

static HWND create_window(int clientWidth, int clientHeight, int fullscreen)
{
    HINSTANCE moduleInstance = GetModuleHandleA(NULL);
    HCURSOR   arrowCursor    = LoadCursorA(NULL, IDC_ARROW);
    HBRUSH    blackBrush     = (HBRUSH)GetStockObject(BLACK_BRUSH);
    WNDCLASSA windowClass;
    ATOM      registeredClass;
    DWORD     windowStyle;
    RECT      desiredClientArea;
    int       windowWidth, windowHeight;
    HWND      window;

    memset(&windowClass, 0, sizeof(windowClass));
    windowClass.lpfnWndProc   = window_procedure;
    windowClass.hInstance     = moduleInstance;
    windowClass.hCursor       = arrowCursor;
    windowClass.hbrBackground = blackBrush;
    windowClass.lpszClassName = windowClassName;

    registeredClass = RegisterClassA(&windowClass);
    if (registeredClass == 0) {
        return NULL;
    }

    windowStyle = fullscreen ? WS_POPUP : (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX);

    desiredClientArea.left   = 0;
    desiredClientArea.top    = 0;
    desiredClientArea.right  = clientWidth;
    desiredClientArea.bottom = clientHeight;
    AdjustWindowRect(&desiredClientArea, windowStyle, FALSE);

    windowWidth  = desiredClientArea.right  - desiredClientArea.left;
    windowHeight = desiredClientArea.bottom - desiredClientArea.top;

    window = CreateWindowExA(0, windowClassName, "d3dcube -- Direct3D-Prueffall fuer qemu-3dfx",
                             windowStyle, CW_USEDEFAULT, CW_USEDEFAULT,
                             windowWidth, windowHeight, NULL, NULL, moduleInstance, NULL);
    return window;
}

static int pump_window_messages(void)
{
    MSG message;

    while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            return 0;
        }
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    return 1;
}

/* ------------------------------------------------------------------ Info -- */

static void report_adapter(IDirect3D8 *direct3d)
{
    D3DADAPTER_IDENTIFIER8 identifier;
    D3DDISPLAYMODE displayMode;
    D3DCAPS8 capabilities;
    HRESULT identifierResult;
    HRESULT displayModeResult;
    HRESULT capabilitiesResult;
    UINT adapterCount;

    adapterCount = IDirect3D8_GetAdapterCount(direct3d);
    printf("GetAdapterCount: %u Adapter\n", adapterCount);

    memset(&identifier, 0, sizeof(identifier));
    identifierResult = IDirect3D8_GetAdapterIdentifier(direct3d, D3DADAPTER_DEFAULT, D3DENUM_NO_WHQL_LEVEL, &identifier);
    if (SUCCEEDED(identifierResult)) {
        printf("  Driver      : %s\n", identifier.Driver);
        printf("  Description : %s\n", identifier.Description);
        printf("  VendorId    : 0x%04lx  DeviceId: 0x%04lx\n",
               (unsigned long)identifier.VendorId, (unsigned long)identifier.DeviceId);
    } else {
        printf("  GetAdapterIdentifier gescheitert: 0x%08lx\n", (unsigned long)identifierResult);
    }

    memset(&displayMode, 0, sizeof(displayMode));
    displayModeResult = IDirect3D8_GetAdapterDisplayMode(direct3d, D3DADAPTER_DEFAULT, &displayMode);
    if (SUCCEEDED(displayModeResult)) {
        printf("  Bildschirm  : %u x %u, Format %u, %u Hz\n",
               displayMode.Width, displayMode.Height,
               (unsigned)displayMode.Format, displayMode.RefreshRate);
    }

    memset(&capabilities, 0, sizeof(capabilities));
    capabilitiesResult = IDirect3D8_GetDeviceCaps(direct3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &capabilities);
    if (SUCCEEDED(capabilitiesResult)) {
        unsigned long vertexShaderVersion = (unsigned long)capabilities.VertexShaderVersion;
        unsigned long pixelShaderVersion  = (unsigned long)capabilities.PixelShaderVersion;

        printf("  HAL vorhanden. MaxTextureWidth %lu, VertexShader %lu.%lu, PixelShader %lu.%lu\n",
               (unsigned long)capabilities.MaxTextureWidth,
               (vertexShaderVersion >> 8) & 0xFF, vertexShaderVersion & 0xFF,
               (pixelShaderVersion  >> 8) & 0xFF, pixelShaderVersion  & 0xFF);
    } else {
        printf("  KEIN HAL: GetDeviceCaps gab 0x%08lx zurueck.\n", (unsigned long)capabilitiesResult);
    }
    fflush(stdout);
}

/* ------------------------------------------------------------------ main -- */

typedef IDirect3D8 * (WINAPI *Direct3DCreate8Function)(UINT sdkVersion);

int main(int argc, char **argv)
{
    const double reportIntervalSeconds = 5.0;
    const double defaultRunSeconds     = 15.0;
    const double rotationsPerSecond    = 0.12;
    const double fullTurnRadians       = 6.283185307179586;
    const float  cameraDistance        = 4.0f;
    const float  fieldOfViewRadians    = 0.7853981634f;   /* 45 degrees */
    const float  nearPlaneDistance     = 1.0f;
    const float  farPlaneDistance      = 100.0f;
    const int    renderWidth           = 640;
    const int    renderHeight          = 480;
    const DWORD  backgroundColor       = D3DCOLOR_XRGB(32, 32, 32);

    const char *libraryName = "d3d8.dll";
    double runSeconds = defaultRunSeconds;
    int infoOnly   = 0;
    int fullscreen = 0;
    int waitForVerticalBlank = 0;
    int argument;

    HMODULE direct3dLibrary;
    FARPROC createEntryPoint;
    Direct3DCreate8Function direct3dCreate;
    IDirect3D8 *direct3d;
    IDirect3DDevice8 *device = NULL;
    IDirect3DVertexBuffer8 *vertexBuffer = NULL;
    IDirect3DIndexBuffer8 *indexBuffer = NULL;
    D3DDISPLAYMODE displayMode;
    D3DPRESENT_PARAMETERS presentParameters;
    D3DMATRIX projectionMatrix, viewMatrix;
    HWND window;
    HRESULT result;
    BYTE *lockedBytes;
    double startTime, lastReportTime, now;
    long framesTotal = 0, framesSinceReport = 0;

    for (argument = 1; argument < argc; argument++) {
        if (strcmp(argv[argument], "-info") == 0) {
            infoOnly = 1;
        } else if (strcmp(argv[argument], "-fs") == 0) {
            fullscreen = 1;
        } else if (strcmp(argv[argument], "-vsync") == 0) {
            waitForVerticalBlank = 1;
        } else if (strcmp(argv[argument], "-dll") == 0 && (argument + 1) < argc) {
            argument++;
            libraryName = argv[argument];
        } else {
            runSeconds = atof(argv[argument]);
        }
    }

    printf("d3dcube -- Direct3D-Prueffall fuer qemu-3dfx\n");
    printf("Direct3DCreate8 aus: %s\n", libraryName);
    fflush(stdout);

    direct3dLibrary = LoadLibraryA(libraryName);
    if (direct3dLibrary == NULL) {
        DWORD loadError = GetLastError();
        printf("FEHLER: %s liess sich nicht laden (Fehler %lu).\n", libraryName, (unsigned long)loadError);
        return 1;
    }

    /* The detour through void * is deliberate: GCC warns otherwise, because
     * FARPROC is an incomplete function type. */
    createEntryPoint = GetProcAddress(direct3dLibrary, "Direct3DCreate8");
    direct3dCreate = (Direct3DCreate8Function)(void *)createEntryPoint;
    if (direct3dCreate == NULL) {
        printf("FEHLER: %s hat keinen Einsprung Direct3DCreate8.\n", libraryName);
        return 1;
    }

    direct3d = direct3dCreate(D3D_SDK_VERSION);
    if (direct3d == NULL) {
        printf("FEHLER: Direct3DCreate8 gab NULL zurueck.\n");
        return 1;
    }

    report_adapter(direct3d);

    if (infoOnly) {
        IDirect3D8_Release(direct3d);
        return 0;
    }

    window = create_window(renderWidth, renderHeight, fullscreen);
    if (window == NULL) {
        printf("FEHLER: Fenster liess sich nicht anlegen.\n");
        IDirect3D8_Release(direct3d);
        return 1;
    }
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    memset(&displayMode, 0, sizeof(displayMode));
    result = IDirect3D8_GetAdapterDisplayMode(direct3d, D3DADAPTER_DEFAULT, &displayMode);
    if (FAILED(result)) {
        printf("FEHLER: GetAdapterDisplayMode gab 0x%08lx zurueck.\n", (unsigned long)result);
        IDirect3D8_Release(direct3d);
        return 1;
    }

    memset(&presentParameters, 0, sizeof(presentParameters));
    presentParameters.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    presentParameters.hDeviceWindow          = window;
    presentParameters.EnableAutoDepthStencil = TRUE;
    presentParameters.AutoDepthStencilFormat = D3DFMT_D16;
    if (fullscreen) {
        presentParameters.Windowed                   = FALSE;
        presentParameters.BackBufferWidth            = renderWidth;
        presentParameters.BackBufferHeight           = renderHeight;
        presentParameters.BackBufferFormat           = D3DFMT_R5G6B5;
        presentParameters.BackBufferCount            = 1;
        presentParameters.FullScreen_RefreshRateInHz = D3DPRESENT_RATE_DEFAULT;
        presentParameters.FullScreen_PresentationInterval =
            waitForVerticalBlank ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    } else {
        presentParameters.Windowed         = TRUE;
        presentParameters.BackBufferFormat = displayMode.Format;
    }

    result = IDirect3D8_CreateDevice(direct3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING, &presentParameters, &device);
    if (FAILED(result)) {
        printf("FEHLER: CreateDevice gab 0x%08lx zurueck.\n", (unsigned long)result);
        printf("        Das ist der Punkt, an dem ein fehlender GL-Kontext zuschlaegt.\n");
        IDirect3D8_Release(direct3d);
        return 1;
    }
    printf("Geraet angelegt: %s, %d x %d\n", fullscreen ? "Vollbild" : "Fenster", renderWidth, renderHeight);
    fflush(stdout);

    result = IDirect3DDevice8_CreateVertexBuffer(device, sizeof(CubeVertex) * CUBE_VERTEX_COUNT,
                                                 D3DUSAGE_WRITEONLY, CUBE_VERTEX_FORMAT,
                                                 D3DPOOL_MANAGED, &vertexBuffer);
    if (FAILED(result)) {
        printf("FEHLER: CreateVertexBuffer gab 0x%08lx zurueck.\n", (unsigned long)result);
        return 1;
    }

    lockedBytes = NULL;
    result = IDirect3DVertexBuffer8_Lock(vertexBuffer, 0, 0, &lockedBytes, 0);
    if (FAILED(result)) {
        printf("FEHLER: Lock auf den Eckenpuffer gab 0x%08lx zurueck.\n", (unsigned long)result);
        return 1;
    }
    fill_cube_vertices((CubeVertex *)lockedBytes);
    IDirect3DVertexBuffer8_Unlock(vertexBuffer);

    result = IDirect3DDevice8_CreateIndexBuffer(device, sizeof(WORD) * CUBE_INDEX_COUNT,
                                                D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                                D3DPOOL_MANAGED, &indexBuffer);
    if (FAILED(result)) {
        printf("FEHLER: CreateIndexBuffer gab 0x%08lx zurueck.\n", (unsigned long)result);
        return 1;
    }

    lockedBytes = NULL;
    result = IDirect3DIndexBuffer8_Lock(indexBuffer, 0, 0, &lockedBytes, 0);
    if (FAILED(result)) {
        printf("FEHLER: Lock auf den Indexpuffer gab 0x%08lx zurueck.\n", (unsigned long)result);
        return 1;
    }
    fill_cube_indices((WORD *)lockedBytes);
    IDirect3DIndexBuffer8_Unlock(indexBuffer);

    IDirect3DDevice8_SetRenderState(device, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice8_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice8_SetRenderState(device, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice8_SetRenderState(device, D3DRS_SHADEMODE, D3DSHADE_GOURAUD);

    matrix_set_identity(&viewMatrix);
    IDirect3DDevice8_SetTransform(device, D3DTS_VIEW, &viewMatrix);

    matrix_set_perspective(&projectionMatrix, fieldOfViewRadians,
                           (float)renderWidth / (float)renderHeight,
                           nearPlaneDistance, farPlaneDistance);
    IDirect3DDevice8_SetTransform(device, D3DTS_PROJECTION, &projectionMatrix);

    IDirect3DDevice8_SetStreamSource(device, 0, vertexBuffer, sizeof(CubeVertex));
    IDirect3DDevice8_SetIndices(device, indexBuffer, 0);
    IDirect3DDevice8_SetVertexShader(device, CUBE_VERTEX_FORMAT);

    startTime      = seconds_now();
    lastReportTime = startTime;

    for (;;) {
        D3DMATRIX rotationAroundY, rotationAroundX, spinMatrix, translationMatrix, worldMatrix;
        float angle;

        if (!pump_window_messages()) {
            break;
        }

        now = seconds_now();
        /* The angle follows elapsed time, not the frame count -- otherwise the
         * cube would spin at a different speed on every machine. Same reasoning
         * as in glidecube. */
        angle = (float)((now - startTime) * rotationsPerSecond * fullTurnRadians);

        matrix_set_rotation_y(&rotationAroundY, angle);
        matrix_set_rotation_x(&rotationAroundX, angle * 0.5f);
        matrix_multiply(&spinMatrix, &rotationAroundX, &rotationAroundY);
        matrix_set_translation(&translationMatrix, 0.0f, 0.0f, cameraDistance);
        matrix_multiply(&worldMatrix, &spinMatrix, &translationMatrix);
        IDirect3DDevice8_SetTransform(device, D3DTS_WORLD, &worldMatrix);

        IDirect3DDevice8_Clear(device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, backgroundColor, 1.0f, 0);
        IDirect3DDevice8_BeginScene(device);
        IDirect3DDevice8_DrawIndexedPrimitive(device, D3DPT_TRIANGLELIST, 0, CUBE_VERTEX_COUNT, 0, CUBE_TRIANGLE_COUNT);
        IDirect3DDevice8_EndScene(device);
        IDirect3DDevice8_Present(device, NULL, NULL, NULL, NULL);

        framesTotal++;
        framesSinceReport++;

        if ((now - lastReportTime) >= reportIntervalSeconds) {
            double elapsed = now - lastReportTime;
            printf("%ld frames in %.1f seconds, %.1f FPS\n",
                   framesSinceReport, elapsed, framesSinceReport / elapsed);
            fflush(stdout);
            framesSinceReport = 0;
            lastReportTime = now;
        }
        if (runSeconds > 0.0 && (now - startTime) >= runSeconds) {
            break;
        }
    }

    now = seconds_now();
    printf("gesamt: %ld frames in %.1f seconds, %.1f FPS\n",
           framesTotal, now - startTime, framesTotal / (now - startTime));
    fflush(stdout);

    IDirect3DIndexBuffer8_Release(indexBuffer);
    IDirect3DVertexBuffer8_Release(vertexBuffer);
    IDirect3DDevice8_Release(device);
    IDirect3D8_Release(direct3d);
    DestroyWindow(window);
    return 0;
}
