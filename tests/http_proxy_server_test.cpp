#include "core/http_proxy_server.h"
#include "core/app_config.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMap>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
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

class TestUpstream : public QObject {
    Q_OBJECT

public:
    enum Mode {
        HoldOpen,
        LargeResponse,
        JsonResponse,
        StreamingSse
    };

    explicit TestUpstream(QObject *parent = 0)
        : QObject(parent),
          mode_(HoldOpen),
          requestCount_(0),
          disconnectedCount_(0),
          streamChunkDelayMs_(25)
    {
        connect(&server_, SIGNAL(newConnection()), this, SLOT(acceptConnections()));
    }

    bool start(Mode mode,
               const QByteArray &responseBody = QByteArray(),
               const QList<QByteArray> &streamChunks = QList<QByteArray>())
    {
        mode_ = mode;
        responseBody_ = responseBody;
        streamChunks_ = streamChunks;
        responseBodySequences_.clear();
        responseStatusSequences_.clear();
        streamChunkSequences_.clear();
        streamFirstChunkDelaySequencesMs_.clear();
        lastPath_.clear();
        lastBody_.clear();
        lastAuthorization_.clear();
        requestCount_ = 0;
        disconnectedCount_ = 0;
        return server_.listen(QHostAddress::LocalHost, 0);
    }

    bool startJsonResponses(const QList<QByteArray> &responseBodySequences,
                            const QList<int> &responseStatusSequences = QList<int>())
    {
        mode_ = JsonResponse;
        responseBody_.clear();
        responseBodySequences_ = responseBodySequences;
        responseStatusSequences_ = responseStatusSequences;
        streamChunks_.clear();
        streamChunkSequences_.clear();
        streamFirstChunkDelaySequencesMs_.clear();
        lastPath_.clear();
        lastBody_.clear();
        lastAuthorization_.clear();
        requestCount_ = 0;
        disconnectedCount_ = 0;
        return server_.listen(QHostAddress::LocalHost, 0);
    }

    bool startStreamingSequences(const QList<QList<QByteArray> > &streamChunkSequences,
                                 const QList<int> &firstChunkDelaySequencesMs = QList<int>())
    {
        mode_ = StreamingSse;
        responseBody_.clear();
        responseBodySequences_.clear();
        responseStatusSequences_.clear();
        streamChunks_.clear();
        streamChunkSequences_ = streamChunkSequences;
        streamFirstChunkDelaySequencesMs_ = firstChunkDelaySequencesMs;
        lastPath_.clear();
        lastBody_.clear();
        lastAuthorization_.clear();
        requestCount_ = 0;
        disconnectedCount_ = 0;
        return server_.listen(QHostAddress::LocalHost, 0);
    }

    int port() const
    {
        return int(server_.serverPort());
    }

    int requestCount() const
    {
        return requestCount_;
    }

    int disconnectedCount() const
    {
        return disconnectedCount_;
    }

    QString lastPath() const
    {
        return lastPath_;
    }

    QByteArray lastBody() const
    {
        return lastBody_;
    }

    QByteArray lastAuthorization() const
    {
        return lastAuthorization_;
    }

signals:
    void requestReceived();

private slots:
    void acceptConnections()
    {
        while (server_.hasPendingConnections()) {
            QTcpSocket *socket = server_.nextPendingConnection();
            socket->setParent(this);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                QByteArray buffer = socket->property("request_buffer").toByteArray();
                buffer.append(socket->readAll());
                socket->setProperty("request_buffer", buffer);
                if (socket->property("handled").toBool()) {
                    return;
                }
                int headerEnd = buffer.indexOf("\r\n\r\n");
                int terminatorLength = 4;
                if (headerEnd < 0) {
                    headerEnd = buffer.indexOf("\n\n");
                    terminatorLength = 2;
                }
                if (headerEnd < 0) {
                    return;
                }
                int contentLength = 0;
                QMap<QByteArray, QByteArray> headers;
                const QList<QByteArray> headerLines = buffer.left(headerEnd).split('\n');
                for (int i = 1; i < headerLines.size(); ++i) {
                    const QByteArray line = headerLines.at(i).trimmed();
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
                if (buffer.size() < headerEnd + terminatorLength + contentLength) {
                    return;
                }
                socket->setProperty("handled", true);
                ++requestCount_;
                const QByteArray requestLine = buffer.left(buffer.indexOf('\n')).trimmed();
                const QList<QByteArray> parts = requestLine.split(' ');
                if (parts.size() >= 2) {
                    QString path = QString::fromLatin1(parts.at(1));
                    const int question = path.indexOf('?');
                    lastPath_ = question >= 0 ? path.left(question) : path;
                }
                lastBody_ = buffer.mid(headerEnd + terminatorLength, contentLength);
                lastAuthorization_ = headers.value("authorization");
                emit requestReceived();

                if (mode_ == LargeResponse || mode_ == JsonResponse) {
                    QByteArray body = responseBody_;
                    int statusCode = 200;
                    if (!responseBodySequences_.isEmpty()) {
                        const int sequenceIndex = qMin(requestCount_ - 1, responseBodySequences_.size() - 1);
                        body = responseBodySequences_.at(sequenceIndex);
                    }
                    if (!responseStatusSequences_.isEmpty()) {
                        const int sequenceIndex = qMin(requestCount_ - 1, responseStatusSequences_.size() - 1);
                        statusCode = responseStatusSequences_.at(sequenceIndex);
                    }
                    QByteArray reason = "OK";
                    if (statusCode == 429) {
                        reason = "Too Many Requests";
                    } else if (statusCode == 502) {
                        reason = "Bad Gateway";
                    } else if (statusCode == 503) {
                        reason = "Service Unavailable";
                    }
                    QByteArray response;
                    response += "HTTP/1.1 " + QByteArray::number(statusCode) + " " + reason + "\r\n";
                    response += "Content-Type: application/json\r\n";
                    response += "X-Upstream-Attempt: " + QByteArray::number(requestCount_) + "\r\n";
                    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
                    response += "Connection: close\r\n\r\n";
                    response += body;
                    socket->write(response);
                    socket->flush();
                    socket->disconnectFromHost();
                } else if (mode_ == StreamingSse) {
                    QList<QByteArray> chunks = streamChunks_;
                    if (!streamChunkSequences_.isEmpty()) {
                        const int sequenceIndex = qMin(requestCount_ - 1, streamChunkSequences_.size() - 1);
                        chunks = streamChunkSequences_.at(sequenceIndex);
                    }
                    int firstChunkDelayMs = streamChunkDelayMs_;
                    if (!streamFirstChunkDelaySequencesMs_.isEmpty()) {
                        const int sequenceIndex = qMin(requestCount_ - 1, streamFirstChunkDelaySequencesMs_.size() - 1);
                        firstChunkDelayMs = streamFirstChunkDelaySequencesMs_.at(sequenceIndex);
                    }
                    QByteArray response;
                    response += "HTTP/1.1 200 OK\r\n";
                    response += "Content-Type: text/event-stream\r\n";
                    response += "Connection: close\r\n\r\n";
                    socket->write(response);
                    socket->flush();
                    for (int i = 0; i < chunks.size(); ++i) {
                        const QByteArray chunk = chunks.at(i);
                        const bool last = i == chunks.size() - 1;
                        QTimer::singleShot(firstChunkDelayMs + streamChunkDelayMs_ * i, socket, [socket, chunk, last]() {
                            if (socket->state() == QAbstractSocket::UnconnectedState) {
                                return;
                            }
                            socket->write(chunk);
                            socket->flush();
                            if (last) {
                                socket->disconnectFromHost();
                            }
                        });
                    }
                }
            });
            connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
                ++disconnectedCount_;
                socket->deleteLater();
            });
        }
    }

private:
    QTcpServer server_;
    Mode mode_;
    QByteArray responseBody_;
    QList<QByteArray> responseBodySequences_;
    QList<int> responseStatusSequences_;
    QList<QByteArray> streamChunks_;
    QList<QList<QByteArray> > streamChunkSequences_;
    QList<int> streamFirstChunkDelaySequencesMs_;
    QString lastPath_;
    QByteArray lastBody_;
    QByteArray lastAuthorization_;
    int requestCount_;
    int disconnectedCount_;
    int streamChunkDelayMs_;
};

static ProxySettings baseSettings(int proxyPort, int upstreamPort)
{
    ProxySettings settings;
    settings.listenHost = "127.0.0.1";
    settings.listenPort = proxyPort;
    settings.proxyPrefix = "/v1";
    settings.upstreamBaseUrl = QString("http://127.0.0.1:%1/v1").arg(upstreamPort);
    settings.upstreamTimeoutSec = 30;
    settings.bufferTimeoutSec = 1;
    settings.requestBodyLimitBytes = defaultRequestBodyLimitBytes();
    settings.responseBufferLimitBytes = defaultResponseBufferLimitBytes();
    settings.guardRetryAttempts = 0;
    return settings;
}

static QByteArray sendRequest(int port, const QByteArray &request, int timeoutMs)
{
    QTcpSocket socket;
    QByteArray response;
    bool disconnected = false;

    QObject::connect(&socket, &QTcpSocket::readyRead, [&]() {
        response.append(socket.readAll());
    });
    QObject::connect(&socket, &QTcpSocket::disconnected, [&]() {
        disconnected = true;
    });
    QObject::connect(&socket, static_cast<void (QTcpSocket::*)(QAbstractSocket::SocketError)>(&QTcpSocket::error), [&]() {
        disconnected = true;
    });

    socket.connectToHost(QHostAddress::LocalHost, quint16(port));
    if (!waitUntil([&]() { return socket.state() == QAbstractSocket::ConnectedState; }, 1000)) {
        return QByteArray("CONNECT_FAILED");
    }
    socket.write(request);
    socket.flush();
    waitUntil([&]() { return disconnected; }, timeoutMs);
    response.append(socket.readAll());
    return response;
}

static QByteArray postRequestToPath(const QByteArray &path, const QByteArray &body);
static QByteArray postRequestToPathWithHeaders(const QByteArray &path,
                                               const QByteArray &body,
                                               const QList<QPair<QByteArray, QByteArray> > &headers);

static QByteArray postRequest(const QByteArray &body)
{
    return postRequestToPath("/v1/responses", body);
}

static QByteArray postRequestToPath(const QByteArray &path, const QByteArray &body)
{
    return postRequestToPathWithHeaders(path, body, QList<QPair<QByteArray, QByteArray> >());
}

static QByteArray postRequestToPathWithHeaders(const QByteArray &path,
                                               const QByteArray &body,
                                               const QList<QPair<QByteArray, QByteArray> > &headers)
{
    QByteArray request;
    request += "POST " + path + " HTTP/1.1\r\n";
    request += "Host: 127.0.0.1\r\n";
    request += "Content-Type: application/json\r\n";
    for (int i = 0; i < headers.size(); ++i) {
        request += headers.at(i).first + ": " + headers.at(i).second + "\r\n";
    }
    request += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    request += "\r\n";
    request += body;
    return request;
}

static QByteArray getRequestToPath(const QByteArray &path)
{
    QByteArray request;
    request += "GET " + path + " HTTP/1.1\r\n";
    request += "Host: 127.0.0.1\r\n";
    request += "\r\n";
    return request;
}

static QByteArray chunkedPostRequestToPath(const QByteArray &path, const QList<QByteArray> &chunks)
{
    QByteArray request;
    request += "POST " + path + " HTTP/1.1\r\n";
    request += "Host: 127.0.0.1\r\n";
    request += "Content-Type: application/json\r\n";
    request += "Transfer-Encoding: chunked\r\n";
    request += "\r\n";
    for (int i = 0; i < chunks.size(); ++i) {
        request += QByteArray::number(chunks.at(i).size(), 16);
        request += "\r\n";
        request += chunks.at(i);
        request += "\r\n";
    }
    request += "0\r\n\r\n";
    return request;
}

static QJsonObject runtimeOf(HttpProxyServer *proxy)
{
    return proxy->statusPayload().value("runtime").toObject();
}

class HttpProxyServerTest : public QObject {
    Q_OBJECT

private slots:
    void requestBodyLimitExceeded()
    {
        TestUpstream upstream;
        QVERIFY(upstream.start(TestUpstream::HoldOpen));

        HttpProxyServer proxy;
        ProxySettings settings = baseSettings(reserveFreePort(), upstream.port());
        settings.requestBodyLimitBytes = 16;
        QString error;
        QVERIFY2(proxy.start(settings, &error), qPrintable(error));

        const QByteArray response = sendRequest(settings.listenPort, postRequest(QByteArray(32, 'x')), 2000);
        QVERIFY(response.contains("413"));
        QVERIFY(response.contains("request_body_limit_exceeded"));
        QCOMPARE(upstream.requestCount(), 0);

        const QJsonObject runtime = runtimeOf(&proxy);
        QCOMPARE(runtime.value("failed_requests_total").toInt(), 1);
        QCOMPARE(runtime.value("local_proxy_error_total").toInt(), 1);
        QCOMPARE(runtime.value("upstream_http_error_total").toInt(), 0);
        QCOMPARE(runtime.value("last_failure").toObject().value("error_type").toString(),
                 QString("request_body_limit_exceeded"));
    }

    void requestBufferTimeout()
    {
        TestUpstream upstream;
        QVERIFY(upstream.start(TestUpstream::HoldOpen));

        HttpProxyServer proxy;
        ProxySettings settings = baseSettings(reserveFreePort(), upstream.port());
        settings.bufferTimeoutSec = 1;
        QString error;
        QVERIFY2(proxy.start(settings, &error), qPrintable(error));

        QByteArray request;
        request += "POST /v1/responses HTTP/1.1\r\n";
        request += "Host: 127.0.0.1\r\n";
        request += "Content-Type: application/json\r\n";
        request += "Content-Length: 8\r\n\r\n";
        request += "abc";

        const QByteArray response = sendRequest(settings.listenPort, request, 2500);
        QVERIFY(response.contains("408"));
        QVERIFY(response.contains("buffer_timeout"));
        QCOMPARE(upstream.requestCount(), 0);

        const QJsonObject runtime = runtimeOf(&proxy);
        QCOMPARE(runtime.value("buffer_timeout_total").toInt(), 1);
        QCOMPARE(runtime.value("last_failure").toObject().value("error_type").toString(),
                 QString("buffer_timeout"));
    }

    void responseBufferLimitExceeded()
    {
        TestUpstream upstream;
        QVERIFY(upstream.start(TestUpstream::LargeResponse, QByteArray(64, 'x')));

        HttpProxyServer proxy;
        ProxySettings settings = baseSettings(reserveFreePort(), upstream.port());
        settings.responseBufferLimitBytes = 16;
        QString error;
        QVERIFY2(proxy.start(settings, &error), qPrintable(error));

        const QByteArray response = sendRequest(settings.listenPort, postRequest("{}"), 2500);
        QVERIFY(response.contains("502"));
        QVERIFY(response.contains("response_buffer_limit_exceeded"));
        QCOMPARE(upstream.requestCount(), 1);

        const QJsonObject runtime = runtimeOf(&proxy);
        QCOMPARE(runtime.value("failed_requests_total").toInt(), 1);
        QCOMPARE(runtime.value("local_proxy_error_total").toInt(), 1);
        QCOMPARE(runtime.value("upstream_http_error_total").toInt(), 0);
        QCOMPARE(runtime.value("last_failure").toObject().value("error_type").toString(),
                 QString("response_buffer_limit_exceeded"));
    }

    void chunkedRequestBodyIsDecodedAndForwarded()
    {
        TestUpstream upstream;
        QVERIFY(upstream.start(TestUpstream::JsonResponse, "{\"ok\":true}"));

        HttpProxyServer proxy;
        ProxySettings settings = baseSettings(reserveFreePort(), upstream.port());
        QString error;
        QVERIFY2(proxy.start(settings, &error), qPrintable(error));

        QList<QByteArray> chunks;
        chunks << "{\"stream\":" << "false}";
        const QByteArray response = sendRequest(settings.listenPort,
                                                chunkedPostRequestToPath("/v1/responses", chunks),
                                                2500);
        QVERIFY(response.contains("200 OK"));
        QCOMPARE(upstream.requestCount(), 1);
        QCOMPARE(upstream.lastBody(), QByteArray("{\"stream\":false}"));
    }

    void chunkedRequestBodyLimitUsesDecodedSize()
    {
        TestUpstream upstream;
        QVERIFY(upstream.start(TestUpstream::JsonResponse, "{\"ok\":true}"));

        HttpProxyServer proxy;
        ProxySettings settings = baseSettings(reserveFreePort(), upstream.port());
        settings.requestBodyLimitBytes = 2;
        QString error;
        QVERIFY2(proxy.start(settings, &error), qPrintable(error));

        QList<QByteArray> chunks;
        chunks << "{}";
        const QByteArray response = sendRequest(settings.listenPort,
                                                chunkedPostRequestToPath("/v1/responses", chunks),
                                                2500);
        QVERIFY(response.contains("200 OK"));
        QCOMPARE(upstream.requestCount(), 1);
        QCOMPARE(upstream.lastBody(), QByteArray("{}"));
    }

    void invalidChunkedRequestIsLocalBadRequest()
    {
        TestUpstream upstream;
        QVERIFY(upstream.start(TestUpstream::JsonResponse, "{\"ok\":true}"));

        HttpProxyServer proxy;
        ProxySettings settings = baseSettings(reserveFreePort(), upstream.port());
        QString error;
        QVERIFY2(proxy.start(settings, &error), qPrintable(error));

        QByteArray request;
        request += "POST /v1/responses HTTP/1.1\r\n";
        request += "Host: 127.0.0.1\r\n";
        request += "Content-Type: application/json\r\n";
        request += "Transfer-Encoding: chunked\r\n\r\n";
        request += "z\r\n{}\r\n0\r\n\r\n";

        const QByteArray response = sendRequest(settings.listenPort, request, 2500);
        QVERIFY(response.contains("400"));
        QVERIFY(response.contains("bad_request"));
        QCOMPARE(upstream.requestCount(), 0);

        const QJsonObject runtime = runtimeOf(&proxy);
        QCOMPARE(runtime.value("local_proxy_error_total").toInt(), 1);
        QCOMPARE(runtime.value("upstream_http_error_total").toInt(), 0);
        QCOMPARE(runtime.value("last_failure").toObject().value("error_type").toString(),
                 QString("bad_request"));
    }

    void configuredApiKeyOverridesIncomingAuthorization()
    {
        TestUpstream upstream;
        QVERIFY(upstream.start(TestUpstream::JsonResponse, "{\"ok\":true}"));

        HttpProxyServer proxy;
        ProxySettings settings = baseSettings(reserveFreePort(), upstream.port());
        settings.upstreamApiKey = "config-key";
        QString error;
        QVERIFY2(proxy.start(settings, &error), qPrintable(error));

        const QByteArray body = "{\"stream\":false}";
        QByteArray request;
        request += "POST /v1/responses HTTP/1.1\r\n";
        request += "Host: 127.0.0.1\r\n";
        request += "Content-Type: application/json\r\n";
        request += "Authorization: Bearer auth-json-key\r\n";
        request += "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n";
        request += body;

        const QByteArray response = sendRequest(settings.listenPort, request, 2500);
        QVERIFY(response.contains("200 OK"));
        QCOMPARE(upstream.lastAuthorization(), QByteArray("Bearer config-key"));
    }

    void emptyConfiguredApiKeyForwardsIncomingAuthorization()
    {
        TestUpstream upstream;
        QVERIFY(upstream.start(TestUpstream::JsonResponse, "{\"ok\":true}"));

        HttpProxyServer proxy;
        ProxySettings settings = baseSettings(reserveFreePort(), upstream.port());
        settings.upstreamApiKey.clear();
        QString error;
        QVERIFY2(proxy.start(settings, &error), qPrintable(error));

        const QByteArray body = "{\"stream\":false}";
        QByteArray request;
        request += "POST /v1/responses HTTP/1.1\r\n";
        request += "Host: 127.0.0.1\r\n";
        request += "Content-Type: application/json\r\n";
        request += "Authorization: Bearer auth-json-key\r\n";
        request += "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n";
        request += body;

        const QByteArray response = sendRequest(settings.listenPort, request, 2500);
        QVERIFY(response.contains("200 OK"));
        QCOMPARE(upstream.lastAuthorization(), QByteArray("Bearer auth-json-key"));
    }

    void upstreamTimeoutHasDedicatedErrorType()
    {
        TestUpstream upstream;
        QVERIFY(upstream.start(TestUpstream::HoldOpen));

        HttpProxyServer proxy;
        ProxySettings settings = baseSettings(reserveFreePort(), upstream.port());
        settings.upstreamTimeoutSec = 1;
        settings.bufferTimeoutSec = 5;
        QString error;
        QVERIFY2(proxy.start(settings, &error), qPrintable(error));

        const QByteArray response = sendRequest(settings.listenPort, postRequest("{}"), 2500);
        QVERIFY(response.contains("504"));
        QVERIFY(response.contains("upstream_timeout"));
        QCOMPARE(upstream.requestCount(), 1);

        const QJsonObject runtime = runtimeOf(&proxy);
        QCOMPARE(runtime.value("failed_requests_total").toInt(), 1);
        QCOMPARE(runtime.value("upstream_timeout_total").toInt(), 1);
        QCOMPARE(runtime.value("proxy_error_total").toInt(), 0);
        QCOMPARE(runtime.value("upstream_http_error_total").toInt(), 0);
        QCOMPARE(runtime.value("last_failure").toObject().value("error_type").toString(),
                 QString("upstream_timeout"));
    }

    void firstTokenTimeoutRetriesStreamingAttempt()
    {
        QList<QByteArray> delayedChunks;
        delayedChunks << "data: {\"type\":\"response.output_text.delta\",\"delta\":\"stale\"}\n\n";
        delayedChunks << "data: {\"type\":\"response.completed\",\"response\":{\"usage\":{\"output_tokens_details\":{\"reasoning_tokens\":367}}}}\n\n";

        QList<QByteArray> recoveredChunks;
        recoveredChunks << "data: {\"type\":\"response.output_text.delta\",\"delta\":\"recovered\"}\n\n";
        recoveredChunks << "data: {\"type\":\"response.completed\",\"response\":{\"usage\":{\"output_tokens_details\":{\"reasoning_tokens\":367}}}}\n\n";
        recoveredChunks << "data: [DONE]\n\n";

        QList<QList<QByteArray> > sequences;
        sequences << delayedChunks << recoveredChunks;
        QList<int> firstChunkDelays;
        firstChunkDelays << 1500 << 25;
        TestUpstream upstream;
        QVERIFY(upstream.startStreamingSequences(sequences, firstChunkDelays));

        HttpProxyServer proxy;
        ProxySettings settings = baseSettings(reserveFreePort(), upstream.port());
        settings.firstTokenTimeoutSec = 1;
        settings.bufferTimeoutSec = 5;
        settings.guardRetryAttempts = 1;
        QString error;
        QVERIFY2(proxy.start(settings, &error), qPrintable(error));

        const QByteArray response = sendRequest(settings.listenPort, postRequest("{\"stream\":true}"), 5000);
        QVERIFY(response.contains("200 OK"));
        QVERIFY(response.contains("recovered"));
        QVERIFY(!response.contains("stale"));
        QCOMPARE(upstream.requestCount(), 2);

        const QJsonObject runtime = runtimeOf(&proxy);
        QCOMPARE(runtime.value("first_token_timeout_total").toInt(), 1);
        QCOMPARE(runtime.value("first_token_timeout_retry_total").toInt(), 1);
        QCOMPARE(runtime.value("upstream_attempts_total").toInt(), 2);
        QCOMPARE(runtime.value("successful_requests_total").toInt(), 1);
        QCOMPARE(runtime.value("failed_requests_total").toInt(), 0);
    }

    void firstTokenTimeoutReturns504WhenRetryExhausted()
    {
        QList<QByteArray> chunks;
        chunks << "data: {\"type\":\"response.output_text.delta\",\"delta\":\"too late\"}\n\n";
        QList<QList<QByteArray> > sequences;
        sequences << chunks;
        QList<int> firstChunkDelays;
        firstChunkDelays << 1500;
        TestUpstream upstream;
        QVERIFY(upstream.startStreamingSequences(sequences, firstChunkDelays));

        HttpProxyServer proxy;
        ProxySettings settings = baseSettings(reserveFreePort(), upstream.port());
        settings.firstTokenTimeoutSec = 1;
        settings.bufferTimeoutSec = 5;
        settings.guardRetryAttempts = 0;
        QString error;
        QVERIFY2(proxy.start(settings, &error), qPrintable(error));

        const QByteArray response = sendRequest(settings.listenPort, postRequest("{\"stream\":true}"), 3000);
        QVERIFY(response.contains("504"));
        QVERIFY(response.contains("first_token_timeout"));
        QCOMPARE(upstream.requestCount(), 1);

        const QJsonObject runtime = runtimeOf(&proxy);
        QCOMPARE(runtime.value("first_token_timeout_total").toInt(), 1);
        QCOMPARE(runtime.value("first_token_timeout_retry_total").toInt(), 0);
        QCOMPARE(runtime.value("upstream_timeout_total").toInt(), 0);
        QCOMPARE(runtime.value("upstream_http_error_total").toInt(), 0);
        QCOMPARE(runtime.value("last_failure").toObject().value("error_type").toString(),
                 QString("first_token_timeout"));
    }

    void capacityRetryExhaustionForwardsFinalUpstreamError()
    {
        QList<QByteArray> bodies;
        bodies << "{\"error\":{\"message\":\"Selected model is at capacity. Please try a different model.\",\"attempt\":1}}";
        bodies << "{\"error\":{\"message\":\"Selected model is at capacity. Please try a different model.\",\"attempt\":2}}";
        QList<int> statuses;
        statuses << 503 << 503;
        TestUpstream upstream;
        QVERIFY(upstream.startJsonResponses(bodies, statuses));

        HttpProxyServer proxy;
        ProxySettings settings = baseSettings(reserveFreePort(), upstream.port());
        settings.guardRetryAttempts = 1;
        settings.retryUpstreamCapacityErrors = true;
        QString error;
        QVERIFY2(proxy.start(settings, &error), qPrintable(error));

        const QByteArray response = sendRequest(settings.listenPort, postRequest("{\"stream\":false}"), 3000);
        QVERIFY(response.contains("503 Service Unavailable"));
        QVERIFY(response.contains("X-Upstream-Attempt: 2"));
        QVERIFY(response.contains("\"attempt\":2"));
        QVERIFY(!response.contains("\"attempt\":1"));
        QCOMPARE(upstream.requestCount(), 2);

        const QJsonObject runtime = runtimeOf(&proxy);
        QCOMPARE(runtime.value("guard_retry_total").toInt(), 1);
        QCOMPARE(runtime.value("upstream_http_error_total").toInt(), 1);
        QCOMPARE(runtime.value("failed_requests_total").toInt(), 1);
        QCOMPARE(runtime.value("last_failure").toObject().value("status_code").toInt(), 503);
        QCOMPARE(runtime.value("last_failure").toObject().value("error_type").toString(),
                 QString("upstream_http_error"));
    }

    void responseBufferTimeout()
    {
        TestUpstream upstream;
        QVERIFY(upstream.start(TestUpstream::HoldOpen));

        HttpProxyServer proxy;
        ProxySettings settings = baseSettings(reserveFreePort(), upstream.port());
        settings.bufferTimeoutSec = 1;
        QString error;
        QVERIFY2(proxy.start(settings, &error), qPrintable(error));

        const QByteArray response = sendRequest(settings.listenPort, postRequest("{}"), 2500);
        QVERIFY(response.contains("502"));
        QVERIFY(response.contains("buffer_timeout"));
        QCOMPARE(upstream.requestCount(), 1);

        const QJsonObject runtime = runtimeOf(&proxy);
        QCOMPARE(runtime.value("buffer_timeout_total").toInt(), 1);
        QCOMPARE(runtime.value("last_failure").toObject().value("error_type").toString(),
                 QString("buffer_timeout"));
    }

    void clientDisconnectAbortsUpstream()
    {
        TestUpstream upstream;
        QVERIFY(upstream.start(TestUpstream::HoldOpen));

        HttpProxyServer proxy;
        ProxySettings settings = baseSettings(reserveFreePort(), upstream.port());
        settings.bufferTimeoutSec = 5;
        QString error;
        QVERIFY2(proxy.start(settings, &error), qPrintable(error));

        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, quint16(settings.listenPort));
        QVERIFY(waitUntil([&]() { return client.state() == QAbstractSocket::ConnectedState; }, 1000));
        client.write(postRequest("{}"));
        client.flush();
        QVERIFY(waitUntil([&]() { return upstream.requestCount() == 1; }, 1000));

        client.abort();
        QVERIFY(waitUntil([&]() {
            return runtimeOf(&proxy).value("client_connection_error_total").toInt() == 1;
        }, 2000));
        QVERIFY(waitUntil([&]() { return upstream.disconnectedCount() >= 1; }, 3000));

        const QJsonObject runtime = runtimeOf(&proxy);
        QCOMPARE(runtime.value("last_failure").toObject().value("error_type").toString(),
                 QString("client_connection_error"));
    }

    void emptyProxyPrefixUsesRoot()
    {
        TestUpstream upstream;
        QVERIFY(upstream.start(TestUpstream::JsonResponse, "{\"ok\":true}"));

        HttpProxyServer proxy;
        ProxySettings settings = baseSettings(reserveFreePort(), upstream.port());
        settings.proxyPrefix = "";
        QString error;
        QVERIFY2(proxy.start(settings, &error), qPrintable(error));
        QCOMPARE(proxy.listenUrl(), QString("http://127.0.0.1:%1/").arg(settings.listenPort));

        const QByteArray response = sendRequest(settings.listenPort, postRequestToPath("/responses", "{}"), 2500);
        QVERIFY(response.contains("200 OK"));
        QCOMPARE(upstream.requestCount(), 1);
        QCOMPARE(upstream.lastPath(), QString("/v1/responses"));
        QCOMPARE(proxy.statusPayload().value("proxy_prefix").toString(), QString("/"));
    }

    void emptyProxyPrefixForwardsRootPath()
    {
        TestUpstream upstream;
        QVERIFY(upstream.start(TestUpstream::JsonResponse, "{\"ok\":true}"));

        HttpProxyServer proxy;
        ProxySettings settings = baseSettings(reserveFreePort(), upstream.port());
        settings.proxyPrefix = "";
        settings.upstreamBaseUrl = QString("http://127.0.0.1:%1").arg(upstream.port());
        QString error;
        QVERIFY2(proxy.start(settings, &error), qPrintable(error));

        const QByteArray response = sendRequest(settings.listenPort, getRequestToPath("/"), 2500);
        QVERIFY(response.contains("200 OK"));
        QCOMPARE(upstream.requestCount(), 1);
        QCOMPARE(upstream.lastPath(), QString("/"));
    }

    void emptyProxyPrefixKeepsExplicitHealthEndpoint()
    {
        TestUpstream upstream;
        QVERIFY(upstream.start(TestUpstream::JsonResponse, "{\"ok\":true}"));

        HttpProxyServer proxy;
        ProxySettings settings = baseSettings(reserveFreePort(), upstream.port());
        settings.proxyPrefix = "";
        settings.upstreamBaseUrl = QString("http://127.0.0.1:%1").arg(upstream.port());
        QString error;
        QVERIFY2(proxy.start(settings, &error), qPrintable(error));

        const QByteArray response = sendRequest(settings.listenPort, getRequestToPath("/healthz"), 2500);
        QVERIFY(response.contains("200 OK"));
        QVERIFY(response.contains("\"ok\":true"));
        QCOMPARE(upstream.requestCount(), 0);
    }

    void customProxyPrefixStillMatchesGuardEndpoint()
    {
        const QByteArray guardedBody =
            "{\"usage\":{\"output_tokens_details\":{\"reasoning_tokens\":516}}}";
        TestUpstream upstream;
        QVERIFY(upstream.start(TestUpstream::JsonResponse, guardedBody));

        HttpProxyServer proxy;
        ProxySettings settings = baseSettings(reserveFreePort(), upstream.port());
        settings.proxyPrefix = "/api";
        settings.guardRetryAttempts = 0;
        QString error;
        QVERIFY2(proxy.start(settings, &error), qPrintable(error));

        const QByteArray response = sendRequest(settings.listenPort, postRequestToPath("/api/responses", "{}"), 2500);
        QVERIFY(response.contains("502"));
        QVERIFY(response.contains("reasoning_guard_triggered"));
        QCOMPARE(upstream.requestCount(), 1);
        QCOMPARE(upstream.lastPath(), QString("/v1/responses"));

        const QJsonObject runtime = runtimeOf(&proxy);
        QCOMPARE(runtime.value("blocked_non_streaming_count").toInt(), 1);
        QCOMPARE(runtime.value("last_failure").toObject().value("error_type").toString(),
                 QString("reasoning_guard_triggered"));
    }

    void finalAnswerOnlyHighXhighIgnoresZeroReasoning()
    {
        const QByteArray finalOnlyZeroBody =
            "{\"usage\":{\"output_tokens_details\":{\"reasoning_tokens\":0}},"
            "\"output\":[{\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"visible final answer\"}]}]}";
        TestUpstream upstream;
        QVERIFY(upstream.start(TestUpstream::JsonResponse, finalOnlyZeroBody));

        HttpProxyServer proxy;
        ProxySettings settings = baseSettings(reserveFreePort(), upstream.port());
        settings.interceptRuleMode = "final_answer_only_high_xhigh";
        settings.guardRetryAttempts = 0;
        QString error;
        QVERIFY2(proxy.start(settings, &error), qPrintable(error));

        const QByteArray response = sendRequest(settings.listenPort,
                                                postRequest("{\"reasoning\":{\"effort\":\"high\"}}"),
                                                2500);
        QVERIFY(response.contains("200 OK"));
        QVERIFY(!response.contains("reasoning_guard_triggered"));
        QCOMPARE(upstream.requestCount(), 1);

        const QJsonObject runtime = runtimeOf(&proxy);
        QCOMPARE(runtime.value("matched_non_streaming_count").toInt(), 0);
        QCOMPARE(runtime.value("failed_requests_total").toInt(), 0);
        const QJsonObject lastResult = runtime.value("last_result").toObject();
        QCOMPARE(lastResult.value("reasoning_tokens").toInt(), 0);
        QCOMPARE(lastResult.value("request_kind").toString(), QString("normal"));
        QVERIFY(!lastResult.contains("intercept_exempt_reason"));
    }

    void contextCompactionZeroExemptionIsRecordedAndLogged()
    {
        const QByteArray finalOnlyZeroBody =
            "{\"usage\":{\"output_tokens_details\":{\"reasoning_tokens\":0}},"
            "\"output\":[{\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"compact summary\"}]}]}";
        TestUpstream upstream;
        QVERIFY(upstream.start(TestUpstream::JsonResponse, finalOnlyZeroBody));

        QStringList logs;
        HttpProxyServer proxy;
        QObject::connect(&proxy, &HttpProxyServer::logLine, [&](const QString &line) {
            logs.append(line);
        });
        ProxySettings settings = baseSettings(reserveFreePort(), upstream.port());
        settings.interceptRuleMode = "final_answer_only_high_xhigh";
        settings.guardRetryAttempts = 0;
        QString error;
        QVERIFY2(proxy.start(settings, &error), qPrintable(error));

        QList<QPair<QByteArray, QByteArray> > headers;
        headers.append(qMakePair(QByteArray("X-Codex-Beta-Features"), QByteArray("remote_compaction_v2")));
        headers.append(qMakePair(QByteArray("X-Codex-Request-Kind"), QByteArray("context_compaction")));
        const QByteArray response = sendRequest(settings.listenPort,
                                                postRequestToPathWithHeaders("/v1/responses",
                                                                             "{\"reasoning\":{\"effort\":\"xhigh\"}}",
                                                                             headers),
                                                2500);
        QVERIFY(response.contains("200 OK"));
        QVERIFY(!response.contains("reasoning_guard_triggered"));

        const QJsonObject runtime = runtimeOf(&proxy);
        const QJsonObject lastResult = runtime.value("last_result").toObject();
        QCOMPARE(lastResult.value("request_kind").toString(), QString("context_compaction"));
        QCOMPARE(lastResult.value("intercept_exempt_reason").toString(), QString("context_compaction"));
        QCOMPARE(lastResult.value("reasoning_tokens").toInt(), 0);
        QVERIFY(logs.join("\n").contains("intercept_exempt_reason=context_compaction"));
    }

    void remoteCompactionV2NormalTurnDoesNotBypassReasoningGuard()
    {
        const QByteArray guardedBody =
            "{\"usage\":{\"output_tokens_details\":{\"reasoning_tokens\":516}}}";
        const QByteArray cleanBody =
            "{\"usage\":{\"output_tokens_details\":{\"reasoning_tokens\":128}}}";
        QList<QByteArray> bodies;
        bodies << guardedBody << cleanBody;
        TestUpstream upstream;
        QVERIFY(upstream.startJsonResponses(bodies));

        HttpProxyServer proxy;
        ProxySettings settings = baseSettings(reserveFreePort(), upstream.port());
        settings.guardRetryAttempts = 1;
        QString error;
        QVERIFY2(proxy.start(settings, &error), qPrintable(error));

        QList<QPair<QByteArray, QByteArray> > headers;
        headers.append(qMakePair(QByteArray("X-Codex-Beta-Features"), QByteArray("remote_compaction_v2")));
        headers.append(qMakePair(QByteArray("X-Codex-Turn-Metadata"), QByteArray("{\"request_kind\":\"turn\"}")));
        const QByteArray response = sendRequest(settings.listenPort,
                                                postRequestToPathWithHeaders("/v1/responses",
                                                                             "{\"reasoning\":{\"effort\":\"xhigh\"}}",
                                                                             headers),
                                                3000);
        QVERIFY(response.contains("200 OK"));
        QVERIFY(response.contains("128"));
        QCOMPARE(upstream.requestCount(), 2);

        const QJsonObject runtime = runtimeOf(&proxy);
        QCOMPARE(runtime.value("guard_retry_total").toInt(), 1);
        QCOMPARE(runtime.value("matched_non_streaming_count").toInt(), 1);
        const QJsonObject lastResult = runtime.value("last_result").toObject();
        QCOMPARE(lastResult.value("request_kind").toString(), QString("normal"));
        QCOMPARE(lastResult.value("reasoning_tokens").toInt(), 128);
        QVERIFY(!lastResult.contains("intercept_exempt_reason"));
    }

    void configSavePreservesProtocolProxyFields()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("config.json");

        AppConfig config;
        config.upstreamProxy = "http://127.0.0.1:7890";
        config.upstreamHttpProxy = "http://127.0.0.1:8080";
        config.upstreamHttpsProxy = "http://127.0.0.1:8443";
        config.upstreamSocksProxy = "socks5://127.0.0.1:1080";
        config.requestBodyLimitBytes = 123456;
        config.responseBufferLimitBytes = 654321;
        config.firstTokenTimeoutSec = 47;
        config.streamAction = "disconnect";

        QString error;
        QVERIFY2(saveConfig(config, path, &error), qPrintable(error));

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(object.value("upstream_proxy").toString(), config.upstreamProxy);
        QCOMPARE(object.value("upstream_http_proxy").toString(), config.upstreamHttpProxy);
        QCOMPARE(object.value("upstream_https_proxy").toString(), config.upstreamHttpsProxy);
        QCOMPARE(object.value("upstream_socks_proxy").toString(), config.upstreamSocksProxy);
        QCOMPARE(qint64(object.value("request_body_limit_bytes").toDouble()), config.requestBodyLimitBytes);
        QCOMPARE(qint64(object.value("response_buffer_limit_bytes").toDouble()), config.responseBufferLimitBytes);
        QCOMPARE(object.value("first_token_timeout_sec").toInt(), config.firstTokenTimeoutSec);
        QCOMPARE(object.value("stream_action").toString(), config.streamAction);

        const AppConfig loaded = loadConfig(path);
        QCOMPARE(loaded.upstreamProxy, config.upstreamProxy);
        QCOMPARE(loaded.upstreamHttpProxy, config.upstreamHttpProxy);
        QCOMPARE(loaded.upstreamHttpsProxy, config.upstreamHttpsProxy);
        QCOMPARE(loaded.upstreamSocksProxy, config.upstreamSocksProxy);
        QCOMPARE(loaded.requestBodyLimitBytes, config.requestBodyLimitBytes);
        QCOMPARE(loaded.responseBufferLimitBytes, config.responseBufferLimitBytes);
        QCOMPARE(loaded.firstTokenTimeoutSec, config.firstTokenTimeoutSec);
        QCOMPARE(loaded.streamAction, config.streamAction);
    }

    void configDefaultsDoNotSetUpstreamBaseUrl()
    {
        const AppConfig config;
        QVERIFY(config.upstreamBaseUrl.isEmpty());

        const ProxySettings settings;
        QVERIFY(settings.upstreamBaseUrl.isEmpty());
    }

    void configLoadPreservesExplicitUpstreamBaseUrl()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("config.json");

        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QJsonObject object;
        object.insert("upstream_base_url", "https://ai.input.im/v1");
        file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
        file.close();

        const AppConfig loaded = loadConfig(path);
        QCOMPARE(loaded.upstreamBaseUrl, QString("https://ai.input.im/v1"));
    }

    void configLoadAcceptsFirstByteTimeoutAlias()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("config.json");

        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QJsonObject object;
        object.insert("upstream_first_byte_timeout_seconds", 17);
        file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
        file.close();

        const AppConfig loaded = loadConfig(path);
        QCOMPARE(loaded.firstTokenTimeoutSec, 17);
    }

    void splitOnlyProxyFieldsStaySplitOnRoundTrip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("config.json");

        AppConfig config;
        config.upstreamProxy.clear();
        config.upstreamHttpProxy.clear();
        config.upstreamHttpsProxy = "http://127.0.0.1:8443";
        config.upstreamSocksProxy.clear();

        QString error;
        QVERIFY2(saveConfig(config, path, &error), qPrintable(error));

        const AppConfig loaded = loadConfig(path);
        QVERIFY(loaded.upstreamProxy.isEmpty());
        QCOMPARE(loaded.upstreamHttpsProxy, config.upstreamHttpsProxy);

        QVERIFY2(saveConfig(loaded, path, &error), qPrintable(error));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(object.value("upstream_proxy").toString(), QString());
        QCOMPARE(object.value("upstream_https_proxy").toString(), config.upstreamHttpsProxy);
    }

    void streamActionDisconnectDropsConnectionAfterPassThrough()
    {
        QList<QByteArray> chunks;
        chunks << "data: {\"type\":\"response.output_text.delta\",\"delta\":\"hello\"}\n\n";
        chunks << "data: {\"type\":\"response.completed\",\"response\":{\"usage\":{\"output_tokens_details\":{\"reasoning_tokens\":516}}}}\n\n";
        TestUpstream upstream;
        QVERIFY(upstream.start(TestUpstream::StreamingSse, QByteArray(), chunks));

        HttpProxyServer proxy;
        ProxySettings settings = baseSettings(reserveFreePort(), upstream.port());
        settings.streamAction = "disconnect";
        settings.guardRetryAttempts = 0;
        QString error;
        QVERIFY2(proxy.start(settings, &error), qPrintable(error));

        const QByteArray response = sendRequest(settings.listenPort, postRequest("{\"stream\":true}"), 3000);
        QVERIFY(!response.contains("reasoning_guard_triggered"));
        QVERIFY(!response.contains("516"));

        const QJsonObject runtime = runtimeOf(&proxy);
        QCOMPARE(runtime.value("blocked_streaming_count").toInt(), 1);
        QCOMPARE(runtime.value("matched_streaming_count").toInt(), 1);
        QCOMPARE(runtime.value("last_failure").toObject().value("error_type").toString(),
                 QString("reasoning_guard_triggered"));
        QCOMPARE(runtime.value("last_failure").toObject().value("status_code").toInt(), 499);
    }

    void incompleteStreamingResponseRetriesBeforeForwarding()
    {
        QList<QByteArray> incompleteChunks;
        incompleteChunks << "data: {\"type\":\"response.created\"}\n\n";
        incompleteChunks << "data: {\"type\":\"response.in_progress\"}\n\n";
        incompleteChunks << "data: {\"type\":\"response.output_item.added\",\"item\":{\"type\":\"reasoning\"}}\n\n";

        QList<QByteArray> validChunks;
        validChunks << "data: {\"type\":\"response.output_text.delta\",\"delta\":\"ok\"}\n\n";
        validChunks << "data: {\"type\":\"response.completed\",\"response\":{\"usage\":{\"output_tokens_details\":{\"reasoning_tokens\":2070}}}}\n\n";

        QList<QList<QByteArray> > sequences;
        sequences << incompleteChunks << validChunks;
        TestUpstream upstream;
        QVERIFY(upstream.startStreamingSequences(sequences));

        HttpProxyServer proxy;
        ProxySettings settings = baseSettings(reserveFreePort(), upstream.port());
        settings.guardRetryAttempts = 1;
        QString error;
        QVERIFY2(proxy.start(settings, &error), qPrintable(error));

        const QByteArray response = sendRequest(settings.listenPort, postRequest("{\"stream\":true}"), 5000);
        QVERIFY(response.contains("200 OK"));
        QVERIFY(response.contains("ok"));
        QVERIFY(!response.contains("stream_incomplete_response"));
        QCOMPARE(upstream.requestCount(), 2);
        QTest::qWait(50);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);

        const QJsonObject runtime = runtimeOf(&proxy);
        QCOMPARE(runtime.value("guard_retry_total").toInt(), 1);
        QCOMPARE(runtime.value("successful_requests_total").toInt(), 1);
        QCOMPARE(runtime.value("failed_requests_total").toInt(), 0);
    }

    void incompleteStreamingResponseReturns502WhenRetryExhausted()
    {
        QList<QByteArray> chunks;
        chunks << "data: {\"type\":\"response.created\"}\n\n";
        chunks << "data: {\"type\":\"response.in_progress\"}\n\n";
        chunks << "data: {\"type\":\"response.output_item.added\",\"item\":{\"type\":\"reasoning\"}}\n\n";
        TestUpstream upstream;
        QVERIFY(upstream.start(TestUpstream::StreamingSse, QByteArray(), chunks));

        HttpProxyServer proxy;
        ProxySettings settings = baseSettings(reserveFreePort(), upstream.port());
        settings.guardRetryAttempts = 0;
        QString error;
        QVERIFY2(proxy.start(settings, &error), qPrintable(error));

        const QByteArray response = sendRequest(settings.listenPort, postRequest("{\"stream\":true}"), 3000);
        QVERIFY(response.contains("502"));
        QVERIFY(response.contains("stream_incomplete_response"));
        QCOMPARE(upstream.requestCount(), 1);

        const QJsonObject runtime = runtimeOf(&proxy);
        QCOMPARE(runtime.value("blocked_streaming_count").toInt(), 1);
        QCOMPARE(runtime.value("failed_requests_total").toInt(), 1);
        QCOMPARE(runtime.value("local_proxy_error_total").toInt(), 1);
        QCOMPARE(runtime.value("upstream_http_error_total").toInt(), 0);
        QCOMPARE(runtime.value("last_failure").toObject().value("error_type").toString(),
                 QString("stream_incomplete_response"));
    }

    void terminalPassThroughKeepsTrailingDoneEvent()
    {
        QList<QByteArray> chunks;
        chunks << "data: {\"type\":\"response.output_text.delta\",\"delta\":\"ok\"}\n\n";
        chunks << "data: {\"type\":\"response.completed\",\"response\":{\"usage\":{\"output_tokens_details\":{\"reasoning_tokens\":367}}}}\n\n";
        chunks << "data: [DONE]\n\n";
        TestUpstream upstream;
        QVERIFY(upstream.start(TestUpstream::StreamingSse, QByteArray(), chunks));

        HttpProxyServer proxy;
        ProxySettings settings = baseSettings(reserveFreePort(), upstream.port());
        QString error;
        QVERIFY2(proxy.start(settings, &error), qPrintable(error));

        const QByteArray response = sendRequest(settings.listenPort, postRequest("{\"stream\":true}"), 3000);
        QVERIFY(response.contains("200 OK"));
        QVERIFY(response.contains("\"delta\":\"ok\""));
        QVERIFY(response.contains("data: [DONE]"));

        QVERIFY(waitUntil([&]() {
            return runtimeOf(&proxy).value("last_transfer_diagnostics").isObject();
        }, 1000));
        const QJsonObject diagnostics = runtimeOf(&proxy).value("last_transfer_diagnostics").toObject();
        QCOMPARE(diagnostics.value("path").toString(), QString("/v1/responses"));
        QCOMPARE(diagnostics.value("status_code").toInt(), 200);
        QVERIFY(diagnostics.value("upstream_bytes_read").toDouble() > 0.0);
        QVERIFY(diagnostics.value("downstream_bytes_queued").toDouble() > 0.0);
        QVERIFY(diagnostics.value("downstream_bytes_written").toDouble() > 0.0);
        QVERIFY(diagnostics.value("upstream_tail_sha256_16").toString().size() > 0);
        QVERIFY(diagnostics.value("downstream_tail_sha256_16").toString().size() > 0);
        QCOMPARE(diagnostics.value("stream_completed_seen").toBool(), true);
        QCOMPARE(diagnostics.value("stream_done_seen").toBool(), true);
        QCOMPARE(diagnostics.value("client_closed_first").toBool(), false);
    }

    void terminalPassThroughKeepsLargeTailUntilClientDrain()
    {
        const QByteArray sentinel = "TAIL_SENTINEL_0123456789_END";
        const QByteArray tail(512 * 1024, 'x');

        QList<QByteArray> chunks;
        chunks << "data: {\"type\":\"response.output_text.delta\",\"delta\":\"prefix ";
        chunks.last().append(tail);
        chunks.last().append(sentinel);
        chunks.last().append("\"}\n\n");
        chunks << "data: {\"type\":\"response.completed\",\"response\":{\"usage\":{\"output_tokens_details\":{\"reasoning_tokens\":367}}}}\n\n";
        chunks << "data: [DONE]\n\n";
        TestUpstream upstream;
        QVERIFY(upstream.start(TestUpstream::StreamingSse, QByteArray(), chunks));

        HttpProxyServer proxy;
        ProxySettings settings = baseSettings(reserveFreePort(), upstream.port());
        QString error;
        QVERIFY2(proxy.start(settings, &error), qPrintable(error));

        const QByteArray response = sendRequest(settings.listenPort, postRequest("{\"stream\":true}"), 5000);
        QVERIFY(response.contains("200 OK"));
        QVERIFY(response.contains(sentinel));
        QVERIFY(response.contains("data: [DONE]"));

        QVERIFY(waitUntil([&]() {
            const QJsonObject diagnostics = runtimeOf(&proxy).value("last_transfer_diagnostics").toObject();
            return diagnostics.value("status_code").toInt() == 200 &&
                diagnostics.value("downstream_bytes_pending").toDouble() == 0.0 &&
                diagnostics.value("downstream_bytes_backlog").toDouble() == 0.0 &&
                diagnostics.value("downstream_bytes_queued").toDouble() ==
                    diagnostics.value("downstream_bytes_written").toDouble();
        }, 1000));
        const QJsonObject diagnostics = runtimeOf(&proxy).value("last_transfer_diagnostics").toObject();
        QCOMPARE(diagnostics.value("upstream_tail_sha256_16").toString(),
                 diagnostics.value("downstream_tail_sha256_16").toString());
        QCOMPARE(diagnostics.value("stream_completed_seen").toBool(), true);
        QCOMPARE(diagnostics.value("stream_done_seen").toBool(), true);
        QCOMPARE(diagnostics.value("client_closed_first").toBool(), false);
    }

    void streamActionDisconnectDropsChunkContainingMatchedEvent()
    {
        QList<QByteArray> chunks;
        chunks << "data: {\"type\":\"response.output_text.delta\",\"delta\":\"hello \"}\n\n";
        chunks << "data: {\"type\":\"response.output_text.delta\",\"delta\":\"tail\"}\n\n"
                  "data: {\"type\":\"response.completed\",\"response\":{\"usage\":{\"output_tokens_details\":{\"reasoning_tokens\":516}}}}\n\n";
        TestUpstream upstream;
        QVERIFY(upstream.start(TestUpstream::StreamingSse, QByteArray(), chunks));

        HttpProxyServer proxy;
        ProxySettings settings = baseSettings(reserveFreePort(), upstream.port());
        settings.streamAction = "disconnect";
        settings.guardRetryAttempts = 0;
        QString error;
        QVERIFY2(proxy.start(settings, &error), qPrintable(error));

        const QByteArray response = sendRequest(settings.listenPort, postRequest("{\"stream\":true}"), 3000);
        QVERIFY(!response.contains("tail"));
        QVERIFY(!response.contains("516"));

        const QJsonObject runtime = runtimeOf(&proxy);
        QCOMPARE(runtime.value("blocked_streaming_count").toInt(), 1);
        QCOMPARE(runtime.value("matched_streaming_count").toInt(), 1);
        QCOMPARE(runtime.value("last_failure").toObject().value("error_type").toString(),
                 QString("reasoning_guard_triggered"));
        QCOMPARE(runtime.value("last_failure").toObject().value("status_code").toInt(), 499);
    }

    void streamActionDisconnectRetriesBeforePassThrough()
    {
        QList<QByteArray> chunks;
        chunks << "data: {\"type\":\"response.output_text.delta\",\"delta\":\"hello \"}\n\n";
        chunks << "data: {\"type\":\"response.output_text.delta\",\"delta\":\"tail\"}\n\n"
                  "data: {\"type\":\"response.completed\",\"response\":{\"usage\":{\"output_tokens_details\":{\"reasoning_tokens\":516}}}}\n\n";
        TestUpstream upstream;
        QVERIFY(upstream.start(TestUpstream::StreamingSse, QByteArray(), chunks));

        HttpProxyServer proxy;
        ProxySettings settings = baseSettings(reserveFreePort(), upstream.port());
        settings.streamAction = "disconnect";
        settings.guardRetryAttempts = 1;
        QString error;
        QVERIFY2(proxy.start(settings, &error), qPrintable(error));

        const QByteArray response = sendRequest(settings.listenPort, postRequest("{\"stream\":true}"), 3000);
        QVERIFY(!response.contains("tail"));
        QVERIFY(!response.contains("516"));
        QCOMPARE(upstream.requestCount(), 2);

        const QJsonObject runtime = runtimeOf(&proxy);
        QCOMPARE(runtime.value("guard_retry_total").toInt(), 1);
        QCOMPARE(runtime.value("reasoning_tokens_516_retry_total").toInt(), 1);
        QCOMPARE(runtime.value("blocked_streaming_count").toInt(), 1);
        QCOMPARE(runtime.value("matched_streaming_count").toInt(), 2);
        QCOMPARE(runtime.value("last_failure").toObject().value("error_type").toString(),
                 QString("reasoning_guard_triggered"));
        QCOMPARE(runtime.value("last_failure").toObject().value("status_code").toInt(), 499);
    }
};

QTEST_MAIN(HttpProxyServerTest)

#include "http_proxy_server_test.moc"
