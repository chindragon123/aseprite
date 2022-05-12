// Aseprite
// Copyright (C) 2018-2020  Igara Studio S.A.
// Copyright (C) 2001-2018  David Capello
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "app/app.h"
#include "app/doc.h"
#include "app/file/file.h"
#include "app/file/file_format.h"
#include "app/file/format_options.h"
#include "app/file/png_format.h"
#include "app/file/png_options.h"
#include "base/clamp.h"
#include "base/file_handle.h"
#include "doc/doc.h"
#include "gfx/color_space.h"

#include <stdio.h>
#include <stdlib.h>

#include "png.h"

#define PNG_TRACE(...) // TRACE

namespace app {

using namespace base;

class PngFormat : public FileFormat {
  const char* onGetName() const override {
    return "png";
  }

  void onGetExtensions(base::paths& exts) const override {
    exts.push_back("png");
  }

  dio::FileFormat onGetDioFormat() const override {
    return dio::FileFormat::PNG_IMAGE;
  }

  int onGetFlags() const override {
    return
      FILE_SUPPORT_LOAD |
      FILE_SUPPORT_SAVE |
      FILE_SUPPORT_RGB |
      FILE_SUPPORT_RGBA |
      FILE_SUPPORT_GRAY |
      FILE_SUPPORT_GRAYA |
      FILE_SUPPORT_INDEXED |
      FILE_SUPPORT_SEQUENCES |
      FILE_SUPPORT_PALETTE_WITH_ALPHA;
  }

  bool onLoad(FileOp* fop) override;
  gfx::ColorSpaceRef loadColorSpace(png_structp png, png_infop info);
#ifdef ENABLE_SAVE
  bool onSave(FileOp* fop) override;
  void saveColorSpace(png_structp png, png_infop info, const gfx::ColorSpace* colorSpace);
#endif
};

FileFormat* CreatePngFormat()
{
  return new PngFormat;
}

static void report_png_error(rlbox::rlbox_sandbox<rlbox::rlbox_wasm2c_sandbox>& _, 
                              rlbox::tainted<png_structp, rlbox::rlbox_wasm2c_sandbox> png, 
                              rlbox::tainted<png_const_charp, rlbox::rlbox_wasm2c_sandbox> error)
{
//   size_t png_size = sizeof(png) + 1;
//   auto tainted_png = sandbox.malloc_in_sandbox<char>(png_size);
//   memcpy(tainted_png.UNSAFE_unverified(), &png, png_size);
  auto png_error_ptr = sandbox.invoke_sandbox_function(png_get_error_ptr, png).copy_and_verify([](png_voidp val) {
    return val;
  })
  ((FileOp*)png_error_ptr)->setError("libpng: %s\n", error);

  // sandbox.free_in_sandbox(tainted_png);
}

void read_data_fn(rlbox::rlbox_sandbox<rlbox::rlbox_wasm2c_sandbox>& _,
                  rlbox::tainted<png_structp, rlbox::rlbox_wasm2c_sandbox> tainted_png,
                  rlbox::tainted<png_bytep, rlbox::rlbox_wasm2c_sandbox> tainted_png_byte,
                  size_t size) 
{
    png_bytep png_byte = tainted_png_byte.UNSAFE_unverified();

    auto fp = sandbox.invoke_sandbox_function(png_get_io_ptr, tainted_png).copy_and_verify([](png_FILE_p val) {
      return val;
    })

    size_t result = fread(png_byte, 1, size, fp);

    if (result != size){
        const char* error_msg = "read error in read_data_memory";
        size_t error_msg_size = strlen(error_msg) + 1;
        auto tainted_msg = sandbox.malloc_in_sandbox<char>(error_msg_size);
        std::strncpy(tainted_msg.UNSAFE_unverified(), error_msg, error_msg_size);

        sandbox.invoke_sandbox_function(png_error, tainted_png, tainted_msg);
    }
}

// TODO this should be information in FileOp parameter of onSave()
static bool fix_one_alpha_pixel = false;

PngEncoderOneAlphaPixel::PngEncoderOneAlphaPixel(bool state)
{
  fix_one_alpha_pixel = state;
}

PngEncoderOneAlphaPixel::~PngEncoderOneAlphaPixel()
{
  fix_one_alpha_pixel = false;
}

namespace {

class DestroyReadPng {
  png_structp png;
  png_infop info;
public:
  DestroyReadPng(png_structp png, png_infop info) : png(png), info(info) { }
    sandbox.invoke_sandbox_function(png_destroy_read_struct, &png, info ? &info: nullptr, nullptr);
  }
};

class DestroyWritePng {
  png_structp png;
  png_infop info;
public:
  DestroyWritePng(png_structp png, png_infop info) : png(png), info(info) { }
  ~DestroyWritePng() {
    sandbox.invoke_sandbox_function(png_destroy_write_struct, &png, info ? &info: nullptr, nullptr);
  }
};

// As in png_fixed_point_to_float() in skia/src/codec/SkPngCodec.cpp
float png_fixtof(png_fixed_point x)
{
  // We multiply by the same factor that libpng used to convert
  // fixed point -> double.  Since we want floats, we choose to
  // do the conversion ourselves rather than convert
  // fixed point -> double -> float.
  return ((float)x) * 0.00001f;
}

png_fixed_point png_ftofix(float x)
{
  return x * 100000.0f;
}

int png_user_chunk(rlbox::rlbox_sandbox<rlbox::rlbox_wasm2c_sandbox>& _, 
                              rlbox::tainted<png_structp, rlbox::rlbox_wasm2c_sandbox> png,
                              rlbox::tainted<png_unknown_chunkp, rlbox::rlbox_wasm2c_sandbox> unknown)
{
  // size_t png_size = sizeof(png) + 1;
  // auto tainted_png = sandbox.malloc_in_sandbox<char>(png_size);
  // memcpy(tainted_png.UNSAFE_unverified(), &png, png_size);
  auto png_user_chunk_ptr = sandbox.invoke_sandbox_function(png_get_user_chunk_ptr, png).copy_and_verify([](png_voidp val) {
    return val;
  })

  auto data = (std::shared_ptr<PngOptions>*)png_user_chunk_ptr;
  std::shared_ptr<PngOptions>& opts = *data;

  PNG_TRACE("PNG: Read unknown chunk '%c%c%c%c'\n",
            unknown->name[0],
            unknown->name[1],
            unknown->name[2],
            unknown->name[3]);

  PngOptions::Chunk chunk;
  chunk.location = unknown->location;
  for (int i=0; i<4; ++i)
    chunk.name.push_back(unknown->name[i]);
  if (unknown->size > 0) {
    chunk.data.resize(unknown->size);
    std::copy(unknown->data,
              unknown->data+unknown->size,
              chunk.data.begin());
  }
  opts->addChunk(std::move(chunk));

  // sandbox.free_in_sandbox(tainted_png);

  return 1;
}

} // anonymous namespace

bool PngFormat::onLoad(FileOp* fop)
{
  png_uint_32 width, height, y;
  unsigned int sig_read = 0;
  int bit_depth, color_type, interlace_type;
  int num_palette;
  png_colorp palette;
  png_bytepp rows_pointer;
  PixelFormat pixelFormat;

  // TODO: Adjust to set custom IO functions
  FileHandle handle(open_file_with_exception(fop->filename(), "rb"));
  FILE* fp = handle.get();

  /* Create and initialize the png_struct with the desired error handler
   * functions.  If you want to use the default stderr and longjump method,
   * you can supply NULL for the last three parameters.  We also supply the
   * the compiler header file version, so that we know if the application
   * was compiled with a compatible version of the library
   */
 
  auto libVerStrSize = strlen(PNG_LIBPNG_VER_STRING);
  auto sbLibVer = sandbox.malloc_in_sandbox<char>(libVerStrSize);
  strncpy(sbLibVer.unverified_safe_pointer_because(libVerStrSize, "compatibility"), PNG_LIBPNG_VER_STRING, libVerStrSize);

  auto png =
    sandbox.invoke_sandbox_function(png_create_read_struct, sbLibVer, NULL,
                           NULL, NULL);
  auto pngCheckPtr = png.unverified_safe_pointer_because(1, "Compatibility");
  if (pngCheckPtr == nullptr) {
    fop->setError("png_create_read_struct\n");
    return false;
  }

  // Do don't check if the sRGB color profile is valid, it gives
  // problems with sRGB IEC61966-2.1 color profile from Photoshop.
  // See this thread: https://community.aseprite.org/t/2656
  sandbox.invoke_sandbox_function(png_set_option, png, PNG_SKIP_sRGB_CHECK_PROFILE, PNG_OPTION_ON);

  // Set a function to read user data chunks
  auto opts = std::make_shared<PngOptions>();
  png_set_read_user_chunk_fn(png, NULL, png_user_chunk);

  /* Allocate/initialize the memory for image information. */
  auto info = png_create_info_struct(png);
  auto infoCheckPtr = info.unverified_safe_pointer_because(1, "Compatibility");
  DestroyReadPng destroyer(png, info);
  if (infoCheckPtr == nullptr) {
    fop->setError("png_create_info_struct\n");
    return false;
  }

  /* Set error handling if you are using the setjmp/longjmp method (this is
   * the normal method of doing things with libpng).
   */
  /*
  if (setjmp(png_jmpbuf(png))) {
    fop->setError("Error reading PNG file\n");
    return false;
  }
  */


  /* Set up the input control if you are using standard C streams */
  sandbox.invoke_sandbox_function(png_init_io, png, sbfp);

  /* If we have already read some of the signature */
  sandbox.invoke_sandbox_function(png_set_sig_bytes, png, sig_read);

  /* The call to png_read_info() gives us all of the information from the
   * PNG file before the first IDAT (image data chunk).
   */
  sandbox.invoke_sandbox_function(png_read_info, png, info);

  auto sb_width = sandbox.malloc_in_sandbox<png_uint_32>(1);
  auto sb_height = sandbox.malloc_in_sandbox<png_uint_32>(1);
  auto sb_bit_depth = sandbox.malloc_in_sandbox<int>(1);
  auto sb_color_type = sandbox.malloc_in_sandbox<int>(1);
  auto sb_interlace_type = sandbox.malloc_in_sandbox<int>(1);

  sandbox.invoke_sandbox_function(png_get_IHDR, png, info, sb_width, sb_height, sb_bit_depth, sb_color_type,
               sb_interlace_type, NULL, NULL);
  width = *(sb_width.unverified_safe_pointer_because(1, "Compatibility"));
  height = *(sb_height.unverified_safe_pointer_because(1, "Compatibility"));
  bit_depth = *(sb_bit_depth.unverified_safe_pointer_because(1, "Compatibility"));
  color_type = *(sb_color_type.unverified_safe_pointer_because(1, "Compatibility"));
  sb_interlace_type = *(sb_interlace_type.unverified_safe_pointer_because(1, "Compatibility"));


  /* Set up the data transformations you want.  Note that these are all
   * optional.  Only call them if you want/need them.  Many of the
   * transformations only work on specific types of images, and many
   * are mutually exclusive.
   */

  /* tell libpng to strip 16 bit/color files down to 8 bits/color */
  sandbox.invoke_sandbox_function(png_set_strip_16, png);

  /* Extract multiple pixels with bit depths of 1, 2, and 4 from a single
   * byte into separate bytes (useful for paletted and grayscale images).
   */
  sandbox.invoke_sandbox_function(png_set_packing, png);

  /* Expand grayscale images to the full 8 bits from 1, 2, or 4 bits/pixel */
  if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
    sandbox.invoke_sandbox_function(png_set_expand_gray_1_2_4_to_8, png);

  /* Turn on interlace handling.  REQUIRED if you are not using
   * png_read_image().  To see how to handle interlacing passes,
   * see the png_read_row() method below:
   */
  int number_passes = sandbox.invoke_sandbox_function(png_set_interlace_handling, png).copy_and_verify([] (int thing) {
	return thing;
  });

  /* Optional call to gamma correct and add the background to the palette
   * and update info structure.
   */
  sandbox.invoke_sandbox_function(png_read_update_info, png, info);

  /* create the output image */
  switch (color_type) {

    case PNG_COLOR_TYPE_RGB_ALPHA:
      fop->sequenceSetHasAlpha(true);
    case PNG_COLOR_TYPE_RGB:
      pixelFormat = IMAGE_RGB;
      break;

    case PNG_COLOR_TYPE_GRAY_ALPHA:
      fop->sequenceSetHasAlpha(true);
    case PNG_COLOR_TYPE_GRAY:
      pixelFormat = IMAGE_GRAYSCALE;
      break;

    case PNG_COLOR_TYPE_PALETTE:
      pixelFormat = IMAGE_INDEXED;
      break;

    default:
      fop->setError("Color type not supported\n)");
      return false;
  }

  int imageWidth = sandbox.invoke_sandbox_function(png_get_image_width, png, info).copy_and_verify([] (int thing) {
	return thing;
  });
  
  int imageHeight = sandbox.invoke_sandbox_function(png_get_image_height, png, info).copy_and_verify([](int thing) {
	return thing;		  
  });
  
  Image* image = fop->sequenceImage(pixelFormat, imageWidth, imageHeight);
  if (!image) {
    fop->setError("file_sequence_image %dx%d\n", imageWidth, imageHeight);
    return false;
  }

  // Transparent color
  png_color_16p png_trans_color = NULL;
  auto png_trans_color_ptr = sandbox.malloc_in_sandbox<png_color_16p>(1);


  // Read the palette
  auto sb_palette = sandbox.malloc_in_sandbox<png_colorp>(1);
  auto sb_num_palette = sandbox.malloc_in_sandbox<int>(1);
  if (color_type == PNG_COLOR_TYPE_PALETTE &&

      sandbox.invoke_sandbox_function(png_get_PLTE, png, info, sb_palette, sb_num_palette)) {
    
    palette = *(sb_palette.unverified_safe_pointer_because(1, "Compatibility"));
    num_palette = *(sb_num_palette.unverified_safe_pointer_because(1, "Compatibility"));  
    fop->sequenceSetNColors(num_palette);

    for (int c=0; c<num_palette; ++c) {
      fop->sequenceSetColor(c,
                            palette[c].red,
                            palette[c].green,
                            palette[c].blue);
    }

    // Read alpha values for palette entries
    png_bytep trans = NULL;     // Transparent palette entries
    int num_trans = 0;
    int mask_entry = -1;
    auto num_trans_ptr = sandbox.malloc_in_sandbox<int>(1);
    auto trans_ptr = sandbox.malloc_in_sandbox<png_bytep>(1);

    sandbox.invoke_sandbox_function(png_get_tRNS, png, info, trans_ptr, num_trans_ptr, nullptr);
    num_trans = *(num_trans_ptr.unverified_safe_pointer_because(1, "Compatibility"));
    png_bytep trans = *(trans_ptr.unverified_safe_pointer_because(1, "Compatibility"));    

    for (int i = 0; i < num_trans; ++i) {
      fop->sequenceSetAlpha(i, trans[i]);

      if (trans[i] < 255) {
        fop->sequenceSetHasAlpha(true); // Is a transparent sprite
        if (trans[i] == 0) {
          if (mask_entry < 0)
            mask_entry = i;
        }
      }
    }

    if (mask_entry >= 0)
      fop->document()->sprite()->setTransparentColor(mask_entry);
  }
  else {
    sandbox.invoke_sandbox_function(png_get_tRNS, png, info, nullptr, nullptr, png_trans_color_ptr);
    png_trans_color = *(png_trans_color_ptr.unverified_safe_pointer_because(1, "compatibility"));
  }

  auto rows_pointer = sandbox.malloc_in_sandbox<png_bytep>(height);
  size_t numRowBytes = sandbox.invoke_sandbox_function(png_get_rowbytes, png, info);
  for (y = 0; y < height; y++)
    rows_pointer[y] = sandbox.malloc_in_sandbox<png_byte>(numRowBytes);

  for (int pass=0; pass<number_passes; ++pass) {
    for (y = 0; y < height; y++) {
   
      sandbox.invoke_sandbox_function(png_read_rows, png, rows_pointer+y, nullptr, 1);

      fop->setProgress(
        (double)((double)pass + (double)(y+1) / (double)(height))
        / (double)number_passes);

      if (fop->isStop())
        break;
    }
  }

  // Unwrap

  // Convert rows_pointer into the doc::Image
  for (y = 0; y < height; y++) {
    // RGB_ALPHA
    auto unwrap_row_ptr = rows_pointer[y].unverified_safe_pointer_because(numRowBytes, "Compatibility");
    if (color_type == PNG_COLOR_TYPE_RGB_ALPHA) {
      uint8_t* src_address = unwrap_row_ptr;
      uint32_t* dst_address = (uint32_t*)image->getPixelAddress(0, y);
      unsigned int x, r, g, b, a;

      for (x=0; x<width; x++) {
        r = *(src_address++);
        g = *(src_address++);
        b = *(src_address++);
        a = *(src_address++);
        *(dst_address++) = rgba(r, g, b, a);
      }
    }
    // RGB
    else if (color_type == PNG_COLOR_TYPE_RGB) {
      uint8_t* src_address = unwrap_row_ptr;
      uint32_t* dst_address = (uint32_t*)image->getPixelAddress(0, y);
      unsigned int x, r, g, b, a;

      for (x=0; x<width; x++) {
        r = *(src_address++);
        g = *(src_address++);
        b = *(src_address++);

        // Transparent color
        if (png_trans_color &&
            r == png_trans_color->red &&
            g == png_trans_color->green &&
            b == png_trans_color->blue) {
          a = 0;
          if (!fop->sequenceGetHasAlpha())
            fop->sequenceSetHasAlpha(true);
        }
        else
          a = 255;

        *(dst_address++) = rgba(r, g, b, a);
      }
    }
    // GRAY_ALPHA
    else if (color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
      uint8_t* src_address = unwrap_row_ptr;
      uint16_t* dst_address = (uint16_t*)image->getPixelAddress(0, y);
      unsigned int x, k, a;

      for (x=0; x<width; x++) {
        k = *(src_address++);
        a = *(src_address++);
        *(dst_address++) = graya(k, a);
      }
    }
    // GRAY
    else if (color_type == PNG_COLOR_TYPE_GRAY) {
      uint8_t* src_address = unwrap_row_ptr;
      uint16_t* dst_address = (uint16_t*)image->getPixelAddress(0, y);
      unsigned int x, k, a;

      for (x=0; x<width; x++) {
        k = *(src_address++);

        // Transparent color
        if (png_trans_color &&
            k == png_trans_color->gray) {
          a = 0;
          if (!fop->sequenceGetHasAlpha())
            fop->sequenceSetHasAlpha(true);
        }
        else
          a = 255;

        *(dst_address++) = graya(k, a);
      }
    }
    // PALETTE
    else if (color_type == PNG_COLOR_TYPE_PALETTE) {
      uint8_t* src_address = unwrap_row_ptr;
      uint8_t* dst_address = (uint8_t*)image->getPixelAddress(0, y);
      unsigned int x;

      for (x=0; x<width; x++)
        *(dst_address++) = *(src_address++);
    }
    sandbox.free_in_sandbox(rows_pointer[y]);
  }
  sandbox.free_in_sandbox(rows_pointer);

  // Setup the color space.
  auto colorSpace = PngFormat::loadColorSpace(png, info);
  if (colorSpace)
    fop->setEmbeddedColorProfile();
  else { // sRGB is the default PNG color space.
    colorSpace = gfx::ColorSpace::MakeSRGB();
  }
  if (colorSpace &&
      fop->document()->sprite()->colorSpace()->type() == gfx::ColorSpace::None) {
    fop->document()->sprite()->setColorSpace(colorSpace);
    fop->document()->notifyColorSpaceChanged();
  }

  ASSERT(opts != nullptr);
  if (!opts->isEmpty())
    fop->setLoadedFormatOptions(opts);

  return true;
}

// Returns a colorSpace object that represents any
// color space information in the encoded data.  If the encoded data
// contains an invalid/unsupported color space, this will return
// NULL. If there is no color space information, it will guess sRGB
//
// Code to read color spaces from png files from Skia (SkPngCodec.cpp)
// by Google Inc.
gfx::ColorSpaceRef PngFormat::loadColorSpace(rlbox::tainted<png_structp, rlbox::rlbox_wasm2c_sandbox> png_ptr, rlbox::tainted<png_infop, rlbox::rlbox_wasm2c_sandbox> info_ptr)
{
  // First check for an ICC profile
  png_bytep profile;
  png_uint_32 length;
  // The below variables are unused, however, we need to pass them in anyway or
  // png_get_iCCP() will return nothing.
  // Could knowing the |name| of the profile ever be interesting?  Maybe for debugging?
  png_charp name;
  // The |compression| is uninteresting since:
  //   (1) libpng has already decompressed the profile for us.
  //   (2) "deflate" is the only mode of decompression that libpng supports.
  int compression;

  // size_t png_size = sizeof(png_ptr) + 1;
  // auto tainted_png = sandbox.malloc_in_sandbox<char>(png_size);
  // memcpy(tainted_png.UNSAFE_unverified(), &png_ptr, png_size);

  // size_t info_size = sizeof(info_ptr) + 1;
  // auto tainted_info = sandbox.malloc_in_sandbox<char>(info_size);
  // memcpy(tainted_info.UNSAFE_unverified(), &info_ptr, info_size);

  size_t profile_size = sizeof(profile) + 1;
  auto tainted_profile = sandbox.malloc_in_sandbox<char>(profile_size);
  memcpy(tainted_profile.UNSAFE_unverified(), &profile, profile_size);

  size_t length_size = sizeof(length) + 1;
  auto tainted_length = sandbox.malloc_in_sandbox<char>(length_size);
  memcpy(tainted_length.UNSAFE_unverified(), &length, length_size);

  size_t name_size = sizeof(name) + 1;
  auto tainted_name = sandbox.malloc_in_sandbox<char>(name_size);
  memcpy(tainted_name.UNSAFE_unverified(), &name, name_size);

  size_t compression_size = sizeof(compression) + 1;
  auto tainted_compression = sandbox.malloc_in_sandbox<char>(compression_size);
  memcpy(tainted_compression.UNSAFE_unverified(), &compression, compression_size);

  auto png_iCCP = sandbox.invoke_sandbox_function(png_get_iCCP, png, info, tainted_name, tainted_compression, tainted_profile, tainted_length).copy_and_verify([](png_uint_32 val) {
    return val;
  })

  profile = *(tainted_profile.UNSAFE_unverified());
  length = *(tainted_length.UNSAFE_unverified());
  name = *(tainted_name.UNSAFE_unverified());
  compression = *(tainted_compression.UNSAFE_unverified());


  if (PNG_INFO_iCCP == png_iCCP) {
    auto colorSpace = gfx::ColorSpace::MakeICC(profile, length);
    if (name)
      colorSpace->setName(name);
    return colorSpace;
  }

  // Second, check for sRGB.

  auto png_valid = sandbox.invoke_sandbox_function(png_get_valid, png, info, PNG_INFO_sRGB).copy_and_verify([](png_uint_32 val) {
    return val;
  })

  if (png_valid) {
    // sRGB chunks also store a rendering intent: Absolute, Relative,
    // Perceptual, and Saturation.
    return gfx::ColorSpace::MakeSRGB();
  }



  // Next, check for chromaticities.
  png_fixed_point wx, wy, rx, ry, gx, gy, bx, by, invGamma;

  size_t wx_size = sizeof(wx) + 1;
  auto tainted_wx = sandbox.malloc_in_sandbox<char>(wx);
  memcpy(tainted_wx.UNSAFE_unverified(), &wx, wx_size);

  size_t wy_size = sizeof(wy) + 1;
  auto tainted_wy = sandbox.malloc_in_sandbox<char>(wy);
  memcpy(tainted_wy.UNSAFE_unverified(), &wy, wy_size)

  size_t rx_size = sizeof(rx) + 1;
  auto tainted_rx = sandbox.malloc_in_sandbox<char>(rx);
  memcpy(tainted_rx.UNSAFE_unverified(), &rx, rx_size)

  size_t ry_size = sizeof(ry) + 1;
  auto tainted_ry = sandbox.malloc_in_sandbox<char>(ry);
  memcpy(tainted_ry.UNSAFE_unverified(), &ry, ry_size)

  size_t gx_size = sizeof(gx) + 1;
  auto tainted_gx = sandbox.malloc_in_sandbox<char>(gx);
  memcpy(tainted_gx.UNSAFE_unverified(), &gx, gx_size)

  size_t gy_size = sizeof(gy) + 1;
  auto tainted_gy = sandbox.malloc_in_sandbox<char>(gy);
  memcpy(tainted_gy.UNSAFE_unverified(), &gy, gy_size)

  size_t bx_size = sizeof(bx) + 1;
  auto tainted_bx = sandbox.malloc_in_sandbox<char>(bx);
  memcpy(tainted_bx.UNSAFE_unverified(), &bx, bx_size)

  size_t by_size = sizeof(by) + 1;
  auto tainted_by = sandbox.malloc_in_sandbox<char>(by);
  memcpy(tainted_by.UNSAFE_unverified(), &by, by_size)

  size_t invGamma_size = sizeof(invGamma) + 1;
  auto tainted_invGamma = sandbox.malloc_in_sandbox<char>(invGamma);
  memcpy(tainted_invGamma.UNSAFE_unverified(), &invGamma, invGamma_size)

  auto png_cHRM_fixed = sandbox.invoke_sandbox_function(png_get_cHRM_fixed, png_ptr, info_ptr, tainted_wx, tainted_wy, tainted_rx, tainted_ry, tainted_gx, tainted_gy, tainted_bx, tainted_by).copy_and_verify([](png_uint_32 val) {
    return val;
  })

  wx = *(tainted_wx.UNSAFE_unverified());
  wy = *(tainted_wy.UNSAFE_unverified());
  rx = *(tainted_rx.UNSAFE_unverified());
  ry = *(tainted_ry.UNSAFE_unverified());
  gx = *(tainted_gx.UNSAFE_unverified());
  gy = *(tainted_gy.UNSAFE_unverified());
  bx = *(tainted_bx.UNSAFE_unverified());
  by = *(tainted_by.UNSAFE_unverified());

  if (png_cHRM_fixed) {
    gfx::ColorSpacePrimaries primaries;
    primaries.wx = png_fixtof(wx); primaries.wy = png_fixtof(wy);
    primaries.rx = png_fixtof(rx); primaries.ry = png_fixtof(ry);
    primaries.gx = png_fixtof(gx); primaries.gy = png_fixtof(gy);
    primaries.bx = png_fixtof(bx); primaries.by = png_fixtof(by);

    auto png_gAMA_fixed = sandbox.invoke_sandbox_function(png_get_cHRM_fixed, png_ptr, info_ptr, tainted_invGamma).copy_and_verify([](png_uint_32 val) {
      return val;
    })

    invGamma = *(tainted_invGamma.UNSAFE_unverified());

    if (PNG_INFO_gAMA == png_gAMA_fixed) {
      gfx::ColorSpaceTransferFn fn;
      fn.a = 1.0f;
      fn.b = fn.c = fn.d = fn.e = fn.f = 0.0f;
      fn.g = 1.0f / png_fixtof(invGamma);

      return gfx::ColorSpace::MakeRGB(fn, primaries);
    }

    // Default to sRGB gamma if the image has color space information,
    // but does not specify gamma.
    return gfx::ColorSpace::MakeRGBWithSRGBGamma(primaries);
  }

  // Last, check for gamma.
  if (PNG_INFO_gAMA == png_get_gAMA_fixed(png_ptr, info_ptr, &invGamma)) {
    // Since there is no cHRM, we will guess sRGB gamut.
    return gfx::ColorSpace::MakeSRGBWithGamma(1.0f / png_fixtof(invGamma));
  }

  sandbox.free_in_sandbox(tainted_profile);
  sandbox.free_in_sandbox(tainted_length);
  sandbox.free_in_sandbox(tainted_name);
  sandbox.free_in_sandbox(tainted_compression);
  sandbox.free_in_sandbox(tainted_wx);
  sandbox.free_in_sandbox(tainted_wy);
  sandbox.free_in_sandbox(tainted_rx);
  sandbox.free_in_sandbox(tainted_ry);
  sandbox.free_in_sandbox(tainted_gx);
  sandbox.free_in_sandbox(tainted_gy);
  sandbox.free_in_sandbox(tainted_bx);
  sandbox.free_in_sandbox(tainted_by);

  // No color space.
  return nullptr;
}

#ifdef ENABLE_SAVE

bool PngFormat::onSave(FileOp* fop)
{
  png_infop info;
  png_colorp palette = nullptr;
  png_bytep row_pointer;
  int color_type = 0;

  FileHandle handle(open_file_with_exception_sync_on_close(fop->filename(), "wb"));
  FILE* fp = handle.get();

  png_structp png =
    png_create_write_struct(PNG_LIBPNG_VER_STRING, (png_voidp)fop,
                            report_png_error, report_png_error);
  if (png == nullptr)
    return false;

  // Remove sRGB profile checks
  png_set_option(png, PNG_SKIP_sRGB_CHECK_PROFILE, PNG_OPTION_ON);

  info = png_create_info_struct(png);
  DestroyWritePng destroyer(png, info);
  if (info == nullptr)
    return false;

  if (setjmp(png_jmpbuf(png)))
    return false;

  auto opIndexPtr = sandbox.malloc_in_sandbox<int>(1);
  *opIndexPtr = 1;

  png_set_read_fn(png, sandbox_static_cast<void*>(opIndexPtr), read_data);

  const Image* image = fop->sequenceImage();
  switch (image->pixelFormat()) {
    case IMAGE_RGB:
      color_type =
        (fop->document()->sprite()->needAlpha() ||
         fix_one_alpha_pixel ?
         PNG_COLOR_TYPE_RGB_ALPHA:
         PNG_COLOR_TYPE_RGB);
      break;
    case IMAGE_GRAYSCALE:
      color_type =
        (fop->document()->sprite()->needAlpha() ||
         fix_one_alpha_pixel ?
         PNG_COLOR_TYPE_GRAY_ALPHA:
         PNG_COLOR_TYPE_GRAY);
      break;
    case IMAGE_INDEXED:
      if (fix_one_alpha_pixel)
        color_type = PNG_COLOR_TYPE_RGB_ALPHA;
      else
        color_type = PNG_COLOR_TYPE_PALETTE;
      break;
  }

  const png_uint_32 width = image->width();
  const png_uint_32 height = image->height();

  png_set_IHDR(png, info, width, height, 8, color_type,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

  // User chunks
  auto opts = fop->formatOptionsOfDocument<PngOptions>();
  if (opts && !opts->isEmpty()) {
    int num_unknowns = opts->size();
    ASSERT(num_unknowns > 0);
    std::vector<png_unknown_chunk> unknowns(num_unknowns);
    int i = 0;
    for (const auto& chunk : opts->chunks()) {
      png_unknown_chunk& unknown = unknowns[i];
      for (int i=0; i<5; ++i) {
        unknown.name[i] =
          (i < int(chunk.name.size()) ? chunk.name[i]: 0);
      }
      PNG_TRACE("PNG: Write unknown chunk '%c%c%c%c'\n",
                unknown.name[0],
                unknown.name[1],
                unknown.name[2],
                unknown.name[3]);
      unknown.data = (png_byte*)&chunk.data[0];
      unknown.size = chunk.data.size();
      unknown.location = chunk.location;
      ++i;
    }
    png_set_unknown_chunks(png, info, &unknowns[0], num_unknowns);
  }

  if (fop->preserveColorProfile() &&
      fop->document()->sprite()->colorSpace())
    saveColorSpace(png, info, fop->document()->sprite()->colorSpace().get());

  if (color_type == PNG_COLOR_TYPE_PALETTE) {
    int c, r, g, b;
    int pal_size = fop->sequenceGetNColors();
    pal_size = base::clamp(pal_size, 1, PNG_MAX_PALETTE_LENGTH);

#if PNG_MAX_PALETTE_LENGTH != 256
#error PNG_MAX_PALETTE_LENGTH should be 256
#endif

    // Save the color palette.
    palette = (png_colorp)png_malloc(png, pal_size * sizeof(png_color));
    for (c = 0; c < pal_size; c++) {
      fop->sequenceGetColor(c, &r, &g, &b);
      palette[c].red   = r;
      palette[c].green = g;
      palette[c].blue  = b;
    }

    png_set_PLTE(png, info, palette, pal_size);

    // If the sprite does not have a (visible) background layer, we
    // put alpha=0 to the transparent color.
    int mask_entry = -1;
    if (fop->document()->sprite()->backgroundLayer() == NULL ||
        !fop->document()->sprite()->backgroundLayer()->isVisible()) {
      mask_entry = fop->document()->sprite()->transparentColor();
    }

    bool all_opaque = true;
    int num_trans = pal_size;
    png_bytep trans = (png_bytep)png_malloc(png, num_trans);

    for (c=0; c<num_trans; ++c) {
      int alpha = 255;
      fop->sequenceGetAlpha(c, &alpha);
      trans[c] = (c == mask_entry ? 0: alpha);
      if (alpha < 255)
        all_opaque = false;
    }

    if (!all_opaque || mask_entry >= 0)
      png_set_tRNS(png, info, trans, num_trans, nullptr);

    png_free(png, trans);
  }

  png_write_info(png, info);
  png_set_packing(png);

  row_pointer = (png_bytep)png_malloc(png, png_get_rowbytes(png, info));

  for (png_uint_32 y=0; y<height; ++y) {
    uint8_t* dst_address = row_pointer;

    if (png_get_color_type(png, info) == PNG_COLOR_TYPE_RGB_ALPHA) {
      unsigned int x, c, a;
      bool opaque = true;

      if (image->pixelFormat() == IMAGE_RGB) {
        uint32_t* src_address = (uint32_t*)image->getPixelAddress(0, y);

        for (x=0; x<width; ++x) {
          c = *(src_address++);
          a = rgba_geta(c);

          if (opaque) {
            if (a < 255)
              opaque = false;
            else if (fix_one_alpha_pixel && x == width-1 && y == height-1)
              a = 254;
          }

          *(dst_address++) = rgba_getr(c);
          *(dst_address++) = rgba_getg(c);
          *(dst_address++) = rgba_getb(c);
          *(dst_address++) = a;
        }
      }
      // In case that we are converting an indexed image to RGB just
      // to convert one pixel with alpha=254.
      else if (image->pixelFormat() == IMAGE_INDEXED) {
        uint8_t* src_address = (uint8_t*)image->getPixelAddress(0, y);
        unsigned int x, c;
        int r, g, b, a;
        bool opaque = true;

        for (x=0; x<width; ++x) {
          c = *(src_address++);
          fop->sequenceGetColor(c, &r, &g, &b);
          fop->sequenceGetAlpha(c, &a);

          if (opaque) {
            if (a < 255)
              opaque = false;
            else if (fix_one_alpha_pixel && x == width-1 && y == height-1)
              a = 254;
          }

          *(dst_address++) = r;
          *(dst_address++) = g;
          *(dst_address++) = b;
          *(dst_address++) = a;
        }
      }
    }
    else if (png_get_color_type(png, info) == PNG_COLOR_TYPE_RGB) {
      uint32_t* src_address = (uint32_t*)image->getPixelAddress(0, y);
      unsigned int x, c;

      for (x=0; x<width; ++x) {
        c = *(src_address++);
        *(dst_address++) = rgba_getr(c);
        *(dst_address++) = rgba_getg(c);
        *(dst_address++) = rgba_getb(c);
      }
    }
    else if (png_get_color_type(png, info) == PNG_COLOR_TYPE_GRAY_ALPHA) {
      uint16_t* src_address = (uint16_t*)image->getPixelAddress(0, y);
      unsigned int x, c, a;
      bool opaque = true;

      for (x=0; x<width; x++) {
        c = *(src_address++);
        a = graya_geta(c);

        if (opaque) {
          if (a < 255)
            opaque = false;
          else if (fix_one_alpha_pixel && x == width-1 && y == height-1)
            a = 254;
        }

        *(dst_address++) = graya_getv(c);
        *(dst_address++) = a;
      }
    }
    else if (png_get_color_type(png, info) == PNG_COLOR_TYPE_GRAY) {
      uint16_t* src_address = (uint16_t*)image->getPixelAddress(0, y);
      unsigned int x, c;

      for (x=0; x<width; ++x) {
        c = *(src_address++);
        *(dst_address++) = graya_getv(c);
      }
    }
    else if (png_get_color_type(png, info) == PNG_COLOR_TYPE_PALETTE) {
      uint8_t* src_address = (uint8_t*)image->getPixelAddress(0, y);
      unsigned int x;

      for (x=0; x<width; ++x)
        *(dst_address++) = *(src_address++);
    }

    png_write_rows(png, &row_pointer, 1);

    fop->setProgress((double)(y+1) / (double)(height));
  }

  png_free(png, row_pointer);
  png_write_end(png, info);

  if (image->pixelFormat() == IMAGE_INDEXED) {
    png_free(png, palette);
    palette = nullptr;
  }

  return true;
}

void PngFormat::saveColorSpace(png_structp png_ptr, png_infop info_ptr,
                               const gfx::ColorSpace* colorSpace)
{
  switch (colorSpace->type()) {

    case gfx::ColorSpace::None:
      // Do just nothing (png file without profile, like old Aseprite versions)
      break;

    case gfx::ColorSpace::sRGB:
      // TODO save the original intent
      if (!colorSpace->hasGamma()) {
        png_set_sRGB(png_ptr, info_ptr, PNG_sRGB_INTENT_PERCEPTUAL);
        return;
      }

      // Continue to RGB case...

    case gfx::ColorSpace::RGB: {
      if (colorSpace->hasPrimaries()) {
        const gfx::ColorSpacePrimaries* p = colorSpace->primaries();

        sandbox.invoke_sandbox_function(png_set_cHRM_fixed, png_ptr, info_ptr,
                                        png_ftofix(p->wx), png_ftofix(p->wy),
                                        png_ftofix(p->rx), png_ftofix(p->ry),
                                        png_ftofix(p->gx), png_ftofix(p->gy),
                                        png_ftofix(p->bx), png_ftofix(p->by));

      }
      if (colorSpace->hasGamma()) {
        sandbox.invoke_sandbox_function(png_set_gAMA_fixed, png_ptr, info_ptr,png_ftofix(1.0f / colorSpace->gamma()));
      }
      break;
    }

    case gfx::ColorSpace::ICC: {
      png_set_iCCP(png_ptr, info_ptr,
                   (png_const_charp)colorSpace->name().c_str(),
                   PNG_COMPRESSION_TYPE_DEFAULT,
                   (png_const_bytep)colorSpace->iccData(),
                   (png_uint_32)colorSpace->iccSize());
      break;
    }

  }
}

#endif  // ENABLE_SAVE

} // namespace app
