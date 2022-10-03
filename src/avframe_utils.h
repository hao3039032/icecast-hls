#ifndef AVFRAME_UTILS_H
#define AVFRAME_UTILS_H

/* provides an object that can convert our internal frames into AVFrames, and vice-versa */

#include "samplefmt.h"
#include "frame.h"

#include <stdint.h>

#include <libavutil/frame.h>

#ifdef __cplusplus
extern "C" {
#endif

int frame_to_avframe(AVFrame* out, const frame* in);
enum AVSampleFormat samplefmt_to_avsampleformat(samplefmt f);

int avframe_to_frame(frame* out, const AVFrame* in);
samplefmt avsampleformat_to_samplefmt(enum AVSampleFormat f);


#ifdef __cplusplus
}
#endif

#endif
