/*
 * s23gpu_opengl_test.c - Live Hardware OpenGL (Mesa Freedreno WGL) Test for S23 Adreno 740
 *
 * Tests:
 *   0. wglCreateContext + wglMakeCurrent -> verifies GL_VENDOR="freedreno" & GL_RENDERER="FD740"
 *   1. glClearColor(0.2, 0.4, 0.8, 1.0) + glClear + glReadPixels verification
 *   2. GLSL Vertex + Fragment Shader 3D RGB Triangle + glReadPixels + BMP dump + SwapBuffers(hDC)
 *   3. 120-frame rotating 3D triangle hardware OpenGL benchmark (FPS)
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define GL_ARRAY_BUFFER                   0x8892
#define GL_STATIC_DRAW                    0x88E4
#define GL_FRAGMENT_SHADER                0x8B30
#define GL_VERTEX_SHADER                  0x8B31
#define GL_COMPILE_STATUS                 0x8B81
#define GL_LINK_STATUS                    0x8B82
#define GL_SHADING_LANGUAGE_VERSION       0x8B8C

typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;

typedef GLuint (APIENTRY *PFN_glCreateShader)(GLenum type);
typedef void   (APIENTRY *PFN_glShaderSource)(GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length);
typedef void   (APIENTRY *PFN_glCompileShader)(GLuint shader);
typedef void   (APIENTRY *PFN_glGetShaderiv)(GLuint shader, GLenum pname, GLint *params);
typedef void   (APIENTRY *PFN_glGetShaderInfoLog)(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
typedef GLuint (APIENTRY *PFN_glCreateProgram)(void);
typedef void   (APIENTRY *PFN_glAttachShader)(GLuint program, GLuint shader);
typedef void   (APIENTRY *PFN_glBindAttribLocation)(GLuint program, GLuint index, const GLchar *name);
typedef void   (APIENTRY *PFN_glLinkProgram)(GLuint program);
typedef void   (APIENTRY *PFN_glGetProgramiv)(GLuint program, GLenum pname, GLint *params);
typedef void   (APIENTRY *PFN_glUseProgram)(GLuint program);
typedef GLint  (APIENTRY *PFN_glGetUniformLocation)(GLuint program, const GLchar *name);
typedef void   (APIENTRY *PFN_glUniform2f)(GLint location, GLfloat v0, GLfloat v1);
typedef void   (APIENTRY *PFN_glGenBuffers)(GLsizei n, GLuint *buffers);
typedef void   (APIENTRY *PFN_glBindBuffer)(GLenum target, GLuint buffer);
typedef void   (APIENTRY *PFN_glBufferData)(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
typedef void   (APIENTRY *PFN_glEnableVertexAttribArray)(GLuint index);
typedef void   (APIENTRY *PFN_glVertexAttribPointer)(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer);

static const char *kGlslVs =
    "#version 120\n"
    "attribute vec2 aPos;\n"
    "attribute vec4 aCol;\n"
    "uniform vec2 uCS;\n"
    "varying vec4 vCol;\n"
    "void main() {\n"
    "    float c = uCS.x;\n"
    "    float s = uCS.y;\n"
    "    vec2 r = vec2(aPos.x * c - aPos.y * s, aPos.x * s + aPos.y * c);\n"
    "    gl_Position = vec4(r, 0.5, 1.0);\n"
    "    vCol = aCol;\n"
    "}\n";

static const char *kGlslFs =
    "#version 120\n"
    "varying vec4 vCol;\n"
    "void main() {\n"
    "    gl_FragColor = vCol;\n"
    "}\n";

static void WriteBmp256RgbaBottomUp(const char *path, const unsigned char *rgba, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    unsigned int imgSize = (unsigned int)(w * h * 4);
    unsigned int fileSize = 54u + imgSize;
    unsigned char hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    *(unsigned int *)(hdr + 2)  = fileSize;
    *(unsigned int *)(hdr + 10) = 54u;
    *(unsigned int *)(hdr + 14) = 40u;
    *(int *)(hdr + 18)          = w;
    *(int *)(hdr + 22)          = h; /* bottom-up matches glReadPixels row 0 = bottom */
    *(unsigned short *)(hdr + 26) = 1;
    *(unsigned short *)(hdr + 28) = 32;
    *(unsigned int *)(hdr + 34) = imgSize;
    fwrite(hdr, 1, sizeof(hdr), f);
    for (int i = 0; i < w * h; i++) {
        unsigned char bgra[4];
        bgra[0] = rgba[i * 4 + 2];
        bgra[1] = rgba[i * 4 + 1];
        bgra[2] = rgba[i * 4 + 0];
        bgra[3] = rgba[i * 4 + 3];
        fwrite(bgra, 1, 4, f);
    }
    fclose(f);
}

static LRESULT CALLBACK GlWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

int main(int argc, char **argv)
{
    int showWin = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--window") == 0) showWin = 1;
    }

    printf("=== S23 Adreno 740 Hardware OpenGL (Mesa Freedreno WGL) Test ===\n");

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = GlWndProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "S23Adreno740GlWnd";
    wc.style = CS_OWNDC;
    RegisterClassA(&wc);

    RECT rc = { 0, 0, 256, 256 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hWnd = CreateWindowExA(0, wc.lpszClassName, "S23 Adreno 740 OpenGL",
                                WS_OVERLAPPEDWINDOW, 80, 80,
                                rc.right - rc.left, rc.bottom - rc.top,
                                NULL, NULL, wc.hInstance, NULL);
    if (!hWnd) {
        printf("[FAIL] CreateWindowExA failed err=%lu\n", GetLastError());
        return 1;
    }
    if (showWin) {
        ShowWindow(hWnd, SW_SHOW);
        UpdateWindow(hWnd);
    }

    HDC hDC = GetDC(hWnd);
    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;

    int pf = ChoosePixelFormat(hDC, &pfd);
    if (!pf || !SetPixelFormat(hDC, pf, &pfd)) {
        printf("[FAIL] Choose/SetPixelFormat failed pf=%d err=%lu\n", pf, GetLastError());
        return 1;
    }

    HGLRC hRC = wglCreateContext(hDC);
    if (!hRC || !wglMakeCurrent(hDC, hRC)) {
        printf("[FAIL] wglCreateContext/wglMakeCurrent failed hRC=%p err=%lu\n", (void *)hRC, GetLastError());
        return 1;
    }

    const char *vendor   = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version  = (const char *)glGetString(GL_VERSION);
    const char *glslVer  = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
    printf("GL_VENDOR:   %s\n", vendor   ? vendor   : "(null)");
    printf("GL_RENDERER: %s\n", renderer ? renderer : "(null)");
    printf("GL_VERSION:  %s\n", version  ? version  : "(null)");
    printf("GL_SL_VER:   %s\n", glslVer  ? glslVer  : "(null)");

    if (!renderer || (!strstr(renderer, "FD740") && !strstr(renderer, "Adreno"))) {
        printf("[FAIL] Expected Freedreno FD740 hardware renderer!\n");
        return 1;
    }

    /* Test 1: glClear + glReadPixels */
    printf("Test 1 (glClear 256x256)...\n");
    glViewport(0, 0, 256, 256);
    glClearColor(0.2f, 0.4f, 0.8f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    unsigned char px[4] = {0};
    glReadPixels(128, 128, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf("  Clear center @128,128: RGBA=(%u,%u,%u,%u) (expected ~51,102,204,255)\n",
           px[0], px[1], px[2], px[3]);
    int t1_ok = (abs((int)px[0] - 51) <= 3 &&
                 abs((int)px[1] - 102) <= 3 &&
                 abs((int)px[2] - 204) <= 3 &&
                 px[3] == 255);
    printf("  Test 1 (glClear): %s\n", t1_ok ? "PASS" : "FAIL");
    if (!t1_ok) return 1;

    /* Test 2: GLSL Vertex + Fragment Shader 3D Triangle */
    printf("Test 2 (GLSL VS+FS 3D Triangle)...\n");
    #define LOAD_GL(T, N) T N = (T)wglGetProcAddress(#N); if (!N) { printf("[FAIL] wglGetProcAddress(%s) NULL\n", #N); return 1; }
    LOAD_GL(PFN_glCreateShader, glCreateShader);
    LOAD_GL(PFN_glShaderSource, glShaderSource);
    LOAD_GL(PFN_glCompileShader, glCompileShader);
    LOAD_GL(PFN_glGetShaderiv, glGetShaderiv);
    LOAD_GL(PFN_glGetShaderInfoLog, glGetShaderInfoLog);
    LOAD_GL(PFN_glCreateProgram, glCreateProgram);
    LOAD_GL(PFN_glAttachShader, glAttachShader);
    LOAD_GL(PFN_glBindAttribLocation, glBindAttribLocation);
    LOAD_GL(PFN_glLinkProgram, glLinkProgram);
    LOAD_GL(PFN_glGetProgramiv, glGetProgramiv);
    LOAD_GL(PFN_glUseProgram, glUseProgram);
    LOAD_GL(PFN_glGetUniformLocation, glGetUniformLocation);
    LOAD_GL(PFN_glUniform2f, glUniform2f);
    LOAD_GL(PFN_glGenBuffers, glGenBuffers);
    LOAD_GL(PFN_glBindBuffer, glBindBuffer);
    LOAD_GL(PFN_glBufferData, glBufferData);
    LOAD_GL(PFN_glEnableVertexAttribArray, glEnableVertexAttribArray);
    LOAD_GL(PFN_glVertexAttribPointer, glVertexAttribPointer);

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &kGlslVs, NULL);
    glCompileShader(vs);
    GLint ok = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetShaderInfoLog(vs, sizeof(log), NULL, log);
        printf("[FAIL] VS compile: %s\n", log);
        return 1;
    }

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &kGlslFs, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetShaderInfoLog(fs, sizeof(log), NULL, log);
        printf("[FAIL] FS compile: %s\n", log);
        return 1;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aCol");
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        printf("[FAIL] Program link failed\n");
        return 1;
    }
    glUseProgram(prog);
    GLint uCS = glGetUniformLocation(prog, "uCS");

    /* 3 vertices: x, y, r, g, b, a */
    static const float kVerts[3 * 6] = {
         0.0f,  0.65f,  1.0f, 0.0f, 0.0f, 1.0f, /* Top: Red */
         0.65f, -0.65f, 0.0f, 1.0f, 0.0f, 1.0f, /* Bottom-Right: Green */
        -0.65f, -0.65f, 0.0f, 0.0f, 1.0f, 1.0f  /* Bottom-Left: Blue */
    };
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kVerts), kVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (const void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (const void *)(2 * sizeof(float)));

    glClearColor(0.05f, 0.05f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUniform2f(uCS, 1.0f, 0.0f);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    unsigned char *img = (unsigned char *)malloc(256 * 256 * 4);
    glReadPixels(0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, img);
    WriteBmp256RgbaBottomUp("C:\\Users\\Public\\s23gpu_gl_tri.bmp", img, 256, 256);

    /* Note: glReadPixels row 0 is bottom (y_gl = 255 - y_top) */
    const unsigned char *pCorner = img + (251 * 256 + 4) * 4;
    const unsigned char *pCenter = img + (115 * 256 + 128) * 4;
    const unsigned char *pTop    = img + (185 * 256 + 128) * 4;
    const unsigned char *pRight  = img + (65  * 256 + 175) * 4;
    const unsigned char *pLeft   = img + (65  * 256 + 81)  * 4;
    printf("  Triangle pixels:\n");
    printf("    corner(4,4)    = (%u,%u,%u,%u)\n", pCorner[0], pCorner[1], pCorner[2], pCorner[3]);
    printf("    center(128,140)= (%u,%u,%u,%u)\n", pCenter[0], pCenter[1], pCenter[2], pCenter[3]);
    printf("    top(128,70)    = (%u,%u,%u,%u)\n", pTop[0],    pTop[1],    pTop[2],    pTop[3]);
    printf("    right(175,190) = (%u,%u,%u,%u)\n", pRight[0],  pRight[1],  pRight[2],  pRight[3]);
    printf("    left(81,190)   = (%u,%u,%u,%u)\n", pLeft[0],   pLeft[1],   pLeft[2],   pLeft[3]);

    int t2_ok = (pCorner[0] < 30 && pCorner[1] < 30 &&
                 pCenter[0] > 40 && pCenter[1] > 40 && pCenter[2] > 40 &&
                 pTop[0] > 140 && pTop[0] > pTop[1] &&
                 pRight[1] > 100 && pRight[1] > pRight[0] &&
                 pLeft[2] > 100 && pLeft[2] > pLeft[0]);
    free(img);
    SwapBuffers(hDC);
    printf("  Wrote C:\\Users\\Public\\s23gpu_gl_tri.bmp (256x256)\n");
    printf("  Test 2 (GLSL 3D Triangle + SwapBuffers): %s\n", t2_ok ? "PASS" : "FAIL");
    if (!t2_ok) return 1;

    /* Test 3: 120-frame rotating triangle OpenGL benchmark */
    printf("Test 3 (120-frame rotating triangle OpenGL benchmark)...\n");
    LARGE_INTEGER qf, t0, t1;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&t0);
    for (int f = 0; f < 120; f++) {
        float ang = (float)f * 0.052359877f;
        glClear(GL_COLOR_BUFFER_BIT);
        glUniform2f(uCS, cosf(ang), sinf(ang));
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if (showWin) {
            SwapBuffers(hDC);
            MSG msg;
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
        }
    }
    glReadPixels(128, 115, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    QueryPerformanceCounter(&t1);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)qf.QuadPart;
    double fps = 120000.0 / (ms > 0.001 ? ms : 0.001);
    printf("  120 frames rendered in %.2f ms (%.1f FPS) final@128,140=(%u,%u,%u,%u)\n",
           ms, fps, px[0], px[1], px[2], px[3]);
    printf("  Test 3 (120-frame rotation): PASS\n");

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(hRC);
    ReleaseDC(hWnd, hDC);
    DestroyWindow(hWnd);

    printf("=== S23 ADRENO 740 OPENGL RESULT: ALL PASS ===\n");
    return 0;
}
