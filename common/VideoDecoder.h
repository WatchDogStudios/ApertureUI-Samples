// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT
//
// VideoDecoder.h - Media Foundation H.264 video decoder for the samples.

#pragma once

#include <APHTML/Interfaces/APCVideoDecoder.h>

#include <deque>
#include <vector>
#include <wrl/client.h>

struct IMFTransform;

namespace apui::samples
{
  /// @brief IAPCVideoDecoder implementation using Windows Media Foundation.
  /// Uses the platform H.264 MFT decoder, outputs YUV420P planes.
  /// Works with both D3D11 and Vulkan rendering backends.
  class VideoDecoder final : public aperture::core::IAPCVideoDecoder
  {
  public:
    VideoDecoder();
    ~VideoDecoder() override;

    bool SupportsCodec(aperture::core::EVideoCodec codec) const override;
    bool GetMaxResolution(aperture::core::EVideoCodec codec, uint32_t& outMaxWidth,
      uint32_t& outMaxHeight) const override;

    aperture::core::APCDecoderSession CreateSession(aperture::core::EVideoCodec codec,
      uint32_t width, uint32_t height, const uint8_t* extradata = nullptr,
      size_t extradataSize = 0) override;
    void DestroySession(aperture::core::APCDecoderSession session) override;

    aperture::core::EDecoderResult SubmitPacket(aperture::core::APCDecoderSession session,
      const uint8_t* data, size_t size, double pts, bool isKeyframe) override;
    aperture::core::EDecoderResult GetDecodedFrame(
      aperture::core::APCDecoderSession session,
      aperture::core::APCDecodedFrame& outFrame) override;
    aperture::core::EDecoderResult Flush(
      aperture::core::APCDecoderSession session) override;
    void Reset(aperture::core::APCDecoderSession session) override;
    uint32_t GetBufferedFrameCount(
      aperture::core::APCDecoderSession session) const override;

  private:
    struct DecodedFrameData
    {
      std::vector<uint8_t> yPlane;
      std::vector<uint8_t> uPlane;
      std::vector<uint8_t> vPlane;
      uint32_t width = 0;
      uint32_t height = 0;
      uint32_t yStride = 0;
      uint32_t uvStride = 0;
      double pts = 0.0;
      uint64_t frameIndex = 0;
    };

    struct Session
    {
      Microsoft::WRL::ComPtr<IMFTransform> mft; ///< released when the session is deleted
      uint32_t width = 0;
      uint32_t height = 0;
      uint32_t outputStride = 0;
      std::deque<DecodedFrameData> decodedFrames;
      DecodedFrameData currentFrame;
      uint64_t nextFrameIndex = 0;
      bool streamStarted = false;
      bool draining = false;
    };

    bool m_mfInitialized = false;

    aperture::core::EDecoderResult DrainOutput(Session* s);
    bool ConvertNV12ToI420(const uint8_t* nv12Data, size_t nv12Bytes, uint32_t width,
      uint32_t height, uint32_t nv12Stride, double pts, uint64_t frameIndex,
      DecodedFrameData& outFrame);
  };
} // namespace apui::samples
