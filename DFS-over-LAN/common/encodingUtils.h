#ifndef ENCODINGUTILS_H
#define ENCODINGUTILS_H

#include <QByteArray>

QByteArray encodeChunk(const QByteArray& data);

QByteArray decodeChunk(const QByteArray& encodedData, bool& corrupted, int* correctedBytes = nullptr);

QByteArray addNoise(const QByteArray& data, double bitErrorRate);

#endif // ENCODINGUTILS_H
