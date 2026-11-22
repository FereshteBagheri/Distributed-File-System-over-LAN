#include "rsCodec.h"

#include <array>

#include "schifra_galois_field.hpp"
#include "schifra_galois_field_polynomial.hpp"
#include "schifra_reed_solomon_block.hpp"
#include "schifra_reed_solomon_decoder.hpp"
#include "schifra_reed_solomon_encoder.hpp"
#include "schifra_sequential_root_generator_polynomial_creator.hpp"

namespace rs {
namespace {

using Encoder = schifra::reed_solomon::encoder<kCodeLength, kFecLength, kDataLength>;
using Decoder = schifra::reed_solomon::decoder<kCodeLength, kFecLength, kDataLength>;
using Block = schifra::reed_solomon::block<kCodeLength, kFecLength>;

constexpr std::size_t kFieldDescriptor = 8;            // GF(2^8): one symbol per byte
constexpr std::size_t kGeneratorPolynomialIndex = 120;

// The field, generator polynomial, encoder and decoder are expensive to build,
// so they are created once and shared.
struct Codec {
    schifra::galois::field field;
    schifra::galois::field_polynomial generator;
    Encoder encoder;
    Decoder decoder;

    Codec()
        : field(kFieldDescriptor, schifra::galois::primitive_polynomial_size06,
                schifra::galois::primitive_polynomial06),
          generator(makeGenerator(field)),
          encoder(field, generator),
          decoder(field, kGeneratorPolynomialIndex) {}

    static schifra::galois::field_polynomial makeGenerator(const schifra::galois::field& f) {
        schifra::galois::field_polynomial g(f);
        schifra::make_sequential_root_generator_polynomial(f, kGeneratorPolynomialIndex,
                                                           kFecLength, g);
        return g;
    }
};

const Codec& codec() {
    static const Codec instance;
    return instance;
}

void putU32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

std::uint32_t getU32(const std::string& in, std::size_t pos) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= std::uint32_t(static_cast<unsigned char>(in[pos + i])) << (8 * i);
    return v;
}

}  // namespace

std::uint32_t crc32(const std::string& data) {
    // Standard CRC-32 (IEEE 802.3, as used by zlib), table-driven.
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();

    std::uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char b : data)
        crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

std::size_t encodedSize(std::size_t dataLength) {
    const std::size_t payload = kHeaderLength + dataLength;
    const std::size_t blocks = (payload + kDataLength - 1) / kDataLength;
    return blocks * kCodeLength;
}

std::string encode(const std::string& data) {
    std::string payload;
    payload.reserve(kHeaderLength + data.size());
    putU32(payload, static_cast<std::uint32_t>(data.size()));
    putU32(payload, crc32(data));
    payload += data;

    const std::size_t blocks = (payload.size() + kDataLength - 1) / kDataLength;
    payload.resize(blocks * kDataLength, '\0');

    std::string encoded;
    encoded.reserve(blocks * kCodeLength);

    const Codec& c = codec();
    for (std::size_t b = 0; b < blocks; ++b) {
        std::string message = payload.substr(b * kDataLength, kDataLength);
        message.resize(kCodeLength, '\0');  // schifra expects a code-word-sized buffer

        Block block;
        c.encoder.encode(message, block);
        for (std::size_t i = 0; i < kCodeLength; ++i)
            encoded.push_back(static_cast<char>(block[i] & 0xFF));
    }
    return encoded;
}

DecodeResult decode(const std::string& encoded) {
    DecodeResult result;
    if (encoded.empty() || encoded.size() % kCodeLength != 0)
        return result;  // truncated or malformed datagram

    const std::size_t blocks = encoded.size() / kCodeLength;
    std::string payload;
    payload.reserve(blocks * kDataLength);

    const Codec& c = codec();
    for (std::size_t b = 0; b < blocks; ++b) {
        Block block;
        for (std::size_t i = 0; i < kCodeLength; ++i)
            block[i] = static_cast<unsigned char>(encoded[b * kCodeLength + i]);

        if (c.decoder.decode(block))
            result.correctedBytes += block.errors_corrected;
        else
            ++result.failedBlocks;  // keep going: the caller still gets best-effort data

        for (std::size_t i = 0; i < kDataLength; ++i)
            payload.push_back(static_cast<char>(block[i] & 0xFF));
    }

    if (payload.size() < kHeaderLength)
        return result;

    const std::uint32_t length = getU32(payload, 0);
    const std::uint32_t expectedCrc = getU32(payload, 4);
    if (length > payload.size() - kHeaderLength)
        return result;  // header destroyed by noise

    result.data = payload.substr(kHeaderLength, length);
    result.ok = result.failedBlocks == 0 && crc32(result.data) == expectedCrc;
    return result;
}

}  // namespace rs
