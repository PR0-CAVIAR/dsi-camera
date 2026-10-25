#ifndef VIDEO_H
#define VIDEO_H

#include "camera.h"

#include <nds/ndstypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VIDEO_DIR "/DCIM/VIDEO"

typedef enum { VIDEO_STOPPED, VIDEO_EXIT } VideoResult;

// Records the preview stream and the mic to VIDEO_DIR/REC_####.AVI until
// SELECT (stop) or START (stop and exit) is pressed. Frames are mirrored to the
// given 256x192 RGB15 framebuffer while recording.
VideoResult videoRecord(Camera camera, u16 *display);

#ifdef __cplusplus
}
#endif

#endif // VIDEO_H
