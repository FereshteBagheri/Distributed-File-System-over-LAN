#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QHostAddress>

#include "chunkServer.h"

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("dfs-chunk");
    QCoreApplication::setApplicationVersion("1.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("Distributed File System chunk servers");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption noiseOption(QStringList() << "n" << "noise",
        "Probability that each bit is flipped in transit, to simulate a noisy network "
        "(default: 0.001; 0 disables the noise).",
        "rate", "0.001");
    parser.addOption(noiseOption);
    parser.process(app);

    bool ok = false;
    const double noiseRate = parser.value(noiseOption).toDouble(&ok);
    if (!ok || noiseRate < 0.0 || noiseRate > 1.0) {
        qCritical() << "Invalid noise rate; expected a number between 0 and 1.";
        return 1;
    }
    qInfo() << "Simulated bit error rate:" << noiseRate;

    QHostAddress localIp = QHostAddress::LocalHost;
    const int numChunkServers = 15;
    for (int i = 0; i < numChunkServers; ++i) {
        ChunkServer* srv = new ChunkServer(i, localIp, noiseRate, &app);
        srv->start();
    }

    return app.exec();
}
