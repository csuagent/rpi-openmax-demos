/*
 * Copyright © 2013 Tuomas Jormola <tj@solitudo.net> <http://solitudo.net>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * <http://www.apache.org/licenses/LICENSE-2.0>
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Short intro about this program:
 *
 * `rpi-camera-playback` records video using the RaspiCam module and displays it
 * on the Raspberry Pi frame buffer display device, i.e. it should be run on the
 * Raspbian console.
 *
 *     $ ./rpi-camera-playback
 *
 * `rpi-camera-playback` uses `camera`, `video_render` and `null_sink` components.
 * `camera` video output port is tunneled to `video_render` input port and
 * `camera` preview output port is tunneled to `null_sink` input port.
 * `video_render` component uses a display region to show the video on local
 * display.
 *
 * Please see README.mdwn for more detailed description of this
 * OpenMAX IL demos for Raspberry Pi bundle.
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <bcm_host.h>

#include <interface/vcos/vcos_semaphore.h>
#include <interface/vmcs_host/vchost.h>

#include <IL/OMX_Core.h>
#include <IL/OMX_Component.h>
#include <IL/OMX_Video.h>
#include <IL/OMX_Broadcom.h>

// Hard coded parameters
#define VIDEO_FRAMERATE                 25
#define VIDEO_BITRATE                   10000000
#define CAM_DEVICE_NUMBER               0
#define CAM_SHARPNESS                   0                       // -100 .. 100
#define CAM_CONTRAST                    0                       // -100 .. 100
#define CAM_BRIGHTNESS                  50                      // 0 .. 100
#define CAM_SATURATION                  0                       // -100 .. 100
#define CAM_EXPOSURE_VALUE_COMPENSTAION 0
#define CAM_EXPOSURE_ISO_SENSITIVITY    100
#define CAM_EXPOSURE_AUTO_SENSITIVITY   OMX_FALSE
#define CAM_FRAME_STABILISATION         OMX_TRUE
#define CAM_WHITE_BALANCE_CONTROL       OMX_WhiteBalControlAuto // OMX_WHITEBALCONTROLTYPE
#define CAM_IMAGE_FILTER                OMX_ImageFilterNoise    // OMX_IMAGEFILTERTYPE
#define CAM_FLIP_HORIZONTAL             OMX_FALSE
#define CAM_FLIP_VERTICAL               OMX_FALSE
#define DISPLAY_DEVICE                  0

// Dunno where this is originally stolen from...
#define OMX_INIT_STRUCTURE(a) \
    memset(&(a), 0, sizeof(a)); \
    (a).nSize = sizeof(a); \
    (a).nVersion.nVersion = OMX_VERSION; \
    (a).nVersion.s.nVersionMajor = OMX_VERSION_MAJOR; \
    (a).nVersion.s.nVersionMinor = OMX_VERSION_MINOR; \
    (a).nVersion.s.nRevision = OMX_VERSION_REVISION; \
    (a).nVersion.s.nStep = OMX_VERSION_STEP

// Global variable used by the signal handler and capture/encoding loop
static int want_quit = 0;

// Our application context passed around
// the main routine and callback handlers
typedef struct {
    OMX_HANDLETYPE camera;
    OMX_BUFFERHEADERTYPE *camera_ppBuffer_in;
    int camera_ready;
    OMX_HANDLETYPE render;
    OMX_HANDLETYPE null_sink;
    int flushed;
    VCOS_SEMAPHORE_T handler_lock;
} appctx;

// Ugly, stupid utility functions
static void say(const char* message, ...) {
    va_list args;
    char str[1024];
    memset(str, 0, sizeof(str));
    va_start(args, message);
    vsnprintf(str, sizeof(str) - 1, message, args);
    va_end(args);
    size_t str_len = strnlen(str, sizeof(str));
    if(str[str_len - 1] != '\n') {
        str[str_len] = '\n';
    }
    fprintf(stderr, str);
}

static void die(const char* message, ...) {
    va_list args;
    char str[1024];
    memset(str, 0, sizeof(str));
    va_start(args, message);
    vsnprintf(str, sizeof(str), message, args);
    va_end(args);
    say(str);
    exit(1);
}

static void omx_die(OMX_ERRORTYPE error, const char* message, ...) {
    va_list args;
    char str[1024];
    char *e;
    memset(str, 0, sizeof(str));
    va_start(args, message);
    vsnprintf(str, sizeof(str), message, args);
    va_end(args);
    switch(error) {
        case OMX_ErrorNone:                     e = "no error";                                      break;
        case OMX_ErrorBadParameter:             e = "bad parameter";                                 break;
        case OMX_ErrorIncorrectStateOperation:  e = "invalid state while trying to perform command"; break;
        case OMX_ErrorIncorrectStateTransition: e = "unallowed state transition";                    break;
        case OMX_ErrorInsufficientResources:    e = "insufficient resource";                         break;
        case OMX_ErrorBadPortIndex:             e = "bad port index, i.e. incorrect port";           break;
        case OMX_ErrorHardware:                 e = "hardware error";                                break;
        /* That's all I've encountered during hacking so let's not bother with the rest... */
        default:                                e = "(no description)";
    }
    die("OMX error: %s: 0x%08x %s", str, error, e);
}

static void dump_event(OMX_HANDLETYPE hComponent, OMX_EVENTTYPE eEvent, OMX_U32 nData1, OMX_U32 nData2) {
    char *e;
    switch(eEvent) {
        case OMX_EventCmdComplete:          e = "command complete";                   break;
        case OMX_EventError:                e = "error";                              break;
        case OMX_EventParamOrConfigChanged: e = "parameter or configuration changed"; break;
        case OMX_EventPortSettingsChanged:  e = "port settings changed";              break;
        /* That's all I've encountered during hacking so let's not bother with the rest... */
        default:
            e = "(no description)";
    }
    say("Received event 0x%08x %s, hComponent:0x%08x, nData1:0x%08x, nData2:0x%08x",
            eEvent, e, hComponent, nData1, nData2);
}

static const char* dump_compression_format(OMX_VIDEO_CODINGTYPE c) {
    switch(c) {
        case OMX_VIDEO_CodingUnused:     return "not used";
        case OMX_VIDEO_CodingAutoDetect: return "autodetect";
        case OMX_VIDEO_CodingMPEG2:      return "MPEG2";
        case OMX_VIDEO_CodingH263:       return "H.263";
        case OMX_VIDEO_CodingMPEG4:      return "MPEG4";
        case OMX_VIDEO_CodingWMV:        return "Windows Media Video";
        case OMX_VIDEO_CodingRV:         return "RealVideo";
        case OMX_VIDEO_CodingAVC:        return "H.264";
        case OMX_VIDEO_CodingMJPEG:      return "MJPEG";
        case OMX_VIDEO_CodingMax:        return "maximum";
        default:                         return "unknown";
    }
}
static const char* dump_color_format(OMX_COLOR_FORMATTYPE c) {
    char *f;
    switch(c) {
        case OMX_COLOR_FormatUnused:             return "not used";
        case OMX_COLOR_FormatYUV420PackedPlanar: return "YUV420PackedPlanar";
        default:
            f = calloc(23, sizeof(char));
            if(f == NULL) {
                die("Failed to allocate memory");
            }
            snprintf(f, 23 * sizeof(char) - 1, "format type 0x%08x", c);
            return f;
    }
}

static void dump_portdef(OMX_PARAM_PORTDEFINITIONTYPE* portdef) {
    say("Port %d is %s, %s, buffers wants:%d needs:%d, size:%d, pop:%d, aligned:%d",
        portdef->nPortIndex,
        (portdef->eDir ==  OMX_DirInput ? "input" : "output"),
        (portdef->bEnabled == OMX_TRUE ? "enabled" : "disabled"),
        portdef->nBufferCountActual,
        portdef->nBufferCountMin,
        portdef->nBufferSize,
        portdef->bPopulated,
        portdef->nBufferAlignment);

    OMX_VIDEO_PORTDEFINITIONTYPE *viddef = &portdef->format.video;
    OMX_IMAGE_PORTDEFINITIONTYPE *imgdef = &portdef->format.image;
    switch(portdef->eDomain) {
        case OMX_PortDomainVideo:
            say("Video type:\n"
                "\tWidth:\t\t%d\n"
                "\tHeight:\t\t%d\n"
                "\tStride:\t\t%d\n"
                "\tSliceHeight:\t%d\n"
                "\tBitrate:\t%d\n"
                "\tFramerate:\t%.02f\n"
                "\tError hiding:\t%s\n"
                "\tCodec:\t\t%s\n"
                "\tColor:\t\t%s\n",
                viddef->nFrameWidth,
                viddef->nFrameHeight,
                viddef->nStride,
                viddef->nSliceHeight,
                viddef->nBitrate,
                ((float)viddef->xFramerate / (float)65536),
                (viddef->bFlagErrorConcealment == OMX_TRUE ? "yes" : "no"),
                dump_compression_format(viddef->eCompressionFormat),
                dump_color_format(viddef->eColorFormat));
            break;
        case OMX_PortDomainImage:
            say("Image type:\n"
                "\tWidth:\t\t%d\n"
                "\tHeight:\t\t%d\n"
                "\tStride:\t\t%d\n"
                "\tSliceHeight:\t%d\n"
                "\tError hiding:\t%s\n"
                "\tCodec:\t\t%s\n"
                "\tColor:\t\t%s\n",
                imgdef->nFrameWidth,
                imgdef->nFrameHeight,
                imgdef->nStride,
                imgdef->nSliceHeight,
                (imgdef->bFlagErrorConcealment == OMX_TRUE ? "yes" : "no"),
                dump_compression_format(imgdef->eCompressionFormat),
                dump_color_format(imgdef->eColorFormat));
            break;
        default:
            break;
    }
}

static void dump_port(OMX_HANDLETYPE hComponent, OMX_U32 nPortIndex) {
    OMX_ERRORTYPE r;
    OMX_PARAM_PORTDEFINITIONTYPE portdef;
    OMX_INIT_STRUCTURE(portdef);
    portdef.nPortIndex = nPortIndex;
    if((r = OMX_GetParameter(hComponent, OMX_IndexParamPortDefinition, &portdef)) != OMX_ErrorNone) {
        omx_die(r, "Failed to get port definition for port %d", nPortIndex);
    }
    dump_portdef(&portdef);
}

// Some busy loops to verify we're running in order
static void block_until_state_changed(OMX_HANDLETYPE hComponent, OMX_STATETYPE wanted_eState) {
    OMX_STATETYPE eState;
    int i = 0;
    while(i++ == 0 || eState != wanted_eState) {
        OMX_GetState(hComponent, &eState);
        if(eState != wanted_eState) {
            usleep(10000);
        }
    }
}

static void block_until_port_changed(OMX_HANDLETYPE hComponent, OMX_U32 nPortIndex, OMX_BOOL bEnabled) {
    OMX_ERRORTYPE r;
    OMX_PARAM_PORTDEFINITIONTYPE portdef;
    OMX_INIT_STRUCTURE(portdef);
    portdef.nPortIndex = nPortIndex;
    OMX_U32 i = 0;
    while(i++ == 0 || portdef.bEnabled != bEnabled) {
        if((r = OMX_GetParameter(hComponent, OMX_IndexParamPortDefinition, &portdef)) != OMX_ErrorNone) {
            omx_die(r, "Failed to get port definition");
        }
        if(portdef.bEnabled != bEnabled) {
            usleep(10000);
        }
    }
}

static void block_until_flushed(appctx *ctx) {
    int quit;
    while(!quit) {
        vcos_semaphore_wait(&ctx->handler_lock);
        if(ctx->flushed) {
            ctx->flushed = 0;
            quit = 1;
        }
        vcos_semaphore_post(&ctx->handler_lock);
        if(!quit) {
            usleep(10000);
        }
    }
}

static void init_component_handle(
        const char *name,
        OMX_HANDLETYPE* hComponent,
        OMX_PTR pAppData,
        OMX_CALLBACKTYPE* callbacks) {
    OMX_ERRORTYPE r;
    char fullname[32];

    // Get handle
    memset(fullname, 0, sizeof(fullname));
    strcat(fullname, "OMX.broadcom.");
    strncat(fullname, name, strlen(fullname) - 1);
    say("Initializing component %s", fullname);
    if((r = OMX_GetHandle(hComponent, fullname, pAppData, callbacks)) != OMX_ErrorNone) {
        omx_die(r, "Failed to get handle for component %s", fullname);
    }

    // Disable ports
    OMX_INDEXTYPE types[] = {
        OMX_IndexParamAudioInit,
        OMX_IndexParamVideoInit,
        OMX_IndexParamImageInit,
        OMX_IndexParamOtherInit
    };
    OMX_PORT_PARAM_TYPE ports;
    OMX_INIT_STRUCTURE(ports);
    OMX_GetParameter(*hComponent, OMX_IndexParamVideoInit, &ports);

    int i;
    for(i = 0; i < 4; i++) {
        if(OMX_GetParameter(*hComponent, types[i], &ports) == OMX_ErrorNone) {
            OMX_U32 nPortIndex;
            for(nPortIndex = ports.nStartPortNumber; nPortIndex < ports.nStartPortNumber + ports.nPorts; nPortIndex++) {
                say("Disabling port %d of component %s", nPortIndex, fullname);
                if((r = OMX_SendCommand(*hComponent, OMX_CommandPortDisable, nPortIndex, NULL)) != OMX_ErrorNone) {
                    omx_die(r, "Failed to disable port %d of component %s", nPortIndex, fullname);
                }
                block_until_port_changed(*hComponent, nPortIndex, OMX_FALSE);
            }
        }
    }
}

// Global signal handler for trapping SIGINT, SIGTERM, and SIGQUIT
static void signal_handler(int signal) {
    want_quit = 1;
}

// OMX calls this handler for all the events it emits
static OMX_ERRORTYPE event_handler(
        OMX_HANDLETYPE hComponent,
        OMX_PTR pAppData,
        OMX_EVENTTYPE eEvent,
        OMX_U32 nData1,
        OMX_U32 nData2,
        OMX_PTR pEventData) {

    dump_event(hComponent, eEvent, nData1, nData2);

    appctx *ctx = (appctx *)pAppData;

    switch(eEvent) {
        case OMX_EventCmdComplete:
            vcos_semaphore_wait(&ctx->handler_lock);
            if(nData1 == OMX_CommandFlush) {
                ctx->flushed = 1;
            }
            vcos_semaphore_post(&ctx->handler_lock);
            break;
        case OMX_EventParamOrConfigChanged:
            vcos_semaphore_wait(&ctx->handler_lock);
            if(nData2 == OMX_IndexParamCameraDeviceNumber) {
                ctx->camera_ready = 1;
            }
            vcos_semaphore_post(&ctx->handler_lock);
            break;
        case OMX_EventError:
            omx_die(nData1, "error event received");
            break;
        default:
            break;
    }

    return OMX_ErrorNone;
}

int main(int argc, char **argv) {
    bcm_host_init();

    OMX_ERRORTYPE r;

    if((r = OMX_Init()) != OMX_ErrorNone) {
        omx_die(r, "OMX initalization failed");
    }

    // Init context
    appctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    if(vcos_semaphore_create(&ctx.handler_lock, "handler_lock", 1) != VCOS_SUCCESS) {
        die("Failed to create handler lock semaphore");
    }

    // Init component handles
    OMX_CALLBACKTYPE callbacks;
    memset(&ctx, 0, sizeof(callbacks));
    callbacks.EventHandler = event_handler;

    init_component_handle("camera", &ctx.camera , &ctx, &callbacks);
    init_component_handle("video_render", &ctx.render, &ctx, &callbacks);
    init_component_handle("null_sink", &ctx.null_sink, &ctx, &callbacks);

    OMX_U32 screen_width = 0, screen_height = 0;
    if(graphics_get_display_size(DISPLAY_DEVICE, &screen_width, &screen_height) < 0) {
        die("Failed to get display size");
    }

    say("Configuring camera...");

    say("Default port definition for camera input port 73");
    dump_port(ctx.camera, 73);
    say("Default port definition for camera preview output port 70");
    dump_port(ctx.camera, 70);
    say("Default port definition for camera video output port 71");
    dump_port(ctx.camera, 71);

    // Request a callback to be made when OMX_IndexParamCameraDeviceNumber is
    // changed signaling that the camera device is ready for use.
    OMX_CONFIG_REQUESTCALLBACKTYPE cbtype;
    OMX_INIT_STRUCTURE(cbtype);
    cbtype.nPortIndex = OMX_ALL;
    cbtype.nIndex     = OMX_IndexParamCameraDeviceNumber;
    cbtype.bEnable    = OMX_TRUE;
    if((r = OMX_SetConfig(ctx.camera, OMX_IndexConfigRequestCallback, &cbtype)) != OMX_ErrorNone) {
        omx_die(r, "Failed to request camera device number parameter change callback for camera");
    }
    // Set device number, this triggers the callback configured just above
    OMX_PARAM_U32TYPE device;
    OMX_INIT_STRUCTURE(device);
    device.nPortIndex = OMX_ALL;
    device.nU32 = CAM_DEVICE_NUMBER;
    if((r = OMX_SetParameter(ctx.camera, OMX_IndexParamCameraDeviceNumber, &device)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set camera parameter device number");
    }
    // Configure video format emitted by camera preview output port
    OMX_PARAM_PORTDEFINITIONTYPE camera_portdef;
    OMX_INIT_STRUCTURE(camera_portdef);
    camera_portdef.nPortIndex = 70;
    if((r = OMX_GetParameter(ctx.camera, OMX_IndexParamPortDefinition, &camera_portdef)) != OMX_ErrorNone) {
        omx_die(r, "Failed to get port definition for camera preview output port 70");
    }
    camera_portdef.format.video.nFrameWidth  = screen_width / 2;
    camera_portdef.format.video.nFrameHeight = screen_height / 2;
    camera_portdef.format.video.xFramerate   = VIDEO_FRAMERATE << 16;
    camera_portdef.format.video.nStride      = camera_portdef.format.image.nFrameWidth;
    if((r = OMX_SetParameter(ctx.camera, OMX_IndexParamPortDefinition, &camera_portdef)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set port definition for camera preview output port 70");
    }
    // Configure video format emitted by camera video output port
    // Use configuration from camera preview output as basis for
    // camera video output configuration
    OMX_INIT_STRUCTURE(camera_portdef);
    camera_portdef.nPortIndex = 70;
    if((r = OMX_GetParameter(ctx.camera, OMX_IndexParamPortDefinition, &camera_portdef)) != OMX_ErrorNone) {
        omx_die(r, "Failed to get port definition for camera preview output port 70");
    }
    camera_portdef.nPortIndex = 71;
    if((r = OMX_SetParameter(ctx.camera, OMX_IndexParamPortDefinition, &camera_portdef)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set port definition for camera video output port 71");
    }
    // Configure frame rate
    OMX_CONFIG_FRAMERATETYPE framerate;
    OMX_INIT_STRUCTURE(framerate);
    framerate.nPortIndex = 70;
    framerate.xEncodeFramerate = camera_portdef.format.video.xFramerate;
    if((r = OMX_SetConfig(ctx.camera, OMX_IndexConfigVideoFramerate, &framerate)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set framerate configuration for camera preview output port 70");
    }
    framerate.nPortIndex = 71;
    if((r = OMX_SetConfig(ctx.camera, OMX_IndexConfigVideoFramerate, &framerate)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set framerate configuration for camera video output port 71");
    }
    // Configure sharpness
    OMX_CONFIG_SHARPNESSTYPE sharpness;
    OMX_INIT_STRUCTURE(sharpness);
    sharpness.nPortIndex = OMX_ALL;
    sharpness.nSharpness = CAM_SHARPNESS;
    if((r = OMX_SetConfig(ctx.camera, OMX_IndexConfigCommonSharpness, &sharpness)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set camera sharpness configuration");
    }
    // Configure contrast
    OMX_CONFIG_CONTRASTTYPE contrast;
    OMX_INIT_STRUCTURE(contrast);
    contrast.nPortIndex = OMX_ALL;
    contrast.nContrast = CAM_CONTRAST;
    if((r = OMX_SetConfig(ctx.camera, OMX_IndexConfigCommonContrast, &contrast)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set camera contrast configuration");
    }
    // Configure saturation
    OMX_CONFIG_SATURATIONTYPE saturation;
    OMX_INIT_STRUCTURE(saturation);
    saturation.nPortIndex = OMX_ALL;
    saturation.nSaturation = CAM_SATURATION;
    if((r = OMX_SetConfig(ctx.camera, OMX_IndexConfigCommonSaturation, &saturation)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set camera saturation configuration");
    }
    // Configure brightness
    OMX_CONFIG_BRIGHTNESSTYPE brightness;
    OMX_INIT_STRUCTURE(brightness);
    brightness.nPortIndex = OMX_ALL;
    brightness.nBrightness = CAM_BRIGHTNESS;
    if((r = OMX_SetConfig(ctx.camera, OMX_IndexConfigCommonBrightness, &brightness)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set camera brightness configuration");
    }
    // Configure exposure value
    OMX_CONFIG_EXPOSUREVALUETYPE exposure_value;
    OMX_INIT_STRUCTURE(exposure_value);
    exposure_value.nPortIndex = OMX_ALL;
    exposure_value.xEVCompensation = CAM_EXPOSURE_VALUE_COMPENSTAION;
    exposure_value.bAutoSensitivity = CAM_EXPOSURE_AUTO_SENSITIVITY;
    exposure_value.nSensitivity = CAM_EXPOSURE_ISO_SENSITIVITY;
    if((r = OMX_SetConfig(ctx.camera, OMX_IndexConfigCommonExposureValue, &exposure_value)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set camera exposure value configuration");
    }
    // Configure frame frame stabilisation
    OMX_CONFIG_FRAMESTABTYPE frame_stabilisation_control;
    OMX_INIT_STRUCTURE(frame_stabilisation_control);
    frame_stabilisation_control.nPortIndex = OMX_ALL;
    frame_stabilisation_control.bStab = CAM_FRAME_STABILISATION;
    if((r = OMX_SetConfig(ctx.camera, OMX_IndexConfigCommonFrameStabilisation, &frame_stabilisation_control)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set camera frame frame stabilisation control configuration");
    }
    // Configure frame white balance control
    OMX_CONFIG_WHITEBALCONTROLTYPE white_balance_control;
    OMX_INIT_STRUCTURE(white_balance_control);
    white_balance_control.nPortIndex = OMX_ALL;
    white_balance_control.eWhiteBalControl = CAM_WHITE_BALANCE_CONTROL;
    if((r = OMX_SetConfig(ctx.camera, OMX_IndexConfigCommonWhiteBalance, &white_balance_control)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set camera frame white balance control configuration");
    }
    // Configure image filter
    OMX_CONFIG_IMAGEFILTERTYPE image_filter;
    OMX_INIT_STRUCTURE(image_filter);
    image_filter.nPortIndex = OMX_ALL;
    image_filter.eImageFilter = CAM_IMAGE_FILTER;
    if((r = OMX_SetConfig(ctx.camera, OMX_IndexConfigCommonImageFilter, &image_filter)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set camera image filter configuration");
    }
    // Configure mirror
    OMX_MIRRORTYPE eMirror = OMX_MirrorNone;
    if(CAM_FLIP_HORIZONTAL && !CAM_FLIP_VERTICAL) {
        eMirror = OMX_MirrorHorizontal;
    } else if(!CAM_FLIP_HORIZONTAL && CAM_FLIP_VERTICAL) {
        eMirror = OMX_MirrorVertical;
    } else if(CAM_FLIP_HORIZONTAL && CAM_FLIP_VERTICAL) {
        eMirror = OMX_MirrorBoth;
    }
    OMX_CONFIG_MIRRORTYPE mirror;
    OMX_INIT_STRUCTURE(mirror);
    mirror.nPortIndex = 71;
    mirror.eMirror = eMirror;
    if((r = OMX_SetConfig(ctx.camera, OMX_IndexConfigCommonMirror, &mirror)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set mirror configuration for camera video output port 71");
    }

    // Ensure camera is ready
    while(!ctx.camera_ready) {
        usleep(10000);
    }

    say("Configuring render...");

    say("Default port definition for render input port 90");
    dump_port(ctx.render, 90);

    // Render input port definition is done automatically upon tunneling

    // Configure display region
    OMX_CONFIG_DISPLAYREGIONTYPE display_region;
    OMX_INIT_STRUCTURE(display_region);
    display_region.nPortIndex = 90;
    display_region.set = OMX_DISPLAY_SET_NUM | OMX_DISPLAY_SET_FULLSCREEN | OMX_DISPLAY_SET_MODE | OMX_DISPLAY_SET_DEST_RECT;
    display_region.num = DISPLAY_DEVICE;
    display_region.fullscreen = OMX_FALSE;
    display_region.mode = OMX_DISPLAY_MODE_FILL;
    display_region.dest_rect.width = camera_portdef.format.video.nFrameWidth;
    display_region.dest_rect.height = camera_portdef.format.video.nFrameHeight;
    display_region.dest_rect.x_offset = display_region.dest_rect.width / 2;
    display_region.dest_rect.y_offset = display_region.dest_rect.height / 2;
    if((r = OMX_SetConfig(ctx.render, OMX_IndexConfigDisplayRegion, &display_region)) != OMX_ErrorNone) {
        omx_die(r, "Failed to set display region for render output port 90");
    }

    say("Configuring null sink...");

    say("Default port definition for null sink input port 240");
    dump_port(ctx.null_sink, 240);

    // Null sink input port definition is done automatically upon tunneling

    // Tunnel camera preview output port and null sink input port
    say("Setting up tunnel from camera preview output port 70 to null sink input port 240...");
    if((r = OMX_SetupTunnel(ctx.camera, 70, ctx.null_sink, 240)) != OMX_ErrorNone) {
        omx_die(r, "Failed to setup tunnel between camera preview output port 70 and null sink input port 240");
    }

    // Tunnel camera video output port and render input port
    say("Setting up tunnel from camera video output port 71 to render input port 90...");
    if((r = OMX_SetupTunnel(ctx.camera, 71, ctx.render, 90)) != OMX_ErrorNone) {
        omx_die(r, "Failed to setup tunnel between camera video output port 71 and render input port 90");
    }

    // Switch components to idle state
    say("Switching state of the camera component to idle...");
    if((r = OMX_SendCommand(ctx.camera, OMX_CommandStateSet, OMX_StateIdle, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the camera component to idle");
    }
    block_until_state_changed(ctx.camera, OMX_StateIdle);
    say("Switching state of the render component to idle...");
    if((r = OMX_SendCommand(ctx.render, OMX_CommandStateSet, OMX_StateIdle, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the render component to idle");
    }
    block_until_state_changed(ctx.render, OMX_StateIdle);
    say("Switching state of the null sink component to idle...");
    if((r = OMX_SendCommand(ctx.null_sink, OMX_CommandStateSet, OMX_StateIdle, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the null sink component to idle");
    }
    block_until_state_changed(ctx.null_sink, OMX_StateIdle);

    // Enable ports
    say("Enabling ports...");
    if((r = OMX_SendCommand(ctx.camera, OMX_CommandPortEnable, 73, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to enable camera input port 73");
    }
    block_until_port_changed(ctx.camera, 73, OMX_TRUE);
    if((r = OMX_SendCommand(ctx.camera, OMX_CommandPortEnable, 70, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to enable camera preview output port 70");
    }
    block_until_port_changed(ctx.camera, 70, OMX_TRUE);
    if((r = OMX_SendCommand(ctx.camera, OMX_CommandPortEnable, 71, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to enable camera video output port 71");
    }
    block_until_port_changed(ctx.camera, 71, OMX_TRUE);
    if((r = OMX_SendCommand(ctx.render, OMX_CommandPortEnable, 90, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to enable render input port 90");
    }
    block_until_port_changed(ctx.render, 90, OMX_TRUE);
    if((r = OMX_SendCommand(ctx.null_sink, OMX_CommandPortEnable, 240, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to enable null sink input port 240");
    }
    block_until_port_changed(ctx.null_sink, 240, OMX_TRUE);

    // Allocate camera input buffer, buffers for tunneled
    // ports are allocated internally by OMX
    say("Allocating buffers...");
    OMX_INIT_STRUCTURE(camera_portdef);
    camera_portdef.nPortIndex = 73;
    if((r = OMX_GetParameter(ctx.camera, OMX_IndexParamPortDefinition, &camera_portdef)) != OMX_ErrorNone) {
        omx_die(r, "Failed to get port definition for camera input port 73");
    }
    if((r = OMX_AllocateBuffer(ctx.camera, &ctx.camera_ppBuffer_in, 73, NULL, camera_portdef.nBufferSize)) != OMX_ErrorNone) {
        omx_die(r, "Failed to allocate buffer for camera input port 73");
    }

    // Switch state of the components prior to starting
    // the video capture and encoding loop
    say("Switching state of the camera component to executing...");
    if((r = OMX_SendCommand(ctx.camera, OMX_CommandStateSet, OMX_StateExecuting, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the camera component to executing");
    }
    block_until_state_changed(ctx.camera, OMX_StateExecuting);
    say("Switching state of the render component to executing...");
    if((r = OMX_SendCommand(ctx.render, OMX_CommandStateSet, OMX_StateExecuting, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the render component to executing");
    }
    block_until_state_changed(ctx.render, OMX_StateExecuting);
    say("Switching state of the null sink component to executing...");
    if((r = OMX_SendCommand(ctx.null_sink, OMX_CommandStateSet, OMX_StateExecuting, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the null sink component to executing");
    }
    block_until_state_changed(ctx.null_sink, OMX_StateExecuting);

    // Start capturing video with the camera
    say("Switching on capture on camera video output port 71...");
    OMX_CONFIG_PORTBOOLEANTYPE capture;
    OMX_INIT_STRUCTURE(capture);
    capture.nPortIndex = 71;
    capture.bEnabled = OMX_TRUE;
    if((r = OMX_SetParameter(ctx.camera, OMX_IndexConfigPortCapturing, &capture)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch on capture on camera video output port 71");
    }

    say("Configured port definition for camera input port 73");
    dump_port(ctx.camera, 73);
    say("Configured port definition for camera preview output port 70");
    dump_port(ctx.camera, 70);
    say("Configured port definition for camera video output port 71");
    dump_port(ctx.camera, 71);
    say("Configured port definition for render input port 90");
    dump_port(ctx.render, 90);
    say("Configured port definition for null sink input port 240");
    dump_port(ctx.null_sink, 240);

    say("Enter capture and playback loop, press Ctrl-C to quit...");

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGQUIT, signal_handler);
    while(!want_quit) {
        // Would be better to use signaling here but hey this works too
        usleep(1000);
    }
    say("Cleaning up...");

    // Restore signal handlers
    signal(SIGINT,  SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);

    // Stop capturing video with the camera
    OMX_INIT_STRUCTURE(capture);
    capture.nPortIndex = 71;
    capture.bEnabled = OMX_FALSE;
    if((r = OMX_SetParameter(ctx.camera, OMX_IndexConfigPortCapturing, &capture)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch off capture on camera video output port 71");
    }

    // Flush the buffers on each component
    if((r = OMX_SendCommand(ctx.camera, OMX_CommandFlush, 73, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to flush buffers of camera input port 73");
    }
    block_until_flushed(&ctx);
    if((r = OMX_SendCommand(ctx.camera, OMX_CommandFlush, 70, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to flush buffers of camera preview output port 70");
    }
    block_until_flushed(&ctx);
    if((r = OMX_SendCommand(ctx.camera, OMX_CommandFlush, 71, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to flush buffers of camera video output port 71");
    }
    block_until_flushed(&ctx);
    if((r = OMX_SendCommand(ctx.render, OMX_CommandFlush, 90, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to flush buffers of render input port 90");
    }
    block_until_flushed(&ctx);
    if((r = OMX_SendCommand(ctx.null_sink, OMX_CommandFlush, 240, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to flush buffers of null sink input port 240");
    }
    block_until_flushed(&ctx);

    // Disable all the ports
    if((r = OMX_SendCommand(ctx.camera, OMX_CommandPortDisable, 73, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to disable camera input port 73");
    }
    block_until_port_changed(ctx.camera, 73, OMX_FALSE);
    if((r = OMX_SendCommand(ctx.camera, OMX_CommandPortDisable, 70, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to disable camera preview output port 70");
    }
    block_until_port_changed(ctx.camera, 70, OMX_FALSE);
    if((r = OMX_SendCommand(ctx.camera, OMX_CommandPortDisable, 71, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to disable camera video output port 71");
    }
    block_until_port_changed(ctx.camera, 71, OMX_FALSE);
    if((r = OMX_SendCommand(ctx.render, OMX_CommandPortDisable, 90, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to disable render input port 90");
    }
    if((r = OMX_SendCommand(ctx.null_sink, OMX_CommandPortDisable, 240, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to disable null sink input port 240");
    }
    block_until_port_changed(ctx.null_sink, 240, OMX_FALSE);

    // Free all the buffers
    if((r = OMX_FreeBuffer(ctx.camera, 73, ctx.camera_ppBuffer_in)) != OMX_ErrorNone) {
        omx_die(r, "Failed to free buffer for camera input port 73");
    }

    // Transition all the components to idle and then to loaded states
    if((r = OMX_SendCommand(ctx.camera, OMX_CommandStateSet, OMX_StateIdle, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the camera component to idle");
    }
    block_until_state_changed(ctx.camera, OMX_StateIdle);
    if((r = OMX_SendCommand(ctx.render, OMX_CommandStateSet, OMX_StateIdle, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the render component to idle");
    }
    block_until_state_changed(ctx.render, OMX_StateIdle);
    if((r = OMX_SendCommand(ctx.null_sink, OMX_CommandStateSet, OMX_StateIdle, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the null sink component to idle");
    }
    block_until_state_changed(ctx.null_sink, OMX_StateIdle);
    if((r = OMX_SendCommand(ctx.camera, OMX_CommandStateSet, OMX_StateLoaded, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the camera component to loaded");
    }
    block_until_state_changed(ctx.camera, OMX_StateLoaded);
    if((r = OMX_SendCommand(ctx.render, OMX_CommandStateSet, OMX_StateLoaded, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the render component to loaded");
    }
    block_until_state_changed(ctx.render, OMX_StateLoaded);
    if((r = OMX_SendCommand(ctx.null_sink, OMX_CommandStateSet, OMX_StateLoaded, NULL)) != OMX_ErrorNone) {
        omx_die(r, "Failed to switch state of the null sink component to loaded");
    }
    block_until_state_changed(ctx.null_sink, OMX_StateLoaded);

    // Free the component handles
    if((r = OMX_FreeHandle(ctx.camera)) != OMX_ErrorNone) {
        omx_die(r, "Failed to free camera component handle");
    }
    if((r = OMX_FreeHandle(ctx.render)) != OMX_ErrorNone) {
        omx_die(r, "Failed to free render component handle");
    }
    if((r = OMX_FreeHandle(ctx.null_sink)) != OMX_ErrorNone) {
        omx_die(r, "Failed to free null sink component handle");
    }

    // Exit
    vcos_semaphore_delete(&ctx.handler_lock);
    if((r = OMX_Deinit()) != OMX_ErrorNone) {
        omx_die(r, "OMX de-initalization failed");
    }

    say("Exit!");

    return 0;
}
