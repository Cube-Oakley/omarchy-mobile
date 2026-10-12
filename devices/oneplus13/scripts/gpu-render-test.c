/* Offscreen GLES2 shader/readback test. Never opens a KMS display or modesets.
 * Load graphics libraries at runtime so an ordinary cross GCC can build this.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#define LOAD(lib, name) \
    __typeof__(&name) p_##name = dlsym(lib, #name); \
    if (!p_##name) { fprintf(stderr, "missing %s: %s\n", #name, dlerror()); return 1; }
#define EGL_CHECK(expr) do { \
    if (!(expr)) { fprintf(stderr, "%s failed: EGL 0x%x\n", #expr, p_eglGetError()); return 1; } \
} while (0)

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/dev/dri/renderD128";
    setvbuf(stdout, NULL, _IONBF, 0);
    /* Make the result independent of the desktop's software-rendering setup. */
    unsetenv("LIBGL_ALWAYS_SOFTWARE");
    unsetenv("GBM_ALWAYS_SOFTWARE");
    unsetenv("MESA_LOADER_DRIVER_OVERRIDE");
    unsetenv("GALLIUM_DRIVER");
    printf("opening %s\n", path);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open render device"); return 1; }

    void *gbm = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
    void *egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    void *gles = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!gbm || !egl || !gles) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    void *(*create_gbm)(int) = dlsym(gbm, "gbm_create_device");
    void (*destroy_gbm)(void *) = dlsym(gbm, "gbm_device_destroy");
    if (!create_gbm || !destroy_gbm) return 1;
    LOAD(egl, eglGetProcAddress);
    LOAD(egl, eglGetError);
    LOAD(egl, eglInitialize);
    LOAD(egl, eglQueryString);
    LOAD(egl, eglBindAPI);
    LOAD(egl, eglChooseConfig);
    LOAD(egl, eglCreateContext);
    LOAD(egl, eglMakeCurrent);
    LOAD(egl, eglDestroyContext);
    LOAD(egl, eglTerminate);
    PFNEGLGETPLATFORMDISPLAYEXTPROC platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)p_eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!platform_display) return 1;
    void *device = create_gbm(fd);
    if (!device) { fprintf(stderr, "gbm_create_device failed\n"); return 1; }
    EGLDisplay display = platform_display(EGL_PLATFORM_GBM_KHR, device, NULL);
    EGLint major, minor, count;
    EGL_CHECK(display != EGL_NO_DISPLAY);
    EGL_CHECK(p_eglInitialize(display, &major, &minor));
    printf("EGL %d.%d vendor=%s\n", major, minor, p_eglQueryString(display, EGL_VENDOR));
    EGL_CHECK(p_eglBindAPI(EGL_OPENGL_ES_API));
    const EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE
    };
    const EGLint context_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLConfig config;
    EGL_CHECK(p_eglChooseConfig(display, config_attrs, &config, 1, &count) && count > 0);
    EGLContext context = p_eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
    EGL_CHECK(context != EGL_NO_CONTEXT);
    EGL_CHECK(p_eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context));

    LOAD(gles, glGetString);
    LOAD(gles, glGetError);
    LOAD(gles, glGenTextures);
    LOAD(gles, glBindTexture);
    LOAD(gles, glTexImage2D);
    LOAD(gles, glTexParameteri);
    LOAD(gles, glGenFramebuffers);
    LOAD(gles, glBindFramebuffer);
    LOAD(gles, glFramebufferTexture2D);
    LOAD(gles, glCheckFramebufferStatus);
    LOAD(gles, glCreateShader);
    LOAD(gles, glShaderSource);
    LOAD(gles, glCompileShader);
    LOAD(gles, glGetShaderiv);
    LOAD(gles, glGetShaderInfoLog);
    LOAD(gles, glCreateProgram);
    LOAD(gles, glAttachShader);
    LOAD(gles, glBindAttribLocation);
    LOAD(gles, glLinkProgram);
    LOAD(gles, glGetProgramiv);
    LOAD(gles, glGetProgramInfoLog);
    LOAD(gles, glUseProgram);
    LOAD(gles, glViewport);
    LOAD(gles, glClearColor);
    LOAD(gles, glClear);
    LOAD(gles, glVertexAttribPointer);
    LOAD(gles, glEnableVertexAttribArray);
    LOAD(gles, glDrawArrays);
    LOAD(gles, glFinish);
    LOAD(gles, glReadPixels);
    LOAD(gles, glDeleteProgram);
    LOAD(gles, glDeleteShader);
    LOAD(gles, glDeleteFramebuffers);
    LOAD(gles, glDeleteTextures);
    const char *renderer = (const char *)p_glGetString(GL_RENDERER);
    printf("GL vendor=%s renderer=%s version=%s\n", p_glGetString(GL_VENDOR),
           renderer ? renderer : "(null)", p_glGetString(GL_VERSION));
    if (!renderer || (!strstr(renderer, "Adreno") && !strstr(renderer, "FD830"))) {
        fprintf(stderr, "FAIL: renderer is not Adreno\n"); return 1;
    }

    GLuint texture, framebuffer;
    p_glGenTextures(1, &texture);
    p_glBindTexture(GL_TEXTURE_2D, texture);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    p_glGenFramebuffers(1, &framebuffer);
    p_glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "incomplete framebuffer\n"); return 1;
    }
    const char *sources[] = {
        "attribute vec2 pos; void main() { gl_Position = vec4(pos, 0.0, 1.0); }",
        "precision mediump float; void main() { gl_FragColor = vec4(0.25, 0.5, 0.75, 1.0); }"
    };
    GLuint shaders[] = { p_glCreateShader(GL_VERTEX_SHADER), p_glCreateShader(GL_FRAGMENT_SHADER) };
    for (int i = 0; i < 2; i++) {
        GLint ok;
        p_glShaderSource(shaders[i], 1, &sources[i], NULL);
        p_glCompileShader(shaders[i]);
        p_glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[2048];
            p_glGetShaderInfoLog(shaders[i], sizeof(log), NULL, log);
            fprintf(stderr, "shader: %s\n", log); return 1;
        }
    }
    GLuint program = p_glCreateProgram();
    p_glAttachShader(program, shaders[0]);
    p_glAttachShader(program, shaders[1]);
    p_glBindAttribLocation(program, 0, "pos");
    p_glLinkProgram(program);
    GLint linked;
    p_glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[2048];
        p_glGetProgramInfoLog(program, sizeof(log), NULL, log);
        fprintf(stderr, "link: %s\n", log); return 1;
    }
    p_glUseProgram(program);
    p_glViewport(0, 0, 16, 16);
    p_glClearColor(1, 0, 0, 1);
    p_glClear(GL_COLOR_BUFFER_BIT);
    const GLfloat vertices[] = { -1, -1, 3, -1, -1, 3 };
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    p_glEnableVertexAttribArray(0);
    printf("submitting shader draw\n");
    p_glDrawArrays(GL_TRIANGLES, 0, 3);
    p_glFinish();
    unsigned char pixel[4] = {0};
    p_glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    GLenum error = p_glGetError();
    printf("readback RGBA=%u,%u,%u,%u GL_error=0x%x\n", pixel[0], pixel[1], pixel[2], pixel[3], error);
    int pass = error == GL_NO_ERROR && abs(pixel[0] - 64) <= 1 &&
        abs(pixel[1] - 128) <= 1 && abs(pixel[2] - 191) <= 1 && pixel[3] == 255;
    p_glDeleteProgram(program);
    p_glDeleteShader(shaders[0]);
    p_glDeleteShader(shaders[1]);
    p_glDeleteFramebuffers(1, &framebuffer);
    p_glDeleteTextures(1, &texture);
    EGL_CHECK(p_eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    p_eglDestroyContext(display, context);
    p_eglTerminate(display);
    destroy_gbm(device);
    close(fd);
    puts(pass ? "PASS: Adreno shader rendered and pixels verified" : "FAIL: unexpected pixels");
    return pass ? 0 : 1;
}
