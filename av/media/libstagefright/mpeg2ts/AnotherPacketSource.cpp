/*
* Copyright (C) 2014 MediaTek Inc.
* Modification based on code covered by the mentioned copyright
* and/or permission notice(s).
*/
/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "AnotherPacketSource"

#include "AnotherPacketSource.h"

#include "include/avc_utils.h"

#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/AString.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>
#include <utils/Vector.h>

#include <inttypes.h>
#ifdef MTK_AOSP_ENHANCEMENT
static int kWholeBufSize = 40000000;    //40Mbytes
static int kTargetTime = 2000;  //ms
#endif
namespace android {

#ifdef MTK_AOSP_ENHANCEMENT
const int64_t kNearEOSMarkUs = 1000000ll;   // change to 1 secs, ensure the data near the end in the file can be played and play smoothly
#else
const int64_t kNearEOSMarkUs = 2000000ll; // 2 secs
#endif

AnotherPacketSource::AnotherPacketSource(const sp<MetaData> &meta)
    : mIsAudio(false),
      mIsVideo(false),
      mEnabled(true),
      mFormat(NULL),
      mLastQueuedTimeUs(0),
      mEOSResult(OK),
      mLatestEnqueuedMeta(NULL),
#ifdef MTK_AOSP_ENHANCEMENT
      mQueuedDiscontinuityCount(0),
#endif
      mLatestDequeuedMeta(NULL){
#ifdef MTK_AOSP_ENHANCEMENT
      mLatestDequeuedMeta = NULL;
      mIsEOS = false;
      mScanForIDR = true;
      mIsAVC = false;
      mNeedScanForIDR = false;
      mStrmSourcePID = 0;
#endif
    setFormat(meta);

    mDiscontinuitySegments.push_back(DiscontinuitySegment());
}

void AnotherPacketSource::setFormat(const sp<MetaData> &meta) {
    if (mFormat != NULL) {
        // Only allowed to be set once. Requires explicit clear to reset.
        return;
    }

    mIsAudio = false;
    mIsVideo = false;

    if (meta == NULL) {
        return;
    }

    mFormat = meta;
#ifdef MTK_AOSP_ENHANCEMENT
    const char *mime;
    CHECK(meta->findCString(kKeyMIMEType, &mime));
    if (!strncasecmp("audio/", mime, 6)) {
        mIsAudio = true;
    } else if (!strncasecmp("text/", mime, 5)) {
    } else {
        if (strncasecmp("video/", mime, 6)) {
            CHECK(!strncasecmp("image/", mime, 6));
        }
    }

    //for bitrate-adaptation
    m_BufQueSize = kWholeBufSize;
    m_TargetTime = kTargetTime;
    m_uiNextAduSeqNum = -1;
    // mtk80902: porting from APacketSource
    if (!strcmp(MEDIA_MIMETYPE_VIDEO_AVC, mime)) {
        ALOGD("This is avc mime!");
        mIsAVC = true;
    }
#else
    const char *mime;
    CHECK(meta->findCString(kKeyMIMEType, &mime));

    if (!strncasecmp("audio/", mime, 6)) {
        mIsAudio = true;
    } else  if (!strncasecmp("video/", mime, 6)) {
        mIsVideo = true;
    } else {
        CHECK(!strncasecmp("text/", mime, 5) || !strncasecmp("application/", mime, 12));
    }
#endif
}

AnotherPacketSource::~AnotherPacketSource() {
}

status_t AnotherPacketSource::start(MetaData * /* params */) {
#ifdef MTK_AOSP_ENHANCEMENT
    mIsEOS = false;
#endif
    return OK;
}

status_t AnotherPacketSource::stop() {
#ifdef MTK_AOSP_ENHANCEMENT
#ifdef MTK_AUDIO_CHANGE_SUPPORT
    clear(true);
#else
    //clear();
#endif
    mIsEOS = true;
#endif
    return OK;
}

sp<MetaData> AnotherPacketSource::getFormat() {
    Mutex::Autolock autoLock(mLock);
    if (mFormat != NULL) {
        return mFormat;
    }

    List<sp<ABuffer> >::iterator it = mBuffers.begin();
    while (it != mBuffers.end()) {
        sp<ABuffer> buffer = *it;
        int32_t discontinuity;
        if (!buffer->meta()->findInt32("discontinuity", &discontinuity)) {
            sp<RefBase> object;
            if (buffer->meta()->findObject("format", &object)) {
                setFormat(static_cast<MetaData*>(object.get()));
                return mFormat;
            }
        }

        ++it;
    }
    return NULL;
}

status_t AnotherPacketSource::dequeueAccessUnit(sp<ABuffer> *buffer) {
    buffer->clear();

    Mutex::Autolock autoLock(mLock);
    while (mEOSResult == OK && mBuffers.empty()) {
        mCondition.wait(mLock);
    }

    if (!mBuffers.empty()) {
        *buffer = *mBuffers.begin();
        mBuffers.erase(mBuffers.begin());

        int32_t discontinuity;
        if ((*buffer)->meta()->findInt32("discontinuity", &discontinuity)) {
            if (wasFormatChange(discontinuity)) {
                mFormat.clear();
            }
#ifdef MTK_AOSP_ENHANCEMENT
            --mQueuedDiscontinuityCount;
            ALOGD("dequeue a dis %d", discontinuity);
#endif
            mDiscontinuitySegments.erase(mDiscontinuitySegments.begin());
            // CHECK(!mDiscontinuitySegments.empty());
            return INFO_DISCONTINUITY;
        }

        // CHECK(!mDiscontinuitySegments.empty());
        DiscontinuitySegment &seg = *mDiscontinuitySegments.begin();

        int64_t timeUs;
        mLatestDequeuedMeta = (*buffer)->meta()->dup();
        CHECK(mLatestDequeuedMeta->findInt64("timeUs", &timeUs));
        if (timeUs > seg.mMaxDequeTimeUs) {
            seg.mMaxDequeTimeUs = timeUs;
        }

        sp<RefBase> object;
        if ((*buffer)->meta()->findObject("format", &object)) {
            setFormat(static_cast<MetaData*>(object.get()));
        }

        return OK;
    }

    return mEOSResult;
}

void AnotherPacketSource::requeueAccessUnit(const sp<ABuffer> &buffer) {
    // TODO: update corresponding book keeping info.
    Mutex::Autolock autoLock(mLock);
    mBuffers.push_front(buffer);
}

#ifdef MTK_AOSP_ENHANCEMENT
status_t AnotherPacketSource::read(
        MediaBuffer **out, const ReadOptions *options) {
#else
status_t AnotherPacketSource::read(
        MediaBuffer **out, const ReadOptions *) {
#endif
    *out = NULL;

    Mutex::Autolock autoLock(mLock);
    while (mEOSResult == OK && mBuffers.empty()) {
        mCondition.wait(mLock);
    }

    if (!mBuffers.empty()) {

        const sp<ABuffer> buffer = *mBuffers.begin();

#ifdef MTK_AOSP_ENHANCEMENT
        m_uiNextAduSeqNum = buffer->int32Data();
#endif
        mBuffers.erase(mBuffers.begin());

        int32_t discontinuity;
        if (buffer->meta()->findInt32("discontinuity", &discontinuity)) {
            if (wasFormatChange(discontinuity)) {
                mFormat.clear();
            }

            mDiscontinuitySegments.erase(mDiscontinuitySegments.begin());
            // CHECK(!mDiscontinuitySegments.empty());
            return INFO_DISCONTINUITY;
        }

        mLatestDequeuedMeta = buffer->meta()->dup();

        sp<RefBase> object;
        if (buffer->meta()->findObject("format", &object)) {
            setFormat(static_cast<MetaData*>(object.get()));
        }

        int64_t timeUs;
        CHECK(buffer->meta()->findInt64("timeUs", &timeUs));
        // CHECK(!mDiscontinuitySegments.empty());
        DiscontinuitySegment &seg = *mDiscontinuitySegments.begin();
        if (timeUs > seg.mMaxDequeTimeUs) {
            seg.mMaxDequeTimeUs = timeUs;
        }

        MediaBuffer *mediaBuffer = new MediaBuffer(buffer);

        mediaBuffer->meta_data()->setInt64(kKeyTime, timeUs);

        int32_t isSync;
        if (buffer->meta()->findInt32("isSync", &isSync)) {
            mediaBuffer->meta_data()->setInt32(kKeyIsSyncFrame, isSync);
        }

        sp<ABuffer> sei;
        if (buffer->meta()->findBuffer("sei", &sei) && sei != NULL) {
            mediaBuffer->meta_data()->setData(kKeySEI, 0, sei->data(), sei->size());
        }

#ifdef MTK_AOSP_ENHANCEMENT
            int32_t fgInvalidtimeUs = false;
            if (buffer->meta()->findInt32("invt", &fgInvalidtimeUs)) {
                mediaBuffer->meta_data()->setInt32(kInvalidKeyTime,
                                                   fgInvalidtimeUs);
            }

            int64_t seekTimeUs;
            ReadOptions::SeekMode seekMode;
            if (options && options->getSeekTo(&seekTimeUs, &seekMode)) {
                mediaBuffer->meta_data()->setInt64(kKeyTargetTime,
                                                   seekTimeUs);
            }
#endif
        *out = mediaBuffer;
        return OK;
    }

    return mEOSResult;
}

bool AnotherPacketSource::wasFormatChange(
        int32_t discontinuityType) const {
    if (mIsAudio) {
        return (discontinuityType & ATSParser::DISCONTINUITY_AUDIO_FORMAT) != 0;
    }

    if (mIsVideo) {
        return (discontinuityType & ATSParser::DISCONTINUITY_VIDEO_FORMAT) != 0;
    }

    return false;
}

void AnotherPacketSource::queueAccessUnit(const sp<ABuffer> &buffer) {
    int32_t damaged;
#ifdef MTK_AOSP_ENHANCEMENT
    // mtk80902: porting from APacketSource
    // wait IDR for 264
    if (mIsAVC && mNeedScanForIDR && mScanForIDR) {
        if ((buffer->data()[0] & 0x1f) != 5) {
            ALOGD("skipping AU while scanning for next IDR frame.");
            return;
        }
        mScanForIDR = false;
    }
#endif
    if (buffer->meta()->findInt32("damaged", &damaged) && damaged) {
        // LOG(VERBOSE) << "discarding damaged AU";
        return;
    }

    Mutex::Autolock autoLock(mLock);
    mBuffers.push_back(buffer);
    mCondition.signal();

    int32_t discontinuity;
    if (buffer->meta()->findInt32("discontinuity", &discontinuity)){
        ALOGV("queueing a discontinuity with queueAccessUnit");
#ifdef MTK_AOSP_ENHANCEMENT
        ++mQueuedDiscontinuityCount;
#endif
        mLastQueuedTimeUs = 0ll;
        mEOSResult = OK;
        mLatestEnqueuedMeta = NULL;

        mDiscontinuitySegments.push_back(DiscontinuitySegment());
        return;
    }

    int64_t lastQueuedTimeUs;
    CHECK(buffer->meta()->findInt64("timeUs", &lastQueuedTimeUs));
    mLastQueuedTimeUs = lastQueuedTimeUs;
    ALOGV("queueAccessUnit timeUs=%" PRIi64 " us (%.2f secs)",
            mLastQueuedTimeUs, mLastQueuedTimeUs / 1E6);

    // CHECK(!mDiscontinuitySegments.empty());
    DiscontinuitySegment &tailSeg = *(--mDiscontinuitySegments.end());
    if (lastQueuedTimeUs > tailSeg.mMaxEnqueTimeUs) {
        tailSeg.mMaxEnqueTimeUs = lastQueuedTimeUs;
    }
    if (tailSeg.mMaxDequeTimeUs == -1) {
        tailSeg.mMaxDequeTimeUs = lastQueuedTimeUs;
    }

    if (mLatestEnqueuedMeta == NULL) {
        mLatestEnqueuedMeta = buffer->meta()->dup();
    } else {
        int64_t latestTimeUs = 0;
        int64_t frameDeltaUs = 0;
        CHECK(mLatestEnqueuedMeta->findInt64("timeUs", &latestTimeUs));
        if (lastQueuedTimeUs > latestTimeUs) {
            mLatestEnqueuedMeta = buffer->meta()->dup();
            frameDeltaUs = lastQueuedTimeUs - latestTimeUs;
            mLatestEnqueuedMeta->setInt64("durationUs", frameDeltaUs);
        } else if (!mLatestEnqueuedMeta->findInt64("durationUs", &frameDeltaUs)) {
            // For B frames
            frameDeltaUs = latestTimeUs - lastQueuedTimeUs;
            mLatestEnqueuedMeta->setInt64("durationUs", frameDeltaUs);
        }
    }
}

#ifdef MTK_AOSP_ENHANCEMENT
void AnotherPacketSource::clear(const bool bKeepFormat) {
    Mutex::Autolock autoLock(mLock);
    if (!mBuffers.empty()) {
        mBuffers.clear();
    }
    mEOSResult = OK;
    mQueuedDiscontinuityCount = 0;
    mDiscontinuitySegments.clear();
    mDiscontinuitySegments.push_back(DiscontinuitySegment());
    mLatestDequeuedMeta = NULL;

    if (bKeepFormat != true) {
        mFormat = NULL;
        mLatestEnqueuedMeta = NULL;
    }

}
#else
void AnotherPacketSource::clear() {
    Mutex::Autolock autoLock(mLock);

    mBuffers.clear();
    mEOSResult = OK;

    mDiscontinuitySegments.clear();
    mDiscontinuitySegments.push_back(DiscontinuitySegment());

    mFormat = NULL;
    mLatestEnqueuedMeta = NULL;
}
#endif

void AnotherPacketSource::queueDiscontinuity(
        ATSParser::DiscontinuityType type,
        const sp<AMessage> &extra,
        bool discard) {
    Mutex::Autolock autoLock(mLock);
    ALOGI("queueDiscontinuity type=%d, discard=%d",type, discard);
#ifdef MTK_AOSP_ENHANCEMENT
#if 0
    if (type == ATSParser::DISCONTINUITY_HTTPLIVE_MEDIATIME) {
        if (!mBuffers.empty()) {
            mBuffers.clear();
        }
        return;
    }
#endif

    if (type & ATSParser::DISCONTINUITY_FLUSH_SOURCE_ONLY) {
        //only flush source, don't queue discontinuity
        if (!mBuffers.empty()) {
            mBuffers.clear();
        }
        mEOSResult = OK;
        mScanForIDR = true;

        ALOGD("found discontinuity flush source only and clear mDiscontinuitySegments!");

        for (List<DiscontinuitySegment>::iterator it2 = mDiscontinuitySegments.begin();
                it2 != mDiscontinuitySegments.end();
                ++it2) {
            DiscontinuitySegment &seg = *it2;
            seg.clear();
        }

        return;
    }
/*
    //do not erase pending buffers while encount explicitDiscontinuity.
    if(type & ATSParser::DISCONTINUITY_FORMATCHANGE)
        ;
    else {
*/
#endif
    if (discard) {
        // Leave only discontinuities in the queue.
        List<sp<ABuffer> >::iterator it = mBuffers.begin();
        ALOGD("drop %zu buffers", mBuffers.size());
        while (it != mBuffers.end()) {
            sp<ABuffer> oldBuffer = *it;

            int32_t oldDiscontinuityType;
            if (!oldBuffer->meta()->findInt32(
                        "discontinuity", &oldDiscontinuityType)) {
                it = mBuffers.erase(it);
                continue;
            }else{
                ALOGD("left a dis buffer");
            }

            ++it;
        }

        for (List<DiscontinuitySegment>::iterator it2 = mDiscontinuitySegments.begin();
                it2 != mDiscontinuitySegments.end();
                ++it2) {
            DiscontinuitySegment &seg = *it2;
            seg.clear();
        }

    }
    mEOSResult = OK;
    mLastQueuedTimeUs = 0;
    mLatestEnqueuedMeta = NULL;

    if (type == ATSParser::DISCONTINUITY_NONE) {
        return;
    }
#ifdef MTK_AOSP_ENHANCEMENT
    ++mQueuedDiscontinuityCount;
#endif
    mDiscontinuitySegments.push_back(DiscontinuitySegment());

    sp<ABuffer> buffer = new ABuffer(0);
    buffer->meta()->setInt32("discontinuity", static_cast<int32_t>(type));
    buffer->meta()->setMessage("extra", extra);

    mBuffers.push_back(buffer);
    mCondition.signal();
}

void AnotherPacketSource::signalEOS(status_t result) {
    CHECK(result != OK);
    Mutex::Autolock autoLock(mLock);
    mEOSResult = result;
    mCondition.signal();
}

bool AnotherPacketSource::hasBufferAvailable(status_t *finalResult) {
    Mutex::Autolock autoLock(mLock);
    *finalResult = OK;
    if (!mEnabled) {
        return false;
    }
    if (!mBuffers.empty()) {
        return true;
    }

    *finalResult = mEOSResult;
    return false;
}

bool AnotherPacketSource::hasDataBufferAvailable(status_t *finalResult) {
    Mutex::Autolock autoLock(mLock);
    *finalResult = OK;
    if (!mEnabled) {
        return false;
    }
    List<sp<ABuffer> >::iterator it;
    for (it = mBuffers.begin(); it != mBuffers.end(); it++) {
        int32_t discontinuity;
        if (!(*it)->meta()->findInt32("discontinuity", &discontinuity)) {
            return true;
        }
    }

    *finalResult = mEOSResult;
    return false;
}

size_t AnotherPacketSource::getAvailableBufferCount(status_t *finalResult) {
    Mutex::Autolock autoLock(mLock);

    *finalResult = OK;
    if (!mEnabled) {
        return 0;
    }
    if (!mBuffers.empty()) {
        return mBuffers.size();
    }
    *finalResult = mEOSResult;
    return 0;
}

int64_t AnotherPacketSource::getBufferedDurationUs(status_t *finalResult) {
    Mutex::Autolock autoLock(mLock);
    *finalResult = mEOSResult;

    int64_t durationUs = 0;
    for (List<DiscontinuitySegment>::iterator it = mDiscontinuitySegments.begin();
            it != mDiscontinuitySegments.end();
            ++it) {
        const DiscontinuitySegment &seg = *it;
        // dequeued access units should be a subset of enqueued access units
        // CHECK(seg.maxEnqueTimeUs >= seg.mMaxDequeTimeUs);
        durationUs += (seg.mMaxEnqueTimeUs - seg.mMaxDequeTimeUs);
    }

    return durationUs;
}

#ifdef MTK_AOSP_ENHANCEMENT
int64_t AnotherPacketSource::getBufferedDurationUs_l(status_t *finalResult) {
    *finalResult = mEOSResult;

    if (mBuffers.empty()) {
        return 0;
    }

    int64_t time1 = -1;
    int64_t time2 = -1;
    int64_t durationUs = 0;

    List<sp<ABuffer> >::iterator it = mBuffers.begin();
    while (it != mBuffers.end()) {
        const sp<ABuffer> &buffer = *it;

        int64_t timeUs;
        if (buffer->meta()->findInt64("timeUs", &timeUs)) {
            if (time1 < 0 || timeUs < time1) {
                time1 = timeUs;
            }

            if (time2 < 0 || timeUs > time2) {
                time2 = timeUs;
            }
        } else {
            // This is a discontinuity, reset everything.
            if(time1 != -1 && time2 != -1){
                durationUs += time2 - time1;
                time1 = time2 = -1;
                ALOGE("discontinuity duration:%lld, time1:%lld, time2:%lld",(long long)durationUs,(long long)time1,(long long)time2);
            }
        }

        ++it;
    }
    if(time1 != -1 && time2 != -1){
        durationUs += (time2 - time1);
    }
    ALOGD("getBufferedDurationUs_l time1:%lld,time2:%lld",(long long)time1,(long long)time2);
    //return durationUs + (time2 - time1);
    return durationUs;
}
#endif

// A cheaper but less precise version of getBufferedDurationUs that we would like to use in
// LiveSession::dequeueAccessUnit to trigger downwards adaptation.
#ifdef MTK_AOSP_ENHANCEMENT
int64_t AnotherPacketSource::getEstimatedDurationUs() {
    Mutex::Autolock autoLock(mLock);
    if (mBuffers.empty()) {
        return 0;
    }

    if (mQueuedDiscontinuityCount > 0) {
        status_t finalResult;
        return getBufferedDurationUs_l(&finalResult);
    }

    List<sp<ABuffer> >::iterator it = mBuffers.begin();
    sp<ABuffer> buffer = *it;

    int64_t startTimeUs;
    buffer->meta()->findInt64("timeUs", &startTimeUs);
    if (startTimeUs < 0) {
        return 0;
    }

    it = mBuffers.end();
    --it;
    buffer = *it;

    int64_t endTimeUs;
    buffer->meta()->findInt64("timeUs", &endTimeUs);
    if (endTimeUs < 0) {
        return 0;
    }

    int64_t diffUs;
    if (endTimeUs > startTimeUs) {
        diffUs = endTimeUs - startTimeUs;
    } else {
        diffUs = startTimeUs - endTimeUs;
    }
    return diffUs;
}
#endif

status_t AnotherPacketSource::nextBufferTime(int64_t *timeUs) {
    *timeUs = 0;

    Mutex::Autolock autoLock(mLock);

    if (mBuffers.empty()) {
        return mEOSResult != OK ? mEOSResult : -EWOULDBLOCK;
    }

    sp<ABuffer> buffer = *mBuffers.begin();
    CHECK(buffer->meta()->findInt64("timeUs", timeUs));

    return OK;
}

bool AnotherPacketSource::isFinished(int64_t duration) const {
    if (duration > 0) {
        int64_t diff = duration - mLastQueuedTimeUs;
        if (diff < kNearEOSMarkUs && diff > -kNearEOSMarkUs) {
#ifdef MTK_AOSP_ENHANCEMENT
            ALOGD("Detecting EOS due to near end");
#else
            ALOGV("Detecting EOS due to near end");
#endif
            return true;
        }
    }
    return (mEOSResult != OK);
}

sp<AMessage> AnotherPacketSource::getLatestEnqueuedMeta() {
    Mutex::Autolock autoLock(mLock);
    return mLatestEnqueuedMeta;
}

sp<AMessage> AnotherPacketSource::getLatestDequeuedMeta() {
    Mutex::Autolock autoLock(mLock);
    return mLatestDequeuedMeta;
}

void AnotherPacketSource::enable(bool enable) {
    Mutex::Autolock autoLock(mLock);
    mEnabled = enable;
}

/*
 * returns the sample meta that's delayUs after queue head
 * (NULL if such sample is unavailable)
 */
sp<AMessage> AnotherPacketSource::getMetaAfterLastDequeued(int64_t delayUs) {
    Mutex::Autolock autoLock(mLock);
    int64_t firstUs = -1;
    int64_t lastUs = -1;
    int64_t durationUs = 0;

    List<sp<ABuffer> >::iterator it;
    for (it = mBuffers.begin(); it != mBuffers.end(); ++it) {
        const sp<ABuffer> &buffer = *it;
        int32_t discontinuity;
        if (buffer->meta()->findInt32("discontinuity", &discontinuity)) {
            durationUs += lastUs - firstUs;
            firstUs = -1;
            lastUs = -1;
            continue;
        }
        int64_t timeUs;
        if (buffer->meta()->findInt64("timeUs", &timeUs)) {
            if (firstUs < 0) {
                firstUs = timeUs;
            }
            if (lastUs < 0 || timeUs > lastUs) {
                lastUs = timeUs;
            }
            if (durationUs + (lastUs - firstUs) >= delayUs) {
                return buffer->meta();
            }
        }
    }
    return NULL;
}

/*
 * removes samples with time equal or after meta
 */
void AnotherPacketSource::trimBuffersAfterMeta(
        const sp<AMessage> &meta) {
    if (meta == NULL) {
        ALOGW("trimming with NULL meta, ignoring");
        return;
    }

    Mutex::Autolock autoLock(mLock);
    if (mBuffers.empty()) {
        return;
    }

    HLSTime stopTime(meta);
    ALOGV("trimBuffersAfterMeta: discontinuitySeq %d, timeUs %lld",
            stopTime.mSeq, (long long)stopTime.mTimeUs);

    List<sp<ABuffer> >::iterator it;
    List<DiscontinuitySegment >::iterator it2;
    sp<AMessage> newLatestEnqueuedMeta = NULL;
    int64_t newLastQueuedTimeUs = 0;
    for (it = mBuffers.begin(), it2 = mDiscontinuitySegments.begin(); it != mBuffers.end(); ++it) {
        const sp<ABuffer> &buffer = *it;
        int32_t discontinuity;
        if (buffer->meta()->findInt32("discontinuity", &discontinuity)) {
            // CHECK(it2 != mDiscontinuitySegments.end());
            ++it2;
            continue;
        }

        HLSTime curTime(buffer->meta());
        if (!(curTime < stopTime)) {
            ALOGV("trimming from %lld (inclusive) to end",
                    (long long)curTime.mTimeUs);
            break;
        }
        newLatestEnqueuedMeta = buffer->meta();
        newLastQueuedTimeUs = curTime.mTimeUs;
    }

    mBuffers.erase(it, mBuffers.end());
    mLatestEnqueuedMeta = newLatestEnqueuedMeta;
    mLastQueuedTimeUs = newLastQueuedTimeUs;

    DiscontinuitySegment &seg = *it2;
    if (newLatestEnqueuedMeta != NULL) {
        seg.mMaxEnqueTimeUs = newLastQueuedTimeUs;
    } else {
        seg.clear();
    }
    mDiscontinuitySegments.erase(++it2, mDiscontinuitySegments.end());
}

/*
 * removes samples with time equal or before meta;
 * returns first sample left in the queue.
 *
 * (for AVC, if trim happens, the samples left will always start
 * at next IDR.)
 */
sp<AMessage> AnotherPacketSource::trimBuffersBeforeMeta(
        const sp<AMessage> &meta) {
    HLSTime startTime(meta);
    ALOGV("trimBuffersBeforeMeta: discontinuitySeq %d, timeUs %lld",
            startTime.mSeq, (long long)startTime.mTimeUs);

    sp<AMessage> firstMeta;
    int64_t firstTimeUs = -1;
    Mutex::Autolock autoLock(mLock);
    if (mBuffers.empty()) {
        return NULL;
    }

    sp<MetaData> format;
    bool isAvc = false;

    List<sp<ABuffer> >::iterator it;
    for (it = mBuffers.begin(); it != mBuffers.end(); ++it) {
        const sp<ABuffer> &buffer = *it;
        int32_t discontinuity;
        if (buffer->meta()->findInt32("discontinuity", &discontinuity)) {
            mDiscontinuitySegments.erase(mDiscontinuitySegments.begin());
            // CHECK(!mDiscontinuitySegments.empty());
            format = NULL;
            isAvc = false;
            continue;
        }
        if (format == NULL) {
            sp<RefBase> object;
            if (buffer->meta()->findObject("format", &object)) {
                const char* mime;
                format = static_cast<MetaData*>(object.get());
                isAvc = format != NULL
                        && format->findCString(kKeyMIMEType, &mime)
                        && !strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_AVC);
            }
        }
        if (isAvc && !IsIDR(buffer)) {
            continue;
        }

        HLSTime curTime(buffer->meta());
        if (startTime < curTime) {
            ALOGV("trimming from beginning to %lld (not inclusive)",
                    (long long)curTime.mTimeUs);
            firstMeta = buffer->meta();
            firstTimeUs = curTime.mTimeUs;
            break;
        }
    }
    mBuffers.erase(mBuffers.begin(), it);
    mLatestDequeuedMeta = NULL;

    // CHECK(!mDiscontinuitySegments.empty());
    DiscontinuitySegment &seg = *mDiscontinuitySegments.begin();
    if (firstTimeUs >= 0) {
        seg.mMaxDequeTimeUs = firstTimeUs;
    } else {
        seg.clear();
    }

    return firstMeta;
}

#ifdef MTK_AOSP_ENHANCEMENT
bool AnotherPacketSource::getNSN(int32_t * uiNextSeqNum) {
    Mutex::Autolock autoLock(mLock);
    if (!mBuffers.empty()) {
        if (m_uiNextAduSeqNum != -1) {
            *uiNextSeqNum = m_uiNextAduSeqNum;
            return true;
        }
        *uiNextSeqNum = (*mBuffers.begin())->int32Data();
        return true;
    }
    return false;
}

size_t AnotherPacketSource::getFreeBufSpace() {
    size_t bufSizeUsed = 0;

    if (mBuffers.empty()) {
        return m_BufQueSize;
    }

    List<sp<ABuffer> >::iterator it = mBuffers.begin();
    while (it != mBuffers.end()) {
        bufSizeUsed += (*it)->size();
        it++;
    }
    if (bufSizeUsed >= m_BufQueSize)
        return 0;

    return m_BufQueSize - bufSizeUsed;
}
unsigned AnotherPacketSource::getSourcePID(){
    return mStrmSourcePID;
}

void AnotherPacketSource::setSourcePID(unsigned uStrmPid){
    ALOGD("setSourcePID 0x%x",uStrmPid);
    mStrmSourcePID = uStrmPid;
    ALOGD("setSourcePID after 0x%x",mStrmSourcePID);
}

status_t AnotherPacketSource::isEOS() {
    return mIsEOS;
}

void AnotherPacketSource::setScanForIDR(bool enable) {
    mNeedScanForIDR = enable;
}

status_t AnotherPacketSource::CheckFormatChangeBuffer(void)
{
    Mutex::Autolock autoLock(mLock);

    List<sp<ABuffer> >::iterator it = mBuffers.begin();
     while (it != mBuffers.end()) {
        sp<ABuffer> buf = *it;
        int32_t discontinuity;
        if (buf->meta()->findInt32("discontinuity", &discontinuity)) {
            if (wasFormatChange(discontinuity)) {
                ALOGD("find formatchange discontinuity...");
                return OK;
            }
        }
        ++it;
    }
    return UNKNOWN_ERROR;
}
#endif
}  // namespace android
