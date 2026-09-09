#include "capture/CaptureFrameHandoff.h"

#include <QColor>
#include <QCoreApplication>
#include <QEvent>
#include <QImage>
#include <QObject>
#include <QTest>
#include <QVector>

#include <array>
#include <memory>
#include <thread>

class SyntheticCapture final : public QObject {
    Q_OBJECT
public:
    CaptureFrameHandoff handoff;

    bool submit() {
        CapturedFrame frame;
        if (!handoff.tryAcquire(frame)) return false;
        frame.image = QImage(320, 180, QImage::Format_RGB32);
        frame.image.fill(QColor(int(frame.sourceSequence % 255), 32, 64));
        emit frameReady(CaptureFrameHandoff::imageForDelivery(std::move(frame)));
        return true;
    }

signals:
    void frameReady(QImage frame);
};

class CaptureHandoffTests final : public QObject {
    Q_OBJECT
private slots:
    void stalledConsumerBoundsQueuedImages();
    void destroyingQueuedReceiverReleasesSlots();
    void fanoutRetainsOneLeaseUntilEveryConsumerReleases();
    void imageRemainsValidAfterProducerDestruction();
    void concurrentReservationsCannotExceedTheBound();
    void emptyImagesAndAbandonedReservationsReleaseSlots();
    void taggedFramesKeepTheirLease();
};

void CaptureHandoffTests::stalledConsumerBoundsQueuedImages() {
    SyntheticCapture source;
    QObject receiver;
    QVector<QImage> delivered;
    connect(&source, &SyntheticCapture::frameReady, &receiver,
            [&delivered](QImage image) { delivered.append(std::move(image)); },
            Qt::QueuedConnection);

    // Joining without processing events deliberately stalls the consumer.
    std::thread producer([&source] {
        for (int i = 0; i < 10000; ++i) source.submit();
    });
    producer.join();

    QCOMPARE(delivered.size(), 0);
    QCOMPARE(source.handoff.inFlight(), 2);
    QCOMPARE(source.handoff.stats().framesProduced, 10000);
    QCOMPARE(source.handoff.stats().framesDropped, 9998);

    QCoreApplication::sendPostedEvents(&receiver, QEvent::MetaCall);
    QCOMPARE(delivered.size(), 2);
    QCOMPARE(source.handoff.inFlight(), 2);
    QCOMPARE(delivered[0].pixelColor(0, 0), QColor(1, 32, 64));
    QCOMPARE(delivered[1].pixelColor(0, 0), QColor(2, 32, 64));

    // A displayed image may be retained while the pending image advances.
    delivered.removeFirst();
    QCOMPARE(source.handoff.inFlight(), 1);
    QVERIFY(source.submit());
    QCoreApplication::sendPostedEvents(&receiver, QEvent::MetaCall);
    QCOMPARE(delivered.size(), 2);
    QCOMPARE(delivered.last().pixelColor(0, 0), QColor(10001 % 255, 32, 64));
    delivered.clear();
    QCOMPARE(source.handoff.inFlight(), 0);
}

void CaptureHandoffTests::destroyingQueuedReceiverReleasesSlots() {
    SyntheticCapture source;
    auto* receiver = new QObject;
    int calls = 0;
    connect(&source, &SyntheticCapture::frameReady, receiver,
            [&calls](QImage) { ++calls; }, Qt::QueuedConnection);

    QVERIFY(source.submit());
    QVERIFY(source.submit());
    QVERIFY(!source.submit());
    QCOMPARE(source.handoff.inFlight(), 2);
    delete receiver;

    QCOMPARE(calls, 0);
    QCOMPARE(source.handoff.inFlight(), 0);
    QVERIFY(source.submit());
    // No connected receiver means the image and lease are released at emit.
    QCOMPARE(source.handoff.inFlight(), 0);
}

void CaptureHandoffTests::fanoutRetainsOneLeaseUntilEveryConsumerReleases() {
    SyntheticCapture source;
    QObject first, second;
    QVector<QImage> firstImages, secondImages;
    connect(&source, &SyntheticCapture::frameReady, &first,
            [&firstImages](QImage image) { firstImages.append(std::move(image)); },
            Qt::QueuedConnection);
    connect(&source, &SyntheticCapture::frameReady, &second,
            [&secondImages](QImage image) { secondImages.append(std::move(image)); },
            Qt::QueuedConnection);

    QVERIFY(source.submit());
    QVERIFY(source.submit());
    QCOMPARE(source.handoff.inFlight(), 2);
    QCoreApplication::sendPostedEvents(&first, QEvent::MetaCall);
    firstImages.clear();
    QCOMPARE(source.handoff.inFlight(), 2);
    QVERIFY(!source.submit());
    QCoreApplication::sendPostedEvents(&second, QEvent::MetaCall);
    QCOMPARE(secondImages.size(), 2);
    secondImages.clear();
    QCOMPARE(source.handoff.inFlight(), 0);
}

void CaptureHandoffTests::imageRemainsValidAfterProducerDestruction() {
    auto source = std::make_unique<SyntheticCapture>();
    CaptureFrameHandoff account = source->handoff;
    QObject receiver;
    QImage image;
    connect(source.get(), &SyntheticCapture::frameReady, &receiver,
            [&image](QImage delivered) { image = std::move(delivered); },
            Qt::QueuedConnection);
    QVERIFY(source->submit());
    source.reset();

    QCOMPARE(account.inFlight(), 1);
    QCoreApplication::sendPostedEvents(&receiver, QEvent::MetaCall);
    QCOMPARE(image.pixelColor(0, 0), QColor(1, 32, 64));
    QImage shared = image;
    image = {};
    QCOMPARE(account.inFlight(), 1);
    QCOMPARE(shared.pixelColor(319, 179), QColor(1, 32, 64));
    shared = {};
    QCOMPARE(account.inFlight(), 0);
}

void CaptureHandoffTests::concurrentReservationsCannotExceedTheBound() {
    CaptureFrameHandoff handoff;
    std::array<std::thread, 8> producers;
    std::array<QVector<CapturedFrame>, 8> retained;
    for (size_t i = 0; i < producers.size(); ++i) {
        producers[i] = std::thread([&handoff, &retained, i] {
            for (int attempt = 0; attempt < 100; ++attempt) {
                CapturedFrame frame;
                if (handoff.tryAcquire(frame)) retained[i].append(std::move(frame));
            }
        });
    }
    for (auto& producer : producers) producer.join();
    QCOMPARE(handoff.inFlight(), 2);
    QCOMPARE(handoff.stats().framesProduced, 800);
    QCOMPARE(handoff.stats().framesDropped, 798);
    for (auto& frames : retained) frames.clear();
    QCOMPARE(handoff.inFlight(), 0);
}

void CaptureHandoffTests::emptyImagesAndAbandonedReservationsReleaseSlots() {
    CaptureFrameHandoff handoff;
    {
        CapturedFrame frame;
        QVERIFY(handoff.tryAcquire(frame));
        QCOMPARE(handoff.inFlight(), 1);
    }
    QCOMPARE(handoff.inFlight(), 0);
    CapturedFrame frame;
    QVERIFY(handoff.tryAcquire(frame));
    QVERIFY(CaptureFrameHandoff::imageForDelivery(std::move(frame)).isNull());
    QCOMPARE(handoff.inFlight(), 0);
}

void CaptureHandoffTests::taggedFramesKeepTheirLease() {
    // imageForDelivery copies the colour space and pixel ratio onto the wrapper
    // it hands out. Both setters detach, and a QImage built over const data
    // detaches by deep copying whatever its reference count says, which would
    // destroy the retained frame and release the lease immediately. The bound
    // would then be gone with nothing to show for it: no error, no dropped
    // frame, just an unbounded queue again. No backend tags a frame today, so
    // this is here to fail on the day one starts.
    CaptureFrameHandoff handoff;
    CapturedFrame frame;
    QVERIFY(handoff.tryAcquire(frame));
    frame.image = QImage(320, 180, QImage::Format_RGB32);
    frame.image.fill(QColor(7, 32, 64));
    frame.image.setColorSpace(QColorSpace::SRgb);
    frame.image.setDevicePixelRatio(2.0);

    QImage delivered = CaptureFrameHandoff::imageForDelivery(std::move(frame));
    QVERIFY(!delivered.isNull());
    QCOMPARE(delivered.pixelColor(0, 0), QColor(7, 32, 64));
    QCOMPARE(delivered.devicePixelRatio(), 2.0);
    QVERIFY(delivered.colorSpace().isValid());
    QCOMPARE(handoff.inFlight(), 1);

    delivered = {};
    QCOMPARE(handoff.inFlight(), 0);
}

QTEST_GUILESS_MAIN(CaptureHandoffTests)
#include "capture_handoff_tests.moc"
