#include "JpegToBmpConverter.h"

#include <HalStorage.h>
#include <JPEGDEC.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>
#include <new>

#include "BitmapHelpers.h"

// ============================================================================
// IMAGE PROCESSING OPTIONS
// ============================================================================
constexpr bool USE_8BIT_OUTPUT = false;
constexpr bool USE_ATKINSON = true;
constexpr bool USE_FLOYD_STEINBERG = false;
constexpr int TARGET_MAX_WIDTH = 480;
constexpr int TARGET_MAX_HEIGHT = 800;
// ============================================================================

namespace {
struct JpegDrawContext {
  int outWidth;
  int outHeight;
  uint32_t scaleX_fp;
  uint32_t scaleY_fp;
  bool oneBit;

  uint32_t* rowAccum;
  uint32_t* rowCount;
  int currentOutY;
  uint32_t nextOutY_srcStart;

  uint8_t* rowBuffer;
  int bytesPerRow;
  Print* bmpOut;

  AtkinsonDitherer* atkinsonDitherer;
  FloydSteinbergDitherer* fsDitherer;
  Atkinson1BitDitherer* atkinson1BitDitherer;

  // JPEGDEC internal scaling info
  int mcuWidth;
  int mcuHeight;
};

JpegDrawContext* g_ctx = nullptr;

int32_t jpegReadCallback(JPEGFILE* pFile, uint8_t* pBuf, int32_t iLen) {
  auto* file = static_cast<FsFile*>(pFile->fHandle);
  return file->read(pBuf, iLen);
}

int32_t jpegSeekCallback(JPEGFILE* pFile, int32_t iPos) {
  auto* file = static_cast<FsFile*>(pFile->fHandle);
  if (file->seek(iPos)) return iPos;
  return -1;
}

void emitOutputRow(JpegDrawContext* ctx) {
  memset(ctx->rowBuffer, 0, ctx->bytesPerRow);

  if (USE_8BIT_OUTPUT && !ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = (ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0;
      ctx->rowBuffer[x] = adjustPixel(gray);
    }
  } else if (ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = (ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0;
      const uint8_t bit = ctx->atkinson1BitDitherer ? ctx->atkinson1BitDitherer->processPixel(gray, x)
                                                    : quantize1bit(gray, x, ctx->currentOutY);
      const int byteIndex = x / 8;
      const int bitOffset = 7 - (x % 8);
      ctx->rowBuffer[byteIndex] |= (bit << bitOffset);
    }
    if (ctx->atkinson1BitDitherer) ctx->atkinson1BitDitherer->nextRow();
  } else {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = adjustPixel((ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0);
      uint8_t twoBit;
      if (ctx->atkinsonDitherer) {
        twoBit = ctx->atkinsonDitherer->processPixel(gray, x);
      } else if (ctx->fsDitherer) {
        twoBit = ctx->fsDitherer->processPixel(gray, x);
      } else {
        twoBit = quantize(gray, x, ctx->currentOutY);
      }
      const int byteIndex = (x * 2) / 8;
      const int bitOffset = 6 - ((x * 2) % 8);
      ctx->rowBuffer[byteIndex] |= (twoBit << bitOffset);
    }
    if (ctx->atkinsonDitherer)
      ctx->atkinsonDitherer->nextRow();
    else if (ctx->fsDitherer)
      ctx->fsDitherer->nextRow();
  }

  ctx->bmpOut->write(ctx->rowBuffer, ctx->bytesPerRow);
}

int jpegDrawCallback(JPEGDRAW* pDraw) {
  auto* ctx = g_ctx;
  if (!ctx) return 0;

  // pDraw coordinates are absolute within the scaled source image
  const int blockXStart = pDraw->x;
  const int blockXEnd = pDraw->x + pDraw->iWidth;

  for (int y = 0; y < pDraw->iHeight; y++) {
    const int srcY = pDraw->y + y;
    const uint16_t* srcRow = pDraw->pPixels + (y * pDraw->iWidth);

    // Map each output pixel X to the range of source pixels in this block
    for (int outX = 0; outX < ctx->outWidth; outX++) {
      const int srcXStart = (static_cast<uint32_t>(outX) * ctx->scaleX_fp) >> 16;
      const int srcXEnd = (static_cast<uint32_t>(outX + 1) * ctx->scaleX_fp) >> 16;

      // Find intersection of [srcXStart, srcXEnd) and [blockXStart, blockXEnd)
      const int intersectXStart = (srcXStart > blockXStart) ? srcXStart : blockXStart;
      const int intersectXEnd = (srcXEnd < blockXEnd) ? srcXEnd : blockXEnd;

      if (intersectXStart < intersectXEnd) {
        int sum = 0;
        int count = 0;
        for (int srcX = intersectXStart; srcX < intersectXEnd; srcX++) {
          const uint16_t rgb565 = srcRow[srcX - blockXStart];
          const uint8_t r = ((rgb565 >> 11) & 0x1F) << 3;
          const uint8_t g = ((rgb565 >> 5) & 0x3F) << 2;
          const uint8_t b = (rgb565 & 0x1F) << 3;
          sum += (r * 25 + g * 50 + b * 25) / 100;
          count++;
        }
        ctx->rowAccum[outX] += sum;
        ctx->rowCount[outX] += count;
      }
    }

    // Check if we have completed all source pixels for this set of rows
    // Since JPEGDEC decodes in MCUs, we can only emit a row once the right-most MCU is done
    const uint32_t srcY_fp = static_cast<uint32_t>(srcY + 1) << 16;
    if (blockXEnd >= pDraw->iWidth && srcY_fp >= ctx->nextOutY_srcStart) {
      // Note: This emit logic only works if MCUs are delivered in row-major order
      // and we are receiving full scanlines or the last MCU of a row.
      // For simplicity, we only emit if we've seen the end of the image width.
      // Actually, JPEGDEC delivered pDraw->x and iWidth. If x + iWidth == fullWidth, we're at the end.
      // We need to know the full (internally scaled) width.
    }
  }

  return 1;
}

// Optimized draw callback for speed and MCU delivery
int jpegDrawCallbackSequential(JPEGDRAW* pDraw) {
  auto* ctx = g_ctx;
  if (!ctx) return 0;

  // Deliver pixels to the correct output rows
  for (int y = 0; y < pDraw->iHeight; y++) {
    const int srcY = pDraw->y + y;
    const uint32_t srcY_fp = static_cast<uint32_t>(srcY + 1) << 16;

    // If this source row has moved into a new output row, emit previous one
    while (srcY_fp >= ctx->nextOutY_srcStart && ctx->currentOutY < ctx->outHeight) {
      // Only emit if we have actually seen pixels for the current row (i.e. we are at the first pixel of a new row)
      // but MCUs make this tricky.
      // Better: Buffer the whole image if it fits, or use a simpler 1:1 row delivery.
      // Since we are downscaling large images, let's just deliver rows when Y changes.
      emitOutputRow(ctx);
      ctx->currentOutY++;
      ctx->nextOutY_srcStart = static_cast<uint32_t>(ctx->currentOutY + 1) * ctx->scaleY_fp;
      memset(ctx->rowAccum, 0, ctx->outWidth * sizeof(uint32_t));
      memset(ctx->rowCount, 0, ctx->outWidth * sizeof(uint32_t));
    }

    const uint16_t* srcRow = pDraw->pPixels + (y * pDraw->iWidth);
    for (int outX = 0; outX < ctx->outWidth; outX++) {
      const int srcXStart = (static_cast<uint32_t>(outX) * ctx->scaleX_fp) >> 16;
      const int srcXEnd = (static_cast<uint32_t>(outX + 1) * ctx->scaleX_fp) >> 16;

      const int intersectXStart = std::max(srcXStart, (int)pDraw->x);
      const int intersectXEnd = std::min(srcXEnd, (int)(pDraw->x + pDraw->iWidthUsed));

      if (intersectXStart < intersectXEnd) {
        for (int srcX = intersectXStart; srcX < intersectXEnd; srcX++) {
          const uint16_t rgb565 = srcRow[srcX - pDraw->x];
          const uint8_t r = ((rgb565 >> 11) & 0x1F) << 3;
          const uint8_t g = ((rgb565 >> 5) & 0x3F) << 2;
          const uint8_t b = (rgb565 & 0x1F) << 3;
          ctx->rowAccum[outX] += (r * 25 + g * 50 + b * 25) / 100;
          ctx->rowCount[outX]++;
        }
      }
    }
  }
  return 1;
}

inline void write16(Print& out, const uint16_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
}

inline void write32(Print& out, const uint32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

inline void write32Signed(Print& out, const int32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

void writeBmpHeader1bit(Print& bmpOut, const int width, const int height) {
  const int bytesPerRow = (width + 31) / 32 * 4;
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 62 + imageSize;
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);
  write32(bmpOut, 0);
  write32(bmpOut, 62);
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);
  write16(bmpOut, 1);
  write16(bmpOut, 1);
  write32(bmpOut, 0);
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);
  write32(bmpOut, 2835);
  write32(bmpOut, 2);
  write32(bmpOut, 2);
  uint8_t palette[8] = {0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x00};
  for (const uint8_t i : palette) bmpOut.write(i);
}

void writeBmpHeader2bit(Print& bmpOut, const int width, const int height) {
  const int bytesPerRow = (width * 2 + 31) / 32 * 4;
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 70 + imageSize;
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);
  write32(bmpOut, 0);
  write32(bmpOut, 70);
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);
  write16(bmpOut, 1);
  write16(bmpOut, 2);
  write32(bmpOut, 0);
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);
  write32(bmpOut, 2835);
  write32(bmpOut, 4);
  write32(bmpOut, 4);
  uint8_t palette[16] = {0x00, 0x00, 0x00, 0x00, 0x55, 0x55, 0x55, 0x00,
                         0xAA, 0xAA, 0xAA, 0x00, 0xFF, 0xFF, 0xFF, 0x00};
  for (const uint8_t i : palette) bmpOut.write(i);
}
}  // namespace

bool JpegToBmpConverter::jpegFileToBmpStreamInternal(FsFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                                     bool oneBit, bool crop) {
  auto* jpeg = new (std::nothrow) JPEGDEC();
  if (!jpeg) {
    LOG_ERR("JPG", "Failed to allocate JPEGDEC");
    return false;
  }
  if (!jpeg->open(&jpegFile, static_cast<int>(jpegFile.size()), nullptr, jpegReadCallback, jpegSeekCallback,
                  jpegDrawCallbackSequential)) {
    LOG_ERR("JPG", "Failed to open JPEG");
    delete jpeg;
    return false;
  }

  const int srcWidth = jpeg->getWidth();
  const int srcHeight = jpeg->getHeight();
  const bool isProgressive = jpeg->getJPEGType() == JPEG_MODE_PROGRESSIVE;

  // Determine optimal internal scaling (1, 2, 4, or 8)
  int decodeFlags = 0;
  int internalScale = 1;

  if (isProgressive) {
    // Progressive JPEGs: JPEGDEC forces JPEG_SCALE_EIGHTH internally (DC-only decode)
    decodeFlags = JPEG_SCALE_EIGHTH;
    internalScale = 8;
  } else {
    const float scaleNeeded = std::min((float)targetWidth / srcWidth, (float)targetHeight / srcHeight);
    if (scaleNeeded <= 0.125f) {
      decodeFlags = JPEG_SCALE_EIGHTH;
      internalScale = 8;
    } else if (scaleNeeded <= 0.25f) {
      decodeFlags = JPEG_SCALE_QUARTER;
      internalScale = 4;
    } else if (scaleNeeded <= 0.5f) {
      decodeFlags = JPEG_SCALE_HALF;
      internalScale = 2;
    }
  }

  // JPEGDEC uses ceiling division for its internal scaled dimensions
  const int scaledSrcWidth = (srcWidth + internalScale - 1) / internalScale;
  const int scaledSrcHeight = (srcHeight + internalScale - 1) / internalScale;

  // Calculate final output dimensions based on target
  float finalScale = std::min((float)targetWidth / scaledSrcWidth, (float)targetHeight / scaledSrcHeight);
  if (crop) finalScale = std::max((float)targetWidth / scaledSrcWidth, (float)targetHeight / scaledSrcHeight);

  const int outWidth = std::max(1, (int)(scaledSrcWidth * finalScale));
  const int outHeight = std::max(1, (int)(scaledSrcHeight * finalScale));

  const uint32_t scaleX_fp = (static_cast<uint32_t>(scaledSrcWidth) << 16) / outWidth;
  const uint32_t scaleY_fp = (static_cast<uint32_t>(scaledSrcHeight) << 16) / outHeight;

  int bytesPerRow = oneBit ? ((outWidth + 31) / 32 * 4) : ((outWidth * 2 + 31) / 32 * 4);
  if (oneBit)
    writeBmpHeader1bit(bmpOut, outWidth, outHeight);
  else
    writeBmpHeader2bit(bmpOut, outWidth, outHeight);

  auto* ctx = new (std::nothrow) JpegDrawContext();
  if (!ctx) {
    LOG_ERR("JPG", "Failed to allocate JpegDrawContext");
    jpeg->close();
    delete jpeg;
    return false;
  }
  memset(ctx, 0, sizeof(JpegDrawContext));
  ctx->outWidth = outWidth;
  ctx->outHeight = outHeight;
  ctx->scaleX_fp = scaleX_fp;
  ctx->scaleY_fp = scaleY_fp;
  ctx->oneBit = oneBit;
  ctx->bmpOut = &bmpOut;
  ctx->bytesPerRow = bytesPerRow;
  ctx->nextOutY_srcStart = scaleY_fp;
  ctx->rowBuffer = static_cast<uint8_t*>(malloc(bytesPerRow));
  ctx->rowAccum = new (std::nothrow) uint32_t[outWidth]();
  ctx->rowCount = new (std::nothrow) uint32_t[outWidth]();

  if (oneBit)
    ctx->atkinson1BitDitherer = new (std::nothrow) Atkinson1BitDitherer(outWidth);
  else
    ctx->atkinsonDitherer = new (std::nothrow) AtkinsonDitherer(outWidth);

  bool success = false;
  if (!ctx->rowBuffer || !ctx->rowAccum || !ctx->rowCount || (oneBit && !ctx->atkinson1BitDitherer) ||
      (!oneBit && !ctx->atkinsonDitherer)) {
    LOG_ERR("JPG", "Failed to allocate buffers/ditherers");
    goto cleanup;
  }

  g_ctx = ctx;
  success = jpeg->decode(0, 0, decodeFlags);
  if (success && ctx->currentOutY < outHeight) emitOutputRow(ctx);
  g_ctx = nullptr;

cleanup:
  delete[] ctx->rowAccum;
  delete[] ctx->rowCount;
  delete ctx->atkinsonDitherer;
  delete ctx->atkinson1BitDitherer;
  free(ctx->rowBuffer);
  delete ctx;
  jpeg->close();
  delete jpeg;

  return success;
}

bool JpegToBmpConverter::jpegFileToBmpStream(FsFile& jpegFile, Print& bmpOut, bool crop) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, TARGET_MAX_WIDTH, TARGET_MAX_HEIGHT, false, crop);
}

bool JpegToBmpConverter::jpegFileToBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                     int targetMaxHeight) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, false);
}

bool JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                         int targetMaxHeight) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, true, true);
}
