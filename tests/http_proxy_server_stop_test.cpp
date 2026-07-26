#include "core/http_proxy_server.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QMap>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QtTest>

#include <functional>

using namespace net_tunnel;

static bool waitUntil(const std::function<bool()> &predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        if (predicate()) {
            return true;
        }
        QTest::qWait(10);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    return predicate();
}

static int reserveFreePort()
{
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        return 0;
    }
    const int port = int(server.serverPort());
    server.close();
    return port;
}

class ControlledUpstream : public QObject {
    Q_OBJECT

public:
    explicit ControlledUpstream(QObject *parent = 0)
        : QObject(parent), holdResponse_(true), requestCount_(0), disconnectedCount_(0)
    {
        connect(&server_, &QTcpServer::newConnection, this, &ControlledUpstream::acceptConnections);
    }

    bool start(bool holdResponse, const QByteArray &marker = QByteArray())
    {
        holdResponse_ = holdResponse;
        marker_ = marker;
        requestCount_ = 0;
        disconnectedCount_ = 0;
        lastAuthorization_.clear();
        return server_.listen(QHostAddress::LocalHost, 0);
    }

    int port() const { return int(server_.serverPort()); }
    int requestCount() const { return requestCount_; }
    int disconnectedCount() const { return disconnectedCount_; }
    QByteArray lastAuthorization() const { return lastAuthorization_; }

private slots:
    void acceptConnections()
    {
        while (server_.hasPendingConnections()) {
            QTcpSocket *socket = server_.nextPendingConnection();
            socket->setParent(this);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                QByteArray request = socket->property("request").toByteArray();
                request.append(socket->readAll());
                socket->setProperty("request", request);
                if (socket->property("handled").toBool()) {
                    return;
                }

                const int headerEnd = request.indexOf("\r\n\r\n");
                if (headerEnd < 0) {
                    return;
                }
                int contentLength = 0;
                QMap<QByteArray, QByteArray> headers;
                const QList<QByteArray> lines = request.left(headerEnd).split('\n');
                for (int i = 1; i < lines.size(); ++i) {
                    const QByteArray line = lines.at(i).trimmed();
                    const int colon = line.indexOf(':');
                    if (colon <= 0) {
                        continue;
                    }
                    const QByteArray name = line.left(colon).trimmed().toLower();
                    const QByteArray value = line.mid(colon + 1).trimmed();
                    headers.insert(name, value);
                    if (name == "content-length") {
                        contentLength = value.toInt();
                    }
                }
                if (request.size() < headerEnd + 4 + contentLength) {
                    return;
                }

                socket->setProperty("handled", true);
                ++requestCount_;
                lastAuthorization_ = headers.value("authorization");
                if (holdResponse_) {
                    return;
                }

                const QByteArray body = "{\"profile\":\"" + marker_ + "\"}";
                QByteArray response = "HTTP/1.1 200 OK\r\n";
                response += "Content-Type: application/json\r\n";
                response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
                response += "Connection: close\r\n\r\n";
                response += body;
                socket->write(response);
                socket->flush();
                socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
                ++disconnectedCount_;
                socket->deleteLater();
            });
        }
    }

private:
    QTcpServer server_;
    bool holdResponse_;
    QByteArray marker_;
    int requestCount_;
    int disconnectedCount_;
    QByteArray lastAuthorization_;
};

static ProxySettings settingsFor(int listenPort,
                                 const ControlledUpstream &upstream,
                                 const QString &apiKey)
{
    ProxySettings settings;
    settings.listenHost = "127.0.0.1";
    settings.listenPort = listenPort;
    settings.proxyPrefix = "/v1";
    settings.upstreamBaseUrl = QString("http://127.0.0.1:%1/v1").arg(upstream.port());
    settings.upstreamApiKey = apiKey;
    settings.upstreamTimeoutSec = 60;
    settings.firstTokenTimeoutSec = 0;
    settings.bufferTimeoutSec = 60;
    settings.guardRetryAttempts = 0;
    return settings;
}

static QByteArray postRequest()
{
    const QByteArray body = "{\"stream\":false}";
    QByteArray request = "POST /v1/responses HTTP/1.1\r\n";
    request += "Host: 127.0.0.1\r\n";
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n";
    request += body;
    return request;
}

static QByteArray sendRequest(int port)
{
    QTcpSocket socket;
    QByteArray response;
    QObject::connect(&socket, &QTcpSocket::readyRead, [&socket, &response]() {
        response.append(socket.readAll());
    });
    socket.connectToHost(QHostAddress::LocalHost, quint16(port));
    if (!waitUntil([&socket]() { return socket.state() == QAbstractSocket::ConnectedState; }, 1000)) {
        return QByteArray("CONNECT_FAILED");
    }
    socket.write(postRequest());
    socket.flush();
    waitUntil([&socket]() { return socket.state() == QAbstractSocket::UnconnectedState; }, 3000);
    response.append(socket.readAll());
    return response;
}

class HttpProxyServerStopTest : public QObject {
    Q_OBJECT

private slots:
    void destructorAbortsInflightRequest()
    {
        ControlledUpstream upstream;
        QVERIFY(upstream.start(true));

        const int proxyPort = reserveFreePort();
        QVERIFY(proxyPort > 0);
        HttpProxyServer *proxy = new HttpProxyServer;
        QString error;
        QVERIFY2(proxy->start(settingsFor(proxyPort, upstream, "destructor-key"), &error),
                 qPrintable(error));

        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, quint16(proxyPort));
        QVERIFY(waitUntil([&client]() {
            return client.state() == QAbstractSocket::ConnectedState;
        }, 1000));
        client.write(postRequest());
        client.flush();
        QVERIFY(waitUntil([&upstream]() { return upstream.requestCount() == 1; }, 1000));

        delete proxy;
        QVERIFY(waitUntil([&client]() {
            return client.state() == QAbstractSocket::UnconnectedState;
        }, 1000));
        QVERIFY(waitUntil([&upstream]() { return upstream.disconnectedCount() == 1; }, 1000));
    }

    void stopAbortsInflightRequestBeforeRestartingWithAnotherProfile()
    {
        ControlledUpstream oldUpstream;
        ControlledUpstream newUpstream;
        QVERIFY(oldUpstream.start(true));
        QVERIFY(newUpstream.start(false, "new-profile"));

        const int proxyPort = reserveFreePort();
        QVERIFY(proxyPort > 0);

        HttpProxyServer proxy;
        QSignalSpy stoppedSpy(&proxy, SIGNAL(stopped()));
        QString error;
        const ProxySettings oldSettings = settingsFor(proxyPort, oldUpstream, "old-key");
        QVERIFY2(proxy.start(oldSettings, &error), qPrintable(error));

        QTcpSocket oldClient;
        oldClient.connectToHost(QHostAddress::LocalHost, quint16(proxyPort));
        QVERIFY(waitUntil([&oldClient]() {
            return oldClient.state() == QAbstractSocket::ConnectedState;
        }, 1000));
        oldClient.write(postRequest());
        oldClient.flush();
        QVERIFY(waitUntil([&oldUpstream]() { return oldUpstream.requestCount() == 1; }, 1000));
        QCOMPARE(oldUpstream.lastAuthorization(), QByteArray("Bearer old-key"));

        proxy.stop();
        QCOMPARE(stoppedSpy.count(), 1);
        QVERIFY(!proxy.isRunning());
        const QJsonObject stoppedRuntime = proxy.statusPayload().value("runtime").toObject();
        QCOMPARE(stoppedRuntime.value("completed_proxy_requests_total").toInt(), 1);
        QCOMPARE(stoppedRuntime.value("in_flight_proxy_requests").toInt(), 0);
        QCOMPARE(stoppedRuntime.value("failed_requests_total").toInt(), 1);
        QCOMPARE(stoppedRuntime.value("local_proxy_error_total").toInt(), 1);
        QCOMPARE(stoppedRuntime.value("client_connection_error_total").toInt(), 0);
        QCOMPARE(stoppedRuntime.value("last_failure").toObject().value("error_type").toString(),
                 QString("proxy_stopped"));

        const ProxySettings newSettings = settingsFor(proxyPort, newUpstream, "new-key");
        error.clear();
        QVERIFY2(proxy.start(newSettings, &error), qPrintable(error));
        QVERIFY(waitUntil([&oldClient]() {
            return oldClient.state() == QAbstractSocket::UnconnectedState;
        }, 1000));
        QVERIFY(waitUntil([&oldUpstream]() { return oldUpstream.disconnectedCount() == 1; }, 1000));

        const QByteArray response = sendRequest(proxyPort);
        QVERIFY2(response.contains("HTTP/1.1 200 OK"), response.constData());
        QVERIFY2(response.contains("\"profile\":\"new-profile\""), response.constData());
        QCOMPARE(newUpstream.requestCount(), 1);
        QCOMPARE(newUpstream.lastAuthorization(), QByteArray("Bearer new-key"));

        QTest::qWait(100);
        QCOMPARE(oldUpstream.requestCount(), 1);
        proxy.stop();
        QCOMPARE(stoppedSpy.count(), 2);
    }
};

QTEST_MAIN(HttpProxyServerStopTest)

#include "http_proxy_server_stop_test.moc"
