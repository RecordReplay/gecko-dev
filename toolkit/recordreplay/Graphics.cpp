/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Interfaces for drawing graphics to an in process buffer when
// recording/replaying.

#include "Graphics.h"

#include "ProcessRecordReplay.h"
#include "mozilla/Base64.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/layers/TextureHost.h"
#include "imgIEncoder.h"
#include "nsComponentManagerUtils.h"
#include "nsPrintfCString.h"

using namespace mozilla::layers;

namespace mozilla { extern void RecordReplayTickRefreshDriver(); }

namespace mozilla::recordreplay {

static void (*gOnPaint)();
static bool (*gSetPaintCallback)(char* (*aCallback)(const char* aMimeType, int aJPEGQuality));

static char* PaintCallback(const char* aMimeType, int aJPEGQuality);

void InitializeGraphics() {
  LoadSymbol("RecordReplayOnPaint", gOnPaint);
  LoadSymbol("RecordReplaySetPaintCallback", gSetPaintCallback);

  gSetPaintCallback(PaintCallback);
}

static CompositorBridgeParent* gCompositorBridge;

// Directory to write paints to when recording, for use in debugging.
static const char* gPaintsDirectory;

static TimeStamp gCompositeTime;

TimeStamp CompositeTime() {
  return gCompositeTime;
}

static void MaybeCreatePaintFile();

void OnPaint() {
  if (!HasCheckpoint() || HasDivergedFromRecording()) {
    return;
  }

  gCompositeTime = TimeStamp::Now();
  recordreplay::RecordReplayBytes("CompositeTime", &gCompositeTime, sizeof(gCompositeTime));

  MaybeCreatePaintFile();

  gOnPaint();
}

// Format to use for graphics data.
static const gfx::SurfaceFormat SurfaceFormat = gfx::SurfaceFormat::R8G8B8X8;

// Buffer for the draw target used for main thread compositing.
static void* gDrawTargetBuffer;
static size_t gDrawTargetBufferSize;

// Dimensions of the last paint which the compositor performed.
static size_t gPaintWidth, gPaintHeight;

// Whether the draw target has been fetched while compositing.
static bool gFetchedDrawTarget;

already_AddRefed<gfx::DrawTarget> DrawTargetForRemoteDrawing(const gfx::IntRect& aSize) {
  MOZ_RELEASE_ASSERT(NS_IsMainThread());

  if (aSize.IsEmpty()) {
    return nullptr;
  }

  gPaintWidth = aSize.width;
  gPaintHeight = aSize.height;

  gfx::IntSize size(aSize.width, aSize.height);
  size_t bufferSize = ImageDataSerializer::ComputeRGBBufferSize(size, SurfaceFormat);

  if (bufferSize != gDrawTargetBufferSize) {
    // Diagnostics for https://github.com/RecordReplay/backend/issues/3145
    if (HasDivergedFromRecording()) {
      PrintLog("Diverged UPDATE_BUFFER %zu", bufferSize);
    }
    free(gDrawTargetBuffer);
    gDrawTargetBuffer = malloc(bufferSize);
    gDrawTargetBufferSize = bufferSize;
  }

  size_t stride = ImageDataSerializer::ComputeRGBStride(SurfaceFormat, aSize.width);
  RefPtr<gfx::DrawTarget> drawTarget = gfx::Factory::CreateDrawTargetForData(
      gfx::BackendType::SKIA, (uint8_t*)gDrawTargetBuffer, size, stride,
      SurfaceFormat,
      /* aUninitialized = */ true);
  MOZ_RELEASE_ASSERT(drawTarget);

  gFetchedDrawTarget = true;
  return drawTarget.forget();
}

struct TextureInfo {
  uint8_t* mBuffer;
  BufferDescriptor mDesc;
  TextureFlags mFlags;
};

static std::unordered_map<PTextureChild*, TextureInfo> gTextureInfo;
static StaticMutex gTextureInfoMutex;

void RegisterTextureChild(PTextureChild* aChild, TextureData* aData,
                          const SurfaceDescriptor& aDesc,
                          TextureFlags aFlags) {
  if (aDesc.type() != SurfaceDescriptor::TSurfaceDescriptorBuffer) {
    PrintLog("RegisterTextureChild %p unknown descriptor type %d", aChild, aDesc.type());
    return;
  }

  const SurfaceDescriptorBuffer& buf = aDesc.get_SurfaceDescriptorBuffer();
  MOZ_RELEASE_ASSERT(buf.data().type() == MemoryOrShmem::TShmem);
  uint8_t* buffer = static_cast<BufferTextureData*>(aData)->GetBuffer();

  TextureInfo info = {
    buffer,
    buf.desc(),
    aFlags
  };

  StaticMutexAutoLock lock(gTextureInfoMutex);
  gTextureInfo[aChild] = info;
}

TextureHost* CreateTextureHost(PTextureChild* aChild) {
  if (!aChild) {
    return nullptr;
  }

  StaticMutexAutoLock lock(gTextureInfoMutex);

  auto iter = gTextureInfo.find(aChild);
  if (iter == gTextureInfo.end()) {
    PrintLog("Error: CreateTextureHost unknown TextureChild %p, crashing...", aChild);
    MOZ_CRASH("CreateTextureHost");
  }
  const TextureInfo& info = iter->second;
  MemoryTextureHost* rv = new MemoryTextureHost(info.mBuffer, info.mDesc, info.mFlags);

  // Leak the result so it doesn't get deleted later. We aren't respecting
  // ownership rules by giving this MemoryTextureHost an internal pointer to
  // a shmem.
  new RefPtr(rv);

  return rv;
}

// Encode the contents of gDrawTargetBuffer as a base64 image.
static char* EncodeGraphicsAsBase64(const char* aMimeType, int aJPEGQuality) {
  // Get an image encoder for the media type.
  nsPrintfCString encoderCID("@mozilla.org/image/encoder;2?type=%s",
                             nsCString(aMimeType).get());
  nsCOMPtr<imgIEncoder> encoder = do_CreateInstance(encoderCID.get());

  size_t stride = layers::ImageDataSerializer::ComputeRGBStride(SurfaceFormat,
                                                                gPaintWidth);

  nsCString options8;
  if (!strcmp(aMimeType, "image/jpeg")) {
    options8 = nsPrintfCString("quality=%d", aJPEGQuality);
  }

  nsString options = NS_ConvertUTF8toUTF16(options8);
  nsresult rv = encoder->InitFromData(
      (const uint8_t*)gDrawTargetBuffer, stride * gPaintHeight, gPaintWidth,
      gPaintHeight, stride, imgIEncoder::INPUT_FORMAT_RGBA, options);
  if (NS_FAILED(rv)) {
    PrintLog("Error: encoder->InitFromData() failed");
    return nullptr;
  }

  uint64_t count;
  rv = encoder->Available(&count);
  if (NS_FAILED(rv)) {
    PrintLog("Error: encoder->Available() failed");
    return nullptr;
  }

  nsCString data;
  rv = Base64EncodeInputStream(encoder, data, count);
  if (NS_FAILED(rv)) {
    PrintLog("Error: Base64EncodeInputStream() failed");
    return nullptr;
  }

  return strdup(data.get());
}

static char* PaintCallback(const char* aMimeType, int aJPEGQuality) {
  if (!gCompositorBridge) {
    return nullptr;
  }

  // When diverged from the recording we need to generate graphics reflecting
  // the current DOM. Tick the refresh drivers to update layers to reflect
  // that current state.
  if (recordreplay::HasDivergedFromRecording()) {
    RecordReplayTickRefreshDriver();
  }

  MOZ_RELEASE_ASSERT(!gFetchedDrawTarget);

  AutoDisallowThreadEvents disallow;
  //gCompositorBridge->CompositeToTarget(VsyncId(), nullptr, nullptr);

  if (!gFetchedDrawTarget && !recordreplay::HasDivergedFromRecording()) {
    return nullptr;
  }
  gFetchedDrawTarget = false;

  return EncodeGraphicsAsBase64(aMimeType, aJPEGQuality);
}

// Write a JPEG file from a base64 encoded image.
static void WriteJPEGFromBase64(const char* aPath, const char* aBuf) {
  FILE* f = fopen(aPath, "w");
  if (!f) {
    fprintf(stderr, "Opening paint file %s failed, crashing.\n", aPath);
    MOZ_CRASH("WriteJPEGFromBase64");
  }

  nsAutoCString jpegBuf;
  nsresult rv = Base64Decode(nsCString(aBuf), jpegBuf);
  if (NS_FAILED(rv)) {
    MOZ_CRASH("WriteJPEGFromBase64 Base64Decode failed");
  }

  size_t count = fwrite(jpegBuf.get(), 1, jpegBuf.Length(), f);
  if (count != jpegBuf.Length()) {
    MOZ_CRASH("WriteJPEGFromBase64 incomplete write");
  }

  fclose(f);
}

static size_t gPaintIndex = 0;
static size_t gPaintSubindex = 0;
static bool gCreatingPaintFile;

static void MaybeCreatePaintFile() {
  if (!IsRecording() || !gPaintsDirectory) {
    return;
  }

  AutoPassThroughThreadEvents pt;

  ++gPaintIndex;
  gPaintSubindex = 0;

  gCreatingPaintFile = true;
  char* buf = PaintCallback("image/jpeg", 50);
  gCreatingPaintFile = false;

  if (!buf) {
    return;
  }

  recordreplay::PrintLog("CreatePaintFile %lu", gPaintIndex);

  nsPrintfCString path("%s/paint-%lu.jpg", gPaintsDirectory, gPaintIndex);
  WriteJPEGFromBase64(path.get(), buf);

  free(buf);
}

// This method is helpful in tracking down rendering problems.
// See https://github.com/RecordReplay/gecko-dev/issues/292
void MaybeCreateCurrentPaintFile(const char* why) {
  if (!gCreatingPaintFile) {
    return;
  }

  AutoPassThroughThreadEvents pt;

  ++gPaintSubindex;

  char* buf = EncodeGraphicsAsBase64("image/jpeg", 50);
  if (!buf) {
    return;
  }

  recordreplay::PrintLog("CreateCurrentPaintFile %lu %lu %s", gPaintIndex, gPaintSubindex, why);

  nsPrintfCString path("%s/paint-%lu-%lu-%s.jpg", gPaintsDirectory, gPaintIndex, gPaintSubindex, why);
  WriteJPEGFromBase64(path.get(), buf);

  free(buf);
}

} // namespace mozilla::recordreplay
