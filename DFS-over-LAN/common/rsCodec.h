#ifndef RSCODEC_H
#define RSCODEC_H

// Reed-Solomon error correction for chunks, built on the schifra library.
//
// Plain C++ (no Qt) so it can be tested on its own.
//
// Encoded format: the chunk is prefixed with an 8-byte header
// (4-byte length + 4-byte CRC32 of the data, little-endian), split into
// 223-byte blocks (the last one padded with zeros), and every block is encoded
// as a 255-byte RS(255,223) code word. Each code word can correct up to 16
// corrupted bytes. The CRC32 catches the rare case where a block has too many
// errors and the decoder "corrects" it to the wrong data.

#include <cstddef>
#include <cstdint>
#include <string>

namespace rs {

constexpr std::size_t kCodeLength = 255;  // bytes per code word
constexpr std::size_t kFecLength = 32;    // parity bytes per code word
constexpr std::size_t kDataLength = kCodeLength - kFecLength;  // 223 data bytes
constexpr std::size_t kMaxCorrectable = kFecLength / 2;        // 16 byte errors per code word
constexpr std::size_t kHeaderLength = 8;

struct DecodeResult {
    std::string data;                 // the decoded chunk (best effort if !ok)
    bool ok = false;                  // false if the chunk could not be recovered
    std::size_t correctedBytes = 0;   // number of bytes the decoder fixed
    std::size_t failedBlocks = 0;     // code words with too many errors to fix
};

std::string encode(const std::string& data);
DecodeResult decode(const std::string& encoded);

// Size of the encoded form of a chunk with dataLength bytes.
std::size_t encodedSize(std::size_t dataLength);

std::uint32_t crc32(const std::string& data);

}  // namespace rs

#endif  // RSCODEC_H
