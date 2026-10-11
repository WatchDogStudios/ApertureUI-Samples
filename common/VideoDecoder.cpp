// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT
//
// VideoDecoder.cpp - Media Foundation H.264 decoder.

#include "VideoDecoder.h"

#include <Foundation/Logging/Log.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>

#include <cstring>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

using namespace aperture::core;
using Microsoft::WRL::ComPtr;

namespace apui::samples
{
  VideoDecoder::VideoDecoder()
  {
    HRESULT hr = MFStartup(MF_VERSION);
    m_mfInitialized = SUCCEEDED(hr);
    if (!m_mfInitialized)
      nsLog::Error("VideoDecoder: MFStartup failed: 0x{0:X}", (unsigned)hr);
    else
      nsLog::Info("VideoDecoder: Media Foundation initialized");
  }

  VideoDecoder::~VideoDecoder()
  {
    if (m_mfInitialized)
      MFShutdown();
  }

  bool VideoDecoder::SupportsCodec(EVideoCodec codec) const
  {
    return codec == EVideoCodec::H264 && m_mfInitialized;
  }

  bool VideoDecoder::GetMaxResolution(
    EVideoCodec codec, uint32_t& outMaxWidth, uint32_t& outMaxHeight) const
  {
    if (codec != EVideoCodec::H264)
      return false;
    outMaxWidth = 4096;
    outMaxHeight = 4096;
    return true;
  }

  APCDecoderSession VideoDecoder::CreateSession(EVideoCodec codec, uint32_t width,
    uint32_t height, const uint8_t* extradata, size_t extradataSize)
  {
    if (codec != EVideoCodec::H264 || !m_mfInitialized)
      return nullptr;

    auto* session = new Session();
    session->width = width;
    session->height = height;

    // Find the H.264 decoder MFT
    MFT_REGISTER_TYPE_INFO inputType = { };
    inputType.guidMajorType = MFMediaType_Video;
    inputType.guidSubtype = MFVideoFormat_H264;

    IMFActivate** ppActivate = nullptr;
    UINT32 numActivate = 0;
    // Synchronous MFTs only: this decoder drives the MFT with blocking ProcessInput /
    // ProcessOutput calls, which an asynchronous (typically hardware) MFT does not accept
    // until it is unlocked and fed through its event queue.
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
      MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &inputType, nullptr,
      &ppActivate, &numActivate);

    if (FAILED(hr) || numActivate == 0)
    {
      nsLog::Error("VideoDecoder: No H.264 decoder MFT found");
      CoTaskMemFree(ppActivate);
      delete session;
      return nullptr;
    }

    hr = ppActivate[0]->ActivateObject(IID_PPV_ARGS(&session->mft));
    for (UINT32 i = 0; i < numActivate; i++)
      ppActivate[i]->Release();
    CoTaskMemFree(ppActivate);

    if (FAILED(hr) || !session->mft)
    {
      nsLog::Error("VideoDecoder: Failed to activate H.264 MFT");
      delete session;
      return nullptr;
    }

    // Set input media type
    ComPtr<IMFMediaType> inputMediaType;
    hr = MFCreateMediaType(&inputMediaType);
    if (FAILED(hr))
    {
      delete session;
      return nullptr;
    }

    inputMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inputMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(inputMediaType.Get(), MF_MT_FRAME_SIZE, width, height);

    if (extradata && extradataSize > 0)
    {
      inputMediaType->SetBlob(
        MF_MT_MPEG_SEQUENCE_HEADER, extradata, static_cast<UINT32>(extradataSize));
    }

    hr = session->mft->SetInputType(0, inputMediaType.Get(), 0);
    if (FAILED(hr))
    {
      nsLog::Error("VideoDecoder: SetInputType failed: 0x{0:X}", (unsigned)hr);
      delete session;
      return nullptr;
    }

    // Set output type - prefer NV12
    bool outputSet = false;
    for (DWORD i = 0;; i++)
    {
      ComPtr<IMFMediaType> outputMediaType;
      hr = session->mft->GetOutputAvailableType(0, i, &outputMediaType);
      if (FAILED(hr))
        break;

      GUID subtype;
      outputMediaType->GetGUID(MF_MT_SUBTYPE, &subtype);
      if (subtype == MFVideoFormat_NV12)
      {
        hr = session->mft->SetOutputType(0, outputMediaType.Get(), 0);
        if (SUCCEEDED(hr))
        {
          // Get output stride
          UINT32 outStride = 0;
          hr = outputMediaType->GetUINT32(MF_MT_DEFAULT_STRIDE, &outStride);
          session->outputStride = SUCCEEDED(hr) && outStride > 0 ? outStride : width;
          outputSet = true;
          break;
        }
      }
    }

    if (!outputSet)
    {
      nsLog::Error("VideoDecoder: Failed to set NV12 output type");
      delete session;
      return nullptr;
    }

    // Start stream
    session->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    session->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    session->streamStarted = true;

    nsLog::Info("VideoDecoder: Created H.264 session {0}x{1}, stride={2}", width, height,
      session->outputStride);

    return static_cast<APCDecoderSession>(session);
  }

  void VideoDecoder::DestroySession(APCDecoderSession sessionHandle)
  {
    auto* s = static_cast<Session*>(sessionHandle);
    if (!s)
      return;

    if (s->mft)
    {
      s->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
      s->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }

    delete s;
  }

  EDecoderResult VideoDecoder::SubmitPacket(APCDecoderSession sessionHandle,
    const uint8_t* data, size_t size, double pts, bool isKeyframe)
  {
    (void)isKeyframe;
    auto* s = static_cast<Session*>(sessionHandle);
    if (!s || !s->mft)
      return EDecoderResult::DecoderNotInitialized;

    // Create input sample
    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buffer;

    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(size), &buffer);
    if (FAILED(hr))
      return EDecoderResult::InternalError;

    BYTE* bufPtr = nullptr;
    hr = buffer->Lock(&bufPtr, nullptr, nullptr);
    if (FAILED(hr))
      return EDecoderResult::InternalError;

    std::memcpy(bufPtr, data, size);
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(size));

    hr = MFCreateSample(&sample);
    if (FAILED(hr))
      return EDecoderResult::InternalError;

    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(static_cast<LONGLONG>(pts * 10000000.0));

    hr = s->mft->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING)
    {
      DrainOutput(s);
      hr = s->mft->ProcessInput(0, sample.Get(), 0);
    }

    if (FAILED(hr))
      return EDecoderResult::InternalError;

    DrainOutput(s);
    return EDecoderResult::Success;
  }

  EDecoderResult VideoDecoder::DrainOutput(Session* s)
  {
    if (!s || !s->mft)
      return EDecoderResult::DecoderNotInitialized;

    while (true)
    {
      MFT_OUTPUT_STREAM_INFO streamInfo = { };
      s->mft->GetOutputStreamInfo(0, &streamInfo);

      MFT_OUTPUT_DATA_BUFFER outputData = { };
      outputData.dwStreamID = 0;
      DWORD status = 0;

      ComPtr<IMFSample> outputSample;
      const bool mftProvidesSamples
        = (streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
      if (!mftProvidesSamples)
      {
        ComPtr<IMFMediaBuffer> outBuffer;
        DWORD bufSize = streamInfo.cbSize;
        if (bufSize == 0)
          bufSize = s->outputStride * s->height * 3 / 2;

        HRESULT hr = MFCreateMemoryBuffer(bufSize, &outBuffer);
        if (FAILED(hr))
          break;

        hr = MFCreateSample(&outputSample);
        if (FAILED(hr))
          break;

        outputSample->AddBuffer(outBuffer.Get());
        outputData.pSample = outputSample.Get();
      }

      HRESULT hr = s->mft->ProcessOutput(0, 1, &outputData, &status);

      // The caller owns what the MFT hands back: an event collection on any
      // call, and the sample when the MFT allocates it.
      if (outputData.pEvents)
        outputData.pEvents->Release();
      if (mftProvidesSamples && outputData.pSample)
        outputSample.Attach(outputData.pSample);

      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
        break;

      if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
      {
        // Re-negotiate output type
        for (DWORD i = 0;; i++)
        {
          ComPtr<IMFMediaType> newType;
          hr = s->mft->GetOutputAvailableType(0, i, &newType);
          if (FAILED(hr))
            break;

          GUID subtype;
          newType->GetGUID(MF_MT_SUBTYPE, &subtype);
          if (subtype == MFVideoFormat_NV12)
          {
            s->mft->SetOutputType(0, newType.Get(), 0);

            UINT32 newWidth = 0, newHeight = 0;
            MFGetAttributeSize(newType.Get(), MF_MT_FRAME_SIZE, &newWidth, &newHeight);
            if (newWidth > 0)
              s->width = newWidth;
            if (newHeight > 0)
              s->height = newHeight;

            UINT32 stride = 0;
            hr = newType->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride);
            s->outputStride = SUCCEEDED(hr) && stride > 0 ? stride : s->width;
            break;
          }
        }
        continue;
      }

      if (FAILED(hr))
        break;

      IMFSample* resultSample = outputSample.Get();
      if (!resultSample)
        break;

      LONGLONG sampleTime = 0;
      resultSample->GetSampleTime(&sampleTime);
      double framePts = static_cast<double>(sampleTime) / 10000000.0;

      ComPtr<IMFMediaBuffer> resultBuffer;
      resultSample->ConvertToContiguousBuffer(&resultBuffer);
      if (resultBuffer)
      {
        BYTE* rawData = nullptr;
        DWORD maxLen = 0, curLen = 0;
        hr = resultBuffer->Lock(&rawData, &maxLen, &curLen);
        if (SUCCEEDED(hr) && rawData)
        {
          DecodedFrameData frameData;
          if (ConvertNV12ToI420(rawData, curLen, s->width, s->height, s->outputStride,
                framePts, s->nextFrameIndex++, frameData))
          {
            s->decodedFrames.push_back(std::move(frameData));
          }
          resultBuffer->Unlock();
        }
      }
    }

    return s->decodedFrames.empty() ? EDecoderResult::NeedMoreData
                                    : EDecoderResult::Success;
  }

  bool VideoDecoder::ConvertNV12ToI420(const uint8_t* nv12Data, size_t nv12Bytes,
    uint32_t width, uint32_t height, uint32_t nv12Stride, double pts, uint64_t frameIndex,
    DecodedFrameData& outFrame)
  {
    if (!nv12Data || width == 0 || height == 0 || nv12Stride < width)
      return false;

    uint32_t uvWidth = width / 2;
    uint32_t uvHeight = height / 2;

    // A buffer shorter than the frame the output type describes (a stream
    // change the decoder has not reported yet) would be read past its end.
    const size_t needed = static_cast<size_t>(nv12Stride) * (height + uvHeight);
    if (nv12Bytes < needed)
      return false;

    outFrame.width = width;
    outFrame.height = height;
    outFrame.yStride = width;
    outFrame.uvStride = uvWidth;
    outFrame.pts = pts;
    outFrame.frameIndex = frameIndex;
    outFrame.yPlane.resize(static_cast<size_t>(width) * height);
    outFrame.uPlane.resize(static_cast<size_t>(uvWidth) * uvHeight);
    outFrame.vPlane.resize(static_cast<size_t>(uvWidth) * uvHeight);

    // Copy Y plane (stride-aware)
    for (uint32_t y = 0; y < height; y++)
    {
      std::memcpy(outFrame.yPlane.data() + static_cast<size_t>(y) * width,
        nv12Data + static_cast<size_t>(y) * nv12Stride, width);
    }

    // NV12 UV plane starts at nv12Stride * height
    const uint8_t* uvBase = nv12Data + static_cast<size_t>(nv12Stride) * height;

    // Deinterleave UV (stride-aware, NV12 UV stride == Y stride)
    for (uint32_t y = 0; y < uvHeight; y++)
    {
      const uint8_t* uvRow = uvBase + static_cast<size_t>(y) * nv12Stride;
      uint8_t* uRow = outFrame.uPlane.data() + static_cast<size_t>(y) * uvWidth;
      uint8_t* vRow = outFrame.vPlane.data() + static_cast<size_t>(y) * uvWidth;
      for (uint32_t x = 0; x < uvWidth; x++)
      {
        uRow[x] = uvRow[x * 2];
        vRow[x] = uvRow[x * 2 + 1];
      }
    }

    return true;
  }

  EDecoderResult VideoDecoder::GetDecodedFrame(
    APCDecoderSession sessionHandle, APCDecodedFrame& outFrame)
  {
    auto* s = static_cast<Session*>(sessionHandle);
    if (!s)
      return EDecoderResult::DecoderNotInitialized;

    DrainOutput(s);

    if (s->decodedFrames.empty())
      return EDecoderResult::NeedMoreData;

    // Move front frame into currentFrame to keep data alive
    s->currentFrame = std::move(s->decodedFrames.front());
    s->decodedFrames.pop_front();

    outFrame.yPlane = s->currentFrame.yPlane.data();
    outFrame.uPlane = s->currentFrame.uPlane.data();
    outFrame.vPlane = s->currentFrame.vPlane.data();
    outFrame.yStride = s->currentFrame.yStride;
    outFrame.uvStride = s->currentFrame.uvStride;
    outFrame.width = s->currentFrame.width;
    outFrame.height = s->currentFrame.height;
    outFrame.pts = s->currentFrame.pts;
    outFrame.frameIndex = s->currentFrame.frameIndex;

    return EDecoderResult::Success;
  }

  EDecoderResult VideoDecoder::Flush(APCDecoderSession sessionHandle)
  {
    auto* s = static_cast<Session*>(sessionHandle);
    if (!s || !s->mft)
      return EDecoderResult::DecoderNotInitialized;

    if (!s->draining)
    {
      s->mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
      s->draining = true;
    }

    DrainOutput(s);

    if (s->decodedFrames.empty())
    {
      s->draining = false;
      return EDecoderResult::EndOfStream;
    }

    return EDecoderResult::Success;
  }

  void VideoDecoder::Reset(APCDecoderSession sessionHandle)
  {
    auto* s = static_cast<Session*>(sessionHandle);
    if (!s || !s->mft)
      return;

    s->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    s->decodedFrames.clear();
    s->draining = false;
  }

  uint32_t VideoDecoder::GetBufferedFrameCount(APCDecoderSession sessionHandle) const
  {
    auto* s = static_cast<Session*>(sessionHandle);
    if (!s)
      return 0;
    return static_cast<uint32_t>(s->decodedFrames.size());
  }
} // namespace apui::samples
