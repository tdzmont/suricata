/* Copyright (C) 2007-2011 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Anoop Saldanha <anoopsaldanha@gmail.com>
 *
 * Performs content inspection on any buffer supplied.
 */

#include "suricata-common.h"
#include "suricata.h"

#include "decode.h"

#include "detect.h"
#include "detect-engine.h"
#include "detect-parse.h"
#include "detect-content.h"
#include "detect-pcre.h"
#include "detect-isdataat.h"
#include "detect-bytetest.h"
#include "detect-bytejump.h"
#include "detect-byte-extract.h"
#include "detect-replace.h"
#include "detect-engine-content-inspection.h"
#include "detect-uricontent.h"
#include "detect-urilen.h"

#include "app-layer-dcerpc.h"

#include "util-spm.h"
#include "util-spm-bm.h"
#include "util-debug.h"
#include "util-print.h"

#include "util-unittest.h"
#include "util-unittest-helper.h"

int DetectEngineContentInspection(DetectEngineCtx *de_ctx, DetectEngineThreadCtx *det_ctx,
                                  Signature *s, SigMatch *sm,
                                  Flow *f,
                                  uint8_t *buffer, uint32_t buffer_len,
                                  uint8_t inspection_mode, void *data)
{
    SCEnter();

    det_ctx->inspection_recursion_counter++;

    if (det_ctx->inspection_recursion_counter == de_ctx->inspection_recursion_limit) {
        det_ctx->discontinue_matching = 1;
        SCReturnInt(0);
    }

    if (sm == NULL || buffer_len == 0) {
        SCReturnInt(0);
    }

    /* \todo unify this which is phase 2 of payload inspection unification */
    if (sm->type == DETECT_CONTENT ||
        sm->type == DETECT_URICONTENT ||
        sm->type == DETECT_AL_HTTP_RAW_URI ||
        sm->type == DETECT_AL_HTTP_HEADER ||
        sm->type == DETECT_AL_HTTP_RAW_HEADER ||
        sm->type == DETECT_AL_HTTP_CLIENT_BODY ||
        sm->type == DETECT_AL_HTTP_SERVER_BODY ||
        sm->type == DETECT_AL_HTTP_COOKIE ||
        sm->type == DETECT_AL_HTTP_METHOD) {

        DetectContentData *cd = (DetectContentData *)sm->ctx;
        SCLogDebug("inspecting content %"PRIu32" buffer_len %"PRIu32, cd->id, buffer_len);

        /* we might have already have this content matched by the mpm.
         * (if there is any other reason why we'd want to avoid checking
         *  it here, please fill it in) */
        if (inspection_mode == DETECT_ENGINE_CONTENT_INSPECTION_MODE_STREAM) {
            if (cd->flags & DETECT_CONTENT_STREAM_MPM && !(cd->flags & DETECT_CONTENT_NEGATED)) {
                goto match;
            }
        }

        /* rule parsers should take care of this */
#ifdef DEBUG
        BUG_ON(cd->depth != 0 && cd->depth <= cd->offset);
#endif

        /* search for our pattern, checking the matches recursively.
         * if we match we look for the next SigMatch as well */
        uint8_t *found = NULL;
        uint32_t offset = 0;
        uint32_t depth = buffer_len;
        uint32_t prev_offset = 0; /**< used in recursive searching */
        uint32_t prev_buffer_offset = det_ctx->payload_offset;

        do {
            if (cd->flags & DETECT_CONTENT_DISTANCE ||
                cd->flags & DETECT_CONTENT_WITHIN) {
                SCLogDebug("det_ctx->payload_offset %"PRIu32, det_ctx->payload_offset);

                offset = prev_buffer_offset;
                depth = buffer_len;

                int distance = cd->distance;
                if (cd->flags & DETECT_CONTENT_DISTANCE) {
                    if (cd->flags & DETECT_CONTENT_DISTANCE_BE) {
                        distance = det_ctx->bj_values[cd->distance];
                    }
                    if (distance < 0 && (uint32_t)(abs(distance)) > offset)
                        offset = 0;
                    else
                        offset += distance;

                    SCLogDebug("cd->distance %"PRIi32", offset %"PRIu32", depth %"PRIu32,
                               distance, offset, depth);
                }

                if (cd->flags & DETECT_CONTENT_WITHIN) {
                    if (cd->flags & DETECT_CONTENT_WITHIN_BE) {
                        if ((int32_t)depth > (int32_t)(prev_buffer_offset + det_ctx->bj_values[cd->within] + distance)) {
                            depth = prev_buffer_offset + det_ctx->bj_values[cd->within] + distance;
                        }
                    } else {
                        if ((int32_t)depth > (int32_t)(prev_buffer_offset + cd->within + distance)) {
                            depth = prev_buffer_offset + cd->within + distance;
                        }

                        SCLogDebug("cd->within %"PRIi32", det_ctx->payload_offset %"PRIu32", depth %"PRIu32,
                                   cd->within, prev_buffer_offset, depth);
                    }
                }

                if (cd->flags & DETECT_CONTENT_DEPTH_BE) {
                    if ((det_ctx->bj_values[cd->depth] + prev_buffer_offset) < depth) {
                        depth = prev_buffer_offset + det_ctx->bj_values[cd->depth];
                    }
                } else {
                    if (cd->depth != 0) {
                        if ((cd->depth + prev_buffer_offset) < depth) {
                            depth = prev_buffer_offset + cd->depth;
                        }

                        SCLogDebug("cd->depth %"PRIu32", depth %"PRIu32, cd->depth, depth);
                    }
                }

                if (cd->flags & DETECT_CONTENT_OFFSET_BE) {
                    if (det_ctx->bj_values[cd->offset] > offset)
                        offset = det_ctx->bj_values[cd->offset];
                } else {
                    if (cd->offset > offset) {
                        offset = cd->offset;
                        SCLogDebug("setting offset %"PRIu32, offset);
                    }
                }
            } else { /* implied no relative matches */
                /* set depth */
                if (cd->flags & DETECT_CONTENT_DEPTH_BE) {
                    depth = det_ctx->bj_values[cd->depth];
                } else {
                    if (cd->depth != 0) {
                        depth = cd->depth;
                    }
                }

                /* set offset */
                if (cd->flags & DETECT_CONTENT_OFFSET_BE)
                    offset = det_ctx->bj_values[cd->offset];
                else
                    offset = cd->offset;
                prev_buffer_offset = 0;
            }

            /* update offset with prev_offset if we're searching for
             * matches after the first occurence. */
            SCLogDebug("offset %"PRIu32", prev_offset %"PRIu32, offset, prev_offset);
            if (prev_offset != 0)
                offset = prev_offset;

            SCLogDebug("offset %"PRIu32", depth %"PRIu32, offset, depth);

            if (depth > buffer_len)
                depth = buffer_len;

            /* if offset is bigger than depth we can never match on a pattern.
             * We can however, "match" on a negated pattern. */
            if (offset > depth || depth == 0) {
                if (cd->flags & DETECT_CONTENT_NEGATED) {
                    goto match;
                } else {
                    SCReturnInt(0);
                }
            }

            uint8_t *sbuffer = buffer + offset;
            uint32_t sbuffer_len = depth - offset;
            uint32_t match_offset = 0;
            SCLogDebug("sbuffer_len %"PRIu32, sbuffer_len);
#ifdef DEBUG
            BUG_ON(sbuffer_len > buffer_len);
#endif

            /* \todo Add another optimization here.  If cd->content_len is
             * greater than sbuffer_len found is anyways NULL */

            /* do the actual search */
            if (cd->flags & DETECT_CONTENT_NOCASE)
                found = BoyerMooreNocase(cd->content, cd->content_len, sbuffer, sbuffer_len, cd->bm_ctx->bmGs, cd->bm_ctx->bmBc);
            else
                found = BoyerMoore(cd->content, cd->content_len, sbuffer, sbuffer_len, cd->bm_ctx->bmGs, cd->bm_ctx->bmBc);

            /* next we evaluate the result in combination with the
             * negation flag. */
            SCLogDebug("found %p cd negated %s", found, cd->flags & DETECT_CONTENT_NEGATED ? "true" : "false");

            if (found == NULL && !(cd->flags & DETECT_CONTENT_NEGATED)) {
                SCReturnInt(0);
            } else if (found == NULL && cd->flags & DETECT_CONTENT_NEGATED) {
                goto match;
            } else if (found != NULL && cd->flags & DETECT_CONTENT_NEGATED) {
                SCLogDebug("content %"PRIu32" matched at offset %"PRIu32", but negated so no match", cd->id, match_offset);
                /* don't bother carrying recursive matches now, for preceding
                 * relative keywords */
                det_ctx->discontinue_matching = 1;
                SCReturnInt(0);
            } else {
                match_offset = (uint32_t)((found - buffer) + cd->content_len);
                SCLogDebug("content %"PRIu32" matched at offset %"PRIu32"", cd->id, match_offset);
                det_ctx->payload_offset = match_offset;

                /* Match branch, add replace to the list if needed */
                if (cd->flags & DETECT_CONTENT_REPLACE) {
                    if (inspection_mode == DETECT_ENGINE_CONTENT_INSPECTION_MODE_PAYLOAD) {
                        /* we will need to replace content if match is confirmed */
                        det_ctx->replist = DetectReplaceAddToList(det_ctx->replist, found, cd);
                    } else {
                        SCLogWarning(SC_ERR_INVALID_VALUE, "Can't modify payload without packet");
                    }
                }
                if (!(cd->flags & DETECT_CONTENT_RELATIVE_NEXT)) {
                    SCLogDebug("no relative match coming up, so this is a match");
                    goto match;
                }

                /* bail out if we have no next match. Technically this is an
                 * error, as the current cd has the DETECT_CONTENT_RELATIVE_NEXT
                 * flag set. */
                if (sm->next == NULL) {
                    SCReturnInt(0);
                }

                SCLogDebug("content %"PRIu32, cd->id);

                /* see if the next buffer keywords match. If not, we will
                 * search for another occurence of this content and see
                 * if the others match then until we run out of matches */
                int r = DetectEngineContentInspection(de_ctx, det_ctx, s, sm->next, f, buffer, buffer_len, inspection_mode, data);
                if (r == 1) {
                    SCReturnInt(1);
                }

                if (det_ctx->discontinue_matching)
                    SCReturnInt(0);

                /* set the previous match offset to the start of this match + 1 */
                prev_offset = (match_offset - (cd->content_len - 1));
                SCLogDebug("trying to see if there is another match after prev_offset %"PRIu32, prev_offset);
            }

        } while(1);

    } else if (sm->type == DETECT_ISDATAAT) {
        SCLogDebug("inspecting isdataat");

        DetectIsdataatData *id = (DetectIsdataatData *)sm->ctx;
        if (id->flags & ISDATAAT_RELATIVE) {
            if (det_ctx->payload_offset + id->dataat > buffer_len) {
                SCLogDebug("det_ctx->payload_offset + id->dataat %"PRIu32" > %"PRIu32, det_ctx->payload_offset + id->dataat, buffer_len);
                if (id->flags & ISDATAAT_NEGATED)
                    goto match;
                SCReturnInt(0);
            } else {
                SCLogDebug("relative isdataat match");
                if (id->flags & ISDATAAT_NEGATED)
                    SCReturnInt(0);
                goto match;
            }
        } else {
            if (id->dataat < buffer_len) {
                SCLogDebug("absolute isdataat match");
                if (id->flags & ISDATAAT_NEGATED)
                    SCReturnInt(0);
                goto match;
            } else {
                SCLogDebug("absolute isdataat mismatch, id->isdataat %"PRIu32", buffer_len %"PRIu32"", id->dataat, buffer_len);
                if (id->flags & ISDATAAT_NEGATED)
                    goto match;
                SCReturnInt(0);
            }
        }

    } else if (sm->type == DETECT_PCRE) {
        SCLogDebug("inspecting pcre");
        DetectPcreData *pe = (DetectPcreData *)sm->ctx;
        uint32_t prev_buffer_offset = det_ctx->payload_offset;
        uint32_t prev_offset = 0;
        int r = 0;

        det_ctx->pcre_match_start_offset = 0;
        do {
            Packet *p = NULL;
            if (inspection_mode == DETECT_ENGINE_CONTENT_INSPECTION_MODE_PAYLOAD)
                p = (Packet *)data;
            r = DetectPcrePayloadMatch(det_ctx, s, sm, p, f,
                                       buffer, buffer_len);
            if (r == 0) {
                det_ctx->discontinue_matching = 1;
                SCReturnInt(0);
            }

            if (!(pe->flags & DETECT_PCRE_RELATIVE_NEXT)) {
                SCLogDebug("no relative match coming up, so this is a match");
                goto match;
            }

            /* save it, in case we need to do a pcre match once again */
            prev_offset = det_ctx->pcre_match_start_offset;

            /* see if the next payload keywords match. If not, we will
             * search for another occurence of this pcre and see
             * if the others match, until we run out of matches */
            r = DetectEngineContentInspection(de_ctx, det_ctx, s, sm->next,
                                              f, buffer, buffer_len, inspection_mode, data);
            if (r == 1) {
                SCReturnInt(1);
            }

            if (det_ctx->discontinue_matching)
                SCReturnInt(0);

            det_ctx->payload_offset = prev_buffer_offset;
            det_ctx->pcre_match_start_offset = prev_offset;
        } while (1);

    } else if (sm->type == DETECT_BYTETEST) {
        DetectBytetestData *btd = (DetectBytetestData *)sm->ctx;
        uint8_t flags = btd->flags;
        int32_t offset = btd->offset;
        uint64_t value = btd->value;
        if (flags & DETECT_BYTETEST_OFFSET_BE) {
            offset = det_ctx->bj_values[offset];
        }
        if (flags & DETECT_BYTETEST_VALUE_BE) {
            value = det_ctx->bj_values[value];
        }

        /* if we have dce enabled we will have to use the endianness
         * specified by the dce header */
        if (flags & DETECT_BYTETEST_DCE) {
            DCERPCState *dcerpc_state = (DCERPCState *)data;
            /* enable the endianness flag temporarily.  once we are done
             * processing we reset the flags to the original value*/
            flags |= ((dcerpc_state->dcerpc.dcerpchdr.packed_drep[0] & 0x10) ?
                      DETECT_BYTETEST_LITTLE: 0);
        }

        if (DetectBytetestDoMatch(det_ctx, s, sm, buffer, buffer_len, flags,
                                  offset, value) != 1) {
            SCReturnInt(0);
        }

        goto match;

    } else if (sm->type == DETECT_BYTEJUMP) {
        DetectBytejumpData *bjd = (DetectBytejumpData *)sm->ctx;
        uint8_t flags = bjd->flags;
        int32_t offset = bjd->offset;

        if (flags & DETECT_BYTEJUMP_OFFSET_BE) {
            offset = det_ctx->bj_values[offset];
        }

        /* if we have dce enabled we will have to use the endianness
         * specified by the dce header */
        if (flags & DETECT_BYTEJUMP_DCE) {
            DCERPCState *dcerpc_state = (DCERPCState *)data;
            /* enable the endianness flag temporarily.  once we are done
             * processing we reset the flags to the original value*/
            flags |= ((dcerpc_state->dcerpc.dcerpchdr.packed_drep[0] & 0x10) ?
                      DETECT_BYTEJUMP_LITTLE: 0);
        }

        if (DetectBytejumpDoMatch(det_ctx, s, sm, buffer, buffer_len,
                                  flags, offset) != 1) {
            SCReturnInt(0);
        }

        goto match;

    } else if (sm->type == DETECT_BYTE_EXTRACT) {

        DetectByteExtractData *bed = (DetectByteExtractData *)sm->ctx;
        uint8_t endian = bed->endian;

        /* if we have dce enabled we will have to use the endianness
         * specified by the dce header */
        if (bed->flags & DETECT_BYTE_EXTRACT_FLAG_ENDIAN &&
            endian == DETECT_BYTE_EXTRACT_ENDIAN_DCE) {

            DCERPCState *dcerpc_state = (DCERPCState *)data;
            /* enable the endianness flag temporarily.  once we are done
             * processing we reset the flags to the original value*/
            endian |= ((dcerpc_state->dcerpc.dcerpchdr.packed_drep[0] == 0x10) ?
                       DETECT_BYTE_EXTRACT_ENDIAN_LITTLE : DETECT_BYTE_EXTRACT_ENDIAN_BIG);
        }

        if (DetectByteExtractDoMatch(det_ctx, sm, s, buffer,
                                     buffer_len,
                                     &det_ctx->bj_values[bed->local_id],
                                     endian) != 1) {
            SCReturnInt(0);
        }

        goto match;

        /* we should never get here, but bail out just in case */
    } else if (sm->type == DETECT_AL_URILEN) {
        SCLogDebug("inspecting uri len");

        int r = 0;
        DetectUrilenData *urilend = (DetectUrilenData *) sm->ctx;

        switch (urilend->mode) {
            case DETECT_URILEN_EQ:
                if (buffer_len == urilend->urilen1)
                    r = 1;
                break;
            case DETECT_URILEN_LT:
                if (buffer_len < urilend->urilen1)
                    r = 1;
                break;
            case DETECT_URILEN_GT:
                if (buffer_len > urilend->urilen1)
                    r = 1;
                break;
            case DETECT_URILEN_RA:
                if (buffer_len > urilend->urilen1 &&
                    buffer_len < urilend->urilen2) {
                    r = 1;
                }
                break;
        }

        if (r == 1) {
            goto match;
        }

        SCReturnInt(0);
    } else {
        SCLogDebug("sm->type %u", sm->type);
#ifdef DEBUG
        BUG_ON(1);
#endif
    }

    SCReturnInt(0);

match:
    /* this sigmatch matched, inspect the next one. If it was the last,
     * the buffer portion of the signature matched. */
    if (sm->next != NULL) {
        int r = DetectEngineContentInspection(de_ctx, det_ctx, s, sm->next, f, buffer, buffer_len, inspection_mode, data);
        SCReturnInt(r);
    } else {
        SCReturnInt(1);
    }
}
