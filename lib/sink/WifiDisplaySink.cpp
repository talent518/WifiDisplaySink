/*
 * Copyright 2012, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "WifiDisplaySinkCPP"
#include <utils/Log.h>

#include "WifiDisplaySink.h"

#include "DirectRenderer.h"
#include "MediaReceiver.h"
#include "TimeSyncer.h"
#include "PlantUtils.h"

#include <cutils/properties.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/ParsedMessage.h>
#include <media/stagefright/foundation/AString.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/Utils.h>

#include <gui/ISurfaceComposer.h>
#include <gui/SurfaceComposerClient.h>
#include <ui/DisplayInfo.h>

namespace android {

// static
const AString WifiDisplaySink::sUserAgent = MakeUserAgent();

WifiDisplaySink::WifiDisplaySink(
        sp<ALooper> looper,
        uint32_t flags,
        const sp<ANetworkSession> &netSession,
        //const sp<IGraphicBufferProducer> &bufferProducer,
        const sp<AMessage> &notify)
    : mWfdSinkLooper(looper),
      mState(UNDEFINED),
      mFlags(flags),
      mNetSession(netSession),
      //mSurfaceTex(bufferProducer),
      mNotify(notify),
      mUsingTCPTransport(false),
      mUsingTCPInterleaving(false),
      mSessionID(0),
      mNextCSeq(1),
      mIDRFrameRequestPending(false),
      mTimeOffsetUs(0ll),
      mTimeOffsetValid(false),
      mSetupDeferred(false),
      mLatencyCount(0),
      mLatencySumUs(0ll),
      mLatencyMaxUs(0ll),
      mMaxDelayMs(-1ll),
      mListener(NULL) {

    mLocalRtpPort = RTPBase::PickRandomRTPPort();

    bool isN10 = false;
    bool isFHD = false;
    // self parse
    FILE* fp = fopen("/data/data/com.example.mira4u/shared_prefs/prefs.xml", "r");
    if (fp == NULL) {
        ALOGE("WifiDisplaySink() fopen error[%d]", errno);
    } else {
        char line[80];
        while( fgets(line , sizeof(line) , fp) != NULL ) {
            char lin[80];
            memset(lin, 0, 80);
            ALOGD("[%s]", strncpy(lin, line, strlen(line)-1)); // delete CR
            int32_t val = -1;
            int32_t ret = sscanf(line, "    <string name=\"persist.sys.wfd.sinkisn10\">%d</string>", &val);
            if (ret == 1 && val == 1) {
                isN10 = true;
                continue;
            }
            val = -1;
            ret = sscanf(line, "    <string name=\"persist.sys.wfd.fhdsink\">%d</string>", &val);
            if (ret == 1 && val == 1) {
                isFHD = true;
                continue;
            }
        }
        fclose(fp);
    }

    // try to negotiate with the native display resolution.
    sp<SurfaceComposerClient> composerClient = new SurfaceComposerClient;
    CHECK_EQ(composerClient->initCheck(), (status_t)OK);
    sp<IBinder> display(SurfaceComposerClient::getBuiltInDisplay(ISurfaceComposer::eDisplayIdMain));
    DisplayInfo info;
    SurfaceComposerClient::getDisplayInfo(display, &info);
    ssize_t displayWidth = info.w;
    ssize_t displayHeight = info.h;
    ALOGD("Native Sink Display[%d, %d]", displayWidth, displayHeight);

    mSinkSupportedVideoFormats.disableAll();

    VideoFormats::ResolutionType resolutionType;
    size_t index = 0;
    if (VideoFormats::getResolutionType(displayWidth, displayHeight, &resolutionType, &index)) {
        mSinkSupportedVideoFormats.setNativeResolution(resolutionType, index);
        // carcenter display info: 800x480 p30 == VideoFormats::HH, 0
        // Enable all resolutions up to native resolution
        mSinkSupportedVideoFormats.enableResolutionUpto(
        resolutionType, index,
        VideoFormats::PROFILE_CBP,
        VideoFormats::LEVEL_32);
        ALOGI("enable resolution up to (ResolutionType: %d, Index: %d)", resolutionType, index);
        return;
    }

    ALOGD("WifiDisplaySink() Sink is Nexus10[%d], FullHD[%d]", isN10, isFHD);
    if (isFHD) {
        mSinkSupportedVideoFormats.setNativeResolution(
            VideoFormats::RESOLUTION_CEA, 8);  // 1920 x 1080 p60
    } else {
        // We support any and all resolutions, but prefer 720p30
        mSinkSupportedVideoFormats.setNativeResolution(
            VideoFormats::RESOLUTION_CEA, 5);  // 1280 x 720 p30
    }

    mSinkSupportedVideoFormats.enableAll();

}

void WifiDisplaySink::stop() {
	if(mSessionID) {
		mNetSession->destroySession(mSessionID);
		mSessionID = 0;
	}
}

WifiDisplaySink::~WifiDisplaySink() {
}

status_t WifiDisplaySink::setListener(const sp<WfdSinkListener>& listener) {
    ALOGD("setListener");
    mListener = listener;
    return NO_ERROR;
}

void WifiDisplaySink::setDisplay(const sp<IGraphicBufferProducer>& bufferProducer) {
    ALOGD("setDisplay");
    mSurfaceTex = bufferProducer;
}

void WifiDisplaySink::start(const char *sourceHost, int32_t sourcePort) {
    sp<AMessage> msg = PlantUtils::newAMessage(kWhatStart, this);
    msg->setString("sourceHost", sourceHost);
    msg->setInt32("sourcePort", sourcePort);
    msg->post();
}

void WifiDisplaySink::start(const char *uri) {
    sp<AMessage> msg = PlantUtils::newAMessage(kWhatStart, this);
    msg->setString("setupURI", uri);
    msg->post();
}

// static
bool WifiDisplaySink::ParseURL(
        const char *url, AString *host, int32_t *port, AString *path,
        AString *user, AString *pass) {
    host->clear();
    *port = 0;
    path->clear();
    user->clear();
    pass->clear();

    if (strncasecmp("rtsp://", url, 7)) {
        return false;
    }

    const char *slashPos = strchr(&url[7], '/');

    if (slashPos == NULL) {
        host->setTo(&url[7]);
        path->setTo("/");
    } else {
        host->setTo(&url[7], slashPos - &url[7]);
        path->setTo(slashPos);
    }

    ssize_t atPos = host->find("@");

    if (atPos >= 0) {
        // Split of user:pass@ from hostname.

        AString userPass(*host, 0, atPos);
        host->erase(0, atPos + 1);

        ssize_t colonPos = userPass.find(":");

        if (colonPos < 0) {
            *user = userPass;
        } else {
            user->setTo(userPass, 0, colonPos);
            pass->setTo(userPass, colonPos + 1, userPass.size() - colonPos - 1);
        }
    }

    const char *colonPos = strchr(host->c_str(), ':');

    if (colonPos != NULL) {
        char *end;
        unsigned long x = strtoul(colonPos + 1, &end, 10);

        if (end == colonPos + 1 || *end != '\0' || x >= 65536) {
            return false;
        }

        *port = x;

        size_t colonOffset = colonPos - host->c_str();
        size_t trailing = host->size() - colonOffset;
        host->erase(colonOffset, trailing);
    } else {
        *port = 554;
    }

    return true;
}

void WifiDisplaySink::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatStart:
        {
            sleep(2);  // XXX

            int32_t sourcePort;
            CHECK(msg->findString("sourceHost", &mRTSPHost));
            CHECK(msg->findInt32("sourcePort", &sourcePort));

            sp<AMessage> notify = PlantUtils::newAMessage(kWhatRTSPNotify, this);

            status_t err = mNetSession->createRTSPClient(
                    mRTSPHost.c_str(), sourcePort, notify, &mSessionID);
            CHECK_EQ(err, (status_t)OK);

            mState = CONNECTING;
            break;
        }

        case kWhatRTSPNotify:
        {
            int32_t reason;
            CHECK(msg->findInt32("reason", &reason));

            switch (reason) {
                case ANetworkSession::kWhatError:
                {
                    mListener->notify(WFD_ERROR, WFD_ERROR_UNKNOWN, 0, NULL);

                    int32_t sessionID;
                    CHECK(msg->findInt32("sessionID", &sessionID));

                    int32_t err;
                    CHECK(msg->findInt32("err", &err));

                    AString detail;
                    CHECK(msg->findString("detail", &detail));

                    ALOGE("An error occurred in session %d (%d, '%s/%s').",
                          sessionID,
                          err,
                          detail.c_str(),
                          strerror(-err));

                    if (sessionID == mSessionID) {
                        ALOGI("Lost control connection.");

                        // The control connection is dead now.
                        mNetSession->destroySession(mSessionID);
                        mSessionID = 0;

                        if (mNotify == NULL) {
                            looper()->stop();
                        } else {
                            sp<AMessage> notify = mNotify->dup();
                            notify->setInt32("what", kWhatDisconnected);
                            notify->post();
                        }
                    }
                    break;
                }

                case ANetworkSession::kWhatConnected:
                {
                    ALOGI("We're now connected.");
                    mState = CONNECTED;

                    if (mFlags & FLAG_SPECIAL_MODE) {
                        sp<AMessage> notify = PlantUtils::newAMessage(
                                kWhatTimeSyncerNotify, this);

                        mTimeSyncer = new TimeSyncer(mNetSession, notify);
                        looper()->registerHandler(mTimeSyncer);

                        mTimeSyncer->startClient(mRTSPHost.c_str(), 8123);
                    }
                    break;
                }

                case ANetworkSession::kWhatData:
                {
                    onReceiveClientData(msg);
                    break;
                }

                default:
                    TRESPASS();
            }
            break;
        }

        case kWhatStop:
        {
            looper()->stop();
            break;
        }

        case kWhatMediaReceiverNotify:
        {
            onMediaReceiverNotify(msg);
            break;
        }

        case kWhatTimeSyncerNotify:
        {
            int32_t what;
            CHECK(msg->findInt32("what", &what));

            if (what == TimeSyncer::kWhatTimeOffset) {
                CHECK(msg->findInt64("offset", &mTimeOffsetUs));
                mTimeOffsetValid = true;

                if (mSetupDeferred) {
                    CHECK_EQ((status_t)OK,
                             sendSetup(
                                mSessionID,
                                "rtsp://x.x.x.x:x/wfd1.0/streamid=0"));

                    mSetupDeferred = false;
                }
            }
            break;
        }

        case kWhatReportLateness:
        {
            if (mLatencyCount > 0) {
                int64_t avgLatencyUs = mLatencySumUs / mLatencyCount;

                ALOGD("avg. latency = %lld ms (max %lld ms)",
                      avgLatencyUs / 1000ll,
                      mLatencyMaxUs / 1000ll);

                sp<AMessage> params = new AMessage;
                params->setInt64("avgLatencyUs", avgLatencyUs);
                params->setInt64("maxLatencyUs", mLatencyMaxUs);
                mMediaReceiver->informSender(0 /* trackIndex */, params);
            }

            mLatencyCount = 0;
            mLatencySumUs = 0ll;
            mLatencyMaxUs = 0ll;

            msg->post(kReportLatenessEveryUs);
            break;
        }

        default:
            TRESPASS();
    }
}

void WifiDisplaySink::dumpDelay(size_t trackIndex, int64_t timeUs) {
    int64_t delayMs = (ALooper::GetNowUs() - timeUs) / 1000ll;

    if (delayMs > mMaxDelayMs) {
        mMaxDelayMs = delayMs;
    }

    static const int64_t kMinDelayMs = 0;
    static const int64_t kMaxDelayMs = 300;

    const char *kPattern = "########################################";
    size_t kPatternSize = strlen(kPattern);

    int n = (kPatternSize * (delayMs - kMinDelayMs))
                / (kMaxDelayMs - kMinDelayMs);

    if (n < 0) {
        n = 0;
    } else if ((size_t)n > kPatternSize) {
        n = kPatternSize;
    }

    ALOGI("[%lld]: (%4lld ms / %4lld ms) %s",
          timeUs / 1000,
          delayMs,
          mMaxDelayMs,
          kPattern + kPatternSize - n);
}

void WifiDisplaySink::onMediaReceiverNotify(const sp<AMessage> &msg) {
    int32_t what;
    CHECK(msg->findInt32("what", &what));

    switch (what) {
        case MediaReceiver::kWhatInitDone:
        {
            status_t err;
            CHECK(msg->findInt32("err", &err));

            ALOGI("MediaReceiver initialization completed w/ err %d", err);
            break;
        }

        case MediaReceiver::kWhatError:
        {
            status_t err;
            CHECK(msg->findInt32("err", &err));

            mListener->notify(WFD_ERROR, WFD_ERROR_UNKNOWN, 0, NULL);
            ALOGE("MediaReceiver signaled error %d", err);
            break;
        }

        case MediaReceiver::kWhatAccessUnit:
        {
            if (mRenderer == NULL) {
                mRenderer = new DirectRenderer(mSurfaceTex);
                looper()->registerHandler(mRenderer);
            }

            sp<ABuffer> accessUnit;
            CHECK(msg->findBuffer("accessUnit", &accessUnit));

            int64_t timeUs;
            CHECK(accessUnit->meta()->findInt64("timeUs", &timeUs));

            if (!mTimeOffsetValid && !(mFlags & FLAG_SPECIAL_MODE)) {
                mTimeOffsetUs = timeUs - ALooper::GetNowUs();
                mTimeOffsetValid = true;
            }

            CHECK(mTimeOffsetValid);

            // We are the timesync _client_,
            // client time = server time - time offset.
            timeUs -= mTimeOffsetUs;

            size_t trackIndex;
            CHECK(msg->findSize("trackIndex", &trackIndex));

            int64_t nowUs = ALooper::GetNowUs();
            int64_t delayUs = nowUs - timeUs;

            mLatencySumUs += delayUs;
            if (mLatencyCount == 0 || delayUs > mLatencyMaxUs) {
                mLatencyMaxUs = delayUs;
            }
            ++mLatencyCount;

            // dumpDelay(trackIndex, timeUs);
            //ALOGD("track[%d] AU delay: %lld", trackIndex, delayUs);

            //it's to syc with audio in a rough way taking the audio hardware latency in to consideration.
            timeUs += 220000ll;  // Assume 220 ms of latency
            accessUnit->meta()->setInt64("timeUs", timeUs);

            sp<AMessage> format;
            if (msg->findMessage("format", &format)) {
                mRenderer->setFormat(trackIndex, format);
            }

            mRenderer->queueAccessUnit(trackIndex, accessUnit);
            break;
        }

        case MediaReceiver::kWhatPacketLost:
        {
#if 1
            if (!mIDRFrameRequestPending) {
                ALOGI("requesting IDR frame");

                sendIDRFrameRequest(mSessionID);

                // send an urgent late info which is not a precise value (fake, in some extent).
                // the purpose is simple to triggle Source's bitrate/fps control logic.
                sp<AMessage> params = new AMessage;
                params->setInt64("avgLatencyUs", 500000ll);
                params->setInt64("maxLatencyUs", 500000ll);
                mMediaReceiver->informSender(0 /* trackIndex */, params);
                mLatencyCount = 1;
                mLatencySumUs = 500000ll;
                mLatencyMaxUs = 500000ll;
            }
#endif
            break;
        }

        default:
            TRESPASS();
    }
}

void WifiDisplaySink::registerResponseHandler(
        int32_t sessionID, int32_t cseq, HandleRTSPResponseFunc func) {
    ResponseID id;
    id.mSessionID = sessionID;
    id.mCSeq = cseq;
    mResponseHandlers.add(id, func);
}

status_t WifiDisplaySink::sendM2(int32_t sessionID) {
    AString request = "OPTIONS * RTSP/1.0\r\n";
    AppendCommonResponse(&request, mNextCSeq);

    request.append(
            "Require: org.wfa.wfd1.0\r\n"
            "\r\n");

    status_t err =
        mNetSession->sendRequest(sessionID, request.c_str(), request.size());

    if (err != OK) {
        return err;
    }

    registerResponseHandler(
            sessionID, mNextCSeq, &WifiDisplaySink::onReceiveM2Response);

    ++mNextCSeq;

    return OK;
}

status_t WifiDisplaySink::onReceiveM2Response(
        int32_t sessionID, const sp<ParsedMessage> &msg) {
    int32_t statusCode;
    if (!msg->getStatusCode(&statusCode)) {
        return ERROR_MALFORMED;
    }

    if (statusCode != 200) {
        return ERROR_UNSUPPORTED;
    }

    return OK;
}

status_t WifiDisplaySink::onReceiveSetupResponse(
        int32_t sessionID, const sp<ParsedMessage> &msg) {
    int32_t statusCode;
    if (!msg->getStatusCode(&statusCode)) {
        return ERROR_MALFORMED;
    }

    if (statusCode != 200) {
        return ERROR_UNSUPPORTED;
    }

    if (!msg->findString("session", &mPlaybackSessionID)) {
        return ERROR_MALFORMED;
    }

    if (!ParsedMessage::GetInt32Attribute(
                mPlaybackSessionID.c_str(),
                "timeout",
                &mPlaybackSessionTimeoutSecs)) {
        mPlaybackSessionTimeoutSecs = -1;
    }

    ssize_t colonPos = mPlaybackSessionID.find(";");
    if (colonPos >= 0) {
        // Strip any options from the returned session id.
        mPlaybackSessionID.erase(
                colonPos, mPlaybackSessionID.size() - colonPos);
    }

    status_t err = configureTransport(msg);

    if (err != OK) {
        return err;
    }

    mState = PAUSED;

    return sendPlay(
            sessionID,
            "rtsp://x.x.x.x:x/wfd1.0/streamid=0");
}

status_t WifiDisplaySink::configureTransport(const sp<ParsedMessage> &msg) {
    if (mUsingTCPTransport && !(mFlags & FLAG_SPECIAL_MODE)) {
        // In "special" mode we still use a UDP RTCP back-channel that
        // needs connecting.
        return OK;
    }

    AString transport;
    if (!msg->findString("transport", &transport)) {
        ALOGE("Missing 'transport' field in SETUP response.");
        return ERROR_MALFORMED;
    }

    AString sourceHost;
    if (!ParsedMessage::GetAttribute(
                transport.c_str(), "source", &sourceHost)) {
        sourceHost = mRTSPHost;
    }

    AString serverPortStr;
    if (!ParsedMessage::GetAttribute(
                transport.c_str(), "server_port", &serverPortStr)) {
        ALOGE("Missing 'server_port' in Transport field.");
        return ERROR_MALFORMED;
    }


    int rtpPort, rtcpPort;
#if 0
    if (sscanf(serverPortStr.c_str(), "%d-%d", &rtpPort, &rtcpPort) != 2
            || rtpPort <= 0 || rtpPort > 65535
            || rtcpPort <=0 || rtcpPort > 65535
            || rtcpPort != rtpPort + 1) {
        ALOGE("Invalid server_port description '%s'.",
                serverPortStr.c_str());

        return ERROR_MALFORMED;
    }
#endif

    if (sscanf(serverPortStr.c_str(), "%d", &rtpPort) != 1
            || rtpPort <= 0 || rtpPort > 65535 ) {
        ALOGE("Invalid server_port description '%s'.",
                serverPortStr.c_str());

        return ERROR_MALFORMED;
    }
    ALOGD("we get rtp port: %d", rtpPort);
    rtcpPort = rtpPort + 1;

    int tmpRtcpPort;
    if (sscanf(serverPortStr.c_str(), "%d-%d", &rtpPort, &tmpRtcpPort) == 2) {
        rtcpPort = tmpRtcpPort;
        ALOGD("we get rtcp port: %d", rtcpPort);
    }

    if (rtpPort & 1) {
        ALOGW("Server picked an odd numbered RTP port.");
    }

    return mMediaReceiver->connectTrack(
            0 /* trackIndex */, sourceHost.c_str(), rtpPort, rtcpPort);
}

status_t WifiDisplaySink::onReceivePlayResponse(
        int32_t sessionID, const sp<ParsedMessage> &msg) {
    int32_t statusCode;
    if (!msg->getStatusCode(&statusCode)) {
        return ERROR_MALFORMED;
    }

    if (statusCode != 200) {
        return ERROR_UNSUPPORTED;
    }

    mState = PLAYING;

    (PlantUtils::newAMessage(kWhatReportLateness, this))->post(kReportLatenessEveryUs);

    return OK;
}

status_t WifiDisplaySink::onReceiveIDRFrameRequestResponse(
        int32_t sessionID, const sp<ParsedMessage> &msg) {
    CHECK(mIDRFrameRequestPending);
    mIDRFrameRequestPending = false;

    return OK;
}

void WifiDisplaySink::onReceiveClientData(const sp<AMessage> &msg) {
    int32_t sessionID;
    CHECK(msg->findInt32("sessionID", &sessionID));

    sp<RefBase> obj;
    CHECK(msg->findObject("data", &obj));

    sp<ParsedMessage> data =
        static_cast<ParsedMessage *>(obj.get());

    ALOGV("session %d received '%s'",
          sessionID, data->debugString().c_str());

    AString method;
    AString uri;
    data->getRequestField(0, &method);

    int32_t cseq;
    if (!data->findInt32("cseq", &cseq)) {
        sendErrorResponse(sessionID, "400 Bad Request", -1 /* cseq */);
        return;
    }

    if (method.startsWith("RTSP/")) {
        // This is a response.

        ResponseID id;
        id.mSessionID = sessionID;
        id.mCSeq = cseq;

        ssize_t index = mResponseHandlers.indexOfKey(id);

        if (index < 0) {
            ALOGW("Received unsolicited server response, cseq %d", cseq);
            return;
        }

        HandleRTSPResponseFunc func = mResponseHandlers.valueAt(index);
        mResponseHandlers.removeItemsAt(index);

        status_t err = (this->*func)(sessionID, data);
        CHECK_EQ(err, (status_t)OK);
    } else {
        AString version;
        data->getRequestField(2, &version);
        if (!(version == AString("RTSP/1.0"))) {
            sendErrorResponse(sessionID, "505 RTSP Version not supported", cseq);
            return;
        }

        if (method == "OPTIONS") {
            onOptionsRequest(sessionID, cseq, data);
        } else if (method == "GET_PARAMETER") {
            onGetParameterRequest(sessionID, cseq, data);
        } else if (method == "SET_PARAMETER") {
            onSetParameterRequest(sessionID, cseq, data);
        } else {
            sendErrorResponse(sessionID, "405 Method Not Allowed", cseq);
        }
    }
}

void WifiDisplaySink::onOptionsRequest(
        int32_t sessionID,
        int32_t cseq,
        const sp<ParsedMessage> &data) {
    AString response = "RTSP/1.0 200 OK\r\n";
    AppendCommonResponse(&response, cseq);
    response.append("Public: org.wfa.wfd1.0, GET_PARAMETER, SET_PARAMETER\r\n");
    response.append("\r\n");

    status_t err = mNetSession->sendRequest(sessionID, response.c_str());
    CHECK_EQ(err, (status_t)OK);

    err = sendM2(sessionID);
    CHECK_EQ(err, (status_t)OK);
}

void WifiDisplaySink::onGetParameterRequest(
        int32_t sessionID,
        int32_t cseq,
        const sp<ParsedMessage> &data) {
    AString body;

    if (mState == CONNECTED) {
        mUsingTCPTransport = false;
        mUsingTCPInterleaving = false;

        char val[PROPERTY_VALUE_MAX];
        if (property_get("media.wfd-sink.tcp-mode", val, NULL)) {
            if (!strcasecmp("true", val) || !strcmp("1", val)) {
                ALOGI("Using TCP unicast transport.");
                mUsingTCPTransport = true;
                mUsingTCPInterleaving = false;
            } else if (!strcasecmp("interleaved", val)) {
                ALOGI("Using TCP interleaved transport.");
                mUsingTCPTransport = true;
                mUsingTCPInterleaving = true;
            }
        } else if (mFlags & FLAG_SPECIAL_MODE) {
            mUsingTCPTransport = true;
        }

        body = "wfd_video_formats: ";
        body.append(mSinkSupportedVideoFormats.getFormatSpec());

        body.append(
                "\r\nwfd_audio_codecs: AAC 0000000F 00\r\n"
                "wfd_client_rtp_ports: RTP/AVP/");

        if (mUsingTCPTransport) {
            body.append("TCP;");
            if (mUsingTCPInterleaving) {
                body.append("interleaved");
            } else {
                //body.append("unicast 19000 0");
                body.append(PlantUtils::newStringPrintf("unicast %d 0", mLocalRtpPort));
            }
        } else {
            //body.append("UDP;unicast 19000 0");
            body.append(PlantUtils::newStringPrintf("UDP;unicast %d 0", mLocalRtpPort));
        }

        body.append(" mode=play\r\n");
    }

    AString response = "RTSP/1.0 200 OK\r\n";
    AppendCommonResponse(&response, cseq);
    response.append("Content-Type: text/parameters\r\n");
    response.append(PlantUtils::newStringPrintf("Content-Length: %d\r\n", body.size()));
    response.append("\r\n");
    response.append(body);

    status_t err = mNetSession->sendRequest(sessionID, response.c_str());
    CHECK_EQ(err, (status_t)OK);
}

status_t WifiDisplaySink::sendSetup(int32_t sessionID, const char *uri) {
    sp<AMessage> notify = PlantUtils::newAMessage(kWhatMediaReceiverNotify, this);

    mMediaReceiverLooper = new ALooper;
    mMediaReceiverLooper->setName("media_receiver");

    mMediaReceiverLooper->start(
            false /* runOnCallingThread */,
            false /* canCallJava */,
            PRIORITY_AUDIO);

    mMediaReceiver = new MediaReceiver(mNetSession, notify);
    mMediaReceiverLooper->registerHandler(mMediaReceiver);

    RTPReceiver::TransportMode rtpMode = RTPReceiver::TRANSPORT_UDP;
    if (mUsingTCPTransport) {
        if (mUsingTCPInterleaving) {
            rtpMode = RTPReceiver::TRANSPORT_TCP_INTERLEAVED;
        } else {
            rtpMode = RTPReceiver::TRANSPORT_TCP;
        }
    }

    int32_t localRTPPort;
    status_t err = mMediaReceiver->addTrack(
            rtpMode, RTPReceiver::TRANSPORT_UDP /* rtcpMode */, &localRTPPort, mLocalRtpPort);

    if (err == OK) {
        err = mMediaReceiver->initAsync(MediaReceiver::MODE_TRANSPORT_STREAM);
    }

    if (err != OK) {
        mMediaReceiverLooper->unregisterHandler(mMediaReceiver->id());
        mMediaReceiver.clear();

        mMediaReceiverLooper->stop();
        mMediaReceiverLooper.clear();

        return err;
    }

    AString request = PlantUtils::newStringPrintf("SETUP %s RTSP/1.0\r\n", uri);

    AppendCommonResponse(&request, mNextCSeq);

    if (rtpMode == RTPReceiver::TRANSPORT_TCP_INTERLEAVED) {
        request.append("Transport: RTP/AVP/TCP;interleaved=0-1\r\n");
    } else if (rtpMode == RTPReceiver::TRANSPORT_TCP) {
        if (mFlags & FLAG_SPECIAL_MODE) {
            // This isn't quite true, since the RTP connection is through TCP
            // and the RTCP connection through UDP...
            request.append(
                    PlantUtils::newStringPrintf(
                        "Transport: RTP/AVP/TCP;unicast;client_port=%d-%d\r\n",
                        localRTPPort, localRTPPort + 1));
        } else {
            request.append(
                    PlantUtils::newStringPrintf(
                        "Transport: RTP/AVP/TCP;unicast;client_port=%d\r\n",
                        localRTPPort));
        }
    } else {
        request.append(
                PlantUtils::newStringPrintf(
                    "Transport: RTP/AVP/UDP;unicast;client_port=%d-%d\r\n",
                    localRTPPort,
                    localRTPPort + 1));
    }

    request.append("\r\n");

    ALOGV("request = '%s'", request.c_str());

    err = mNetSession->sendRequest(sessionID, request.c_str(), request.size());

    if (err != OK) {
        return err;
    }

    registerResponseHandler(
            sessionID, mNextCSeq, &WifiDisplaySink::onReceiveSetupResponse);

    ++mNextCSeq;

    return OK;
}

status_t WifiDisplaySink::sendPlay(int32_t sessionID, const char *uri) {
    AString request = PlantUtils::newStringPrintf("PLAY %s RTSP/1.0\r\n", uri);

    AppendCommonResponse(&request, mNextCSeq);

    request.append(PlantUtils::newStringPrintf("Session: %s\r\n", mPlaybackSessionID.c_str()));
    request.append("\r\n");

    status_t err =
        mNetSession->sendRequest(sessionID, request.c_str(), request.size());

    if (err != OK) {
        return err;
    }

    registerResponseHandler(
            sessionID, mNextCSeq, &WifiDisplaySink::onReceivePlayResponse);

    ++mNextCSeq;

    return OK;
}

status_t WifiDisplaySink::sendIDRFrameRequest(int32_t sessionID) {
    CHECK(!mIDRFrameRequestPending);

    AString request = "SET_PARAMETER rtsp://localhost/wfd1.0 RTSP/1.0\r\n";

    AppendCommonResponse(&request, mNextCSeq);

    AString content = "wfd_idr_request\r\n";

    request.append(PlantUtils::newStringPrintf("Session: %s\r\n", mPlaybackSessionID.c_str()));
    request.append(PlantUtils::newStringPrintf("Content-Length: %d\r\n", content.size()));
    request.append("\r\n");
    request.append(content);

    status_t err =
        mNetSession->sendRequest(sessionID, request.c_str(), request.size());

    if (err != OK) {
        return err;
    }

    registerResponseHandler(
            sessionID,
            mNextCSeq,
            &WifiDisplaySink::onReceiveIDRFrameRequestResponse);

    ++mNextCSeq;

    mIDRFrameRequestPending = true;

    return OK;
}

void WifiDisplaySink::onSetParameterRequest(
        int32_t sessionID,
        int32_t cseq,
        const sp<ParsedMessage> &data) {
    const char *content = data->getContent();

    if (strstr(content, "wfd_trigger_method: SETUP\r\n") != NULL) {
        if ((mFlags & FLAG_SPECIAL_MODE) && !mTimeOffsetValid) {
            mSetupDeferred = true;
        } else {
            status_t err =
                sendSetup(
                        sessionID,
                        "rtsp://x.x.x.x:x/wfd1.0/streamid=0");

            CHECK_EQ(err, (status_t)OK);
        }
    }

    if (strstr(content, "wfd_trigger_method: TEARDOWN\r\n") != NULL) {
        ALOGD("wfd_trigger_method: TEARDOWN");
        if (mListener == NULL) {
            ALOGE("should set listener at first");
        }
        mListener->notify(WFD_INFO, WFD_INFO_RTSP_TEARDOWN, 0, NULL);
    }

    AString response = "RTSP/1.0 200 OK\r\n";
    AppendCommonResponse(&response, cseq);
    response.append("\r\n");

    status_t err = mNetSession->sendRequest(sessionID, response.c_str());
    CHECK_EQ(err, (status_t)OK);
}

void WifiDisplaySink::sendErrorResponse(
        int32_t sessionID,
        const char *errorDetail,
        int32_t cseq) {
    AString response;
    response.append("RTSP/1.0 ");
    response.append(errorDetail);
    response.append("\r\n");

    AppendCommonResponse(&response, cseq);

    response.append("\r\n");

    status_t err = mNetSession->sendRequest(sessionID, response.c_str());
    CHECK_EQ(err, (status_t)OK);
}

// static
void WifiDisplaySink::AppendCommonResponse(AString *response, int32_t cseq) {
    time_t now = time(NULL);
    struct tm *now2 = gmtime(&now);
    char buf[128];
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S %z", now2);

    response->append("Date: ");
    response->append(buf);
    response->append("\r\n");

    response->append(PlantUtils::newStringPrintf("User-Agent: %s\r\n", sUserAgent.c_str()));

    if (cseq >= 0) {
        response->append(PlantUtils::newStringPrintf("CSeq: %d\r\n", cseq));
    }
}

}  // namespace android
