/* GPU writes to a display-owned dumb buffer, followed by direct CPU reads.
 * Reproduces the kmsro ownership path; uses DMA-BUF CPU-access ioctls.
 * EGL setup follows the shared GPU bring-up test.
 * No modesetting, display memory, hardware registers or storage writes.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <drm.h>
#include <drm_mode.h>
#include <drm_fourcc.h>
#include <linux/dma-buf.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#define LOAD(lib, name) \
    __typeof__(&name) p_##name = dlsym(lib, #name); \
    if (!p_##name) { fprintf(stderr, "missing %s: %s\n", #name, dlerror()); return 1; }
#define EGL_CHECK(expr) do { \
    if (!(expr)) { fprintf(stderr, "%s failed: EGL 0x%x\n", #expr, p_eglGetError()); return 1; } \
} while (0)

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/dev/dri/card2";
    setvbuf(stdout, NULL, _IONBF, 0);
    /* This test must never accept software rendering as a GPU success. */
    unsetenv("GBM_ALWAYS_SOFTWARE");
    unsetenv("LIBGL_ALWAYS_SOFTWARE");
    unsetenv("GALLIUM_DRIVER");
    unsetenv("MESA_LOADER_DRIVER_OVERRIDE");
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
    if (!renderer || !strstr(renderer, "Adreno (TM) 830") || strstr(renderer, "llvmpipe")) {
        fprintf(stderr, "FAIL: renderer is not the Adreno (TM) 830 hardware driver\n"); return 1;
    }

    char driver[64]={0}; struct drm_version version={.name_len=sizeof(driver)-1,.name=driver};
    if(ioctl(fd,DRM_IOCTL_VERSION,&version) || strcmp(driver,"oneplus13-scanout")) return 1;
    struct drm_mode_create_dumb create={.width=32,.height=32,.bpp=32};
    if(ioctl(fd,DRM_IOCTL_MODE_CREATE_DUMB,&create)) {perror("CREATE_DUMB");return 1;}
    struct drm_mode_map_dumb map={.handle=create.handle};
    if(ioctl(fd,DRM_IOCTL_MODE_MAP_DUMB,&map)) {perror("MAP_DUMB");return 1;}
    volatile uint32_t *cpu=mmap(NULL,create.size,PROT_READ,MAP_SHARED,fd,map.offset);
    if(cpu==MAP_FAILED) {perror("mmap");return 1;}
    struct drm_prime_handle prime={.handle=create.handle,.flags=DRM_CLOEXEC|DRM_RDWR};
    if(ioctl(fd,DRM_IOCTL_PRIME_HANDLE_TO_FD,&prime)) {perror("PRIME_HANDLE_TO_FD");return 1;}
    PFNEGLCREATEIMAGEKHRPROC create_image=(PFNEGLCREATEIMAGEKHRPROC)p_eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC destroy_image=(PFNEGLDESTROYIMAGEKHRPROC)p_eglGetProcAddress("eglDestroyImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_texture=(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)p_eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if(!create_image || !destroy_image || !image_texture) return 1;
    const EGLint attrs[]={EGL_WIDTH,32,EGL_HEIGHT,32,EGL_LINUX_DRM_FOURCC_EXT,DRM_FORMAT_XRGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT,prime.fd,EGL_DMA_BUF_PLANE0_OFFSET_EXT,0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT,create.pitch,EGL_NONE};
    EGLImageKHR img=create_image(display,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,NULL,attrs);
    EGL_CHECK(img!=EGL_NO_IMAGE_KHR);
    GLuint texture,framebuffer;
    p_glGenTextures(1,&texture);p_glBindTexture(GL_TEXTURE_2D,texture);
    image_texture(GL_TEXTURE_2D,img);
    p_glGenFramebuffers(1,&framebuffer);p_glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    if(p_glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) return 1;
    p_glViewport(0,0,32,32);
    int stale=0;
    for(int phase=0;phase<3;phase++) {
        struct dma_buf_sync sync={.flags=DMA_BUF_SYNC_START|DMA_BUF_SYNC_READ};
        if(ioctl(prime.fd,DMA_BUF_IOCTL_SYNC,&sync)) {perror("CPU start");return 1;}
        uint32_t warm=0;
        for(unsigned y=0;y<32;y++) for(unsigned x=0;x<32;x++) warm^=cpu[y*(create.pitch/4)+x];
        sync.flags=DMA_BUF_SYNC_END|DMA_BUF_SYNC_READ;
        if(ioctl(prime.fd,DMA_BUF_IOCTL_SYNC,&sync)) return 1;
        p_glClearColor(phase==0,phase==1,phase==2,1);p_glClear(GL_COLOR_BUFFER_BIT);p_glFinish();
        if(p_glGetError()!=GL_NO_ERROR) return 1;
        sync.flags=DMA_BUF_SYNC_START|DMA_BUF_SYNC_READ;
        if(ioctl(prime.fd,DMA_BUF_IOCTL_SYNC,&sync)) return 1;
        uint32_t expected=0xff0000U>>(phase*8),mismatch=0,observed=cpu[8*(create.pitch/4)+8];
        for(unsigned y=0;y<32;y++) for(unsigned x=0;x<32;x++)
            mismatch+=((cpu[y*(create.pitch/4)+x]&0xffffff)!=expected);
        sync.flags=DMA_BUF_SYNC_END|DMA_BUF_SYNC_READ;
        if(ioctl(prime.fd,DMA_BUF_IOCTL_SYNC,&sync)) return 1;
        unsigned char pixel[4];p_glReadPixels(8,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        printf("phase=%d warm=%08x expected=%06x cpu=%08x mismatched=%u/1024 GPU_RGBA=%u,%u,%u,%u\n",
            phase,warm,expected,observed,mismatch,pixel[0],pixel[1],pixel[2],pixel[3]);
        /* Imported linear-image ReadPixels may also use a CPU mapping, so it
         * is diagnostic output, not an independent GPU-side oracle here. */
        if(p_glGetError()!=GL_NO_ERROR) return 1;
        stale+=!!mismatch;
    }
    p_glDeleteFramebuffers(1,&framebuffer);p_glDeleteTextures(1,&texture);destroy_image(display,img);
    munmap((void *)cpu,create.size);close(prime.fd);
    struct drm_mode_destroy_dumb destroy={.handle=create.handle};ioctl(fd,DRM_IOCTL_MODE_DESTROY_DUMB,&destroy);
    p_eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    p_eglDestroyContext(display,context);p_eglTerminate(display);destroy_gbm(device);close(fd);
    puts(stale?"FAIL: CPU observed stale display-owned GPU pixels":"PASS: display-owned GPU writes visible to CPU in every phase");
    return stale?2:0;
}
