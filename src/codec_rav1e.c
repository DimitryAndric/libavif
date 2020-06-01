// Copyright 2019 Joe Drago. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#include "avif/internal.h"

#include "rav1e/rav1e.h"

#include <string.h>

struct avifCodecInternal
{
    RaContext * rav1eContext;
    RaChromaSampling chromaSampling;
    int yShift;
};

static void rav1eCodecDestroyInternal(avifCodec * codec)
{
    if (codec->internal->rav1eContext) {
        rav1e_context_unref(codec->internal->rav1eContext);
        codec->internal->rav1eContext = NULL;
    }
    avifFree(codec->internal);
}

static avifBool rav1eCodecOpen(struct avifCodec * codec, uint32_t firstSampleIndex)
{
    (void)firstSampleIndex; // Codec is encode-only, this isn't used

    codec->internal->rav1eContext = NULL;
    return AVIF_TRUE;
}

static avifBool rav1eCodecEncodeImage(avifCodec * codec, const avifImage * image, avifEncoder * encoder, avifBool alpha, avifRWData * obu)
{
    (void)codec; // unused

    avifBool success = AVIF_FALSE;

    RaConfig * rav1eConfig = NULL;
    RaFrame * rav1eFrame = NULL;

    if (!codec->internal->rav1eContext) {
        RaPixelRange rav1eRange;
        if (alpha) {
            rav1eRange = (image->alphaRange == AVIF_RANGE_FULL) ? RA_PIXEL_RANGE_FULL : RA_PIXEL_RANGE_LIMITED;
            codec->internal->chromaSampling = RA_CHROMA_SAMPLING_CS400;
        } else {
            rav1eRange = (image->yuvRange == AVIF_RANGE_FULL) ? RA_PIXEL_RANGE_FULL : RA_PIXEL_RANGE_LIMITED;
            codec->internal->yShift = 0;
            switch (image->yuvFormat) {
                case AVIF_PIXEL_FORMAT_YUV444:
                    codec->internal->chromaSampling = RA_CHROMA_SAMPLING_CS444;
                    break;
                case AVIF_PIXEL_FORMAT_YUV422:
                    codec->internal->chromaSampling = RA_CHROMA_SAMPLING_CS422;
                    break;
                case AVIF_PIXEL_FORMAT_YUV420:
                    codec->internal->chromaSampling = RA_CHROMA_SAMPLING_CS420;
                    codec->internal->yShift = 1;
                    break;
                case AVIF_PIXEL_FORMAT_YUV400:
                    codec->internal->chromaSampling = RA_CHROMA_SAMPLING_CS400;
                    codec->internal->yShift = 1;
                    break;
                case AVIF_PIXEL_FORMAT_YV12:
                case AVIF_PIXEL_FORMAT_NONE:
                default:
                    return AVIF_FALSE;
            }
        }

        rav1eConfig = rav1e_config_default();
        if (rav1e_config_set_pixel_format(
                rav1eConfig, (uint8_t)image->depth, codec->internal->chromaSampling, RA_CHROMA_SAMPLE_POSITION_UNKNOWN, rav1eRange) < 0) {
            goto cleanup;
        }

        if (rav1e_config_parse(rav1eConfig, "still_picture", "true") == -1) {
            goto cleanup;
        }
        if (rav1e_config_parse_int(rav1eConfig, "width", image->width) == -1) {
            goto cleanup;
        }
        if (rav1e_config_parse_int(rav1eConfig, "height", image->height) == -1) {
            goto cleanup;
        }
        if (rav1e_config_parse_int(rav1eConfig, "threads", encoder->maxThreads) == -1) {
            goto cleanup;
        }

        int minQuantizer = AVIF_CLAMP(encoder->minQuantizer, 0, 63);
        int maxQuantizer = AVIF_CLAMP(encoder->maxQuantizer, 0, 63);
        if (alpha) {
            minQuantizer = AVIF_CLAMP(encoder->minQuantizerAlpha, 0, 63);
            maxQuantizer = AVIF_CLAMP(encoder->maxQuantizerAlpha, 0, 63);
        }
        minQuantizer = (minQuantizer * 255) / 63; // Rescale quantizer values as rav1e's QP range is [0,255]
        maxQuantizer = (maxQuantizer * 255) / 63;
        if (rav1e_config_parse_int(rav1eConfig, "min_quantizer", minQuantizer) == -1) {
            goto cleanup;
        }
        if (rav1e_config_parse_int(rav1eConfig, "quantizer", maxQuantizer) == -1) {
            goto cleanup;
        }
        if (encoder->tileRowsLog2 != 0) {
            int tileRowsLog2 = AVIF_CLAMP(encoder->tileRowsLog2, 0, 6);
            if (rav1e_config_parse_int(rav1eConfig, "tile_rows", 1 << tileRowsLog2) == -1) {
                goto cleanup;
            }
        }
        if (encoder->tileColsLog2 != 0) {
            int tileColsLog2 = AVIF_CLAMP(encoder->tileColsLog2, 0, 6);
            if (rav1e_config_parse_int(rav1eConfig, "tile_cols", 1 << tileColsLog2) == -1) {
                goto cleanup;
            }
        }
        if (encoder->speed != AVIF_SPEED_DEFAULT) {
            int speed = AVIF_CLAMP(encoder->speed, 0, 10);
            if (rav1e_config_parse_int(rav1eConfig, "speed", speed) == -1) {
                goto cleanup;
            }
        }

        rav1e_config_set_color_description(rav1eConfig,
                                           (RaMatrixCoefficients)image->matrixCoefficients,
                                           (RaColorPrimaries)image->colorPrimaries,
                                           (RaTransferCharacteristics)image->transferCharacteristics);

        codec->internal->rav1eContext = rav1e_context_new(rav1eConfig);
        if (!codec->internal->rav1eContext) {
            goto cleanup;
        }
    }

    rav1eFrame = rav1e_frame_new(codec->internal->rav1eContext);

    int byteWidth = (image->depth > 8) ? 2 : 1;
    if (alpha) {
        rav1e_frame_fill_plane(rav1eFrame, 0, image->alphaPlane, image->alphaRowBytes * image->height, image->alphaRowBytes, byteWidth);
    } else {
        uint32_t uvHeight = (image->height + codec->internal->yShift) >> codec->internal->yShift;
        rav1e_frame_fill_plane(rav1eFrame, 0, image->yuvPlanes[0], image->yuvRowBytes[0] * image->height, image->yuvRowBytes[0], byteWidth);
        if (codec->internal->chromaSampling != RA_CHROMA_SAMPLING_CS400) {
            rav1e_frame_fill_plane(rav1eFrame, 1, image->yuvPlanes[1], image->yuvRowBytes[1] * uvHeight, image->yuvRowBytes[1], byteWidth);
            rav1e_frame_fill_plane(rav1eFrame, 2, image->yuvPlanes[2], image->yuvRowBytes[2] * uvHeight, image->yuvRowBytes[2], byteWidth);
        }
    }

    RaEncoderStatus encoderStatus = rav1e_send_frame(codec->internal->rav1eContext, rav1eFrame);
    if (encoderStatus != 0) {
        goto cleanup;
    }

    RaPacket * pkt = NULL;
    encoderStatus = rav1e_receive_packet(codec->internal->rav1eContext, &pkt);
    if (encoderStatus != 0) {
        goto cleanup;
    } else if (pkt && pkt->data && (pkt->len > 0)) {
        avifRWDataSet(obu, pkt->data, pkt->len);
        rav1e_packet_unref(pkt);
        pkt = NULL;
    }
    success = AVIF_TRUE;
cleanup:
    if (rav1eFrame) {
        rav1e_frame_unref(rav1eFrame);
        rav1eFrame = NULL;
    }
    if (rav1eConfig) {
        rav1e_config_unref(rav1eConfig);
        rav1eConfig = NULL;
    }
    return success;
}

static avifBool rav1eCodecEncodeFinish(avifCodec * codec, avifRWData * obu)
{
    RaEncoderStatus encoderStatus = rav1e_send_frame(codec->internal->rav1eContext, NULL); // flush
    if (encoderStatus != 0) {
        return AVIF_FALSE;
    }

    RaPacket * pkt = NULL;
    encoderStatus = rav1e_receive_packet(codec->internal->rav1eContext, &pkt);
    if (encoderStatus != 0) {
        return AVIF_FALSE;
    }
    if (pkt && pkt->data && (pkt->len > 0)) {
        avifRWDataSet(obu, pkt->data, pkt->len);
        rav1e_packet_unref(pkt);
        pkt = NULL;
    }
    return AVIF_TRUE;
}

const char * avifCodecVersionRav1e(void)
{
    return rav1e_version_full();
}

avifCodec * avifCodecCreateRav1e(void)
{
    avifCodec * codec = (avifCodec *)avifAlloc(sizeof(avifCodec));
    memset(codec, 0, sizeof(struct avifCodec));
    codec->open = rav1eCodecOpen;
    codec->encodeImage = rav1eCodecEncodeImage;
    codec->encodeFinish = rav1eCodecEncodeFinish;
    codec->destroyInternal = rav1eCodecDestroyInternal;

    codec->internal = (struct avifCodecInternal *)avifAlloc(sizeof(struct avifCodecInternal));
    memset(codec->internal, 0, sizeof(struct avifCodecInternal));
    return codec;
}
