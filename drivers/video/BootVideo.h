#ifndef _BootVideo_H_
#define _BootVideo_H_

enum {
    VIDEO_MODE_UNKNOWN=-1,
    VIDEO_MODE_640x480=0,
    VIDEO_MODE_640x576,
    VIDEO_MODE_720x576,
    VIDEO_MODE_800x600,
    VIDEO_MODE_1024x576,
    VIDEO_MODE_COUNT
};

typedef struct {
        // fill on entry
    int m_nVideoModeIndex; // fill on entry to BootVgaInitializationKernel(), eg, VIDEO_MODE_800x600
    unsigned char m_fForceEncoderLumaAndChromaToZeroInitially; // fill on entry to BootVgaInitializationKernel(), 0=mode change visible immediately, !0=initially forced to black raster
    unsigned int m_dwFrameBufferStart; // frame buffer start address, set to zero to use default
    unsigned char * volatile m_pbBaseAddressVideo; // base address of video, usually 0xfd000000
        // filled on exit
    unsigned int width; // everything else filled by BootVgaInitializationKernel() on return
    unsigned int height;
    unsigned int xmargin;
    unsigned int ymargin;
    unsigned char m_bAvPack;
    unsigned int m_dwVideoFadeupTimer;
    double hoc;
    double voc;
    unsigned char m_bBPP;
} CURRENT_VIDEO_MODE_DETAILS;

void BootVgaInitializationKernelNG(CURRENT_VIDEO_MODE_DETAILS * pvmode);

#endif // _BootVideo_H_
