#include "fast_dct.h"

#include <cassert>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>
#include <algorithm>
#include <numeric>

// #define VERBOSE
#include "opencv_dct.hpp"

#include "histogram.h"
#include "ans_ocl.h"

#include "codec.h"
#include "dxt_image.h"

#include <opencv2/opencv.hpp>

#ifndef _MSC_VER
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuninitialized"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion-null"
#endif
#include "stb_dxt.h"
#define CRND_HEADER_FILE_ONLY
#include "crn_decomp.h"
#ifndef _MSC_VER
#pragma GCC diagnostic pop
#pragma GCC diagnostic pop
#pragma GCC diagnostic pop
#endif

static uint64_t CompressRGB(const uint8_t *img, int width) {
  unsigned char block[64];
  memset(block, 0, sizeof(block));

  for (int j = 0; j < 4; ++j) {
    for (int i = 0; i < 4; ++i) {
      int src_idx = (j * width + i) * 3;
      int dst_idx = (j * 4 + i) * 4;

      unsigned char *block_pixel = block + dst_idx;
      const unsigned char *img_pixel = img + src_idx;

      block_pixel[0] = img_pixel[0];
      block_pixel[1] = img_pixel[1];
      block_pixel[2] = img_pixel[2];
      block_pixel[3] = 0xFF;
    }
  }

  uint64_t result;
  stb_compress_dxt_block(reinterpret_cast<unsigned char *>(&result),
                         block, 0, STB_DXT_HIGHQUAL);
  return result;
}

static uint64_t CompressRGBA(const uint8_t *img, int width) {
  unsigned char block[64];
  memset(block, 0, sizeof(block));

  for (int j = 0; j < 4; ++j) {
    memcpy(block + j*16, img + width*4*j, 16);
  }

  uint64_t result;
  stb_compress_dxt_block(reinterpret_cast<unsigned char *>(&result),
                         block, 0, STB_DXT_HIGHQUAL);
  return result;
}

uint16_t Into565(uint8_t r, uint8_t g, uint8_t b) {
  uint16_t rr = (r >> 3) & 0x1F;
  uint16_t gg = (g >> 2) & 0x3F;
  uint16_t bb = (b >> 3) & 0x1F;

  return (rr << 11) | (gg << 5) | bb;
}

#ifdef VERBOSE
static const int coeff_offset = 512;
#endif

static const uint32_t kNumStreams = 16;
void encode(const cv::Mat &img, std::vector<uint8_t> *result) {
  // Collect stats for frequency analysis
  std::vector<int16_t> coeffs(img.rows * img.cols, 0);
  assert(coeffs.size() % (ans::ocl::kNumEncodedSymbols * kNumStreams) == 0);

  int16_t min_coeff = std::numeric_limits<int16_t>::max();
  int16_t max_coeff = std::numeric_limits<int16_t>::min();
  int32_t num_outliers = 0;
  int32_t num_zeros = 0;

#ifdef VERBOSE
  std::cout << std::endl << "First 16 encoded values: ";
#endif // Verbose

  for (int j = 0; j < img.rows; j++) {
    for (int i = 0; i < img.cols; i++) {
      int16_t coeff = img.at<int16_t>(j, i);
      if (coeff < -127 || coeff > 127) {
        num_outliers++;
      }

      if (coeff == 0) {
        num_zeros++;
      }

      min_coeff = std::min(min_coeff, coeff);
      max_coeff = std::max(max_coeff, coeff);

      int coeff_idx = j*img.cols + i;
#ifdef VERBOSE
      if (coeff_offset <= coeff_idx && coeff_idx < coeff_offset + 16) {
        std::cout << coeff << ", ";
        if (coeff_idx == coeff_offset + 15) {
          std::cout << std::endl;
        }
      }
#endif // VERBOSE

      coeffs[coeff_idx] = coeff;
    }
  }

  std::vector<uint8_t> symbols;
  symbols.reserve(coeffs.size());

  for (const auto coeff : coeffs) {
    if (coeff < -127 || coeff > 127) {
      symbols.push_back(0);
    } else {
      symbols.push_back(static_cast<uint8_t>(coeff + 128));
    }
  }

  const std::vector<uint32_t> counts = std::move(ans::CountSymbols(symbols));
  assert(counts.size() == 256);

  std::vector<uint8_t> encoded_symbols(256, 0);
  std::vector<uint32_t> encoded_counts;
  encoded_counts.reserve(256);

  uint32_t sym_idx = 0;
  for (uint32_t i = 0; i < 256; ++i) {
    if (counts[i] == 0) {
      continue;
    }

    encoded_symbols[i] = sym_idx++;
    encoded_counts.push_back(counts[i]);
  }

  size_t bytes_written = result->size();
  result->resize(bytes_written + 2);
  *reinterpret_cast<uint16_t *>(result->data() + bytes_written) =
    static_cast<uint16_t>(encoded_counts.size());
  bytes_written += 2;

  for (uint32_t i = 0; i < 256; ++i) {
    if (counts[i] == 0) {
      continue;
    }

    result->resize(bytes_written + 3);

    uint8_t *symbol_ptr = result->data() + bytes_written;
    uint16_t *count_ptr = reinterpret_cast<uint16_t *>(symbol_ptr + 1);

    *symbol_ptr = static_cast<uint8_t>(i);
    *count_ptr = static_cast<uint16_t>(counts[i]);
    bytes_written += 3;
  }
#ifdef VERBOSE
  std::cout << "Total symbols: " << img.cols * img.rows << std::endl;
  std::cout << "Num outliers: " << num_outliers << std::endl;
  std::cout << "Num zeros: " << num_zeros << std::endl;
  std::cout << "Num unique symbols: " << encoded_counts.size() << std::endl;
  std::cout << "Min coefficient: " << min_coeff << std::endl;
  std::cout << "Max coefficient: " << max_coeff << std::endl;

  for (uint32_t i = 0; i < counts.size(); ++i) {
    std::cout << counts[i] << ", ";
    if ((i + 1) % 16 == 0) {
      std::cout << std::endl;
    }
  }
#endif  //  VERBOSE

  // Write counts to output
  std::vector<std::unique_ptr<ans::Encoder> > encoders;
  encoders.reserve(kNumStreams);
  for (uint32_t i = 0; i < kNumStreams; ++i) {
    encoders.push_back(std::move(ans::ocl::CreateCPUEncoder(encoded_counts)));
  }

  std::vector<uint8_t> encoded(10, 0);
  uint32_t encoded_bytes_written = 0;
  uint32_t last_encoded_bytes_written = 0;

  uint32_t symbol_offset = 0;
  while(symbol_offset < symbols.size()) {
    for (uint32_t sym_idx = 0; sym_idx < ans::ocl::kNumEncodedSymbols; ++sym_idx) {
      // Make sure that we have at least 4*kNumStreams bytes available
      encoded.resize(encoded_bytes_written + (4*kNumStreams));

      int16_t *output = reinterpret_cast<int16_t *>(encoded.data() + encoded_bytes_written);
      for (uint32_t strm_idx = 0; strm_idx < kNumStreams; strm_idx++) {
        uint32_t sidx = symbol_offset + strm_idx * ans::ocl::kNumEncodedSymbols + sym_idx;
        if (symbols[sidx] == 0) {
          *output = coeffs[sidx];
          output++;
          encoded_bytes_written += 2;
        }
      }

      for (uint32_t strm_idx = 0; strm_idx < kNumStreams; ++strm_idx) {
        ans::BitWriter w(encoded.data() + encoded_bytes_written);
        uint32_t sidx = symbol_offset + strm_idx * ans::ocl::kNumEncodedSymbols + sym_idx;
        uint8_t symbol = encoded_symbols[symbols[sidx]];

        assert(symbol < encoded_counts.size());
        assert(counts[symbols[sidx]] > 0);

        encoders[strm_idx]->Encode(symbol, &w);
        encoded_bytes_written += w.BytesWritten();
      }
    }

    // Dump all of the encoder states
    encoded.resize(encoded_bytes_written + 4*kNumStreams);

    uint32_t *encoder_state = reinterpret_cast<uint32_t *>(encoded.data() + encoded_bytes_written);
    for (uint32_t i = 0; i < kNumStreams; ++i) {
      encoder_state[i] = encoders[i]->GetState();
    }
    encoded_bytes_written += 4 * kNumStreams;

    // Add the offset to the stream...
    uint32_t offset = encoded_bytes_written - last_encoded_bytes_written;
    result->resize(bytes_written + 2);
    *reinterpret_cast<uint16_t *>(result->data() + bytes_written) = static_cast<uint16_t>(offset);
    bytes_written += 2;
    assert(offset <= ((1 << 16) - 1));
    last_encoded_bytes_written = encoded_bytes_written;

    // Advance the symbol offset...
    symbol_offset += kNumStreams * ans::ocl::kNumEncodedSymbols;
  }

  // Add the encoded bytes
  result->resize(bytes_written + encoded_bytes_written);
  memcpy(result->data() + bytes_written, encoded.data(), encoded_bytes_written);
}

int decode(cv::Mat *result, const uint8_t *buf) {
  int offset = 0;

  uint16_t num_symbols = *reinterpret_cast<const uint16_t *>(buf + offset);
  offset += 2;

  std::vector<uint8_t> symbols(num_symbols, 0);
  std::vector<uint32_t> counts(num_symbols, 0);

  for (uint32_t i = 0; i < num_symbols; ++i) {
    symbols[i] = *reinterpret_cast<const uint8_t *>(buf + offset);
    counts[i] = *reinterpret_cast<const uint16_t *>(buf + offset + 1);
    offset += 3;
  }

  int num_macroblocks =
    (result->cols * result->rows) / (kNumStreams * ans::ocl::kNumEncodedSymbols);
  assert(num_macroblocks * kNumStreams * ans::ocl::kNumEncodedSymbols == 
         static_cast<uint32_t>(result->cols * result->rows));
  std::vector<uint16_t> macroblock_sizes(num_macroblocks);

  for (int i = 0; i < num_macroblocks; ++i) {
    macroblock_sizes[i] = *reinterpret_cast<const uint16_t *>(buf + offset);
    offset += 2;
  }

  std::vector<int16_t> coeffs(result->cols * result->rows);

  int symbol_offset = 0;
  for (uint16_t mb_size : macroblock_sizes) {
    int mb_off = offset + mb_size;

    const uint32_t *states = reinterpret_cast<const uint32_t *>(buf + mb_off) - kNumStreams;
    std::vector<std::unique_ptr<ans::Decoder> > decoders;
    decoders.reserve(kNumStreams);
    for (uint32_t i = 0; i < kNumStreams; ++i) {
      decoders.push_back(ans::ocl::CreateCPUDecoder(states[kNumStreams - i - 1], counts));
    }

    int data_sz_bytes = mb_size - 4 * kNumStreams;
    assert(data_sz_bytes % 2 == 0);

    std::vector<uint16_t> mb_data(data_sz_bytes / 2);
    memcpy(mb_data.data(), buf + offset, data_sz_bytes);
    std::reverse(mb_data.begin(), mb_data.end());

    ans::BitReader r(reinterpret_cast<const uint8_t *>(mb_data.data()));
    for (uint32_t sym_idx = 0; sym_idx < ans::ocl::kNumEncodedSymbols; sym_idx++) {
      for (uint32_t strm_idx = 0; strm_idx < kNumStreams; ++strm_idx) {
        uint32_t sidx = symbol_offset + (kNumStreams - strm_idx) * ans::ocl::kNumEncodedSymbols - sym_idx - 1;
        coeffs[sidx] = static_cast<int16_t>(symbols[decoders[strm_idx]->Decode(&r)]) - 128;
      }

      for (uint32_t strm_idx = 0; strm_idx < kNumStreams; ++strm_idx) {
        uint32_t idx = symbol_offset + (kNumStreams - strm_idx) * ans::ocl::kNumEncodedSymbols - sym_idx - 1;
        if (coeffs[idx] == -128) {
          coeffs[idx] = static_cast<int16_t>(r.ReadBits(16));
        }
      }
    }

    offset = mb_off;
    symbol_offset += kNumStreams * ans::ocl::kNumEncodedSymbols;
  }

  // Populate the image properly
  assert(result->type() == CV_16SC1);
  int coeff_idx = 0;
#ifdef VERBOSE
  std::cout << "First 16 decoded values: ";
#endif
  for (int j = 0; j < result->rows; ++j) {
    for (int i = 0; i < result->cols; ++i) {
#ifdef VERBOSE
      if (coeff_offset <= coeff_idx && coeff_idx < coeff_offset + 16) {
        std::cout << coeffs[coeff_idx] << ", ";
        if (coeff_idx == coeff_offset + 15) {
          std::cout << std::endl;
        }
      }
#endif  // VERBOSE
      result->at<int16_t>(j, i) = coeffs[coeff_idx++];
    }
  }

  return offset;
}

static const cv::Mat quant_table_luma = (cv::Mat_<int16_t>(8, 8) <<
  16, 11, 10, 16, 24, 40, 51, 61,
  12, 12, 14, 19, 26, 58, 60, 55,
  14, 13, 16, 24, 40, 57, 69, 56,
  14, 17, 22, 29, 51, 87, 80, 62,
  18, 22, 37, 56, 68, 109, 103, 77,
  24, 35, 55, 64, 81, 104, 113, 92,
  49, 64, 78, 87, 103, 121, 120, 101,
  72, 92, 95, 98, 112, 100, 103, 99);

static const cv::Mat quant_table_chroma = (cv::Mat_<int16_t>(8, 8) <<
  17, 18, 24, 47, 99, 99, 99, 99,
  18, 21, 26, 66, 99, 99, 99, 99,
  24, 26, 56, 99, 99, 99, 99, 99,
  47, 66, 99, 99, 99, 99, 99, 99,
  99, 99, 99, 99, 99, 99, 99, 99,
  99, 99, 99, 99, 99, 99, 99, 99,
  99, 99, 99, 99, 99, 99, 99, 99,
  99, 99, 99, 99, 99, 99, 99, 99);

void quantize(cv::Mat *dct, bool is_chroma) {
#ifdef VERBOSE
  static int prequantized_copy = 0;
  char fname[256];
  sprintf(fname, "prequantized_%d.png", prequantized_copy);
  cv::imwrite(fname, *dct);
  prequantized_copy++;
#endif // VERBOSE

  for (int j = 0; j < dct->rows / 8; ++j) {
    for (int i = 0; i < dct->cols / 8; ++i) {
      cv::Rect_<int> window(i * 8, j * 8, 8, 8);
      cv::Mat block = (*dct)(window).clone();

      cv::divide(block, is_chroma ? quant_table_chroma : quant_table_luma, block);

      block.copyTo((*dct)(window));
    }
  }

#ifdef VERBOSE
  static int quantized_copy = 0;
  sprintf(fname, "quantized_%d.png", quantized_copy);
  cv::imwrite(fname, *dct);
  quantized_copy++;
#endif // VERBOSE
}

void dequantize(cv::Mat *dct, bool is_chroma) {
#ifdef VERBOSE
  static int predequantized_copy = 0;
  char fname[256];
  sprintf(fname, "predequantized_%d.png", predequantized_copy);
  cv::imwrite(fname, *dct);
  predequantized_copy++;
#endif // VERBOSE

  for (int j = 0; j < dct->rows / 8; ++j) {
    for (int i = 0; i < dct->cols / 8; ++i) {
      cv::Rect_<int> window(i * 8, j * 8, 8, 8);
      cv::Mat block = (*dct)(window).clone();

      cv::multiply(block, is_chroma ? quant_table_chroma : quant_table_luma, block);

      block.copyTo((*dct)(window));
    }
  }

#ifdef VERBOSE
  static int dequantized_copy = 0;
  sprintf(fname, "dequantized_%d.png", dequantized_copy);
  cv::imwrite(fname, *dct);
  dequantized_copy++;
#endif // VERBOSE
}

void compressChannel(const cv::Mat &img, std::vector<uint8_t> *result, bool is_chroma) {
  assert(img.channels() == 1);

  // DCT
  cv::Mat dct_img = img.clone();
  dct::RunDCT(&dct_img);

  assert(dct_img.type() == CV_16SC1);

  // Quantize
  quantize(&dct_img, is_chroma);

  // Encode
  encode(dct_img, result);
}

int decompressChannel(cv::Mat *result, const uint8_t *buf, int width, int height, bool is_chroma) {
  *result = cv::Mat(height, width, CV_16SC1);
  int offset = decode(result, buf);

  dequantize(result, is_chroma);
  dct::RunIDCT(result);

  return offset;
}

std::vector<uint8_t> compress(const cv::Mat &img) {
  cv::Mat img_YCrCb;
  cv::cvtColor(img, img_YCrCb, CV_RGB2YCrCb);

  cv::Mat channels[3];
  cv::split(img_YCrCb, channels);

  // Subsample chroma...
  cv::resize(channels[1], channels[1], cv::Size((img.cols + 1) / 2, (img.rows + 1) / 2));
  cv::resize(channels[2], channels[2], cv::Size((img.cols + 1) / 2, (img.rows + 1) / 2));

  std::vector<uint8_t> result;
  result.resize(8);

  uint32_t *resultBuf = reinterpret_cast<uint32_t *>(result.data());
  resultBuf[0] = img.cols;
  resultBuf[1] = img.rows;

  compressChannel(channels[0], &result, false);
  compressChannel(channels[1], &result, true);
  compressChannel(channels[2], &result, true);

#ifdef VERBOSE
  std::cout << "Endpoint Image uncompressed size: " << img.cols * img.rows * 2 << std::endl;
  std::cout << "Endpoint Image compressed size: " << result.size() << std::endl;
#endif

  return std::move(result);
}

cv::Mat decompress(const std::vector<uint8_t> &stream) {
  const uint8_t *stream_buf = stream.data();

  const uint32_t *size_buf = reinterpret_cast<const uint32_t *>(stream_buf);
  int width = size_buf[0];
  int height = size_buf[1];

  int offset = 8;

  cv::Mat channels[3];
  offset += decompressChannel(channels + 0, stream_buf + offset, width, height, false);
  offset += decompressChannel(channels + 1, stream_buf + offset, (width + 1) / 2, (height + 1) / 2, true);
  offset += decompressChannel(channels + 2, stream_buf + offset, (width + 1) / 2, (height + 1) / 2, true);

  // Resize chroma to full...
  cv::resize(channels[1], channels[1], cv::Size(width, height));
  cv::resize(channels[2], channels[2], cv::Size(width, height));

  cv::Mat img_YCrCb;
  cv::merge(channels, 3, img_YCrCb);

  cv::Mat result(height, width, CV_8UC4);
  cv::cvtColor(img_YCrCb, result, CV_YCrCb2RGB);
  return result;
}

std::vector<uint8_t> entropy_encode_index_symbols(const std::vector<uint8_t> &symbols) {
  // Make sure that we have a multiples of 256 symbols
  assert((symbols.size() % (256 * 16)) == 0);

  // First collect histogram
  std::vector<uint32_t> counts(4, 0);
  for (auto symbol : symbols) {
    assert(symbol < 4);
    counts[symbol]++;
  }

#ifdef VERBOSE
  {
    double num_predicted = static_cast<double>(counts[0]);
    double num_indices = static_cast<double>(std::accumulate(counts.begin(), counts.end(), 0));
    std::cout << "Predicted: " << (num_predicted * 100.0) / num_indices << std::endl;

    ans::Options opts;
    opts.b = 2;
    opts.k = 1;
    const int denominator = 2048;
    std::vector<uint32_t> F = ans::GenerateHistogram(counts, denominator);
    opts.Fs = F;

    const uint32_t M = std::accumulate(F.begin(), F.end(), 0);
    assert(static_cast<int>(M) == denominator);

    std::vector<uint8_t> rANS_Stream(2048, 0);
    ans::BitWriter rANS_Writer = ans::BitWriter(rANS_Stream.data());

    std::vector<uint8_t> tANS_Stream(2048, 0);
    ans::BitWriter tANS_Writer = ans::BitWriter(tANS_Stream.data());

    opts.type = ans::eType_rANS;
    std::unique_ptr<ans::Encoder> rANS_coder = ans::Encoder::Create(opts);

    opts.type = ans::eType_tANS;
    std::unique_ptr<ans::Encoder> tANS_coder = ans::Encoder::Create(opts);

    double H = 0;
    for (auto f : F) {
      double Ps = static_cast<double>(f);
      H -= Ps * log2(Ps);
    }
    H = log2(static_cast<double>(M)) + (H / static_cast<double>(M));

    const int num_symbols = 2048;
    for (int i = 0; i < num_symbols; ++i) {
      int r = rand() % M;
      uint32_t symbol = 0;
      int freq = 0;
      for (auto f : F) {
        freq += f;
        if (r < freq) {
          break;
        }
        symbol++;
      }

      rANS_coder->Encode(symbol, &rANS_Writer);
      tANS_coder->Encode(symbol, &tANS_Writer);
    }

    std::cout << "Interpolation value stats:" << std::endl;
    std::cout << "Uncompressed Size of 2-bit symbols: " << (num_symbols * 2) / 8 << std::endl;
    std::cout << "H: " << H << std::endl;
    std::cout << "Expected num bytes: " << H*(num_symbols / 8) << std::endl;
    std::cout << "rANS state: " << rANS_coder->GetState() << std::endl;
    std::cout << "tANS state: " << tANS_coder->GetState() << std::endl;
    std::cout << "rANS bytes written: " << rANS_Writer.BytesWritten() << std::endl;
    std::cout << "tANS bytes written: " << tANS_Writer.BytesWritten() << std::endl << std::endl;
  }
#endif  // VERBOSE

  std::vector<uint8_t> output(4 * sizeof(uint32_t));
  uint32_t bytes_written = 4 * sizeof(uint32_t);

  // Write counts to output
  for (size_t i = 0; i < counts.size(); ++i) {
    assert(static_cast<uint64_t>(counts[i]) < (1ULL << 32));
    reinterpret_cast<uint32_t *>(output.data())[i] = counts[i];
  }

  std::vector<std::unique_ptr<ans::Encoder> > encoders;
  encoders.reserve(kNumStreams);
  for (uint32_t i = 0; i < kNumStreams; ++i) {
    encoders.push_back(std::move(ans::ocl::CreateCPUEncoder(counts)));
  }

  std::vector<uint8_t> encoded;
  uint32_t encoded_bytes_written = 0;
  uint32_t last_encoded_bytes_written = 0;

  uint32_t symbol_offset = 0;
  while(symbol_offset < symbols.size()) {
    for (uint32_t sym_idx = 0; sym_idx < ans::ocl::kNumEncodedSymbols; sym_idx++) {
      encoded.resize(encoded_bytes_written + 2*kNumStreams);

      for (uint32_t strm_idx = 0; strm_idx < kNumStreams; ++strm_idx) {
        ans::BitWriter w(encoded.data() + encoded_bytes_written);
        uint32_t sidx = symbol_offset + (strm_idx + 1) * ans::ocl::kNumEncodedSymbols - sym_idx - 1;
        uint8_t symbol = symbols[sidx];

        assert(symbol < counts.size());
        assert(counts[symbol] > 0);

        encoders[strm_idx]->Encode(symbol, &w);
        encoded_bytes_written += w.BytesWritten();
      }
    }

    // Write the encoder states to the encoded stream...
    encoded.resize(encoded_bytes_written + 4*kNumStreams);
    uint32_t *states = reinterpret_cast<uint32_t *>(encoded.data() + encoded_bytes_written);
    for (uint32_t strm_idx = 0; strm_idx < kNumStreams; ++strm_idx) {
      states[strm_idx] = encoders[strm_idx]->GetState();
    }
    encoded_bytes_written += 4 * kNumStreams;

    // Add the offset to the stream...
    uint32_t offset = encoded_bytes_written - last_encoded_bytes_written;
    output.resize(bytes_written + 2);
    *reinterpret_cast<uint16_t *>(output.data() + bytes_written) = static_cast<uint16_t>(offset);
    bytes_written += 2;
    assert(offset <= ((1 << 16) - 1));
    last_encoded_bytes_written = encoded_bytes_written;

    // Get ready for the next symbols...
    symbol_offset += kNumStreams * ans::ocl::kNumEncodedSymbols;
  }

  output.resize(bytes_written + encoded_bytes_written);
  output.insert(output.begin() + bytes_written, encoded.begin(), encoded.end());

  return std::move(output);
}

std::vector<uint8_t> compress_indices(const GenTC::DXTImage &dxt) {
  std::vector<uint8_t> symbolized_indices = dxt.PredictIndicesLinearize(16, 16);

  // Visualize
  cv::imwrite("img_dxt_interp_predicted.png", cv::Mat(dxt.Height(), dxt.Width(), CV_8UC1,
    GenTC::DXTImage::TwoBitValuesToImage(dxt.PredictIndices(16, 16)).data()));

  return std::move(entropy_encode_index_symbols(symbolized_indices));
}

cv::Mat dft_opencv(const cv::Mat I) {
  using namespace cv; // !YUCK!
  
  Mat padded;                            //expand input image to optimal size
  int m = getOptimalDFTSize( I.rows );
  int n = getOptimalDFTSize( I.cols ); // on the border add zero values
  copyMakeBorder(I, padded, 0, m - I.rows, 0, n - I.cols, BORDER_CONSTANT, Scalar::all(0));

  Mat planes[] = {Mat_<float>(padded), Mat::zeros(padded.size(), CV_32F)};
  Mat complexI;
  merge(planes, 2, complexI);         // Add to the expanded another plane with zeros

  dft(complexI, complexI);            // this way the result may fit in the source matrix

  // compute the magnitude and switch to logarithmic scale
  // => log(1 + sqrt(Re(DFT(I))^2 + Im(DFT(I))^2))
  split(complexI, planes);                   // planes[0] = Re(DFT(I), planes[1] = Im(DFT(I))
  magnitude(planes[0], planes[1], planes[0]);// planes[0] = magnitude
  Mat magI = planes[0];

  magI += Scalar::all(1);                    // switch to logarithmic scale
  log(magI, magI);
  
  // crop the spectrum, if it has an odd number of rows or columns
  magI = magI(Rect(0, 0, magI.cols & -2, magI.rows & -2));

  // rearrange the quadrants of Fourier image  so that the origin is at the image center
  int cx = magI.cols/2;
  int cy = magI.rows/2;

  Mat q0(magI, Rect(0, 0, cx, cy));   // Top-Left - Create a ROI per quadrant
  Mat q1(magI, Rect(cx, 0, cx, cy));  // Top-Right
  Mat q2(magI, Rect(0, cy, cx, cy));  // Bottom-Left
  Mat q3(magI, Rect(cx, cy, cx, cy)); // Bottom-Right

  Mat tmp;                           // swap quadrants (Top-Left with Bottom-Right)
  q0.copyTo(tmp);
  q3.copyTo(q0);
  tmp.copyTo(q3);

  q1.copyTo(tmp);                    // swap quadrant (Top-Right with Bottom-Left)
  q2.copyTo(q1);
  tmp.copyTo(q2);

  normalize(magI, magI, 0, 1, CV_MINMAX); // Transform the matrix with float values into a

  Mat result;
  magI.convertTo(result, CV_8UC1, 255.0, 0.5);
  
  return result;
}

int main(int argc, char **argv) {
  // Make sure that we have the proper number of arguments...
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <filename>" << std::endl;
    return 1;
  }

  // Is it a crunch img?
  size_t width, height;
  std::vector<GenTC::PhysicalDXTBlock> dxt_blocks;
  std::string fname(argv[1]);
  if (fname.substr(fname.find_last_of(".") + 1) == "crn") {
    std::ifstream ifs(fname.c_str(), std::ifstream::binary|std::ifstream::ate);
    std::ifstream::pos_type pos = ifs.tellg();

    std::vector<uint8_t> crn(pos);

    ifs.seekg(0, std::ifstream::beg);
    ifs.read(reinterpret_cast<char *>(crn.data()), pos);

    crnd::crn_texture_info tinfo;
    if (!crnd::crnd_get_texture_info(crn.data(), crn.size(), &tinfo)) {
      assert(!"Invalid texture?");
      return 1;
    }

    width = tinfo.m_width;
    height = tinfo.m_height;

    crnd::crnd_unpack_context ctx = crnd::crnd_unpack_begin(crn.data(), crn.size());
    if (!ctx) {
      assert(!"Error beginning crn decoding!");
      return 1;
    }

    const int num_blocks_x = (width + 3) / 4;
    const int num_blocks_y = (height + 3) / 4;
    const int num_blocks = num_blocks_x * num_blocks_y;
    dxt_blocks.resize(num_blocks);

    void *dst = dxt_blocks.data();
    if (!crnd::crnd_unpack_level(ctx, &dst, dxt_blocks.size() * 8, num_blocks_x * 8, 0)) {
      assert(!"Error decoding crunch texture!");
      return 1;
    }

    crnd::crnd_unpack_end(ctx);

  } else {
    // Otherwise, load the file
    const cv::Mat img = cv::imread(argv[1], -1);
    if (!img.data) {
      std::cerr << "Error loading image: " << argv[1] << std::endl;
      return 1;
    }
    width = img.cols;
    height = img.rows;

    assert((img.cols & 0x3) == 0);
    assert((img.rows & 0x3) == 0);

    const int num_blocks_x = (img.cols + 3) / 4;
    const int num_blocks_y = (img.rows + 3) / 4;
    const int num_blocks = num_blocks_x * num_blocks_y;

    // Now do the dxt compression...
    dxt_blocks.resize(num_blocks);
    for (int j = 0; j < img.rows; j += 4) {
      for (int i = 0; i < img.cols; i += 4) {
        int block_idx = (j / 4) * num_blocks_x + (i / 4);
        const unsigned char *offset_data = img.ptr(j) + i * img.channels();
        if (3 == img.channels()) {
          dxt_blocks[block_idx].dxt_block = CompressRGB(offset_data, img.cols);
        } else if (4 == img.channels()) {
          dxt_blocks[block_idx].dxt_block = CompressRGBA(offset_data, img.cols);
        } else {
          std::cerr << "Error! Only accepts RGB or RGBA images!" << std::endl;
        }
      }
    }

    cv::Mat planes[3];
    cv::split(img, planes);
    cv::imwrite("img_red.png", planes[0]);
    cv::imwrite("img_red_dft.png", dft_opencv(planes[0]));
  }

  const uint8_t *dxt_data = reinterpret_cast<const uint8_t *>(dxt_blocks.data());
  GenTC::DXTImage dxt_img(dxt_data, width, height);
  GenTC::CompressDXT(argv[1], width, height);

  // Decompress into image...
  cv::imwrite("img_dxt.png", cv::Mat(height, width, CV_8UC4,
    const_cast<uint8_t*>(dxt_img.DecompressedImage()->Pack().data())));

  // Visualize interpolation data...
  cv::Mat interp_img = cv::Mat(height, width, CV_8UC1,
                               dxt_img.InterpolationImage().data());
  cv::imwrite("img_dxt_interp.png", interp_img);

  cv::imwrite("img_dxt_interp_dft.png", dft_opencv(interp_img));

  return 0;
}
