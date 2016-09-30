/* Copyright (c) 2016, Fengping Bao <jamol@live.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "H2ConnectionImpl.h"
#include "util/kmtrace.h"
#include "util/base64.h"
#include "H2ConnectionMgr.h"
#include "Http2Response.h"

#include <sstream>
#include <algorithm>

using namespace kuma;

namespace {
#ifdef KUMA_HAS_OPENSSL
    static const AlpnProtos alpnProtos{ 2, 'h', '2' };
#endif
    static const std::string ClientConnectionPreface("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n");
}

const uint32_t H2_INITIAL_WINDOW_SIZE = 10*1024*1024;//2147418112;//2147483647;

//////////////////////////////////////////////////////////////////////////
H2Connection::Impl::Impl(EventLoop::Impl* loop)
: TcpConnection(loop), frameParser_(this), flow_ctrl_(0, [this] (uint32_t w) { sendWindowUpdate(0, w); })
{
    flow_ctrl_.initLocalWindowSize(H2_INITIAL_WINDOW_SIZE);
    flow_ctrl_.setMinLocalWindowSize(initLocalWindowSize_);
    flow_ctrl_.setLocalWindowStep(H2_INITIAL_WINDOW_SIZE);
    cmpPreface_ = ClientConnectionPreface;
    KM_SetObjKey("H2Connection");
    KUMA_INFOXTRACE("H2Connection");
}

H2Connection::Impl::~Impl()
{
    KUMA_INFOXTRACE("~H2Connection");
}

void H2Connection::Impl::cleanup()
{
    if (registeredToLoop_) {
        registeredToLoop_ = false;
        tcp_.getEventLoop()->removeListener(this);
    }
    tcp_.close();
    if (!key_.empty()) {
        auto &connMgr = H2ConnectionMgr::getRequestConnMgr(sslEnabled());
        std::string key(std::move(key_));
        // will destroy self when calling from loop stop
        connMgr.removeConnection(key);
    }
}

void H2Connection::Impl::setConnectionKey(const std::string &key)
{
    key_ = key;
    if (!key.empty()) {
        KUMA_INFOXTRACE("setConnectionKey, key="<<key);
    }
}

KMError H2Connection::Impl::connect(const std::string &host, uint16_t port, ConnectCallback cb)
{
    if(getState() != State::IDLE) {
        KUMA_ERRXTRACE("connect, invalid state, state="<<getState());
        return KMError::INVALID_STATE;
    }
    connect_cb_ = std::move(cb);
    
    // add to EventLoop to get notification when loop exit
    tcp_.getEventLoop()->addListener(this);
    registeredToLoop_ = true;
    return connect_i(host, port);
}

KMError H2Connection::Impl::connect_i(const std::string &host, uint16_t port)
{
    nextStreamId_ = 1;
    httpParser_.setDataCallback([this] (const char* data, size_t len) { onHttpData(data, len); });
    httpParser_.setEventCallback([this] (HttpEvent ev) { onHttpEvent(ev); });
    setState(State::CONNECTING);
    
    if (0 == port) {
        port = sslEnabled() ? 443 : 80;
    }
    
#ifdef KUMA_HAS_OPENSSL
    if (sslEnabled()) {
        tcp_.setAlpnProtocols(alpnProtos);
    }
#endif

    return TcpConnection::connect(host.c_str(), port);
}

KMError H2Connection::Impl::attachFd(SOCKET_FD fd, const uint8_t* data, size_t size)
{
    nextStreamId_ = 2;
    
    auto ret = TcpConnection::attachFd(fd, data, size);
    if (ret == KMError::NOERR) {
        if (sslEnabled()) {
            // waiting for client preface
            setState(State::HANDSHAKE);
            sendPreface();
        } else {
            // waiting for http upgrade reuqest
            setState(State::UPGRADING);
        }
    }
    return ret;
}

KMError H2Connection::Impl::attachSocket(TcpSocket::Impl&& tcp, HttpParser::Impl&& parser)
{
    KUMA_ASSERT(parser.isRequest());
    httpParser_ = std::move(parser);
    nextStreamId_ = 2;
    if (tcp.sslEnabled()) {
        setState(State::HANDSHAKE);
    } else {
        setState(State::UPGRADING);
    }
    
    auto ret = TcpConnection::attachSocket(std::move(tcp));
    if (ret != KMError::NOERR) {
        return ret;
    }
    
    if (tcp.sslEnabled()) {
        sendPreface();
        return KMError::NOERR;
    } else {
        return handleUpgradeRequest();
    }
}

KMError H2Connection::Impl::attachStream(uint32_t streamId, HttpResponse::Impl* rsp)
{
    return rsp->attachStream(this, streamId);
}

KMError H2Connection::Impl::close()
{
    KUMA_INFOXTRACE("close");
    
    if (getState() <= State::OPEN) {
        sendGoaway(H2Error::NOERR);
    }
    setState(State::CLOSED);
    cleanup();
    return KMError::NOERR;
}

KMError H2Connection::Impl::sendH2Frame(H2Frame *frame)
{
    if (!sendBufferEmpty() && !isControlFrame(frame)) {
        blocked_streams_.push_back(frame->getStreamId());
        return KMError::AGAIN;
    }
    
    if (isControlFrame(frame))
    {
        KUMA_INFOXTRACE("sendH2Frame, type="<<frame->type()<<", streamId="<<frame->getStreamId());
    }
    
    if (frame->type() == H2FrameType::HEADERS) {
        HeadersFrame *headers = dynamic_cast<HeadersFrame*>(frame);
        return sendHeadersFrame(headers);
    } else if (frame->type() == H2FrameType::DATA) {
        if (flow_ctrl_.getRemoteWindowSize() < frame->getPayloadLength()) {
            KUMA_INFOXTRACE("sendH2Frame, BUFFER_TOO_SMALL, win="<<flow_ctrl_.getRemoteWindowSize()<<", len="<<frame->getPayloadLength());
            blocked_streams_.push_back(frame->getStreamId());
            return KMError::BUFFER_TOO_SMALL;
        }
        flow_ctrl_.notifyBytesSent(frame->getPayloadLength());
    } else if (frame->type() == H2FrameType::WINDOW_UPDATE && frame->getStreamId() != 0) {
        //WindowUpdateFrame *wu = dynamic_cast<WindowUpdateFrame*>(frame);
        //flow_ctrl_.increaseLocalWindowSize(wu->getWindowSizeIncrement());
    }
    
    size_t payloadSize = frame->calcPayloadSize();
    size_t frameSize = payloadSize + H2_FRAME_HEADER_SIZE;
    size_t buffSize = 0;
    if (!sendBufferEmpty() && send_offset_) {
        buffSize = send_buffer_.size() - send_offset_;
        memmove(&send_buffer_[0], &send_buffer_[0] + send_offset_, buffSize);
    }
    if (buffSize + frameSize != send_buffer_.size()) {
        send_buffer_.resize(buffSize + frameSize);
    }
    int ret = frame->encode(&send_buffer_[0] + buffSize, frameSize);
    if (ret < 0) {
        KUMA_ERRXTRACE("sendH2Frame, failed to encode frame");
        return KMError::INVALID_PARAM;
    }
    send_offset_ = 0;
    return sendBufferedData();
}

KMError H2Connection::Impl::sendHeadersFrame(HeadersFrame *frame)
{
    h2_priority_t pri;
    frame->setPriority(pri);
    size_t len1 = H2_FRAME_HEADER_SIZE + (frame->hasPriority()?H2_PRIORITY_PAYLOAD_SIZE:0);
    auto &headers = frame->getHeaders();
    size_t hdrSize = frame->getHeadersSize();
    send_buffer_.resize(len1 + hdrSize * 3 / 2);
    int ret = hpEncoder_.encode(headers, &send_buffer_[0] + len1, send_buffer_.size() - len1);
    if (ret < 0) {
        return KMError::FAILED;
    }
    size_t bsize = ret;
    ret = frame->encode(&send_buffer_[0], len1, bsize);
    KUMA_ASSERT(ret == len1);
    size_t total_len = len1 + bsize;
    send_buffer_.resize(total_len);
    send_offset_ = 0;
    return sendBufferedData();
}

H2StreamPtr H2Connection::Impl::createStream()
{
    H2StreamPtr stream(new H2Stream(nextStreamId_, this, initLocalWindowSize_, initRemoteWindowSize_));
    nextStreamId_ += 2;
    addStream(stream);
    return stream;
}

H2StreamPtr H2Connection::Impl::createStream(uint32_t streamId)
{
    H2StreamPtr stream(new H2Stream(streamId, this, initLocalWindowSize_, initRemoteWindowSize_));
    addStream(stream);
    return stream;
}

void H2Connection::Impl::handleDataFrame(DataFrame *frame)
{
    //KUMA_INFOXTRACE("handleDataFrame, streamId="<<frame->getStreamId()<<", size="<<frame->size()<<", flags="<<int(frame->getFlags()));
    H2StreamPtr stream = getStream(frame->getStreamId());
    if (stream) {
        flow_ctrl_.notifyBytesReceived(frame->getPayloadLength());
        stream->handleDataFrame(frame);
    } else {
        KUMA_WARNXTRACE("handleDataFrame, cannot find stream, streamId="<<frame->getStreamId()<<", size="<<frame->size()<<", flags="<<int(frame->getFlags()));
    }
}

void H2Connection::Impl::handleHeadersFrame(HeadersFrame *frame)
{
    KUMA_INFOXTRACE("handleHeadersFrame, streamId="<<frame->getStreamId()<<", flags="<<int(frame->getFlags()));
    H2StreamPtr stream = getStream(frame->getStreamId());
    if (!stream && !isServer()) {
        KUMA_WARNXTRACE("handleHeadersFrame, cannot find local stream or promised stream, streamId="<<frame->getStreamId());
        return; // client: no local steram or promised stream
    }
    
    HeaderVector headers;
    if (hpDecoder_.decode(frame->getBlock(), frame->getBlockSize(), headers) < 0) {
        KUMA_ERRXTRACE("handleHeadersFrame, hpack decode failed");
        return;
    }
    
    if (!stream) {
        // new stream arrived on server side
        stream = createStream(frame->getStreamId());
        if (accept_cb_ && !accept_cb_(frame->getStreamId())) {
            removeStream(frame->getStreamId());
            return;
        }
        lastStreamId_ = frame->getStreamId();
    }
    frame->setHeaders(std::move(headers), 0);
    stream->handleHeadersFrame(frame);
}

void H2Connection::Impl::handlePriorityFrame(PriorityFrame *frame)
{
    KUMA_INFOXTRACE("handlePriorityFrame, streamId="<<frame->getStreamId()<<", dep="<<frame->getPriority().streamId<<", weight="<<frame->getPriority().weight);
}

void H2Connection::Impl::handleRSTStreamFrame(RSTStreamFrame *frame)
{
    KUMA_INFOXTRACE("handleRSTStreamFrame, streamId="<<frame->getStreamId()<<", err="<<frame->getErrorCode());
    if (frame->getStreamId() == 0) {
        connectionError(H2Error::PROTOCOL_ERROR);
        return;
    }
    H2StreamPtr stream = getStream(frame->getStreamId());
    if (stream) {
        stream->handleRSTStreamFrame(frame);
    }
}

void H2Connection::Impl::handleSettingsFrame(SettingsFrame *frame)
{
    KUMA_INFOXTRACE("handleSettingsFrame, streamId="<<frame->getStreamId()<<", count="<<frame->getParams().size()<<", flags="<<int(frame->getFlags()));
    if (frame->isAck()) {
        return;
    } else { // send setings ack
        SettingsFrame settings;
        settings.setStreamId(frame->getStreamId());
        settings.setAck(true);
        sendH2Frame(&settings);
    }
    if (frame->getStreamId() == 0) {
        applySettings(frame->getParams());
        if (!isServer() && getState() < State::OPEN) { // first frame from server must be settings
            prefaceReceived_ = true;
            if (getState() == State::HANDSHAKE && sendBufferEmpty()) {
                onStateOpen();
            }
        }
    } else {
        // PROTOCOL_ERROR on connection
        // SETTINGS frames always apply to a connection, never a single stream
        connectionError(H2Error::PROTOCOL_ERROR);
    }
}

void H2Connection::Impl::handlePushFrame(PushPromiseFrame *frame)
{
    KUMA_INFOXTRACE("handlePushFrame, streamId="<<frame->getStreamId()<<", promStreamId="<<frame->getPromisedStreamId()<<", bsize="<<frame->getBlockSize()<<", flags="<<int(frame->getFlags()));

    if (!isPromisedStream(frame->getPromisedStreamId())) {
        KUMA_ERRXTRACE("handlePushFrame, invalid stream id");
        return;
    }
    if (frame->getBlockSize() > 0) {
        HeaderVector headers;
        if (hpDecoder_.decode(frame->getBlock(), frame->getBlockSize(), headers) < 0) {
            KUMA_ERRXTRACE("handlePushFrame, hpack decode failed");
            return;
        }
    }
    H2StreamPtr stream(new H2Stream(frame->getPromisedStreamId(), this, initLocalWindowSize_, initLocalWindowSize_));
    addStream(stream);
    stream->handlePushFrame(frame);
}

void H2Connection::Impl::handlePingFrame(PingFrame *frame)
{
    KUMA_INFOXTRACE("handlePingFrame, streamId="<<frame->getStreamId());
    if (!frame->isAck()) {
        PingFrame pingFrame;
        pingFrame.setStreamId(0);
        pingFrame.setAck(true);
        pingFrame.setData(frame->getData(), H2_PING_PAYLOAD_SIZE);
        sendH2Frame(&pingFrame);
    }
}

void H2Connection::Impl::handleGoawayFrame(GoawayFrame *frame)
{
    KUMA_INFOXTRACE("handleGoawayFrame, streamId="<<frame->getLastStreamId()<<", err="<<frame->getErrorCode());
    if (registeredToLoop_) {
        registeredToLoop_ = false;
        tcp_.getEventLoop()->removeListener(this);
    }
    tcp_.close();
    auto streams = std::move(streams_);
    for (auto it : streams) {
        it.second->onError(frame->getErrorCode());
    }
    streams = std::move(promisedStreams_);
    for (auto it : streams) {
        it.second->onError(frame->getErrorCode());
    }
    auto error_cb = std::move(error_cb_);
    if (!key_.empty()) {
        auto &connMgr = H2ConnectionMgr::getRequestConnMgr(sslEnabled());
        std::string key(std::move(key_));
        // will destroy self when calling from loop stop
        connMgr.removeConnection(key);
    }
    if (error_cb) {
        error_cb(frame->getErrorCode());
    }
}

void H2Connection::Impl::handleWindowUpdateFrame(WindowUpdateFrame *frame)
{
    if (flow_ctrl_.getRemoteWindowSize() + frame->getWindowSizeIncrement() > H2_MAX_WINDOW_SIZE) {
        if (frame->getStreamId() == 0) {
            connectionError(H2Error::FLOW_CONTROL_ERROR);
        } else {
            streamError(frame->getStreamId(), H2Error::FLOW_CONTROL_ERROR);
        }
        return;
    }
    if (frame->getStreamId() == 0) {
        KUMA_INFOXTRACE("handleWindowUpdateFrame, streamId="<<frame->getStreamId()<<", size=" << frame->getWindowSizeIncrement()<<", old="<<flow_ctrl_.getRemoteWindowSize());
        if (frame->getWindowSizeIncrement() == 0) {
            connectionError(H2Error::PROTOCOL_ERROR);
            return;
        }
        bool need_notify = !blocked_streams_.empty();
        flow_ctrl_.increaseRemoteWindowSize(frame->getWindowSizeIncrement());
        if (need_notify) {
            notifyBlockedStreams();
        }
    } else {
        //flow_ctrl_.increaseRemoteWindowSize(frame->getWindowSizeIncrement());
        H2StreamPtr stream = getStream(frame->getStreamId());
        if (!stream && isServer()) {
            // new stream arrived on server side
            stream = createStream(frame->getStreamId());
            if (accept_cb_ && !accept_cb_(frame->getStreamId())) {
                removeStream(frame->getStreamId());
                return;
            }
            lastStreamId_ = frame->getStreamId();
        }
        stream->handleWindowUpdateFrame(frame);
    }
}

void H2Connection::Impl::handleContinuationFrame(ContinuationFrame *frame)
{
    KUMA_INFOXTRACE("handleContinuationFrame, streamId="<<frame->getStreamId()<<", flags="<<int(frame->getFlags()));
    H2StreamPtr stream = getStream(frame->getStreamId());
    if (stream) {
        HeaderVector headers;
        if (hpDecoder_.decode(frame->getBlock(), frame->getBlockSize(), headers) < 0) {
            KUMA_ERRXTRACE("handleHeadersFrame, hpack decode failed");
            return;
        }
        
        frame->setHeaders(std::move(headers), 0);
        stream->handleContinuationFrame(frame);
    }
}

KMError H2Connection::Impl::handleInputData(uint8_t *buf, size_t len)
{
    if (getState() == State::OPEN) {
        return parseInputData(buf, len);
    } else if (getState() == State::UPGRADING) {
        // H2 connection will be destroyed when invalid http request received
        DESTROY_DETECTOR_SETUP();
        int ret = httpParser_.parse((char*)buf, (uint32_t)len);
        DESTROY_DETECTOR_CHECK(KMError::DESTROYED);
        if (ret >= len) {
            return KMError::NOERR;
        }
        // residual data, should be preface
        len -= ret;
        buf += ret;
    }
    
    if (getState() == State::HANDSHAKE) {
        if (isServer()) {
            size_t cmpSize = std::min(cmpPreface_.size(), len);
            if (memcmp(cmpPreface_.c_str(), buf, cmpSize) != 0) {
                cleanup();
                setState(State::CLOSED);
                KUMA_ERRXTRACE("handleInputData, invalid protocol");
                return KMError::INVALID_PROTO;
            }
            cmpPreface_ = cmpPreface_.substr(cmpSize);
            if (!cmpPreface_.empty()) {
                return KMError::NOERR; // need more data
            }
            prefaceReceived_ = true;
            onStateOpen();
            return parseInputData(buf + cmpSize, len - cmpSize);
        } else {
            return parseInputData(buf, len);
        }
    } else {
        KUMA_WARNXTRACE("handleInputData, invalid state: "<<getState());
    }
    return KMError::NOERR;
}

KMError H2Connection::Impl::parseInputData(const uint8_t *buf, size_t len)
{
    DESTROY_DETECTOR_SETUP();
    auto parseState = frameParser_.parseInputData(buf, len);
    DESTROY_DETECTOR_CHECK(KMError::DESTROYED);
    if(getState() == State::IN_ERROR || getState() == State::CLOSED) {
        return KMError::INVALID_STATE;
    }
    if(parseState == FrameParser::ParseState::FAILURE) {
        cleanup();
        setState(State::CLOSED);
        return KMError::FAILED;
    }
    return KMError::NOERR;
}

void H2Connection::Impl::onFrame(H2Frame *frame)
{
    switch (frame->type()) {
        case H2FrameType::DATA:
            handleDataFrame(dynamic_cast<DataFrame*>(frame));
            break;
            
        case H2FrameType::HEADERS:
            handleHeadersFrame(dynamic_cast<HeadersFrame*>(frame));
            break;
            
        case H2FrameType::PRIORITY:
            handlePriorityFrame(dynamic_cast<PriorityFrame*>(frame));
            break;
            
        case H2FrameType::RST_STREAM:
            handleRSTStreamFrame(dynamic_cast<RSTStreamFrame*>(frame));
            break;
            
        case H2FrameType::SETTINGS:
            handleSettingsFrame(dynamic_cast<SettingsFrame*>(frame));
            break;
            
        case H2FrameType::PUSH_PROMISE:
            handlePushFrame(dynamic_cast<PushPromiseFrame*>(frame));
            break;
            
        case H2FrameType::PING:
            handlePingFrame(dynamic_cast<PingFrame*>(frame));
            break;
            
        case H2FrameType::GOAWAY:
            handleGoawayFrame(dynamic_cast<GoawayFrame*>(frame));
            break;
            
        case H2FrameType::WINDOW_UPDATE:
            handleWindowUpdateFrame(dynamic_cast<WindowUpdateFrame*>(frame));
            break;
            
        case H2FrameType::CONTINUATION:
            handleContinuationFrame(dynamic_cast<ContinuationFrame*>(frame));
            break;
            
        default:
            break;
    }
}

void H2Connection::Impl::onFrameError(const FrameHeader &hdr, H2Error err, bool stream)
{
    KUMA_ERRXTRACE("onFrameError, streamId="<<hdr.getStreamId()<<", type="<<hdr.getType()<<", err="<<int(err)<<", stream="<<stream);
    if (!stream) {
        connectionError(err);
    }
}

void H2Connection::Impl::addStream(H2StreamPtr stream)
{
    KUMA_INFOXTRACE("addStream, streamId="<<stream->getStreamId());
    if (isPromisedStream(stream->getStreamId())) {
        promisedStreams_[stream->getStreamId()] = stream;
    } else {
        streams_[stream->getStreamId()] = stream;
    }
}

H2StreamPtr H2Connection::Impl::getStream(uint32_t streamId)
{
    auto &streams = isPromisedStream(streamId) ? promisedStreams_ : streams_;
    auto it = streams.find(streamId);
    if (it != streams.end()) {
        return it->second;
    }
    return H2StreamPtr();
}

void H2Connection::Impl::removeStream(uint32_t streamId)
{
    KUMA_INFOXTRACE("removeStream, streamId="<<streamId);
    if (isPromisedStream(streamId)) {
        promisedStreams_.erase(streamId);
    } else {
        streams_.erase(streamId);
    }
}

void H2Connection::Impl::loopStopped()
{ // event loop exited
    registeredToLoop_ = false;
    cleanup();
}

std::string H2Connection::Impl::buildUpgradeRequest()
{
    ParamVector params;
    params.emplace_back(std::make_pair(INITIAL_WINDOW_SIZE, H2_INITIAL_WINDOW_SIZE));
    params.emplace_back(std::make_pair(MAX_FRAME_SIZE, 65536));
    uint8_t buf[2 * H2_SETTING_ITEM_SIZE];
    SettingsFrame settings;
    settings.encodePayload(buf, sizeof(buf), params);
    
    uint8_t x64_encode_buf[sizeof(buf) * 3 / 2] = {0};
    uint32_t x64_encode_len = x64_encode(buf, sizeof(buf), x64_encode_buf, sizeof(x64_encode_buf), false);
    std::string settings_str((char*)x64_encode_buf, x64_encode_len);
    
    std::stringstream ss;
    ss << "GET / HTTP/1.1\r\n";
    ss << "Host: " << host_ << "\r\n";
    ss << "Connection: Upgrade, HTTP2-Settings\r\n";
    ss << "Upgrade: h2c\r\n";
    ss << "HTTP2-Settings: " << settings_str << "\r\n";
    ss << "\r\n";
    return ss.str();
}

std::string H2Connection::Impl::buildUpgradeResponse()
{
    std::stringstream ss;
    ss << "HTTP/1.1 101 Switching Protocols\r\n";
    ss << "Connection: Upgrade\r\n";
    ss << "Upgrade: "<< httpParser_.getHeaderValue("Upgrade") <<"\r\n";
    ss << "\r\n";
    return ss.str();
}

void H2Connection::Impl::sendUpgradeRequest()
{
    std::string str(buildUpgradeRequest());
    send_buffer_.assign(str.begin(), str.end());
    send_offset_ = 0;
    setState(State::UPGRADING);
    sendBufferedData();
}

void H2Connection::Impl::sendUpgradeResponse()
{
    std::string str(buildUpgradeResponse());
    send_buffer_.assign(str.begin(), str.end());
    send_offset_ = 0;
    setState(State::UPGRADING);
    sendBufferedData();
    if (sendBufferEmpty()) {
        sendPreface(); // send server preface
    }
}

void H2Connection::Impl::sendPreface()
{
    setState(State::HANDSHAKE);
    ParamVector params;
    params.emplace_back(std::make_pair(INITIAL_WINDOW_SIZE, initLocalWindowSize_));
    params.emplace_back(std::make_pair(MAX_FRAME_SIZE, 65536));
    size_t setting_size = H2_FRAME_HEADER_SIZE + params.size() * H2_SETTING_ITEM_SIZE;
    size_t encoded_len = 0;
    if (!isServer()) {
        size_t total_len = ClientConnectionPreface.size() + setting_size + H2_WINDOW_UPDATE_FRAME_SIZE;
        send_buffer_.resize(total_len);
        memcpy(&send_buffer_[0], ClientConnectionPreface.c_str(), ClientConnectionPreface.size());
        encoded_len += ClientConnectionPreface.size();
    } else {
        params.emplace_back(std::make_pair(MAX_CONCURRENT_STREAMS, 128));
        setting_size += H2_SETTING_ITEM_SIZE;
        size_t total_len = setting_size + H2_WINDOW_UPDATE_FRAME_SIZE;
        send_buffer_.resize(total_len);
    }
    SettingsFrame settings;
    settings.setStreamId(0);
    settings.setParams(std::move(params));
    int ret = settings.encode(&send_buffer_[0] + encoded_len, send_buffer_.size() - encoded_len);
    if (ret < 0) {
        KUMA_ERRXTRACE("sendPreface, failed to encode setting frame");
        return;
    }
    encoded_len += ret;
    WindowUpdateFrame win_update;
    win_update.setStreamId(0);
    win_update.setWindowSizeIncrement(flow_ctrl_.getLocalWindowSize());
    win_update.encode(&send_buffer_[0] + encoded_len, send_buffer_.size() - encoded_len);
    send_offset_ = 0;
    sendBufferedData();
    if (sendBufferEmpty() && prefaceReceived_) {
        onStateOpen();
    }
}

void H2Connection::Impl::onConnect(KMError err)
{
    if(err != KMError::NOERR) {
        onConnectError(err);
        return ;
    }
    if (sslEnabled()) {
        sendPreface();
        return ;
    }
    nextStreamId_ += 2; // stream id 1 is for upgrade request
    sendUpgradeRequest();
}

void H2Connection::Impl::onWrite()
{// send_buffer_ must be empty
    if(isServer() && getState() == State::UPGRADING) {
        // upgrade response is sent out, waiting for client preface
        setState(State::HANDSHAKE);
        sendPreface();
    } else if (getState() == State::HANDSHAKE && prefaceReceived_) {
        onStateOpen();
    }
    if (getState() == State::OPEN) {
        notifyBlockedStreams();
    }
}

void H2Connection::Impl::onError(KMError err)
{
    KUMA_INFOXTRACE("onError, err="<<int(err));
    cleanup();
    if (error_cb_) {
        error_cb_(int(err));
    }
}

void H2Connection::Impl::onConnectError(KMError err)
{
    cleanup();
    setState(State::IN_ERROR);
    auto connect_cb(std::move(connect_cb_));
    if (connect_cb) connect_cb(err);
}

void H2Connection::Impl::notifyBlockedStreams()
{
    while (!blocked_streams_.empty() && sendBufferEmpty()) {
        uint32_t streamId = blocked_streams_.back();
        blocked_streams_.pop_back();
        auto stream = getStream(streamId);
        if (stream) {
            stream->onWrite();
        }
    }
}

void H2Connection::Impl::sendWindowUpdate(uint32_t streamId, uint32_t windowSize)
{
    WindowUpdateFrame frame;
    frame.setStreamId(streamId);
    frame.setWindowSizeIncrement(windowSize);
    sendH2Frame(&frame);
}

bool H2Connection::Impl::isControlFrame(H2Frame *frame)
{
    return frame->type() != H2FrameType::DATA;
}

void H2Connection::Impl::applySettings(const ParamVector &params)
{
    for (auto &kv : params) {
        KUMA_INFOXTRACE("applySettings, id="<<kv.first<<", value="<<kv.second);
        switch (kv.first) {
            case HEADER_TABLE_SIZE:
                hpDecoder_.setMaxTableSize(kv.second);
                break;
            case INITIAL_WINDOW_SIZE:
                initRemoteWindowSize_ = kv.second;
                break;
            case MAX_FRAME_SIZE:
                remoteFrameSize_ = kv.second;
                break;
        }
    }
}

void H2Connection::Impl::sendGoaway(H2Error err)
{
    KUMA_INFOXTRACE("sendGoaway, err="<<int(err)<<", last="<<lastStreamId_);
    GoawayFrame frame;
    frame.setErrorCode(uint32_t(err));
    frame.setStreamId(0);
    frame.setLastStreamId(lastStreamId_);
    sendH2Frame(&frame);
}

void H2Connection::Impl::connectionError(H2Error err)
{
    sendGoaway(err);
    setState(State::CLOSED);
    if (error_cb_) {
        error_cb_(int(err));
    }
}

void H2Connection::Impl::streamError(uint32_t streamId, H2Error err)
{
    RSTStreamFrame frame;
    frame.setStreamId(streamId);
    frame.setErrorCode(uint32_t(err));
}

void H2Connection::Impl::onHttpData(const char* data, size_t len)
{
    KUMA_ERRXTRACE("onHttpData, len="<<len);
}

void H2Connection::Impl::onHttpEvent(HttpEvent ev)
{
    KUMA_INFOXTRACE("onHttpEvent, ev="<<int(ev));
    switch (ev) {
        case HttpEvent::HEADER_COMPLETE:
            if(101 != httpParser_.getStatusCode() ||
               !is_equal(httpParser_.getHeaderValue("Connection"), "Upgrade")) {
                KUMA_ERRXTRACE("onHttpEvent, not HTTP2 upgrade response");
            }
            break;
            
        case HttpEvent::COMPLETE:
            if(httpParser_.isRequest()) {
                handleUpgradeRequest();
            } else {
                handleUpgradeResponse();
            }
            break;
            
        case HttpEvent::HTTP_ERROR:
            setState(State::IN_ERROR);
            onConnectError(KMError::FAILED);
            break;
    }
}

KMError H2Connection::Impl::handleUpgradeRequest()
{
    bool hasUpgrade = false;
    bool hasH2Settings = false;
    std::stringstream ss(httpParser_.getHeaderValue("Connection"));
    std::string item;
    while (getline(ss, item, ',')) {
        trim_left(item);
        trim_right(item);
        if (item == "Upgrade") {
            hasUpgrade = true;
        } else if (item == "HTTP2-Settings") {
            hasH2Settings = true;
        }
    }

    if(!hasUpgrade || !hasH2Settings  ||
       !is_equal(httpParser_.getHeaderValue("Upgrade"), "h2c")) {
        setState(State::IN_ERROR);
        KUMA_ERRXTRACE("handleRequest, not HTTP2 request");
        return KMError::INVALID_PROTO;
    }
    sendUpgradeResponse();
    
    return KMError::NOERR;
}

KMError H2Connection::Impl::handleUpgradeResponse()
{
    if(101 == httpParser_.getStatusCode() &&
       is_equal(httpParser_.getHeaderValue("Connection"), "Upgrade")) {
        sendPreface();
        return KMError::NOERR;
    } else {
        setState(State::IN_ERROR);
        KUMA_INFOXTRACE("handleResponse, invalid status code: "<<httpParser_.getStatusCode());
        onConnectError(KMError::INVALID_PROTO);
        return KMError::INVALID_PROTO;
    }
}

void H2Connection::Impl::onStateOpen()
{
    KUMA_INFOXTRACE("onStateOpen");
    setState(State::OPEN);
    if (!isServer() && connect_cb_) {
        auto connect_cb(std::move(connect_cb_));
        connect_cb(KMError::NOERR);
    }
}
