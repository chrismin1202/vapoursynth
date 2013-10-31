/*
* Copyright (c) 2012-2013 Fredrik Mellbin
*
* This file is part of VapourSynth.
*
* VapourSynth is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* VapourSynth is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with VapourSynth; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include "mergefilters.h"
#include "VSHelper.h"
#include "filtershared.h"
#include <stdlib.h>

//////////////////////////////////////////
// Merge

#ifdef VS_TARGET_CPU_X86
extern void vs_merge_uint8_sse2(const uint8_t *srcp1, const uint8_t *srcp2, int maskp, uint8_t *dstp, int stride, int height);
#endif

typedef struct {
    VSNodeRef *node1;
    VSNodeRef *node2;
    const VSVideoInfo *vi;
    int weight[3];
    float fweight[3];
    int process[3];
} MergeData;

const int MergeShift = 15;

static void VS_CC mergeInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    MergeData *d = (MergeData *)*instanceData;
    vsapi->setVideoInfo(d->vi, 1, node);
}

static const VSFrameRef *VS_CC mergeGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    MergeData *d = (MergeData *)*instanceData;

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node1, frameCtx);
        vsapi->requestFrameFilter(n, d->node2, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *src1 = vsapi->getFrameFilter(n, d->node1, frameCtx);
        const VSFrameRef *src2 = vsapi->getFrameFilter(n, d->node2, frameCtx);
        const int pl[] = {0, 1, 2};
        const VSFrameRef *fs[] = { 0, src1, src2 };
        const VSFrameRef *fr[] = {fs[d->process[0]], fs[d->process[1]], fs[d->process[2]]};
        VSFrameRef *dst = vsapi->newVideoFrame2(d->vi->format, d->vi->width, d->vi->height, fr, pl, src1, core);
        int plane;
        int x, y;
        for (plane = 0; plane < d->vi->format->numPlanes; plane++) {
            if (d->process[plane] == 0) {
                int weight = d->weight[plane];
                float fweight = d->fweight[plane];
                int h = vsapi->getFrameHeight(src1, plane);
                int w = vsapi->getFrameWidth(src2, plane);
                int stride = vsapi->getStride(src1, plane);
                const uint8_t *srcp1 = vsapi->getReadPtr(src1, plane);
                const uint8_t *srcp2 = vsapi->getReadPtr(src2, plane);
                uint8_t *dstp = vsapi->getWritePtr(dst, plane);

                if (d->vi->format->sampleType == stInteger) {
                    const int round = 1 << (MergeShift - 1);
                    if (d->vi->format->bytesPerSample == 1) {
#ifdef VS_TARGET_CPU_X86
                        vs_merge_uint8_sse2(srcp1, srcp2, weight, dstp, stride, h);
#else
                        for (y = 0; y < h; y++) {
                            for (x = 0; x < w; x++)
                                dstp[x] = srcp1[x] + (((srcp2[x] - srcp1[x]) * weight + round) >> MergeShift);
                            srcp1 += stride;
                            srcp2 += stride;
                            dstp += stride;
                        }
#endif
                    } else if (d->vi->format->bytesPerSample == 2) {
                        for (y = 0; y < h; y++) {
                            for (x = 0; x < w; x++)
                                ((uint16_t *)dstp)[x] = ((const uint16_t *)srcp1)[x] + (((((const uint16_t *)srcp2)[x] - ((const uint16_t *)srcp1)[x]) * weight + round) >> MergeShift);
                            srcp1 += stride;
                            srcp2 += stride;
                            dstp += stride;
                        }
                    }
                } else if (d->vi->format->sampleType == stFloat) {
                    if (d->vi->format->bytesPerSample == 4) {
                        for (y = 0; y < h; y++) {
                            for (x = 0; x < w; x++)
                                ((float *)dstp)[x] = (((const float *)srcp1)[x] + (((const float *)srcp2)[x] - ((const float *)srcp1)[x]) * fweight);
                            srcp1 += stride;
                            srcp2 += stride;
                            dstp += stride;
                        }
                    }
                }
            }
        }

        vsapi->freeFrame(src1);
        vsapi->freeFrame(src2);
        return dst;
    }

    return 0;
}

static void VS_CC mergeFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    MergeData *d = (MergeData *)instanceData;
    vsapi->freeNode(d->node1);
    vsapi->freeNode(d->node2);
    free(d);
}

static void VS_CC mergeCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    MergeData d;
    MergeData *data;
    int nweight;
    int i;

    nweight = vsapi->propNumElements(in, "weight");
    for (i = 0; i < 3; i++)
        d.fweight[i] = 0.5;
    for (i = 0; i < nweight; i++)
        d.fweight[i] = (float)vsapi->propGetFloat(in, "weight", i, 0);

    if (nweight == 2) {
        d.fweight[2] = d.fweight[1];
    } else if (nweight == 1) {
        d.fweight[1] = d.fweight[0];
        d.fweight[2] = d.fweight[0];
    }

    for (i = 0; i < 3; i++) {
        if (d.fweight[i] < 0 || d.fweight[i] > 1)
            RETERROR("Merge: weights must be between 0 and 1");
        d.weight[i] = (int)(d.fweight[i] * (1 << MergeShift) + 0.5f);
    }

    if (vsapi->propNumElements(in, "clips") != 2)
        RETERROR("Merge: exactly two input clips must be specified");

    d.node1 = vsapi->propGetNode(in, "clips", 0, 0);
    d.node2 = vsapi->propGetNode(in, "clips", 1, 0);
    d.vi = vsapi->getVideoInfo(d.node1);

    for (i = 0; i < 3; i++) {
        d.process[i] = 0;
        if (d.vi->format->sampleType == stInteger) {
            if (d.weight[i] == 0)
                d.process[i] = 1;
            else if (d.weight[i] == 1 << MergeShift)
                d.process[i] = 2;
        } else if (d.vi->format->sampleType == stFloat) {
            if (d.fweight[i] == 0.0f)
                d.process[i] = 1;
            else if (d.fweight[i] == 1.0f)
                d.process[i] = 2;
        }
    }

    if (!isConstantFormat(d.vi) || !isSameFormat(d.vi, vsapi->getVideoInfo(d.node2))
        || isCompatFormat(vsapi->getVideoInfo(d.node1)) || isCompatFormat(vsapi->getVideoInfo(d.node2))) {
        vsapi->freeNode(d.node1);
        vsapi->freeNode(d.node2);
        RETERROR("Merge: both clips must be constant format and also be the same format and dimensions");
    }

    if ((d.vi->format->sampleType == stInteger && d.vi->format->bytesPerSample != 1 && d.vi->format->bytesPerSample != 2)
        || (d.vi->format->sampleType == stFloat && d.vi->format->bytesPerSample != 4)) {
        vsapi->freeNode(d.node1);
        vsapi->freeNode(d.node2);
        RETERROR("Merge: only 8-16 bit integer and 32 bit float input supported");
    }

    if (nweight > d.vi->format->numPlanes) {
        vsapi->freeNode(d.node1);
        vsapi->freeNode(d.node2);
        RETERROR("Merge: more weights given than the number of planes to merge");
    }

    data = malloc(sizeof(d));
    *data = d;

    vsapi->createFilter(in, out, "Merge", mergeInit, mergeGetFrame, mergeFree, fmParallel, 0, data, core);
}

//////////////////////////////////////////
// MaskedMerge

#ifdef VS_TARGET_CPU_X86
extern void vs_masked_merge_uint8_sse2(const uint8_t *srcp1, const uint8_t *srcp2, const uint8_t *maskp, uint8_t *dstp, int stride, int height);
#endif

typedef struct {
    const VSVideoInfo *vi;
    VSNodeRef *node1;
    VSNodeRef *node2;
    VSNodeRef *mask;
    VSNodeRef *mask23;
    int first_plane;
    int process[3];
} MaskedMergeData;

static void VS_CC maskedMergeInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    MaskedMergeData *d = (MaskedMergeData *) * instanceData;
    vsapi->setVideoInfo(d->vi, 1, node);
}

static const VSFrameRef *VS_CC maskedMergeGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    MaskedMergeData *d = (MaskedMergeData *) * instanceData;

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node1, frameCtx);
        vsapi->requestFrameFilter(n, d->node2, frameCtx);
        vsapi->requestFrameFilter(n, d->mask, frameCtx);
        if (d->mask23)
            vsapi->requestFrameFilter(n, d->mask23, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *src1 = vsapi->getFrameFilter(n, d->node1, frameCtx);
        const VSFrameRef *src2 = vsapi->getFrameFilter(n, d->node2, frameCtx);
        const VSFrameRef *mask = vsapi->getFrameFilter(n, d->mask, frameCtx);
        const VSFrameRef *mask23 = 0;
        const int pl[] = {0, 1, 2};
        const VSFrameRef *fr[] = {d->process[0] ? 0 : src1, d->process[1] ? 0 : src1, d->process[2] ? 0 : src1};
        VSFrameRef *dst = vsapi->newVideoFrame2(d->vi->format, d->vi->width, d->vi->height, fr, pl, src1, core);
        int plane;
        int x, y;
        if (d->mask23)
           mask23 = vsapi->getFrameFilter(n, d->mask23, frameCtx);
        for (plane = 0; plane < d->vi->format->numPlanes; plane++) {
            if (d->process[plane]) {
                int h = vsapi->getFrameHeight(src1, plane);
                int w = vsapi->getFrameWidth(src2, plane);
                int stride = vsapi->getStride(src1, plane);
                const uint8_t *srcp1 = vsapi->getReadPtr(src1, plane);
                const uint8_t *srcp2 = vsapi->getReadPtr(src2, plane);
                const uint8_t *maskp = vsapi->getReadPtr((plane && mask23) ? mask23 : mask, d->first_plane ? 0 : plane);
                uint8_t *dstp = vsapi->getWritePtr(dst, plane);

                if (d->vi->format->sampleType == stInteger) {
                    if (d->vi->format->bytesPerSample == 1) {
#ifdef VS_TARGET_CPU_X86
                        vs_masked_merge_uint8_sse2(srcp1, srcp2, maskp, dstp, stride, h);
#else
                        for (y = 0; y < h; y++) {
                            for (x = 0; x < w; x++)
                                dstp[x] = srcp1[x] + (((srcp2[x] - srcp1[x]) * (maskp[x] > 2 ? maskp[x] + 1 : maskp[x]) + 128) >> 8);
                            srcp1 += stride;
                            srcp2 += stride;
                            maskp += stride;
                            dstp += stride;
                        }
#endif
                    } else if (d->vi->format->bytesPerSample == 2) {
                        int shift = d->vi->format->bitsPerSample;
                        int round = 1 << (shift - 1);
                        for (y = 0; y < h; y++) {
                            for (x = 0; x < w; x++)
                                ((uint16_t *)dstp)[x] = ((const uint16_t *)srcp1)[x] + (((((const uint16_t *)srcp2)[x]
                                    - ((const uint16_t *)srcp1)[x]) * (((const uint16_t *)maskp)[x] > 2 ? ((const uint16_t *)maskp)[x] + 1 : ((const uint16_t *)maskp)[x]) + round) >> shift);
                            srcp1 += stride;
                            srcp2 += stride;
                            maskp += stride;
                            dstp += stride;
                        }
                    }
                } else if (d->vi->format->sampleType == stFloat) {
                    if (d->vi->format->bytesPerSample == 4) {
                        for (y = 0; y < h; y++) {
                            for (x = 0; x < w; x++)
                                ((float *)dstp)[x] = ((const float *)srcp1)[x] + ((((const float *)srcp2)[x] - ((const float *)srcp1)[x]) * ((const float *)maskp)[x]);
                            srcp1 += stride;
                            srcp2 += stride;
                            maskp += stride;
                            dstp += stride;
                        }
                    }
                }
            }
        }

        vsapi->freeFrame(src1);
        vsapi->freeFrame(src2);
        vsapi->freeFrame(mask);
        vsapi->freeFrame(mask23);
        return dst;
    }

    return 0;
}

static void VS_CC maskedMergeFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    MaskedMergeData *d = (MaskedMergeData *)instanceData;
    vsapi->freeNode(d->node1);
    vsapi->freeNode(d->node2);
    vsapi->freeNode(d->mask);
    vsapi->freeNode(d->mask23);
    free(d);
}

static void VS_CC maskedMergeCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    MaskedMergeData d;
    MaskedMergeData *data;
    const VSVideoInfo *maskvi;
    int err;
    int m, n, o, i;
    VSMap *mout, *min;

    if (vsapi->propNumElements(in, "clips") != 2)
        RETERROR("MaskedMerge: exactly two input clips must be specified");

    d.mask23 = 0;
    d.node1 = vsapi->propGetNode(in, "clips", 0, 0);
    d.node2 = vsapi->propGetNode(in, "clips", 1, 0);
    d.mask = vsapi->propGetNode(in, "mask", 0, 0);
    d.vi = vsapi->getVideoInfo(d.node1);
    maskvi = vsapi->getVideoInfo(d.mask);
    d.first_plane = !!vsapi->propGetInt(in, "first_plane", 0, &err);
    // always use the first mask plane for all planes when it is the only one
    if (maskvi->format->numPlanes == 1)
        d.first_plane = 1;

    if (!isConstantFormat(d.vi) || !isSameFormat(d.vi, vsapi->getVideoInfo(d.node2))
        || isCompatFormat(vsapi->getVideoInfo(d.node1)) || isCompatFormat(vsapi->getVideoInfo(d.node2)) || isCompatFormat(maskvi)) {
        vsapi->freeNode(d.node1);
        vsapi->freeNode(d.node2);
        vsapi->freeNode(d.mask);
        RETERROR("MaskedMerge: both clips must be constant format and have the same format and dimensions");
    }

    if ((d.vi->format->sampleType == stInteger && d.vi->format->bytesPerSample != 1 && d.vi->format->bytesPerSample != 2)
        || (d.vi->format->sampleType == stFloat && d.vi->format->bytesPerSample != 4)) {
        vsapi->freeNode(d.node1);
        vsapi->freeNode(d.node2);
        vsapi->freeNode(d.mask);
        RETERROR("MaskedMerge: only 8-16 bit integer and 32 bit float input supported");
    }

    if (maskvi->width != d.vi->width || maskvi->height != d.vi->height || maskvi->format->bitsPerSample != d.vi->format->bitsPerSample
        || (maskvi->format != d.vi->format && maskvi->format->colorFamily != cmGray)) {
        vsapi->freeNode(d.node1);
        vsapi->freeNode(d.node2);
        vsapi->freeNode(d.mask);
        RETERROR("MaskedMerge: mask clip must have same dimensions as main clip and be the same format or equivalent grayscale version");
    }

    n = d.vi->format->numPlanes;
    m = vsapi->propNumElements(in, "planes");

    for (i = 0; i < 3; i++)
        d.process[i] = m <= 0;

    for (i = 0; i < m; i++) {
        o = int64ToIntS(vsapi->propGetInt(in, "planes", i, 0));

        if (o < 0 || o >= n) {
            vsapi->freeNode(d.node1);
            vsapi->freeNode(d.node2);
            vsapi->freeNode(d.mask);
            RETERROR("MaskedMerge: plane index out of range");
        }

        if (d.process[o]) {
            vsapi->freeNode(d.node1);
            vsapi->freeNode(d.node2);
            vsapi->freeNode(d.mask);
            RETERROR("MaskedMerge: plane specified twice");
        }

        d.process[o] = 1;
    }

    // do we need to resample the first mask plane and use it for all the planes?
    if ((d.first_plane && d.vi->format->numPlanes > 1) && (d.vi->format->subSamplingH > 0 || d.vi->format->subSamplingW > 0) && (d.process[1] || d.process[2])) {
        min = vsapi->createMap();
        vsapi->propSetNode(min, "clip", d.mask, paAppend);
        vsapi->propSetInt(min, "width", d.vi->width >> d.vi->format->subSamplingW, paAppend);
        vsapi->propSetInt(min, "height", d.vi->height >> d.vi->format->subSamplingH, paAppend);
        mout = vsapi->invoke(vsapi->getPluginById("com.vapoursynth.resize", core), "Bilinear", min);
        d.mask23 = vsapi->propGetNode(mout, "clip", 0, 0);
        vsapi->freeMap(mout);
        vsapi->freeMap(min);
    }

    data = malloc(sizeof(d));
    *data = d;

    vsapi->createFilter(in, out, "MaskedMerge", maskedMergeInit, maskedMergeGetFrame, maskedMergeFree, fmParallel, 0, data, core);
}

//////////////////////////////////////////
// Init

void VS_CC mergeInitialize(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
    //configFunc("com.vapoursynth.std", "std", "VapourSynth Core Functions", VAPOURSYNTH_API_VERSION, 1, plugin);
    registerFunc("Merge", "clips:clip[];weight:float[]:opt;", mergeCreate, 0, plugin);
    registerFunc("MaskedMerge", "clips:clip[];mask:clip;planes:int[]:opt;first_plane:int:opt;", maskedMergeCreate, 0, plugin);
}