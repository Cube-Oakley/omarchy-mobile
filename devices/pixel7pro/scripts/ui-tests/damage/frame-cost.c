// frame-cost.c (test only): the GPU cycles one compositor-like frame costs on
// the phone's Mali (Panfrost on Panthor), from this process's own
// drm-cycles-panthor in /proc/self/fdinfo (needs
// /sys/devices/platform/28000000.gpu/profiling = 1).
//
// Each frame, as Hyprland 0.56 does: into a work buffer, a clear and a small
// quad (the damage), then a copy of the damage into an output buffer.
//
//   frame-cost W H DAMAGE FLAGS
//     DAMAGE  side of the square damage, in pixels
//     FLAGS   any of: c = clear the whole work buffer (not just the damage)
//                     i = invalidate the work buffer after each frame
//                     o = also copy into the output buffer (second pass)
//                     n = no draw at all (clear only)
//                     l = the output buffer is a linear GBM scanout buffer
//                         (as Aquamarine's swapchain with AQ_NO_MODIFIERS=1)
//                     s = the work buffer has a depth/stencil attachment
//                         (Z24S8, as Hyprland's), invalidated each frame
// Build: gcc -O2 -o frame-cost frame-cost.c -lEGL -lGLESv2 -lgbm
// Run with LD_LIBRARY_PATH=/opt/pixel-mesa/lib.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static long long cycles(void)
{
    // The render node's fdinfo, the highest value among this process's fds.
    long long best = 0;
    char path[64], line[256];
    for (int fd = 0; fd < 64; fd++) {
        snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        while (fgets(line, sizeof(line), f)) {
            long long v;
            if (sscanf(line, "drm-cycles-panthor: %lld", &v) == 1 && v > best)
                best = v;
        }
        fclose(f);
    }
    return best;
}

static GLuint shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    return s;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: frame-cost W H DAMAGE FLAGS\n");
        return 2;
    }
    const int w = atoi(argv[1]), h = atoi(argv[2]), dmg = atoi(argv[3]);
    const char *flags = argv[4];
    const int fullClear = strchr(flags, 'c') != NULL, invalidate = strchr(flags, 'i') != NULL;
    const int copy = strchr(flags, 'o') != NULL, noDraw = strchr(flags, 'n') != NULL;
    const int linear = strchr(flags, 'l') != NULL, stencil = strchr(flags, 's') != NULL;

    int drm = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    struct gbm_device *gbm = gbm_create_device(drm);
    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    eglInitialize(dpy, NULL, NULL);
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ctxAttr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctxAttr);
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        fprintf(stderr, "no surfaceless context\n");
        return 1;
    }

    GLuint tex[2], fbo[2];
    glGenTextures(2, tex);
    glGenFramebuffers(2, fbo);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[i], 0);
        glClearColor(0.1f, 0.1f, 0.1f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    const char *vs = "#version 300 es\nlayout(location=0) in vec2 p; out vec2 uv;\n"
                     "void main() { uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0, 1); }\n";
    const char *fsColor = "#version 300 es\nprecision mediump float; out vec4 c;\n"
                          "uniform float t; void main() { c = vec4(t, 0.5, 0.2, 1); }\n";
    const char *fsTex = "#version 300 es\nprecision mediump float; in vec2 uv; out vec4 c;\n"
                        "uniform sampler2D s; void main() { c = texture(s, uv); }\n";
    GLuint prog[2];
    const char *fss[2] = { fsColor, fsTex };
    for (int i = 0; i < 2; i++) {
        prog[i] = glCreateProgram();
        glAttachShader(prog[i], shader(GL_VERTEX_SHADER, vs));
        glAttachShader(prog[i], shader(GL_FRAGMENT_SHADER, fss[i]));
        glLinkProgram(prog[i]);
    }
    if (stencil) {
        GLuint zs;
        glGenRenderbuffers(1, &zs);
        glBindRenderbuffer(GL_RENDERBUFFER, zs);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, zs);
    }
    if (linear) {
        // The output as a scanout buffer: a linear GBM bo as a renderbuffer.
        struct gbm_bo *bo = gbm_bo_create(gbm, w, h, GBM_FORMAT_XRGB8888,
                                          GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
        if (!bo) {
            fprintf(stderr, "no linear bo\n");
            return 1;
        }
        const EGLint attr[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_LINUX_DRM_FOURCC_EXT, GBM_FORMAT_XRGB8888,
                                EGL_DMA_BUF_PLANE0_FD_EXT, gbm_bo_get_fd(bo), EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                                EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)gbm_bo_get_stride(bo), EGL_NONE };
        PFNEGLCREATEIMAGEKHRPROC createImage = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
        PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC rbStorage =
                (PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
        EGLImageKHR image = createImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attr);
        GLuint rb;
        glGenRenderbuffers(1, &rb);
        glBindRenderbuffer(GL_RENDERBUFFER, rb);
        rbStorage(GL_RENDERBUFFER, image);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[1]);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "linear framebuffer incomplete\n");
            return 1;
        }
        glClearColor(0.1f, 0.1f, 0.1f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    const GLfloat quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(0);
    glViewport(0, 0, w, h);
    glFinish();

    const int frames = 240;
    const long long c0 = cycles();
    for (int f = 0; f < frames; f++) {
        const int x = 100 + (f % 50), y = 300;
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
        glClearColor(0, 0, 0, 0);
        if (fullClear) {
            glDisable(GL_SCISSOR_TEST);
        } else {
            glEnable(GL_SCISSOR_TEST);
            glScissor(x, y, dmg, dmg);
        }
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
        glScissor(x, y, dmg, dmg);
        if (!noDraw) {
            glUseProgram(prog[0]);
            glUniform1f(glGetUniformLocation(prog[0], "t"), (f % 10) / 10.0f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        if (copy) {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo[1]);
            glUseProgram(prog[1]);
            glBindTexture(GL_TEXTURE_2D, tex[0]);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        if (invalidate) {
            const GLenum att[] = { GL_COLOR_ATTACHMENT0, GL_DEPTH_STENCIL_ATTACHMENT };
            glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
            glInvalidateFramebuffer(GL_FRAMEBUFFER, stencil ? 2 : 1, att);
        }
        glFlush();
        struct timespec ts = { 0, 8000000 };
        nanosleep(&ts, NULL);
    }
    glFinish();
    const long long c1 = cycles();
    printf("%dx%d damage %d flags %-4s: %lld kcycles/frame\n", w, h, dmg, flags, (c1 - c0) / frames / 1000);
    return 0;
}
