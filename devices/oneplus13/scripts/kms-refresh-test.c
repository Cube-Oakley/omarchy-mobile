// SPDX-License-Identifier: GPL-2.0-only
/* Bounded display-only motion test: immutable CPU-created dumb buffers.
 * No /dev/mem or private display ioctls. Only the oneplus13-scanout DRM driver.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#include <drm.h>
#include <drm_mode.h>
#include <drm_fourcc.h>

struct buffer { uint32_t handle, fb, pitch; uint64_t size; uint32_t *map; };
static void die(const char *s) { perror(s); exit(1); }
static void call(int fd, unsigned long request, void *arg, const char *name)
{ if (ioctl(fd, request, arg)) die(name); }
static struct buffer make_buffer(int fd, unsigned int phase)
{
    struct drm_mode_create_dumb create = {.width=1440,.height=3168,.bpp=32};
    call(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create, "CREATE_DUMB");
    struct drm_mode_map_dumb map = {.handle=create.handle};
    call(fd, DRM_IOCTL_MODE_MAP_DUMB, &map, "MAP_DUMB");
    struct buffer b = {.handle=create.handle,.pitch=create.pitch,.size=create.size};
    b.map = mmap(NULL, b.size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, map.offset);
    if (b.map == MAP_FAILED) die("mmap");
    const uint32_t colors[] = {0xffff3030,0xff30e060,0xff3080ff};
    for (unsigned int y=0; y<3168; y++)
        for (unsigned int x=0; x<1440; x++) {
            uint32_t color=0xff101820;
            if (y>=1250 && y<2400 && x>=phase*150+80 && x<phase*150+380)
                color=colors[(y-1250)/400];
            b.map[y*(b.pitch/4)+x]=color;
        }
    struct drm_mode_fb_cmd2 fb = {.width=1440,.height=3168,.pixel_format=DRM_FORMAT_XRGB8888};
    fb.handles[0]=b.handle; fb.pitches[0]=b.pitch;
    call(fd, DRM_IOCTL_MODE_ADDFB2, &fb, "ADDFB2"); b.fb=fb.fb_id;
    printf("buffer phase=%u fb=%u handle=%u pitch=%u bytes=%llu\n",phase,b.fb,b.handle,b.pitch,(unsigned long long)b.size);
    return b;
}
int main(int argc, char **argv)
{
    unsigned int hz = argc > 2 ? strtoul(argv[2], NULL, 10) : 120;
    if (hz != 60 && hz != 120) return 1;
    unsigned int seconds = argc>1 ? strtoul(argv[1],NULL,10) : 30;
    if (seconds<1 || seconds>120) { fputs("duration must be 1..120\n",stderr); return 1; }
    setvbuf(stdout,NULL,_IOLBF,0);
    int fd=open(argc > 3 ? argv[3] : "/dev/dri/card2",O_RDWR|O_CLOEXEC);
    if(fd<0) die("open DRM");
    char name[64]={0}; struct drm_version v={.name_len=sizeof(name)-1,.name=name};
    call(fd, DRM_IOCTL_VERSION, &v, "VERSION");
    if(strcmp(name,"oneplus13-scanout")) { fprintf(stderr,"Refusing driver %s\n",name); return 1; }
    call(fd, DRM_IOCTL_SET_MASTER, NULL, "SET_MASTER");
    struct drm_mode_card_res res={0};
    call(fd,DRM_IOCTL_MODE_GETRESOURCES,&res,"GETRESOURCES counts");
    if(res.count_connectors!=1 || res.count_crtcs!=1) { fputs("unexpected resource counts\n",stderr); return 1; }
    uint32_t connector=0,crtc=0;
    res.connector_id_ptr=(uintptr_t)&connector; res.crtc_id_ptr=(uintptr_t)&crtc;
    res.count_encoders=0; res.count_fbs=0;
    call(fd,DRM_IOCTL_MODE_GETRESOURCES,&res,"GETRESOURCES");
    struct drm_mode_get_connector conn={.connector_id=connector};
    call(fd,DRM_IOCTL_MODE_GETCONNECTOR,&conn,"GETCONNECTOR counts");
    if (!conn.count_modes || conn.count_modes > 8 || conn.connection != 1) {
        fputs("unexpected modes\n", stderr); return 1;
    }
    struct drm_mode_modeinfo modes[8] = {0}, mode = {0};
    conn.modes_ptr = (uintptr_t)modes; conn.count_props = conn.count_encoders = 0;
    call(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn, "GETCONNECTOR modes");
    for (unsigned int i = 0; i < conn.count_modes; i++)
        if (modes[i].hdisplay == 1440 && modes[i].vdisplay == 3168 && modes[i].vrefresh == hz)
            mode = modes[i];
    if (!mode.clock) { fputs("requested mode missing\n", stderr); return 1; }
    printf("driver=%s connector=%u crtc=%u mode=%s %ux%u@%u\n",
           name, connector, crtc, mode.name, mode.hdisplay, mode.vdisplay, mode.vrefresh);
    struct buffer b[4];
    for(unsigned int i=0;i<4;i++) b[i]=make_buffer(fd,i*2);
    struct drm_mode_crtc set={.set_connectors_ptr=(uintptr_t)&connector,.count_connectors=1,
        .crtc_id=crtc,.fb_id=b[0].fb,.mode_valid=1,.mode=mode};
    call(fd,DRM_IOCTL_MODE_SETCRTC,&set,"SETCRTC");
    puts("ONEPLUS13_KMS_MODESET_OK");
    struct timespec start,now;
    clock_gettime(CLOCK_MONOTONIC,&start);
    unsigned int frames=0;
    double elapsed=0;
    for(unsigned int i=1;;i++) {
        clock_gettime(CLOCK_MONOTONIC,&now);
        elapsed=now.tv_sec-start.tv_sec+(now.tv_nsec-start.tv_nsec)/1e9;
        if(elapsed>=seconds) break;
        unsigned int step=(i/3)%6,phase=step<4?step:6-step;
        struct drm_mode_crtc_page_flip flip={.crtc_id=crtc,.fb_id=b[phase].fb,.flags=DRM_MODE_PAGE_FLIP_EVENT,.user_data=i};
        call(fd,DRM_IOCTL_MODE_PAGE_FLIP,&flip,"PAGE_FLIP");
        struct pollfd p={.fd=fd,.events=POLLIN};
        if(poll(&p,1,3000)!=1 || !(p.revents&POLLIN)) { fputs("flip event timeout\n",stderr); return 1; }
        struct drm_event_vblank event;
        ssize_t n=read(fd,&event,sizeof(event));
        if(n!=sizeof(event) || event.base.type!=DRM_EVENT_FLIP_COMPLETE || event.user_data!=i) {
            fputs("unexpected DRM event\n",stderr); return 1;
        }
        frames++;
    }
    printf("ONEPLUS13_KMS_MOTION_OK frames=%u elapsed=%.3f rate=%.3f; immutable buffers only\n",
           frames,elapsed,frames/elapsed);
    struct drm_mode_crtc off={.crtc_id=crtc};
    call(fd,DRM_IOCTL_MODE_SETCRTC,&off,"DISABLE_CRTC");
    for (unsigned i=0;i<4;i++) {
        call(fd,DRM_IOCTL_MODE_RMFB,&b[i].fb,"RMFB");
        munmap(b[i].map,b[i].size);
        struct drm_mode_destroy_dumb destroy={.handle=b[i].handle};
        call(fd,DRM_IOCTL_MODE_DESTROY_DUMB,&destroy,"DESTROY_DUMB");
    }
    close(fd);
    puts("ONEPLUS13_KMS_RESTORE_OK");
    return 0;
}
