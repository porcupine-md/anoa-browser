#pragma once

#include <functional>

#include <QObject>
#include <QJsonObject>
#include <QString>

class QWebEnginePage;
class TabHost;

class CdpExtensions : public QObject {
    Q_OBJECT
public:
    // Three outcomes, not two:
    //
    //   non-empty return                -> answered now, send this
    //   empty return, *deferred false   -> not ours, forward upstream unchanged
    //   empty return, *deferred true    -> ours, nothing goes upstream, and
    //                                      exactly one reply arrives through
    //                                      sendLater on a later event loop turn
    //
    // The third case exists for Target.createTarget: a page exists before its
    // DevTools target does, so the id cannot be known in the same turn. Waiting
    // for it would mean a nested event loop, and a seam is crossed by signals,
    // never by a blocking call.
    //
    // sendLater must be invoked at most once per command, and the caller is
    // responsible for a handler that never invokes it at all.
    static QString processCommand(const QJsonObject &cmd, QWebEnginePage *page,
                                  TabHost *tabs = nullptr,
                                  bool *deferred = nullptr,
                                  const std::function<void(const QString &)> &sendLater = {});

    // Returns a possibly-modified command for upstream forwarding; null object = no change.
    static QJsonObject rewritePassthrough(const QJsonObject &cmd);

private:
    static QString handleProfiler(const QJsonObject &cmd);
    static QString handleHeapProfiler(const QJsonObject &cmd);
    static QString handleSecurity(const QJsonObject &cmd, QWebEnginePage *page);
    static QString handleBrowser(const QJsonObject &cmd, TabHost *tabs);
    static QString handleTarget(const QJsonObject &cmd, TabHost *tabs, bool *deferred,
                                const std::function<void(const QString &)> &sendLater);
};
