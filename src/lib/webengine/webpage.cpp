/* ============================================================
* Falkon - Qt web browser
* Copyright (C) 2010-2018 David Rosca <nowrep@gmail.com>
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
* ============================================================ */
#include "webpage.h"
#include "tabbedwebview.h"
#include "browserwindow.h"
#include "pluginproxy.h"
#include "downloadmanager.h"
#include "mainapplication.h"
#include "checkboxdialog.h"
#include "qztools.h"
#include "speeddial.h"
#include "autofill.h"
#include "popupwebview.h"
#include "popupwindow.h"
#include "iconprovider.h"
#include "qzsettings.h"
#include "useragentmanager.h"
#include "delayedfilewatcher.h"
#include "searchenginesmanager.h"
#include "html5permissions/html5permissionsmanager.h"
#include "javascript/externaljsobject.h"
#include "tabwidget.h"
#include "networkmanager.h"
#include "webhittestresult.h"
#include "ui_jsconfirm.h"
#include "ui_jsalert.h"
#include "ui_jsprompt.h"
#include "passwordmanager.h"
#include "scripts.h"
#include "ocssupport.h"

#include <iostream>

#include <QDir>
#include <QMouseEvent>
#include <QWebChannel>
#include <QWebEngineHistory>
#include <QWebEngineSettings>
#include <QTimer>
#include <QDesktopServices>
#include <QMessageBox>
#include <QFileDialog>
#include <QAuthenticator>
#include <QPushButton>
#include <QUrlQuery>
#include <QtWebEngineWidgetsVersion>

#if QTWEBENGINEWIDGETS_VERSION >= QT_VERSION_CHECK(5, 11, 0)
#include <QWebEngineRegisterProtocolHandlerRequest>
#endif

QString WebPage::s_lastUploadLocation = QDir::homePath();
QUrl WebPage::s_lastUnsupportedUrl;
QTime WebPage::s_lastUnsupportedUrlTime;
QStringList s_supportedSchemes;

static const bool kEnableJsOutput = qEnvironmentVariableIsSet("FALKON_ENABLE_JS_OUTPUT");
static const bool kEnableJsNonBlockDialogs = qEnvironmentVariableIsSet("FALKON_ENABLE_JS_NONBLOCK_DIALOGS");

WebPage::WebPage(QObject* parent)
    : QWebEnginePage(mApp->webProfile(), parent)
    , m_fileWatcher(0)
    , m_runningLoop(0)
    , m_loadProgress(100)
    , m_blockAlerts(false)
    , m_secureStatus(false)
{
    QWebChannel *channel = new QWebChannel(this);
    ExternalJsObject::setupWebChannel(channel, this);
    setWebChannel(channel, SafeJsWorld);

    connect(this, &QWebEnginePage::loadProgress, this, &WebPage::progress);
    connect(this, &QWebEnginePage::loadFinished, this, &WebPage::finished);
    connect(this, &QWebEnginePage::urlChanged, this, &WebPage::urlChanged);
    connect(this, &QWebEnginePage::featurePermissionRequested, this, &WebPage::featurePermissionRequested);
    connect(this, &QWebEnginePage::windowCloseRequested, this, &WebPage::windowCloseRequested);
    connect(this, &QWebEnginePage::fullScreenRequested, this, &WebPage::fullScreenRequested);
    connect(this, &QWebEnginePage::renderProcessTerminated, this, &WebPage::renderProcessTerminated);

    connect(this, &QWebEnginePage::authenticationRequired, this, [this](const QUrl &url, QAuthenticator *auth) {
        mApp->networkManager()->authentication(url, auth, view());
    });

    connect(this, &QWebEnginePage::proxyAuthenticationRequired, this, [this](const QUrl &, QAuthenticator *auth, const QString &proxyHost) {
        mApp->networkManager()->proxyAuthentication(proxyHost, auth, view());
    });

    // Workaround QWebEnginePage not scrolling to anchors when opened in background tab
    m_contentsResizedConnection = connect(this, &QWebEnginePage::contentsSizeChanged, this, [this]() {
        const QString fragment = url().fragment();
        if (!fragment.isEmpty()) {
            runJavaScript(Scripts::scrollToAnchor(fragment));
        }
        disconnect(m_contentsResizedConnection);
    });

    // Workaround for broken load started/finished signals in QtWebEngine 5.10, 5.11
    connect(this, &QWebEnginePage::loadProgress, this, [this](int progress) {
        if (progress == 100) {
            emit loadFinished(true);
        }
    });

#if QTWEBENGINEWIDGETS_VERSION >= QT_VERSION_CHECK(5, 11, 0)
    connect(this, &QWebEnginePage::registerProtocolHandlerRequested, this, [this](QWebEngineRegisterProtocolHandlerRequest request) {
        delete m_registerProtocolHandlerRequest;
        m_registerProtocolHandlerRequest = new QWebEngineRegisterProtocolHandlerRequest(request);
    });
#endif

#if QTWEBENGINEWIDGETS_VERSION >= QT_VERSION_CHECK(5, 12, 0)
    connect(this, &QWebEnginePage::printRequested, this, &WebPage::printRequested);
    connect(this, &QWebEnginePage::selectClientCertificate, this, [this](QWebEngineClientCertificateSelection selection) {
        // TODO: It should prompt user
        selection.select(selection.certificates().at(0));
    });
#endif
}

WebPage::~WebPage()
{
#if QTWEBENGINEWIDGETS_VERSION >= QT_VERSION_CHECK(5, 11, 0)
    delete m_registerProtocolHandlerRequest;
#endif

    if (m_runningLoop) {
        m_runningLoop->exit(1);
        m_runningLoop = 0;
    }
}

WebView *WebPage::view() const
{
    return static_cast<WebView*>(QWebEnginePage::view());
}

bool WebPage::execPrintPage(QPrinter *printer, int timeout)
{
    QPointer<QEventLoop> loop = new QEventLoop;
    bool result = false;
    QTimer::singleShot(timeout, loop.data(), &QEventLoop::quit);

    print(printer, [loop, &result](bool res) {
        if (loop && loop->isRunning()) {
            result = res;
            loop->quit();
        }
    });

    loop->exec();
    delete loop;

    return result;
}

QVariant WebPage::execJavaScript(const QString &scriptSource, quint32 worldId, int timeout)
{
    QPointer<QEventLoop> loop = new QEventLoop;
    QVariant result;
    QTimer::singleShot(timeout, loop.data(), &QEventLoop::quit);

    runJavaScript(scriptSource, worldId, [loop, &result](const QVariant &res) {
        if (loop && loop->isRunning()) {
            result = res;
            loop->quit();
        }
    });

    loop->exec(QEventLoop::ExcludeUserInputEvents);
    delete loop;

    return result;
}

QPointF WebPage::mapToViewport(const QPointF &pos) const
{
    return QPointF(pos.x() / zoomFactor(), pos.y() / zoomFactor());
}

WebHitTestResult WebPage::hitTestContent(const QPoint &pos) const
{
    return WebHitTestResult(this, pos);
}

void WebPage::scroll(int x, int y)
{
    runJavaScript(QSL("window.scrollTo(window.scrollX + %1, window.scrollY + %2)").arg(x).arg(y), SafeJsWorld);
}

void WebPage::setScrollPosition(const QPointF &pos)
{
    const QPointF v = mapToViewport(pos.toPoint());
    runJavaScript(QSL("window.scrollTo(%1, %2)").arg(v.x()).arg(v.y()), SafeJsWorld);
}

bool WebPage::isRunningLoop()
{
    return m_runningLoop;
}

bool WebPage::isLoading() const
{
    return m_loadProgress < 100;
}

// static
QStringList WebPage::internalSchemes()
{
    return QStringList{
        QSL("http"),
        QSL("https"),
        QSL("file"),
        QSL("ftp"),
        QSL("data"),
        QSL("about"),
        QSL("view-source"),
        QSL("chrome")
    };
}

// static
QStringList WebPage::supportedSchemes()
{
    if (s_supportedSchemes.isEmpty()) {
        s_supportedSchemes = internalSchemes();
    }
    return s_supportedSchemes;
}

// static
void WebPage::addSupportedScheme(const QString &scheme)
{
    s_supportedSchemes = supportedSchemes();
    if (!s_supportedSchemes.contains(scheme)) {
        s_supportedSchemes.append(scheme);
    }
}

// static
void WebPage::removeSupportedScheme(const QString &scheme)
{
    s_supportedSchemes.removeOne(scheme);
}

void WebPage::urlChanged(const QUrl &url)
{
    Q_UNUSED(url)

    if (isLoading()) {
        m_blockAlerts = false;
    }
}

void WebPage::progress(int prog)
{
    m_loadProgress = prog;

    bool secStatus = url().scheme() == QL1S("https");

    if (secStatus != m_secureStatus) {
        m_secureStatus = secStatus;
        emit privacyChanged(secStatus);
    }
}

void WebPage::finished()
{
    progress(100);

    // File scheme watcher
    if (url().scheme() == QLatin1String("file")) {
        QFileInfo info(url().toLocalFile());
        if (info.isFile()) {
            if (!m_fileWatcher) {
                m_fileWatcher = new DelayedFileWatcher(this);
                connect(m_fileWatcher, &DelayedFileWatcher::delayedFileChanged, this, &WebPage::watchedFileChanged);
            }

            const QString filePath = url().toLocalFile();

            if (QFile::exists(filePath) && !m_fileWatcher->files().contains(filePath)) {
                m_fileWatcher->addPath(filePath);
            }
        }
    }
    else if (m_fileWatcher && !m_fileWatcher->files().isEmpty()) {
        m_fileWatcher->removePaths(m_fileWatcher->files());
    }

    // AutoFill
    m_autoFillUsernames = mApp->autoFill()->completePage(this, url());
    this->setBackgroundColor(Qt::transparent);
    const char * transparencyScript = R"V(
    (function () {
    var v = document.createElement("style");
    v.innerHTML = "body,html,.darkmode-background,.darkmode-layer,html,.trans:not(.quaz),html[dark]{background:transparent !important;}::-webkit-scrollbar {    background-color:#ffffff00;    width:8px;height:8px;}::-webkit-scrollbar-track {    background-color:#ffffff00;width:16px;height:16px;}::-webkit-scrollbar-thumb {    background-color:#babac080;    border-radius:8px;    border:4px solid transparent}::-webkit-scrollbar-button {display:none} .bblur{background:rgba(49, 49, 58,0.2) !important;}";
    document.body.append(v);

    function getBkColor(el) {
        var c = window.getComputedStyle(el, null).getPropertyValue("background-color");
        var mt = /\d+/g;
        if (c.match(mt)) {
            var q = c.match(mt).map(parseFloat);
            return q.length < 4 ? q.concat([1]) : q;
        }
        return [0, 0, 0, 0];
    }

    function bkInfluence(e) {
        if (!e.parentElement) {
            return 0;
        }
        var a = getBkColor(e);
        var b = getBkColor(e.parentElement);
        var diff = [Math.abs(a[0] - b[0]) / 255, Math.abs(a[1] - b[1]) / 255, Math.abs(a[2] - b[2]) / 255];
        return (diff[0] + diff[2] + diff[2]) * a[3] / 3;
    }

    function shouldT(e) {
        var c = window.getComputedStyle(e, null).getPropertyValue("background-color");
        var mt = /\d+/g;
        if (c.match(mt)) {
            var cc = c.match(mt).map(parseFloat);
            var tcond = (!(cc.length > 3 && cc[3] < 0.25) || e.classList.contains("trans"));
            if (cc[0] + cc[1] + cc[2] > 127 * 3 || tcond) {
                if (tcond && e.parentElement) {
                    return !shouldT(e.parentElement);
                }
                return true;
            }
        }
        return false;
    }

    function colorWild(color) {
        var ave = (color[0] + color[1] + color[2]) / 255 / 3;
        var st = (color.slice(0, 3).map(x => (x / 255 - ave) ** 2).reduce((a, b) => a + b) / 2) ** 0.5;
        return st;
    }
    var tim = 0;

    function magicWandTheme() {
        tim += 1;
        var all = document.querySelectorAll(tim % 10 === 0 ? "*" : "*:not(.dark-checked)");
        for (var i = 0; i < all.length; i++) {
            var e = all[i];
            e.classList.add("dark-checked");
            var c = window.getComputedStyle(e, null).getPropertyValue("background-color");
            var bi = window.getComputedStyle(e, null).getPropertyValue("background-image");
            var cd = window.getComputedStyle(e, null).getPropertyValue("position");
            var mt = /\d+/g;
            if (c.match(mt)) {
                var cc = c.match(mt).map(parseFloat);
                if (cc[0] + cc[1] + cc[2] < 127 * 3 / 4) {
                    e.style.backgroundColor = "rgba(0,0,0,0)";
                    if (!(cc.length > 3 && cc[3] < 0.25)) {
                        e.classList.add("trans")
                    }
                }
                if (e.classList.contains("docos-avatar")) {
                    e.style.zIndex = 1; //fix google doc avatar
                }
                if (shouldT(e) || (bkInfluence(e) < 1 / 3 || cc[0] + cc[1] + cc[2] > 127 * 3 || (e.style.backgroundImage.substring(0, "linear-gradient".length) == "linear-gradient"))) { //||(cc.length>3&&cc[3]<0.25)){
                    //e.style.backgroundColor="rgba(0,0,0,0)";
                    e.style.backgroundColor = "rgba(" + cc[0] + "," + cc[1] + "," + cc[2] + "," + Math.max(colorWild(cc), 0) + ")";
                    if (cc.length < 4 && colorWild(cc) > 0.125) {
                        e.classList.add("quaz")
                    }
                    if (!(cc.length > 3 && cc[3] < 0.25)) {
                        e.classList.add("trans")
                    }
                    if (!(bi.length > 3 && (bi.substring(0, 3) === "non"))) { //||bi.substring(0,3)==="url"))){
                        // console.log(e.style.backgroundImage.substring(0,3));
                        if (bi.match("gradient") || e.webkitMatchesSelector(".vectorTabs li")) { //}.substring(0,"linear-gradient".length)=="linear-gradient"||e.webkitMatchesSelector(".vectorTabs li")  ){
                            e.style.backgroundImage = "none";
                        }
                    }

                    var tcond = (!(cc.length > 3 && cc[3] < 0.25) || e.classList.contains("trans"));
                    //console.log(tcond);
                    if (tcond || e.tagName.toLowerCase() !== "span" || true && !(e.webkitMatchesSelector("pre code span"))) e.style.color = "rgba(255,255,255,1)";
                    var nt = bkInfluence(e) > 1 / 2 || e.style.boxShadow != "none";
                    if (tcond && ((cd === "absolute" || cd === "fixed" || cd === "sticky" || !(e.parentElement && shouldT(e.parentElement))) || (nt && !(e.parentElement && shouldT(e.parentElement))))) {
                        if ((!(e.parentElement && e.parentElement.innerText && e.parentElement.innerText.length < e.innerText.length + 2) || nt) &&
                            (!e.webkitMatchesSelector("ytd-app,ytd-watch-flexy"))) {
                            //console.log(e.style);
                            e.style.backdropFilter = "blur(5px) opacity(0.8)";
                            //if(cc[0]+cc[1]+cc[2]<255*3/2){
                                e.classList.add("bblur");
                            e.style.backdropFilter = "opacity(" + (1 - Math.abs((cc[0] + cc[1] + cc[2]) / 255 / 3 - 0.5) / 5 * 2) + ") blur(5px)";
                            //}
                            if (cc.length < 4) {
                                e.style.backgroundColor = "rgba(" + cc[0] + "," + cc[1] + "," + cc[2] + "," + Math.max(colorWild(cc), 0) + ")";
                                if (colorWild(cc) > 0.125) e.classList.add("quaz")
                            }
                            e.style.boxShadow = "none";
                        }
                    }
                }
            }
            var c = window.getComputedStyle(e, null).getPropertyValue("border-left-color");
            var mt = /\d+/g;
            if (c.match(mt)) {
                var cc = c.match(mt).map(parseFloat);
                if (cc[0] + cc[1] + cc[2] > 127 * 3 && (cc.length < 4 || cc[3] > 0.25)) {
                    e.style.borderLeftColor = "rgb(255,255,255)";
                }
            }
            var c = window.getComputedStyle(e, null).getPropertyValue("border-right-color");
            var mt = /\d+/g;
            if (c.match(mt)) {
                var cc = c.match(mt).map(parseFloat);
                if (cc[0] + cc[1] + cc[2] > 127 * 3 && (cc.length < 4 || cc[3] > 0.25)) {
                    e.style.borderRightColor = "rgb(255,255,255)";
                }
            }
            var c = window.getComputedStyle(e, null).getPropertyValue("border-top-color");
            var mt = /\d+/g;
            if (c.match(mt)) {
                var cc = c.match(mt).map(parseFloat);
                if (cc[0] + cc[1] + cc[2] > 127 * 3 && (cc.length < 4 || cc[3] > 0.25)) {
                    e.style.borderTopColor = "rgb(255,255,255)";
                }
            }
            var c = window.getComputedStyle(e, null).getPropertyValue("border-bottom-color");
            var mt = /\d+/g;
            if (c.match(mt)) {
                var cc = c.match(mt).map(parseFloat);
                if (cc[0] + cc[1] + cc[2] > 127 * 3 && (cc.length < 4 || cc[3] > 0.25)) {
                    e.style.borderBottomColor = "rgb(255,255,255)";
                }
            }
        }
    }
    window.setInterval(magicWandTheme, 10);
    //var v2=document.createElement("script");
    //v2.src="https://cdn.jsdelivr.net/npm/darkmode-js@1.5.3/lib/darkmode-js.min.js";
    //document.body.append(v2);
    //window.setTimeout(()=>{window.dark=new Darkmode();if(!window.dark.isActivated()){window.dark.toggle();}},1000);
})();)V";
    this->runJavaScript(QString::fromUtf8(transparencyScript));
}

void WebPage::watchedFileChanged(const QString &file)
{
    if (url().toLocalFile() == file) {
        triggerAction(QWebEnginePage::Reload);
    }
}

void WebPage::handleUnknownProtocol(const QUrl &url)
{
    const QString protocol = url.scheme();

    if (protocol == QLatin1String("mailto")) {
        desktopServicesOpen(url);
        return;
    }

    if (qzSettings->blockedProtocols.contains(protocol)) {
        qDebug() << "WebPage::handleUnknownProtocol Protocol" << protocol << "is blocked!";
        return;
    }

    if (qzSettings->autoOpenProtocols.contains(protocol)) {
        desktopServicesOpen(url);
        return;
    }

    CheckBoxDialog dialog(QMessageBox::Yes | QMessageBox::No, view());
    dialog.setDefaultButton(QMessageBox::Yes);

    const QString wrappedUrl = QzTools::alignTextToWidth(url.toString(), "<br/>", dialog.fontMetrics(), 450);
    const QString text = tr("Falkon cannot handle <b>%1:</b> links. The requested link "
                            "is <ul><li>%2</li></ul>Do you want Falkon to try "
                            "open this link in system application?").arg(protocol, wrappedUrl);

    dialog.setText(text);
    dialog.setCheckBoxText(tr("Remember my choice for this protocol"));
    dialog.setWindowTitle(tr("External Protocol Request"));
    dialog.setIcon(QMessageBox::Question);

    switch (dialog.exec()) {
    case QMessageBox::Yes:
        if (dialog.isChecked()) {
            qzSettings->autoOpenProtocols.append(protocol);
            qzSettings->saveSettings();
        }


        QDesktopServices::openUrl(url);
        break;

    case QMessageBox::No:
        if (dialog.isChecked()) {
            qzSettings->blockedProtocols.append(protocol);
            qzSettings->saveSettings();
        }

        break;

    default:
        break;
    }
}

void WebPage::desktopServicesOpen(const QUrl &url)
{
    // Open same url only once in 2 secs
    const int sameUrlTimeout = 2 * 1000;

    if (s_lastUnsupportedUrl != url || s_lastUnsupportedUrlTime.isNull() || s_lastUnsupportedUrlTime.elapsed() > sameUrlTimeout) {
        s_lastUnsupportedUrl = url;
        s_lastUnsupportedUrlTime.restart();
        QDesktopServices::openUrl(url);
    }
    else {
        qWarning() << "WebPage::desktopServicesOpen Url" << url << "has already been opened!\n"
                   "Ignoring it to prevent infinite loop!";
    }
}

void WebPage::windowCloseRequested()
{
    if (!view())
        return;
    view()->closeView();
}

void WebPage::fullScreenRequested(QWebEngineFullScreenRequest fullScreenRequest)
{
    view()->requestFullScreen(fullScreenRequest.toggleOn());

    const bool accepted = fullScreenRequest.toggleOn() == view()->isFullScreen();

    if (accepted)
        fullScreenRequest.accept();
    else
        fullScreenRequest.reject();
}

void WebPage::featurePermissionRequested(const QUrl &origin, const QWebEnginePage::Feature &feature)
{
    if (feature == MouseLock && view()->isFullScreen())
        setFeaturePermission(origin, feature, PermissionGrantedByUser);
    else
        mApp->html5PermissionsManager()->requestPermissions(this, origin, feature);
}

void WebPage::renderProcessTerminated(QWebEnginePage::RenderProcessTerminationStatus terminationStatus, int exitCode)
{
    Q_UNUSED(exitCode)

    if (terminationStatus == NormalTerminationStatus)
        return;

    QTimer::singleShot(0, this, [this]() {
        QString page = QzTools::readAllFileContents(":html/tabcrash.html");
        page.replace(QL1S("%IMAGE%"), QzTools::pixmapToDataUrl(IconProvider::standardIcon(QStyle::SP_MessageBoxWarning).pixmap(45)).toString());
        page.replace(QL1S("%TITLE%"), tr("Failed loading page"));
        page.replace(QL1S("%HEADING%"), tr("Failed loading page"));
        page.replace(QL1S("%LI-1%"), tr("Something went wrong while loading this page."));
        page.replace(QL1S("%LI-2%"), tr("Try reloading the page or closing some tabs to make more memory available."));
        page.replace(QL1S("%RELOAD-PAGE%"), tr("Reload page"));
        page = QzTools::applyDirectionToPage(page);
        setHtml(page.toUtf8(), url());
    });
}

bool WebPage::acceptNavigationRequest(const QUrl &url, QWebEnginePage::NavigationType type, bool isMainFrame)
{
    if (mApp->isClosing()) {
        return QWebEnginePage::acceptNavigationRequest(url, type, isMainFrame);
    }

    if (!mApp->plugins()->acceptNavigationRequest(this, url, type, isMainFrame))
        return false;

    if (url.scheme() == QL1S("falkon")) {
        if (url.path() == QL1S("AddSearchProvider")) {
            QUrlQuery query(url);
            mApp->searchEnginesManager()->addEngine(QUrl(query.queryItemValue(QSL("url"))));
            return false;
#if QTWEBENGINEWIDGETS_VERSION < QT_VERSION_CHECK(5, 12, 0)
        } else if (url.path() == QL1S("PrintPage")) {
            emit printRequested();
            return false;
#endif
        }
    }

    if (url.scheme() == QL1S("ocs") && OcsSupport::instance()->handleUrl(url)) {
        return false;
    }

    const bool result = QWebEnginePage::acceptNavigationRequest(url, type, isMainFrame);

    if (result) {
        if (isMainFrame) {
            const bool isWeb = url.scheme() == QL1S("http") || url.scheme() == QL1S("https") || url.scheme() == QL1S("file");
            const bool globalJsEnabled = mApp->webSettings()->testAttribute(QWebEngineSettings::JavascriptEnabled);
            settings()->setAttribute(QWebEngineSettings::JavascriptEnabled, isWeb ? globalJsEnabled : true);
        }
        emit navigationRequestAccepted(url, type, isMainFrame);
    }

    return result;
}

bool WebPage::certificateError(const QWebEngineCertificateError &error)
{
    return mApp->networkManager()->certificateError(error, view());
}

QStringList WebPage::chooseFiles(QWebEnginePage::FileSelectionMode mode, const QStringList &oldFiles, const QStringList &acceptedMimeTypes)
{
    Q_UNUSED(acceptedMimeTypes);

    QStringList files;
    QString suggestedFileName = s_lastUploadLocation;
    if (!oldFiles.isEmpty())
        suggestedFileName = oldFiles.at(0);

    switch (mode) {
    case FileSelectOpen:
        files = QStringList(QzTools::getOpenFileName("WebPage-ChooseFile", view(), tr("Choose file..."), suggestedFileName));
        break;

    case FileSelectOpenMultiple:
        files = QzTools::getOpenFileNames("WebPage-ChooseFile", view(), tr("Choose files..."), suggestedFileName);
        break;

    default:
        files = QWebEnginePage::chooseFiles(mode, oldFiles, acceptedMimeTypes);
        break;
    }

    if (!files.isEmpty())
        s_lastUploadLocation = files.at(0);

    return files;
}

QStringList WebPage::autoFillUsernames() const
{
    return m_autoFillUsernames;
}

QUrl WebPage::registerProtocolHandlerRequestUrl() const
{
#if QTWEBENGINEWIDGETS_VERSION >= QT_VERSION_CHECK(5, 11, 0)
    if (m_registerProtocolHandlerRequest && url().host() == m_registerProtocolHandlerRequest->origin().host()) {
        return m_registerProtocolHandlerRequest->origin();
    }
#endif
    return QUrl();
}

QString WebPage::registerProtocolHandlerRequestScheme() const
{
#if QTWEBENGINEWIDGETS_VERSION >= QT_VERSION_CHECK(5, 11, 0)
    if (m_registerProtocolHandlerRequest && url().host() == m_registerProtocolHandlerRequest->origin().host()) {
        return m_registerProtocolHandlerRequest->scheme();
    }
#endif
    return QString();
}

bool WebPage::javaScriptPrompt(const QUrl &securityOrigin, const QString &msg, const QString &defaultValue, QString* result)
{
    if (!kEnableJsNonBlockDialogs) {
        return QWebEnginePage::javaScriptPrompt(securityOrigin, msg, defaultValue, result);
    }

    if (m_runningLoop) {
        return false;
    }

    QFrame *widget = new QFrame(view()->overlayWidget());

    widget->setObjectName("jsFrame");
    Ui_jsPrompt* ui = new Ui_jsPrompt();
    ui->setupUi(widget);
    ui->message->setText(msg);
    ui->lineEdit->setText(defaultValue);
    ui->lineEdit->setFocus();
    widget->resize(view()->size());
    widget->show();

    QAbstractButton *clicked = nullptr;
    connect(ui->buttonBox, &QDialogButtonBox::clicked, this, [&](QAbstractButton *button) {
        clicked = button;
    });

    connect(view(), &WebView::viewportResized, widget, QOverload<const QSize &>::of(&QFrame::resize));
    connect(ui->lineEdit, SIGNAL(returnPressed()), ui->buttonBox->button(QDialogButtonBox::Ok), SLOT(animateClick()));

    QEventLoop eLoop;
    m_runningLoop = &eLoop;
    connect(ui->buttonBox, &QDialogButtonBox::clicked, &eLoop, &QEventLoop::quit);

    if (eLoop.exec() == 1) {
        return result;
    }
    m_runningLoop = 0;

    QString x = ui->lineEdit->text();
    bool _result = ui->buttonBox->buttonRole(clicked) == QDialogButtonBox::AcceptRole;
    *result = x;

    delete widget;
    view()->setFocus();

    return _result;
}

bool WebPage::javaScriptConfirm(const QUrl &securityOrigin, const QString &msg)
{
    if (!kEnableJsNonBlockDialogs) {
        return QWebEnginePage::javaScriptConfirm(securityOrigin, msg);
    }

    if (m_runningLoop) {
        return false;
    }

    QFrame *widget = new QFrame(view()->overlayWidget());

    widget->setObjectName("jsFrame");
    Ui_jsConfirm* ui = new Ui_jsConfirm();
    ui->setupUi(widget);
    ui->message->setText(msg);
    ui->buttonBox->button(QDialogButtonBox::Ok)->setFocus();
    widget->resize(view()->size());
    widget->show();

    QAbstractButton *clicked = nullptr;
    connect(ui->buttonBox, &QDialogButtonBox::clicked, this, [&](QAbstractButton *button) {
        clicked = button;
    });

    connect(view(), &WebView::viewportResized, widget, QOverload<const QSize &>::of(&QFrame::resize));

    QEventLoop eLoop;
    m_runningLoop = &eLoop;
    connect(ui->buttonBox, &QDialogButtonBox::clicked, &eLoop, &QEventLoop::quit);

    if (eLoop.exec() == 1) {
        return false;
    }
    m_runningLoop = 0;

    bool result = ui->buttonBox->buttonRole(clicked) == QDialogButtonBox::AcceptRole;

    delete widget;
    view()->setFocus();

    return result;
}

void WebPage::javaScriptAlert(const QUrl &securityOrigin, const QString &msg)
{
    Q_UNUSED(securityOrigin)

    if (m_blockAlerts || m_runningLoop) {
        return;
    }

    if (!kEnableJsNonBlockDialogs) {
        QString title = tr("JavaScript alert");
        if (!url().host().isEmpty()) {
            title.append(QString(" - %1").arg(url().host()));
        }

        CheckBoxDialog dialog(QMessageBox::Ok, view());
        dialog.setDefaultButton(QMessageBox::Ok);
        dialog.setWindowTitle(title);
        dialog.setText(msg);
        dialog.setCheckBoxText(tr("Prevent this page from creating additional dialogs"));
        dialog.setIcon(QMessageBox::Information);
        dialog.exec();

        m_blockAlerts = dialog.isChecked();
        return;
    }

    QFrame *widget = new QFrame(view()->overlayWidget());

    widget->setObjectName("jsFrame");
    Ui_jsAlert* ui = new Ui_jsAlert();
    ui->setupUi(widget);
    ui->message->setText(msg);
    ui->buttonBox->button(QDialogButtonBox::Ok)->setFocus();
    widget->resize(view()->size());
    widget->show();

    connect(view(), &WebView::viewportResized, widget, QOverload<const QSize &>::of(&QFrame::resize));

    QEventLoop eLoop;
    m_runningLoop = &eLoop;
    connect(ui->buttonBox, &QDialogButtonBox::clicked, &eLoop, &QEventLoop::quit);

    if (eLoop.exec() == 1) {
        return;
    }
    m_runningLoop = 0;

    m_blockAlerts = ui->preventAlerts->isChecked();

    delete widget;

    view()->setFocus();
}

void WebPage::javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level, const QString &message, int lineNumber, const QString &sourceID)
{
    if (!kEnableJsOutput) {
        return;
    }

    switch (level) {
    case InfoMessageLevel:
        std::cout << "[I] ";
        break;

    case WarningMessageLevel:
        std::cout << "[W] ";
        break;

    case ErrorMessageLevel:
        std::cout << "[E] ";
        break;
    }

    std::cout << qPrintable(sourceID) << ":" << lineNumber << " " << qPrintable(message);
}

QWebEnginePage* WebPage::createWindow(QWebEnginePage::WebWindowType type)
{
    this->setBackgroundColor(Qt::transparent);
    TabbedWebView *tView = qobject_cast<TabbedWebView*>(view());
    BrowserWindow *window = tView ? tView->browserWindow() : mApp->getWindow();

    auto createTab = [=](Qz::NewTabPositionFlags pos) {
        int index = window->tabWidget()->addView(QUrl(), pos);
        TabbedWebView* view = window->weView(index);
        view->setPage(new WebPage);
        if (tView) {
            tView->webTab()->addChildTab(view->webTab());
        }
        // Workaround focus issue when creating tab
        if (pos.testFlag(Qz::NT_SelectedTab)) {
            QPointer<TabbedWebView> pview = view;
            pview->setFocus();
            QTimer::singleShot(100, this, [pview]() {
                if (pview && pview->webTab()->isCurrentTab()) {
                    pview->setFocus();
                }
            });
        }
        return view->page();
    };

    switch (type) {
    case QWebEnginePage::WebBrowserWindow: {
        BrowserWindow *window = mApp->createWindow(Qz::BW_NewWindow);
        WebPage *page = new WebPage;
        window->setStartPage(page);
        return page;
    }

    case QWebEnginePage::WebDialog:
        if (!qzSettings->openPopupsInTabs) {
            PopupWebView* view = new PopupWebView;
            view->setPage(new WebPage);
            PopupWindow* popup = new PopupWindow(view);
            popup->show();
            window->addDeleteOnCloseWidget(popup);
            return view->page();
        }
        // else fallthrough

    case QWebEnginePage::WebBrowserTab:
        return createTab(Qz::NT_CleanSelectedTab);

    case QWebEnginePage::WebBrowserBackgroundTab:
        return createTab(Qz::NT_CleanNotSelectedTab);

    default:
        break;
    }

    return Q_NULLPTR;
}
