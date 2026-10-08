// SPDX-License-Identifier: GPL-3.0-or-later
// OpenGL test for MYIOSDECK: an x86-64 Windows program (llvm-mingw) that draws a spinning
// triangle through opengl32 -> win32u's winios GL driver (Madeira, c-gow's fork) -> Mesa
// Zink -> MoltenVK -> Metal, or Apple's OpenGL ES when the Zink dylibs are not bundled.
// Without JIT its own code runs in FXI; with JIT, in FEX.
//
// It asks for a 3.3 core context (wglCreateContextAttribsARB), keeps the legacy context
// when that fails, and picks the shader dialect from GL_VERSION (desktop 3.3 core, desktop
// 2.x, or OpenGL ES 3.0). After a few frames it reads the centre pixel back: the triangle's
// colour there proves the GL path rendered, not just cleared.
// Every line goes to the console and, as it happens, to C:\myiosdeck-output.txt ([gl] lines
// in the app log). Runs 20 s or until the window closes. Exit code: 0 rendered,
// 2 drew nothing, 1 no GL context.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <GL/gl.h>

#define GL_ARRAY_BUFFER            0x8892
#define GL_STATIC_DRAW             0x88E4
#define GL_FRAGMENT_SHADER         0x8B30
#define GL_VERTEX_SHADER           0x8B31
#define GL_COMPILE_STATUS          0x8B81
#define GL_LINK_STATUS             0x8B82
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define WGL_CONTEXT_MAJOR_VERSION_ARB    0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB    0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB     0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x0001

typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef HGLRC (WINAPI *PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int *);
typedef GLuint (APIENTRY *PFN_glCreateShader)(GLenum);
typedef void (APIENTRY *PFN_glShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
typedef void (APIENTRY *PFN_glCompileShader)(GLuint);
typedef void (APIENTRY *PFN_glGetShaderiv)(GLuint, GLenum, GLint *);
typedef void (APIENTRY *PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef GLuint (APIENTRY *PFN_glCreateProgram)(void);
typedef void (APIENTRY *PFN_glAttachShader)(GLuint, GLuint);
typedef void (APIENTRY *PFN_glBindAttribLocation)(GLuint, GLuint, const GLchar *);
typedef void (APIENTRY *PFN_glLinkProgram)(GLuint);
typedef void (APIENTRY *PFN_glGetProgramiv)(GLuint, GLenum, GLint *);
typedef void (APIENTRY *PFN_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void (APIENTRY *PFN_glUseProgram)(GLuint);
typedef GLint (APIENTRY *PFN_glGetUniformLocation)(GLuint, const GLchar *);
typedef void (APIENTRY *PFN_glUniform1f)(GLint, GLfloat);
typedef void (APIENTRY *PFN_glGenBuffers)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_glBindBuffer)(GLenum, GLuint);
typedef void (APIENTRY *PFN_glBufferData)(GLenum, GLsizeiptr, const void *, GLenum);
typedef void (APIENTRY *PFN_glGenVertexArrays)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_glBindVertexArray)(GLuint);
typedef void (APIENTRY *PFN_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
typedef void (APIENTRY *PFN_glEnableVertexAttribArray)(GLuint);

static PFN_glCreateShader p_glCreateShader;
static PFN_glShaderSource p_glShaderSource;
static PFN_glCompileShader p_glCompileShader;
static PFN_glGetShaderiv p_glGetShaderiv;
static PFN_glGetShaderInfoLog p_glGetShaderInfoLog;
static PFN_glCreateProgram p_glCreateProgram;
static PFN_glAttachShader p_glAttachShader;
static PFN_glBindAttribLocation p_glBindAttribLocation;
static PFN_glLinkProgram p_glLinkProgram;
static PFN_glGetProgramiv p_glGetProgramiv;
static PFN_glGetProgramInfoLog p_glGetProgramInfoLog;
static PFN_glUseProgram p_glUseProgram;
static PFN_glGetUniformLocation p_glGetUniformLocation;
static PFN_glUniform1f p_glUniform1f;
static PFN_glGenBuffers p_glGenBuffers;
static PFN_glBindBuffer p_glBindBuffer;
static PFN_glBufferData p_glBufferData;
static PFN_glGenVertexArrays p_glGenVertexArrays;
static PFN_glBindVertexArray p_glBindVertexArray;
static PFN_glVertexAttribPointer p_glVertexAttribPointer;
static PFN_glEnableVertexAttribArray p_glEnableVertexAttribArray;

static HANDLE g_out = INVALID_HANDLE_VALUE;
static volatile LONG g_quit;

static void say(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 3) n = (int)sizeof line - 3;
    line[n++] = '\r'; line[n++] = '\n';
    DWORD w;
    if (g_out != INVALID_HANDLE_VALUE) WriteFile(g_out, line, (DWORD)n, &w, NULL);
    fwrite(line, 1, (size_t)n, stdout);
    fflush(stdout);
}

static const char *gl_str(GLenum e) {
    const char *s = (const char *)glGetString(e);
    return s ? s : "(null)";
}

static void *gl_proc(const char *name) {
    void *p = (void *)wglGetProcAddress(name);
    if (p == NULL || p == (void *)1 || p == (void *)2 || p == (void *)3 || p == (void *)-1) {
        HMODULE gl = GetModuleHandleA("opengl32.dll");
        p = gl ? (void *)GetProcAddress(gl, name) : NULL;
    }
    return p;
}

#define LOAD(name, required) do { \
        p_##name = (PFN_##name)gl_proc(#name); \
        if (!p_##name && (required)) { say("[gl] FAIL missing %s", #name); missing++; } \
    } while (0)

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE: g_quit = 1; return 0;
    case WM_DESTROY: g_quit = 1; PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static GLuint compile(GLenum type, const char *src) {
    GLuint s = p_glCreateShader(type);
    GLint ok = 0;
    p_glShaderSource(s, 1, &src, NULL);
    p_glCompileShader(s);
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        p_glGetShaderInfoLog(s, sizeof log, NULL, log);
        say("[gl] FAIL %s shader: %s", type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        return 0;
    }
    return s;
}

int main(void) {
    g_out = CreateFileA("C:\\myiosdeck-output.txt", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    say("[gl] OpenGL triangle (x64): start");

    WNDCLASSA wc = {0};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = "myiosdeck-gl";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA(wc.lpszClassName, "MYIOSDECK OpenGL triangle", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { say("[gl] FAIL CreateWindow %lu", GetLastError()); return 1; }
    HDC dc = GetDC(hwnd);

    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    int pf = ChoosePixelFormat(dc, &pfd);
    if (!pf || !SetPixelFormat(dc, pf, &pfd)) { say("[gl] FAIL pixel format %d (%lu)", pf, GetLastError()); return 1; }
    HGLRC rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) { say("[gl] FAIL wglCreateContext (%lu)", GetLastError()); return 1; }
    say("[gl] legacy context: %s | %s | %s", gl_str(GL_VENDOR), gl_str(GL_RENDERER), gl_str(GL_VERSION));

    PFN_wglCreateContextAttribsARB create_attribs =
        (PFN_wglCreateContextAttribsARB)gl_proc("wglCreateContextAttribsARB");
    int core = 0;
    if (create_attribs) {
        const int attribs[] = { WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                                WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0 };
        HGLRC rc33 = create_attribs(dc, NULL, attribs);
        if (rc33 && wglMakeCurrent(dc, rc33)) {
            wglDeleteContext(rc);
            rc = rc33;
            core = 1;
            say("[gl] 3.3 core context: %s | %s | %s", gl_str(GL_VENDOR), gl_str(GL_RENDERER), gl_str(GL_VERSION));
        } else {
            wglMakeCurrent(dc, rc);
            say("[gl] no 3.3 core context (%lu): keeping the legacy one", GetLastError());
        }
    } else {
        say("[gl] no wglCreateContextAttribsARB");
    }
    const char *version = gl_str(GL_VERSION);
    int es = !strncmp(version, "OpenGL ES", 9);
    say("[gl] GLSL %s", gl_str(GL_SHADING_LANGUAGE_VERSION));

    int missing = 0;
    LOAD(glCreateShader, 1); LOAD(glShaderSource, 1); LOAD(glCompileShader, 1); LOAD(glGetShaderiv, 1);
    LOAD(glGetShaderInfoLog, 1); LOAD(glCreateProgram, 1); LOAD(glAttachShader, 1);
    LOAD(glBindAttribLocation, 1); LOAD(glLinkProgram, 1); LOAD(glGetProgramiv, 1);
    LOAD(glGetProgramInfoLog, 1); LOAD(glUseProgram, 1); LOAD(glGetUniformLocation, 1); LOAD(glUniform1f, 1);
    LOAD(glGenBuffers, 1); LOAD(glBindBuffer, 1); LOAD(glBufferData, 1); LOAD(glVertexAttribPointer, 1);
    LOAD(glEnableVertexAttribArray, 1); LOAD(glGenVertexArrays, core || es); LOAD(glBindVertexArray, core || es);
    if (missing) return 1;

    // One triangle, colour per vertex; the centre pixel ends up a mix with no blue-only clear.
    static const char *vs_core =
        "#version 330 core\n"
        "layout(location = 0) in vec2 pos; layout(location = 1) in vec3 col; out vec3 v_col; uniform float angle;\n"
        "void main() { float c = cos(angle), s = sin(angle);\n"
        "  gl_Position = vec4(c * pos.x - s * pos.y, s * pos.x + c * pos.y, 0.0, 1.0); v_col = col; }\n";
    static const char *fs_core =
        "#version 330 core\n in vec3 v_col; out vec4 frag; void main() { frag = vec4(v_col, 1.0); }\n";
    static const char *vs_es =
        "#version 300 es\n"
        "layout(location = 0) in vec2 pos; layout(location = 1) in vec3 col; out vec3 v_col; uniform float angle;\n"
        "void main() { float c = cos(angle), s = sin(angle);\n"
        "  gl_Position = vec4(c * pos.x - s * pos.y, s * pos.x + c * pos.y, 0.0, 1.0); v_col = col; }\n";
    static const char *fs_es =
        "#version 300 es\nprecision mediump float; in vec3 v_col; out vec4 frag; void main() { frag = vec4(v_col, 1.0); }\n";
    static const char *vs_110 =
        "#version 110\n attribute vec2 pos; attribute vec3 col; varying vec3 v_col; uniform float angle;\n"
        "void main() { float c = cos(angle), s = sin(angle);\n"
        "  gl_Position = vec4(c * pos.x - s * pos.y, s * pos.x + c * pos.y, 0.0, 1.0); v_col = col; }\n";
    static const char *fs_110 =
        "#version 110\n varying vec3 v_col; void main() { gl_FragColor = vec4(v_col, 1.0); }\n";
    const char *vs = es ? vs_es : core ? vs_core : vs_110;
    const char *fs = es ? fs_es : core ? fs_core : fs_110;
    say("[gl] shaders: %s", es ? "GLSL ES 3.00" : core ? "GLSL 3.30 core" : "GLSL 1.10");

    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 1;
    GLuint prog = p_glCreateProgram();
    p_glAttachShader(prog, v);
    p_glAttachShader(prog, f);
    p_glBindAttribLocation(prog, 0, "pos");
    p_glBindAttribLocation(prog, 1, "col");
    p_glLinkProgram(prog);
    GLint linked = 0;
    p_glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512] = "";
        p_glGetProgramInfoLog(prog, sizeof log, NULL, log);
        say("[gl] FAIL link: %s", log);
        return 1;
    }
    p_glUseProgram(prog);
    GLint angle_loc = p_glGetUniformLocation(prog, "angle");

    static const float verts[] = {
        //  x      y     r    g    b
         0.0f,  0.8f, 1.0f, 0.2f, 0.2f,
        -0.7f, -0.6f, 0.2f, 1.0f, 0.2f,
         0.7f, -0.6f, 0.2f, 0.2f, 1.0f,
    };
    GLuint vao = 0, vbo = 0;
    if (p_glGenVertexArrays) { p_glGenVertexArrays(1, &vao); p_glBindVertexArray(vao); }
    p_glGenBuffers(1, &vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
    p_glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(2 * sizeof(float)));
    p_glEnableVertexAttribArray(0);
    p_glEnableVertexAttribArray(1);
    GLenum err = glGetError();
    if (err) say("[gl] glGetError after setup: 0x%x", err);

    int rendered = -1;
    unsigned frames = 0, window_frames = 0;
    DWORD start = GetTickCount(), last = start;
    while (!g_quit && GetTickCount() - start < 20000) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_quit = 1;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        RECT r;
        GetClientRect(hwnd, &r);
        int w = r.right - r.left, h = r.bottom - r.top;
        glViewport(0, 0, w, h);
        glClearColor(0.05f, 0.05f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        p_glUniform1f(angle_loc, (float)(GetTickCount() - start) * 0.001f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if (frames == 30 && rendered < 0 && w > 0 && h > 0) {
            // The triangle always covers the centre (it spins around it).
            unsigned char px[4] = {0};
            glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            rendered = px[0] + px[1] + px[2] > 3 * 0.12f * 255;
            say("[gl] centre pixel after 30 frames: %u %u %u %u -> %s", px[0], px[1], px[2], px[3],
                rendered ? "ok (triangle drawn)" : "FAIL (only the clear colour)");
        }
        SwapBuffers(dc);
        frames++;
        window_frames++;
        DWORD now = GetTickCount();
        if (now - last >= 2000) {
            say("[gl] %u frames, %.1f fps (%dx%d)", frames, window_frames * 1000.0 / (now - last), w, h);
            window_frames = 0;
            last = now;
        }
    }
    err = glGetError();
    say("[gl] done: %u frames in %.1f s, glGetError 0x%x, %s", frames, (GetTickCount() - start) / 1000.0, err,
        rendered > 0 ? "PASS" : "FAIL");
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
    ReleaseDC(hwnd, dc);
    DestroyWindow(hwnd);
    if (g_out != INVALID_HANDLE_VALUE) CloseHandle(g_out);
    return rendered > 0 ? 0 : 2;
}
