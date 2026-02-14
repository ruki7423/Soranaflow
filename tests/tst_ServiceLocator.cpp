#include <QtTest/QtTest>
#include "ServiceLocator.h"

// Dummy service classes for testing
class IFoo {
public:
    virtual ~IFoo() = default;
    virtual int value() const = 0;
};

class FooImpl : public IFoo {
public:
    int value() const override { return 42; }
};

class MockFoo : public IFoo {
public:
    int value() const override { return 99; }
};

class IBar {
public:
    virtual ~IBar() = default;
    virtual QString name() const = 0;
};

class BarImpl : public IBar {
public:
    QString name() const override { return QStringLiteral("BarImpl"); }
};

class tst_ServiceLocator : public QObject {
    Q_OBJECT

private slots:
    void cleanup()
    {
        ServiceLocator::reset();
    }

    void get_returnsNullWhenNotRegistered()
    {
        QVERIFY(ServiceLocator::get<IFoo>() == nullptr);
    }

    void provide_thenGet()
    {
        FooImpl foo;
        ServiceLocator::provide<IFoo>(&foo);
        auto* resolved = ServiceLocator::get<IFoo>();
        QVERIFY(resolved != nullptr);
        QCOMPARE(resolved->value(), 42);
    }

    void provide_overrideWithMock()
    {
        FooImpl real;
        MockFoo mock;
        ServiceLocator::provide<IFoo>(&real);
        QCOMPARE(ServiceLocator::get<IFoo>()->value(), 42);

        ServiceLocator::provide<IFoo>(&mock);
        QCOMPARE(ServiceLocator::get<IFoo>()->value(), 99);
    }

    void multipleServices()
    {
        FooImpl foo;
        BarImpl bar;
        ServiceLocator::provide<IFoo>(&foo);
        ServiceLocator::provide<IBar>(&bar);

        QCOMPARE(ServiceLocator::get<IFoo>()->value(), 42);
        QCOMPARE(ServiceLocator::get<IBar>()->name(), QStringLiteral("BarImpl"));
    }

    void remove_singleService()
    {
        FooImpl foo;
        ServiceLocator::provide<IFoo>(&foo);
        QVERIFY(ServiceLocator::get<IFoo>() != nullptr);

        ServiceLocator::remove<IFoo>();
        QVERIFY(ServiceLocator::get<IFoo>() == nullptr);
    }

    void reset_clearsAll()
    {
        FooImpl foo;
        BarImpl bar;
        ServiceLocator::provide<IFoo>(&foo);
        ServiceLocator::provide<IBar>(&bar);

        ServiceLocator::reset();
        QVERIFY(ServiceLocator::get<IFoo>() == nullptr);
        QVERIFY(ServiceLocator::get<IBar>() == nullptr);
    }
};

QTEST_MAIN(tst_ServiceLocator)
#include "tst_ServiceLocator.moc"
