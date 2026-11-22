#include "encodingUtils.h"
#include "rsCodec.h"

#include <QRandomGenerator>

#include <cmath>
#include <string>

QByteArray encodeChunk(const QByteArray& data) {
    const std::string encoded = rs::encode(data.toStdString());
    return QByteArray(encoded.data(), qsizetype(encoded.size()));
}

QByteArray decodeChunk(const QByteArray& encodedData, bool& corrupted, int* correctedBytes) {
    const rs::DecodeResult result = rs::decode(encodedData.toStdString());
    corrupted = !result.ok;
    if (correctedBytes)
        *correctedBytes = int(result.correctedBytes);
    return QByteArray(result.data.data(), qsizetype(result.data.size()));
}

QByteArray addNoise(const QByteArray& data, double bitErrorRate) {
    QByteArray noisyData = data;
    if (bitErrorRate <= 0.0 || data.isEmpty())
        return noisyData;

    QRandomGenerator* rand = QRandomGenerator::global();
    const qint64 totalBits = qint64(noisyData.size()) * 8;

    if (bitErrorRate >= 1.0) {
        for (char& c : noisyData)
            c = char(~c);
        return noisyData;
    }

    const double logKeep = std::log1p(-bitErrorRate);
    qint64 bit = -1;
    while (true) {
        const double u = 1.0 - rand->generateDouble();  // in (0, 1]
        bit += 1 + qint64(std::floor(std::log(u) / logKeep));
        if (bit < 0 || bit >= totalBits)
            break;
        noisyData[int(bit / 8)] = char(noisyData[int(bit / 8)] ^ (1 << (bit % 8)));
    }
    return noisyData;
}
