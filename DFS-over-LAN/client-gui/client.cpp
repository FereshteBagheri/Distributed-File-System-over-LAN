#include "client.h"
#include "encodingUtils.h"

#include <QDataStream>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QTextStream>

Client::Client(const QHostAddress& serverAddress, quint16 serverPort, QObject* parent)
    : QObject(parent), m_masterIp(serverAddress), m_masterPort(serverPort) {
    m_tcp = new QTcpSocket(this);
    connect(m_tcp, &QTcpSocket::connected, this, &Client::onConnected);
    connect(m_tcp, &QTcpSocket::disconnected, this, &Client::onDisconnected);
    connect(m_tcp, &QTcpSocket::readyRead, this, &Client::onReadyRead);
    connect(m_tcp, &QTcpSocket::errorOccurred, this, &Client::onError);
    m_tcp->connectToHost(m_masterIp, m_masterPort);

    m_udp = new QUdpSocket(this);
    connect(m_udp, &QUdpSocket::readyRead, this, &Client::onUdpReadyRead);
    m_udp->bind(QHostAddress::AnyIPv4, 0);

    m_retryTimer.setSingleShot(true);
    m_retryTimer.setInterval(RETRY_TIMEOUT_MS);
    connect(&m_retryTimer, &QTimer::timeout, this, &Client::onRetryTimeout);

    m_holePunching = qEnvironmentVariableIntValue("DFS_HOLE_PUNCH") == 1;
}

Client::~Client() {
    m_tcp->deleteLater();
    m_udp->deleteLater();
}

void Client::sendCommand(const QString& command, const QString& params) {
    QString pkt_params = params;
    if (command == "ALLOCATE_CHUNKS") {
        QStringList parts = params.split(' ', Qt::SkipEmptyParts);
        if (parts.size() >= 1) {
            m_fileId = parts[0];

            const QString parentPath = parentDirectory(); // FIXME: update for executable
            QString fullPath = QDir(parentPath).filePath(m_fileId);

            m_file.setFileName(fullPath);
            if (!m_file.open(QIODevice::ReadOnly)) {
                emit errorOccurred("Cannot open file: " + fullPath);
                return;
            }

            m_fileSize = m_file.size();
            m_numChunks = int((m_fileSize + CHUNK_SIZE - 1) / CHUNK_SIZE);
            m_currentChunk = 0;
            m_uploadChunks.clear();

            pkt_params += " " + QString::number(m_fileSize);
        } else {
            emit errorOccurred("Invalid parameters for ALLOCATE_CHUNKS");
            return;
        }
    } else if (command == "LOOKUP_FILE") {
        m_downloadId = params.trimmed();
        m_downloadCurrent = 0;
        m_downloadChunksInfo.clear();

        m_outFile.setFileName(m_downloadId + ".download");
        if (!m_outFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            emit errorOccurred("Cannot open output: " + m_outFile.fileName());
            return;
        }
    }

    QByteArray pkt = (command + " " + pkt_params + "\n").toUtf8();
    if (m_tcp->state() == QAbstractSocket::ConnectedState)
        m_tcp->write(pkt);
    else
        emit errorOccurred("Not connected to master server");
}

void Client::onConnected() {
    m_connected = true;
    emit connectionStateChanged(true);
}

void Client::onDisconnected() {
    m_connected = false;
    emit connectionStateChanged(false);
}

void Client::onError(QAbstractSocket::SocketError socketError) {
    emit errorOccurred(m_tcp->errorString());
    if (socketError == QAbstractSocket::ConnectionRefusedError)
        emit connectionStateChanged(false);
}

void Client::onReadyRead() {
    while (m_tcp->canReadLine()) {
        QString pkt = m_tcp->readLine().trimmed();
        emit responseReceived(pkt);

        auto parts = pkt.split(' ', Qt::SkipEmptyParts);
        if (parts.size() >= 3 && parts[0] == "OK" && parts[1] == "Allocated") {
            int NumberOfChunks = parts[2].toInt();
            int idx = 3;
            getChunkInfos(pkt, m_uploadChunks, idx, NumberOfChunks);
            m_transfer = Transfer::Upload;
            m_attempts = 0;
            m_reallocations = 0;
            uploadFileToChunk();
        }
        else if (parts.size() >= 5 && parts[0] == "OK" && parts[1] == "Reallocated") {
            // OK Reallocated <chunkId> <ip> <port>
            if (m_transfer != Transfer::Upload || !m_waitingForReallocation)
                continue;
            auto& info = m_uploadChunks[m_currentChunk];
            if (parts[2] != info.chunkId)
                continue;
            info.ip = QHostAddress(parts[3]);
            info.port = parts[4].toUShort();
            m_waitingForReallocation = false;
            m_attempts = 0;
            emit logReceived(QString("Re-routing %1 → %2:%3").arg(info.chunkId, info.ip.toString()).arg(info.port));
            uploadFileToChunk();
        }
        else if (parts.size() >= 4 && parts[0] == "FILE_METADATA") {
            int NumberOfChunks = (parts.size() - 2) / 3;
            int idx = 2;
            m_downloadChunks = NumberOfChunks;
            getChunkInfos(pkt, m_downloadChunksInfo, idx, NumberOfChunks);
            m_transfer = Transfer::Download;
            m_attempts = 0;
            downloadFileFromChunk();
        }
        else if (parts.size() >= 1 && parts[0] == "ERROR" && m_waitingForReallocation) {
            failTransfer("Master could not re-route the chunk: " + pkt);
        }
    }
}

void Client::getChunkInfos(const QString& pkt, QVector<ChunkServerInfo>& infos, int idx, int numChunks) {
    auto parts = pkt.split(' ', Qt::SkipEmptyParts);
    // Each chunk is described by three fields: chunk id, ip and port.
    for (int i = 0; i < numChunks && idx + 2 < parts.size(); ++i) {
        QString cid = parts[idx++];
        QHostAddress ip(parts[idx++]);
        quint16 port = parts[idx++].toUShort();
        infos.append({cid, ip, port});
    }
}

void Client::sendDatagram(const QByteArray& datagram, const QHostAddress& ip, quint16 port) {
    if (m_holePunching) {
        const QString key = ip.toString() + ":" + QString::number(port);
        if (!m_punchedServers.contains(key)) {
            m_udp->writeDatagram(QByteArray("PING\n"), ip, port);
            m_punchedServers.insert(key);
        }
    }
    m_udp->writeDatagram(datagram, ip, port);
}

void Client::uploadFileToChunk() {
    if (m_uploadChunks.isEmpty())
        return;

    if (m_currentChunk >= m_uploadChunks.size()) {
        m_retryTimer.stop();
        m_transfer = Transfer::None;
        m_file.close();
        emit uploadFinished(m_fileId);
        return;
    }

    auto& info = m_uploadChunks[m_currentChunk];
    m_file.seek(qint64(m_currentChunk) * CHUNK_SIZE);
    QByteArray data = m_file.read(CHUNK_SIZE);
    QByteArray encodedData = encodeChunk(data);

    QString header = QString("STORE %1 %2\n").arg(info.chunkId).arg(encodedData.size());
    QByteArray pkt = header.toUtf8() + encodedData;

    ++m_attempts;
    sendDatagram(pkt, info.ip, info.port);
    m_retryTimer.start();

    emit logReceived(QString("Sent %1 → %2:%3%4").arg(info.chunkId).arg(info.ip.toString()).arg(info.port)
                         .arg(m_attempts > 1 ? QString(" (attempt %1)").arg(m_attempts) : QString()));
}

void Client::downloadFileFromChunk() {
    if (m_downloadChunksInfo.isEmpty() || m_downloadCurrent >= m_downloadChunks)
        return;

    auto& info = m_downloadChunksInfo[m_downloadCurrent];
    QString header = QString("RETRIEVE %1\n").arg(info.chunkId);

    ++m_attempts;
    sendDatagram(header.toUtf8(), info.ip, info.port);
    m_retryTimer.start();

    emit logReceived(QString("Requested %1 → %2:%3%4").arg(info.chunkId).arg(info.ip.toString()).arg(info.port)
                         .arg(m_attempts > 1 ? QString(" (attempt %1)").arg(m_attempts) : QString()));
}

void Client::onRetryTimeout() {
    retryCurrentChunk("no reply");
}

void Client::retryCurrentChunk(const QString& reason) {
    if (m_transfer == Transfer::Upload) {
        if (m_waitingForReallocation)
            return;
        const QString cid = m_uploadChunks[m_currentChunk].chunkId;
        if (m_attempts < MAX_ATTEMPTS) {
            emit logReceived(QString("%1: %2, sending again").arg(cid, reason));
            uploadFileToChunk();
        } else {
            requestReallocation();
        }
    }
    else if (m_transfer == Transfer::Download) {
        const QString cid = m_downloadChunksInfo[m_downloadCurrent].chunkId;
        // A chunk is stored on one server only, so a download cannot be re-routed;
        // it gets more attempts instead.
        if (m_attempts < MAX_DOWNLOAD_ATTEMPTS) {
            emit logReceived(QString("%1: %2, requesting again").arg(cid, reason));
            downloadFileFromChunk();
        } else {
            failTransfer(QString("Could not download %1 after %2 attempts (%3)").arg(cid).arg(m_attempts).arg(reason));
        }
    }
}

void Client::requestReallocation() {
    const QString cid = m_uploadChunks[m_currentChunk].chunkId;
    if (m_reallocations >= MAX_REALLOCATIONS) {
        failTransfer(QString("Could not store %1 on %2 different chunk servers").arg(cid).arg(m_reallocations + 1));
        return;
    }
    ++m_reallocations;
    m_waitingForReallocation = true;
    m_retryTimer.stop();
    emit logReceived(QString("%1 failed %2 times, asking the master for another chunk server").arg(cid).arg(m_attempts));
    m_tcp->write(QString("REALLOCATE_CHUNK %1 %2\n").arg(m_fileId, cid).toUtf8());
}

void Client::failTransfer(const QString& reason) {
    m_retryTimer.stop();
    m_waitingForReallocation = false;
    if (m_transfer == Transfer::Upload)
        m_file.close();
    if (m_transfer == Transfer::Download)
        m_outFile.close();
    m_transfer = Transfer::None;
    emit errorOccurred(reason);
}

void Client::onUdpReadyRead() {
    while (m_udp->hasPendingDatagrams()) {
        QByteArray dg;
        dg.resize(int(m_udp->pendingDatagramSize()));
        QHostAddress sender;
        quint16 senderPort;
        m_udp->readDatagram(dg.data(), dg.size(), &sender, &senderPort);

        int nl = dg.indexOf('\n');
        if (nl < 0)
            continue;
        QString hdr = QString::fromUtf8(dg.constData(), nl).trimmed();
        QByteArray payload = dg.mid(nl + 1);

        auto parts = hdr.split(' ', Qt::SkipEmptyParts);
        if (parts.isEmpty() || parts[0] == "PONG")
            continue;

        emit responseReceived(hdr);

        if (parts[0] == "ACK" && parts.size() >= 4) {
            if (m_transfer != Transfer::Upload || m_waitingForReallocation)
                continue;
            auto& info = m_uploadChunks[m_currentChunk];
            QString cid = parts[1];
            if (cid != info.chunkId)
                continue; // late reply to an earlier attempt

            QString nip = parts[2];
            quint16 npt = parts[3].toUShort();
            bool corrupt = (parts.size() > 4 && parts[4] == "1");
            emit chunkAckReceived(cid, nip, npt, corrupt);

            if (corrupt) {
                m_retryTimer.stop();
                retryCurrentChunk("arrived corrupted");
                continue;
            }

            info.ip = QHostAddress(nip);
            info.port = npt;

            m_retryTimer.stop();
            emit uploadProgress(m_currentChunk + 1, m_uploadChunks.size());
            ++m_currentChunk;
            m_attempts = 0;
            m_reallocations = 0;
            uploadFileToChunk();
        }
        else if (parts[0] == "DATA" && parts.size() >= 4) {
            if (m_transfer != Transfer::Download)
                continue;
            QString cid = parts[1];
            if (cid != m_downloadChunksInfo[m_downloadCurrent].chunkId)
                continue; // late reply to an earlier attempt

            int len = parts[3].toInt();
            QByteArray encodedData = payload.left(len);
            bool corrupt = false;
            int correctedBytes = 0;
            QByteArray data = decodeChunk(encodedData, corrupt, &correctedBytes);
            if (parts[2] == "1")
                corrupt = true;

            emit chunkDataReceived(cid, data, corrupt);
            m_retryTimer.stop();

            if (corrupt) {
                retryCurrentChunk("arrived corrupted");
                continue;
            }
            if (correctedBytes > 0)
                emit logReceived(QString("%1: corrected %2 bytes").arg(cid).arg(correctedBytes));

            m_outFile.write(data);
            emit downloadProgress(m_downloadCurrent + 1, m_downloadChunks);
            ++m_downloadCurrent;
            m_attempts = 0;
            if (m_downloadCurrent < m_downloadChunks)
                downloadFileFromChunk();
            else {
                m_transfer = Transfer::None;
                m_outFile.close();
                QFileInfo fi(m_outFile.fileName());
                emit downloadFinished(m_downloadId, fi.absoluteFilePath(), fi.size());
            }
        }
        else if (parts[0] == "ERROR" && m_transfer == Transfer::Download) {
            failTransfer("Chunk server error: " + hdr);
        }
    }
}


QString Client::parentDirectory() {
    QFileInfo fi(__FILE__);
    return fi.absolutePath().left(fi.absolutePath().lastIndexOf('/'));
}
