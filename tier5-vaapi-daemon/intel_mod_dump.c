#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <gbm.h>
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <drm_fourcc.h>

int main() {
    int fd = open("/dev/dri/renderD128", O_RDWR);
    struct gbm_device *gbm = gbm_create_device(fd);
    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    EGLint major, minor;
    eglInitialize(dpy, &major, &minor);
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC queryMods =
        (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
    EGLint num_mods = 0;
    queryMods(dpy, DRM_FORMAT_ABGR8888, 0, NULL, NULL, &num_mods);
    EGLuint64KHR *mods = calloc(num_mods, sizeof(EGLuint64KHR));
    EGLBoolean *external = calloc(num_mods, sizeof(EGLBoolean));
    queryMods(dpy, DRM_FORMAT_ABGR8888, num_mods, mods, external, &num_mods);
    printf("I915_FORMAT_MOD_X_TILED = 0x%016llx\n", (unsigned long long)0x0100000000000001ULL);
    printf("I915_FORMAT_MOD_Y_TILED = 0x%016llx\n", (unsigned long long)0x0100000000000002ULL);
    for (int i = 0; i < num_mods; i++) {
        printf("  modifier[%d] = 0x%016llx external_only=%d\n", i, (unsigned long long)mods[i], external[i]);
    }
    return 0;
}
